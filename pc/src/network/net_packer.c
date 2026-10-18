/* net_packer.c - Compact binary packet serialization & deserialization subsystem
 *
 * Implements deterministic, endian-safe packing for multiplayer network packets.
 * Wire format is big-endian (network byte order).
 */

#include "network/net_packets.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* --- Memory Stream Cursor Helper --- */
typedef struct {
    u8* data;
    size_t size;
    size_t offset;
    int error;
} net_buf_writer_t;

typedef struct {
    const u8* data;
    size_t size;
    size_t offset;
    int error;
} net_buf_reader_t;

static void w_init(net_buf_writer_t* w, u8* data, size_t size) {
    w->data = data;
    w->size = size;
    w->offset = 0;
    w->error = 0;
}

static void r_init(net_buf_reader_t* r, const u8* data, size_t size) {
    r->data = data;
    r->size = size;
    r->offset = 0;
    r->error = 0;
}

static void w_u8(net_buf_writer_t* w, u8 val) {
    if (w->offset + 1 > w->size) { w->error = 1; return; }
    w->data[w->offset++] = val;
}

static void w_s8(net_buf_writer_t* w, s8 val) {
    w_u8(w, (u8)val);
}

static void w_u16(net_buf_writer_t* w, u16 val) {
    if (w->offset + 2 > w->size) { w->error = 1; return; }
    w->data[w->offset++] = (u8)((val >> 8) & 0xFF);
    w->data[w->offset++] = (u8)(val & 0xFF);
}

static void w_s16(net_buf_writer_t* w, s16 val) {
    w_u16(w, (u16)val);
}

static void w_u32(net_buf_writer_t* w, u32 val) {
    if (w->offset + 4 > w->size) { w->error = 1; return; }
    w->data[w->offset++] = (u8)((val >> 24) & 0xFF);
    w->data[w->offset++] = (u8)((val >> 16) & 0xFF);
    w->data[w->offset++] = (u8)((val >> 8) & 0xFF);
    w->data[w->offset++] = (u8)(val & 0xFF);
}

static void w_s32(net_buf_writer_t* w, s32 val) {
    w_u32(w, (u32)val);
}

static void w_f32(net_buf_writer_t* w, f32 val) {
    u32 u;
    memcpy(&u, &val, 4);
    w_u32(w, u);
}

static void w_bytes(net_buf_writer_t* w, const void* src, size_t len) {
    if (w->offset + len > w->size) { w->error = 1; return; }
    memcpy(w->data + w->offset, src, len);
    w->offset += len;
}

static void w_str(net_buf_writer_t* w, const char* str, size_t fixed_len) {
    size_t slen = strlen(str);
    size_t copy_len = (slen < fixed_len) ? slen : fixed_len;
    if (w->offset + fixed_len > w->size) { w->error = 1; return; }
    memset(w->data + w->offset, 0, fixed_len);
    memcpy(w->data + w->offset, str, copy_len);
    w->offset += fixed_len;
}

static u8 r_u8(net_buf_reader_t* r) {
    if (r->offset + 1 > r->size) { r->error = 1; return 0; }
    return r->data[r->offset++];
}

static s8 r_s8(net_buf_reader_t* r) {
    return (s8)r_u8(r);
}

static u16 r_u16(net_buf_reader_t* r) {
    u16 val;
    if (r->offset + 2 > r->size) { r->error = 1; return 0; }
    val = ((u16)r->data[r->offset] << 8) | (u16)r->data[r->offset + 1];
    r->offset += 2;
    return val;
}

static s16 r_s16(net_buf_reader_t* r) {
    return (s16)r_u16(r);
}

static u32 r_u32(net_buf_reader_t* r) {
    u32 val;
    if (r->offset + 4 > r->size) { r->error = 1; return 0; }
    val = ((u32)r->data[r->offset] << 24) |
          ((u32)r->data[r->offset + 1] << 16) |
          ((u32)r->data[r->offset + 2] << 8) |
          (u32)r->data[r->offset + 3];
    r->offset += 4;
    return val;
}

static s32 r_s32(net_buf_reader_t* r) {
    return (s32)r_u32(r);
}

static f32 r_f32(net_buf_reader_t* r) {
    u32 u = r_u32(r);
    f32 f;
    memcpy(&f, &u, 4);
    return f;
}

static void r_bytes(net_buf_reader_t* r, void* dst, size_t len) {
    if (r->offset + len > r->size) { r->error = 1; return; }
    memcpy(dst, r->data + r->offset, len);
    r->offset += len;
}

static void r_str(net_buf_reader_t* r, char* dst, size_t fixed_len) {
    if (r->offset + fixed_len > r->size) { r->error = 1; return; }
    memcpy(dst, r->data + r->offset, fixed_len);
    dst[fixed_len - 1] = '\0';
    r->offset += fixed_len;
}

/* --- Packet Header --- */

int net_pack_header(u8* buf, size_t buf_size, const net_header_t* hdr) {
    net_buf_writer_t w;
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr->magic);
    w_u8(&w, hdr->packet_id);
    w_u8(&w, hdr->channel);
    w_u16(&w, hdr->payload_size);
    return w.error ? -1 : (int)w.offset;
}

int net_unpack_header(const u8* buf, size_t buf_size, net_header_t* hdr) {
    net_buf_reader_t r;
    r_init(&r, buf, buf_size);
    hdr->magic = r_u16(&r);
    hdr->packet_id = r_u8(&r);
    hdr->channel = r_u8(&r);
    hdr->payload_size = r_u16(&r);
    if (r.error || hdr->magic != NET_PACKET_MAGIC) {
        return -1;
    }
    return (int)r.offset;
}

/* --- Ping / Pong --- */

