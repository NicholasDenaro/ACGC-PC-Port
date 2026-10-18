#include "server/server_storage.h"
#include "server/server_backup.h"
#include "m_common_data.h"
#include "m_name_table.h"
#include "m_land.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#undef near
#undef far
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static ServerExtendedDB s_db;
static bool s_storage_initialized = false;
static char s_save_path[256] = SERVER_EXTENDED_PATH;
static char s_base_dir[256] = "save";
static bool s_dirty = false;
static uint64_t s_last_mutation_ms = 0;
static uint64_t s_last_periodic_sec = 0;

/* Canonical initial home mortgage debt in Animal Crossing: 19,800 Bells */
#define INITIAL_HOUSE_DEBT 19800

static uint32_t crc32_compute(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

static void ensure_save_directory(const char* dir) {
    if (!dir || dir[0] == '\0') dir = SERVER_DEFAULT_SAVE_DIR;
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

bool server_storage_init(const char* save_dir) {
    if (save_dir != NULL && save_dir[0] != '\0') {
        strncpy(s_base_dir, save_dir, sizeof(s_base_dir) - 1);
        s_base_dir[sizeof(s_base_dir) - 1] = '\0';
        snprintf(s_save_path, sizeof(s_save_path), "%s/server_extended.dat", save_dir);
    } else {
        strncpy(s_base_dir, SERVER_DEFAULT_SAVE_DIR, sizeof(s_base_dir) - 1);
        snprintf(s_save_path, sizeof(s_save_path), "%s", SERVER_EXTENDED_PATH);
    }

    ensure_save_directory(s_base_dir);
    server_backup_init(s_base_dir);
    s_last_periodic_sec = (uint64_t)time(NULL);

    FILE* f = fopen(s_save_path, "rb");
    if (f != NULL) {
        size_t n = fread(&s_db, 1, sizeof(ServerExtendedDB), f);
        fclose(f);

        if (n == sizeof(ServerExtendedDB)) {
            if (s_db.magic == SERVER_EXTENDED_MAGIC && s_db.version == SERVER_EXTENDED_VERSION) {
                uint32_t expected_crc = s_db.checksum;
                size_t payload_len = sizeof(ServerExtendedDB) - offsetof(ServerExtendedDB, houses);
                uint32_t computed_crc = crc32_compute((const uint8_t*)&s_db.houses, payload_len);

                if (expected_crc == computed_crc) {
                    s_storage_initialized = true;

                    int res_count = 0;
                    for (int i = 0; i < MAX_EXTENDED_RESIDENTS; i++) {
                        if (s_db.residents[i].is_active) res_count++;
                    }
                    int vis_count = 0;
                    for (int i = 0; i < MAX_EXTENDED_VISITORS; i++) {
                        if (s_db.visitors[i].is_active) vis_count++;
                    }

                    printf("[SERVER STORAGE] Loaded %s (Residents: %d/%d, Visitors: %d/%d, CRC: 0x%08X)\n",
                           s_save_path, res_count, MAX_EXTENDED_RESIDENTS, vis_count, MAX_EXTENDED_VISITORS,
                           computed_crc);

                    server_storage_sync_to_gci();
                    return true;
                } else {
                    fprintf(stderr, "[SERVER STORAGE WARNING] CRC mismatch in %s (expected 0x%08X, got 0x%08X). Re-initializing.\n",
                            s_save_path, expected_crc, computed_crc);
                }
            } else {
                fprintf(stderr, "[SERVER STORAGE WARNING] Invalid magic/version in %s. Re-initializing.\n", s_save_path);
            }
        }
    }

    /* Missing or invalid database: initialize default blank structure */
    memset(&s_db, 0, sizeof(ServerExtendedDB));
    s_db.magic = SERVER_EXTENDED_MAGIC;
    s_db.version = SERVER_EXTENDED_VERSION;
    s_db.last_save_time = (uint64_t)time(NULL);

    for (int h = 0; h < MAX_EXTENDED_HOUSES; h++) {
        s_db.houses[h].shared_debt = 0; /* Vacant until first resident moves in */
        s_db.houses[h].total_paid = 0;
    }

    for (int r = 0; r < MAX_EXTENDED_RESIDENTS; r++) {
        s_db.residents[r].house_idx = (uint8_t)(r / 2);
        s_db.residents[r].roommate_slot = (uint8_t)(r % 2);
        s_db.residents[r].is_active = 0;
    }

    for (int v = 0; v < MAX_EXTENDED_VISITORS; v++) {
        s_db.visitors[v].is_active = 0;
    }

    s_storage_initialized = true;
    printf("[SERVER STORAGE] Initialized new server extended database: %s\n", s_save_path);
    server_storage_save();
    return true;
}

bool server_storage_save(void) {
    if (!s_storage_initialized) {
        return false;
    }

    ensure_save_directory(s_base_dir);

    char tmp_path[300];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", s_save_path);

    s_db.last_save_time = (uint64_t)time(NULL);
    size_t payload_len = sizeof(ServerExtendedDB) - offsetof(ServerExtendedDB, houses);
    s_db.checksum = crc32_compute((const uint8_t*)&s_db.houses, payload_len);

    FILE* f = fopen(tmp_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "[SERVER STORAGE ERROR] Failed to open %s for write!\n", tmp_path);
        return false;
    }

    size_t written = fwrite(&s_db, 1, sizeof(ServerExtendedDB), f);
    fflush(f);
    fclose(f);

    if (written != sizeof(ServerExtendedDB)) {
        fprintf(stderr, "[SERVER STORAGE ERROR] Incomplete write to %s!\n", tmp_path);
        remove(tmp_path);
        return false;
    }

#ifdef _WIN32
    if (!MoveFileExA(tmp_path, s_save_path, MOVEFILE_REPLACE_EXISTING)) {
        remove(s_save_path);
        if (rename(tmp_path, s_save_path) != 0) {
            fprintf(stderr, "[SERVER STORAGE ERROR] Failed to replace %s!\n", s_save_path);
            return false;
        }
    }
#else
    if (rename(tmp_path, s_save_path) != 0) {
        fprintf(stderr, "[SERVER STORAGE ERROR] Failed to replace %s!\n", s_save_path);
        return false;
    }
#endif

    /* Keep canonical GCI save synchronized */
    server_storage_sync_to_gci();
    return true;
}

void server_storage_mark_dirty(void) {
    s_dirty = true;
#ifdef _WIN32
    s_last_mutation_ms = GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_last_mutation_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

bool server_storage_save_all(void) {
    if (!s_storage_initialized) {
        return false;
    }
    server_storage_sync_to_gci();
    bool ext_ok = server_storage_save();
    extern int pc_save_force_write(void);
    int gci_ok = pc_save_force_write();
    s_dirty = false;
    return ext_ok && (gci_ok != 0);
}

void server_storage_tick(void) {
    if (!s_storage_initialized) {
        return;
    }

#ifdef _WIN32
    uint64_t now_ms = GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
#endif
    uint64_t now_sec = (uint64_t)time(NULL);

    /* 1. Debounced autosave (1 second after last mutation) */
    if (s_dirty && (now_ms >= s_last_mutation_ms + 1000)) {
        server_storage_save_all();
    }

    /* 2. Periodic backup ticker (every 60 seconds) */
    if (s_last_periodic_sec == 0) {
        s_last_periodic_sec = now_sec;
    } else if (now_sec >= s_last_periodic_sec + 60) {
        if (s_dirty) {
            server_storage_save_all();
        }
        server_backup_tick(now_sec);
        s_last_periodic_sec = now_sec;
    }
}

int server_storage_find_resident_by_token(const uint8_t token[AUTH_TOKEN_SIZE]) {
    if (!s_storage_initialized || token == NULL) {
        return -1;
    }
    for (int i = 0; i < MAX_EXTENDED_RESIDENTS; i++) {
        if (s_db.residents[i].is_active && memcmp(s_db.residents[i].auth_token, token, AUTH_TOKEN_SIZE) == 0) {
            return i;
        }
    }
    return -1;
}

int server_storage_find_visitor_by_token(const uint8_t token[AUTH_TOKEN_SIZE]) {
    if (!s_storage_initialized || token == NULL) {
        return -1;
    }
    for (int i = 0; i < MAX_EXTENDED_VISITORS; i++) {
        if (s_db.visitors[i].is_active && memcmp(s_db.visitors[i].auth_token, token, AUTH_TOKEN_SIZE) == 0) {
            return i;
        }
    }
    return -1;
}

int server_storage_find_player(const uint8_t token[AUTH_TOKEN_SIZE], bool* out_is_resident) {
    int res = server_storage_find_resident_by_token(token);
    if (res >= 0) {
        if (out_is_resident) *out_is_resident = true;
        return res;
    }

    int vis = server_storage_find_visitor_by_token(token);
    if (vis >= 0) {
        if (out_is_resident) *out_is_resident = false;
        return vis;
    }

    return -1;
}

int server_storage_register_visitor(const uint8_t token[AUTH_TOKEN_SIZE], const Private_c* priv) {
    if (!s_storage_initialized || token == NULL) {
        return -1;
    }

    /* Check if already registered */
    int existing = server_storage_find_visitor_by_token(token);
    if (existing >= 0) {
        if (priv != NULL) {
            s_db.visitors[existing].private_data = *priv;
        }
        s_db.visitors[existing].last_seen_time = (uint64_t)time(NULL);
        return existing;
    }

    /* Find open visitor slot */
    for (int i = 0; i < MAX_EXTENDED_VISITORS; i++) {
        if (!s_db.visitors[i].is_active) {
            s_db.visitors[i].is_active = 1;
            memcpy(s_db.visitors[i].auth_token, token, AUTH_TOKEN_SIZE);
            s_db.visitors[i].first_visit_time = (uint64_t)time(NULL);
            s_db.visitors[i].last_seen_time = s_db.visitors[i].first_visit_time;

            if (priv != NULL) {
                s_db.visitors[i].private_data = *priv;
            } else {
                mPr_InitPrivateInfo(&s_db.visitors[i].private_data);
            }
            s_db.visitors[i].private_data.exists = TRUE;
            return i;
        }
    }

    fprintf(stderr, "[SERVER STORAGE WARNING] Visitor registry is full (%d)!\n", MAX_EXTENDED_VISITORS);
    return -1;
}

int server_storage_get_house_vacancies(uint8_t house_idx) {
    if (!s_storage_initialized || house_idx >= MAX_EXTENDED_HOUSES) {
        return 0;
    }
    int slot0 = house_idx * 2;
    int slot1 = house_idx * 2 + 1;
    int occupied = (s_db.residents[slot0].is_active ? 1 : 0) +
                   (s_db.residents[slot1].is_active ? 1 : 0);
    return 2 - occupied;
}

bool server_storage_promote_to_resident(const uint8_t token[AUTH_TOKEN_SIZE], uint8_t house_idx,
                                       const Private_c* priv, int* out_resident_idx) {
    if (!s_storage_initialized || token == NULL || house_idx >= MAX_EXTENDED_HOUSES) {
        return false;
    }

    int slot0 = house_idx * 2;
    int slot1 = house_idx * 2 + 1;
    int target_slot = -1;

    if (!s_db.residents[slot0].is_active) {
        target_slot = slot0;
    } else if (!s_db.residents[slot1].is_active) {
        target_slot = slot1;
    } else {
        printf("[SERVER STORAGE] House %d is completely full (2/2 roommates).\n", house_idx);
        return false;
    }

    ServerResident* r = &s_db.residents[target_slot];
    r->is_active = 1;
    r->house_idx = house_idx;
    r->roommate_slot = (uint8_t)(target_slot % 2);
    memcpy(r->auth_token, token, AUTH_TOKEN_SIZE);
    r->registered_time = (uint64_t)time(NULL);
    r->last_seen_time = r->registered_time;

    if (priv != NULL) {
        r->private_data = *priv;
    } else {
        /* Check if they were an existing visitor */
        int vis = server_storage_find_visitor_by_token(token);
        if (vis >= 0) {
            r->private_data = s_db.visitors[vis].private_data;
            s_db.visitors[vis].is_active = 0; /* Clear from visitors */
        } else {
            mPr_InitPrivateInfo(&r->private_data);
        }
    }

    r->private_data.exists = TRUE;

    /* Initialize shared debt if this is the first resident in the house */
    if (s_db.houses[house_idx].shared_debt == 0 && s_db.houses[house_idx].total_paid == 0) {
        s_db.houses[house_idx].shared_debt = INITIAL_HOUSE_DEBT;
    }
    r->private_data.inventory.loan = s_db.houses[house_idx].shared_debt;

    if (out_resident_idx != NULL) {
        *out_resident_idx = target_slot;
    }

    printf("[SERVER STORAGE] Promoted player to Resident Slot %d in House %d (Roommate %d)!\n",
           target_slot, house_idx, r->roommate_slot);

    server_storage_save();
    return true;
}

bool server_storage_evict_resident(int resident_idx) {
    if (!s_storage_initialized || resident_idx < 0 || resident_idx >= MAX_EXTENDED_RESIDENTS) {
        return false;
    }

    uint8_t house_idx = s_db.residents[resident_idx].house_idx;
    memset(&s_db.residents[resident_idx], 0, sizeof(ServerResident));
    s_db.residents[resident_idx].house_idx = (uint8_t)(resident_idx / 2);
    s_db.residents[resident_idx].roommate_slot = (uint8_t)(resident_idx % 2);
    s_db.residents[resident_idx].is_active = 0;

    /* If house has no remaining roommates, reset shared debt to 0 */
    if (server_storage_get_house_vacancies(house_idx) == 2) {
        s_db.houses[house_idx].shared_debt = 0;
        s_db.houses[house_idx].total_paid = 0;
    }

    printf("[SERVER STORAGE] Evicted Resident Slot %d from House %d.\n", resident_idx, house_idx);
    server_storage_save();
    return true;
}

ServerResident* server_storage_get_resident(int resident_idx) {
    if (!s_storage_initialized || resident_idx < 0 || resident_idx >= MAX_EXTENDED_RESIDENTS) {
        return NULL;
    }
    return &s_db.residents[resident_idx];
}

ServerVisitor* server_storage_get_visitor(int visitor_idx) {
    if (!s_storage_initialized || visitor_idx < 0 || visitor_idx >= MAX_EXTENDED_VISITORS) {
        return NULL;
    }
    return &s_db.visitors[visitor_idx];
}

int server_storage_get_roommate(int resident_idx) {
    if (!s_storage_initialized || resident_idx < 0 || resident_idx >= MAX_EXTENDED_RESIDENTS) {
        return -1;
    }
    int peer_idx = resident_idx ^ 1;
    if (s_db.residents[peer_idx].is_active) {
        return peer_idx;
    }
    return -1;
}

uint32_t server_storage_get_house_debt(uint8_t house_idx) {
    if (!s_storage_initialized || house_idx >= MAX_EXTENDED_HOUSES) {
        return 0;
    }
    return s_db.houses[house_idx].shared_debt;
}

uint32_t server_storage_pay_house_debt(uint8_t house_idx, uint32_t bells) {
    if (!s_storage_initialized || house_idx >= MAX_EXTENDED_HOUSES) {
        return 0;
    }

    if (bells >= s_db.houses[house_idx].shared_debt) {
        s_db.houses[house_idx].total_paid += s_db.houses[house_idx].shared_debt;
        s_db.houses[house_idx].shared_debt = 0;
    } else {
        s_db.houses[house_idx].shared_debt -= bells;
        s_db.houses[house_idx].total_paid += bells;
    }

    /* Synchronize remaining loan across both roommates */
    int slot0 = house_idx * 2;
    int slot1 = house_idx * 2 + 1;
    if (s_db.residents[slot0].is_active) {
        s_db.residents[slot0].private_data.inventory.loan = s_db.houses[house_idx].shared_debt;
    }
    if (s_db.residents[slot1].is_active) {
        s_db.residents[slot1].private_data.inventory.loan = s_db.houses[house_idx].shared_debt;
    }

    printf("[SERVER STORAGE] House %d mortgage payment of %u Bells received. Remaining debt: %u Bells.\n",
           house_idx, bells, s_db.houses[house_idx].shared_debt);

    server_storage_save();
    return s_db.houses[house_idx].shared_debt;
}

void server_storage_set_house_debt(uint8_t house_idx, uint32_t debt) {
    if (!s_storage_initialized || house_idx >= MAX_EXTENDED_HOUSES) {
        return;
    }
    s_db.houses[house_idx].shared_debt = debt;
    int slot0 = house_idx * 2;
    int slot1 = house_idx * 2 + 1;
    if (s_db.residents[slot0].is_active) {
        s_db.residents[slot0].private_data.inventory.loan = debt;
    }
    if (s_db.residents[slot1].is_active) {
        s_db.residents[slot1].private_data.inventory.loan = debt;
    }
    server_storage_save();
}

void server_storage_sync_to_gci(void) {
    if (!s_storage_initialized) {
        return;
    }

    for (int h = 0; h < MAX_EXTENDED_HOUSES; h++) {
        int r0 = h * 2;
        int r1 = h * 2 + 1;

        if (s_db.residents[r0].is_active) {
            /* Mirror primary resident (slot 0) to vanilla Save.private_data[h] */
            Private_c* gci_priv = Save_GetPointer(private_data[h]);
            memcpy(gci_priv, &s_db.residents[r0].private_data, sizeof(Private_c));
            mPr_CopyPersonalID(&Save_Get(homes[h]).ownerID, &gci_priv->player_ID);
            Save_Get(homes[h]).flags.has_saved = TRUE;
            gci_priv->inventory.loan = s_db.houses[h].shared_debt;
        } else if (s_db.residents[r1].is_active) {
            /* If slot 0 vacant but roommate slot 1 active, mirror slot 1 */
            Private_c* gci_priv = Save_GetPointer(private_data[h]);
            memcpy(gci_priv, &s_db.residents[r1].private_data, sizeof(Private_c));
            mPr_CopyPersonalID(&Save_Get(homes[h]).ownerID, &gci_priv->player_ID);
            Save_Get(homes[h]).flags.has_saved = TRUE;
            gci_priv->inventory.loan = s_db.houses[h].shared_debt;
        } else {
            /* Both roommate slots vacant: mark house unowned */
            mPr_ClearPrivateInfo(Save_GetPointer(private_data[h]));
            mPr_ClearPersonalID(&Save_Get(homes[h]).ownerID);
            Save_Get(homes[h]).flags.has_saved = FALSE;
            Save_Get(homes[h]).size_info.size = mHm_HOMESIZE_SMALL;
        }
    }
}

bool server_storage_run_tests(void) {
    printf("[SERVER STORAGE TEST] Starting server extended storage unit tests...\n");

    const char* test_dir = "save/test_storage";
#ifdef _WIN32
    _mkdir("save");
    _mkdir(test_dir);
#else
    mkdir("save", 0755);
    mkdir(test_dir, 0755);
#endif

    /* 1. Init new database */
    if (!server_storage_init(test_dir)) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: server_storage_init failed!\n");
        return false;
    }

    for (int h = 0; h < MAX_EXTENDED_HOUSES; h++) {
        if (server_storage_get_house_vacancies(h) != 2) {
            fprintf(stderr, "[SERVER STORAGE TEST] FAIL: House %d should start with 2 vacancies!\n", h);
            return false;
        }
    }

    /* 2. Visitor registration & lookup */
    uint8_t vis_tok[AUTH_TOKEN_SIZE];
    memset(vis_tok, 0x11, sizeof(vis_tok));
    Private_c vis_priv;
    mPr_InitPrivateInfo(&vis_priv);
    mPr_CopyPlayerName(vis_priv.player_ID.player_name, (u8*)"VisBob  ");

    int vis_idx = server_storage_register_visitor(vis_tok, &vis_priv);
    if (vis_idx < 0) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: server_storage_register_visitor failed!\n");
        return false;
    }

    bool is_res = false;
    int found_idx = server_storage_find_player(vis_tok, &is_res);
    if (found_idx != vis_idx || is_res != false) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: server_storage_find_player visitor lookup failed!\n");
        return false;
    }

    /* 3. Promote visitor to resident in House 0 (Roommate 0) */
    int res0_idx = -1;
    if (!server_storage_promote_to_resident(vis_tok, 0, NULL, &res0_idx) || res0_idx != 0) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Promote visitor to House 0 slot 0 failed (res0_idx=%d)!\n", res0_idx);
        return false;
    }

    if (server_storage_get_house_vacancies(0) != 1) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: House 0 should now have 1 vacancy!\n");
        return false;
    }

    /* Verify promoted player is now recognized as resident */
    found_idx = server_storage_find_player(vis_tok, &is_res);
    if (found_idx != 0 || is_res != true) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Promoted player should be resident at index 0!\n");
        return false;
    }

    /* 4. Promote 2nd player into House 0 (Roommate 1) */
    uint8_t roomie_tok[AUTH_TOKEN_SIZE];
    memset(roomie_tok, 0x22, sizeof(roomie_tok));
    Private_c roomie_priv;
    mPr_InitPrivateInfo(&roomie_priv);
    mPr_CopyPlayerName(roomie_priv.player_ID.player_name, (u8*)"Alice   ");

    int res1_idx = -1;
    if (!server_storage_promote_to_resident(roomie_tok, 0, &roomie_priv, &res1_idx) || res1_idx != 1) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Promote roommate into House 0 slot 1 failed!\n");
        return false;
    }

    if (server_storage_get_house_vacancies(0) != 0) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: House 0 should have 0 vacancies!\n");
        return false;
    }

    /* 5. Verify 3rd player cannot join House 0 (house full) */
    uint8_t third_tok[AUTH_TOKEN_SIZE];
    memset(third_tok, 0x33, sizeof(third_tok));
    int third_idx = -1;
    if (server_storage_promote_to_resident(third_tok, 0, NULL, &third_idx)) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: House 0 should reject 3rd resident!\n");
        return false;
    }

    /* 6. Verify Roommate peer linkage */
    if (server_storage_get_roommate(0) != 1 || server_storage_get_roommate(1) != 0) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Roommate peer linkage failed!\n");
        return false;
    }

    /* 7. Shared Mortgage Debt Payment */
    uint32_t init_debt = server_storage_get_house_debt(0);
    if (init_debt != INITIAL_HOUSE_DEBT) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Initial house debt should be %u (got %u)!\n",
                INITIAL_HOUSE_DEBT, init_debt);
        return false;
    }

    uint32_t remain = server_storage_pay_house_debt(0, 5000);
    if (remain != INITIAL_HOUSE_DEBT - 5000) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Debt after 5000 payment should be %u (got %u)!\n",
                INITIAL_HOUSE_DEBT - 5000, remain);
        return false;
    }

    ServerResident* r0 = server_storage_get_resident(0);
    ServerResident* r1 = server_storage_get_resident(1);
    if (r0->private_data.inventory.loan != remain || r1->private_data.inventory.loan != remain) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Both roommates should reflect identical shared debt!\n");
        return false;
    }

    /* 8. Persistence round-trip & CRC verification */
    if (!server_storage_save()) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: server_storage_save failed!\n");
        return false;
    }

    /* Re-init from test dir */
    if (!server_storage_init(test_dir)) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: server_storage_init reload failed!\n");
        return false;
    }

    if (server_storage_find_resident_by_token(vis_tok) != 0 ||
        server_storage_find_resident_by_token(roomie_tok) != 1) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Reloaded residents do not match!\n");
        return false;
    }

    /* 9. Eviction & Vacancy cleanup */
    if (!server_storage_evict_resident(0) || !server_storage_evict_resident(1)) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: Eviction failed!\n");
        return false;
    }

    if (server_storage_get_house_vacancies(0) != 2) {
        fprintf(stderr, "[SERVER STORAGE TEST] FAIL: House 0 should have 2 vacancies after eviction!\n");
        return false;
    }

    /* Clean up test directory */
    char test_file[300];
    char test_tmp[300];
    snprintf(test_file, sizeof(test_file), "%s/server_extended.dat", test_dir);
    snprintf(test_tmp, sizeof(test_tmp), "%s/server_extended.dat.tmp", test_dir);
    remove(test_file);
    remove(test_tmp);

    /* Re-initialize default storage */
    server_storage_init(NULL);

    /* 10. Run tiered rolling backup unit tests */
    if (!server_backup_run_tests()) {
        return false;
    }

    printf("[SERVER STORAGE TEST] All server extended storage unit tests passed successfully!\n");
    return true;
}
