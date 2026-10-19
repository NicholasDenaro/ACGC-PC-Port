/* client_sync.c - Client-side transform streaming and entity interpolation */
#include "client/client_sync.h"
#include "client/client_network.h"
#include "server/server_player.h"
#include "network/net_packets.h"
#include "m_common_data.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "m_controller.h"
#include "m_collision_bg.h"
#include "m_field_info.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CLIENT_SEND_INTERVAL_MS 33 /* ~30 Hz client transform streaming */
#define REMOTE_TIMEOUT_MS 600      /* 600ms timeout before despawning remote players */
#define SNAP_DISTANCE_THRESHOLD 450.0f /* Max distance before snapping instead of lerping */

ClientSyncContext g_client_sync = {0};

void client_sync_init(void) {
    memset(&g_client_sync, 0, sizeof(g_client_sync));
    g_client_sync.last_send_ms = SDL_GetTicks();
    printf("[CLIENT SYNC] Client transform sync & interpolation subsystem initialized.\n");
}

void client_sync_reset(void) {
    for (int i = 0; i < CLIENT_SYNC_MAX_REMOTE_PLAYERS; i++) {
        if (g_client_sync.remote_players[i].is_active && g_client_sync.remote_players[i].player_actor != NULL) {
            Actor_delete((ACTOR*)g_client_sync.remote_players[i].player_actor);
        }
    }
    memset(&g_client_sync, 0, sizeof(g_client_sync));
    g_client_sync.last_send_ms = SDL_GetTicks();
}

void client_sync_handle_packet(const pkt_s2c_entity_sync_t* pkt) {
    if (pkt == NULL || !client_network_is_connected()) {
        return;
    }

    uint32_t now = SDL_GetTicks();

    for (uint16_t i = 0; i < pkt->count; i++) {
        const net_entity_state_t* ent = &pkt->entities[i];

        /* Skip target update if it refers to local client */
        if (ent->actor_id == g_client_net.client_id) {
            continue;
        }

        /* Find existing slot or allocate a new one */
        RemotePlayerState* remote = NULL;
        for (int j = 0; j < CLIENT_SYNC_MAX_REMOTE_PLAYERS; j++) {
            if (g_client_sync.remote_players[j].is_active &&
                g_client_sync.remote_players[j].client_id == ent->actor_id) {
                remote = &g_client_sync.remote_players[j];
                break;
            }
        }

        if (remote == NULL) {
            for (int j = 0; j < CLIENT_SYNC_MAX_REMOTE_PLAYERS; j++) {
                if (!g_client_sync.remote_players[j].is_active) {
                    remote = &g_client_sync.remote_players[j];
                    memset(remote, 0, sizeof(RemotePlayerState));
                    remote->client_id = ent->actor_id;
                    remote->is_active = true;
                    remote->zone_id = ent->zone_id;
                    remote->current_pos.x = ent->pos_x;
                    remote->current_pos.y = ent->pos_y;
                    remote->current_pos.z = ent->pos_z;
                    remote->target_pos = remote->current_pos;
                    remote->current_angle_y = ent->angle_y;
                    remote->target_angle_y = ent->angle_y;
                    remote->current_anim_id = ent->anim_id;
                    remote->target_anim_id = ent->anim_id;
                    remote->tool_id = ent->tool_id;
                    remote->state_flags = ent->state_flags;
                    remote->last_packet_ms = now;
                    printf("[CLIENT SYNC] Registered remote player Client ID %u in slot %d\n",
                           (unsigned int)ent->actor_id, j);
                    break;
                }
            }
        }

        if (remote != NULL) {
            remote->zone_id = ent->zone_id;
            remote->target_pos.x = ent->pos_x;
            remote->target_pos.y = ent->pos_y;
            remote->target_pos.z = ent->pos_z;
            remote->target_angle_y = ent->angle_y;
            remote->target_anim_id = ent->anim_id;
            remote->tool_id = ent->tool_id;
            remote->state_flags = ent->state_flags;
            remote->last_packet_ms = now;
        }
    }
}

