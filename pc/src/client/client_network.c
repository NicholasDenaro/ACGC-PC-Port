/* client_network.c - Client-side network controller and session manager */
#include "client/client_network.h"
#include "client/client_identity.h"
#include "server/server_network.h"
#include "network/net_packets.h"
#include "m_common_data.h"
#include "m_name_table.h"
#include "m_private.h"
#include "m_land.h"
#include "m_font.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ClientNetworkContext g_client_net = {0};

bool client_network_init(void) {
    if (g_client_net.host != NULL) {
        return true;
    }

    /* Initialize persistent client identity token */
    client_identity_init();

    g_client_net.host = enet_host_create(NULL, 1, NET_CH_COUNT, 0, 0);
    if (g_client_net.host == NULL) {
        fprintf(stderr, "[CLIENT NET ERROR] Failed to create ENet client host.\n");
        return false;
    }

    g_client_net.state = CLIENT_NET_DISCONNECTED;
    g_client_net.server_peer = NULL;
    g_client_net.client_id = 0;
    g_client_net.input_sequence = 0;
    g_client_net.ping_rtt_ms = 0;

    printf("[CLIENT NET] Client network subsystem initialized.\n");
    return true;
}

void client_network_shutdown(void) {
    if (g_client_net.host == NULL) {
        return;
    }

    if (g_client_net.server_peer != NULL) {
        client_network_disconnect(0);
    }

    enet_host_destroy(g_client_net.host);
    memset(&g_client_net, 0, sizeof(g_client_net));
    printf("[CLIENT NET] Client network subsystem shut down.\n");
}

static void client_network_send_handshake(void) {
    pkt_c2s_handshake_t hs;
    memset(&hs, 0, sizeof(hs));
    hs.client_version = 1;
    hs.protocol_version = NET_PROTOCOL_VERSION;
    client_identity_get_token((uint8_t*)hs.auth_token);

    Private_c* priv = Common_Get(now_private);
    if (priv != NULL && priv->player_ID.player_name[0] != CHAR_SPACE && priv->player_ID.player_name[0] != 0) {
        mPr_CopyPlayerName((u8*)hs.player_name, priv->player_ID.player_name);
        mLd_CopyLandName((u8*)hs.town_name, priv->player_ID.land_name);
        hs.player_id = priv->player_ID.player_id;
        hs.land_id = priv->player_ID.land_id;
        hs.gender = priv->gender;
        hs.face_type = priv->face;
        hs.clothes_id = priv->cloth.item;
    } else {
        snprintf(hs.player_name, sizeof(hs.player_name), "Player");
        snprintf(hs.town_name, sizeof(hs.town_name), "Town");
        hs.player_id = 0x55AA;
        hs.land_id = 0x1234;
        hs.gender = 0;
        hs.face_type = 0;
        hs.clothes_id = 0x2400;
    }

    u8 buf[256];
    int len = net_pack_handshake(buf, sizeof(buf), &hs);
    if (len > 0) {
        client_network_send_raw(NET_CH_RELIABLE, buf, len, true);
        enet_host_flush(g_client_net.host);
        g_client_net.state = CLIENT_NET_HANDSHAKING;
        printf("[CLIENT NET] Handshake transmitted for '%s'. Awaiting server ACK...\n", hs.player_name);
    } else {
        fprintf(stderr, "[CLIENT NET ERROR] Failed to serialize handshake packet.\n");
    }
}

