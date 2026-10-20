/* server_world.c - Authoritative Foreground Item (Save.fg) Mutation & Sync */
#include "server/server_world.h"
#include "server/server_network.h"
#include "server/server_player.h"
#include "server/server_storage.h"
#include "client/client_network.h"
#include "m_common_data.h"
#include "m_field_info.h"
#include "m_name_table.h"
#include "m_scene_table.h"
#include "m_collision_bg.h"
#include "ac_effectbg.h"
#include "libultra/libultra.h"

#include <stdio.h>
#include <string.h>

/* Re-entrancy guard to prevent incoming PKT_S2C_FG_DELTA from echoing PKT_C2S_INTERACT_REQ */
static int s_applying_remote_fg_delta = 0;

/* Full-town Save.fg + Save.deposit snapshot cache on the client so mFM_FieldInit overlays
 * the server's authoritative foreground even after the client loads its local .gci file. */
#define FG_SNAPSHOT_HEADER_BYTES  6
#define FG_SNAPSHOT_FG_WORDS      (FG_BLOCK_Z_NUM * FG_BLOCK_X_NUM * UT_Z_NUM * UT_X_NUM)
#define FG_SNAPSHOT_DEPOSIT_WORDS (FG_BLOCK_TOTAL_NUM * UT_Z_NUM)
#define FG_SNAPSHOT_PAYLOAD_BYTES ((FG_SNAPSHOT_FG_WORDS + FG_SNAPSHOT_DEPOSIT_WORDS) * sizeof(u16))

static inline void fg_write_u16(u8* buf, size_t* offset, u16 val) {
    buf[(*offset)++] = (u8)((val >> 8) & 0xFF);
    buf[(*offset)++] = (u8)(val & 0xFF);
}

static inline u16 fg_read_u16(const u8* buf, size_t* offset) {
    u16 hi = buf[(*offset)++];
    u16 lo = buf[(*offset)++];
    return (u16)((hi << 8) | lo);
}

static bool s_has_cached_server_fg = false;
static u16  s_cached_server_fg[FG_BLOCK_Z_NUM][FG_BLOCK_X_NUM][UT_Z_NUM][UT_X_NUM];
static u16  s_cached_server_deposit[FG_BLOCK_TOTAL_NUM][UT_Z_NUM];

/* Track original item on tiles temporarily reserved with RSV_NO (0xFFFF) during dig/bury/drop animations */
#define RSV_CACHE_SIZE 32
typedef struct {
    u8  active;
    u8  bx;
    u8  bz;
    u8  ut_x;
    u8  ut_z;
    u16 pre_rsv_item;
} RsvTileCacheEntry;

static RsvTileCacheEntry s_rsv_cache[RSV_CACHE_SIZE];
static int s_rsv_cache_next = 0;

static void rsv_cache_record(int bx, int bz, int ut_x, int ut_z, u16 old_item) {
    if (old_item == RSV_NO) return;
    for (int i = 0; i < RSV_CACHE_SIZE; i++) {
        if (s_rsv_cache[i].active &&
            s_rsv_cache[i].bx == (u8)bx &&
            s_rsv_cache[i].bz == (u8)bz &&
            s_rsv_cache[i].ut_x == (u8)ut_x &&
            s_rsv_cache[i].ut_z == (u8)ut_z) {
            s_rsv_cache[i].pre_rsv_item = old_item;
            return;
        }
    }
    RsvTileCacheEntry* e = &s_rsv_cache[s_rsv_cache_next];
    s_rsv_cache_next = (s_rsv_cache_next + 1) % RSV_CACHE_SIZE;
    e->active = 1;
    e->bx = (u8)bx;
    e->bz = (u8)bz;
    e->ut_x = (u8)ut_x;
    e->ut_z = (u8)ut_z;
    e->pre_rsv_item = old_item;
}

static u16 rsv_cache_consume(int bx, int bz, int ut_x, int ut_z) {
    for (int i = 0; i < RSV_CACHE_SIZE; i++) {
        if (s_rsv_cache[i].active &&
            s_rsv_cache[i].bx == (u8)bx &&
            s_rsv_cache[i].bz == (u8)bz &&
            s_rsv_cache[i].ut_x == (u8)ut_x &&
            s_rsv_cache[i].ut_z == (u8)ut_z) {
            s_rsv_cache[i].active = 0;
            return s_rsv_cache[i].pre_rsv_item;
        }
    }
    return EMPTY_NO;
}

