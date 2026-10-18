/* server_network.c - ENet server listener and client session management */
#include "server/server_network.h"
#include "server/server_player.h"
#include "server/server_zone.h"
#include "server/server_spatial.h"
#include "server/server_runner.h"
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
#include <time.h>

static ServerClientSession s_sessions[SERVER_NET_MAX_CLIENTS];
static ENetHost* s_server_host = NULL;
static uint32_t s_next_client_id = 2; /* Client 1 is host/local player */
static uint16_t s_bound_port = 0;

bool server_network_init(uint16_t port) {
    if (s_server_host != NULL) {
        return true;
    }

    if (port == 0) {
        port = SERVER_DEFAULT_PORT;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    s_server_host = enet_host_create(&address, SERVER_NET_MAX_CLIENTS, NET_CH_COUNT, 0, 0);
    if (s_server_host == NULL) {
        fprintf(stderr, "[SERVER NET ERROR] Failed to create ENet listener on port %u\n", port);
        return false;
    }

    memset(s_sessions, 0, sizeof(s_sessions));
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        s_sessions[i].player_slot = -1;
    }

    s_bound_port = port;
    printf("[SERVER NET] ENet listener started on 0.0.0.0:%u (Max Peers: %d, Channels: %d)\n",
           port, SERVER_NET_MAX_CLIENTS, NET_CH_COUNT);
    return true;
}

void server_network_shutdown(void) {
    if (s_server_host == NULL) {
        return;
    }

    /* Disconnect all connected peers gracefully */
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        if (s_sessions[i].peer != NULL && s_sessions[i].state != NET_SESSION_DISCONNECTED) {
            enet_peer_disconnect(s_sessions[i].peer, 0);
            if (s_sessions[i].player_slot >= 0) {
                server_player_unregister(s_sessions[i].client_id);
            }
        }
    }

    /* Allow disconnect packets to flush */
    enet_host_flush(s_server_host);
    ENetEvent event;
    uint32_t start_ms = SDL_GetTicks();
    while (SDL_GetTicks() - start_ms < 200) {
        if (enet_host_service(s_server_host, &event, 50) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        }
    }

    enet_host_destroy(s_server_host);
    s_server_host = NULL;
    memset(s_sessions, 0, sizeof(s_sessions));
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        s_sessions[i].player_slot = -1;
    }

    printf("[SERVER NET] ENet listener shut down on port %u.\n", s_bound_port);
    s_bound_port = 0;
}

bool server_network_is_running(void) {
    return s_server_host != NULL;
}

ServerClientSession* server_network_get_session(uint32_t client_id) {
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        if (s_sessions[i].state != NET_SESSION_DISCONNECTED && s_sessions[i].client_id == client_id) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

ServerClientSession* server_network_get_session_by_peer(ENetPeer* peer) {
    if (peer == NULL) return NULL;
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        if (s_sessions[i].peer == peer) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

int server_network_get_authenticated_count(void) {
    int count = 0;
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        if (s_sessions[i].state >= NET_SESSION_AUTHENTICATED) {
            count++;
        }
    }
    return count;
}

bool server_network_send_raw(uint32_t client_id, uint8_t channel, const void* data, size_t len, bool reliable) {
    ServerClientSession* session = server_network_get_session(client_id);
    if (session == NULL || session->peer == NULL) {
        return false;
    }

    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(data, len, flags);
    if (packet == NULL) {
        return false;
    }

    if (enet_peer_send(session->peer, channel, packet) < 0) {
        enet_packet_destroy(packet);
        return false;
    }

    return true;
}

void server_network_broadcast_raw(uint8_t channel, const void* data, size_t len, bool reliable, uint32_t exclude_client_id) {
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        if (s_sessions[i].state >= NET_SESSION_AUTHENTICATED &&
            s_sessions[i].client_id != exclude_client_id &&
            s_sessions[i].peer != NULL) {
            server_network_send_raw(s_sessions[i].client_id, channel, data, len, reliable);
        }
    }
}

