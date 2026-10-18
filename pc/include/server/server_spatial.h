/* server_spatial.h - 3x3 Acre Subscription & Spatial Interest Grid */
#ifndef SERVER_SPATIAL_H
#define SERVER_SPATIAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Grid dimensions matching GameCube Animal Crossing world */
#define SPATIAL_BLOCK_X_NUM 7
#define SPATIAL_BLOCK_Z_NUM 10
#define SPATIAL_FG_X_NUM    5
#define SPATIAL_FG_Z_NUM    6
#define SPATIAL_FG_TOTAL    (SPATIAL_FG_X_NUM * SPATIAL_FG_Z_NUM) /* 30 acres */

/* Block sizes in world units (1 unit = 40.0f, 1 block = 16 units = 640.0f) */
#define SPATIAL_ACRE_SIZE_F 640.0f
#define SPATIAL_UNIT_SIZE_F 40.0f

#define SPATIAL_MAX_CLIENTS 8

typedef struct {
    uint32_t client_id;
    uint32_t zone_id;       /* 0 = outdoor town, >0 = indoor room */
    int      block_x;       /* Block X in [0..6] */
    int      block_z;       /* Block Z in [0..9] */
    int      fg_x;          /* Foreground X in [0..4], or -1 if border */
    int      fg_z;          /* Foreground Z in [0..5], or -1 if border */
    bool     is_outdoor;
    bool     is_registered;

    /* 3x3 window bounds in block coordinates */
    int min_bx;
    int max_bx;
    int min_bz;
    int max_bz;

    /* Bitmask of subscribed FG acres: bit = (fg_z * 5 + fg_x) */
    uint32_t fg_acre_mask;
} SpatialClientInfo;

/* Lifecycle */
void server_spatial_init(void);
void server_spatial_reset(void);

/* Coordinate conversions */
bool server_spatial_wpos_to_block(float world_x, float world_z, int* out_bx, int* out_bz);
bool server_spatial_block_to_fg(int bx, int bz, int* out_fg_x, int* out_fg_z);
bool server_spatial_fg_to_block(int fg_x, int fg_z, int* out_bx, int* out_bz);
bool server_spatial_fg_to_name(int fg_x, int fg_z, char* out_buf, size_t buf_size);
bool server_spatial_block_to_name(int bx, int bz, char* out_buf, size_t buf_size);

/* Client spatial management */
void server_spatial_update_client_acre(uint32_t client_id, uint32_t zone_id, int bx, int bz);
void server_spatial_update_client_wpos(uint32_t client_id, uint32_t zone_id, float world_x, float world_z);
void server_spatial_remove_client(uint32_t client_id);
const SpatialClientInfo* server_spatial_get_client(uint32_t client_id);

/* Spatial queries */
bool server_spatial_is_client_interested(uint32_t client_id, uint32_t zone_id, int bx, int bz);
bool server_spatial_is_client_interested_wpos(uint32_t client_id, uint32_t zone_id, float world_x, float world_z);
uint32_t server_spatial_get_subscribed_clients(uint32_t zone_id, int bx, int bz, uint32_t* out_client_ids, uint32_t max_clients);
uint32_t server_spatial_get_client_mask(uint32_t zone_id, int bx, int bz);
bool server_spatial_clients_overlap(uint32_t client_a, uint32_t client_b);

/* Self-test suite */
int server_spatial_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_SPATIAL_H */