/* Returns true if item is a valid foreground item (scenery, furniture, obtainable item, misc item)
 * and NOT a runtime structure/actor/system dummy placeholder (0xF000+, NAME_TYPE_STRUCT, etc.). */
static bool is_syncable_fg_item(u16 item) {
    if (item == RSV_NO || item == RSV_WALL_NO || item >= DUMMY_START) {
        return false;
    }
    u16 type = ITEM_NAME_GET_TYPE(item);
    switch (type) {
        case NAME_TYPE_ITEM0:
        case NAME_TYPE_FTR0:
        case NAME_TYPE_ITEM1:
        case NAME_TYPE_FTR1:
        case NAME_TYPE_ITEM2:
            return true;
        default:
            return false;
    }
}

static u8 classify_fg_action(u16 old_item, u16 new_item) {
    if ((new_item >= HOLE_START && new_item <= HOLE_END) || new_item == HOLE_SHINE) {
        return NET_INTERACT_DIG;
    }
    if (mNT_ItIsStump(new_item)) {
        return NET_INTERACT_CUT_TREE;
    }
    if (new_item == EMPTY_NO) {
        if ((old_item >= HOLE_START && old_item <= HOLE_END) || old_item == HOLE_SHINE) {
            return NET_INTERACT_FILL_HOLE;
        }
        return NET_INTERACT_PICK_ITEM;
    }
    if (IS_ITEM_TREE(old_item) && IS_ITEM_TREE(new_item)) {
        return NET_INTERACT_SHAKE_TREE;
    }
    return NET_INTERACT_DROP_ITEM;
}

static int get_block_deposit_bit(int bx, int bz, int ut_x, int ut_z) {
    if (ut_x < 0 || ut_x >= UT_X_NUM || ut_z < 0 || ut_z >= UT_Z_NUM) {
        return 0;
    }
    u16* deposit_p = mFI_GetDepositP(bx, bz);
    if (deposit_p != NULL) {
        return (deposit_p[ut_z] >> ut_x) & 1;
    }
    if (bx >= 1 && bx <= FG_BLOCK_X_NUM && bz >= 1 && bz <= FG_BLOCK_Z_NUM) {
        u16* save_dep = Save_Get(deposit[(bx - 1) + (bz - 1) * FG_BLOCK_X_NUM]);
        return (save_dep[ut_z] >> ut_x) & 1;
    }
    return 0;
}

static void set_block_deposit_bit(int bx, int bz, int ut_x, int ut_z, int deposit_on) {
    if (ut_x < 0 || ut_x >= UT_X_NUM || ut_z < 0 || ut_z >= UT_Z_NUM) {
        return;
    }
    if (bx >= 1 && bx <= FG_BLOCK_X_NUM && bz >= 1 && bz <= FG_BLOCK_Z_NUM) {
        int blk_idx = (bx - 1) + (bz - 1) * FG_BLOCK_X_NUM;
        u16* save_dep = Save_Get(deposit[blk_idx]);
        if (deposit_on) {
            save_dep[ut_z] |= (u16)(1u << ut_x);
        } else {
            save_dep[ut_z] &= (u16)~(1u << ut_x);
        }
        if (s_has_cached_server_fg) {
            if (deposit_on) {
                s_cached_server_deposit[blk_idx][ut_z] |= (u16)(1u << ut_x);
            } else {
                s_cached_server_deposit[blk_idx][ut_z] &= (u16)~(1u << ut_x);
            }
        }
    }
    u16* deposit_p = mFI_GetDepositP(bx, bz);
    if (deposit_p != NULL) {
        if (deposit_on) {
            deposit_p[ut_z] |= (u16)(1u << ut_x);
        } else {
            deposit_p[ut_z] &= (u16)~(1u << ut_x);
        }
    }
}

