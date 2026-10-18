/* server_spatial.c - 3x3 Acre Subscription & Spatial Interest Grid */
#include "server/server_spatial.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static SpatialClientInfo s_spatial_clients[SPATIAL_MAX_CLIENTS];
static bool s_spatial_initialized = false;

void server_spatial_init(void) {
    memset(s_spatial_clients, 0, sizeof(s_spatial_clients));
    s_spatial_initialized = true;
    printf("[SERVER SPATIAL] Spatial interest grid initialized (%dx%d full, %dx%d FG, %d max clients).\n",
           SPATIAL_BLOCK_X_NUM, SPATIAL_BLOCK_Z_NUM, SPATIAL_FG_X_NUM, SPATIAL_FG_Z_NUM, SPATIAL_MAX_CLIENTS);
}

void server_spatial_reset(void) {
    memset(s_spatial_clients, 0, sizeof(s_spatial_clients));
}

bool server_spatial_wpos_to_block(float world_x, float world_z, int* out_bx, int* out_bz) {
    if (!out_bx || !out_bz) return false;

    int bx = (int)floorf(world_x / SPATIAL_ACRE_SIZE_F);
    int bz = (int)floorf(world_z / SPATIAL_ACRE_SIZE_F);

    if (bx < 0) bx = 0;
    if (bx >= SPATIAL_BLOCK_X_NUM) bx = SPATIAL_BLOCK_X_NUM - 1;
    if (bz < 0) bz = 0;
    if (bz >= SPATIAL_BLOCK_Z_NUM) bz = SPATIAL_BLOCK_Z_NUM - 1;

    *out_bx = bx;
    *out_bz = bz;
    return true;
}

bool server_spatial_block_to_fg(int bx, int bz, int* out_fg_x, int* out_fg_z) {
    int fx = bx - 1;
    int fz = bz - 1;

    if (fx >= 0 && fx < SPATIAL_FG_X_NUM && fz >= 0 && fz < SPATIAL_FG_Z_NUM) {
        if (out_fg_x) *out_fg_x = fx;
        if (out_fg_z) *out_fg_z = fz;
        return true;
    }

    if (out_fg_x) *out_fg_x = -1;
    if (out_fg_z) *out_fg_z = -1;
    return false;
}

bool server_spatial_fg_to_block(int fg_x, int fg_z, int* out_bx, int* out_bz) {
    if (fg_x < 0 || fg_x >= SPATIAL_FG_X_NUM || fg_z < 0 || fg_z >= SPATIAL_FG_Z_NUM) {
        return false;
    }
    if (out_bx) *out_bx = fg_x + 1;
    if (out_bz) *out_bz = fg_z + 1;
    return true;
}

bool server_spatial_fg_to_name(int fg_x, int fg_z, char* out_buf, size_t buf_size) {
    if (!out_buf || buf_size < 4) return false;
    if (fg_x < 0 || fg_x >= SPATIAL_FG_X_NUM || fg_z < 0 || fg_z >= SPATIAL_FG_Z_NUM) {
        snprintf(out_buf, buf_size, "???");
        return false;
    }
    snprintf(out_buf, buf_size, "%c-%d", 'A' + fg_z, fg_x + 1);
    return true;
}

bool server_spatial_block_to_name(int bx, int bz, char* out_buf, size_t buf_size) {
    if (!out_buf || buf_size < 4) return false;

    int fx, fz;
    if (server_spatial_block_to_fg(bx, bz, &fx, &fz)) {
        return server_spatial_fg_to_name(fx, fz, out_buf, buf_size);
    }

    if (bz == 0) {
        snprintf(out_buf, buf_size, "Railroad");
    } else if (bz >= 7) {
        snprintf(out_buf, buf_size, "Ocean");
    } else if (bx == 0) {
        snprintf(out_buf, buf_size, "West Cliff");
    } else if (bx >= 6) {
        snprintf(out_buf, buf_size, "East Cliff");
    } else {
        snprintf(out_buf, buf_size, "Border");
    }
    return true;
}