void client_sync_send_local(GAME_PLAY* play) {
    if (!client_network_is_connected() || play == NULL) {
        return;
    }

    uint32_t now = SDL_GetTicks();
    if (now - g_client_sync.last_send_ms < CLIENT_SEND_INTERVAL_MS) {
        return;
    }

    PLAYER_ACTOR* player = get_player_actor_withoutCheck(play);
    if (player == NULL || player->is_dummy) {
        return;
    }

    pkt_c2s_input_t inp;
    memset(&inp, 0, sizeof(inp));
    inp.sequence = ++g_client_sync.last_input_seq;
    inp.client_id = (u16)g_client_net.client_id;
    inp.player_x = player->actor_class.world.position.x;
    inp.player_y = player->actor_class.world.position.y;
    inp.player_z = player->actor_class.world.position.z;
    inp.angle_y = (s16)player->actor_class.shape_info.rotation.y;
    inp.stick_x = (s8)getJoystick_X();
    inp.stick_y = (s8)getJoystick_Y();
    inp.button_mask = (u32)getButton();

    Private_c* priv = Common_Get(now_private);
    inp.current_tool_id = (priv != NULL) ? priv->equipment : EMPTY_NO;

    client_network_send_input(&inp);
    g_client_sync.last_send_ms = now;
}