static void apply_cached_fg_snapshot(void) {
    if (!s_has_cached_server_fg) {
        return;
    }

    s_applying_remote_fg_delta = 1;

    for (int bz = 0; bz < FG_BLOCK_Z_NUM; bz++) {
        for (int bx = 0; bx < FG_BLOCK_X_NUM; bx++) {
            int blk_idx = bx + bz * FG_BLOCK_X_NUM;
            u16* save_dep = Save_Get(deposit[blk_idx]);
            for (int uz = 0; uz < UT_Z_NUM; uz++) {
                for (int ux = 0; ux < UT_X_NUM; ux++) {
                    u16 srv_item = s_cached_server_fg[bz][bx][uz][ux];
                    u16 cli_item = Save_Get(fg[bz][bx]).items[uz][ux];
                    if (is_syncable_fg_item(srv_item) && is_syncable_fg_item(cli_item)) {
                        Save_Get(fg[bz][bx]).items[uz][ux] = srv_item;
                        int dep_bit = (s_cached_server_deposit[blk_idx][uz] >> ux) & 1;
                        if (dep_bit) {
                            save_dep[uz] |= (u16)(1u << ux);
                        } else {
                            save_dep[uz] &= (u16)~(1u << ux);
                        }
                    }
                }
            }
        }
    }

    if (mFI_CheckFieldData() == TRUE && mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELD_FG) {
        mFI_SetFGUpData();
    }

    s_applying_remote_fg_delta = 0;
}

void server_world_on_field_init(int scene) {
#ifndef TARGET_SERVER
    if (scene == SCENE_FG && client_network_is_connected() && s_has_cached_server_fg) {
        apply_cached_fg_snapshot();
    }
#else
    (void)scene;
#endif
}

void server_world_send_initial_sync(u16 client_id) {
    static u8 s_snap_buf[FG_SNAPSHOT_HEADER_BYTES + FG_SNAPSHOT_PAYLOAD_BYTES];
    net_header_t hdr;
    hdr.magic = NET_PACKET_MAGIC;
    hdr.packet_id = PKT_S2C_FG_DELTA;
    hdr.channel = NET_CH_RELIABLE;
    hdr.payload_size = (u16)FG_SNAPSHOT_PAYLOAD_BYTES;

    if (net_pack_header(s_snap_buf, sizeof(s_snap_buf), &hdr) < 0) {
        return;
    }

    size_t offset = FG_SNAPSHOT_HEADER_BYTES;
    for (int bz = 0; bz < FG_BLOCK_Z_NUM; bz++) {
        for (int bx = 0; bx < FG_BLOCK_X_NUM; bx++) {
            for (int uz = 0; uz < UT_Z_NUM; uz++) {
                for (int ux = 0; ux < UT_X_NUM; ux++) {
                    fg_write_u16(s_snap_buf, &offset, Save_Get(fg[bz][bx]).items[uz][ux]);
                }
            }
        }
    }
    for (int b = 0; b < FG_BLOCK_TOTAL_NUM; b++) {
        u16* dep = Save_Get(deposit[b]);
        for (int uz = 0; uz < UT_Z_NUM; uz++) {
            fg_write_u16(s_snap_buf, &offset, dep[uz]);
        }
    }

    server_network_send_raw(client_id, NET_CH_RELIABLE, s_snap_buf, offset, true);
}

