#ifndef NET_PACKETS_H
#define NET_PACKETS_H

#include "types.h"
#include "lb_rtc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_PACKET_MAGIC 0x4143 /* 'AC' */
#define NET_PROTOCOL_VERSION 1
#define NET_DEFAULT_PORT 21025

/* Channels */
enum {
    NET_CH_UNRELIABLE = 0, /* Transform and input stream (20-30 Hz) */
    NET_CH_RELIABLE   = 1, /* Handshakes, world mutations, zone changes, dialog */
    NET_CH_COUNT      = 2
};

/* Packet IDs */
enum {
    PKT_INVALID           = 0x00,

    /* Keepalive / Control */
    PKT_PING              = 0x01,
    PKT_PONG              = 0x02,
    PKT_C2S_DISCONNECT    = 0x03,
    PKT_S2C_DISCONNECT    = 0x04,

    /* Handshake, Character Creation & Move-In (Reliable) */
    PKT_C2S_HANDSHAKE          = 0x10,
    PKT_S2C_HANDSHAKE_ACK      = 0x11,
    PKT_C2S_CHARACTER_FINALIZE = 0x12,
    PKT_S2C_CHARACTER_ACK      = 0x13,
    PKT_C2S_MOVEIN_REQ         = 0x14,
    PKT_S2C_MOVEIN_ACK         = 0x15,

    /* Movement & Continuous State (Unreliable) */
    PKT_C2S_INPUT         = 0x20,
    PKT_S2C_ENTITY_SYNC   = 0x21,

    /* World & Gameplay Interactions (Reliable) */
    PKT_C2S_INTERACT_REQ  = 0x30,
    PKT_S2C_FG_DELTA      = 0x31,
    PKT_S2C_ZONE_ENTER    = 0x40,

    /* Dialogue & UI (Reliable) */
    PKT_C2S_DIALOG_INPUT  = 0x50,
    PKT_S2C_DIALOG_NODE   = 0x51,

    PKT_ID_MAX
};

/* Handshake ACK Status */
enum {
    NET_HS_OK = 0,
    NET_HS_OK_NEW_PLAYER,
    NET_HS_VERSION_MISMATCH,
    NET_HS_SERVER_FULL,
    NET_HS_AUTH_FAILED
};

/* Move-In ACK Status */
enum {
    NET_MOVEIN_OK = 0,
    NET_MOVEIN_HOUSE_FULL,
    NET_MOVEIN_ALREADY_RESIDENT,
    NET_MOVEIN_ERROR
};

/* Interaction Actions */
enum {
    NET_INTERACT_NONE = 0,
    NET_INTERACT_DIG,
    NET_INTERACT_FILL_HOLE,
    NET_INTERACT_SHAKE_TREE,
    NET_INTERACT_HIT_ROCK,
    NET_INTERACT_TALK_NPC,
    NET_INTERACT_ENTER_DOOR,
    NET_INTERACT_ZONE_CHANGE,
    NET_INTERACT_PICK_ITEM,
    NET_INTERACT_DROP_ITEM,
    NET_INTERACT_CUT_TREE,
    NET_INTERACT_PULL_WEED
};

/* Maximum constants */
#define NET_MAX_NAME_LEN 16
#define NET_MAX_AUTH_TOKEN_LEN 32
#define NET_MAX_ENTITIES_PER_SYNC 32
#define NET_MAX_ROOM_FURNITURE 64

/* Packet Header (Wire format: 6 bytes) */
typedef struct {
    u16 magic;        /* NET_PACKET_MAGIC */
    u8  packet_id;    /* PKT_* */
    u8  channel;      /* NET_CH_* */
    u16 payload_size; /* Size of packet payload following header */
} net_header_t;

/* Ping / Pong */
typedef struct {
    u32 timestamp_ms;
} pkt_ping_t;

typedef struct {
    u32 client_timestamp_ms;
    u32 server_timestamp_ms;
} pkt_pong_t;

/* Disconnect */
typedef struct {
    u16 client_id;
    u8  reason;
} pkt_disconnect_t;

/* Handshake Client -> Server */
typedef struct {
    u32 client_version;
    u16 protocol_version;
    char player_name[NET_MAX_NAME_LEN];
    char town_name[NET_MAX_NAME_LEN];
    u16 player_id;
    u16 land_id;
    u8  gender;
    u8  face_type;
    u16 clothes_id;
    char auth_token[NET_MAX_AUTH_TOKEN_LEN];
} pkt_c2s_handshake_t;

