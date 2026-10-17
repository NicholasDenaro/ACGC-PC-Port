#ifndef CLIENT_IDENTITY_H
#define CLIENT_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>

#define CLIENT_TOKEN_SIZE 32
#define CLIENT_TOKEN_HEX_LEN 64
#define CLIENT_TOKEN_PATH "save/client_token.dat"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize client identity subsystem.
 *        Loads existing 32-byte token from `save/client_token.dat`, or generates
 *        a cryptographically random 32-byte token and persists it if missing.
 * @return true on success, false on failure.
 */
bool client_identity_init(void);

/**
 * @brief Retrieve the 32-byte client authentication token.
 * @param out_token Buffer of at least 32 bytes to copy the token into.
 * @return true if token is loaded and valid, false otherwise.
 */
bool client_identity_get_token(uint8_t out_token[CLIENT_TOKEN_SIZE]);

/**
 * @brief Get human-readable hex string representation of the token (64 chars + null).
 * @param out_hex Buffer of at least 65 bytes.
 * @return true on success, false otherwise.
 */
bool client_identity_get_hex(char out_hex[CLIENT_TOKEN_HEX_LEN + 1]);

/**
 * @brief Check if client currently has a valid identity token.
 */
bool client_identity_is_valid(void);

/**
 * @brief Run unit tests for client identity generation and persistence.
 * @return true if all tests pass, false otherwise.
 */
bool client_identity_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_IDENTITY_H */
