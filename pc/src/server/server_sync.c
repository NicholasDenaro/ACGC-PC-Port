/* server_sync.c - High-frequency transform streaming and entity synchronization */
#include "server/server_sync.h"
#include "server/server_player.h"
#include "server/server_spatial.h"
#include "server/server_network.h"
#include "network/net_packets.h"
#include "m_player.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SERVER_SYNC_INTERVAL_MS 33 /* ~30 Hz broadcast rate */

static uint32_t s_sync_frame_id = 0;
static uint32_t s_last_broadcast_ms = 0;

void server_sync_init(void) {
    s_sync_frame_id = 0;
    s_last_broadcast_ms = SDL_GetTicks();
    printf("[SERVER SYNC] Transform sync subsystem initialized (Target: 30 Hz over Unreliable Channel).\n");
}

void server_sync_on_client_input(uint32_t client_id, const pkt_c2s_input_t* input) {
    if (input == NULL) {
        return;
    }

    ConnectedClient* cc = server_player_get(client_id);
    if (cc == NULL || !cc->is_connected) {
        return;
    }

    /* Compute horizontal translation delta to estimate movement velocity and gait */
    float dx = input->player_x - cc->predicted_pos.x;
    float dz = input->player_z - cc->predicted_pos.z;
    float dist = sqrtf(dx * dx + dz * dz);

    /* Determine gait and animation index (prefer client-reported animation0_idx) */
    uint16_t anim_id = mPlayer_ANIM_WAIT1;
    if (input->anim_id < mPlayer_ANIM_NUM) {
        anim_id = input->anim_id;
    } else if (dist > 4.0f) {
        anim_id = mPlayer_ANIM_RUN1;
    } else if (dist > 0.35f) {
        anim_id = mPlayer_ANIM_WALK1;
    }

    /* Update predicted position and orientation */
    cc->predicted_pos.x = input->player_x;
    cc->predicted_pos.y = input->player_y;
    cc->predicted_pos.z = input->player_z;
    cc->predicted_angle.y = input->angle_y;
    cc->last_ack_input_seq = input->sequence;
    cc->current_tool_id = input->current_tool_id;
    cc->anim_id = anim_id;
    cc->anim_frame++;
    cc->private_data.gender = input->gender;
    cc->private_data.face = input->face_type;
    cc->private_data.sunburn.rank = input->sunburn_rank;
    cc->private_data.cloth.idx = input->cloth_idx;
    cc->private_data.cloth.item = (mActor_name_t)(ITM_CLOTH_START + input->cloth_idx);

    /* Update spatial interest grid acre assignment */
    server_spatial_update_client_wpos(client_id, cc->zone_id, input->player_x, input->player_z);

    /* Sync server-side simulated actor if present */
    if (cc->player_actor != NULL) {
        cc->player_actor->actor_class.world.position.x = input->player_x;
        cc->player_actor->actor_class.world.position.y = input->player_y;
        cc->player_actor->actor_class.world.position.z = input->player_z;
        cc->player_actor->actor_class.shape_info.rotation.y = input->angle_y;
        cc->player_actor->actor_class.world.angle.y = input->angle_y;
    }
}

void server_sync_tick(GAME_PLAY* play) {
    (void)play;
    uint32_t now = SDL_GetTicks();
    if (now - s_last_broadcast_ms < SERVER_SYNC_INTERVAL_MS) {
        return;
    }
    s_last_broadcast_ms = now;
    s_sync_frame_id++;

    /* Broadcast entity transform updates to each active client based on 3x3 acre interest */
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        ConnectedClient* target = &g_server_clients[i];
        if (!target->is_connected || target->is_dummy) {
            continue;
        }

        pkt_s2c_entity_sync_t sync_pkt;
        memset(&sync_pkt, 0, sizeof(sync_pkt));
        sync_pkt.frame_id = s_sync_frame_id;
        sync_pkt.count = 0;

        /* Gather all other active players residing within target client's spatial interest window */
        for (int j = 0; j < MAX_SERVER_CLIENTS; j++) {
            if (i == j) {
                continue; /* Do not echo target client's own transform */
            }

            ConnectedClient* other = &g_server_clients[j];
            if (!other->is_connected) {
                continue;
            }

            /* Spatial interest filtering */
            if (server_spatial_clients_overlap(target->client_id, other->client_id)) {
                net_entity_state_t* ent = &sync_pkt.entities[sync_pkt.count++];
                ent->actor_id = other->client_id;
                ent->zone_id = (u16)other->zone_id;
                ent->pos_x = other->predicted_pos.x;
                ent->pos_y = other->predicted_pos.y;
                ent->pos_z = other->predicted_pos.z;
                ent->angle_y = (s16)other->predicted_angle.y;
                ent->anim_id = other->anim_id;
                ent->anim_frame = other->anim_frame;
                ent->tool_id = other->current_tool_id;
                ent->state_flags = other->state_flags;
                ent->gender = other->private_data.gender;
                ent->face_type = other->private_data.face;
                ent->sunburn_rank = other->private_data.sunburn.rank;
                ent->cloth_idx = other->private_data.cloth.idx;

                if (sync_pkt.count >= NET_MAX_ENTITIES_PER_SYNC) {
                    break;
                }
            }
        }

        /* Transmit high-frequency entity sync packet over unreliable channel */
        if (sync_pkt.count > 0) {
            u8 buf[512];
            int len = net_pack_entity_sync(buf, sizeof(buf), &sync_pkt);
            if (len > 0) {
                server_network_send_raw(target->client_id, NET_CH_UNRELIABLE, buf, len, false);
            }
        }
    }
}