/* Handshake ACK Server -> Client */
typedef struct {
    u8  status; /* NET_HS_* */
    u16 client_id;
    lbRTC_time_c world_time;
    char town_name[NET_MAX_NAME_LEN];
    u16 land_id;
    u8  weather;
    u16 starting_zone_id;
    u8  is_resident;
    s8  house_idx;
    s8  resident_slot;
    s8  roommate_slot;
} pkt_s2c_handshake_ack_t;

/* Character Finalization Client -> Server (Rover Train Complete) */
typedef struct {
    u16  client_id;
    char player_name[NET_MAX_NAME_LEN];
    u8   gender;
    u8   face_type;
    u16  clothes_id;
    char auth_token[NET_MAX_AUTH_TOKEN_LEN];
} pkt_c2s_character_finalize_t;

/* Character Finalization ACK Server -> Client */
typedef struct {
    u8   status; /* 0 = OK */
    u16  client_id;
    u8   is_resident;
    u8   slot_idx;
} pkt_s2c_character_ack_t;

/* House Move-In Request Client -> Server (Gyroid Interaction) */
typedef struct {
    u16  client_id;
    u8   house_idx; /* 0..3 */
    char auth_token[NET_MAX_AUTH_TOKEN_LEN];
} pkt_c2s_movein_req_t;

/* House Move-In ACK Server -> Client */
typedef struct {
    u8  status; /* NET_MOVEIN_* */
    u16 client_id;
    u8  house_idx;     /* 0..3 */
    u8  resident_slot; /* 0..7 */
    u8  roommate_slot; /* 0 or 1 */
} pkt_s2c_movein_ack_t;

/* Controller Input Client -> Server */
typedef struct {
    u32 sequence;
    u16 client_id;
    s8  stick_x;
    s8  stick_y;
    u32 button_mask;
    u16 current_tool_id;
    u16 anim_id;
    f32 player_x;
    f32 player_y;
    f32 player_z;
    s16 angle_y;
} pkt_c2s_input_t;

/* Entity State for Entity Sync */
typedef struct {
    u32 actor_id;
    u16 zone_id;
    f32 pos_x;
    f32 pos_y;
    f32 pos_z;
    s16 angle_y;
    u16 anim_id;
    u16 anim_frame;
    u16 tool_id;
    u16 state_flags;
} net_entity_state_t;

/* Entity Sync Server -> Client */
typedef struct {
    u32 frame_id;
    u16 count;
    net_entity_state_t entities[NET_MAX_ENTITIES_PER_SYNC];
} pkt_s2c_entity_sync_t;

/* World Interaction Request Client -> Server */
typedef struct {
    u16 client_id;
    u8  action_type; /* NET_INTERACT_* */
    s8  block_x;
    s8  block_z;
    u8  ut_x;
    u8  ut_z;
    f32 world_x;
    f32 world_y;
    f32 world_z;
    u32 target_actor_id;
    u16 item_arg;
} pkt_c2s_interact_req_t;

/* Foreground Delta Server -> Client */
typedef struct {
    u16 zone_id;
    u8  block_x;
    u8  block_z;
    u8  ut_x;
    u8  ut_z;
    u16 old_item;
    u16 new_item;
} pkt_s2c_fg_delta_t;

/* Room Furniture Entry */
typedef struct {
    u16 ftr_id;
    u8  ut_x;
    u8  ut_z;
    u8  rotation;
} net_room_furniture_t;

/* Zone Enter Server -> Client */
typedef struct {
    u16 client_id;
    u16 zone_id;
    s16 scene_id;
    f32 spawn_x;
    f32 spawn_y;
    f32 spawn_z;
    s16 spawn_angle_y;
    u8  has_room_data;
    u8  furniture_count;
    net_room_furniture_t furniture[NET_MAX_ROOM_FURNITURE];
} pkt_s2c_zone_enter_t;

/* Dialogue Input Client -> Server */
typedef struct {
    u16 client_id;
    u32 speaker_actor_id;
    u8  input_action; /* 0 = advance text, 1 = select choice */
    u8  choice_idx;
} pkt_c2s_dialog_input_t;

/* Dialogue Node Server -> Client */
typedef struct {
    u16 client_id;
    u32 speaker_actor_id;
    u16 message_id;
    u8  num_choices;
    u8  options_bitmask;
} pkt_s2c_dialog_node_t;