bool client_network_connect(const char* host, uint16_t port) {
    if (host == NULL || host[0] == '\0') {
        host = "127.0.0.1";
    }
    if (port == 0) {
        port = NET_DEFAULT_PORT;
    }

    if (g_client_net.host == NULL) {
        if (!client_network_init()) {
            return false;
        }
    }

    if (g_client_net.server_peer != NULL) {
        client_network_disconnect(0);
    }

    ENetAddress addr;
    if (enet_address_set_host(&addr, host) < 0) {
        fprintf(stderr, "[CLIENT NET ERROR] Could not resolve host '%s'\n", host);
        return false;
    }
    addr.port = port;

    g_client_net.server_peer = enet_host_connect(g_client_net.host, &addr, NET_CH_COUNT, 0);
    if (g_client_net.server_peer == NULL) {
        fprintf(stderr, "[CLIENT NET ERROR] Could not initiate connection to %s:%u\n", host, port);
        return false;
    }

    snprintf(g_client_net.server_address, sizeof(g_client_net.server_address), "%s", host);
    g_client_net.server_port = port;
    g_client_net.state = CLIENT_NET_CONNECTING;
    g_client_net.connected_at_ms = SDL_GetTicks();
    g_client_net.last_activity_ms = g_client_net.connected_at_ms;
    g_client_net.last_ping_sent_ms = g_client_net.connected_at_ms;
    g_client_net.ping_rtt_ms = 0;

    printf("[CLIENT NET] Connecting to server %s:%u...\n", host, port);
    return true;
}

void client_network_disconnect(uint8_t reason) {
    if (g_client_net.server_peer != NULL) {
        u8 buf[32];
        pkt_disconnect_t dc;
        dc.client_id = g_client_net.client_id;
        dc.reason = reason;
        int len = net_pack_disconnect(buf, sizeof(buf), PKT_C2S_DISCONNECT, &dc);
        if (len > 0) {
            client_network_send_raw(NET_CH_RELIABLE, buf, len, true);
        }
        enet_peer_disconnect_later(g_client_net.server_peer, 0);
        enet_host_flush(g_client_net.host);
        g_client_net.server_peer = NULL;
    }

    g_client_net.state = CLIENT_NET_DISCONNECTED;
    g_client_net.client_id = 0;
    printf("[CLIENT NET] Disconnected from server (reason %u).\n", reason);
}

bool client_network_is_connected(void) {
    return g_client_net.state == CLIENT_NET_CONNECTED && g_client_net.server_peer != NULL;
}

ClientNetState client_network_get_state(void) {
    return g_client_net.state;
}

uint16_t client_network_get_client_id(void) {
    return g_client_net.client_id;
}

uint32_t client_network_get_ping_rtt(void) {
    return g_client_net.ping_rtt_ms;
}

bool client_network_send_raw(uint8_t channel, const void* data, size_t len, bool reliable) {
    if (g_client_net.host == NULL || g_client_net.server_peer == NULL) {
        return false;
    }

    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(data, len, flags);
    if (packet == NULL) {
        return false;
    }

    if (enet_peer_send(g_client_net.server_peer, channel, packet) < 0) {
        enet_packet_destroy(packet);
        return false;
    }

    return true;
}

bool client_network_send_input(const pkt_c2s_input_t* input) {
    if (!client_network_is_connected() || input == NULL) {
        return false;
    }

    pkt_c2s_input_t copy = *input;
    copy.client_id = g_client_net.client_id;
    copy.sequence = ++g_client_net.input_sequence;

    u8 buf[128];
    int len = net_pack_input(buf, sizeof(buf), &copy);
    if (len > 0) {
        return client_network_send_raw(NET_CH_UNRELIABLE, buf, len, false);
    }

    return false;
}

