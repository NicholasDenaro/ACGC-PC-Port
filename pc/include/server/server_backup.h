/* server_backup.h - Tiered rolling backups & snapshot management */
#ifndef SERVER_BACKUP_H
#define SERVER_BACKUP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKUP_MAX_MINUTE_SNAPSHOTS 10
#define BACKUP_MAX_HOUR_SNAPSHOTS   24
#define BACKUP_MAX_DAILY_SNAPSHOTS  14

#define BACKUP_INTERVAL_MINUTE_SEC 60
#define BACKUP_INTERVAL_HOUR_SEC   3600
#define BACKUP_INTERVAL_DAILY_SEC  86400

typedef enum {
    BACKUP_TIER_MINUTE = 0,
    BACKUP_TIER_HOUR,
    BACKUP_TIER_DAILY,
    BACKUP_TIER_COUNT
} BackupTier;

/**
 * @brief Initialize the tiered rolling backup subsystem.
 *        Creates directories `save/backups/minute`, `hour`, `daily`.
 * @param save_dir Base save directory (e.g. "save").
 * @return true on success, false on error.
 */
bool server_backup_init(const char* save_dir);

/**
 * @brief Create a snapshot for a specific tier, rotating older snapshots out.
 *        Copies both DobutsunomoriP_MURA.gci and server_extended.dat.
 * @param tier BACKUP_TIER_MINUTE, BACKUP_TIER_HOUR, or BACKUP_TIER_DAILY.
 * @return true on success, false on error.
 */
bool server_backup_create_snapshot(BackupTier tier);

/**
 * @brief Periodic backup tick. Evaluates timestamps and triggers snapshots
 *        when minute (60s), hour (3600s), or daily (86400s) intervals elapse.
 * @param now_sec Current UNIX timestamp in seconds.
 */
void server_backup_tick(uint64_t now_sec);

/**
 * @brief Run unit tests for tiered backup creation and rotation.
 * @return true if all unit tests pass, false otherwise.
 */
bool server_backup_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_BACKUP_H */
