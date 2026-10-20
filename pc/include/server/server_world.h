/* server_world.h - Authoritative Foreground Item (Save.fg) Mutation & Sync */
#ifndef SERVER_WORLD_H
#define SERVER_WORLD_H

#include "types.h"
#include "network/net_packets.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode/decode buried deposit flag in high bit of ut_z (ut_z is 0..15) */
#define FG_DELTA_UT_MASK      0x0F
#define FG_DELTA_DEPOSIT_FLAG 0x80

/* Client/Engine hook: called in mFM_FieldInit before mFM_MakeField to overlay server Save.fg */
void server_world_on_field_init(int scene);

/* Client/Engine hook: called when mFI_UtNumtoFGSet_common or mFI_BlockUtNumtoFGSet modifies a tile */
void server_world_notify_local_fg_change(int bx, int bz, int ut_x, int ut_z, u16 old_item, u16 new_item);

/* Client/Engine hook: called when mFI_UtNum2DepositON/OFF modifies a tile's buried deposit bit */
void server_world_notify_local_deposit_change(int world_ut_x, int world_ut_z, int deposit_on);

/* Server-side: send full authoritative Save.fg + Save.deposit snapshot to a newly authenticated client */
void server_world_send_initial_sync(u16 client_id);

/* Server-side: authoritatively mutate Save.fg and Save.deposit and mark storage dirty */
bool server_world_apply_fg_mutation(u16 zone_id, int bx, int bz, int ut_x, int ut_z,
                                    u16 new_item, int deposit, u16* out_old_item);

/* Server-side: handle incoming PKT_C2S_INTERACT_REQ for foreground mutations and broadcast PKT_S2C_FG_DELTA */
void server_world_handle_fg_interact(u16 client_id, const pkt_c2s_interact_req_t* req);

/* Client-side: apply incoming PKT_S2C_FG_DELTA (single tile or full snapshot) from server */
void server_world_handle_fg_packet(const u8* data, size_t len);
void server_world_apply_client_fg_delta(const pkt_s2c_fg_delta_t* delta);

/* --- Economy & Shop Synchronization (Task 7.4) --- */

/* Client/Engine hook: restore cached server Shop_c state into Save_Get(shop) */
void server_world_reapply_cached_shop(void);

/* Client/Engine hook: notify server when local player purchases an item from Nook's shop floor */
void server_world_notify_shop_purchase(int ux, int uz, u16 sold_item, u16 sold_rsv_item, u32 price);

/* Client/Engine hook: notify server when local player sells items to Nook or orders from catalog */
void server_world_notify_shop_plus_sales(u32 sum);

/* Client/Engine hook: notify server when local player wins a monthly lottery item */
void server_world_notify_shop_lottery(int lottery_idx, u16 sold_item);

/* Server-side: send or broadcast authoritative Shop_c state (client_id == 0 broadcasts to all) */
void server_world_send_shop_sync(u16 client_id, int has_sale_event, int ut_x, int ut_z,
                                 u16 sold_item, u16 sold_rsv_item);

/* Client-side: handle incoming PKT_S2C_SHOP_SYNC from server */
void server_world_handle_shop_sync_packet(const u8* data, size_t len);

/* Self-test suite for authoritative foreground mutations and delta broadcast */
int server_world_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_WORLD_H */