int net_pack_ping(u8* buf, size_t buf_size, const pkt_ping_t* pkt) {
    net_buf_writer_t w;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_PING, NET_CH_RELIABLE, 4 };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);
    w_u32(&w, pkt->timestamp_ms);
    return w.error ? -1 : (int)w.offset;
}

int net_unpack_ping(const u8* buf, size_t buf_size, pkt_ping_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_PING || hdr.payload_size < 4) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);
    pkt->timestamp_ms = r_u32(&r);
    return r.error ? -1 : (int)(hlen + r.offset);
}

int net_pack_pong(u8* buf, size_t buf_size, const pkt_pong_t* pkt) {
    net_buf_writer_t w;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_PONG, NET_CH_RELIABLE, 8 };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);
    w_u32(&w, pkt->client_timestamp_ms);
    w_u32(&w, pkt->server_timestamp_ms);
    return w.error ? -1 : (int)w.offset;
}

int net_unpack_pong(const u8* buf, size_t buf_size, pkt_pong_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_PONG || hdr.payload_size < 8) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);
    pkt->client_timestamp_ms = r_u32(&r);
    pkt->server_timestamp_ms = r_u32(&r);
    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Disconnect --- */

int net_pack_disconnect(u8* buf, size_t buf_size, u8 packet_id, const pkt_disconnect_t* pkt) {
    net_buf_writer_t w;
    net_header_t hdr = { NET_PACKET_MAGIC, packet_id, NET_CH_RELIABLE, 3 };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);
    w_u16(&w, pkt->client_id);
    w_u8(&w, pkt->reason);
    return w.error ? -1 : (int)w.offset;
}

int net_unpack_disconnect(const u8* buf, size_t buf_size, pkt_disconnect_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || (hdr.packet_id != PKT_C2S_DISCONNECT && hdr.packet_id != PKT_S2C_DISCONNECT) || hdr.payload_size < 3) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);
    pkt->client_id = r_u16(&r);
    pkt->reason = r_u8(&r);
    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Handshake --- */

int net_pack_handshake(u8* buf, size_t buf_size, const pkt_c2s_handshake_t* pkt) {
    net_buf_writer_t w;
    u16 payload_size = 4 + 2 + NET_MAX_NAME_LEN + NET_MAX_NAME_LEN + 2 + 2 + 1 + 1 + 2 + NET_MAX_AUTH_TOKEN_LEN;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_C2S_HANDSHAKE, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u32(&w, pkt->client_version);
    w_u16(&w, pkt->protocol_version);
    w_str(&w, pkt->player_name, NET_MAX_NAME_LEN);
    w_str(&w, pkt->town_name, NET_MAX_NAME_LEN);
    w_u16(&w, pkt->player_id);
    w_u16(&w, pkt->land_id);
    w_u8(&w, pkt->gender);
    w_u8(&w, pkt->face_type);
    w_u16(&w, pkt->clothes_id);
    w_bytes(&w, pkt->auth_token, NET_MAX_AUTH_TOKEN_LEN);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_handshake(const u8* buf, size_t buf_size, pkt_c2s_handshake_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_C2S_HANDSHAKE) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->client_version = r_u32(&r);
    pkt->protocol_version = r_u16(&r);
    r_str(&r, pkt->player_name, NET_MAX_NAME_LEN);
    r_str(&r, pkt->town_name, NET_MAX_NAME_LEN);
    pkt->player_id = r_u16(&r);
    pkt->land_id = r_u16(&r);
    pkt->gender = r_u8(&r);
    pkt->face_type = r_u8(&r);
    pkt->clothes_id = r_u16(&r);
    r_bytes(&r, pkt->auth_token, NET_MAX_AUTH_TOKEN_LEN);

    return r.error ? -1 : (int)(hlen + r.offset);
}

