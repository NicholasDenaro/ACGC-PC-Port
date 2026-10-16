# ACGC PC Port - Repository Guidelines & MCP Tool Protocol

This document defines the core operational rules, tool protocols, and diagnostic standards for AI assistants working in this repository.

---

## 1. Pinned Diagnostic Log Files

All runtime diagnostics in this repository are categorized into three pinned log files:

| Pinned Target | Associated File | Primary Location | Purpose / Contents |
| :--- | :--- | :--- | :--- |
| **`"client"`** (or `"game"`) | `game.log` | `pc/build32/bin/game.log` | PC client runtime logs: OpenGL renderer, textures, sound heap, asset loading, GameCube OSReport diagnostics. |
| **`"server"`** | `server.log` | `pc/build32/bin/server.log` | Headless dedicated server runtime logs: socket initialization, client connections, packet dispatch. |
| **`"player"`** (or `"server_player"`) | `server_player.log` | `pc/build32/bin/server_player.log` | Multiplayer simulation: zone transfers, position/velocity sync, outdoor zone persistence, NPC room dormancy. |

### Rules for Inspecting Logs
1. **Always** use the `read_game_logs` tool from the `acgc-tools` MCP server.
2. **Never** execute shell commands (`Get-Content`, `type`, `cat`, `grep`) to search for or read log files.
3. **Never** pass or request a `custom_path` for these logs. The three pinned targets (`"client"`, `"server"`, `"player"`) cover all diagnostic logging in this repo.

---

## 2. MCP Tools Reference & Usage Matrix

The `acgc-tools` MCP server provides specialized tools tailored for this codebase. Always prefer these tools over manual terminal commands:

### `read_game_logs`
- **When to use**: Whenever diagnosing runtime errors, crashes, state transitions, scene changes, or verifying gameplay events.
- **Parameters**:
  - `target` (*string*): `"client"`, `"server"`, or `"player"`. Defaults to `"client"`.
  - `search_query` (*string*, optional): String or regex pattern to search for in the log.
  - `filter_pattern` (*string*, optional): Alias for `search_query` (substring filter).
  - `context_lines` (*integer*, optional): Number of context lines before and after each match (like `grep -C`). Defaults to 0.
  - `show_line_numbers` (*boolean*, optional): Prefix lines with 1-indexed line numbers. Defaults to true when searching.
  - `case_sensitive` (*boolean*, optional): Case-sensitive matching. Defaults to false.
  - `is_regex` (*boolean*, optional): Regular expression matching. Defaults to false.
  - `max_matches` (*integer*, optional): Maximum match groups to return with context. Defaults to 50.
  - `max_lines` (*integer*): Number of recent lines to tail (e.g., 50 or 100). Default is 100. Set to 0 for all lines.
  - `clear` (*boolean*, optional): Set to `true` before a test run to clear out old log noise.
- **Examples**:
  ```json
  // Read recent client logs:
  call_mcp_tool(ServerName="acgc-tools", ToolName="read_game_logs", Arguments={"target": "client", "max_lines": 50})

  // Search player/zone logs with surrounding context:
  call_mcp_tool(ServerName="acgc-tools", ToolName="read_game_logs", Arguments={"target": "player", "search_query": "RESUME DOOR", "context_lines": 5})
  ```

### `compile_check`
- **When to use**: After editing any C/C++ source or header file. Always verify compilation before claiming a fix is ready.
- **What it provides**: Runs Ninja against the 32-bit MSYS2/MinGW toolchain (`pc/build32`), checking syntax and linkability of `AnimalCrossing.exe` and `AnimalCrossingServer.exe`.
- **Parameters**: None required (or optional `target` string like `"ac_pc"` or `"ac_server"`).
- **Rule**: Never run `gcc` or `ninja` manually via terminal. Always call `compile_check`.

### `build_game`
- **When to use**: When performing a full rebuild or when asset compilation / shader copies are required.
- **What it provides**: Builds both client and server executables, ensures asset bundles are refreshed, and sets up dependencies.
- **Parameters**: `clean` (*boolean*, optional): Set `true` for a clean rebuild.

### `run_server_test`
- **When to use**: To verify dedicated server stability, zone initialization, and headless packet handling without launching a GUI.
- **What it provides**: Starts `AnimalCrossingServer.exe` in headless mode for a specified duration, captures output, and verifies that zones initialize without crash.
- **Parameters**:
  - `timeout_seconds` (*integer*): Run duration before graceful termination (default: 5).
  - `extra_args` (*array of strings*, optional): Extra CLI flags.

### `git_status`
- **When to use**: Before committing, after making changes, or when inspecting diffs.
- **What it provides**: Clean workspace status or diffs without terminal paging issues.
- **Parameters**:
  - `diff` (*boolean*): Set `true` to get a `git diff`.
  - `files` (*array of strings*, optional): List of files to restrict the diff to.

### `search_code`
- **When to use**: When searching the codebase for symbols, functions, variables, or text patterns without running terminal grep/Select-String commands.
- **What it provides**: Searches C/C++ source, headers, and configs, returning matched files, line numbers, and optional surrounding context blocks.
- **Parameters**:
  - `query` (*string*, required): String or regex pattern to search for in source code.
  - `path` (*string*, optional): Subdirectory or file path to search within (e.g., `"src/game"`, `"include"`, `"src/game/m_field_info.c"`). Defaults to repository root.
  - `file_pattern` (*string*, optional): Glob pattern for file matching (e.g., `"*.c"`, `"*.h"`, `"*.c_inc"`).
  - `context_lines` (*integer*, optional): Lines of context before and after each match (like `grep -C`). Defaults to 0.
  - `case_sensitive` (*boolean*, optional): Case-sensitive matching. Defaults to false.
  - `is_regex` (*boolean*, optional): Regular expression matching. Defaults to false.
  - `max_matches` (*integer*, optional): Maximum matching lines to return. Defaults to 50.
- **Examples**:
  ```json
  call_mcp_tool(ServerName="acgc-tools", ToolName="search_code", Arguments={"query": "mFI_InitRegisterBgInfo", "path": "src/game"})
  ```

---

## 3. General Development Rules

1. **No Background GUI Binaries**: Never launch `AnimalCrossing.exe` directly via background shell commands. Always instruct the user to launch and test interactive gameplay.
2. **Architecture & Decomp Preservation**:
   - The root `src/` directory contains GameCube decompilation source. Modifications must be guarded with `#ifdef TARGET_PC` or `#ifdef TARGET_SERVER`.
   - PC-specific harness and server networking code belong in `pc/src/`.
   - Preserve all existing comments, docstrings, and formatting.
