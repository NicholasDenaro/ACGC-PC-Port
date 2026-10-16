/* server_zone.c - Multi-Zone Simulation Engine (Outdoor + Rooms) */
#include "server/server_zone.h"
#include "server/server_player.h"
#include "m_play.h"
#include "m_actor.h"
#include "m_collision_obj.h"
#include "m_field_info.h"
#include "m_scene.h"
#include "m_scene_table.h"
#include "m_common_data.h"
#include "m_event.h"
#include "dolphin/os.h"
#include "m_player_lib.h"
#include <stdio.h>
#include <string.h>

ServerZone g_server_zones[MAX_SERVER_ZONES];
static ServerZone* s_current_zone = NULL;

static Scene_Word_u* server_zone_get_scene_data(int scene_id) {
    static Scene_Word_u* scene_word_data[SCENE_NUM] = {
        test01_info,
        test02_info,
        test03_info,
        water_test_info,
        test_step01_info,
        test04_info,
        npc_room01_info,
        test_fd_npc_land_info,
        field_tool_field_info,
        shop01_info,
        BG_TEST01_info,
        BG_TEST01_XLU_info,
        broker_shop_info,
        fg_tool_in_info,
        post_office_info,
        start_demo1_info,
        start_demo2_info,
        police_box_info,
        buggy_info,
        player_select_info,
        player_room_s_info,
        player_room_m_info,
        player_room_l_info,
        shop02_info,
        shop03_info,
        shop04_1f_info,
        test05_info,
        PLAYER_SELECT2_info,
        PLAYER_SELECT3_info,
        shop04_2f_info,
        event_notification_info,
        kamakura_info,
        field_tool_field_info,
        title_demo_info,
        PLAYER_SELECT4_info,
        museum_entrance_info,
        museum_picture_info,
        museum_fossil_info,
        museum_insect_info,
        museum_fish_info,
        player_room_ll1_info,
        player_room_ll2_info,
        p_room_bm_s_info,
        p_room_bm_m_info,
        p_room_bm_l_info,
        p_room_bm_ll1_info,
        NEEDLEWORK_info,
        player_room_island_info,
        npc_room_island_info,
        start_demo3_info,
        lighthouse_info,
        tent_info,
    };

    if (scene_id >= 0 && scene_id < SCENE_NUM) {
        return scene_word_data[scene_id];
    }
    return NULL;
}

void server_zone_init(GAME_PLAY* play) {
    if (play != NULL && mEv_IsTitleDemo()) {
        return;
    }

    static int s_initialized = 0;
    if (s_initialized) {
        return;
    }
    s_initialized = 1;

    memset(g_server_zones, 0, sizeof(g_server_zones));

    /* Initialize Zone 0 (Outdoor Town) */
    ServerZone* outdoor = &g_server_zones[ZONE_ID_OUTDOOR_TOWN];
    outdoor->zone_id = ZONE_ID_OUTDOOR_TOWN;
    outdoor->type = ZONE_TYPE_OUTDOOR_TOWN;
    outdoor->scene_id = (play != NULL) ? play->scene_id : SCENE_FG;
    outdoor->room_index = -1;
    outdoor->is_active = TRUE;
    outdoor->is_persistent = TRUE;
    outdoor->player_occupant_mask = 0;
    outdoor->player_count = 0;

    if (play != NULL) {
        outdoor->actor_info = play->actor_info;
        outdoor->collision_check = play->collision_check;
        outdoor->block_table = play->block_table;
        outdoor->scene_data = play->current_scene_data;
        outdoor->field_info = g_fdinfo;
        outdoor->clip = Common_Get(clip);
        outdoor->field_type = Common_Get(field_type);
    }

    s_current_zone = outdoor;

    OSReport("[SERVER ZONE] Initialized Zone 0 (Outdoor Town, Scene %d, persistent)\n",
             outdoor->scene_id);
}

ServerZone* server_zone_get(uint32_t zone_id) {
    if (zone_id < MAX_SERVER_ZONES) {
        return &g_server_zones[zone_id];
    }
    return NULL;
}

ServerZone* server_zone_get_by_scene(int scene_id, int room_index) {
    for (int i = 0; i < MAX_SERVER_ZONES; i++) {
        ServerZone* zone = &g_server_zones[i];
        if (zone->is_active && zone->scene_id == scene_id) {
            if (room_index < 0 || zone->room_index == room_index) {
                return zone;
            }
        }
    }
    return NULL;
}

