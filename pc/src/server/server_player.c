/* server_player.c - Multi-player ConnectedClient registry and tracking */
#include "server/server_player.h"
#include "server/server_zone.h"
#include "server/server_runner.h"
#include "server/server_network.h"
#include "server/server_storage.h"
#include "m_common_data.h"
#include "m_name_table.h"
#include "m_actor.h"
#include "m_play.h"
#include "m_string.h"
#include "m_collision_bg.h"
#include "m_player_lib.h"
#include "m_font.h"
#include "m_land.h"
#include "m_private.h"
#include "m_event.h"
#include "m_field_info.h"
#include "m_scene_table.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

ConnectedClient g_server_clients[MAX_SERVER_CLIENTS] = {0};
static int s_player_registry_initialized = 0;

void server_player_init(void) {
    memset(g_server_clients, 0, sizeof(g_server_clients));

#ifndef TARGET_SERVER
    /* On client, slot 0 is the local player */
    g_server_clients[0].client_id = 1;
    g_server_clients[0].is_connected = 1;
    g_server_clients[0].is_dummy = 0;
    g_server_clients[0].zone_id = 0; /* Permanent outdoor town */

    Private_c* now_priv = Common_Get(now_private);
    if (now_priv != NULL) {
        memcpy(&g_server_clients[0].private_data, now_priv, sizeof(Private_c));
    }
#endif

    s_player_registry_initialized = 1;
#ifdef TARGET_SERVER
    printf("[SERVER] Player registry initialized (Capacity: %d clients). All slots open.\n",
           MAX_SERVER_CLIENTS);
#else
    printf("[SERVER] Player registry initialized (Capacity: %d clients). Slot 0 reserved for Local Player.\n",
           MAX_SERVER_CLIENTS);
#endif
}

ConnectedClient* server_player_get(uint32_t client_id) {
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server_clients[i].is_connected && g_server_clients[i].client_id == client_id) {
            return &g_server_clients[i];
        }
    }
    return NULL;
}

ConnectedClient* server_player_get_by_actor(PLAYER_ACTOR* actor) {
    if (actor == NULL) {
        return NULL;
    }
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server_clients[i].is_connected && g_server_clients[i].player_actor == actor) {
            return &g_server_clients[i];
        }
    }
    return NULL;
}

ConnectedClient* server_player_get_by_slot(int slot) {
    if (slot >= 0 && slot < MAX_SERVER_CLIENTS) {
        return &g_server_clients[slot];
    }
    return NULL;
}

int server_player_register(uint32_t client_id, uint32_t zone_id, const Private_c* priv_data) {
    if (!s_player_registry_initialized) {
        server_player_init();
    }

    /* Check if already registered */
    ConnectedClient* existing = server_player_get(client_id);
    if (existing != NULL) {
        existing->zone_id = zone_id;
        if (priv_data != NULL) {
            memcpy(&existing->private_data, priv_data, sizeof(Private_c));
        }
        return (int)(existing - g_server_clients);
    }

    /* Find next free slot */
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (!g_server_clients[i].is_connected) {
            ConnectedClient* client = &g_server_clients[i];
            memset(client, 0, sizeof(ConnectedClient));
            client->client_id = client_id;
            client->is_connected = 1;
            client->zone_id = zone_id;
            client->last_ack_input_seq = 0;

            if (priv_data != NULL) {
                memcpy(&client->private_data, priv_data, sizeof(Private_c));
            } else {
                /* Initialize default private data */
                mPr_InitPrivateInfo(&client->private_data);
                client->private_data.inventory.wallet = 1000;
                client->private_data.cloth.idx = 0;
                client->private_data.cloth.item = 0x2400; /* Default civilian clothes */
            }

            printf("[SERVER] Registered Client ID %u in slot %d (Zone %u).\n",
                   client_id, i, zone_id);
            ServerZone* zone = server_zone_get(zone_id);
            if (zone != NULL) {
                server_zone_add_player(zone, i);
            }
            return i;
        }
    }

    fprintf(stderr, "[SERVER WARNING] Player registry full! Cannot register Client ID %u.\n", client_id);
    return -1;
}