void server_network_broadcast_spatial(uint32_t zone_id, int block_x, int block_z, uint8_t channel, const void* data, size_t len, bool reliable, uint32_t exclude_client_id) {
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        if (s_sessions[i].state >= NET_SESSION_AUTHENTICATED &&
            s_sessions[i].client_id != exclude_client_id &&
            s_sessions[i].peer != NULL) {
            if (server_spatial_is_client_interested(s_sessions[i].client_id, zone_id, block_x, block_z)) {
                server_network_send_raw(s_sessions[i].client_id, channel, data, len, reliable);
            }
        }
    }
}

void server_network_broadcast_spatial_wpos(uint32_t zone_id, float world_x, float world_z, uint8_t channel, const void* data, size_t len, bool reliable, uint32_t exclude_client_id) {
    int bx = 0, bz = 0;
    server_spatial_wpos_to_block(world_x, world_z, &bx, &bz);
    server_network_broadcast_spatial(zone_id, bx, bz, channel, data, len, reliable, exclude_client_id);
}

void server_network_disconnect_client(uint32_t client_id, uint8_t reason) {
    ServerClientSession* session = server_network_get_session(client_id);
    if (session == NULL) {
        return;
    }

    if (session->peer != NULL) {
        u8 buf[32];
        pkt_disconnect_t dc;
        dc.client_id = (u16)client_id;
        dc.reason = reason;
        int len = net_pack_disconnect(buf, sizeof(buf), PKT_S2C_DISCONNECT, &dc);
        if (len > 0) {
            server_network_send_raw(client_id, NET_CH_RELIABLE, buf, len, true);
        }
        enet_peer_disconnect_later(session->peer, 0);
    }

    if (session->player_slot >= 0) {
        server_player_unregister(session->client_id);
    }

    server_spatial_remove_client(client_id);

    printf("[SERVER NET] Disconnected Client ID %u (reason %u).\n", client_id, reason);
    session->state = NET_SESSION_DISCONNECTED;
    session->peer = NULL;
    session->player_slot = -1;
}