void server_world_notify_local_fg_change(int bx, int bz, int ut_x, int ut_z, u16 old_item, u16 new_item) {
    if (s_applying_remote_fg_delta) {
        return;
    }

    /* Validate outdoor town block coordinates (1..FG_BLOCK_X_NUM, 1..FG_BLOCK_Z_NUM) */
    if (bx < 1 || bx > FG_BLOCK_X_NUM || bz < 1 || bz > FG_BLOCK_Z_NUM) {
        return;
    }
    if (ut_x < 0 || ut_x >= UT_X_NUM || ut_z < 0 || ut_z >= UT_Z_NUM) {
        return;
    }

    /* Only sync during active outdoor gameplay (not title demos) */
    int scene = Save_Get(scene_no);
    if (scene != SCENE_FG) {
        return;
    }

    /* Record RSV_NO temporary reservation and wait for the committed item write */
    if (new_item == RSV_NO) {
        rsv_cache_record(bx, bz, ut_x, ut_z, old_item);
        return;
    }

    /* Recover original item if tile was temporarily reserved with RSV_NO */
    if (old_item == RSV_NO) {
        old_item = rsv_cache_consume(bx, bz, ut_x, ut_z);
    }

    /* Ignore runtime structure/actor/dummy placeholders */
    if (!is_syncable_fg_item(new_item) || !is_syncable_fg_item(old_item)) {
        return;
    }

    /* Ignore no-op transitions (e.g. NPC house doorstep EMPTY_NO -> RSV_NO -> EMPTY_NO) */
    if (old_item == new_item) {
        return;
    }

    int deposit = get_block_deposit_bit(bx, bz, ut_x, ut_z);
    if (s_has_cached_server_fg) {
        s_cached_server_fg[bz - 1][bx - 1][ut_z][ut_x] = new_item;
    }

#ifndef TARGET_SERVER
    if (client_network_is_connected()) {
        pkt_c2s_interact_req_t req;
        memset(&req, 0, sizeof(req));
        req.client_id = client_network_get_client_id();
        req.action_type = classify_fg_action(old_item, new_item);
        req.block_x = (s8)bx;
        req.block_z = (s8)bz;
        req.ut_x = (u8)(ut_x & FG_DELTA_UT_MASK);
        req.ut_z = (u8)((ut_z & FG_DELTA_UT_MASK) | (deposit ? FG_DELTA_DEPOSIT_FLAG : 0));
        req.target_actor_id = (u32)old_item | (deposit ? 0x10000u : 0u);
        req.item_arg = new_item;

        u8 buf[64];
        int len = net_pack_interact_req(buf, sizeof(buf), &req);
        if (len > 0) {
            client_network_send_raw(NET_CH_RELIABLE, buf, (size_t)len, true);
        }
    }
#else
    /* Server-originated FG change (e.g. mushrooms, events, daily growth while clients connected) */
    server_storage_mark_dirty();
    if (server_network_get_authenticated_count() > 0) {
        pkt_s2c_fg_delta_t delta;
        delta.zone_id = 0;
        delta.block_x = (u8)bx;
        delta.block_z = (u8)bz;
        delta.ut_x = (u8)(ut_x & FG_DELTA_UT_MASK);
        delta.ut_z = (u8)((ut_z & FG_DELTA_UT_MASK) | (deposit ? FG_DELTA_DEPOSIT_FLAG : 0));
        delta.old_item = old_item;
        delta.new_item = new_item;

        u8 buf[32];
        int len = net_pack_fg_delta(buf, sizeof(buf), &delta);
        if (len > 0) {
            server_network_broadcast_raw(NET_CH_RELIABLE, buf, (size_t)len, true, 0);
        }
    }
#endif
}

void server_world_notify_local_deposit_change(int world_ut_x, int world_ut_z, int deposit_on) {
    if (s_applying_remote_fg_delta || !deposit_on) {
        return;
    }

    int scene = Save_Get(scene_no);
    if (scene != SCENE_FG) {
        return;
    }

    if (world_ut_x < 0 || world_ut_z < 0) {
        return;
    }

    int bx = world_ut_x / UT_X_NUM;
    int bz = world_ut_z / UT_Z_NUM;
    int b_ut_x = world_ut_x & (UT_X_NUM - 1);
    int b_ut_z = world_ut_z & (UT_Z_NUM - 1);

    if (bx < 1 || bx > FG_BLOCK_X_NUM || bz < 1 || bz > FG_BLOCK_Z_NUM) {
        return;
    }

    u16 cur_item = Save_Get(fg[bz - 1][bx - 1]).items[b_ut_z][b_ut_x];
    if (cur_item == EMPTY_NO || !is_syncable_fg_item(cur_item)) {
        return;
    }

    if (s_has_cached_server_fg) {
        int blk_idx = (bx - 1) + (bz - 1) * FG_BLOCK_X_NUM;
        s_cached_server_fg[bz - 1][bx - 1][b_ut_z][b_ut_x] = cur_item;
        s_cached_server_deposit[blk_idx][b_ut_z] |= (u16)(1u << b_ut_x);
    }

#ifndef TARGET_SERVER
    if (client_network_is_connected()) {
        pkt_c2s_interact_req_t req;
        memset(&req, 0, sizeof(req));
        req.client_id = client_network_get_client_id();
        req.action_type = NET_INTERACT_DROP_ITEM;
        req.block_x = (s8)bx;
        req.block_z = (s8)bz;
        req.ut_x = (u8)(b_ut_x & FG_DELTA_UT_MASK);
        req.ut_z = (u8)((b_ut_z & FG_DELTA_UT_MASK) | FG_DELTA_DEPOSIT_FLAG);
        req.target_actor_id = (u32)HOLE00 | 0x10000u;
        req.item_arg = cur_item;

        u8 buf[64];
        int len = net_pack_interact_req(buf, sizeof(buf), &req);
        if (len > 0) {
            client_network_send_raw(NET_CH_RELIABLE, buf, (size_t)len, true);
        }
    }
#endif
}