/* --- Serialization APIs --- */

/* Packet Header pack / unpack */
int net_pack_header(u8* buf, size_t buf_size, const net_header_t* hdr);
int net_unpack_header(const u8* buf, size_t buf_size, net_header_t* hdr);

/* Ping / Pong */
int net_pack_ping(u8* buf, size_t buf_size, const pkt_ping_t* pkt);
int net_unpack_ping(const u8* buf, size_t buf_size, pkt_ping_t* pkt);
int net_pack_pong(u8* buf, size_t buf_size, const pkt_pong_t* pkt);
int net_unpack_pong(const u8* buf, size_t buf_size, pkt_pong_t* pkt);

/* Disconnect */
int net_pack_disconnect(u8* buf, size_t buf_size, u8 packet_id, const pkt_disconnect_t* pkt);
int net_unpack_disconnect(const u8* buf, size_t buf_size, pkt_disconnect_t* pkt);

/* Handshake */
int net_pack_handshake(u8* buf, size_t buf_size, const pkt_c2s_handshake_t* pkt);
int net_unpack_handshake(const u8* buf, size_t buf_size, pkt_c2s_handshake_t* pkt);
int net_pack_handshake_ack(u8* buf, size_t buf_size, const pkt_s2c_handshake_ack_t* pkt);
int net_unpack_handshake_ack(const u8* buf, size_t buf_size, pkt_s2c_handshake_ack_t* pkt);

/* Character Finalization */
int net_pack_character_finalize(u8* buf, size_t buf_size, const pkt_c2s_character_finalize_t* pkt);
int net_unpack_character_finalize(const u8* buf, size_t buf_size, pkt_c2s_character_finalize_t* pkt);
int net_pack_character_ack(u8* buf, size_t buf_size, const pkt_s2c_character_ack_t* pkt);
int net_unpack_character_ack(const u8* buf, size_t buf_size, pkt_s2c_character_ack_t* pkt);

/* House Move-In */
int net_pack_movein_req(u8* buf, size_t buf_size, const pkt_c2s_movein_req_t* pkt);
int net_unpack_movein_req(const u8* buf, size_t buf_size, pkt_c2s_movein_req_t* pkt);
int net_pack_movein_ack(u8* buf, size_t buf_size, const pkt_s2c_movein_ack_t* pkt);
int net_unpack_movein_ack(const u8* buf, size_t buf_size, pkt_s2c_movein_ack_t* pkt);

/* Input */
int net_pack_input(u8* buf, size_t buf_size, const pkt_c2s_input_t* pkt);
int net_unpack_input(const u8* buf, size_t buf_size, pkt_c2s_input_t* pkt);

/* Entity Sync */
int net_pack_entity_sync(u8* buf, size_t buf_size, const pkt_s2c_entity_sync_t* pkt);
int net_unpack_entity_sync(const u8* buf, size_t buf_size, pkt_s2c_entity_sync_t* pkt);

/* Interaction */
int net_pack_interact_req(u8* buf, size_t buf_size, const pkt_c2s_interact_req_t* pkt);
int net_unpack_interact_req(const u8* buf, size_t buf_size, pkt_c2s_interact_req_t* pkt);

/* FG Delta */
int net_pack_fg_delta(u8* buf, size_t buf_size, const pkt_s2c_fg_delta_t* pkt);
int net_unpack_fg_delta(const u8* buf, size_t buf_size, pkt_s2c_fg_delta_t* pkt);

/* Zone Enter */
int net_pack_zone_enter(u8* buf, size_t buf_size, const pkt_s2c_zone_enter_t* pkt);
int net_unpack_zone_enter(const u8* buf, size_t buf_size, pkt_s2c_zone_enter_t* pkt);

/* Dialogue */
int net_pack_dialog_input(u8* buf, size_t buf_size, const pkt_c2s_dialog_input_t* pkt);
int net_unpack_dialog_input(const u8* buf, size_t buf_size, pkt_c2s_dialog_input_t* pkt);
int net_pack_dialog_node(u8* buf, size_t buf_size, const pkt_s2c_dialog_node_t* pkt);
int net_unpack_dialog_node(const u8* buf, size_t buf_size, pkt_s2c_dialog_node_t* pkt);

/* Self-test suite */
int net_packer_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_PACKETS_H */