static void server_network_handle_handshake(ServerClientSession* session, const u8* data, size_t len) {
    pkt_c2s_handshake_t hs;
    if (net_unpack_handshake(data, len, &hs) <= 0) {
        fprintf(stderr, "[SERVER NET WARNING] Malformed handshake packet from Client ID %u\n", session->client_id);
        return;
    }

    pkt_s2c_handshake_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.client_id = (u16)session->client_id;

    if (hs.protocol_version != NET_PROTOCOL_VERSION) {
        printf("[SERVER NET] Handshake rejected for Client ID %u: protocol mismatch (client=%u, server=%u)\n",
               session->client_id, hs.protocol_version, NET_PROTOCOL_VERSION);
        ack.status = NET_HS_VERSION_MISMATCH;
        u8 sbuf[128];
        int slen = net_pack_handshake_ack(sbuf, sizeof(sbuf), &ack);
        if (slen > 0) {
            server_network_send_raw(session->client_id, NET_CH_RELIABLE, sbuf, slen, true);
        }
        enet_peer_disconnect_later(session->peer, 0);
        session->state = NET_SESSION_DISCONNECTED;
        return;
    }

    /* Query server extended storage for persistent client token */
    bool is_resident = false;
    int player_idx = server_storage_find_player((const uint8_t*)hs.auth_token, &is_resident);
    bool is_new_player = (player_idx < 0);

    Private_c priv;
    if (!is_new_player) {
        if (is_resident) {
            ServerResident* res = server_storage_get_resident(player_idx);
            priv = res->private_data;
            res->last_seen_time = (uint64_t)time(NULL);
            printf("[SERVER NET] Handshake recognized Resident Slot %d in House %d (Roommate %d).\n",
                   player_idx, res->house_idx, res->roommate_slot);
        } else {
            ServerVisitor* vis = server_storage_get_visitor(player_idx);
            priv = vis->private_data;
            vis->last_seen_time = (uint64_t)time(NULL);
            printf("[SERVER NET] Handshake recognized returning Visitor Slot %d.\n", player_idx);
        }
    } else {
        /* New player token: create initial profile and register as visitor */
        mPr_InitPrivateInfo(&priv);

        int name_len = (int)strlen(hs.player_name);
        u8 dummy_name[PLAYER_NAME_LEN];
        for (int i = 0; i < PLAYER_NAME_LEN; i++) {
            dummy_name[i] = (i < name_len) ? (u8)hs.player_name[i] : CHAR_SPACE;
        }
        mPr_CopyPlayerName(priv.player_ID.player_name, dummy_name);
        mLd_CopyLandName(priv.player_ID.land_name, Save_Get(land_info).name);
        priv.player_ID.player_id = hs.player_id ? hs.player_id : 0x55AA;
        priv.player_ID.land_id = Save_Get(land_info).id;
        priv.gender = hs.gender;
        priv.face = hs.face_type;
        priv.cloth.idx = 0;
        priv.cloth.item = hs.clothes_id ? hs.clothes_id : 0x2400;
        priv.inventory.wallet = 1000;
        priv.exists = TRUE;

        player_idx = server_storage_register_visitor((const uint8_t*)hs.auth_token, &priv);
        printf("[SERVER NET] Handshake registered initial Visitor Slot %d (new player awaiting Rover questionnaire).\n", player_idx);
    }

    int slot = server_player_register(session->client_id, 0, &priv);
    if (slot < 0) {
        printf("[SERVER NET] Handshake rejected for Client ID %u: server player registry full\n",
               session->client_id);
        ack.status = NET_HS_SERVER_FULL;
        u8 sbuf[128];
        int slen = net_pack_handshake_ack(sbuf, sizeof(sbuf), &ack);
        if (slen > 0) {
            server_network_send_raw(session->client_id, NET_CH_RELIABLE, sbuf, slen, true);
        }
        enet_peer_disconnect_later(session->peer, 0);
        session->state = NET_SESSION_DISCONNECTED;
        return;
    }

    /* Bind client identity token to ConnectedClient */
    ConnectedClient* cc = server_player_get(session->client_id);
    if (cc != NULL) {
        memcpy(cc->auth_token, hs.auth_token, 32);
        cc->resident_idx = is_resident ? player_idx : -1;
    }

    session->player_slot = slot;
    snprintf(session->player_name, sizeof(session->player_name), "%s", hs.player_name);
    snprintf(session->town_name, sizeof(session->town_name), "%s", hs.town_name);
    session->player_id = hs.player_id;
    session->land_id = hs.land_id;
    session->gender = hs.gender;
    session->face_type = hs.face_type;
    session->clothes_id = hs.clothes_id;

    ack.status = is_new_player ? NET_HS_OK_NEW_PLAYER : NET_HS_OK;
    ack.world_time = Common_Get(time.rtc_time);
    snprintf(ack.town_name, sizeof(ack.town_name), "%s", (const char*)Save_Get(land_info).name);
    ack.land_id = Save_Get(land_info).id;
    ack.weather = Common_Get(weather);
    ack.starting_zone_id = 0; /* Town outdoors */

    u8 sbuf[128];
    int slen = net_pack_handshake_ack(sbuf, sizeof(sbuf), &ack);
    if (slen > 0) {
        server_network_send_raw(session->client_id, NET_CH_RELIABLE, sbuf, slen, true);
        enet_host_flush(s_server_host);
        session->state = NET_SESSION_AUTHENTICATED;
        server_spatial_update_client_acre(session->client_id, ack.starting_zone_id, 3, 1);
        printf("[SERVER NET] Handshake accepted: Client ID %u ('%s') assigned to slot %d%s\n",
               session->client_id, hs.player_name, slot, is_new_player ? " (New Player / Rover Train Ride)" : "");
    }
}

