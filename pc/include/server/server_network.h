/* server_network.h - ENet server listener and client session management */
#ifndef SERVER_NETWORK_H
#define SERVER_NETWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <enet/enet.h>

#ifdef _WIN32
#undef near
#undef far
#endif

#include "network/net_packets.h"
#include "server/server_player.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_NET_MAX_CLIENTS MAX_SERVER_CLIENTS
#define SERVER_NET_TIMEOUT_HANDSHAKE_MS 5000
#define SERVER_NET_TIMEOUT_HEARTBEAT_MS 15000
#define SERVER_NET_PING_INTERVAL_MS 3000

typedef enum {
    NET_SESSION_DISCONNECTED = 0,
    NET_SESSION_CONNECTING,     /* ENet peer connected, awaiting PKT_C2S_HANDSHAKE */
    NET_SESSION_AUTHENTICATED,  /* Handshake accepted, ACK sent, registered with server_player */
    NET_SESSION_ACTIVE          /* Active gameplay state */
} NetSessionState;

typedef struct {
    uint32_t client_id;
    NetSessionState state;
    ENetPeer* peer;
    uint32_t connected_at_ms;
    uint32_t last_activity_ms;
    uint32_t last_ping_sent_ms;
    uint32_t ping_rtt_ms;

    char player_name[NET_MAX_NAME_LEN];
    char town_name[NET_MAX_NAME_LEN];
    uint16_t player_id;
    uint16_t land_id;
    uint8_t  gender;
    uint8_t  face_type;
    uint16_t clothes_id;
    int player_slot;            /* Index in g_server_clients, or -1 if unassigned */
} ServerClientSession;

/* Initialize ENet server listener host on given port (or SERVER_DEFAULT_PORT if 0) */
bool server_network_init(uint16_t port);

/* Cleanly shutdown server host and disconnect all peers */
void server_network_shutdown(void);

/* Check if server network host is listening */
bool server_network_is_running(void);

/* Poll network events (non-blocking). To be called each server tick */
void server_network_poll(void);

/* Send raw packet buffer to a specific client */
bool server_network_send_raw(uint32_t client_id, uint8_t channel, const void* data, size_t len, bool reliable);

/* Broadcast packet buffer to all authenticated clients (optionally excluding one) */
void server_network_broadcast_raw(uint8_t channel, const void* data, size_t len, bool reliable, uint32_t exclude_client_id);

/* Broadcast packet buffer filtered by spatial interest (3x3 acre subscription for outdoors, zone_id for indoors) */
void server_network_broadcast_spatial(uint32_t zone_id, int block_x, int block_z, uint8_t channel, const void* data, size_t len, bool reliable, uint32_t exclude_client_id);
void server_network_broadcast_spatial_wpos(uint32_t zone_id, float world_x, float world_z, uint8_t channel, const void* data, size_t len, bool reliable, uint32_t exclude_client_id);

/* Disconnect a specific client */
void server_network_disconnect_client(uint32_t client_id, uint8_t reason);

/* Lookup session by client_id */
ServerClientSession* server_network_get_session(uint32_t client_id);

/* Lookup session by peer pointer */
ServerClientSession* server_network_get_session_by_peer(ENetPeer* peer);

/* Count of currently authenticated client sessions */
int server_network_get_authenticated_count(void);

/* Self-test suite verifying host creation, client connect, handshake, ping/pong, and disconnect */
int server_network_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_NETWORK_H */
