/* client_network.h - Client-side network controller and session manager */
#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <enet/enet.h>

#ifdef _WIN32
#undef near
#undef far
#endif

#include "network/net_packets.h"
#include "m_private_h.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLIENT_NET_TIMEOUT_CONNECT_MS 5000
#define CLIENT_NET_TIMEOUT_HEARTBEAT_MS 15000
#define CLIENT_NET_PING_INTERVAL_MS 3000

typedef enum {
    CLIENT_NET_DISCONNECTED = 0,
    CLIENT_NET_CONNECTING,     /* ENet connect in progress */
    CLIENT_NET_HANDSHAKING,    /* Connected to server, sent PKT_C2S_HANDSHAKE, awaiting ACK */
    CLIENT_NET_CONNECTED,      /* Handshake ACK received, active session */
    CLIENT_NET_DISCONNECTING   /* Graceful disconnect in progress */
} ClientNetState;

typedef struct {
    ClientNetState state;
    ENetHost* host;
    ENetPeer* server_peer;
    char server_address[64];
    uint16_t server_port;

    uint16_t client_id;
    uint32_t input_sequence;
    uint32_t connected_at_ms;
    uint32_t last_activity_ms;
    uint32_t last_ping_sent_ms;
    uint32_t ping_rtt_ms;

    /* Handshake acknowledgment world state */
    pkt_s2c_handshake_ack_t server_world_info;
    bool is_new_player;
    bool character_finalized;
    bool is_resident;
    int8_t resident_slot;
    int8_t house_idx;
    int8_t roommate_slot;
} ClientNetworkContext;

extern ClientNetworkContext g_client_net;

/* Initialize client network subsystem */
bool client_network_init(void);

/* Shutdown client network subsystem and disconnect if active */
void client_network_shutdown(void);

/* Initiate connection to server at host:port */
bool client_network_connect(const char* host, uint16_t port);

/* Gracefully disconnect from server */
void client_network_disconnect(uint8_t reason);

/* Poll client network events (non-blocking). Called each game frame tick */
void client_network_poll(void);

/* Check if client is currently in an authenticated/active multiplayer session */
bool client_network_is_connected(void);

/* Get current client network state */
ClientNetState client_network_get_state(void);

/* Get client ID assigned by server */
uint16_t client_network_get_client_id(void);

/* Get current measured round-trip time in milliseconds */
uint32_t client_network_get_ping_rtt(void);

/* Send raw buffer over specified channel */
bool client_network_send_raw(uint8_t channel, const void* data, size_t len, bool reliable);

/* Stream local controller input and player position to server */
bool client_network_send_input(const pkt_c2s_input_t* input);

/* Check if server acknowledged this client as a new player requiring Rover onboarding */
bool client_network_is_new_player(void);

/* Transmit finalized character appearance and name to server upon train arrival */
bool client_network_send_character_finalize(const Private_c* priv);

/* Check if this client is registered as a town resident */
bool client_network_is_resident(void);

/* Get resident slot (0..7) or -1 if visitor */
int8_t client_network_get_resident_slot(void);

/* Get house index (0..3) or -1 if visitor */
int8_t client_network_get_house_idx(void);

/* Get roommate slot (0 or 1) or -1 if visitor */
int8_t client_network_get_roommate_slot(void);

/* Set local resident state after claiming a house */
void client_network_set_local_resident(uint8_t house_idx);

/* Send house move-in request to server */
bool client_network_send_movein_req(uint8_t house_idx);

/* Notify server of door interaction (e.g. entering house door) */
bool client_network_send_door_interact(int house_idx, int is_open);

/* Notify server of scene/zone change */
bool client_network_send_zone_change(int scene_id, int room_idx);

/* Self-test suite verifying client connection, handshake, ping/pong, input streaming, and disconnect */
int client_network_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_NETWORK_H */