static SpatialClientInfo* find_client_slot(uint32_t client_id) {
    for (int i = 0; i < SPATIAL_MAX_CLIENTS; i++) {
        if (s_spatial_clients[i].is_registered && s_spatial_clients[i].client_id == client_id) {
            return &s_spatial_clients[i];
        }
    }
    return NULL;
}

static SpatialClientInfo* allocate_client_slot(uint32_t client_id) {
    SpatialClientInfo* existing = find_client_slot(client_id);
    if (existing) return existing;

    for (int i = 0; i < SPATIAL_MAX_CLIENTS; i++) {
        if (!s_spatial_clients[i].is_registered) {
            s_spatial_clients[i].client_id = client_id;
            s_spatial_clients[i].is_registered = true;
            return &s_spatial_clients[i];
        }
    }
    return NULL;
}

void server_spatial_update_client_acre(uint32_t client_id, uint32_t zone_id, int bx, int bz) {
    if (!s_spatial_initialized) {
        server_spatial_init();
    }

    SpatialClientInfo* c = allocate_client_slot(client_id);
    if (!c) {
        fprintf(stderr, "[SERVER SPATIAL WARNING] Client capacity reached for Client ID %u\n", client_id);
        return;
    }

    c->zone_id = zone_id;
    c->is_outdoor = (zone_id == 0);

    if (bx < 0) bx = 0;
    if (bx >= SPATIAL_BLOCK_X_NUM) bx = SPATIAL_BLOCK_X_NUM - 1;
    if (bz < 0) bz = 0;
    if (bz >= SPATIAL_BLOCK_Z_NUM) bz = SPATIAL_BLOCK_Z_NUM - 1;

    c->block_x = bx;
    c->block_z = bz;
    server_spatial_block_to_fg(bx, bz, &c->fg_x, &c->fg_z);

    if (c->is_outdoor) {
        /* Compute 3x3 acre subscription window clamped to block bounds */
        c->min_bx = (bx > 0) ? bx - 1 : 0;
        c->max_bx = (bx < SPATIAL_BLOCK_X_NUM - 1) ? bx + 1 : SPATIAL_BLOCK_X_NUM - 1;
        c->min_bz = (bz > 0) ? bz - 1 : 0;
        c->max_bz = (bz < SPATIAL_BLOCK_Z_NUM - 1) ? bz + 1 : SPATIAL_BLOCK_Z_NUM - 1;

        /* Build bitmask of subscribed foreground acres */
        c->fg_acre_mask = 0;
        for (int z = c->min_bz; z <= c->max_bz; z++) {
            for (int x = c->min_bx; x <= c->max_bx; x++) {
                int fx, fz;
                if (server_spatial_block_to_fg(x, z, &fx, &fz)) {
                    c->fg_acre_mask |= (1U << (fz * SPATIAL_FG_X_NUM + fx));
                }
            }
        }
    } else {
        /* Indoor rooms: no outdoor acres subscribed */
        c->min_bx = c->max_bx = c->min_bz = c->max_bz = -1;
        c->fg_acre_mask = 0;
    }
}

void server_spatial_update_client_wpos(uint32_t client_id, uint32_t zone_id, float world_x, float world_z) {
    int bx, bz;
    server_spatial_wpos_to_block(world_x, world_z, &bx, &bz);
    server_spatial_update_client_acre(client_id, zone_id, bx, bz);
}

void server_spatial_remove_client(uint32_t client_id) {
    SpatialClientInfo* c = find_client_slot(client_id);
    if (c) {
        memset(c, 0, sizeof(SpatialClientInfo));
    }
}

const SpatialClientInfo* server_spatial_get_client(uint32_t client_id) {
    return find_client_slot(client_id);
}

bool server_spatial_is_client_interested(uint32_t client_id, uint32_t zone_id, int bx, int bz) {
    const SpatialClientInfo* c = find_client_slot(client_id);
    if (!c || !c->is_registered) {
        return false;
    }

    /* Zone isolation: clients only receive events from their current zone */
    if (c->zone_id != zone_id) {
        return false;
    }

    /* Indoors: all occupants of the room receive the update */
    if (zone_id > 0) {
        return true;
    }

    /* Outdoors: 3x3 acre subscription check */
    return (bx >= c->min_bx && bx <= c->max_bx &&
            bz >= c->min_bz && bz <= c->max_bz);
}