ServerZone* server_zone_find_or_create(GAME_PLAY* play, int scene_id, ZoneType type, int room_index) {
    if (type == ZONE_TYPE_OTHER) {
        return NULL;
    }
    if (scene_id == SCENE_FG) {
        return &g_server_zones[ZONE_ID_OUTDOOR_TOWN];
    }

    /* Check if already active */
    ServerZone* existing = server_zone_get_by_scene(scene_id, room_index);
    if (existing != NULL) {
        return existing;
    }

    /* Find free slot from 1 to MAX_SERVER_ZONES-1 */
    for (int i = 1; i < MAX_SERVER_ZONES; i++) {
        ServerZone* zone = &g_server_zones[i];
        if (!zone->is_active) {
            memset(zone, 0, sizeof(ServerZone));
            zone->zone_id = i;
            zone->type = type;
            zone->scene_id = scene_id;
            zone->room_index = room_index;
            zone->is_active = TRUE;
            zone->is_persistent = FALSE;
            zone->scene_data = server_zone_get_scene_data(scene_id);

            OSReport("[SERVER ZONE] Instantiated Zone %u (type=%d, scene=%d, room=%d)\n",
                     zone->zone_id, (int)zone->type, zone->scene_id, zone->room_index);
            return zone;
        }
    }

    OSReport("[SERVER ZONE ERROR] Maximum zones (%d) reached! Cannot instantiate scene=%d\n",
             MAX_SERVER_ZONES, scene_id);
    return NULL;
}

int server_zone_activate(ServerZone* zone, GAME_PLAY* play) {
    (void)play;
    if (zone == NULL) return FALSE;
    zone->is_active = TRUE;
    return TRUE;
}

int server_zone_deactivate(ServerZone* zone, GAME_PLAY* play) {
    if (zone == NULL || zone->is_persistent) return FALSE;

    /* Clean up actors in the zone if any remain */
    if (play != NULL && zone->actor_info.total_num > 0) {
        Actor_info_dt(&zone->actor_info, play);
    }

    memset(&zone->actor_info, 0, sizeof(Actor_info));
    memset(&zone->collision_check, 0, sizeof(CollisionCheck_c));
    zone->field_info = NULL;
    zone->scene_data = NULL;
    zone->is_active = FALSE;
    zone->player_occupant_mask = 0;
    zone->player_count = 0;
    if (s_current_zone == zone) {
        s_current_zone = NULL;
    }
    OSReport("[SERVER ZONE] Zone %u (scene=%d, room=%d) deactivated\n",
             zone->zone_id, zone->scene_id, zone->room_index);
    return TRUE;
}

void server_zone_add_player(ServerZone* zone, int client_slot) {
    if (zone == NULL || client_slot < 0 || client_slot >= 32) return;

    if (!(zone->player_occupant_mask & (1u << client_slot))) {
        zone->player_occupant_mask |= (1u << client_slot);
        zone->player_count++;
    }
}

void server_zone_remove_player(ServerZone* zone, int client_slot) {
    if (zone == NULL || client_slot < 0 || client_slot >= 32) return;

    if (zone->player_occupant_mask & (1u << client_slot)) {
        zone->player_occupant_mask &= ~(1u << client_slot);
        if (zone->player_count > 0) {
            zone->player_count--;
        }
        if (zone->player_count == 0 && !zone->is_persistent) {
            OSReport("[SERVER ZONE] Zone %u is now empty (entering dormancy)\n", zone->zone_id);
            server_zone_deactivate(zone, (GAME_PLAY*)gamePT);
        }
    }
}

void server_set_active_zone_context(GAME_PLAY* play, ServerZone* zone) {
    if (zone == NULL || play == NULL) return;
    if (s_current_zone == zone) return;

    /* Sync active state back to current zone before switching */
    if (s_current_zone != NULL) {
        s_current_zone->actor_info = play->actor_info;
        s_current_zone->collision_check = play->collision_check;
        s_current_zone->block_table = play->block_table;
        s_current_zone->scene_id = play->scene_id;
        s_current_zone->field_info = g_fdinfo;
        s_current_zone->clip = Common_Get(clip);
        s_current_zone->field_type = Common_Get(field_type);
    }

    /* Switch to the new zone context */
    s_current_zone = zone;
    play->actor_info = zone->actor_info;
    play->collision_check = zone->collision_check;
    play->block_table = zone->block_table;
    play->scene_id = zone->scene_id;
    if (zone->scene_data != NULL) {
        play->current_scene_data = zone->scene_data;
    }
    if (zone->field_info != NULL) {
        g_fdinfo = zone->field_info;
    }
    Common_Get(clip) = zone->clip;
    Common_Set(field_type, zone->field_type);
    Save_Set(scene_no, zone->scene_id);
    if (get_player_actor_withoutCheck(play) != NULL) {
        Common_Set(player_actor_exists, TRUE);
    }
}

void server_restore_zone_context(GAME_PLAY* play) {
    if (play == NULL) return;
    server_set_active_zone_context(play, &g_server_zones[ZONE_ID_OUTDOOR_TOWN]);
}

ServerZone* server_zone_get_current(void) {
    return s_current_zone;
}

void server_zone_reset_current_context(void) {
    s_current_zone = NULL;
}

static int s_in_background_tick = 0;

int server_zone_is_background(void) {
    return s_in_background_tick;
}