bool server_world_apply_fg_mutation(u16 zone_id, int bx, int bz, int ut_x, int ut_z,
                                    u16 new_item, int deposit, u16* out_old_item) {
    (void)zone_id;
    if (bx < 1 || bx > FG_BLOCK_X_NUM || bz < 1 || bz > FG_BLOCK_Z_NUM) {
        return false;
    }
    if (ut_x < 0 || ut_x >= UT_X_NUM || ut_z < 0 || ut_z >= UT_Z_NUM) {
        return false;
    }
    if (!is_syncable_fg_item(new_item)) {
        return false;
    }

    u16 prev_item = Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x];
    if (out_old_item != NULL) {
        *out_old_item = prev_item;
    }

    s_applying_remote_fg_delta = 1;

    Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] = new_item;
    set_block_deposit_bit(bx, bz, ut_x, ut_z, deposit);

    if (mFI_CheckFieldData() == TRUE && mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELD_FG) {
        mFI_BlockUtNumtoFGSet(new_item, bx, bz, ut_x, ut_z);
        mFI_SetFGUpData();
    }

    s_applying_remote_fg_delta = 0;

    server_storage_mark_dirty();
    return true;
}

void server_world_handle_fg_interact(u16 client_id, const pkt_c2s_interact_req_t* req) {
    if (req == NULL) return;

    int bx = (int)req->block_x;
    int bz = (int)req->block_z;
    int ut_x = (int)(req->ut_x & FG_DELTA_UT_MASK);
    int ut_z = (int)(req->ut_z & FG_DELTA_UT_MASK);
    int deposit = ((req->ut_z & FG_DELTA_DEPOSIT_FLAG) != 0) || ((req->target_actor_id & 0x10000u) != 0);
    u16 new_item = req->item_arg;

    u16 zone_id = 0;
    ConnectedClient* cc = server_player_get(client_id);
    if (cc != NULL) {
        zone_id = (u16)cc->zone_id;
    }

    u16 prev_item = EMPTY_NO;
    if (!server_world_apply_fg_mutation(zone_id, bx, bz, ut_x, ut_z, new_item, deposit, &prev_item)) {
        return;
    }

    u16 reported_old = (prev_item != EMPTY_NO && is_syncable_fg_item(prev_item))
                           ? prev_item
                           : (u16)(req->target_actor_id & 0xFFFFu);

    pkt_s2c_fg_delta_t delta;
    delta.zone_id = zone_id;
    delta.block_x = (u8)bx;
    delta.block_z = (u8)bz;
    delta.ut_x = (u8)ut_x;
    delta.ut_z = (u8)(ut_z | (deposit ? FG_DELTA_DEPOSIT_FLAG : 0));
    delta.old_item = reported_old;
    delta.new_item = new_item;

    u8 buf[32];
    int len = net_pack_fg_delta(buf, sizeof(buf), &delta);
    if (len > 0) {
        server_network_broadcast_raw(NET_CH_RELIABLE, buf, (size_t)len, true, 0);
    }

    printf("[SERVER WORLD] Client %u FG mutation (action %u): block(%d,%d) ut(%d,%d) 0x%04X -> 0x%04X%s\n",
           client_id, req->action_type, bx, bz, ut_x, ut_z, reported_old, new_item,
           deposit ? " [BURIED]" : "");
}

