/* server_player.h - Multi-player ConnectedClient registry and tracking */
#ifndef SERVER_PLAYER_H
#define SERVER_PLAYER_H

#include "types.h"
#include "m_private.h"
#include "m_player.h"
#include "m_play.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SERVER_CLIENTS 8

typedef struct {
    uint32_t client_id;
    int is_connected;
    int is_dummy;                /* TRUE if spawned as a simulated/dummy player */
    uint32_t zone_id;            /* Current zone: 0 for outdoor, >0 for room */
    PLAYER_ACTOR* player_actor;  /* Allocated in zone's actor_info */
    Private_c private_data;      /* Name, pockets, bells, design, face, hair */
    xyz_t predicted_pos;
    s_xyz predicted_angle;
    uint32_t last_ack_input_seq;
    uint8_t auth_token[32];      /* Client persistent authentication token */
    int resident_idx;            /* 0..7 if resident, -1 if visitor */
    uint16_t current_tool_id;    /* Equipped tool item ID */
    uint16_t anim_id;            /* Current animation index */
    uint16_t anim_frame;         /* Animation sequence frame counter */
    uint16_t state_flags;        /* State flags */
} ConnectedClient;

extern ConnectedClient g_server_clients[MAX_SERVER_CLIENTS];

void server_player_init(void);
ConnectedClient* server_player_get(uint32_t client_id);
ConnectedClient* server_player_get_by_actor(PLAYER_ACTOR* actor);
ConnectedClient* server_player_get_by_slot(int slot);
int server_player_register(uint32_t client_id, uint32_t zone_id, const Private_c* priv_data);
void server_player_unregister(uint32_t client_id);
int server_player_count(void);
PLAYER_ACTOR* server_player_spawn_actor(GAME_PLAY* play, int slot, xyz_t pos, s_xyz angle);
int server_spawn_dummy_player(GAME_PLAY* play, const char* name, xyz_t pos);
int server_spawn_dummy_player_north(void);
int server_is_dummy_player_alive(void);
void server_player_tick(GAME_PLAY* play);
Private_c* server_player_get_private_by_actor(PLAYER_ACTOR* actor);
Private_c* server_player_get_private_by_slot(int slot);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_PLAYER_H */