bool server_spatial_is_client_interested_wpos(uint32_t client_id, uint32_t zone_id, float world_x, float world_z) {
    int bx, bz;
    server_spatial_wpos_to_block(world_x, world_z, &bx, &bz);
    return server_spatial_is_client_interested(client_id, zone_id, bx, bz);
}

uint32_t server_spatial_get_subscribed_clients(uint32_t zone_id, int bx, int bz, uint32_t* out_client_ids, uint32_t max_clients) {
    if (!out_client_ids || max_clients == 0) {
        return 0;
    }

    uint32_t count = 0;
    for (int i = 0; i < SPATIAL_MAX_CLIENTS && count < max_clients; i++) {
        if (s_spatial_clients[i].is_registered) {
            if (server_spatial_is_client_interested(s_spatial_clients[i].client_id, zone_id, bx, bz)) {
                out_client_ids[count++] = s_spatial_clients[i].client_id;
            }
        }
    }
    return count;
}

uint32_t server_spatial_get_client_mask(uint32_t zone_id, int bx, int bz) {
    uint32_t mask = 0;
    for (int i = 0; i < SPATIAL_MAX_CLIENTS; i++) {
        if (s_spatial_clients[i].is_registered) {
            if (server_spatial_is_client_interested(s_spatial_clients[i].client_id, zone_id, bx, bz)) {
                mask |= (1U << i);
            }
        }
    }
    return mask;
}

bool server_spatial_clients_overlap(uint32_t client_a, uint32_t client_b) {
    const SpatialClientInfo* ca = find_client_slot(client_a);
    const SpatialClientInfo* cb = find_client_slot(client_b);

    if (!ca || !cb || !ca->is_registered || !cb->is_registered) {
        return false;
    }

    if (ca->zone_id != cb->zone_id) {
        return false;
    }

    /* Indoors in same zone */
    if (ca->zone_id > 0) {
        return true;
    }

    /* Outdoors: check bounding box intersection */
    return !(ca->max_bx < cb->min_bx || ca->min_bx > cb->max_bx ||
             ca->max_bz < cb->min_bz || ca->min_bz > cb->max_bz);
}

/* --- Unit Tests --- */

#define TEST_SPATIAL_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[SERVER SPATIAL TEST FAILED] %s (line %d)\n", msg, __LINE__); \
            return 0; \
        } \
    } while (0)

