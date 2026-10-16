/* server_zone.h - Multi-Zone Simulation Engine (Outdoor + Rooms) */
#ifndef SERVER_ZONE_H
#define SERVER_ZONE_H

#include "types.h"
#include "m_play.h"
#include "m_actor.h"
#include "m_collision_obj.h"
#include "m_field_info.h"
#include "m_scene.h"
#include "m_clip.h"
#include <stdint.h>
#include <stdbool.h>

struct field_info_s;

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SERVER_ZONES 32
#define ZONE_ID_OUTDOOR_TOWN 0

typedef enum {
    ZONE_TYPE_OUTDOOR_TOWN = 0,
    ZONE_TYPE_SHOP,             /* Nook's cranny, convenience, supermarket, department */
    ZONE_TYPE_MUSEUM,           /* Entrance, paintings, fossils, insects, fish */
    ZONE_TYPE_POST_OFFICE,
    ZONE_TYPE_POLICE_BOX,
    ZONE_TYPE_PLAYER_ROOM,      /* Player houses: main room, upstairs, basement */
    ZONE_TYPE_VILLAGER_ROOM,    /* Villager houses */
    ZONE_TYPE_ISLAND,
    ZONE_TYPE_OTHER
} ZoneType;

typedef struct ServerZone_s {
    uint32_t zone_id;               /* 0 for outdoor town, >0 for dynamic rooms */
    ZoneType type;
    int scene_id;                   /* e.g. SCENE_FG, SCENE_SHOP0, SCENE_POST_OFFICE, etc. */
    int room_index;                 /* House/room identifier (player house slot 0..3, villager slot 0..14, etc.) */
    int is_active;                  /* TRUE if currently simulated */
    int is_persistent;              /* TRUE for Zone 0 (never freed or deactivated) */
    uint32_t player_occupant_mask;  /* Bitmask of connected client slots in this zone (1 << slot) */
    int player_count;               /* Number of players currently in this zone */

    /* Zone Simulation State */
    Actor_info actor_info;
    CollisionCheck_c collision_check;
    mFI_block_tbl_c block_table;
    Scene_Word_u* scene_data;
    struct field_info_s* field_info;
    Clip_c clip;
    u8 field_type;

    /* Indoor specific data */
    mActor_name_t furniture_layout[16][16];
    struct ServerZone_s* next_zone;
} ServerZone;

extern ServerZone g_server_zones[MAX_SERVER_ZONES];

/* Management API */
void server_zone_init(GAME_PLAY* play);
ServerZone* server_zone_get(uint32_t zone_id);
ServerZone* server_zone_get_by_scene(int scene_id, int room_index);
ServerZone* server_zone_find_or_create(GAME_PLAY* play, int scene_id, ZoneType type, int room_index);
int server_zone_activate(ServerZone* zone, GAME_PLAY* play);
int server_zone_deactivate(ServerZone* zone, GAME_PLAY* play);
void server_zone_add_player(ServerZone* zone, int client_slot);
void server_zone_remove_player(ServerZone* zone, int client_slot);
void server_set_active_zone_context(GAME_PLAY* play, ServerZone* zone);
void server_restore_zone_context(GAME_PLAY* play);
ServerZone* server_zone_get_current(void);
void server_zone_reset_current_context(void);
int server_zone_is_background(void);
void server_tick_zones(GAME_PLAY* play);
int server_zone_active_count(void);
ZoneType server_zone_type_from_scene(int scene_id);
void server_zone_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_ZONE_H */