void client_network_poll(void) {
    if (g_client_net.host == NULL) {
        return;
    }

    ENetEvent event;
    while (enet_host_service(g_client_net.host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                printf("[CLIENT NET] ENet socket connected to server. Sending handshake...\n");
                client_network_send_handshake();
                break;

            case ENET_EVENT_TYPE_RECEIVE: {
                uint32_t now = SDL_GetTicks();
                g_client_net.last_activity_ms = now;

                net_header_t hdr;
                if (net_unpack_header(event.packet->data, event.packet->dataLength, &hdr) >= 0) {
                    switch (hdr.packet_id) {
                        case PKT_S2C_HANDSHAKE_ACK: {
                            pkt_s2c_handshake_ack_t ack;
                            if (net_unpack_handshake_ack(event.packet->data, event.packet->dataLength, &ack) > 0) {
                                if (ack.status == NET_HS_OK) {
                                    g_client_net.state = CLIENT_NET_CONNECTED;
                                    g_client_net.client_id = ack.client_id;
                                    g_client_net.server_world_info = ack;
                                    printf("[CLIENT NET] Handshake OK! Connected as Client ID %u (Town: '%s', Land: 0x%04X, Zone: %u)\n",
                                           ack.client_id, ack.town_name, ack.land_id, ack.starting_zone_id);
                                } else {
                                    printf("[CLIENT NET ERROR] Server rejected handshake (status code %u).\n", ack.status);
                                    client_network_disconnect(0);
                                }
                            }
                            break;
                        }

                        case PKT_PING: {
                            pkt_ping_t ping;
                            if (net_unpack_ping(event.packet->data, event.packet->dataLength, &ping) > 0) {
                                pkt_pong_t pong;
                                pong.client_timestamp_ms = ping.timestamp_ms;
                                pong.server_timestamp_ms = now;
                                u8 sbuf[64];
                                int slen = net_pack_pong(sbuf, sizeof(sbuf), &pong);
                                if (slen > 0) {
                                    client_network_send_raw(NET_CH_UNRELIABLE, sbuf, slen, false);
                                }
                            }
                            break;
                        }

                        case PKT_PONG: {
                            pkt_pong_t pong;
                            if (net_unpack_pong(event.packet->data, event.packet->dataLength, &pong) > 0) {
                                if (now >= pong.client_timestamp_ms) {
                                    g_client_net.ping_rtt_ms = now - pong.client_timestamp_ms;
                                }
                            }
                            break;
                        }

                        case PKT_S2C_DISCONNECT: {
                            pkt_disconnect_t dc;
                            net_unpack_disconnect(event.packet->data, event.packet->dataLength, &dc);
                            printf("[CLIENT NET] Disconnected by server (reason %u).\n", dc.reason);
                            g_client_net.state = CLIENT_NET_DISCONNECTED;
                            g_client_net.server_peer = NULL;
                            g_client_net.client_id = 0;
                            break;
                        }

                        default:
                            /* Remaining gameplay packets to be handled in Phase 7 */
                            break;
                    }
                }

                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                printf("[CLIENT NET] Server peer disconnected.\n");
                g_client_net.state = CLIENT_NET_DISCONNECTED;
                g_client_net.server_peer = NULL;
                g_client_net.client_id = 0;
                break;

            case ENET_EVENT_TYPE_NONE:
            default:
                break;
        }
    }

    /* Timeout checks and keepalive pings */
    uint32_t now = SDL_GetTicks();
    if (g_client_net.state == CLIENT_NET_CONNECTING || g_client_net.state == CLIENT_NET_HANDSHAKING) {
        if (now - g_client_net.connected_at_ms > CLIENT_NET_TIMEOUT_CONNECT_MS) {
            printf("[CLIENT NET] Connection / Handshake timed out.\n");
            client_network_disconnect(0);
        }
    } else if (g_client_net.state == CLIENT_NET_CONNECTED) {
        if (now - g_client_net.last_activity_ms > CLIENT_NET_TIMEOUT_HEARTBEAT_MS) {
            printf("[CLIENT NET] Inactivity timeout. Disconnecting from server.\n");
            client_network_disconnect(0);
        } else if (now - g_client_net.last_ping_sent_ms > CLIENT_NET_PING_INTERVAL_MS) {
            pkt_ping_t ping;
            ping.timestamp_ms = now;
            u8 pbuf[32];
            int plen = net_pack_ping(pbuf, sizeof(pbuf), &ping);
            if (plen > 0) {
                client_network_send_raw(NET_CH_UNRELIABLE, pbuf, plen, false);
            }
            g_client_net.last_ping_sent_ms = now;
        }
    }
}

/* --- Unit & Integration Self-Tests --- */

#define TEST_CLIENT_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[CLIENT NET TEST FAILED] %s: line %d\n", msg, __LINE__); \
            return 0; \
        } \
    } while (0)