int server_spatial_run_tests(void) {
    printf("[SERVER SPATIAL TEST] Running 3x3 Acre Subscription & Spatial Interest Grid tests...\n");
    server_spatial_init();
    server_spatial_reset();

    /* 1. Coordinate conversions */
    {
        int bx, bz, fx, fz;
        /* World (0, 0) is block (0, 0) [border] */
        server_spatial_wpos_to_block(0.0f, 0.0f, &bx, &bz);
        TEST_SPATIAL_ASSERT(bx == 0 && bz == 0, "wpos 0,0 -> block 0,0");
        TEST_SPATIAL_ASSERT(!server_spatial_block_to_fg(bx, bz, &fx, &fz), "block 0,0 is not FG");

        /* World (640, 640) is Acre A-1 (block 1, 1; FG 0, 0) */
        server_spatial_wpos_to_block(640.0f, 640.0f, &bx, &bz);
        TEST_SPATIAL_ASSERT(bx == 1 && bz == 1, "wpos 640,640 -> block 1,1");
        TEST_SPATIAL_ASSERT(server_spatial_block_to_fg(bx, bz, &fx, &fz), "block 1,1 is FG");
        TEST_SPATIAL_ASSERT(fx == 0 && fz == 0, "block 1,1 -> fg 0,0");

        char name[16];
        TEST_SPATIAL_ASSERT(server_spatial_fg_to_name(fx, fz, name, sizeof(name)), "fg 0,0 name");
        TEST_SPATIAL_ASSERT(strcmp(name, "A-1") == 0, "fg 0,0 is A-1");

        /* World for Acre F-5: col 5 (bx=5, x=3200), row F (bz=6, z=3840) */
        server_spatial_wpos_to_block(3200.0f + 100.0f, 3840.0f + 100.0f, &bx, &bz);
        TEST_SPATIAL_ASSERT(bx == 5 && bz == 6, "wpos -> Acre F-5 block 5,6");
        TEST_SPATIAL_ASSERT(server_spatial_block_to_fg(bx, bz, &fx, &fz), "block 5,6 is FG");
        TEST_SPATIAL_ASSERT(fx == 4 && fz == 5, "block 5,6 -> fg 4,5");
        TEST_SPATIAL_ASSERT(server_spatial_fg_to_name(fx, fz, name, sizeof(name)), "fg 4,5 name");
        TEST_SPATIAL_ASSERT(strcmp(name, "F-5") == 0, "fg 4,5 is F-5");
    }

    /* 2. 3x3 Acre Subscription Window & Acceptance Criteria:
          "Actions occurring in Acre A-1 are not broadcast to players in Acre F-5, saving bandwidth." */
    {
        server_spatial_reset();

        /* Player 1 at Acre A-1 (block 1, 1; outdoor zone 0) */
        server_spatial_update_client_acre(1, 0, 1, 1);
        const SpatialClientInfo* p1 = server_spatial_get_client(1);
        TEST_SPATIAL_ASSERT(p1 != NULL, "p1 registered");
        TEST_SPATIAL_ASSERT(p1->min_bx == 0 && p1->max_bx == 2, "p1 X window [0..2]");
        TEST_SPATIAL_ASSERT(p1->min_bz == 0 && p1->max_bz == 2, "p1 Z window [0..2]");

        /* Player 2 at Acre F-5 (block 5, 6; outdoor zone 0) */
        server_spatial_update_client_acre(2, 0, 5, 6);
        const SpatialClientInfo* p2 = server_spatial_get_client(2);
        TEST_SPATIAL_ASSERT(p2 != NULL, "p2 registered");
        TEST_SPATIAL_ASSERT(p2->min_bx == 4 && p2->max_bx == 6, "p2 X window [4..6]");
        TEST_SPATIAL_ASSERT(p2->min_bz == 5 && p2->max_bz == 7, "p2 Z window [5..7]");

        /* ACCEPTANCE CRITERIA VERIFICATION:
           - Action in Acre A-1 (block 1, 1):
             p1 MUST be interested.
             p2 MUST NOT be interested. */
        TEST_SPATIAL_ASSERT(server_spatial_is_client_interested(1, 0, 1, 1), "p1 interested in Acre A-1");
        TEST_SPATIAL_ASSERT(!server_spatial_is_client_interested(2, 0, 1, 1), "p2 NOT interested in Acre A-1 (Acceptance Criteria)");

        /* - Action in Acre F-5 (block 5, 6):
             p2 MUST be interested.
             p1 MUST NOT be interested. */
        TEST_SPATIAL_ASSERT(server_spatial_is_client_interested(2, 0, 5, 6), "p2 interested in Acre F-5");
        TEST_SPATIAL_ASSERT(!server_spatial_is_client_interested(1, 0, 5, 6), "p1 NOT interested in Acre F-5 (Acceptance Criteria)");

        /* Overlap check between P1 (A-1) and P2 (F-5) should be false */
        TEST_SPATIAL_ASSERT(!server_spatial_clients_overlap(1, 2), "p1 and p2 do not overlap");

        /* Player 3 at Acre B-2 (block 2, 2; adjacent to A-1) */
        server_spatial_update_client_acre(3, 0, 2, 2);
        TEST_SPATIAL_ASSERT(server_spatial_is_client_interested(1, 0, 2, 2), "p1 interested in B-2");
        TEST_SPATIAL_ASSERT(server_spatial_is_client_interested(3, 0, 1, 1), "p3 interested in A-1");
        TEST_SPATIAL_ASSERT(server_spatial_clients_overlap(1, 3), "p1 and p3 overlap");
        TEST_SPATIAL_ASSERT(!server_spatial_clients_overlap(2, 3), "p2 and p3 do not overlap");
    }

    /* 3. Subscribed Clients Query & Mask */
    {
        uint32_t clients[SPATIAL_MAX_CLIENTS];
        /* Query clients interested in Acre A-1 (block 1, 1) */
        uint32_t count = server_spatial_get_subscribed_clients(0, 1, 1, clients, SPATIAL_MAX_CLIENTS);
        TEST_SPATIAL_ASSERT(count == 2, "2 clients interested in A-1 (P1 and P3)");
        TEST_SPATIAL_ASSERT((clients[0] == 1 && clients[1] == 3) || (clients[0] == 3 && clients[1] == 1), "P1 & P3 in query");

        /* Query clients interested in Acre F-5 (block 5, 6) */
        count = server_spatial_get_subscribed_clients(0, 5, 6, clients, SPATIAL_MAX_CLIENTS);
        TEST_SPATIAL_ASSERT(count == 1, "1 client interested in F-5 (P2)");
        TEST_SPATIAL_ASSERT(clients[0] == 2, "P2 in F-5 query");
    }

    /* 4. Indoor Zone Isolation */
    {
        /* Player 4 enters Nook's Shop (zone_id = 1) */
        server_spatial_update_client_acre(4, 1, 0, 0);
        /* Player 5 enters Nook's Shop (zone_id = 1) */
        server_spatial_update_client_acre(5, 1, 0, 0);
        /* Player 6 enters Museum (zone_id = 2) */
        server_spatial_update_client_acre(6, 2, 0, 0);

        /* P4 and P5 are in same room (zone 1) */
        TEST_SPATIAL_ASSERT(server_spatial_is_client_interested(4, 1, 0, 0), "P4 interested in room zone 1");
        TEST_SPATIAL_ASSERT(server_spatial_is_client_interested(5, 1, 0, 0), "P5 interested in room zone 1");
        TEST_SPATIAL_ASSERT(!server_spatial_is_client_interested(6, 1, 0, 0), "P6 (Museum) NOT interested in Shop");
        TEST_SPATIAL_ASSERT(!server_spatial_is_client_interested(1, 1, 0, 0), "P1 (Outdoors) NOT interested in Shop");

        /* Outdoor events not received indoors */
        TEST_SPATIAL_ASSERT(!server_spatial_is_client_interested(4, 0, 1, 1), "P4 (Shop) NOT interested in outdoors A-1");

        /* Overlap between roommates in Shop */
        TEST_SPATIAL_ASSERT(server_spatial_clients_overlap(4, 5), "P4 & P5 overlap in same shop");
        TEST_SPATIAL_ASSERT(!server_spatial_clients_overlap(4, 6), "P4 & P6 in different rooms do not overlap");
        TEST_SPATIAL_ASSERT(!server_spatial_clients_overlap(4, 1), "P4 (Shop) & P1 (Outdoors) do not overlap");
    }

    /* 5. Client Removal & Cleanup */
    {
        server_spatial_remove_client(1);
        TEST_SPATIAL_ASSERT(server_spatial_get_client(1) == NULL, "P1 removed");
        TEST_SPATIAL_ASSERT(!server_spatial_is_client_interested(1, 0, 1, 1), "Removed client not interested");

        uint32_t clients[SPATIAL_MAX_CLIENTS];
        uint32_t count = server_spatial_get_subscribed_clients(0, 1, 1, clients, SPATIAL_MAX_CLIENTS);
        TEST_SPATIAL_ASSERT(count == 1, "Only P3 now interested in A-1");
        TEST_SPATIAL_ASSERT(clients[0] == 3, "P3 remains");
    }

    server_spatial_reset();
    printf("[SERVER SPATIAL TEST] All 3x3 Acre Subscription & Spatial tests passed successfully!\n");
    return 1;
}
