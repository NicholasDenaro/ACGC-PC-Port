# Tasks: Authoritative Dedicated Server Architecture

This task tracker outlines all implementation milestones defined in [Specification 001: Authoritative Dedicated Server Architecture](file:///C:/Source/ACGC-PC-Port/specs/SERVER_ARCHITECTURE.md).

---

## Task Summary Dashboard

| Phase | Category | Completed | Total | Status |
| :--- | :--- | :---: | :---: | :--- |
| **Phase 1** | Headless Build Target & Dedicated Server Daemon | 0 | 4 | In Progress / Planned |
| **Phase 2** | Simulation Unpausing & Input Isolation | 0 | 4 | Not Implemented |
| **Phase 3** | Multi-Player Entity Decoupling | 0 | 4 | Not Implemented |
| **Phase 4** | Multi-Zone Simulation Engine (Outdoor + Rooms) | 0 | 5 | Not Implemented |
| **Phase 5** | Network Transport Layer (ENet / UDP) | 0 | 4 | Not Implemented |
| **Phase 6** | World State Replication & Spatial Management | 0 | 5 | Not Implemented |
| **Phase 7** | World Persistence & Save Journaling | 0 | 3 | Not Implemented |
| **Total** | | **0** | **29** | **0% Complete** |

---

## Detailed Task Breakdown

### Phase 1: Headless Build Target & Dedicated Server Daemon

- [ ] **Task 1.1: CMake Build Target for Headless Server**
  - **State**: `Not Implemented`
  - **Files**: `pc/CMakeLists.txt`, `pc/src/server/server_main.c`
  - **Description**: Add a `BUILD_SERVER=ON` CMake configuration option defining `-DTARGET_SERVER -DHEADLESS`. Create `pc/src/server/server_main.c` as the dedicated server binary entry point.
  - **Acceptance Criteria**: Running `cmake --build . --target AnimalCrossingServer` compiles a standalone CLI executable without requiring SDL2 display windows or OpenGL contexts.

- [ ] **Task 1.2: Graphics & GX Subsystem Stubs**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/pc_gx.c`, `pc/src/pc_gx_tev.c`, `src/graph.c`
  - **Description**: Add conditional `#ifndef TARGET_SERVER` guards around all OpenGL rendering, shader compilation, and GX display list submission. When running in server mode, vertex submission and texture binding must be instant no-ops.
  - **Acceptance Criteria**: Graphics initialization returns success immediately with 0 GPU memory allocated.

- [ ] **Task 1.3: Audio Subsystem Stubs**
  - **State**: `Not Implemented`
  - **Files**: `src/audio.c`, `src/game/m_bgm.c`, `pc/src/pc_platform.c`
  - **Description**: Bypass SDL2 audio device initialization and stub out `sAdo_` / `mBGM_` calls when running on the server.
  - **Acceptance Criteria**: Server boots without requesting audio hardware or generating audio threads.

- [ ] **Task 1.4: Fixed 30 Hz Server Tick Runner**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_runner.c`, `pc/include/server/server_runner.h`
  - **Description**: Implement a high-resolution, accumulator-based 30 Hz main loop (`1.0 / 30.0` seconds per tick) driving game simulation (`play_main()` logic) without being tied to VSync or display refresh rates.
  - **Acceptance Criteria**: Server maintains steady 30.0 ticks/sec across extended runs with under 1% CPU utilization on idle town.

---

### Phase 2: Simulation Unpausing & Input Isolation

- [ ] **Task 2.1: Isolate Submenu Pausing from World Simulation**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_play.c`, `src/game/m_submenu.c`
  - **Description**: In `Game_play_move_fbdemo_not_move()`, decouple `Actor_info_call_actor` and `CollisionCheck_OC` from `play->submenu.process_status == mSM_PROCESS_WAIT`. Allow actor logic to continue updating while a client has their inventory, letter, or design submenus open.
  - **Acceptance Criteria**: Opening pockets on a client does not pause other entities, villagers, fish, or time.

- [ ] **Task 2.2: Convert Global Dialogue Pausing to Per-Entity State**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_demo.c`, `src/actor/npc/ac_npc.c`, `include/m_demo.h`
  - **Description**: Deprecate global `CAMERA2_PROCESS_LOCK` and scene-wide pause during `mDemo_Request(mDemo_TYPE_TALK, ...)`. Replace with an entity-specific `is_busy` flag and `talking_player_id` assigned to the interacting NPC.
  - **Acceptance Criteria**: While one player is talking to a villager, other players can run around, catch bugs, or talk to other villagers simultaneously.

- [ ] **Task 2.3: Intercept Multi-Player Dialogue Contention**
  - **State**: `Not Implemented`
  - **Files**: `src/actor/npc/ac_npc_talk.c`, `src/game/m_demo.c`
  - **Description**: If Player $B$ attempts to talk to an NPC already engaged in dialogue with Player $A$, return an immediate polite rejection message ("Excuse me, I'm talking with [Player A] right now!") without hijacking Player $B$'s camera or movement.
  - **Acceptance Criteria**: Two players attempting to talk to the same villager do not deadlock or desync the dialogue state machine.

- [ ] **Task 2.4: Eliminate Acre Transition Lock**
  - **State**: `Not Implemented`
  - **Files**: `src/actor/ac_set_manager.c`, `src/game/m_camera2.c`
  - **Description**: Remove the camera pause and actor deallocation trigger when crossing acre coordinate seams (`x % 16 == 0`, `z % 16 == 0`).
  - **Acceptance Criteria**: Players cross acre boundaries seamlessly without screen freezes.

---

### Phase 3: Multi-Player Entity Decoupling

- [ ] **Task 3.1: Player Entity Registry & Connection Table**
  - **State**: `Not Implemented`
  - **Files**: `include/server/server_player.h`, `pc/src/server/server_player.c`
  - **Description**: Create the `ConnectedClient` registry (supporting up to 8 concurrent players). Each entry holds client ID, `PLAYER_ACTOR` instance, connection state, zone ID, and `Private_c` player data.
  - **Acceptance Criteria**: Multiple players can be registered, allocated, and tracked concurrently in memory.

- [ ] **Task 3.2: Decouple `Common_Get(now_private)` and `player_no`**
  - **State**: `Not Implemented`
  - **Files**: `include/m_common_data.h`, `src/game/m_player.c`, `src/game/m_player_lib.c`
  - **Description**: Refactor functions relying on global `now_private` or `player_no` to accept an explicit `PLAYER_ACTOR*` or `client_id` context parameter.
  - **Acceptance Criteria**: Operating on Player 2's inventory or bells does not overwrite Player 1's memory.

- [ ] **Task 3.3: Refactor Actor Distance & Nearest-Player Perception**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_actor.c`, `include/m_actor.h`
  - **Description**: In `Actor_info_call_actor()`, replace the single `player_actor` distance calculation with `Actor_find_nearest_player()`. Actors must measure `player_distance_xz` and `player_angle_y` against the closest player residing in the same zone.
  - **Acceptance Criteria**: Insects, fish, and villagers react dynamically to whichever player is closest to them.

- [ ] **Task 3.4: Multi-Player Actor-to-Actor Hitbox Collision**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_collision_obj.c`, `src/game/m_player.c`
  - **Description**: Enable collision checks between multiple `PLAYER_ACTOR` instances so players cannot clip through each other.
  - **Acceptance Criteria**: Players bump into each other and properly block movement pathways.

---

### Phase 4: Multi-Zone Simulation Engine (Outdoor + Rooms)

- [ ] **Task 4.1: Server Zone Data Structure & Manager**
  - **State**: `Not Implemented`
  - **Files**: `include/server/server_zone.h`, `pc/src/server/server_zone.c`
  - **Description**: Implement `ServerZone` management. Allocate Zone 0 as the permanent 30-acre outdoor town, and a pool of up to 16 dynamic room zones (Nook's shop, Able Sisters, Museum, Post Office, Police Station, villager houses).
  - **Acceptance Criteria**: The server can instantiate and track multiple independent simulation contexts simultaneously.

- [ ] **Task 4.2: Expand Memory Arenas for Concurrent Zones**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/pc_os.c`, `src/gamealloc.c`
  - **Description**: Increase the Dolphin OS memory arena allocation (`pc_os.c`) from 32MB to 256MB+ to allow concurrent storage of 30 acres of outdoor actors plus indoor room actors.
  - **Acceptance Criteria**: Server allocates memory for all outdoor acres and active rooms without running out of arena heap space.

- [ ] **Task 4.3: Persistent 30-Acre Outdoor Simulation**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_field_make.c`, `src/game/m_field_info.c`, `src/actor/ac_set_manager.c`
  - **Description**: Prevent outdoor actors and foreground items from unloading when players enter buildings. Outdoor field simulation must remain active in Zone 0.
  - **Acceptance Criteria**: While all players are inside houses or shops, outdoor weeds, flowers, and weather timers continue ticking uninterrupted.

- [ ] **Task 4.4: Dynamic Room Spawning & Dormancy Lifecycle**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_zone.c`, `src/game/m_scene.c`
  - **Description**: When a player interacts with a building door (`door_data`), check if the target room zone is already active. If not, instantiate it from its scene template (`src/data/scene/`). When the last player leaves a room, serialize changes and mark the zone dormant.
  - **Acceptance Criteria**: Rooms are only simulated while occupied; entering a room already occupied by another player joins that room seamlessly.

- [ ] **Task 4.5: Doorway Transition Handshake**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_scene.c`, `include/server/server_network.h`
  - **Description**: Replace the client-side `goto_other_scene()` with a server request (`PKT_C2S_INTERACT_REQ` -> `ACTION_ENTER_DOOR`). The server transfers the player entity between zones and replies with `PKT_S2C_ZONE_ENTER`.
  - **Acceptance Criteria**: Moving between outdoors and indoors updates the player's zone membership cleanly without client or server desync.

---

### Phase 5: Network Transport Layer (ENet / UDP)

- [ ] **Task 5.1: Integrate ENet Library into PC Port**
  - **State**: `Not Implemented`
  - **Files**: `pc/CMakeLists.txt`, `pc/lib/enet/`
  - **Description**: Add ENet as a vendored or CMake FetchContent dependency in `pc/CMakeLists.txt` for both client and server targets.
  - **Acceptance Criteria**: Both client and server compile and link against `enet.lib` / `libenet.a`.

- [ ] **Task 5.2: Network Packet Serialization Subsystem**
  - **State**: `Not Implemented`
  - **Files**: `include/network/net_packets.h`, `pc/src/network/net_packer.c`
  - **Description**: Implement compact binary serializers/deserializers for all packet types (`PKT_C2S_HANDSHAKE`, `PKT_C2S_INPUT`, `PKT_S2C_ENTITY_SYNC`, `PKT_S2C_FG_DELTA`, etc.) with endian safety.
  - **Acceptance Criteria**: Packet structures serialize to tightly packed byte buffers and pass round-trip unit tests.

- [ ] **Task 5.3: Server Network Listener & Client Session State Machine**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_network.c`, `pc/include/server/server_network.h`
  - **Description**: Implement ENet host creation, event polling (`ENET_EVENT_TYPE_CONNECT`, `RECEIVE`, `DISCONNECT`), connection authentication, and timeout handling.
  - **Acceptance Criteria**: Server accepts incoming client connections, tracks heartbeat ping/pong, and cleans up on client disconnect.

- [ ] **Task 5.4: Client Network Controller & Server Discovery**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/client/client_network.c`, `pc/include/client/client_network.h`
  - **Description**: Create the client-side networking manager that connects to a target server IP:port, transmits input packets, and applies server state packets to local actors.
  - **Acceptance Criteria**: Client successfully connects to a local or remote server and completes the initial handshake.

---

### Phase 6: World State Replication & Spatial Management

- [ ] **Task 6.1: Spatial Interest Grid (3x3 Acre Subscription)**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_spatial.c`, `pc/include/server/server_spatial.h`
  - **Description**: Implement the 3×3 acre interest calculation based on each client's current acre coordinate. Filter entity broadcasts so clients only receive updates relevant to their immediate surroundings.
  - **Acceptance Criteria**: Actions occurring in Acre A-1 are not broadcast to players in Acre F-5, saving bandwidth.

- [ ] **Task 6.2: High-Frequency Transform Sync (Unreliable Channel)**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_sync.c`, `pc/src/client/client_sync.c`
  - **Description**: Stream player and mobile entity positions, yaw rotation, velocity, and animation indices at 20–30 Hz over ENet Channel 0. Implement client-side linear interpolation ($lerp$) to ensure smooth visual movement.
  - **Acceptance Criteria**: Remote players move smoothly on screen without jitter or rubber-banding.

- [ ] **Task 6.3: Authoritative Foreground Item (`Save.fg`) Mutations**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_field_make.c`, `pc/src/server/server_world.c`
  - **Description**: Handle digging holes, burying items, planting seeds/trees, picking fruit, picking flowers, and cutting trees authoritatively on the server. Broadcast `PKT_S2C_FG_DELTA` to update all nearby clients.
  - **Acceptance Criteria**: When Player 1 digs a hole or chops a tree, the modification appears instantaneously on Player 2's screen.

- [ ] **Task 6.4: Economy & Shop Synchronization**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_shop.c`, `src/actor/npc/ac_npc_shop_master.c`
  - **Description**: Synchronize Nook's shelf stock and purchase limits. When an item is purchased, the shelf slot is marked sold-out across all clients currently in the shop.
  - **Acceptance Criteria**: Two players cannot purchase the same limited daily item from Nook's store.

- [ ] **Task 6.5: Dynamic Entity Lifecycle Synchronization (Bugs & Fish)**
  - **State**: `Not Implemented`
  - **Files**: `src/actor/ac_insect.c`, `src/actor/ac_gyoei.c`, `src/game/m_actor.c`
  - **Description**: Synchronize fish shadows and insect spawns/despawns from the server. When any player catches or scares a fish/bug, broadcast the despawn event to all clients.
  - **Acceptance Criteria**: Both players see the same fish shadow swimming in the river; one player catching it removes it for both players.

---

### Phase 7: World Persistence & Save Journaling

- [ ] **Task 7.1: Master Town Save Loader & Periodic Snapshotting**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_storage.c`, `src/game/m_flashrom.c`
  - **Description**: Implement master town save loading from `save/town_master.sav`. Run periodic atomic flushes every 5 minutes and upon clean shutdown.
  - **Acceptance Criteria**: Server restarts reload exact town condition, villager roster, and planted flora.

- [ ] **Task 7.2: Write-Ahead Delta Journaling**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_journal.c`, `pc/include/server/server_journal.h`
  - **Description**: Append every tile modification, inventory trade, and villager dialogue milestone to an append-only log (`save/journal.log`). On crash recovery, replay the journal against the last master snapshot.
  - **Acceptance Criteria**: Abruptly terminating the server process preserves all tile and item changes made prior to termination.

- [ ] **Task 7.3: Individual Player Data Persistence**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_player_storage.c`
  - **Description**: Save each connecting player's pockets, bells, debts, inventory, and custom designs into separate files (`save/players/<uuid>.dat`) to allow character portability across different dedicated servers.
  - **Acceptance Criteria**: Players retain their personal inventory, clothes, and wallet across disconnects and server transitions.