static void server_network_handle_character_finalize(ServerClientSession* session, const u8* data, size_t len) {
    pkt_c2s_character_finalize_t fin;
    if (net_unpack_character_finalize(data, len, &fin) <= 0) {
        fprintf(stderr, "[SERVER NET WARNING] Malformed character finalize packet from Client ID %u\n", session->client_id);
        return;
    }

    /* Update or register Visitor in server extended storage */
    bool is_resident = false;
    int player_idx = server_storage_find_player((const uint8_t*)fin.auth_token, &is_resident);
    Private_c* priv_ptr = NULL;

    if (player_idx >= 0) {
        if (is_resident) {
            ServerResident* res = server_storage_get_resident(player_idx);
            priv_ptr = &res->private_data;
        } else {
            ServerVisitor* vis = server_storage_get_visitor(player_idx);
            priv_ptr = &vis->private_data;
        }
    } else {
        Private_c new_priv;
        mPr_InitPrivateInfo(&new_priv);
        mLd_CopyLandName(new_priv.player_ID.land_name, Save_Get(land_info).name);
        new_priv.player_ID.player_id = 0x55AA;
        new_priv.player_ID.land_id = Save_Get(land_info).id;
        new_priv.inventory.wallet = 1000;
        new_priv.exists = TRUE;
        player_idx = server_storage_register_visitor((const uint8_t*)fin.auth_token, &new_priv);
        ServerVisitor* vis = server_storage_get_visitor(player_idx);
        priv_ptr = &vis->private_data;
        is_resident = false;
    }

    if (priv_ptr != NULL) {
        int name_len = (int)strlen(fin.player_name);
        u8 formatted_name[PLAYER_NAME_LEN];
        for (int i = 0; i < PLAYER_NAME_LEN; i++) {
            formatted_name[i] = (i < name_len) ? (u8)fin.player_name[i] : CHAR_SPACE;
        }
        mPr_CopyPlayerName(priv_ptr->player_ID.player_name, formatted_name);
        priv_ptr->gender = fin.gender;
        priv_ptr->face = fin.face_type;
        priv_ptr->cloth.idx = 0;
        priv_ptr->cloth.item = fin.clothes_id ? fin.clothes_id : 0x2400;

        server_storage_mark_dirty();
        printf("[SERVER NET] Character customized for Client ID %u: '%s' (Gender %u, Face %u, Clothes 0x%04X)\n",
               session->client_id, fin.player_name, fin.gender, fin.face_type, fin.clothes_id);

        ConnectedClient* cc = server_player_get(session->client_id);
        if (cc != NULL) {
            cc->private_data = *priv_ptr;
        }
    }

    snprintf(session->player_name, sizeof(session->player_name), "%s", fin.player_name);
    session->gender = fin.gender;
    session->face_type = fin.face_type;
    session->clothes_id = fin.clothes_id;

    pkt_s2c_character_ack_t ack;
    ack.status = 0;
    ack.client_id = (u16)session->client_id;
    ack.is_resident = is_resident ? 1 : 0;
    ack.slot_idx = (u8)player_idx;

    u8 sbuf[64];
    int slen = net_pack_character_ack(sbuf, sizeof(sbuf), &ack);
    if (slen > 0) {
        server_network_send_raw(session->client_id, NET_CH_RELIABLE, sbuf, slen, true);
    }
}

