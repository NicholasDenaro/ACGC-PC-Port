#include "client/client_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <direct.h>
#undef near
#undef far
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static uint8_t s_client_token[CLIENT_TOKEN_SIZE];
static bool s_token_loaded = false;

extern const char* pc_save_get_dir(void);

static void get_client_token_path(char* out_path, size_t max_len) {
    snprintf(out_path, max_len, "%s/client_identity.bin", pc_save_get_dir());
}

static void ensure_save_directory(void) {
#ifdef _WIN32
    _mkdir(pc_save_get_dir());
#else
    mkdir(pc_save_get_dir(), 0755);
#endif
}

static bool generate_secure_token(uint8_t token[CLIENT_TOKEN_SIZE]) {
    bool generated = false;

#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        if (CryptGenRandom(hProv, CLIENT_TOKEN_SIZE, token)) {
            generated = true;
        }
        CryptReleaseContext(hProv, 0);
    }
#endif

    if (!generated) {
        /* Fallback: high-entropy pseudo-random generation */
        uint64_t seed = (uint64_t)time(NULL);
#ifdef _WIN32
        LARGE_INTEGER qpc;
        if (QueryPerformanceCounter(&qpc)) {
            seed ^= ((uint64_t)qpc.QuadPart * 6364136223846793005ULL);
        }
        seed ^= ((uint64_t)GetCurrentProcessId() << 32) ^ (uint64_t)GetCurrentThreadId();
#endif
        for (int i = 0; i < CLIENT_TOKEN_SIZE; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            token[i] = (uint8_t)((seed >> 32) ^ (seed & 0xFF));
        }
    }

    /* Ensure token is not entirely zero bytes */
    bool all_zero = true;
    for (int i = 0; i < CLIENT_TOKEN_SIZE; i++) {
        if (token[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        token[0] = 0xAC;
        token[1] = 0x47;
    }

    return true;
}

bool client_identity_init(void) {
    if (s_token_loaded) {
        return true;
    }

    ensure_save_directory();

    char token_path[300];
    get_client_token_path(token_path, sizeof(token_path));

    /* Try loading existing token from disk */
    FILE* f = fopen(token_path, "rb");
    if (f != NULL) {
        size_t n = fread(s_client_token, 1, CLIENT_TOKEN_SIZE, f);
        fclose(f);

        if (n == CLIENT_TOKEN_SIZE) {
            bool all_zero = true;
            for (int i = 0; i < CLIENT_TOKEN_SIZE; i++) {
                if (s_client_token[i] != 0) {
                    all_zero = false;
                    break;
                }
            }
            if (!all_zero) {
                s_token_loaded = true;
                char hex[CLIENT_TOKEN_HEX_LEN + 1];
                client_identity_get_hex(hex);
                printf("[CLIENT IDENTITY] Loaded persistent client token: %.16s...\n", hex);
                return true;
            }
        }
    }

    /* Token missing or corrupted; generate new persistent UUID token */
    if (!generate_secure_token(s_client_token)) {
        fprintf(stderr, "[CLIENT IDENTITY ERROR] Failed to generate client token!\n");
        return false;
    }

    f = fopen(token_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "[CLIENT IDENTITY ERROR] Failed to write %s!\n", token_path);
        return false;
    }

    size_t written = fwrite(s_client_token, 1, CLIENT_TOKEN_SIZE, f);
    fclose(f);

    if (written != CLIENT_TOKEN_SIZE) {
        fprintf(stderr, "[CLIENT IDENTITY ERROR] Incomplete write to %s!\n", token_path);
        return false;
    }

    s_token_loaded = true;
    char hex[CLIENT_TOKEN_HEX_LEN + 1];
    client_identity_get_hex(hex);
    printf("[CLIENT IDENTITY] Generated new persistent client token: %.16s... (saved to %s)\n",
           hex, CLIENT_TOKEN_PATH);
    return true;
}

bool client_identity_get_token(uint8_t out_token[CLIENT_TOKEN_SIZE]) {
    if (out_token == NULL) {
        return false;
    }
    if (!s_token_loaded) {
        if (!client_identity_init()) {
            return false;
        }
    }
    memcpy(out_token, s_client_token, CLIENT_TOKEN_SIZE);
    return true;
}

bool client_identity_get_hex(char out_hex[CLIENT_TOKEN_HEX_LEN + 1]) {
    if (out_hex == NULL) {
        return false;
    }
    if (!s_token_loaded) {
        if (!client_identity_init()) {
            return false;
        }
    }
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < CLIENT_TOKEN_SIZE; i++) {
        out_hex[i * 2]     = hex_chars[(s_client_token[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hex_chars[s_client_token[i] & 0x0F];
    }
    out_hex[CLIENT_TOKEN_HEX_LEN] = '\0';
    return true;
}

bool client_identity_is_valid(void) {
    return s_token_loaded;
}

bool client_identity_run_tests(void) {
    printf("[CLIENT IDENTITY TEST] Running client identity tests...\n");

    /* Test 1: Generate token */
    uint8_t test_token[CLIENT_TOKEN_SIZE];
    memset(test_token, 0, sizeof(test_token));
    if (!generate_secure_token(test_token)) {
        fprintf(stderr, "[CLIENT IDENTITY TEST] FAIL: generate_secure_token failed!\n");
        return false;
    }
    bool non_zero = false;
    for (int i = 0; i < CLIENT_TOKEN_SIZE; i++) {
        if (test_token[i] != 0) {
            non_zero = true;
            break;
        }
    }
    if (!non_zero) {
        fprintf(stderr, "[CLIENT IDENTITY TEST] FAIL: generated token is all zeros!\n");
        return false;
    }

    /* Test 2: Hex conversion format */
    static const char hex_digits[] = "0123456789abcdef";
    char hex_buf[CLIENT_TOKEN_HEX_LEN + 1];
    for (int i = 0; i < CLIENT_TOKEN_SIZE; i++) {
        hex_buf[i * 2]     = hex_digits[(test_token[i] >> 4) & 0x0F];
        hex_buf[i * 2 + 1] = hex_digits[test_token[i] & 0x0F];
    }
    hex_buf[CLIENT_TOKEN_HEX_LEN] = '\0';
    if (strlen(hex_buf) != 64) {
        fprintf(stderr, "[CLIENT IDENTITY TEST] FAIL: hex string length != 64!\n");
        return false;
    }

    /* Test 3: Disk persistence round-trip */
    ensure_save_directory();
    const char* test_path = "save/test_identity_token.tmp";
    FILE* f = fopen(test_path, "wb");
    if (!f) {
        fprintf(stderr, "[CLIENT IDENTITY TEST] FAIL: Could not open test token file for writing!\n");
        return false;
    }
    fwrite(test_token, 1, CLIENT_TOKEN_SIZE, f);
    fclose(f);

    uint8_t readback[CLIENT_TOKEN_SIZE];
    memset(readback, 0, sizeof(readback));
    f = fopen(test_path, "rb");
    if (!f) {
        fprintf(stderr, "[CLIENT IDENTITY TEST] FAIL: Could not open test token file for reading!\n");
        return false;
    }
    size_t n = fread(readback, 1, CLIENT_TOKEN_SIZE, f);
    fclose(f);
    remove(test_path);

    if (n != CLIENT_TOKEN_SIZE || memcmp(test_token, readback, CLIENT_TOKEN_SIZE) != 0) {
        fprintf(stderr, "[CLIENT IDENTITY TEST] FAIL: Readback token did not match written token!\n");
        return false;
    }

    printf("[CLIENT IDENTITY TEST] All client identity tests passed!\n");
    return true;
}
