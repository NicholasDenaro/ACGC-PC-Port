#ifndef SERVER_STORAGE_H
#define SERVER_STORAGE_H

#include "types.h"
#include "m_private.h"
#include "m_home.h"
#include <stdint.h>
#include <stdbool.h>

#define SERVER_EXTENDED_MAGIC 0x41434558 /* "ACEX" */
#define SERVER_EXTENDED_VERSION 1
#define MAX_EXTENDED_RESIDENTS 8
#define MAX_EXTENDED_HOUSES 4
#define MAX_EXTENDED_VISITORS 32
#define AUTH_TOKEN_SIZE 32
#define SERVER_DEFAULT_SAVE_DIR "server_save"
#define SERVER_EXTENDED_PATH "server_save/server_extended.dat"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t auth_token[AUTH_TOKEN_SIZE];
    uint8_t house_idx;      /* 0..3 */
    uint8_t roommate_slot;  /* 0..1 */
    uint8_t is_active;      /* 1 = occupied resident, 0 = vacant slot */
    uint8_t pad;
    uint64_t registered_time;
    uint64_t last_seen_time;
    Private_c private_data;
} ServerResident;

typedef struct {
    uint32_t shared_debt;
    uint32_t total_paid;
    uint8_t pad[24];
} ServerHouseExtra;

typedef struct {
    uint8_t auth_token[AUTH_TOKEN_SIZE];
    uint8_t is_active;      /* 1 = active visitor profile, 0 = unused */
    uint8_t pad[3];
    uint64_t first_visit_time;
    uint64_t last_seen_time;
    Private_c private_data;
} ServerVisitor;

typedef struct {
    uint32_t magic;         /* "ACEX" */
    uint32_t version;       /* 1 */
    uint32_t checksum;      /* CRC32 of payload */
    uint32_t reserved;
    uint64_t last_save_time;

    ServerHouseExtra houses[MAX_EXTENDED_HOUSES];
    ServerResident   residents[MAX_EXTENDED_RESIDENTS];
    ServerVisitor    visitors[MAX_EXTENDED_VISITORS];
} ServerExtendedDB;

/**
 * @brief Initialize server extended storage subsystem.
 *        Loads `save/server_extended.dat` if it exists and validates checksum.
 *        If missing or corrupted, initializes a blank database and seeds house structures.
 * @param save_dir Directory containing save files (default: "save" if NULL).
 * @return true on success, false on error.
 */
bool server_storage_init(const char* save_dir);

/**
 * @brief Atomically save the extended database to disk (`.tmp` + rename).
 *        Also syncs primary residents to `DobutsunomoriP_MURA.gci` so vanilla
 *        GameCube compatibility is preserved.
 * @return true on success, false on error.
 */
bool server_storage_save(void);

/**
 * @brief Mark server storage as dirty (mutation occurred).
 *        Will trigger a debounced autosave (1 second cooldown) and periodic backups.
 */
void server_storage_mark_dirty(void);

/**
 * @brief Atomically save both server_extended.dat and DobutsunomoriP_MURA.gci.
 * @return true on success, false on error.
 */
bool server_storage_save_all(void);

/**
 * @brief Periodic simulation tick for server storage & autosaves.
 *        Executes debounced saves when dirty and 60-second periodic backups.
 */
void server_storage_tick(void);

/**
 * @brief Find resident index by auth token.
 * @param token 32-byte authentication token.
 * @return Resident index 0..7 if found, -1 if not found.
 */
int server_storage_find_resident_by_token(const uint8_t token[AUTH_TOKEN_SIZE]);

/**
 * @brief Find visitor index by auth token.
 * @param token 32-byte authentication token.
 * @return Visitor index 0..31 if found, -1 if not found.
 */
int server_storage_find_visitor_by_token(const uint8_t token[AUTH_TOKEN_SIZE]);

/**
 * @brief Look up a player by token, determining whether they are a resident or visitor.
 * @param token 32-byte authentication token.
 * @param out_is_resident Output boolean indicating true for resident, false for visitor.
 * @return Index into residents array (0..7) or visitors array (0..31), or -1 if unknown token.
 */
int server_storage_find_player(const uint8_t token[AUTH_TOKEN_SIZE], bool* out_is_resident);

/**
 * @brief Register or update a visitor profile.
 * @param token 32-byte authentication token.
 * @param priv Player data.
 * @return Visitor index 0..31 on success, -1 if visitor database full.
 */
int server_storage_register_visitor(const uint8_t token[AUTH_TOKEN_SIZE], const Private_c* priv);

/**
 * @brief Check if a house has an open roommate slot (< 2 residents).
 * @param house_idx House index 0..3.
 * @return Number of vacant slots (0, 1, or 2).
 */
int server_storage_get_house_vacancies(uint8_t house_idx);

/**
 * @brief Promote a visitor or token to become a town resident in the specified house.
 * @param token 32-byte authentication token.
 * @param house_idx House index 0..3.
 * @param priv Player data (or NULL to use existing visitor data).
 * @param out_resident_idx Output resident index (0..7) assigned.
 * @return true on success, false if house is full or error.
 */
bool server_storage_promote_to_resident(const uint8_t token[AUTH_TOKEN_SIZE], uint8_t house_idx,
                                       const Private_c* priv, int* out_resident_idx);

/**
 * @brief Evict a resident from a house (marks slot vacant, frees up house slot).
 * @param resident_idx Resident index 0..7.
 * @return true on success, false if invalid index.
 */
bool server_storage_evict_resident(int resident_idx);

/**
 * @brief Get pointer to resident data.
 */
ServerResident* server_storage_get_resident(int resident_idx);

/**
 * @brief Get pointer to visitor data.
 */
ServerVisitor* server_storage_get_visitor(int visitor_idx);

/**
 * @brief Get roommate resident index for a resident (0<->1, 2<->3, 4<->5, 6<->7).
 * @param resident_idx Resident index 0..7.
 * @return Roommate resident index, or -1 if the roommate slot is vacant.
 */
int server_storage_get_roommate(int resident_idx);

/**
 * @brief Get house mortgage balance for a house.
 */
uint32_t server_storage_get_house_debt(uint8_t house_idx);

/**
 * @brief Make a mortgage debt payment towards a house.
 *        Reduces debt on the house and syncs across both roommates.
 * @param house_idx House index 0..3.
 * @param bells Amount paid in Bells.
 * @return Remaining mortgage debt balance.
 */
uint32_t server_storage_pay_house_debt(uint8_t house_idx, uint32_t bells);

/**
 * @brief Set the house mortgage balance (e.g. upon house expansion).
 */
void server_storage_set_house_debt(uint8_t house_idx, uint32_t debt);

/**
 * @brief Synchronize primary residents and house ownership to canonical Save data
 *        (which is saved to DobutsunomoriP_MURA.gci) for 100% vanilla GameCube compatibility.
 */
void server_storage_sync_to_gci(void);

/**
 * @brief Run automated unit tests for extended storage, token lookups, and roommate mechanics.
 * @return true if all tests pass, false otherwise.
 */
bool server_storage_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_STORAGE_H */