static void server_network_handle_movein_req(ServerClientSession* session, const u8* data, size_t len) {
    pkt_c2s_movein_req_t req;
    if (net_unpack_movein_req(data, len, &req) <= 0) {
        return;
    }

    pkt_s2c_movein_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.client_id = (u16)session->client_id;
    ack.house_idx = req.house_idx;

    ConnectedClient* cc = server_player_get(session->client_id);
    if (cc == NULL || req.house_idx >= MAX_EXTENDED_HOUSES) {
        ack.status = NET_MOVEIN_ERROR;
    } else if (cc->resident_idx >= 0) {
        ack.status = NET_MOVEIN_ALREADY_RESIDENT;
        printf("[SERVER NET] Move-in request rejected for Client ID %u: already resident of house %d\n",
               session->client_id, cc->resident_idx / 2);
    } else {
        int vacancies = server_storage_get_house_vacancies(req.house_idx);
        if (vacancies <= 0) {
            ack.status = NET_MOVEIN_HOUSE_FULL;
            printf("[SERVER NET] Move-in request rejected for Client ID %u: House %u is full\n",
                   session->client_id, req.house_idx);
        } else {
            int resident_slot = -1;
            bool ok = server_storage_promote_to_resident((const uint8_t*)req.auth_token, req.house_idx,
                                                        &cc->private_data, &resident_slot);
            if (!ok) {
                ack.status = NET_MOVEIN_ERROR;
            } else {
                ack.status = NET_MOVEIN_OK;
                ack.resident_slot = (u8)resident_slot;
                ServerResident* res = server_storage_get_resident(resident_slot);
                ack.roommate_slot = res ? res->roommate_slot : (u8)(resident_slot % 2);

                cc->resident_idx = resident_slot;
                memcpy(cc->auth_token, req.auth_token, 32);

                /* Mirror primary resident to canonical .gci save */
                if (mPr_NullCheckPersonalID(Save_GetPointer(homes[req.house_idx].ownerID)) == TRUE) {
                    mPr_CopyPersonalID(&Save_Get(homes[req.house_idx]).ownerID, &cc->private_data.player_ID);
                    Save_Get(homes[req.house_idx]).size_info.size = mHm_HOMESIZE_SMALL;
                    Save_Get(homes[req.house_idx]).flags.has_saved = TRUE;
                }

                server_storage_mark_dirty();
                printf("[SERVER NET] Client ID %u promoted to Resident Slot %d in House %u (Roommate %u)!\n",
                       session->client_id, resident_slot, req.house_idx, ack.roommate_slot);
            }
        }
    }

    u8 sbuf[64];
    int slen = net_pack_movein_ack(sbuf, sizeof(sbuf), &ack);
    if (slen > 0) {
        server_network_send_raw(session->client_id, NET_CH_RELIABLE, sbuf, slen, true);
    }
}

