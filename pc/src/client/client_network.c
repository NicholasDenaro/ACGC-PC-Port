/* client_network.c - Client-side network controller and session manager */
#include "client/client_network.h"
#include "client/client_identity.h"
#include "client/client_sync.h"
#include "server/server_network.h"
#include "server/server_storage.h"
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
#include <math.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

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
    g_client_net.is_resident = false;
    g_client_net.resident_slot = -1;
    g_client_net.house_idx = -1;
    g_client_net.roommate_slot = -1;

    printf("[CLIENT NET] Client network subsystem initialized.\n");
    client_sync_init();
    return true;
}

void client_network_shutdown(void) {
    if (g_client_net.host == NULL) {
        return;
    }

    client_sync_reset();
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
    g_client_net.is_resident = false;
    g_client_net.resident_slot = -1;
    g_client_net.house_idx = -1;
    g_client_net.roommate_slot = -1;

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

bool client_network_is_new_player(void) {
    return g_client_net.is_new_player;
}

bool client_network_send_character_finalize(const Private_c* priv) {
    if (!client_network_is_connected() || priv == NULL) {
        return false;
    }

    pkt_c2s_character_finalize_t fin;
    memset(&fin, 0, sizeof(fin));
    fin.client_id = g_client_net.client_id;

    /* Extract clean null-terminated player name from GameCube format */
    char clean_name[PLAYER_NAME_LEN + 1];
    memset(clean_name, 0, sizeof(clean_name));
    for (int i = 0; i < PLAYER_NAME_LEN; i++) {
        if (priv->player_ID.player_name[i] == CHAR_SPACE || priv->player_ID.player_name[i] == 0) {
            break;
        }
        clean_name[i] = (char)priv->player_ID.player_name[i];
    }
    snprintf(fin.player_name, sizeof(fin.player_name), "%s", clean_name[0] ? clean_name : "Player");
    fin.gender = priv->gender;
    fin.face_type = priv->face;
    fin.clothes_id = priv->cloth.item;
    client_identity_get_token((uint8_t*)fin.auth_token);

    u8 buf[128];
    int len = net_pack_character_finalize(buf, sizeof(buf), &fin);
    if (len > 0) {
        printf("[CLIENT NET] Transmitting finalized character profile for '%s' to server...\n", fin.player_name);
        return client_network_send_raw(NET_CH_RELIABLE, buf, len, true);
    }
    return false;
}

bool client_network_is_resident(void) {
    return g_client_net.is_resident;
}

int8_t client_network_get_resident_slot(void) {
    return g_client_net.resident_slot;
}

int8_t client_network_get_house_idx(void) {
    return g_client_net.house_idx;
}

int8_t client_network_get_roommate_slot(void) {
    return g_client_net.roommate_slot;
}

void client_network_set_local_resident(uint8_t house_idx) {
    g_client_net.is_resident = true;
    g_client_net.house_idx = (int8_t)house_idx;
}

bool client_network_send_movein_req(uint8_t house_idx) {
    if (!client_network_is_connected()) {
        return false;
    }

    pkt_c2s_movein_req_t req;
    memset(&req, 0, sizeof(req));
    req.client_id = g_client_net.client_id;
    req.house_idx = house_idx;
    client_identity_get_token((uint8_t*)req.auth_token);

    u8 buf[128];
    int len = net_pack_movein_req(buf, sizeof(buf), &req);
    if (len > 0) {
        printf("[CLIENT NET] Transmitting Move-In request for House %u to server...\n", house_idx);
        return client_network_send_raw(NET_CH_RELIABLE, buf, len, true);
    }
    return false;
}

bool client_network_send_door_interact(int house_idx, int is_open) {
    if (!client_network_is_connected()) return false;
    pkt_c2s_interact_req_t req;
    memset(&req, 0, sizeof(req));
    req.client_id = g_client_net.client_id;
    req.action_type = NET_INTERACT_ENTER_DOOR;
    req.target_actor_id = HOUSE0 + house_idx;
    req.item_arg = (u16)is_open;
    u8 buf[128];
    int len = net_pack_interact_req(buf, sizeof(buf), &req);
    if (len > 0) {
        return client_network_send_raw(NET_CH_RELIABLE, buf, len, true);
    }
    return false;
}

bool client_network_send_zone_change(int scene_id, int room_idx) {
    if (!client_network_is_connected()) return false;
    pkt_c2s_interact_req_t req;
    memset(&req, 0, sizeof(req));
    req.client_id = g_client_net.client_id;
    req.action_type = NET_INTERACT_ZONE_CHANGE;
    req.target_actor_id = (u32)(room_idx >= 0 ? room_idx : 0);
    req.item_arg = (u16)scene_id;
    u8 buf[128];
    int len = net_pack_interact_req(buf, sizeof(buf), &req);
    if (len > 0) {
        printf("[CLIENT NET] Reporting scene change: scene=%d, room=%d\n", scene_id, room_idx);
        return client_network_send_raw(NET_CH_RELIABLE, buf, len, true);
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
                                if (ack.status == NET_HS_OK || ack.status == NET_HS_OK_NEW_PLAYER) {
                                    g_client_net.state = CLIENT_NET_CONNECTED;
                                    g_client_net.client_id = ack.client_id;
                                    g_client_net.server_world_info = ack;
                                    g_client_net.is_new_player = (ack.status == NET_HS_OK_NEW_PLAYER);
                                    if (ack.is_resident) {
                                        g_client_net.is_resident = true;
                                        g_client_net.house_idx = ack.house_idx;
                                        g_client_net.resident_slot = ack.resident_slot;
                                        g_client_net.roommate_slot = ack.roommate_slot;
                                    }
                                    if (g_client_net.is_new_player) {
                                        printf("[CLIENT NET] Handshake OK! New player token recognized (Rover train intro enabled).\n");
                                    } else {
                                        printf("[CLIENT NET] Handshake OK! Connected as Client ID %u (Town: '%s', Land: 0x%04X, Zone: %u, Resident: %d, House: %d)\n",
                                               ack.client_id, ack.town_name, ack.land_id, ack.starting_zone_id, ack.is_resident, ack.house_idx);
                                    }
                                } else {
                                    printf("[CLIENT NET ERROR] Server rejected handshake (status code %u).\n", ack.status);
                                    client_network_disconnect(0);
                                }
                            }
                            break;
                        }

                        case PKT_S2C_CHARACTER_ACK: {
                            pkt_s2c_character_ack_t ack;
                            if (net_unpack_character_ack(event.packet->data, event.packet->dataLength, &ack) > 0) {
                                if (ack.status == 0) {
                                    g_client_net.character_finalized = true;
                                    g_client_net.is_new_player = false;
                                    if (ack.is_resident) {
                                        g_client_net.is_resident = true;
                                        g_client_net.resident_slot = (int8_t)ack.slot_idx;
                                        g_client_net.house_idx = (int8_t)(ack.slot_idx / 2);
                                    }
                                    printf("[CLIENT NET] Character registered on server as %s (Slot %u)!\n",
                                           ack.is_resident ? "Resident" : "Visitor", ack.slot_idx);
                                }
                            }
                            break;
                        }

                        case PKT_S2C_MOVEIN_ACK: {
                            pkt_s2c_movein_ack_t ack;
                            if (net_unpack_movein_ack(event.packet->data, event.packet->dataLength, &ack) > 0) {
                                if (ack.status == NET_MOVEIN_OK) {
                                    g_client_net.is_resident = true;
                                    g_client_net.house_idx = (int8_t)ack.house_idx;
                                    g_client_net.resident_slot = (int8_t)ack.resident_slot;
                                    g_client_net.roommate_slot = (int8_t)ack.roommate_slot;
                                    printf("[CLIENT NET] Move-in confirmed by server! House %u, Resident Slot %u (Roommate %u)\n",
                                           ack.house_idx, ack.resident_slot, ack.roommate_slot);
                                } else {
                                    printf("[CLIENT NET] Move-in request rejected by server (status %u).\n", ack.status);
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
                            client_sync_reset();
                            g_client_net.state = CLIENT_NET_DISCONNECTED;
                            g_client_net.server_peer = NULL;
                            g_client_net.client_id = 0;
                            break;
                        }

                        case PKT_S2C_ENTITY_SYNC: {
                            pkt_s2c_entity_sync_t sync;
                            if (net_unpack_entity_sync(event.packet->data, event.packet->dataLength, &sync) > 0) {
                                client_sync_handle_packet(&sync);
                            }
                            break;
                        }

                        case PKT_C2S_INTERACT_REQ: {
                            pkt_c2s_interact_req_t req;
                            if (net_unpack_interact_req(event.packet->data, event.packet->dataLength, &req) > 0) {
                                if (req.action_type == NET_INTERACT_ENTER_DOOR && (req.item_arg == 1 || req.item_arg == 2)) {
                                    int house_idx = (int)(req.target_actor_id - HOUSE0);
                                    if (house_idx >= 0 && house_idx < 4) {
#ifndef TARGET_SERVER
                                        extern void aMHS_remote_open_door(int house_idx, int is_out);
                                        aMHS_remote_open_door(house_idx, req.item_arg == 2 ? 1 : 0);
#endif
                                    }
                                    if (req.item_arg == 1) {
                                        client_sync_notify_remote_door_open(req.client_id);
                                    }
                                } else if (req.action_type == NET_INTERACT_ZONE_CHANGE) {
                                    client_sync_notify_remote_zone_change(req.client_id, (int)req.item_arg, (int)req.target_actor_id);
                                }
                            }
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
            server_storage_init(NULL); \
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
        /* Use isolated test storage for client network testing */
        const char* test_storage_dir = "save/test_net_storage";
#ifdef _WIN32
        _mkdir("save");
        _mkdir(test_storage_dir);
#else
        mkdir("save", 0755);
        mkdir(test_storage_dir, 0755);
#endif
        char test_file[300], test_tmp[300];
        snprintf(test_file, sizeof(test_file), "%s/server_extended.dat", test_storage_dir);
        snprintf(test_tmp, sizeof(test_tmp), "%s/server_extended.dat.tmp", test_storage_dir);
        remove(test_file);
        remove(test_tmp);
        server_storage_init(test_storage_dir);

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

        /* Test character customization finalization (Rover questionnaire completion) */
        Private_c test_priv;
        memset(&test_priv, 0, sizeof(test_priv));
        test_priv.gender = mPr_SEX_FEMALE;
        test_priv.face = 4;
        test_priv.cloth.item = 0x2403;
        const char* custom_name = "Kiki";
        for (int i = 0; i < PLAYER_NAME_LEN; i++) {
            test_priv.player_ID.player_name[i] = (i < (int)strlen(custom_name)) ? (u8)custom_name[i] : CHAR_SPACE;
        }

        TEST_CLIENT_ASSERT(client_network_send_character_finalize(&test_priv), "client_network_send_character_finalize");
        enet_host_flush(g_client_net.host);

        start_ms = SDL_GetTicks();
        while (!g_client_net.character_finalized && SDL_GetTicks() - start_ms < 1000) {
            server_network_poll();
            client_network_poll();
            SDL_Delay(5);
        }

        TEST_CLIENT_ASSERT(g_client_net.character_finalized, "character finalization acknowledged by server");
        ConnectedClient* cc_check = server_player_get(client_network_get_client_id());
        TEST_CLIENT_ASSERT(cc_check != NULL, "server ConnectedClient lookup");
        TEST_CLIENT_ASSERT(cc_check->private_data.gender == mPr_SEX_FEMALE, "server recorded gender");
        TEST_CLIENT_ASSERT(cc_check->private_data.face == 4, "server recorded face");
        TEST_CLIENT_ASSERT(cc_check->private_data.cloth.item == 0x2403, "server recorded clothes");

        /* 3. House Move-In Request (Gyroid Move-In Interaction) */
        TEST_CLIENT_ASSERT(!client_network_is_resident(), "client starts as visitor before move-in");
        TEST_CLIENT_ASSERT(client_network_send_movein_req(0), "client_network_send_movein_req");
        enet_host_flush(g_client_net.host);

        /* Pump events to let server process move-in and send ACK */
        start_ms = SDL_GetTicks();
        while (!client_network_is_resident() && SDL_GetTicks() - start_ms < 1000) {
            server_network_poll();
            client_network_poll();
            SDL_Delay(5);
        }

        TEST_CLIENT_ASSERT(client_network_is_resident(), "client became resident after move-in");
        TEST_CLIENT_ASSERT(client_network_get_house_idx() == 0, "client assigned house 0");
        TEST_CLIENT_ASSERT(cc_check->resident_idx == 0, "server bound resident to slot 0");

        /* 4. Send Client Input stream */
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

        remove(test_file);
        remove(test_tmp);
        server_storage_init(NULL);
    }

    printf("[CLIENT NET TEST] All client network controller tests passed successfully!\n");
    return 1;
}