void client_sync_tick(GAME_PLAY* play) {
    if (play == NULL) {
        return;
    }

    /* Transmit local transform stream to server */
    client_sync_send_local(play);

    /* Skip remote player actor interpolation during title or intro menus */
    switch (play->scene_id) {
        case SCENE_TITLE_DEMO:
        case SCENE_START_DEMO:
        case SCENE_START_DEMO2:
        case SCENE_START_DEMO3:
        case SCENE_PLAYERSELECT:
        case SCENE_PLAYERSELECT_2:
        case SCENE_PLAYERSELECT_3:
        case SCENE_PLAYERSELECT_SAVE:
            return;
        default:
            break;
    }

    /* Synchronize scene/zone state transitions with dedicated server */
    static int s_last_synced_scene = -1;
    static int s_last_synced_field = -1;
    mActor_name_t cur_field = mFI_GetFieldId();
    if (play->scene_id != s_last_synced_scene || cur_field != s_last_synced_field) {
        s_last_synced_scene = play->scene_id;
        s_last_synced_field = cur_field;
        int house_idx = 0;
        if (mFI_IS_PLAYER_ROOM(cur_field)) {
            house_idx = (int)mFI_GET_PLAYER_ROOM_NO(cur_field);
        }
        client_network_send_zone_change(play->scene_id, house_idx);
        client_sync_reset();
    }

    uint32_t now = SDL_GetTicks();

    for (int i = 0; i < CLIENT_SYNC_MAX_REMOTE_PLAYERS; i++) {
        RemotePlayerState* remote = &g_client_sync.remote_players[i];
        if (!remote->is_active) {
            continue;
        }

        /* If remote player is in a different zone than local player, hide/despawn actor */
        bool same_zone = (play->scene_id == SCENE_FG) ? (remote->zone_id == 0) : (remote->zone_id > 0);
        if (!same_zone) {
            if (remote->player_actor != NULL) {
                Actor_delete((ACTOR*)remote->player_actor);
                remote->player_actor = NULL;
            }
            continue;
        }

        /* Inactivity timeout check */
        if (now - remote->last_packet_ms > REMOTE_TIMEOUT_MS) {
            if (remote->player_actor != NULL) {
                Actor_delete((ACTOR*)remote->player_actor);
                remote->player_actor = NULL;
            }
            remote->is_active = false;
            printf("[CLIENT SYNC] Remote player Client ID %u timed out (despawned).\n", (unsigned int)remote->client_id);
            continue;
        }

        /* Verify tracked actor is still valid in play->actor_info */
        if (remote->player_actor != NULL) {
            int exists = 0;
            for (ACTOR* a = play->actor_info.list[ACTOR_PART_PLAYER].actor; a != NULL; a = a->next_actor) {
                if (a == (ACTOR*)remote->player_actor) {
                    exists = 1;
                    break;
                }
            }
            if (!exists) {
                remote->player_actor = NULL;
            }
        }

        /* Spawn remote player actor if needed */
        if (remote->player_actor == NULL) {
            int actor_slot = i + 1; /* Slots 1..7 for remote players (Slot 0 is local player) */
            if (actor_slot >= MAX_SERVER_CLIENTS) {
                actor_slot = 1;
            }

            s_xyz angle = {0, remote->target_angle_y, 0};
            g_server_clients[actor_slot].is_connected = 1;
            g_server_clients[actor_slot].is_dummy = 1;
            g_server_clients[actor_slot].client_id = remote->client_id;

            remote->player_actor = server_player_spawn_actor(play, actor_slot, remote->current_pos, angle);
            if (remote->player_actor != NULL) {
                remote->player_actor->is_dummy = 1;
                remote->player_actor->client_id = remote->client_id;
                remote->player_actor->player_no = actor_slot;
                remote->player_actor->actor_class.cull_distance = 10000.0f;
                remote->player_actor->actor_class.state_bitfield |=
                    (ACTOR_STATE_NO_CULL | ACTOR_STATE_NO_MOVE_WHILE_CULLED | ACTOR_STATE_NO_DRAW_WHILE_CULLED);
                printf("[CLIENT SYNC] Spawned remote PLAYER_ACTOR for Client ID %u in slot %d.\n",
                       (unsigned int)remote->client_id, actor_slot);
            }
        }

        if (remote->player_actor != NULL) {
            /* Position interpolation (Exponential Decay Lerp) */
            float dx = remote->target_pos.x - remote->current_pos.x;
            float dy = remote->target_pos.y - remote->current_pos.y;
            float dz = remote->target_pos.z - remote->current_pos.z;
            float dist = sqrtf(dx * dx + dz * dz);

            if (dist > SNAP_DISTANCE_THRESHOLD) {
                /* Snap if too far (e.g. teleports or zone warps) */
                remote->current_pos = remote->target_pos;
            } else {
                const float alpha = 0.32f;
                remote->current_pos.x += dx * alpha;
                remote->current_pos.z += dz * alpha;
                remote->current_pos.y += dy * alpha;
            }

            /* Terrain surface snapping */
            f32 ground_y = mCoBG_GetBgY_OnlyCenter_FromWpos2(remote->current_pos, 0.0f);
            if (ground_y > -1000.0f && ground_y < 10000.0f) {
                if (fabsf(ground_y - remote->target_pos.y) <= 16.0f) {
                    remote->current_pos.y = ground_y;
                }
            }

            /* Angle interpolation (Shortest angular distance Slerp) */
            s16 diff_angle = (s16)(remote->target_angle_y - remote->current_angle_y);
            remote->current_angle_y += (s16)(diff_angle * 0.35f);

            /* Apply interpolated transform to actor */
            remote->player_actor->actor_class.world.position = remote->current_pos;
            remote->player_actor->actor_class.shape_info.rotation.y = remote->current_angle_y;
            remote->player_actor->actor_class.world.angle.y = remote->current_angle_y;

            /* Select animation based on velocity / displacement */
            int desired_anim = mPlayer_ANIM_WAIT1;
            if (dist > 3.2f) {
                desired_anim = mPlayer_ANIM_RUN1;
            } else if (dist > 0.35f) {
                desired_anim = mPlayer_ANIM_WALK1;
            }

            if (desired_anim != remote->current_anim_id) {
                remote->current_anim_id = desired_anim;
                extern void pc_player_set_animation(PLAYER_ACTOR* player, GAME* game, int anim_idx);
                pc_player_set_animation(remote->player_actor, (GAME*)play, desired_anim);
            }
        }
    }
}

int client_sync_get_active_count(void) {
    int count = 0;
    for (int i = 0; i < CLIENT_SYNC_MAX_REMOTE_PLAYERS; i++) {
        if (g_client_sync.remote_players[i].is_active) {
            count++;
        }
    }
    return count;
}