void server_network_poll(void) {
    if (s_server_host == NULL) {
        return;
    }

    ENetEvent event;
    while (enet_host_service(s_server_host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                int slot = -1;
                for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
                    if (s_sessions[i].state == NET_SESSION_DISCONNECTED) {
                        slot = i;
                        break;
                    }
                }

                if (slot < 0) {
                    printf("[SERVER NET] Connection refused: maximum client sessions reached\n");
                    enet_peer_reset(event.peer);
                    break;
                }

                ServerClientSession* session = &s_sessions[slot];
                memset(session, 0, sizeof(ServerClientSession));
                session->client_id = s_next_client_id++;
                session->state = NET_SESSION_CONNECTING;
                session->peer = event.peer;
                session->connected_at_ms = SDL_GetTicks();
                session->last_activity_ms = session->connected_at_ms;
                session->last_ping_sent_ms = session->connected_at_ms;
                session->player_slot = -1;

                event.peer->data = session;

                char ip[64] = "unknown";
                enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
                printf("[SERVER NET] Peer connected from %s:%u (Assigned Client ID %u, Session Slot %d)\n",
                       ip, event.peer->address.port, session->client_id, slot);
                break;
            }

            case ENET_EVENT_TYPE_RECEIVE: {
                ServerClientSession* session = (ServerClientSession*)event.peer->data;
                if (session == NULL || session->state == NET_SESSION_DISCONNECTED) {
                    enet_packet_destroy(event.packet);
                    break;
                }

                uint32_t now = SDL_GetTicks();
                session->last_activity_ms = now;

                net_header_t hdr;
                if (net_unpack_header(event.packet->data, event.packet->dataLength, &hdr) >= 0) {
                    switch (hdr.packet_id) {
                        case PKT_PING: {
                            pkt_ping_t ping;
                            if (net_unpack_ping(event.packet->data, event.packet->dataLength, &ping) > 0) {
                                pkt_pong_t pong;
                                pong.client_timestamp_ms = ping.timestamp_ms;
                                pong.server_timestamp_ms = now;
                                u8 sbuf[64];
                                int slen = net_pack_pong(sbuf, sizeof(sbuf), &pong);
                                if (slen > 0) {
                                    server_network_send_raw(session->client_id, NET_CH_UNRELIABLE, sbuf, slen, false);
                                }
                            }
                            break;
                        }

                        case PKT_PONG: {
                            pkt_pong_t pong;
                            if (net_unpack_pong(event.packet->data, event.packet->dataLength, &pong) > 0) {
                                if (now >= pong.server_timestamp_ms) {
                                    session->ping_rtt_ms = now - pong.server_timestamp_ms;
                                }
                            }
                            break;
                        }

                        case PKT_C2S_HANDSHAKE:
                            if (session->state == NET_SESSION_CONNECTING) {
                                server_network_handle_handshake(session, event.packet->data, event.packet->dataLength);
                            }
                            break;

                        case PKT_C2S_CHARACTER_FINALIZE:
                            if (session->state >= NET_SESSION_AUTHENTICATED) {
                                server_network_handle_character_finalize(session, event.packet->data, event.packet->dataLength);
                            }
                            break;

                        case PKT_C2S_MOVEIN_REQ:
                            if (session->state >= NET_SESSION_AUTHENTICATED) {
                                server_network_handle_movein_req(session, event.packet->data, event.packet->dataLength);
                            }
                            break;

                        case PKT_C2S_DISCONNECT: {
                            pkt_disconnect_t dc;
                            net_unpack_disconnect(event.packet->data, event.packet->dataLength, &dc);
                            printf("[SERVER NET] Client ID %u requested disconnect (reason %u)\n",
                                   session->client_id, dc.reason);
                            server_network_disconnect_client(session->client_id, dc.reason);
                            break;
                        }

                        case PKT_C2S_INPUT: {
                            pkt_c2s_input_t inp;
                            if (net_unpack_input(event.packet->data, event.packet->dataLength, &inp) > 0) {
                                session->state = NET_SESSION_ACTIVE;
                                ConnectedClient* cc = server_player_get(session->client_id);
                                if (cc != NULL) {
                                    cc->predicted_pos.x = inp.player_x;
                                    cc->predicted_pos.y = inp.player_y;
                                    cc->predicted_pos.z = inp.player_z;
                                    cc->last_ack_input_seq = inp.sequence;
                                    if (cc->player_actor != NULL) {
                                        cc->player_actor->actor_class.world.position.x = inp.player_x;
                                        cc->player_actor->actor_class.world.position.y = inp.player_y;
                                        cc->player_actor->actor_class.world.position.z = inp.player_z;
                                    }
                                    server_spatial_update_client_wpos(session->client_id, cc->zone_id, inp.player_x, inp.player_z);
                                }
                            }
                            break;
                        }

                        default:
                            /* Other packet types handled by subsequent phases */
                            break;
                    }
                }

                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT: {
                ServerClientSession* session = (ServerClientSession*)event.peer->data;
                if (session != NULL) {
                    printf("[SERVER NET] Peer disconnected for Client ID %u\n", session->client_id);
                    server_spatial_remove_client(session->client_id);
                    if (session->player_slot >= 0) {
                        server_player_unregister(session->client_id);
                    }
                    memset(session, 0, sizeof(ServerClientSession));
                    session->player_slot = -1;
                    event.peer->data = NULL;
                }
                break;
            }

            case ENET_EVENT_TYPE_NONE:
            default:
                break;
        }
    }

    /* Periodic heartbeat and timeout checks */
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < SERVER_NET_MAX_CLIENTS; i++) {
        ServerClientSession* session = &s_sessions[i];
        if (session->state == NET_SESSION_CONNECTING) {
            if (now - session->connected_at_ms > SERVER_NET_TIMEOUT_HANDSHAKE_MS) {
                printf("[SERVER NET] Handshake timeout for Client ID %u\n", session->client_id);
                server_network_disconnect_client(session->client_id, 0);
            }
        } else if (session->state >= NET_SESSION_AUTHENTICATED) {
            if (now - session->last_activity_ms > SERVER_NET_TIMEOUT_HEARTBEAT_MS) {
                printf("[SERVER NET] Heartbeat inactivity timeout for Client ID %u\n", session->client_id);
                server_network_disconnect_client(session->client_id, 0);
            } else if (now - session->last_ping_sent_ms > SERVER_NET_PING_INTERVAL_MS) {
                pkt_ping_t ping;
                ping.timestamp_ms = now;
                u8 pbuf[32];
                int plen = net_pack_ping(pbuf, sizeof(pbuf), &ping);
                if (plen > 0) {
                    server_network_send_raw(session->client_id, NET_CH_UNRELIABLE, pbuf, plen, false);
                }
                session->last_ping_sent_ms = now;
            }
        }
    }
}