void server_tick_zones(GAME_PLAY* play) {
    if (play == NULL) return;
    if (play->game.exec == NULL) return; /* Transitioning scenes - do not tick background zones */
    if (mEv_IsTitleDemo()) return;
    if (Save_Get(scene_no) != SCENE_FG && server_zone_type_from_scene(Save_Get(scene_no)) == ZONE_TYPE_OTHER) return;

    static int s_tick_log_counter = 0;
    s_tick_log_counter++;

    ServerZone* original_zone = s_current_zone;
    if (original_zone == NULL) {
        original_zone = server_zone_get_by_scene(play->scene_id, -1);
        if (original_zone == NULL) {
            return;
        }
    }

    int saved_scene_no = Save_Get(scene_no);
    u8 saved_field_type = Common_Get(field_type);
    Clip_c saved_clip = Common_Get(clip);

    s_in_background_tick = 1;

    /* Tick all active zones that are NOT currently the primary active zone */
    for (int i = 0; i < MAX_SERVER_ZONES; i++) {
        ServerZone* zone = &g_server_zones[i];
        if (zone->is_active && zone != original_zone && zone->actor_info.total_num > 0) {
            FILE* flog = fopen("server_player.log", "a");
            if (flog) {
                fprintf(flog, "[STZ] Ticking background Zone %u (scene %d, %d actors)...\n",
                        zone->zone_id, zone->scene_id, zone->actor_info.total_num);
                fclose(flog);
            }
            server_set_active_zone_context(play, zone);
            CollisionCheck_clear((GAME*)play, &play->collision_check);
            Actor_info_call_actor(play, &play->actor_info);
            CollisionCheck_OC((GAME*)play, &play->collision_check);
            flog = fopen("server_player.log", "a");
            if (flog) {
                fprintf(flog, "[STZ] Finished ticking background Zone %u\n", zone->zone_id);
                fclose(flog);
            }

            if ((s_tick_log_counter % 180) == 0) {
                int actor_count = 0;
                for (int p = 0; p < ACTOR_PART_NUM; p++) {
                    actor_count += zone->actor_info.list[p].num_actors;
                }
                OSReport("[SERVER ZONE TICK] Concurrent background tick: Zone %u (scene %d, %d actors)\n",
                         zone->zone_id, zone->scene_id, actor_count);
            }
        }
    }

    if (original_zone != NULL) {
        server_set_active_zone_context(play, original_zone);
    }

    Common_Get(clip) = saved_clip;
    Common_Set(field_type, saved_field_type);
    Save_Set(scene_no, saved_scene_no);

    s_in_background_tick = 0;
}

ZoneType server_zone_type_from_scene(int scene_id) {
    switch (scene_id) {
        case SCENE_FG:
            return ZONE_TYPE_OUTDOOR_TOWN;
        case SCENE_SHOP0:
        case SCENE_CONVENI:
        case SCENE_SUPER:
        case SCENE_DEPART:
        case SCENE_DEPART_2:
        case SCENE_BROKER_SHOP:
            return ZONE_TYPE_SHOP;
        case SCENE_MUSEUM_ENTRANCE:
        case SCENE_MUSEUM_ROOM_PAINTING:
        case SCENE_MUSEUM_ROOM_FOSSIL:
        case SCENE_MUSEUM_ROOM_INSECT:
        case SCENE_MUSEUM_ROOM_FISH:
            return ZONE_TYPE_MUSEUM;
        case SCENE_POST_OFFICE:
            return ZONE_TYPE_POST_OFFICE;
        case SCENE_POLICE_BOX:
            return ZONE_TYPE_POLICE_BOX;
        case SCENE_NPC_HOUSE:
        case SCENE_COTTAGE_NPC:
            return ZONE_TYPE_VILLAGER_ROOM;
        case SCENE_MY_ROOM_S:
        case SCENE_MY_ROOM_M:
        case SCENE_MY_ROOM_L:
        case SCENE_MY_ROOM_LL1:
        case SCENE_MY_ROOM_LL2:
        case SCENE_MY_ROOM_BASEMENT_S:
        case SCENE_MY_ROOM_BASEMENT_M:
        case SCENE_MY_ROOM_BASEMENT_L:
        case SCENE_MY_ROOM_BASEMENT_LL1:
        case SCENE_COTTAGE_MY:
            return ZONE_TYPE_PLAYER_ROOM;
        default:
            return ZONE_TYPE_OTHER;
    }
}

int server_zone_active_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_SERVER_ZONES; i++) {
        if (g_server_zones[i].is_active) {
            count++;
        }
    }
    return count;
}

void server_zone_print_status(void) {
    int active_count = 0;
    OSReport("=== SERVER ZONE STATUS ===\n");
    for (int i = 0; i < MAX_SERVER_ZONES; i++) {
        ServerZone* zone = &g_server_zones[i];
        if (zone->is_active) {
            active_count++;
            int actor_count = 0;
            for (int p = 0; p < ACTOR_PART_NUM; p++) {
                actor_count += zone->actor_info.list[p].num_actors;
            }
            OSReport("  Zone %2u: type=%d, scene=%2d, room=%2d, players=%d (mask=0x%02x), actors=%d, persistent=%d\n",
                     zone->zone_id, (int)zone->type, zone->scene_id, zone->room_index,
                     zone->player_count, zone->player_occupant_mask, actor_count, zone->is_persistent);
        }
    }
    OSReport("Total Active Zones: %d / %d\n", active_count, MAX_SERVER_ZONES);
    OSReport("==========================\n");
}
