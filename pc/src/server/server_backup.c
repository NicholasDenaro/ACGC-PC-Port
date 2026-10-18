/* server_backup.c - Tiered rolling backups & snapshot management */
#include "server/server_backup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

static char s_base_save_dir[256] = "server_save";
static char s_tier_dirs[BACKUP_TIER_COUNT][300];
static const char* const s_tier_names[BACKUP_TIER_COUNT] = {
    "minute",
    "hour",
    "daily"
};

static const int s_tier_max_snapshots[BACKUP_TIER_COUNT] = {
    BACKUP_MAX_MINUTE_SNAPSHOTS,
    BACKUP_MAX_HOUR_SNAPSHOTS,
    BACKUP_MAX_DAILY_SNAPSHOTS
};

static const uint64_t s_tier_intervals_sec[BACKUP_TIER_COUNT] = {
    BACKUP_INTERVAL_MINUTE_SEC,
    BACKUP_INTERVAL_HOUR_SEC,
    BACKUP_INTERVAL_DAILY_SEC
};

static uint64_t s_tier_last_backup_time[BACKUP_TIER_COUNT] = { 0, 0, 0 };
static bool s_backup_initialized = false;

static void create_directory_recursive(const char* path) {
    if (!path || !path[0]) return;
    char temp[512];
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    for (char* p = temp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char slash = *p;
            *p = '\0';
#ifdef _WIN32
            _mkdir(temp);
#else
            mkdir(temp, 0755);
#endif
            *p = slash;
        }
    }
#ifdef _WIN32
    _mkdir(temp);
#else
    mkdir(temp, 0755);
#endif
}

static bool copy_file(const char* src, const char* dst) {
#ifdef _WIN32
    if (CopyFileA(src, dst, FALSE)) {
        return true;
    }
#endif
    FILE* in = fopen(src, "rb");
    if (!in) return false;

    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    char buffer[65536];
    size_t bytes;
    bool success = true;
    while ((bytes = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, bytes, out) != bytes) {
            success = false;
            break;
        }
    }

    fflush(out);
    fclose(out);
    fclose(in);

    if (!success) {
        remove(dst);
    }
    return success;
}

static void rotate_tier_files(const char* tier_dir, const char* suffix, int max_keep) {
    char old_path[512];
    char new_path[512];

    /* 1. Delete oldest snapshot if it exists */
    snprintf(old_path, sizeof(old_path), "%s/snapshot_%02d_%s", tier_dir, max_keep, suffix);
    remove(old_path);

    /* 2. Shift older snapshots down: snapshot_09 -> snapshot_10, etc. */
    for (int i = max_keep - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s/snapshot_%02d_%s", tier_dir, i, suffix);
        snprintf(new_path, sizeof(new_path), "%s/snapshot_%02d_%s", tier_dir, i + 1, suffix);

        /* Remove destination if it exists before renaming */
        remove(new_path);
#ifdef _WIN32
        MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING);
#else
        rename(old_path, new_path);
#endif
    }
}

bool server_backup_init(const char* save_dir) {
    if (save_dir && save_dir[0] != '\0') {
        strncpy(s_base_save_dir, save_dir, sizeof(s_base_save_dir) - 1);
        s_base_save_dir[sizeof(s_base_save_dir) - 1] = '\0';
    }

    char backups_root[300];
    snprintf(backups_root, sizeof(backups_root), "%s/backups", s_base_save_dir);
    create_directory_recursive(s_base_save_dir);
    create_directory_recursive(backups_root);

    for (int t = 0; t < BACKUP_TIER_COUNT; t++) {
        snprintf(s_tier_dirs[t], sizeof(s_tier_dirs[t]), "%s/backups/%s", s_base_save_dir, s_tier_names[t]);
        create_directory_recursive(s_tier_dirs[t]);
    }

    uint64_t now_sec = (uint64_t)time(NULL);
    for (int t = 0; t < BACKUP_TIER_COUNT; t++) {
        s_tier_last_backup_time[t] = now_sec;
    }

    s_backup_initialized = true;
    printf("[SERVER BACKUP] Tiered rolling backup subsystem initialized (%s/backups/[minute,hour,daily])\n",
           s_base_save_dir);
    return true;
}

bool server_backup_create_snapshot(BackupTier tier) {
    if (!s_backup_initialized || tier < 0 || tier >= BACKUP_TIER_COUNT) {
        return false;
    }

    const char* tier_dir = s_tier_dirs[tier];
    int max_keep = s_tier_max_snapshots[tier];

    /* Paths to active live saves */
    char gci_src[300];
    char ext_src[300];
    snprintf(gci_src, sizeof(gci_src), "%s/card_a/DobutsunomoriP_MURA.gci", s_base_save_dir);
    snprintf(ext_src, sizeof(ext_src), "%s/server_extended.dat", s_base_save_dir);

    /* 1. Rotate existing snapshots */
    rotate_tier_files(tier_dir, "DobutsunomoriP_MURA.gci", max_keep);
    rotate_tier_files(tier_dir, "server_extended.dat", max_keep);

    /* 2. Copy current live files to snapshot_01 */
    char gci_dst[320];
    char ext_dst[320];
    snprintf(gci_dst, sizeof(gci_dst), "%s/snapshot_01_DobutsunomoriP_MURA.gci", tier_dir);
    snprintf(ext_dst, sizeof(ext_dst), "%s/snapshot_01_server_extended.dat", tier_dir);

    bool gci_copied = false;
    bool ext_copied = false;

    struct stat st;
    if (stat(gci_src, &st) == 0) {
        gci_copied = copy_file(gci_src, gci_dst);
    }
    if (stat(ext_src, &st) == 0) {
        ext_copied = copy_file(ext_src, ext_dst);
    }

    printf("[SERVER BACKUP] [%s] Created snapshot_01 (GCI: %s, EXT: %s)\n",
           s_tier_names[tier], gci_copied ? "OK" : "SKIP/ERR", ext_copied ? "OK" : "SKIP/ERR");

    return gci_copied || ext_copied;
}