/* --- Unit & Integration Self-Tests --- */

#define TEST_NET_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[SERVER NET TEST FAILED] %s: line %d\n", msg, __LINE__); \
            return 0; \
        } \
    } while (0)

int server_network_run_tests(void) {
    const uint16_t test_port = 21098;

    printf("[SERVER NET TEST] Starting server network listener tests on port %u...\n", test_port);

    /* 1. Host Creation & Shutdown lifecycle */
    {
        TEST_NET_ASSERT(server_network_init(test_port), "server_network_init");
        TEST_NET_ASSERT(server_network_is_running(), "server_network_is_running");
        server_network_shutdown();
        TEST_NET_ASSERT(!server_network_is_running(), "!server_network_is_running");
    }

    /* 2. Full Client Connection, Handshake, Ping/Pong, and Disconnect lifecycle */
    {
        TEST_NET_ASSERT(server_network_init(test_port), "server_network_init for test 2");

        /* Create client ENet host */
        ENetHost* client_host = enet_host_create(NULL, 1, NET_CH_COUNT, 0, 0);
        TEST_NET_ASSERT(client_host != NULL, "client enet_host_create");

        ENetAddress server_addr;
        enet_address_set_host_ip(&server_addr, "127.0.0.1");
        server_addr.port = test_port;

        ENetPeer* server_peer = enet_host_connect(client_host, &server_addr, NET_CH_COUNT, 0);
        TEST_NET_ASSERT(server_peer != NULL, "enet_host_connect");

        /* Pump events until both client and server recognize connection */
        int client_connected = 0;
        uint32_t start_ms = SDL_GetTicks();
        while ((!client_connected || s_sessions[0].state == NET_SESSION_DISCONNECTED) && SDL_GetTicks() - start_ms < 2000) {
            ENetEvent cev;
            while (enet_host_service(client_host, &cev, 10) > 0) {
                if (cev.type == ENET_EVENT_TYPE_CONNECT) {
                    client_connected = 1;
                }
            }
            server_network_poll();
        }
        TEST_NET_ASSERT(client_connected, "client connection to server");
        TEST_NET_ASSERT(s_sessions[0].state == NET_SESSION_CONNECTING, "server session state connecting");

        ServerClientSession* session = &s_sessions[0];
        TEST_NET_ASSERT(session->peer != NULL, "session peer valid");

        /* 3. Send Handshake packet from Client to Server */
        pkt_c2s_handshake_t hs;
        memset(&hs, 0, sizeof(hs));
        hs.client_version = 1;
        hs.protocol_version = NET_PROTOCOL_VERSION;
        snprintf(hs.player_name, sizeof(hs.player_name), "TestNetBob");
        snprintf(hs.town_name, sizeof(hs.town_name), "TestNetTown");
        hs.player_id = 0x1234;
        hs.land_id = 0x5678;
        hs.gender = 0;
        hs.face_type = 2;
        hs.clothes_id = 0x2401;

        u8 hs_buf[256];
        int hs_len = net_pack_handshake(hs_buf, sizeof(hs_buf), &hs);
        TEST_NET_ASSERT(hs_len > 0, "net_pack_handshake");

        ENetPacket* hs_packet = enet_packet_create(hs_buf, hs_len, ENET_PACKET_FLAG_RELIABLE);
        TEST_NET_ASSERT(enet_peer_send(server_peer, NET_CH_RELIABLE, hs_packet) == 0, "send handshake");
        enet_host_flush(client_host);

        /* Pump events until client receives Handshake ACK */
        int ack_received = 0;
        pkt_s2c_handshake_ack_t ack;
        start_ms = SDL_GetTicks();
        while (!ack_received && SDL_GetTicks() - start_ms < 2000) {
            server_network_poll();
            ENetEvent cev;
            while (enet_host_service(client_host, &cev, 10) > 0) {
                if (cev.type == ENET_EVENT_TYPE_RECEIVE) {
                    if (net_unpack_handshake_ack(cev.packet->data, cev.packet->dataLength, &ack) > 0) {
                        ack_received = 1;
                    }
                    enet_packet_destroy(cev.packet);
                }
            }
        }
        TEST_NET_ASSERT(ack_received, "handshake ack received");
        TEST_NET_ASSERT(ack.status == NET_HS_OK || ack.status == NET_HS_OK_NEW_PLAYER, "handshake ack status OK");
        TEST_NET_ASSERT(ack.client_id == session->client_id, "handshake ack client_id");
        TEST_NET_ASSERT(session->state == NET_SESSION_AUTHENTICATED, "session state authenticated");
        TEST_NET_ASSERT(server_network_get_authenticated_count() == 1, "authenticated count == 1");

        /* 4. Client sends Ping, verifies Server replies with Pong */
        pkt_ping_t ping = { 0x55AA1234 };
        u8 ping_buf[64];
        int ping_len = net_pack_ping(ping_buf, sizeof(ping_buf), &ping);
        TEST_NET_ASSERT(ping_len > 0, "net_pack_ping");

        ENetPacket* ping_packet = enet_packet_create(ping_buf, ping_len, 0);
        TEST_NET_ASSERT(enet_peer_send(server_peer, NET_CH_UNRELIABLE, ping_packet) == 0, "send ping");
        enet_host_flush(client_host);

        int pong_received = 0;
        pkt_pong_t pong;
        start_ms = SDL_GetTicks();
        while (!pong_received && SDL_GetTicks() - start_ms < 2000) {
            server_network_poll();
            ENetEvent cev;
            while (enet_host_service(client_host, &cev, 10) > 0) {
                if (cev.type == ENET_EVENT_TYPE_RECEIVE) {
                    if (net_unpack_pong(cev.packet->data, cev.packet->dataLength, &pong) > 0) {
                        pong_received = 1;
                    }
                    enet_packet_destroy(cev.packet);
                }
            }
        }
        TEST_NET_ASSERT(pong_received, "pong received");
        TEST_NET_ASSERT(pong.client_timestamp_ms == 0x55AA1234, "pong client timestamp match");

        /* 5. Client Disconnect */
        enet_peer_disconnect(server_peer, 0);
        enet_host_flush(client_host);

        start_ms = SDL_GetTicks();
        while (session->state != NET_SESSION_DISCONNECTED && SDL_GetTicks() - start_ms < 2000) {
            ENetEvent cev;
            while (enet_host_service(client_host, &cev, 10) > 0) {
                if (cev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(cev.packet);
                }
            }
            server_network_poll();
        }
        TEST_NET_ASSERT(session->state == NET_SESSION_DISCONNECTED, "server session disconnected");
        TEST_NET_ASSERT(server_network_get_authenticated_count() == 0, "server authenticated count == 0");

        enet_host_destroy(client_host);
        server_network_shutdown();
    }

    printf("[SERVER NET TEST] All server network listener tests passed successfully!\n");
    return 1;
}
