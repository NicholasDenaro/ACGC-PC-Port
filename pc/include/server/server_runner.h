/* server_runner.h - Authoritative server tick runner & lifecycle management */
#ifndef SERVER_RUNNER_H
#define SERVER_RUNNER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_DEFAULT_PORT 21025
#define SERVER_DEFAULT_TICKRATE 30

typedef struct {
    uint16_t port;
    uint32_t tickrate;
    uint32_t seed;
    bool has_seed;
    const char* save_dir;
    const char* town_name;
    bool verbose;
} ServerConfig;

typedef struct {
    uint64_t total_ticks;
    double avg_tick_time_ms;
    double max_tick_time_ms;
    double current_tps;
} ServerStats;

extern ServerConfig g_server_config;
extern ServerStats g_server_stats;
extern volatile int g_server_running;

void server_config_init_defaults(ServerConfig* config);
void server_runner_init(void);
void server_runner_shutdown(void);
void server_runner_update_stats(double tick_duration_ms);
void server_runner_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_RUNNER_H */
