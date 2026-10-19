/* client_sync.h - Client-side transform streaming and entity interpolation */
#ifndef CLIENT_SYNC_H
#define CLIENT_SYNC_H

#include "types.h"
#include "network/net_packets.h"
#include "m_play.h"
#include "m_player.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLIENT_SYNC_MAX_REMOTE_PLAYERS 8

typedef struct {
    uint32_t client_id;
    bool is_active;
    uint16_t zone_id;
    PLAYER_ACTOR* player_actor;

    /* Current smoothed interpolated position and rotation */
    xyz_t current_pos;
    s16 current_angle_y;

    /* Network target position and rotation from latest snapshot */
    xyz_t target_pos;
    s16 target_angle_y;

    /* Animation and tool state */
    u16 current_anim_id;
    u16 target_anim_id;
    u16 tool_id;
    u16 state_flags;

    /* Timing */
    uint32_t last_packet_ms;
} RemotePlayerState;

typedef struct {
    RemotePlayerState remote_players[CLIENT_SYNC_MAX_REMOTE_PLAYERS];
    uint32_t last_send_ms;
    uint32_t last_input_seq;
} ClientSyncContext;

extern ClientSyncContext g_client_sync;

/* Initialize client sync subsystem */
void client_sync_init(void);

/* Reset client sync subsystem (e.g. on disconnect or scene load) */
void client_sync_reset(void);

/* Receive and process an entity sync packet from the server */
void client_sync_handle_packet(const pkt_s2c_entity_sync_t* pkt);

/* Transmit local player's transform to the server at 30 Hz */
void client_sync_send_local(GAME_PLAY* play);

/* Interpolate remote players and advance animations */
void client_sync_tick(GAME_PLAY* play);

/* Get number of currently active remote players */
int client_sync_get_active_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_SYNC_H */