void server_world_handle_fg_packet(const u8* data, size_t len) {
    if (data == NULL || len < FG_SNAPSHOT_HEADER_BYTES) {
        return;
    }

    net_header_t hdr;
    if (net_unpack_header(data, len, &hdr) < 0 || hdr.packet_id != PKT_S2C_FG_DELTA) {
        return;
    }

    /* Check if this is a full-town Save.fg + Save.deposit snapshot */
    if (hdr.payload_size == FG_SNAPSHOT_PAYLOAD_BYTES &&
        len >= FG_SNAPSHOT_HEADER_BYTES + FG_SNAPSHOT_PAYLOAD_BYTES) {
        size_t offset = FG_SNAPSHOT_HEADER_BYTES;
        for (int bz = 0; bz < FG_BLOCK_Z_NUM; bz++) {
            for (int bx = 0; bx < FG_BLOCK_X_NUM; bx++) {
                for (int uz = 0; uz < UT_Z_NUM; uz++) {
                    for (int ux = 0; ux < UT_X_NUM; ux++) {
                        s_cached_server_fg[bz][bx][uz][ux] = fg_read_u16(data, &offset);
                    }
                }
            }
        }
        for (int b = 0; b < FG_BLOCK_TOTAL_NUM; b++) {
            for (int uz = 0; uz < UT_Z_NUM; uz++) {
                s_cached_server_deposit[b][uz] = fg_read_u16(data, &offset);
            }
        }
        s_has_cached_server_fg = true;
        apply_cached_fg_snapshot();
        return;
    }

    /* Otherwise unpack single-tile foreground delta */
    pkt_s2c_fg_delta_t delta;
    if (net_unpack_fg_delta(data, len, &delta) > 0) {
        server_world_apply_client_fg_delta(&delta);
    }
}

void server_world_apply_client_fg_delta(const pkt_s2c_fg_delta_t* delta) {
    if (delta == NULL) return;

    int bx = (int)delta->block_x;
    int bz = (int)delta->block_z;
    int ut_x = (int)(delta->ut_x & FG_DELTA_UT_MASK);
    int ut_z = (int)(delta->ut_z & FG_DELTA_UT_MASK);
    int deposit = (delta->ut_z & FG_DELTA_DEPOSIT_FLAG) != 0;
    u16 new_item = delta->new_item;

    if (bx < 1 || bx > FG_BLOCK_X_NUM || bz < 1 || bz > FG_BLOCK_Z_NUM) {
        return;
    }
    if (ut_x < 0 || ut_x >= UT_X_NUM || ut_z < 0 || ut_z >= UT_Z_NUM) {
        return;
    }
    if (!is_syncable_fg_item(new_item)) {
        return;
    }

    s_applying_remote_fg_delta = 1;

    u16 local_item = Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x];
    Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] = new_item;
    if (s_has_cached_server_fg) {
        s_cached_server_fg[bz - 1][bx - 1][ut_z][ut_x] = new_item;
    }
    set_block_deposit_bit(bx, bz, ut_x, ut_z, deposit);

    if (mFI_CheckFieldData() == TRUE && mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELD_FG) {
        mActor_name_t* fg_p = mFI_BkNumtoUtFGTop(bx, bz);
        if (fg_p != NULL) {
            local_item = fg_p[(ut_z << 4) | ut_x];
        }

        mFI_BlockUtNumtoFGSet(new_item, bx, bz, ut_x, ut_z);

        /* If a remote player chopped down a tree into a stump, update collision offset and play tree fall effect */
        if (local_item != new_item && mNT_ItIsStump(new_item)) {
            int world_ut_x = bx * UT_X_NUM + ut_x;
            int world_ut_z = bz * UT_Z_NUM + ut_z;
            xyz_t wpos;
            if (mFI_UtNum2CenterWpos(&wpos, world_ut_x, world_ut_z)) {
                mNT_offset_table_c* ofs_tbl = obj_hight_table_item0_nogrow(new_item);
                if (ofs_tbl != NULL) {
                    mCoBG_SetPlussOffset(wpos, ofs_tbl->table.centerRight_offset, ofs_tbl->table.unit_attribute);
                }
#ifndef TARGET_SERVER
                if (gamePT != NULL && Common_Get(clip).make_effect_bg_proc != NULL && IS_ITEM_TREE(delta->old_item)) {
                    wpos.y = mCoBG_UtNum2UtCenterY_Keep(world_ut_x, world_ut_z);
                    Common_Get(clip).make_effect_bg_proc(gamePT, EffectBG_EFFECT_CUT_RIGHT, EffectBG_VARIANT_TREE_FULL, &wpos);
                }
#endif
            }
        }

        mFI_SetFGUpData();
    }

    s_applying_remote_fg_delta = 0;
}

