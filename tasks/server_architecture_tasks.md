# Tasks: Authoritative Dedicated Server Architecture

This task tracker outlines all implementation milestones defined in [Specification 001: Authoritative Dedicated Server Architecture](file:///C:/Source/ACGC-PC-Port/specs/SERVER_ARCHITECTURE.md).

---

## Task Summary Dashboard

| Phase | Category | Completed | Total | Status |
| :--- | :--- | :---: | :---: | :--- |
| **Phase 1** | Headless Build Target & Dedicated Server Daemon | 4 | 4 | Implemented |
| **Phase 2** | Procedural Map Generation & World Initialization (Save Bootstrap) | 4 | 4 | Implemented |
| **Phase 3** | Simulation Unpausing & Input Isolation | 0 | 4 | Not Implemented |
| **Phase 4** | Multi-Player Entity Decoupling | 0 | 5 | Not Implemented |
| **Phase 5** | Multi-Zone Simulation Engine (Outdoor + Rooms) | 0 | 5 | Not Implemented |
| **Phase 6** | Network Transport Layer (ENet / UDP) | 0 | 4 | Not Implemented |
| **Phase 7** | World State Replication & Spatial Management | 0 | 5 | Not Implemented |
| **Phase 8** | World Persistence & Save Journaling | 0 | 3 | Not Implemented |
| **Total** | | **8** | **34** | **24% Complete** |

---

## Detailed Task Breakdown

### Phase 1: Headless Build Target & Dedicated Server Daemon

- [x] **Task 1.1: CMake Build Target for Headless Server**
  - **State**: `Implemented`
  - **Files**: `pc/CMakeLists.txt`, `pc/src/server/server_main.c`
  - **Description**: Add a `BUILD_SERVER=ON` CMake configuration option defining `-DTARGET_SERVER -DHEADLESS`. Create `pc/src/server/server_main.c` as the dedicated server binary entry point.
  - **Acceptance Criteria**: Running `cmake --build . --target AnimalCrossingServer` compiles a standalone CLI executable without requiring SDL2 display windows or OpenGL contexts.

- [x] **Task 1.2: Graphics & GX Subsystem Stubs**
  - **State**: `Implemented`
  - **Files**: `pc/src/pc_gx.c`, `pc/src/pc_gx_tev.c`, `pc/src/pc_gx_texture.c`, `src/graph.c`
  - **Description**: Add conditional `#ifndef TARGET_SERVER` guards around all OpenGL rendering, shader compilation, and GX display list submission. When running in server mode, vertex submission and texture binding must be instant no-ops.
  - **Acceptance Criteria**: Graphics initialization returns success immediately with 0 GPU memory allocated.

- [x] **Task 1.3: Audio Subsystem Stubs**
  - **State**: `Implemented`
  - **Files**: `pc/src/pc_audio.c`, `src/graph.c`, `src/second_game.c`
  - **Description**: Bypass SDL2 audio device initialization and stub out `sAdo_` / `mBGM_` calls when running on the server.
  - **Acceptance Criteria**: Server boots without requesting audio hardware or generating audio threads.

- [x] **Task 1.4: Fixed 30 Hz Server Tick Runner**
  - **State**: `Implemented`
  - **Files**: `pc/src/server/server_runner.c`, `pc/include/server/server_runner.h`, `pc/src/pc_vi.c`
  - **Description**: Implement a high-resolution, accumulator-based 30 Hz main loop (`1.0 / 30.0` seconds per tick) driving game simulation (`play_main()` logic) without being tied to VSync or display refresh rates.
  - **Acceptance Criteria**: Server maintains steady 30.0 ticks/sec across extended runs with under 1% CPU utilization on idle town.

---

### Phase 2: Procedural Map Generation & World Initialization (Save Bootstrap)

- [x] **Task 2.1: Save Data Absence Detection & CLI Bootstrap Flags**
  - **State**: `Implemented`
  - **Files**: `src/game/m_play.c`, `pc/src/pc_m_card.c`, `pc/src/server/server_main.c`
  - **Description**: During startup, check for existence and integrity of save data via `mFRm_CheckSaveData()`. If missing or uninitialized, trigger the autonomous world generation pipeline before scene read.
  - **Acceptance Criteria**: Starting the server without an existing save file logs a bootstrap notice and generates a new world without crashing or hanging.

- [x] **Task 2.2: Headless Procedural Acre Generation (`mRF_MakeRandomField`)**
  - **State**: `Implemented`
  - **Files**: `src/game/m_random_field.c`, `src/game/m_start_data_init.c`, `src/game/m_play.c`
  - **Description**: Execute procedural acre generation headlessly to generate the full acre grid, cliffs, rivers, waterfalls, bridges, and infrastructure acres.
  - **Acceptance Criteria**: `Save_Get(combi_table)` contains a completely valid, traversal-verified acre layout adhering to original GameCube placement rules.

- [x] **Task 2.3: Foreground Item Matrix & Flora Population (`mFM_InitFgCombiSaveData`)**
  - **State**: `Implemented`
  - **Files**: `src/game/m_field_make.c`, `src/game/m_start_data_init.c`, `src/game/m_play.c`
  - **Description**: Run `mFM_InitFgCombiSaveData()` to populate each acre tile grid with starting trees, rocks, signboards, flowers, and weed patches. Roll town native fruit, cedar trees, and buried items.
  - **Acceptance Criteria**: `Save_Get(fg)` contains a fully furnished 30-acre foreground item matrix with native fruit trees, rocks, and buried items.

- [x] **Task 2.4: Town Identity, Initial Villager Roster & Island Seeding**
  - **State**: `Implemented`
  - **Files**: `src/game/m_start_data_init.c`, `pc/src/pc_m_card.c`, `src/game/m_play.c`
  - **Description**: Assign unique Land ID, town tune melody, and grass pattern. Roll initial starting villagers with balanced personalities, assign their homes, seed island, and write the completed save directly to `save/card_a/DobutsunomoriP_MURA.gci`.
  - **Acceptance Criteria**: The newly generated town is immediately saved to disk; subsequent server reboots successfully load and recognize the town.

---

### Phase 3: Simulation Unpausing & Input Isolation

- [x] **Task 3.1: Isolate Submenu Pausing from World Simulation**
  - **State**: `Implemented`
  - **Files**: `src/game/m_play.c`, `src/game/m_submenu.c`, `src/game/m_player_controller.c_inc`, `src/game/m_player_other_func.c_inc`, `src/game/m_player.c`
  - **Description**: In `Game_play_move_fbdemo_not_move()`, decouple `Actor_info_call_actor` and `CollisionCheck_OC` from `play->submenu.process_status == mSM_PROCESS_WAIT`. Allow actor logic to continue updating while a player has their inventory, letter, or design submenus open.
  - **Acceptance Criteria (Testable in Client)**: Launch `./pc/build32/bin/AnimalCrossing.exe` and open pockets/inventory next to a river or walking villager. River water, tree sway, wind, and nearby villagers must continue animating and walking in real-time while the menu is open, instead of freezing.

- [x] **Task 3.2: Convert Global Dialogue Pausing to Per-Entity State**
  - **State**: `Implemented`
  - **Files**: `src/game/m_demo.c`, `src/actor/npc/ac_npc.c`, `include/m_demo.h`, `include/ac_npc.h`, `src/actor/npc/ac_npc_talk.c_inc`
  - **Description**: Deprecate global `CAMERA2_PROCESS_LOCK` and scene-wide pause during `mDemo_Request(mDemo_TYPE_TALK, ...)`. Replace with an entity-specific `is_busy` flag and `talking_player_id` assigned to the interacting NPC.
  - **Acceptance Criteria (Testable in Client)**: Initiate dialogue with a villager or inspect a signboard. Background environmental effects, river animations, and other non-engaged villagers in the acre must continue moving normally rather than locking the entire scene.

- [x] **Task 3.3: Intercept Multi-Player Dialogue Contention**
  - **State**: `Implemented`
  - **Files**: `src/actor/npc/ac_npc_talk.c_inc`, `src/game/m_demo.c`, `src/actor/ac_quest_manager.c`
  - **Description**: If Player $B$ attempts to talk to an NPC already engaged in dialogue with Player $A$, return an immediate polite rejection message ("Excuse me, I'm talking with someone right now!") without hijacking Player $B$'s camera or movement.
  - **Acceptance Criteria (Testable via State Check)**: Attempting to trigger dialogue on an already-busy NPC returns a non-blocking busy response and never locks the interacting entity's controls or camera.

- [x] **Task 3.4: Eliminate Acre Transition Lock**
  - **State**: `Implemented`
  - **Files**: `src/actor/ac_set_manager.c`, `src/game/m_camera2.c`, `src/actor/ac_birth_control.c`, `src/game/m_actor.c`, `pc/src/server/server_main.c`
  - **Description**: Remove the camera pause and actor deallocation trigger when crossing acre coordinate seams (`x % 16 == 0`, `z % 16 == 0`).
  - **Acceptance Criteria (Testable in Client)**: Walking across acre boundaries in the client proceeds seamlessly without screen pauses or camera freeze stutters.

---

### Phase 4: Multi-Player Entity Decoupling

- [x] **Task 4.1: Player Entity Registry & Connection Table**
  - **State**: `Implemented`
  - **Files**: `include/server/server_player.h`, `pc/include/server/server_player.h`, `pc/src/server/server_player.c`, `pc/src/server/server_main.c`, `pc/src/pc_main.c`, `src/game/m_play.c`, `src/game/m_player.c`, `src/game/m_player_main_wait.c_inc`, `include/m_player.h`
  - **Description**: Create the `ConnectedClient` registry (supporting up to 8 concurrent players). Each entry holds client ID, `PLAYER_ACTOR` instance, connection state, zone ID, and `Private_c` player data.
  - **Acceptance Criteria (Testable via Simulated Player)**: Server supports adding a simulated/dummy player (`--spawn-dummy-player` or test harness). The server logs confirm both Player 1 and Player 2 are allocated, tracked, and simulated concurrently in memory.

- [ ] **Task 4.2: Decouple `Common_Get(now_private)` and `player_no`**
  - **State**: `Not Implemented`
  - **Files**: `include/m_common_data.h`, `src/game/m_player.c`, `src/game/m_player_lib.c`
  - **Description**: Refactor functions relying on global `now_private` or `player_no` to accept an explicit `PLAYER_ACTOR*` or `client_id` context parameter.
  - **Acceptance Criteria (Testable via Memory Isolation)**: Modifying bells, pockets, or cloth for Player 2 does not alter or corrupt Player 1's memory space.

- [ ] **Task 4.3: Refactor Actor Distance & Nearest-Player Perception**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_actor.c`, `include/m_actor.h`
  - **Description**: In `Actor_info_call_actor()`, replace the single `player_actor` distance calculation with `Actor_find_nearest_player()`. Actors must measure `player_distance_xz` and `player_angle_y` against the closest player residing in the same zone.
  - **Acceptance Criteria (Testable via Distance Check)**: When two players are in the world, insects, fish, and villagers react dynamically to whichever player is physically closer to them.

- [ ] **Task 4.4: Multi-Player Actor-to-Actor Hitbox Collision**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_collision_obj.c`, `src/game/m_player.c`
  - **Description**: Enable collision checks between multiple `PLAYER_ACTOR` instances so players cannot clip through each other.
  - **Acceptance Criteria (Testable in Game/Simulation)**: Player 1 running into the dummy player bumps into their physical collision cylinder and is stopped from walking through them.

- [x] **Task 4.5: Player Onboarding & Part-Time Job (Tutorial) Bypass**
  - **State**: `Implemented`
  - **Files**: `src/game/m_event.c`, `src/game/m_start_data_init.c`, `src/game/m_play.c`, `pc/src/pc_m_card.c`, `src/game/m_card.c`, `pc/src/pc_settings.c`
  - **Description**: Automatically skip Tom Nook's Part-Time Job (`FirstJob` / `arbeit`) upon onboarding new players. Clear tutorial event flags via `mEv_ClearPersonalEventFlag(player_no)`, assign an available home slot with default starter items (tape deck, wallpaper, flooring), set mortgage to post-tutorial 17,000 Bells (`mPlayer_DEBT0 - 1400`), provide starter bells and random civilian clothes, clear errand queues, and set Tom Nook's shop to normal commerce (`SP_NPC_SHOP_MASTER`).
  - **Acceptance Criteria (Testable via State Check & In-Game)**: When a new player character is initialized in multiplayer:
    1. `mEv_CheckFirstJob()` evaluates to `FALSE`.
    2. Hourly town BGM plays instead of `BGM_FIRST_JOB`.
    3. Entering Tom Nook's shop presents standard buying/selling rather than chore dialogues.
    4. Villagers adhere to normal wandering/indoor schedules rather than waiting outside for deliveries.
    5. Player has home ownership with a starter radio and an active 17,000 Bell mortgage.

---

### Phase 5: Multi-Zone Simulation Engine (Outdoor + Rooms)

- [ ] **Task 5.1: Server Zone Data Structure & Manager**
  - **State**: `Not Implemented`
  - **Files**: `include/server/server_zone.h`, `pc/src/server/server_zone.c`
  - **Description**: Implement `ServerZone` management. Allocate Zone 0 as the permanent 30-acre outdoor town, and a pool of dynamic room zones (Nook's shop, Able Sisters, Museum, Post Office, Police Station, villager houses).
  - **Acceptance Criteria (Testable via Server Logs)**: Server instantiates and maintains Zone 0 (outdoors) alongside active indoor room zones simultaneously without crashing.

- [ ] **Task 5.2: Expand Memory Arenas for Concurrent Zones**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/pc_os.c`, `src/gamealloc.c`
  - **Description**: Increase the Dolphin OS memory arena allocation (`pc_os.c`) from 32MB to 256MB+ to allow concurrent storage of 30 acres of outdoor actors plus indoor room actors.
  - **Acceptance Criteria (Testable Heap Inspection)**: Memory diagnostics confirm sufficient headroom (>128MB free) while outdoor acres and multiple interior rooms are concurrently allocated.

- [ ] **Task 5.3: Persistent 30-Acre Outdoor Simulation**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_field_make.c`, `src/game/m_field_info.c`, `src/actor/ac_set_manager.c`
  - **Description**: Prevent outdoor actors and foreground items from unloading when players enter buildings. Outdoor field simulation must remain active in Zone 0.
  - **Acceptance Criteria (Testable Concurrent Ticking)**: When a player enters a house, server logs confirm Zone 0 (outdoor weather, tree growth, villager wandering) continues ticking in the background.

- [ ] **Task 5.4: Dynamic Room Spawning & Dormancy Lifecycle**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_zone.c`, `src/game/m_scene.c`
  - **Description**: When a player interacts with a building door (`door_data`), check if the target room zone is already active. If not, instantiate it from its scene template (`src/data/scene/`). When the last player leaves a room, serialize changes and mark the zone dormant.
  - **Acceptance Criteria (Testable Room Joining)**: Entering an already-occupied room joins the existing room zone rather than creating a duplicate instance.

- [ ] **Task 5.5: Doorway Transition Handshake**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_scene.c`, `include/server/server_network.h`
  - **Description**: Replace the client-side `goto_other_scene()` with a server request (`PKT_C2S_INTERACT_REQ` -> `ACTION_ENTER_DOOR`). The server transfers the player entity between zones and replies with `PKT_S2C_ZONE_ENTER`.
  - **Acceptance Criteria (Testable Zone Transfer)**: Moving through doors updates the player's zone membership cleanly without client or server desync.

---

### Phase 6: Network Transport Layer (ENet / UDP)

- [ ] **Task 6.1: Integrate ENet Library into PC Port**
  - **State**: `Not Implemented`
  - **Files**: `pc/CMakeLists.txt`, `pc/lib/enet/`
  - **Description**: Add ENet as a vendored or CMake FetchContent dependency in `pc/CMakeLists.txt` for both client and server targets.
  - **Acceptance Criteria**: Both client and server compile and link against `enet.lib` / `libenet.a`.

- [ ] **Task 6.2: Network Packet Serialization Subsystem**
  - **State**: `Not Implemented`
  - **Files**: `include/network/net_packets.h`, `pc/src/network/net_packer.c`
  - **Description**: Implement compact binary serializers/deserializers for all packet types (`PKT_C2S_HANDSHAKE`, `PKT_C2S_INPUT`, `PKT_S2C_ENTITY_SYNC`, `PKT_S2C_FG_DELTA`, etc.) with endian safety.
  - **Acceptance Criteria**: Packet structures serialize to tightly packed byte buffers and pass round-trip unit tests.

- [ ] **Task 6.3: Server Network Listener & Client Session State Machine**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_network.c`, `pc/include/server/server_network.h`
  - **Description**: Implement ENet host creation, event polling (`ENET_EVENT_TYPE_CONNECT`, `RECEIVE`, `DISCONNECT`), connection authentication, and timeout handling.
  - **Acceptance Criteria**: Server accepts incoming client connections, tracks heartbeat ping/pong, and cleans up on client disconnect.

- [ ] **Task 6.4: Client Network Controller & Server Discovery**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/client/client_network.c`, `pc/include/client/client_network.h`
  - **Description**: Create the client-side networking manager that connects to a target server IP:port, transmits input packets, and applies server state packets to local actors.
  - **Acceptance Criteria**: Client successfully connects to a local or remote server and completes the initial handshake.

---

### Phase 7: World State Replication & Spatial Management

- [ ] **Task 7.1: Spatial Interest Grid (3x3 Acre Subscription)**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_spatial.c`, `pc/include/server/server_spatial.h`
  - **Description**: Implement the 3×3 acre interest calculation based on each client's current acre coordinate. Filter entity broadcasts so clients only receive updates relevant to their immediate surroundings.
  - **Acceptance Criteria**: Actions occurring in Acre A-1 are not broadcast to players in Acre F-5, saving bandwidth.

- [ ] **Task 7.2: High-Frequency Transform Sync (Unreliable Channel)**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_sync.c`, `pc/src/client/client_sync.c`
  - **Description**: Stream player and mobile entity positions, yaw rotation, velocity, and animation indices at 20–30 Hz over ENet Channel 0. Implement client-side linear interpolation ($lerp$) to ensure smooth visual movement.
  - **Acceptance Criteria**: Remote players move smoothly on screen without jitter or rubber-banding.

- [ ] **Task 7.3: Authoritative Foreground Item (`Save.fg`) Mutations**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_field_make.c`, `pc/src/server/server_world.c`
  - **Description**: Handle digging holes, burying items, planting seeds/trees, picking fruit, picking flowers, and cutting trees authoritatively on the server. Broadcast `PKT_S2C_FG_DELTA` to update all nearby clients.
  - **Acceptance Criteria**: When Player 1 digs a hole or chops a tree, the modification appears instantaneously on Player 2's screen.

- [ ] **Task 7.4: Economy & Shop Synchronization**
  - **State**: `Not Implemented`
  - **Files**: `src/game/m_shop.c`, `src/actor/npc/ac_npc_shop_master.c`
  - **Description**: Synchronize Nook's shelf stock and purchase limits. When an item is purchased, the shelf slot is marked sold-out across all clients currently in the shop.
  - **Acceptance Criteria**: Two players cannot purchase the same limited daily item from Nook's store.

- [ ] **Task 7.5: Dynamic Entity Lifecycle Synchronization (Bugs & Fish)**
  - **State**: `Not Implemented`
  - **Files**: `src/actor/ac_insect.c`, `src/actor/ac_gyoei.c`, `src/game/m_actor.c`
  - **Description**: Synchronize fish shadows and insect spawns/despawns from the server. When any player catches or scares a fish/bug, broadcast the despawn event to all clients.
  - **Acceptance Criteria**: Both players see the same fish shadow swimming in the river; one player catching it removes it for both players.

---

### Phase 8: World Persistence & Save Journaling

- [ ] **Task 8.1: Master Town Save Loader & Periodic Snapshotting**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_storage.c`, `src/game/m_flashrom.c`
  - **Description**: Implement master town save loading from `save/town_master.sav`. Run periodic atomic flushes every 5 minutes and upon clean shutdown.
  - **Acceptance Criteria**: Server restarts reload exact town condition, villager roster, and planted flora.

- [ ] **Task 8.2: Write-Ahead Delta Journaling**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_journal.c`, `pc/include/server/server_journal.h`
  - **Description**: Append every tile modification, inventory trade, and villager dialogue milestone to an append-only log (`save/journal.log`). On crash recovery, replay the journal against the last master snapshot.
  - **Acceptance Criteria**: Abruptly terminating the server process preserves all tile and item changes made prior to termination.

- [ ] **Task 8.3: Individual Player Data Persistence**
  - **State**: `Not Implemented`
  - **Files**: `pc/src/server/server_player_storage.c`
  - **Description**: Save each connecting player's pockets, bells, debts, inventory, and custom designs into separate files (`save/players/<uuid>.dat`) to allow character portability across different dedicated servers.
  - **Acceptance Criteria**: Players retain their personal inventory, clothes, and wallet across disconnects and server transitions.