void server_backup_tick(uint64_t now_sec) {
    if (!s_backup_initialized) {
        return;
    }

    for (int t = 0; t < BACKUP_TIER_COUNT; t++) {
        if (now_sec >= s_tier_last_backup_time[t] + s_tier_intervals_sec[t]) {
            server_backup_create_snapshot((BackupTier)t);
            s_tier_last_backup_time[t] = now_sec;
        }
    }
}

bool server_backup_run_tests(void) {
    printf("[SERVER BACKUP TEST] Starting tiered backup unit tests...\n");

    const char* test_dir = "save/test_backups";
    char minute_dir[512];
    snprintf(minute_dir, sizeof(minute_dir), "%s/backups/minute", test_dir);

    create_directory_recursive("save");
    create_directory_recursive(test_dir);
    create_directory_recursive(minute_dir);

    /* Create dummy live files */
    char dummy_gci[512];
    char dummy_ext[512];
    snprintf(dummy_gci, sizeof(dummy_gci), "%s/card_a", test_dir);
    create_directory_recursive(dummy_gci);
    snprintf(dummy_gci, sizeof(dummy_gci), "%s/card_a/DobutsunomoriP_MURA.gci", test_dir);
    snprintf(dummy_ext, sizeof(dummy_ext), "%s/server_extended.dat", test_dir);

    FILE* f1 = fopen(dummy_gci, "wb");
    if (f1) {
        fputs("GCI_TEST_DATA", f1);
        fclose(f1);
    }
    FILE* f2 = fopen(dummy_ext, "wb");
    if (f2) {
        fputs("EXT_TEST_DATA", f2);
        fclose(f2);
    }

    /* Test rotation: rotate 12 times into a tier limited to 5 */
    for (int round = 1; round <= 12; round++) {
        /* Update content slightly so each snapshot is distinct */
        f1 = fopen(dummy_gci, "wb");
        if (f1) {
            fprintf(f1, "GCI_ROUND_%02d", round);
            fclose(f1);
        }

        rotate_tier_files(minute_dir, "DobutsunomoriP_MURA.gci", 5);
        char dst[512];
        snprintf(dst, sizeof(dst), "%s/snapshot_01_DobutsunomoriP_MURA.gci", minute_dir);
        copy_file(dummy_gci, dst);
    }

    /* Verify snapshot_01 contains ROUND_12 */
    char check_path[512];
    snprintf(check_path, sizeof(check_path), "%s/snapshot_01_DobutsunomoriP_MURA.gci", minute_dir);
    FILE* check = fopen(check_path, "rb");
    if (!check) {
        fprintf(stderr, "[SERVER BACKUP TEST] FAIL: snapshot_01 missing!\n");
        return false;
    }
    char buf[64] = {0};
    fread(buf, 1, sizeof(buf) - 1, check);
    fclose(check);

    if (strcmp(buf, "GCI_ROUND_12") != 0) {
        fprintf(stderr, "[SERVER BACKUP TEST] FAIL: expected GCI_ROUND_12, got '%s'!\n", buf);
        return false;
    }

    /* Verify snapshot_05 exists and contains ROUND_08 */
    snprintf(check_path, sizeof(check_path), "%s/snapshot_05_DobutsunomoriP_MURA.gci", minute_dir);
    check = fopen(check_path, "rb");
    if (!check) {
        fprintf(stderr, "[SERVER BACKUP TEST] FAIL: snapshot_05 missing!\n");
        return false;
    }
    memset(buf, 0, sizeof(buf));
    fread(buf, 1, sizeof(buf) - 1, check);
    fclose(check);

    if (strcmp(buf, "GCI_ROUND_08") != 0) {
        fprintf(stderr, "[SERVER BACKUP TEST] FAIL: expected GCI_ROUND_08, got '%s'!\n", buf);
        return false;
    }

    /* Verify snapshot_06 does NOT exist (capped at 5) */
    snprintf(check_path, sizeof(check_path), "%s/snapshot_06_DobutsunomoriP_MURA.gci", minute_dir);
    struct stat st;
    if (stat(check_path, &st) == 0) {
        fprintf(stderr, "[SERVER BACKUP TEST] FAIL: snapshot_06 exists beyond max 5!\n");
        return false;
    }

    /* Cleanup test artifacts */
    for (int i = 1; i <= 6; i++) {
        snprintf(check_path, sizeof(check_path), "%s/snapshot_%02d_DobutsunomoriP_MURA.gci", minute_dir, i);
        remove(check_path);
    }
    remove(dummy_gci);
    remove(dummy_ext);

    printf("[SERVER BACKUP TEST] All tiered backup unit tests passed successfully!\n");
    return true;
}
