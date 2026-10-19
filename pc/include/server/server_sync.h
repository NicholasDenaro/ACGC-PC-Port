/* server_sync.h - High-frequency transform streaming and entity synchronization */
#ifndef SERVER_SYNC_H
#define SERVER_SYNC_H

#include "types.h"
#include "network/net_packets.h"
#include "m_play.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the server transform sync subsystem */
void server_sync_init(void);

/* Handle high-frequency controller input / transform packet from a client */
void server_sync_on_client_input(uint32_t client_id, const pkt_c2s_input_t* input);

/* Server tick: evaluate spatial interest and broadcast entity sync packets to active clients */
void server_sync_tick(GAME_PLAY* play);

/* Run unit tests verifying entity packing, rate-limiting, and spatial interest filtering */
bool server_sync_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_SYNC_H */
