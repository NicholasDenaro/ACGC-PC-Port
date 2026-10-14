/* server_runner.c - Authoritative server tick runner & lifecycle management */
#include "server/server_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ServerConfig g_server_config;
ServerStats g_server_stats = {0};
volatile int g_server_running = 1;

void server_config_init_defaults(ServerConfig* config) {
    if (!config) return;
    memset(config, 0, sizeof(ServerConfig));
    config->port = SERVER_DEFAULT_PORT;
    config->tickrate = SERVER_DEFAULT_TICKRATE;
    config->seed = 0;
    config->has_seed = false;
    config->save_dir = "save";
    config->town_name = NULL;
    config->verbose = false;
}

void server_runner_init(void) {
    memset(&g_server_stats, 0, sizeof(ServerStats));
    g_server_running = 1;
    printf("[SERVER] Server runner initialized (Target Tickrate: %u Hz, Port: %u)\n",
           g_server_config.tickrate, g_server_config.port);
}

void server_runner_shutdown(void) {
    g_server_running = 0;
    printf("[SERVER] Server runner shutting down. Total ticks processed: %llu\n",
           (unsigned long long)g_server_stats.total_ticks);
}

void server_runner_update_stats(double tick_duration_ms) {
    g_server_stats.total_ticks++;
    if (tick_duration_ms > g_server_stats.max_tick_time_ms) {
        g_server_stats.max_tick_time_ms = tick_duration_ms;
    }

    /* Exponential moving average for tick time */
    if (g_server_stats.total_ticks == 1) {
        g_server_stats.avg_tick_time_ms = tick_duration_ms;
    } else {
        const double alpha = 0.05;
        g_server_stats.avg_tick_time_ms = alpha * tick_duration_ms + (1.0 - alpha) * g_server_stats.avg_tick_time_ms;
    }
}

void server_runner_print_status(void) {
    printf("[SERVER STATUS] Ticks: %llu | Avg Tick: %.2f ms | Max Tick: %.2f ms\n",
           (unsigned long long)g_server_stats.total_ticks,
           g_server_stats.avg_tick_time_ms,
           g_server_stats.max_tick_time_ms);
}