void server_player_unregister(uint32_t client_id) {
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server_clients[i].is_connected && g_server_clients[i].client_id == client_id) {
            ConnectedClient* client = &g_server_clients[i];
            ServerZone* zone = server_zone_get(client->zone_id);
            if (zone != NULL) {
                server_zone_remove_player(zone, i);
            }
            if (client->player_actor != NULL) {
                Actor_delete((ACTOR*)client->player_actor);
                client->player_actor = NULL;
            }

            /* Persist latest player state back to extended storage */
            bool is_resident = false;
            int idx = server_storage_find_player(client->auth_token, &is_resident);
            if (idx >= 0) {
                if (is_resident) {
                    ServerResident* r = server_storage_get_resident(idx);
                    if (r != NULL) {
                        r->private_data = client->private_data;
                        r->last_seen_time = (uint64_t)time(NULL);
                    }
                } else {
                    ServerVisitor* v = server_storage_get_visitor(idx);
                    if (v != NULL) {
                        v->private_data = client->private_data;
                        v->last_seen_time = (uint64_t)time(NULL);
                    }
                }
                server_storage_save();
            }

            client->is_connected = 0;
            printf("[SERVER] Unregistered Client ID %u from slot %d.\n", client_id, i);
            return;
        }
    }
}

int server_player_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server_clients[i].is_connected) {
            count++;
        }
    }
    return count;
}

PLAYER_ACTOR* server_player_spawn_actor(GAME_PLAY* play, int slot, xyz_t pos, s_xyz angle) {
    if (play == NULL || slot < 0 || slot >= MAX_SERVER_CLIENTS) {
        return NULL;
    }

    ConnectedClient* client = &g_server_clients[slot];
    if (!client->is_connected) {
        return NULL;
    }

    /* Snap Y position to terrain surface if valid */
    f32 ground_y = mCoBG_GetBgY_OnlyCenter_FromWpos2(pos, 0.0f);
    if (ground_y > -1000.0f && ground_y < 10000.0f) {
        pos.y = ground_y;
    }

    ACTOR* actor = Actor_info_make_actor(
        &play->actor_info,
        (GAME*)play,
        mAc_PROFILE_PLAYER,
        pos.x, pos.y, pos.z,
        angle.x, angle.y, angle.z,
        -1, -1, -1,
        EMPTY_NO,
        slot, /* arg = slot index */
        -1, -1
    );

    if (actor != NULL) {
        PLAYER_ACTOR* p_actor = (PLAYER_ACTOR*)actor;
        p_actor->player_no = slot;
        p_actor->client_id = client->client_id;
        p_actor->is_dummy = client->is_dummy;

        /* Ensure dummy/secondary players are never culled */
        actor->cull_distance = 10000.0f;
        actor->state_bitfield |= (ACTOR_STATE_NO_CULL | ACTOR_STATE_NO_MOVE_WHILE_CULLED | ACTOR_STATE_NO_DRAW_WHILE_CULLED);

        client->player_actor = p_actor;
        client->predicted_pos = pos;
        client->predicted_angle = angle;

        FILE* flog = fopen("server_player.log", "a");
        if (flog != NULL) {
            fprintf(flog, "[SPAWN ACTOR OK] Client ID %u (Slot %d, is_dummy=%d) at (%.1f, %.1f, %.1f), actor_p=%p\n",
                    client->client_id, slot, client->is_dummy, pos.x, pos.y, pos.z, (void*)actor);
            fclose(flog);
        }

        printf("[SERVER] Spawned PLAYER_ACTOR for Client ID %u (Slot %d, is_dummy=%d) at (%.1f, %.1f, %.1f).\n",
               client->client_id, slot, client->is_dummy, pos.x, pos.y, pos.z);
        return p_actor;
    }

    FILE* flog = fopen("server_player.log", "a");
    if (flog != NULL) {
        fprintf(flog, "[SPAWN ACTOR FAIL] Actor_info_make_actor returned NULL for Client ID %u slot %d (total_actors=%d)\n",
                client->client_id, slot, play->actor_info.total_num);
        fclose(flog);
    }
    fprintf(stderr, "[SERVER ERROR] Failed to spawn PLAYER_ACTOR for Client ID %u in slot %d.\n",
            client->client_id, slot);
    return NULL;
}

