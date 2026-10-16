/* server_main.c - Headless dedicated server entry point */
#include "pc_platform.h"
#include "pc_assets.h"
#include "pc_disc.h"
#include "pc_settings.h"
#include "server/server_runner.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Global state symbols required by PC layer */
SDL_Window*    g_pc_window = NULL;
SDL_GLContext  g_pc_gl_context = NULL;
int            g_pc_running = 1;
int            g_pc_frame_limit_override = -1;
int            g_pc_speedhack_enabled = 0;
int            g_pc_verbose = 1;
int            g_pc_time_override = -1;
int            g_pc_min_override = -1;
int            g_pc_sec_override = -1;
int            g_pc_date_month = -1;
int            g_pc_date_day = -1;
int            g_pc_date_year = -1;
int            g_pc_weather_override = -1;
int            g_pc_weather_intensity_override = 0;
int            g_pc_window_w = PC_SCREEN_WIDTH;
int            g_pc_window_h = PC_SCREEN_HEIGHT;
int            g_pc_widescreen_stretch = 0;

unsigned int pc_image_base = 0;
unsigned int pc_image_end  = 0;

/* Platform stubs for headless server */
void pc_platform_update_window_size(void) {
    g_pc_window_w = PC_SCREEN_WIDTH;
    g_pc_window_h = PC_SCREEN_HEIGHT;
}

void pc_platform_swap_buffers(void) {
    /* Headless server - no window buffers to swap */
}

int pc_platform_poll_events(void) {
    return g_pc_running && g_server_running;
}

void pc_platform_shutdown(void) {
    static int s_shutdown_done = 0;
    if (s_shutdown_done) return;
    s_shutdown_done = 1;

    server_runner_shutdown();
    pc_disc_shutdown();
    if (g_pc_log_file != NULL) {
        fclose(g_pc_log_file);
        g_pc_log_file = NULL;
    }
    SDL_Quit();
}

/* Game entry points */
extern void ac_entry(void);
extern int boot_main(int argc, const char** argv);

static void handle_signal(int sig) {
    (void)sig;
    printf("\n[SERVER] Shutdown signal received. Exiting gracefully...\n");
    g_server_running = 0;
    g_pc_running = 0;
}

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            printf("\n[SERVER] Console event (%lu). Exiting gracefully...\n", ctrl_type);
            g_server_running = 0;
            g_pc_running = 0;
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

static void print_usage(const char* prog) {
    printf("Animal Crossing Headless Dedicated Server\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --port <port>       Network listening port (default: %d)\n", SERVER_DEFAULT_PORT);
    printf("  --tickrate <rate>   Server simulation tickrate in Hz (default: %d)\n", SERVER_DEFAULT_TICKRATE);
    printf("  --seed <u32>        RNG seed for new town map generation\n");
    printf("  --town-name <name>  Custom town name for new town\n");
    printf("  --save-dir <path>   Directory containing save files (default: 'save')\n");
    printf("  --spawn-dummy-player Spawn a simulated player into the world for testing\n");
    printf("  -v, --verbose       Enable verbose diagnostic output\n");
    printf("  -h, --help          Show this help message\n");
}

int main(int argc, char** argv) {
    if (g_pc_log_file == NULL) {
        g_pc_log_file = fopen("server.log", "w");
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    server_config_init_defaults(&g_server_config);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_server_config.port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tickrate") == 0 && i + 1 < argc) {
            int r = atoi(argv[++i]);
            if (r > 0 && r <= 120) g_server_config.tickrate = (uint32_t)r;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            g_server_config.seed = (uint32_t)strtoul(argv[++i], NULL, 0);
            g_server_config.has_seed = true;
        } else if (strcmp(argv[i], "--town-name") == 0 && i + 1 < argc) {
            g_server_config.town_name = argv[++i];
        } else if (strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) {
            g_server_config.save_dir = argv[++i];
        } else if (strcmp(argv[i], "--spawn-dummy-player") == 0) {
            g_server_config.spawn_dummy_player = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_server_config.verbose = true;
            g_pc_verbose = 1;
        }
    }

    printf("==============================================================\n");
    printf(" Animal Crossing PC Port - Authoritative Dedicated Server\n");
    printf("==============================================================\n");
    printf("[SERVER] Starting dedicated server daemon on port %u (Tickrate: %u Hz)...\n",
           g_server_config.port, g_server_config.tickrate);

    /* Signal handlers for clean termination */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

    /* Setup image boundaries for seg2k0 pointer checking */
#ifdef _WIN32
    {
        HMODULE exe = GetModuleHandle(NULL);
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)exe + dos->e_lfanew);
        pc_image_base = (unsigned int)(uintptr_t)exe;
        pc_image_end = pc_image_base + nt->OptionalHeader.SizeOfImage;
    }
#endif

    /* Minimal platform initialization (timers only; no video window or audio device) */
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "[SERVER ERROR] SDL_Init(TIMER) failed: %s\n", SDL_GetError());
        return 1;
    }

    pc_settings_load();
    pc_disc_init();
    if (!pc_assets_init()) {
        fprintf(stderr, "[SERVER ERROR] No game data found.\n");
        fprintf(stderr, "Animal Crossing dedicated server requires the original GameCube ROM.\n");
        fprintf(stderr, "Place a disc image (.iso, .gcm, or .ciso) in 'rom/' or 'orig/'.\n");
        pc_disc_shutdown();
        SDL_Quit();
        return 1;
    }

    server_runner_init();

    /* Boot game engine into headless gameplay */
    printf("[SERVER] Booting game engine...\n");
    ac_entry();
    boot_main(argc, (const char**)argv);

    /* Shutdown */
    pc_platform_shutdown();
    printf("[SERVER] Daemon terminated.\n");
    return 0;
}