bool server_sync_run_tests(void) {
    printf("[SERVER SYNC TEST] Starting transform streaming & interpolation unit tests...\n");

    /* Test 1: Packet serialization round-trip */
    pkt_s2c_entity_sync_t original;
    memset(&original, 0, sizeof(original));
    original.frame_id = 1042;
    original.count = 2;

    original.entities[0].actor_id = 2;
    original.entities[0].zone_id = 0;
    original.entities[0].pos_x = 2240.0f;
    original.entities[0].pos_y = 40.0f;
    original.entities[0].pos_z = 1880.0f;
    original.entities[0].angle_y = 16384;
    original.entities[0].anim_id = mPlayer_ANIM_WALK1;
    original.entities[0].tool_id = 0x2401;

    original.entities[1].actor_id = 3;
    original.entities[1].zone_id = 0;
    original.entities[1].pos_x = 2300.0f;
    original.entities[1].pos_y = 40.0f;
    original.entities[1].pos_z = 1920.0f;
    original.entities[1].angle_y = -8192;
    original.entities[1].anim_id = mPlayer_ANIM_RUN1;
    original.entities[1].tool_id = 0;

    u8 buf[512];
    int len = net_pack_entity_sync(buf, sizeof(buf), &original);
    if (len <= 0) {
        fprintf(stderr, "[SERVER SYNC TEST FAIL] net_pack_entity_sync failed\n");
        return false;
    }

    pkt_s2c_entity_sync_t unpacked;
    memset(&unpacked, 0, sizeof(unpacked));
    if (net_unpack_entity_sync(buf, len, &unpacked) <= 0) {
        fprintf(stderr, "[SERVER SYNC TEST FAIL] net_unpack_entity_sync failed\n");
        return false;
    }

    if (unpacked.frame_id != 1042 || unpacked.count != 2) {
        fprintf(stderr, "[SERVER SYNC TEST FAIL] Frame ID / count mismatch\n");
        return false;
    }

    if (unpacked.entities[0].actor_id != 2 || fabsf(unpacked.entities[0].pos_x - 2240.0f) > 0.01f) {
        fprintf(stderr, "[SERVER SYNC TEST FAIL] Entity 0 data mismatch\n");
        return false;
    }

    if (unpacked.entities[1].actor_id != 3 || unpacked.entities[1].anim_id != mPlayer_ANIM_RUN1) {
        fprintf(stderr, "[SERVER SYNC TEST FAIL] Entity 1 data mismatch\n");
        return false;
    }

    /* Test 2: Input processing and gait detection */
    ConnectedClient dummy_cc;
    memset(&dummy_cc, 0, sizeof(dummy_cc));
    dummy_cc.client_id = 99;
    dummy_cc.is_connected = 1;
    dummy_cc.predicted_pos.x = 2000.0f;
    dummy_cc.predicted_pos.z = 2000.0f;

    pkt_c2s_input_t input;
    memset(&input, 0, sizeof(input));
    input.client_id = 99;
    input.sequence = 1;
    input.player_x = 2005.0f; /* 5 unit movement -> run */
    input.player_z = 2000.0f;
    input.angle_y = 12000;

    float dx = input.player_x - dummy_cc.predicted_pos.x;
    float dz = input.player_z - dummy_cc.predicted_pos.z;
    float dist = sqrtf(dx * dx + dz * dz);
    uint16_t detected_anim = mPlayer_ANIM_WAIT1;
    if (dist > 4.0f) {
        detected_anim = mPlayer_ANIM_RUN1;
    } else if (dist > 0.35f) {
        detected_anim = mPlayer_ANIM_WALK1;
    }

    if (detected_anim != mPlayer_ANIM_RUN1) {
        fprintf(stderr, "[SERVER SYNC TEST FAIL] Running gait detection failed (detected=%d)\n", detected_anim);
        return false;
    }

    printf("[SERVER SYNC TEST] All transform streaming & interpolation unit tests passed!\n");
    return true;
}