int server_spawn_dummy_player(GAME_PLAY* play, const char* name, xyz_t pos) {
    if (play == NULL) {
        return -1;
    }

    /* Register a simulated client in slot 1 (Client ID 2) */
    uint32_t dummy_id = 2;
    int slot = server_player_register(dummy_id, 0, NULL);
    if (slot < 0) {
        return -1;
    }

    ConnectedClient* client = &g_server_clients[slot];
    client->is_dummy = 1;

    /* Set dummy player identity */
    static const u8 default_dummy_name[PLAYER_NAME_LEN] = { 'D', 'u', 'm', 'm', 'y', CHAR_SPACE, CHAR_SPACE, CHAR_SPACE };
    u8 dummy_name_buf[PLAYER_NAME_LEN];
    const char* log_name = "Dummy";
    if (name != NULL && name[0] != '\0') {
        log_name = name;
        int len = strlen(name);
        for (int ci = 0; ci < PLAYER_NAME_LEN; ci++) {
            dummy_name_buf[ci] = (ci < len) ? (u8)name[ci] : CHAR_SPACE;
        }
        mPr_CopyPlayerName(client->private_data.player_ID.player_name, dummy_name_buf);
    } else {
        mPr_CopyPlayerName(client->private_data.player_ID.player_name, (u8*)default_dummy_name);
    }

    mLd_CopyLandName(client->private_data.player_ID.land_name, Save_Get(land_info).name);
    client->private_data.player_ID.player_id = 0x55AA;
    client->private_data.player_ID.land_id = Save_Get(land_info).id;
    client->private_data.gender = 0; /* Boy */
    client->private_data.face = 0;
    client->private_data.cloth.idx = 0;
    client->private_data.cloth.item = 0x2400; /* Civilian shirt */
    client->private_data.inventory.wallet = 1000;

    /* Position dummy player nearby Player 1 if pos is zero */
    if (pos.x == 0.0f && pos.z == 0.0f) {
        PLAYER_ACTOR* p1 = get_player_actor_withoutCheck(play);
        if (p1 != NULL && !p1->is_dummy && (fabsf(p1->actor_class.world.position.x) > 1.0f || fabsf(p1->actor_class.world.position.z) > 1.0f)) {
            pos.x = p1->actor_class.world.position.x;
            pos.y = p1->actor_class.world.position.y;
            pos.z = p1->actor_class.world.position.z - 80.0f;
        } else {
            pos.x = 800.0f;
            pos.y = 0.0f;
            pos.z = 800.0f;
        }
    }

    /* Snap ground Y */
    f32 ground_y = mCoBG_GetBgY_OnlyCenter_FromWpos2(pos, 0.0f);
    if (ground_y > -1000.0f && ground_y < 10000.0f) {
        pos.y = ground_y;
    }

    if (client->player_actor == NULL) {
        for (ACTOR* a = play->actor_info.list[ACTOR_PART_PLAYER].actor; a != NULL; a = a->next_actor) {
            PLAYER_ACTOR* pa = (PLAYER_ACTOR*)a;
            if (pa->is_dummy || pa->player_no == slot) {
                client->player_actor = pa;
                break;
            }
        }
    }

    /* If dummy actor already exists in the scene, teleport it */
    if (client->player_actor != NULL) {
        int exists = 0;
        for (ACTOR* a = play->actor_info.list[ACTOR_PART_PLAYER].actor; a != NULL; a = a->next_actor) {
            if (a == (ACTOR*)client->player_actor) {
                exists = 1;
                break;
            }
        }
        if (exists) {
            PLAYER_ACTOR* pa = client->player_actor;
            pa->actor_class.world.position = pos;
            pa->actor_class.last_world_position = pos;
            pa->actor_class.eye.position = pos;
            pa->actor_class.position_speed = (xyz_t){0.0f, 0.0f, 0.0f};
            pa->actor_class.speed = 0.0f;
            pa->actor_class.shape_info.rotation.y = 0;
            pa->actor_class.world.angle.y = 0;
            int bx = 0, bz = 0;
            mFI_Wpos2BlockNum(&bx, &bz, pos);
            pa->actor_class.block_x = (s8)bx;
            pa->actor_class.block_z = (s8)bz;

            client->predicted_pos = pos;
            client->predicted_angle = (s_xyz){0, 0, 0};

            printf("[SERVER] Teleported existing Dummy Player (Slot %d) to (%.1f, %.1f, %.1f).\n",
                   slot, pos.x, pos.y, pos.z);
            return slot;
        } else {
            client->player_actor = NULL;
        }
    }

    s_xyz angle = {0, 0, 0};
    PLAYER_ACTOR* spawned = server_player_spawn_actor(play, slot, pos, angle);
    if (spawned != NULL) {
        printf("[SERVER] Concurrent Player Simulation Active:\n"
               "  -> Player 1 (Host, Client ID %u, Slot 0) is tracked in memory.\n"
               "  -> Player 2 (Dummy '%s', Client ID %u, Slot %d) allocated, tracked, and simulated concurrently in memory.\n",
               g_server_clients[0].client_id, log_name, dummy_id, slot);
        return slot;
    }

    return -1;
}