int client_network_run_tests(void) {
    const uint16_t test_port = 21096;

    printf("[CLIENT NET TEST] Starting client network controller tests on port %u...\n", test_port);

    /* 0. Client persistent identity token subsystem tests */
    TEST_CLIENT_ASSERT(client_identity_run_tests(), "client_identity_run_tests");

    /* 1. Subsystem init / shutdown lifecycle */
    {
        TEST_CLIENT_ASSERT(client_network_init(), "client_network_init");
        TEST_CLIENT_ASSERT(client_network_get_state() == CLIENT_NET_DISCONNECTED, "initial state disconnected");
        client_network_shutdown();
    }

    /* 2. Client Connection, Automated Handshake, Input streaming, and Disconnect */
    {
        /* Start dedicated test server listener */
        TEST_CLIENT_ASSERT(server_network_init(test_port), "server_network_init");

        /* Initialize client */
        TEST_CLIENT_ASSERT(client_network_init(), "client_network_init for test 2");
        TEST_CLIENT_ASSERT(client_network_connect("127.0.0.1", test_port), "client_network_connect");
        TEST_CLIENT_ASSERT(client_network_get_state() == CLIENT_NET_CONNECTING, "state connecting");

        /* Service loop: wait for handshake completion */
        uint32_t start_ms = SDL_GetTicks();
        while (client_network_get_state() != CLIENT_NET_CONNECTED && SDL_GetTicks() - start_ms < 2500) {
            client_network_poll();
            server_network_poll();
            SDL_Delay(5);
        }

        TEST_CLIENT_ASSERT(client_network_is_connected(), "client_network_is_connected");
        TEST_CLIENT_ASSERT(client_network_get_client_id() > 0, "client assigned client_id > 0");
        TEST_CLIENT_ASSERT(server_network_get_authenticated_count() == 1, "server has 1 authenticated client");

        /* 3. Send Client Input stream */
        pkt_c2s_input_t inp;
        memset(&inp, 0, sizeof(inp));
        inp.stick_x = 42;
        inp.stick_y = -35;
        inp.button_mask = 0x8000;
        inp.player_x = 100.5f;
        inp.player_y = 0.0f;
        inp.player_z = -250.25f;

        TEST_CLIENT_ASSERT(client_network_send_input(&inp), "client_network_send_input");
        enet_host_flush(g_client_net.host);

        /* Pump events to let server receive input */
        start_ms = SDL_GetTicks();
        while (SDL_GetTicks() - start_ms < 50) {
            server_network_poll();
            client_network_poll();
            SDL_Delay(2);
        }

        ConnectedClient* cc = server_player_get(client_network_get_client_id());
        TEST_CLIENT_ASSERT(cc != NULL, "server ConnectedClient lookup");
        TEST_CLIENT_ASSERT(fabsf(cc->predicted_pos.x - 100.5f) < 0.01f, "server received input pos.x");
        TEST_CLIENT_ASSERT(fabsf(cc->predicted_pos.z - (-250.25f)) < 0.01f, "server received input pos.z");

        /* 4. Client Disconnect */
        client_network_disconnect(0);
        TEST_CLIENT_ASSERT(client_network_get_state() == CLIENT_NET_DISCONNECTED, "client state disconnected");

        /* Pump server to process disconnect */
        start_ms = SDL_GetTicks();
        while (server_network_get_authenticated_count() > 0 && SDL_GetTicks() - start_ms < 1000) {
            server_network_poll();
            client_network_poll();
            SDL_Delay(5);
        }

        TEST_CLIENT_ASSERT(server_network_get_authenticated_count() == 0, "server cleaned up after client disconnect");

        client_network_shutdown();
        server_network_shutdown();
    }

    printf("[CLIENT NET TEST] All client network controller tests passed successfully!\n");
    return 1;
}