/* --- Self-Tests --- */

#define TEST_WORLD_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[SERVER WORLD TEST FAILED] %s: line %d\n", msg, __LINE__); \
            return 0; \
        } \
    } while (0)

int server_world_run_tests(void) {
    printf("[SERVER WORLD TEST] Running authoritative foreground mutation tests...\n");

    const int bx = 2;
    const int bz = 3;
    const int ut_x = 5;
    const int ut_z = 7;

    u16 orig_item = Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x];
    int orig_dep = get_block_deposit_bit(bx, bz, ut_x, ut_z);

    /* 1. Digging a hole */
    Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] = EMPTY_NO;
    set_block_deposit_bit(bx, bz, ut_x, ut_z, 0);

    pkt_c2s_interact_req_t req;
    memset(&req, 0, sizeof(req));
    req.client_id = 1;
    req.action_type = NET_INTERACT_DIG;
    req.block_x = (s8)bx;
    req.block_z = (s8)bz;
    req.ut_x = (u8)ut_x;
    req.ut_z = (u8)ut_z;
    req.target_actor_id = EMPTY_NO;
    req.item_arg = HOLE00;

    server_world_handle_fg_interact(1, &req);
    TEST_WORLD_ASSERT(Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] == HOLE00, "dig hole sets Save.fg to HOLE00");
    TEST_WORLD_ASSERT(get_block_deposit_bit(bx, bz, ut_x, ut_z) == 0, "dig hole deposit bit is 0");

    /* 2. Burying an item in the hole */
    req.action_type = NET_INTERACT_DROP_ITEM;
    req.ut_z = (u8)(ut_z | FG_DELTA_DEPOSIT_FLAG);
    req.target_actor_id = (u32)HOLE00 | 0x10000u;
    req.item_arg = ITM_PITFALL;
    server_world_handle_fg_interact(1, &req);
    TEST_WORLD_ASSERT(Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] == ITM_PITFALL, "bury item sets Save.fg to ITM_PITFALL");
    TEST_WORLD_ASSERT(get_block_deposit_bit(bx, bz, ut_x, ut_z) == 1, "bury item sets deposit bit to 1");

    /* 3. Client applying PKT_S2C_FG_DELTA */
    Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] = EMPTY_NO;
    set_block_deposit_bit(bx, bz, ut_x, ut_z, 0);

    pkt_s2c_fg_delta_t delta;
    delta.zone_id = 0;
    delta.block_x = (u8)bx;
    delta.block_z = (u8)bz;
    delta.ut_x = (u8)ut_x;
    delta.ut_z = (u8)(ut_z | FG_DELTA_DEPOSIT_FLAG);
    delta.old_item = HOLE00;
    delta.new_item = ITM_MONEY_1000;

    server_world_apply_client_fg_delta(&delta);
    TEST_WORLD_ASSERT(Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] == ITM_MONEY_1000, "client delta sets ITM_MONEY_1000");
    TEST_WORLD_ASSERT(get_block_deposit_bit(bx, bz, ut_x, ut_z) == 1, "client delta sets deposit bit to 1");

    /* 4. Chopping a tree into a stump clears deposit and sets TREE_STUMP004 */
    delta.ut_z = (u8)ut_z;
    delta.old_item = TREE;
    delta.new_item = TREE_STUMP004;
    server_world_apply_client_fg_delta(&delta);
    TEST_WORLD_ASSERT(Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] == TREE_STUMP004, "client delta sets TREE_STUMP004");
    TEST_WORLD_ASSERT(get_block_deposit_bit(bx, bz, ut_x, ut_z) == 0, "client delta clears deposit bit");

    /* Restore original tile state */
    Save_Get(fg[bz - 1][bx - 1]).items[ut_z][ut_x] = orig_item;
    set_block_deposit_bit(bx, bz, ut_x, ut_z, orig_dep);

    printf("[SERVER WORLD TEST] All authoritative foreground mutation tests passed!\n");
    return 1;
}