void server_player_tick(GAME_PLAY* play) {
    if (play == NULL) {
        return;
    }

    /* Do not tick or spawn players during title screen demo or menu/selection scenes */
    if (mEv_CheckTitleDemo() != mEv_TITLEDEMO_NONE) {
        return;
    }

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

    if (!s_player_registry_initialized) {
        server_player_init();
    }

    /* Process incoming network events and packets */
    server_network_poll();

    /* Tick server storage autosave debounce & periodic backups */
    server_storage_tick();

    /* Reset actor pointers if scene or play context changed */
    static GAME_PLAY* s_last_play = NULL;
    static int s_last_scene_id = -1;
    if (play != s_last_play || play->scene_id != s_last_scene_id) {
        s_last_play = play;
        s_last_scene_id = play->scene_id;
        for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
            g_server_clients[i].player_actor = NULL;
        }
    }

    /* Verify tracked dummy/secondary players are still present in actor_info */
    for (int i = 1; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server_clients[i].player_actor != NULL) {
            int exists = 0;
            for (ACTOR* a = play->actor_info.list[ACTOR_PART_PLAYER].actor; a != NULL; a = a->next_actor) {
                if (a == (ACTOR*)g_server_clients[i].player_actor) {
                    exists = 1;
                    break;
                }
            }
            if (!exists) {
                g_server_clients[i].player_actor = NULL;
            }
        }
    }

#ifndef TARGET_SERVER
    /* Bind Player 1 (Local Player) actor on client */
    PLAYER_ACTOR* host_actor = get_player_actor_withoutCheck(play);
    if (host_actor != NULL && !host_actor->is_dummy) {
        g_server_clients[0].player_actor = host_actor;
        host_actor->player_no = 0;
        host_actor->client_id = 1;
        host_actor->is_dummy = 0;
    }
#endif

    /* Check if dummy player spawn was requested */
    int should_spawn = 0;
#ifdef TARGET_SERVER
    if (g_server_config.spawn_dummy_player) {
        should_spawn = 1;
    }
#else
    extern int g_pc_spawn_dummy_player;
    if (g_pc_spawn_dummy_player) {
        should_spawn = 1;
    }