int net_pack_handshake_ack(u8* buf, size_t buf_size, const pkt_s2c_handshake_ack_t* pkt) {
    net_buf_writer_t w;
    /* 1 (status) + 2 (client_id) + 8 (lbRTC_time_c) + 16 (town_name) + 2 (land_id) + 1 (weather) + 2 (starting_zone_id) = 32 */
    u16 payload_size = 1 + 2 + sizeof(lbRTC_time_c) + NET_MAX_NAME_LEN + 2 + 1 + 2;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_S2C_HANDSHAKE_ACK, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u8(&w, pkt->status);
    w_u16(&w, pkt->client_id);

    /* lbRTC_time_c serialization */
    w_u16(&w, pkt->world_time.year);
    w_u8(&w, pkt->world_time.month);
    w_u8(&w, pkt->world_time.day);
    w_u8(&w, pkt->world_time.hour);
    w_u8(&w, pkt->world_time.min);
    w_u8(&w, pkt->world_time.sec);
    w_u8(&w, pkt->world_time.weekday);

    w_str(&w, pkt->town_name, NET_MAX_NAME_LEN);
    w_u16(&w, pkt->land_id);
    w_u8(&w, pkt->weather);
    w_u16(&w, pkt->starting_zone_id);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_handshake_ack(const u8* buf, size_t buf_size, pkt_s2c_handshake_ack_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_S2C_HANDSHAKE_ACK) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->status = r_u8(&r);
    pkt->client_id = r_u16(&r);

    pkt->world_time.year = r_u16(&r);
    pkt->world_time.month = r_u8(&r);
    pkt->world_time.day = r_u8(&r);
    pkt->world_time.hour = r_u8(&r);
    pkt->world_time.min = r_u8(&r);
    pkt->world_time.sec = r_u8(&r);
    pkt->world_time.weekday = r_u8(&r);

    r_str(&r, pkt->town_name, NET_MAX_NAME_LEN);
    pkt->land_id = r_u16(&r);
    pkt->weather = r_u8(&r);
    pkt->starting_zone_id = r_u16(&r);

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Character Finalization --- */

int net_pack_character_finalize(u8* buf, size_t buf_size, const pkt_c2s_character_finalize_t* pkt) {
    net_buf_writer_t w;
    /* 2 (client_id) + NET_MAX_NAME_LEN (16) + 1 (gender) + 1 (face) + 2 (clothes) + NET_MAX_AUTH_TOKEN_LEN (36) = 58 */
    u16 payload_size = 2 + NET_MAX_NAME_LEN + 1 + 1 + 2 + NET_MAX_AUTH_TOKEN_LEN;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_C2S_CHARACTER_FINALIZE, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u16(&w, pkt->client_id);
    w_str(&w, pkt->player_name, NET_MAX_NAME_LEN);
    w_u8(&w, pkt->gender);
    w_u8(&w, pkt->face_type);
    w_u16(&w, pkt->clothes_id);
    w_bytes(&w, pkt->auth_token, NET_MAX_AUTH_TOKEN_LEN);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_character_finalize(const u8* buf, size_t buf_size, pkt_c2s_character_finalize_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_C2S_CHARACTER_FINALIZE) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->client_id = r_u16(&r);
    r_str(&r, pkt->player_name, NET_MAX_NAME_LEN);
    pkt->gender = r_u8(&r);
    pkt->face_type = r_u8(&r);
    pkt->clothes_id = r_u16(&r);
    r_bytes(&r, pkt->auth_token, NET_MAX_AUTH_TOKEN_LEN);

    return r.error ? -1 : (int)(hlen + r.offset);
}

int net_pack_character_ack(u8* buf, size_t buf_size, const pkt_s2c_character_ack_t* pkt) {
    net_buf_writer_t w;
    /* 1 (status) + 2 (client_id) + 1 (is_resident) + 1 (slot_idx) = 5 */
    u16 payload_size = 1 + 2 + 1 + 1;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_S2C_CHARACTER_ACK, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u8(&w, pkt->status);
    w_u16(&w, pkt->client_id);
    w_u8(&w, pkt->is_resident);
    w_u8(&w, pkt->slot_idx);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_character_ack(const u8* buf, size_t buf_size, pkt_s2c_character_ack_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_S2C_CHARACTER_ACK) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->status = r_u8(&r);
    pkt->client_id = r_u16(&r);
    pkt->is_resident = r_u8(&r);
    pkt->slot_idx = r_u8(&r);

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- House Move-In --- */

int net_pack_movein_req(u8* buf, size_t buf_size, const pkt_c2s_movein_req_t* pkt) {
    net_buf_writer_t w;
    /* 2 (client_id) + 1 (house_idx) + NET_MAX_AUTH_TOKEN_LEN (32) = 35 */
    u16 payload_size = 2 + 1 + NET_MAX_AUTH_TOKEN_LEN;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_C2S_MOVEIN_REQ, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u16(&w, pkt->client_id);
    w_u8(&w, pkt->house_idx);
    w_bytes(&w, pkt->auth_token, NET_MAX_AUTH_TOKEN_LEN);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_movein_req(const u8* buf, size_t buf_size, pkt_c2s_movein_req_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_C2S_MOVEIN_REQ) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->client_id = r_u16(&r);
    pkt->house_idx = r_u8(&r);
    r_bytes(&r, pkt->auth_token, NET_MAX_AUTH_TOKEN_LEN);

    return r.error ? -1 : (int)(hlen + r.offset);
}

int net_pack_movein_ack(u8* buf, size_t buf_size, const pkt_s2c_movein_ack_t* pkt) {
    net_buf_writer_t w;
    /* 1 (status) + 2 (client_id) + 1 (house_idx) + 1 (resident_slot) + 1 (roommate_slot) = 6 */
    u16 payload_size = 1 + 2 + 1 + 1 + 1;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_S2C_MOVEIN_ACK, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u8(&w, pkt->status);
    w_u16(&w, pkt->client_id);
    w_u8(&w, pkt->house_idx);
    w_u8(&w, pkt->resident_slot);
    w_u8(&w, pkt->roommate_slot);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_movein_ack(const u8* buf, size_t buf_size, pkt_s2c_movein_ack_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_S2C_MOVEIN_ACK) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->status = r_u8(&r);
    pkt->client_id = r_u16(&r);
    pkt->house_idx = r_u8(&r);
    pkt->resident_slot = r_u8(&r);
    pkt->roommate_slot = r_u8(&r);

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Input --- */

int net_pack_input(u8* buf, size_t buf_size, const pkt_c2s_input_t* pkt) {
    net_buf_writer_t w;
    /* 4 (seq) + 2 (client_id) + 1 (stick_x) + 1 (stick_y) + 4 (buttons) + 2 (tool) + 12 (xyz) + 2 (angle_y) = 28 */
    u16 payload_size = 4 + 2 + 1 + 1 + 4 + 2 + 12 + 2;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_C2S_INPUT, NET_CH_UNRELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u32(&w, pkt->sequence);
    w_u16(&w, pkt->client_id);
    w_s8(&w, pkt->stick_x);
    w_s8(&w, pkt->stick_y);
    w_u32(&w, pkt->button_mask);
    w_u16(&w, pkt->current_tool_id);
    w_f32(&w, pkt->player_x);
    w_f32(&w, pkt->player_y);
    w_f32(&w, pkt->player_z);
    w_s16(&w, pkt->angle_y);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_input(const u8* buf, size_t buf_size, pkt_c2s_input_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_C2S_INPUT) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->sequence = r_u32(&r);
    pkt->client_id = r_u16(&r);
    pkt->stick_x = r_s8(&r);
    pkt->stick_y = r_s8(&r);
    pkt->button_mask = r_u32(&r);
    pkt->current_tool_id = r_u16(&r);
    pkt->player_x = r_f32(&r);
    pkt->player_y = r_f32(&r);
    pkt->player_z = r_f32(&r);
    pkt->angle_y = r_s16(&r);

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Entity Sync --- */

int net_pack_entity_sync(u8* buf, size_t buf_size, const pkt_s2c_entity_sync_t* pkt) {
    net_buf_writer_t w;
    u16 count = pkt->count > NET_MAX_ENTITIES_PER_SYNC ? NET_MAX_ENTITIES_PER_SYNC : pkt->count;
    /* Single entity record size: 4 (actor_id) + 2 (zone_id) + 12 (pos) + 2 (angle) + 2 (anim) + 2 (frame) + 2 (tool) + 2 (flags) = 28 bytes */
    u16 payload_size = 4 + 2 + (count * 28);
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_S2C_ENTITY_SYNC, NET_CH_UNRELIABLE, payload_size };
    u16 i;

    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u32(&w, pkt->frame_id);
    w_u16(&w, count);

    for (i = 0; i < count; i++) {
        const net_entity_state_t* e = &pkt->entities[i];
        w_u32(&w, e->actor_id);
        w_u16(&w, e->zone_id);
        w_f32(&w, e->pos_x);
        w_f32(&w, e->pos_y);
        w_f32(&w, e->pos_z);
        w_s16(&w, e->angle_y);
        w_u16(&w, e->anim_id);
        w_u16(&w, e->anim_frame);
        w_u16(&w, e->tool_id);
        w_u16(&w, e->state_flags);
    }

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_entity_sync(const u8* buf, size_t buf_size, pkt_s2c_entity_sync_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    u16 i, count;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_S2C_ENTITY_SYNC) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->frame_id = r_u32(&r);
    count = r_u16(&r);
    if (count > NET_MAX_ENTITIES_PER_SYNC) {
        count = NET_MAX_ENTITIES_PER_SYNC;
    }
    pkt->count = count;

    for (i = 0; i < count; i++) {
        net_entity_state_t* e = &pkt->entities[i];
        e->actor_id = r_u32(&r);
        e->zone_id = r_u16(&r);
        e->pos_x = r_f32(&r);
        e->pos_y = r_f32(&r);
        e->pos_z = r_f32(&r);
        e->angle_y = r_s16(&r);
        e->anim_id = r_u16(&r);
        e->anim_frame = r_u16(&r);
        e->tool_id = r_u16(&r);
        e->state_flags = r_u16(&r);
    }

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Interaction Request --- */

int net_pack_interact_req(u8* buf, size_t buf_size, const pkt_c2s_interact_req_t* pkt) {
    net_buf_writer_t w;
    /* 2 (client_id) + 1 (action) + 1 (bx) + 1 (bz) + 1 (ux) + 1 (uz) + 12 (world xyz) + 4 (target_id) + 2 (item_arg) = 25 */
    u16 payload_size = 2 + 1 + 1 + 1 + 1 + 1 + 12 + 4 + 2;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_C2S_INTERACT_REQ, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u16(&w, pkt->client_id);
    w_u8(&w, pkt->action_type);
    w_s8(&w, pkt->block_x);
    w_s8(&w, pkt->block_z);
    w_u8(&w, pkt->ut_x);
    w_u8(&w, pkt->ut_z);
    w_f32(&w, pkt->world_x);
    w_f32(&w, pkt->world_y);
    w_f32(&w, pkt->world_z);
    w_u32(&w, pkt->target_actor_id);
    w_u16(&w, pkt->item_arg);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_interact_req(const u8* buf, size_t buf_size, pkt_c2s_interact_req_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_C2S_INTERACT_REQ) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->client_id = r_u16(&r);
    pkt->action_type = r_u8(&r);
    pkt->block_x = r_s8(&r);
    pkt->block_z = r_s8(&r);
    pkt->ut_x = r_u8(&r);
    pkt->ut_z = r_u8(&r);
    pkt->world_x = r_f32(&r);
    pkt->world_y = r_f32(&r);
    pkt->world_z = r_f32(&r);
    pkt->target_actor_id = r_u32(&r);
    pkt->item_arg = r_u16(&r);

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- FG Delta --- */

int net_pack_fg_delta(u8* buf, size_t buf_size, const pkt_s2c_fg_delta_t* pkt) {
    net_buf_writer_t w;
    /* 2 (zone_id) + 1 (bx) + 1 (bz) + 1 (ux) + 1 (uz) + 2 (old_item) + 2 (new_item) = 10 */
    u16 payload_size = 2 + 1 + 1 + 1 + 1 + 2 + 2;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_S2C_FG_DELTA, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u16(&w, pkt->zone_id);
    w_u8(&w, pkt->block_x);
    w_u8(&w, pkt->block_z);
    w_u8(&w, pkt->ut_x);
    w_u8(&w, pkt->ut_z);
    w_u16(&w, pkt->old_item);
    w_u16(&w, pkt->new_item);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_fg_delta(const u8* buf, size_t buf_size, pkt_s2c_fg_delta_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_S2C_FG_DELTA) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->zone_id = r_u16(&r);
    pkt->block_x = r_u8(&r);
    pkt->block_z = r_u8(&r);
    pkt->ut_x = r_u8(&r);
    pkt->ut_z = r_u8(&r);
    pkt->old_item = r_u16(&r);
    pkt->new_item = r_u16(&r);

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Zone Enter --- */

int net_pack_zone_enter(u8* buf, size_t buf_size, const pkt_s2c_zone_enter_t* pkt) {
    net_buf_writer_t w;
    u8 count = pkt->furniture_count > NET_MAX_ROOM_FURNITURE ? NET_MAX_ROOM_FURNITURE : pkt->furniture_count;
    /* 2 (client_id) + 2 (zone_id) + 2 (scene_id) + 12 (spawn xyz) + 2 (spawn_angle) + 1 (has_room_data) + 1 (count) + count*5 = 22 + count*5 */
    u16 payload_size = 2 + 2 + 2 + 12 + 2 + 1 + 1 + (count * 5);
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_S2C_ZONE_ENTER, NET_CH_RELIABLE, payload_size };
    u8 i;

    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u16(&w, pkt->client_id);
    w_u16(&w, pkt->zone_id);
    w_s16(&w, pkt->scene_id);
    w_f32(&w, pkt->spawn_x);
    w_f32(&w, pkt->spawn_y);
    w_f32(&w, pkt->spawn_z);
    w_s16(&w, pkt->spawn_angle_y);
    w_u8(&w, pkt->has_room_data);
    w_u8(&w, count);

    for (i = 0; i < count; i++) {
        const net_room_furniture_t* f = &pkt->furniture[i];
        w_u16(&w, f->ftr_id);
        w_u8(&w, f->ut_x);
        w_u8(&w, f->ut_z);
        w_u8(&w, f->rotation);
    }

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_zone_enter(const u8* buf, size_t buf_size, pkt_s2c_zone_enter_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    u8 i, count;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_S2C_ZONE_ENTER) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->client_id = r_u16(&r);
    pkt->zone_id = r_u16(&r);
    pkt->scene_id = r_s16(&r);
    pkt->spawn_x = r_f32(&r);
    pkt->spawn_y = r_f32(&r);
    pkt->spawn_z = r_f32(&r);
    pkt->spawn_angle_y = r_s16(&r);
    pkt->has_room_data = r_u8(&r);
    count = r_u8(&r);
    if (count > NET_MAX_ROOM_FURNITURE) {
        count = NET_MAX_ROOM_FURNITURE;
    }
    pkt->furniture_count = count;

    for (i = 0; i < count; i++) {
        net_room_furniture_t* f = &pkt->furniture[i];
        f->ftr_id = r_u16(&r);
        f->ut_x = r_u8(&r);
        f->ut_z = r_u8(&r);
        f->rotation = r_u8(&r);
    }

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Dialogue Input & Dialogue Node --- */

int net_pack_dialog_input(u8* buf, size_t buf_size, const pkt_c2s_dialog_input_t* pkt) {
    net_buf_writer_t w;
    /* 2 (client_id) + 4 (speaker_actor_id) + 1 (action) + 1 (choice_idx) = 8 */
    u16 payload_size = 2 + 4 + 1 + 1;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_C2S_DIALOG_INPUT, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u16(&w, pkt->client_id);
    w_u32(&w, pkt->speaker_actor_id);
    w_u8(&w, pkt->input_action);
    w_u8(&w, pkt->choice_idx);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_dialog_input(const u8* buf, size_t buf_size, pkt_c2s_dialog_input_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_C2S_DIALOG_INPUT) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->client_id = r_u16(&r);
    pkt->speaker_actor_id = r_u32(&r);
    pkt->input_action = r_u8(&r);
    pkt->choice_idx = r_u8(&r);

    return r.error ? -1 : (int)(hlen + r.offset);
}

int net_pack_dialog_node(u8* buf, size_t buf_size, const pkt_s2c_dialog_node_t* pkt) {
    net_buf_writer_t w;
    /* 2 (client_id) + 4 (speaker_actor_id) + 2 (message_id) + 1 (num_choices) + 1 (options_bitmask) = 10 */
    u16 payload_size = 2 + 4 + 2 + 1 + 1;
    net_header_t hdr = { NET_PACKET_MAGIC, PKT_S2C_DIALOG_NODE, NET_CH_RELIABLE, payload_size };
    w_init(&w, buf, buf_size);
    w_u16(&w, hdr.magic);
    w_u8(&w, hdr.packet_id);
    w_u8(&w, hdr.channel);
    w_u16(&w, hdr.payload_size);

    w_u16(&w, pkt->client_id);
    w_u32(&w, pkt->speaker_actor_id);
    w_u16(&w, pkt->message_id);
    w_u8(&w, pkt->num_choices);
    w_u8(&w, pkt->options_bitmask);

    return w.error ? -1 : (int)w.offset;
}

int net_unpack_dialog_node(const u8* buf, size_t buf_size, pkt_s2c_dialog_node_t* pkt) {
    net_buf_reader_t r;
    net_header_t hdr;
    int hlen = net_unpack_header(buf, buf_size, &hdr);
    if (hlen < 0 || hdr.packet_id != PKT_S2C_DIALOG_NODE) return -1;
    r_init(&r, buf + hlen, buf_size - hlen);

    pkt->client_id = r_u16(&r);
    pkt->speaker_actor_id = r_u32(&r);
    pkt->message_id = r_u16(&r);
    pkt->num_choices = r_u8(&r);
    pkt->options_bitmask = r_u8(&r);

    return r.error ? -1 : (int)(hlen + r.offset);
}

/* --- Round-Trip Serialization Unit Tests --- */

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[NET TEST FAILED] %s: line %d\n", msg, __LINE__); \
            return 0; \
        } \
    } while(0)

#define FLOAT_EQ(a, b) (fabsf((a) - (b)) < 0.0001f)

int net_packer_run_tests(void) {
    u8 buf[2048];
    int len;

    /* 1. Ping / Pong */
    {
        pkt_ping_t orig = { 987654321 };
        pkt_ping_t dec;
        len = net_pack_ping(buf, sizeof(buf), &orig);
        TEST_ASSERT(len > 0, "net_pack_ping");
        TEST_ASSERT(net_unpack_ping(buf, len, &dec) > 0, "net_unpack_ping");
        TEST_ASSERT(dec.timestamp_ms == orig.timestamp_ms, "ping timestamp");

        pkt_pong_t pong_orig = { 111111, 222222 };
        pkt_pong_t pong_dec;
        len = net_pack_pong(buf, sizeof(buf), &pong_orig);
        TEST_ASSERT(len > 0, "net_pack_pong");
        TEST_ASSERT(net_unpack_pong(buf, len, &pong_dec) > 0, "net_unpack_pong");
        TEST_ASSERT(pong_dec.client_timestamp_ms == pong_orig.client_timestamp_ms &&
                    pong_dec.server_timestamp_ms == pong_orig.server_timestamp_ms, "pong timestamps");
    }

    /* 2. Disconnect */
    {
        pkt_disconnect_t orig = { 3, 42 };
        pkt_disconnect_t dec;
        len = net_pack_disconnect(buf, sizeof(buf), PKT_C2S_DISCONNECT, &orig);
        TEST_ASSERT(len > 0, "net_pack_disconnect");
        TEST_ASSERT(net_unpack_disconnect(buf, len, &dec) > 0, "net_unpack_disconnect");
        TEST_ASSERT(dec.client_id == orig.client_id && dec.reason == orig.reason, "disconnect fields");
    }

    /* 3. Handshake & Handshake ACK */
    {
        pkt_c2s_handshake_t orig;
        pkt_c2s_handshake_t dec;
        memset(&orig, 0, sizeof(orig));
        orig.client_version = 0x00010002;
        orig.protocol_version = 1;
        strncpy(orig.player_name, "Villager", sizeof(orig.player_name) - 1);
        strncpy(orig.town_name, "Animal", sizeof(orig.town_name) - 1);
        orig.player_id = 0x1234;
        orig.land_id = 0x5678;
        orig.gender = 1;
        orig.face_type = 3;
        orig.clothes_id = 0x2400;
        strncpy(orig.auth_token, "abcdef0123456789", sizeof(orig.auth_token) - 1);

        len = net_pack_handshake(buf, sizeof(buf), &orig);
        TEST_ASSERT(len > 0, "net_pack_handshake");
        TEST_ASSERT(net_unpack_handshake(buf, len, &dec) > 0, "net_unpack_handshake");
        TEST_ASSERT(dec.client_version == orig.client_version, "handshake version");
        TEST_ASSERT(strcmp(dec.player_name, orig.player_name) == 0, "handshake player_name");
        TEST_ASSERT(strcmp(dec.town_name, orig.town_name) == 0, "handshake town_name");
        TEST_ASSERT(dec.player_id == orig.player_id, "handshake player_id");
        TEST_ASSERT(dec.land_id == orig.land_id, "handshake land_id");
        TEST_ASSERT(dec.gender == orig.gender && dec.face_type == orig.face_type, "handshake appearance");
        TEST_ASSERT(strcmp(dec.auth_token, orig.auth_token) == 0, "handshake auth_token");

        pkt_s2c_handshake_ack_t ack_orig;
        pkt_s2c_handshake_ack_t ack_dec;
        memset(&ack_orig, 0, sizeof(ack_orig));
        ack_orig.status = NET_HS_OK;
        ack_orig.client_id = 2;
        ack_orig.world_time.year = 2026;
        ack_orig.world_time.month = 10;
        ack_orig.world_time.day = 16;
        ack_orig.world_time.hour = 20;
        ack_orig.world_time.min = 30;
        ack_orig.world_time.sec = 45;
        ack_orig.world_time.weekday = 5;
        strncpy(ack_orig.town_name, "Animal", sizeof(ack_orig.town_name) - 1);
        ack_orig.land_id = 0x5678;
        ack_orig.weather = 1;
        ack_orig.starting_zone_id = 0;

        len = net_pack_handshake_ack(buf, sizeof(buf), &ack_orig);
        TEST_ASSERT(len > 0, "net_pack_handshake_ack");
        TEST_ASSERT(net_unpack_handshake_ack(buf, len, &ack_dec) > 0, "net_unpack_handshake_ack");
        TEST_ASSERT(ack_dec.status == ack_orig.status && ack_dec.client_id == ack_orig.client_id, "ack status/id");
        TEST_ASSERT(ack_dec.world_time.year == 2026 && ack_dec.world_time.min == 30, "ack world_time");
        TEST_ASSERT(strcmp(ack_dec.town_name, "Animal") == 0, "ack town_name");
        TEST_ASSERT(ack_dec.land_id == 0x5678 && ack_dec.weather == 1, "ack land/weather");
    }

    /* 4. Input Stream */
    {
        pkt_c2s_input_t orig = { 1024, 1, -45, 80, 0x00000009, 0x2410, 1234.5f, 50.0f, -678.9f, 0x4000 };
        pkt_c2s_input_t dec;
        len = net_pack_input(buf, sizeof(buf), &orig);
        TEST_ASSERT(len > 0, "net_pack_input");
        TEST_ASSERT(net_unpack_input(buf, len, &dec) > 0, "net_unpack_input");
        TEST_ASSERT(dec.sequence == orig.sequence && dec.client_id == orig.client_id, "input seq/client");
        TEST_ASSERT(dec.stick_x == orig.stick_x && dec.stick_y == orig.stick_y, "input sticks");
        TEST_ASSERT(dec.button_mask == orig.button_mask && dec.current_tool_id == orig.current_tool_id, "input buttons/tool");
        TEST_ASSERT(FLOAT_EQ(dec.player_x, orig.player_x) &&
                    FLOAT_EQ(dec.player_y, orig.player_y) &&
                    FLOAT_EQ(dec.player_z, orig.player_z), "input positions");
        TEST_ASSERT(dec.angle_y == orig.angle_y, "input angle");
    }

    /* 5. Entity Sync */
    {
        pkt_s2c_entity_sync_t orig;
        pkt_s2c_entity_sync_t dec;
        orig.frame_id = 555;
        orig.count = 2;
        orig.entities[0].actor_id = 1001;
        orig.entities[0].zone_id = 0;
        orig.entities[0].pos_x = 100.5f;
        orig.entities[0].pos_y = 20.0f;
        orig.entities[0].pos_z = -300.25f;
        orig.entities[0].angle_y = -8000;
        orig.entities[0].anim_id = 4;
        orig.entities[0].anim_frame = 12;
        orig.entities[0].tool_id = 0x2405;
        orig.entities[0].state_flags = 0x0001;

        orig.entities[1].actor_id = 2002;
        orig.entities[1].zone_id = 1;
        orig.entities[1].pos_x = -50.0f;
        orig.entities[1].pos_y = 0.0f;
        orig.entities[1].pos_z = 75.0f;
        orig.entities[1].angle_y = 16000;
        orig.entities[1].anim_id = 1;
        orig.entities[1].anim_frame = 0;
        orig.entities[1].tool_id = 0;
        orig.entities[1].state_flags = 0x0002;

        len = net_pack_entity_sync(buf, sizeof(buf), &orig);
        TEST_ASSERT(len > 0, "net_pack_entity_sync");
        TEST_ASSERT(net_unpack_entity_sync(buf, len, &dec) > 0, "net_unpack_entity_sync");
        TEST_ASSERT(dec.frame_id == orig.frame_id && dec.count == 2, "entity sync frame/count");
        TEST_ASSERT(dec.entities[0].actor_id == 1001 && FLOAT_EQ(dec.entities[0].pos_x, 100.5f), "entity 0");
        TEST_ASSERT(dec.entities[1].actor_id == 2002 && dec.entities[1].angle_y == 16000, "entity 1");
    }

    /* 6. Interaction Request */
    {
        pkt_c2s_interact_req_t orig = {
            1, NET_INTERACT_DIG, 3, 2, 7, 9, 320.0f, 40.0f, 280.0f, 0x10A0, 0x2001
        };
        pkt_c2s_interact_req_t dec;
        len = net_pack_interact_req(buf, sizeof(buf), &orig);
        TEST_ASSERT(len > 0, "net_pack_interact_req");
        TEST_ASSERT(net_unpack_interact_req(buf, len, &dec) > 0, "net_unpack_interact_req");
        TEST_ASSERT(dec.client_id == orig.client_id && dec.action_type == orig.action_type, "interact ids");
        TEST_ASSERT(dec.block_x == 3 && dec.block_z == 2 && dec.ut_x == 7 && dec.ut_z == 9, "interact tiles");
        TEST_ASSERT(FLOAT_EQ(dec.world_x, 320.0f) && FLOAT_EQ(dec.world_z, 280.0f), "interact coords");
        TEST_ASSERT(dec.target_actor_id == 0x10A0 && dec.item_arg == 0x2001, "interact target/arg");
    }

    /* 7. FG Delta */
    {
        pkt_s2c_fg_delta_t orig = { 0, 2, 3, 10, 11, 0x0804, 0x0000 };
        pkt_s2c_fg_delta_t dec;
        len = net_pack_fg_delta(buf, sizeof(buf), &orig);
        TEST_ASSERT(len > 0, "net_pack_fg_delta");
        TEST_ASSERT(net_unpack_fg_delta(buf, len, &dec) > 0, "net_unpack_fg_delta");
        TEST_ASSERT(dec.zone_id == orig.zone_id && dec.block_x == 2 && dec.block_z == 3, "fg_delta zone/block");
        TEST_ASSERT(dec.ut_x == 10 && dec.ut_z == 11, "fg_delta tile");
        TEST_ASSERT(dec.old_item == 0x0804 && dec.new_item == 0x0000, "fg_delta items");
    }

    /* 8. Zone Enter */
    {
        pkt_s2c_zone_enter_t orig;
        pkt_s2c_zone_enter_t dec;
        orig.client_id = 0;
        orig.zone_id = 5;
        orig.scene_id = 18;
        orig.spawn_x = 40.0f;
        orig.spawn_y = 0.0f;
        orig.spawn_z = 80.0f;
        orig.spawn_angle_y = -16384;
        orig.has_room_data = 1;
        orig.furniture_count = 2;
        orig.furniture[0].ftr_id = 0x1000;
        orig.furniture[0].ut_x = 4;
        orig.furniture[0].ut_z = 4;
        orig.furniture[0].rotation = 0;
        orig.furniture[1].ftr_id = 0x1005;
        orig.furniture[1].ut_x = 8;
        orig.furniture[1].ut_z = 6;
        orig.furniture[1].rotation = 2;

        len = net_pack_zone_enter(buf, sizeof(buf), &orig);
        TEST_ASSERT(len > 0, "net_pack_zone_enter");
        TEST_ASSERT(net_unpack_zone_enter(buf, len, &dec) > 0, "net_unpack_zone_enter");
        TEST_ASSERT(dec.client_id == 0 && dec.zone_id == 5 && dec.scene_id == 18, "zone_enter ids");
        TEST_ASSERT(FLOAT_EQ(dec.spawn_x, 40.0f) && dec.spawn_angle_y == -16384, "zone_enter spawn");
        TEST_ASSERT(dec.has_room_data == 1 && dec.furniture_count == 2, "zone_enter ftr count");
        TEST_ASSERT(dec.furniture[0].ftr_id == 0x1000 && dec.furniture[1].rotation == 2, "zone_enter ftr data");
    }

    /* 9. Dialogue Input & Node */
    {
        pkt_c2s_dialog_input_t d_in_orig = { 1, 0x12345678, 1, 2 };
        pkt_c2s_dialog_input_t d_in_dec;
        len = net_pack_dialog_input(buf, sizeof(buf), &d_in_orig);
        TEST_ASSERT(len > 0, "net_pack_dialog_input");
        TEST_ASSERT(net_unpack_dialog_input(buf, len, &d_in_dec) > 0, "net_unpack_dialog_input");
        TEST_ASSERT(d_in_dec.client_id == 1 && d_in_dec.speaker_actor_id == 0x12345678, "dialog input ids");
        TEST_ASSERT(d_in_dec.input_action == 1 && d_in_dec.choice_idx == 2, "dialog input choice");

        pkt_s2c_dialog_node_t d_node_orig = { 1, 0x12345678, 0x0542, 3, 0x07 };
        pkt_s2c_dialog_node_t d_node_dec;
        len = net_pack_dialog_node(buf, sizeof(buf), &d_node_orig);
        TEST_ASSERT(len > 0, "net_pack_dialog_node");
        TEST_ASSERT(net_unpack_dialog_node(buf, len, &d_node_dec) > 0, "net_unpack_dialog_node");
        TEST_ASSERT(d_node_dec.client_id == 1 && d_node_dec.message_id == 0x0542, "dialog node msg");
        TEST_ASSERT(d_node_dec.num_choices == 3 && d_node_dec.options_bitmask == 0x07, "dialog node choices");
    }

    /* 10. Character Finalization & ACK */
    {
        pkt_c2s_character_finalize_t c_fin_orig;
        pkt_c2s_character_finalize_t c_fin_dec;
        memset(&c_fin_orig, 0, sizeof(c_fin_orig));
        c_fin_orig.client_id = 4;
        snprintf(c_fin_orig.player_name, sizeof(c_fin_orig.player_name), "RoverFan");
        c_fin_orig.gender = 1;
        c_fin_orig.face_type = 3;
        c_fin_orig.clothes_id = 0x2405;
        memcpy(c_fin_orig.auth_token, "e1f9ccb01d394307a52e93b4f65809ef", 32);

        len = net_pack_character_finalize(buf, sizeof(buf), &c_fin_orig);
        TEST_ASSERT(len > 0, "net_pack_character_finalize");
        TEST_ASSERT(net_unpack_character_finalize(buf, len, &c_fin_dec) > 0, "net_unpack_character_finalize");
        TEST_ASSERT(c_fin_dec.client_id == 4 && strcmp(c_fin_dec.player_name, "RoverFan") == 0, "character finalize name");
        TEST_ASSERT(c_fin_dec.gender == 1 && c_fin_dec.face_type == 3 && c_fin_dec.clothes_id == 0x2405, "character finalize appearance");
        TEST_ASSERT(memcmp(c_fin_dec.auth_token, "e1f9ccb01d394307a52e93b4f65809ef", 32) == 0, "character finalize token");

        pkt_s2c_character_ack_t c_ack_orig = { 0, 4, 0, 2 };
        pkt_s2c_character_ack_t c_ack_dec;
        len = net_pack_character_ack(buf, sizeof(buf), &c_ack_orig);
        TEST_ASSERT(len > 0, "net_pack_character_ack");
        TEST_ASSERT(net_unpack_character_ack(buf, len, &c_ack_dec) > 0, "net_unpack_character_ack");
        TEST_ASSERT(c_ack_dec.status == 0 && c_ack_dec.client_id == 4, "character ack status");
        TEST_ASSERT(c_ack_dec.is_resident == 0 && c_ack_dec.slot_idx == 2, "character ack slot");
    }

    /* 11. House Move-In Request & ACK */
    {
        pkt_c2s_movein_req_t m_req_orig;
        pkt_c2s_movein_req_t m_req_dec;
        memset(&m_req_orig, 0, sizeof(m_req_orig));
        m_req_orig.client_id = 5;
        m_req_orig.house_idx = 2;
        memcpy(m_req_orig.auth_token, "0123456789abcdef0123456789abcdef", 32);

        len = net_pack_movein_req(buf, sizeof(buf), &m_req_orig);
        TEST_ASSERT(len > 0, "net_pack_movein_req");
        TEST_ASSERT(net_unpack_movein_req(buf, len, &m_req_dec) > 0, "net_unpack_movein_req");
        TEST_ASSERT(m_req_dec.client_id == 5 && m_req_dec.house_idx == 2, "movein req header");
        TEST_ASSERT(memcmp(m_req_dec.auth_token, "0123456789abcdef0123456789abcdef", 32) == 0, "movein req token");

        pkt_s2c_movein_ack_t m_ack_orig = { NET_MOVEIN_OK, 5, 2, 4, 0 };
        pkt_s2c_movein_ack_t m_ack_dec;
        len = net_pack_movein_ack(buf, sizeof(buf), &m_ack_orig);
        TEST_ASSERT(len > 0, "net_pack_movein_ack");
        TEST_ASSERT(net_unpack_movein_ack(buf, len, &m_ack_dec) > 0, "net_unpack_movein_ack");
        TEST_ASSERT(m_ack_dec.status == NET_MOVEIN_OK && m_ack_dec.client_id == 5, "movein ack status");
        TEST_ASSERT(m_ack_dec.house_idx == 2 && m_ack_dec.resident_slot == 4 && m_ack_dec.roommate_slot == 0, "movein ack slots");
    }

    return 1; /* All tests passed */
}