#endif

    /* Diagnostic log to server_player.log once per unique state */
    static int s_logged_scene = -1;
    static int s_log_counter = 0;
    int cur_field_type = Common_Get(field_type);
    if (should_spawn && (play->scene_id != s_logged_scene || (s_log_counter++ % 300 == 0))) {
        s_logged_scene = play->scene_id;
        FILE* flog = fopen("server_player.log", "a");
        if (flog != NULL) {
            fprintf(flog, "[TICK] frame=%u, scene_id=%d, field_type=%d, host_actor=%p, dummy_actor=%p\n",
                    (unsigned int)play->game_frame, play->scene_id, cur_field_type,
                    (void*)g_server_clients[0].player_actor, (void*)g_server_clients[1].player_actor);
            fclose(flog);
        }
    }

    /* Spawn outdoors in town: play->scene_id == SCENE_FG and field_type == mFI_FIELDTYPE2_FG */
    int is_outdoors = (play->scene_id == SCENE_FG && cur_field_type == mFI_FIELDTYPE2_FG);
    if (should_spawn && is_outdoors && g_server_clients[0].player_actor != NULL) {
        ConnectedClient* dummy_client = &g_server_clients[1];
        if (dummy_client->player_actor == NULL) {
            for (ACTOR* a = play->actor_info.list[ACTOR_PART_PLAYER].actor; a != NULL; a = a->next_actor) {
                PLAYER_ACTOR* pa = (PLAYER_ACTOR*)a;
                if (pa->is_dummy || pa->player_no == 1) {
                    dummy_client->player_actor = pa;
                    dummy_client->is_connected = 1;
                    dummy_client->is_dummy = 1;
                    pa->player_no = 1;
                    pa->client_id = 2;
                    pa->is_dummy = 1;
                    a->cull_distance = 10000.0f;
                    a->state_bitfield |= (ACTOR_STATE_NO_CULL | ACTOR_STATE_NO_MOVE_WHILE_CULLED | ACTOR_STATE_NO_DRAW_WHILE_CULLED);
                    break;
                }
            }
        }
        if (!dummy_client->is_connected || dummy_client->player_actor == NULL) {
            xyz_t dummy_pos = {0.0f, 0.0f, 0.0f};
            FILE* flog = fopen("server_player.log", "a");
            if (flog != NULL) {
                fprintf(flog, "[SPAWN] Calling server_spawn_dummy_player in scene_id=%d field_type=%d...\n",
                        play->scene_id, cur_field_type);
                fclose(flog);
            }
            int slot = server_spawn_dummy_player(play, "Dummy", dummy_pos);
            flog = fopen("server_player.log", "a");
            if (flog != NULL) {
                fprintf(flog, "[SPAWN RESULT] slot=%d, dummy_actor=%p\n",
                        slot, (void*)dummy_client->player_actor);
                fclose(flog);
            }
        }
    }

    /* Update predicted position and angle for each active client */
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        ConnectedClient* client = &g_server_clients[i];
        if (client->is_connected && client->player_actor != NULL) {
            client->predicted_pos = client->player_actor->actor_class.world.position;
            client->predicted_angle = client->player_actor->actor_class.shape_info.rotation;
        }
    }
}

Private_c* server_player_get_private_by_actor(PLAYER_ACTOR* actor) {
    if (actor == NULL) {
        return Common_Get(now_private);
    }
    if (actor->player_no > 0 && actor->player_no < MAX_SERVER_CLIENTS) {
        if (g_server_clients[actor->player_no].is_connected) {
            return &g_server_clients[actor->player_no].private_data;
        }
    }
    return Common_Get(now_private);
}

Private_c* server_player_get_private_by_slot(int slot) {
    if (slot > 0 && slot < MAX_SERVER_CLIENTS) {
        if (g_server_clients[slot].is_connected) {
            return &g_server_clients[slot].private_data;
        }
    }
    return Common_Get(now_private);
}

int server_is_dummy_player_alive(void) {
    if (!g_server_clients[1].is_connected) {
        return 0;
    }
    return (g_server_clients[1].player_actor != NULL);
}

int server_spawn_dummy_player_north(void) {
    extern struct game_s* gamePT;
    GAME_PLAY* play = (GAME_PLAY*)gamePT;
    if (play == NULL) {
        return -1;
    }

    if (mEv_CheckTitleDemo() != mEv_TITLEDEMO_NONE) {
        return -1;
    }

    switch (play->scene_id) {
        case SCENE_TITLE_DEMO:
        case SCENE_START_DEMO:
        case SCENE_START_DEMO2:
        case SCENE_START_DEMO3:
        case SCENE_PLAYERSELECT:
        case SCENE_PLAYERSELECT_2:
        case SCENE_PLAYERSELECT_3:
        case SCENE_PLAYERSELECT_SAVE:
            return -1;
        default:
            break;
    }

    PLAYER_ACTOR* p1 = get_player_actor_withoutCheck(play);
    if (p1 == NULL || p1->is_dummy) {
        return -1;
    }

    xyz_t pos = p1->actor_class.world.position;
    pos.z -= 80.0f; /* 80 units North (-Z) */

    f32 ground_y = mCoBG_GetBgY_OnlyCenter_FromWpos2(pos, 0.0f);
    if (ground_y > -1000.0f && ground_y < 10000.0f) {
        pos.y = ground_y;
    }

#ifndef TARGET_SERVER
    extern int g_pc_spawn_dummy_player;
    g_pc_spawn_dummy_player = 1;
#endif

    return server_spawn_dummy_player(play, "Dummy", pos);
}



