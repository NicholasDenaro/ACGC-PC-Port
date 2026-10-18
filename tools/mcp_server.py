#!/usr/bin/env python3
"""
mcp_server.py - Zero-dependency Model Context Protocol (MCP) server for ACGC-PC-Port.

Provides native tools for:
  - build_game: Builds Animal Crossing client and/or dedicated server via Ninja
  - compile_check: Rapidly verifies compilation of modified C/C++ files
  - run_server_test: Runs the dedicated server in test mode and checks for crashes
  - git_status: Queries repository status and diffs safely
  - read_game_logs: Reads and filters client (game.log) or server (server.log) runtime logs
  - search_code: Searches codebase for literal strings or regex patterns across files
"""

import sys
import os
import json
import subprocess
import threading
import time
import re
import fnmatch

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.path.join(PROJECT_ROOT, "pc", "build32")
BIN_DIR = os.path.join(BUILD_DIR, "bin")
CLIENT_EXE = os.path.join(BIN_DIR, "AnimalCrossing.exe")
SERVER_EXE = os.path.join(BIN_DIR, "AnimalCrossingServer.exe")
MSYS2_SHELL = "C:/msys64/msys2_shell.cmd"

def run_msys2(bash_cmd, timeout=300, cwd=PROJECT_ROOT):
    """Executes a command inside the official MSYS2 MINGW32 shell environment."""
    cmd = [
        MSYS2_SHELL,
        "-defterm",
        "-here",
        "-no-start",
        "-mingw32",
        "-c",
        bash_cmd
    ]
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace"
        )
        stdout, _ = proc.communicate(timeout=timeout)
        return proc.returncode, stdout
    except subprocess.TimeoutExpired:
        proc.kill()
        return -1, f"Command timed out after {timeout} seconds."
    except Exception as e:
        return -1, f"Failed to execute command: {e}"

# --- Tool Implementations ---

def tool_build_game(target="all", clean=False):
    """Builds client, server, or both using the MSYS2 MINGW32 shell and ./build_pc.sh."""
    output = []

    if clean:
        code, out = run_msys2("ninja -C pc/build32 clean")
        output.append("=== Ninja Clean ===")
        output.append(out)
        if code != 0:
            return {"success": False, "output": "\n".join(output)}

    if target == "client":
        bash_cmd = "ninja -C pc/build32 bin/AnimalCrossing.exe"
    elif target == "server":
        bash_cmd = "ninja -C pc/build32 bin/AnimalCrossingServer.exe"
    else:
        # Canonical full build
        bash_cmd = "./build_pc.sh"

    code, out = run_msys2(bash_cmd, timeout=300)
    output.append(f"=== Build (target: {target}, command: {bash_cmd}) ===")
    output.append(out)

    success = (code == 0)
    return {
        "success": success,
        "return_code": code,
        "output": "\n".join(output)
    }

def tool_compile_check(files=None):
    """Checks compilation of specific files or pending changes in MINGW32 shell."""
    if files:
        targets = []
        for f in files:
            norm = f.replace("\\", "/").strip()
            if norm.startswith("src/"):
                targets.append(f"CMakeFiles/ac_pc.dir/C_/Source/ACGC-PC-Port/{norm}.obj")
                targets.append(f"CMakeFiles/ac_server.dir/C_/Source/ACGC-PC-Port/{norm}.obj")
            elif norm.startswith("pc/src/"):
                sub = norm[len("pc/"):]
                targets.append(f"CMakeFiles/ac_pc.dir/{sub}.obj")
                targets.append(f"CMakeFiles/ac_server.dir/{sub}.obj")
            else:
                targets.append(norm)

        bash_cmd = f"ninja -C pc/build32 {' '.join(targets)}"
    else:
        bash_cmd = "ninja -C pc/build32"

    code, out = run_msys2(bash_cmd, timeout=180)
    return {
        "success": (code == 0),
        "return_code": code,
        "output": out
    }

def tool_run_server_test(timeout_seconds=5, extra_args=None, debug=False):
    """Runs the headless dedicated server temporarily to verify startup/zones."""
    if not os.path.exists(SERVER_EXE):
        return {
            "success": False,
            "output": f"Server binary not found at {SERVER_EXE}. Please build it first."
        }

    args_str = " ".join(extra_args) if extra_args else ""

    if debug:
        gdb_cmd = f"gdb --batch -ex 'set pagination off' -ex 'run {args_str}' -ex 'bt' -ex 'quit' ./AnimalCrossingServer.exe"
        code, out = run_msys2(gdb_cmd, timeout=timeout_seconds + 30, cwd=BIN_DIR)
        return {
            "success": (code == 0),
            "debug": True,
            "return_code": code,
            "output": out
        }

    cmd = [SERVER_EXE]
    if extra_args:
        cmd.extend(extra_args)

    merged_env = os.environ.copy()
    merged_env["PATH"] = "C:\\msys64\\mingw32\\bin;" + merged_env.get("PATH", "")

    try:
        proc = subprocess.Popen(
            cmd,
            cwd=BIN_DIR,
            env=merged_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace"
        )
        
        # Wait up to timeout_seconds
        try:
            stdout, _ = proc.communicate(timeout=timeout_seconds)
            # If server stopped on its own
            code = proc.returncode
            res = {
                "success": (code == 0),
                "terminated_naturally": True,
                "return_code": code,
                "output": stdout
            }
            if code != 0:
                # Extract addresses from crash report and resolve with addr2line
                crash_addrs = re.findall(r'\[0x([0-9a-fA-F]+)\]', stdout)
                fault = re.search(r'Fault Address:\s*([0-9a-fA-F]+)', stdout)
                if fault and fault.group(1) not in crash_addrs:
                    crash_addrs.insert(0, fault.group(1))
                if crash_addrs:
                    addrs_str = " ".join(f"0x{a}" for a in crash_addrs[:10])
                    acode, aout = run_msys2(f"addr2line -e ./AnimalCrossingServer.exe -f -C -p {addrs_str}", timeout=15, cwd=BIN_DIR)
                    res["resolved_crash_stack"] = aout
                else:
                    gdb_cmd = f"gdb --batch -ex 'set pagination off' -ex 'run {args_str}' -ex 'bt' -ex 'quit' ./AnimalCrossingServer.exe"
                    gcode, gdb_out = run_msys2(gdb_cmd, timeout=30, cwd=BIN_DIR)
                    res["gdb_backtrace"] = gdb_out
            return res
        except subprocess.TimeoutExpired:
            # Server is alive and running as expected!
            proc.terminate()
            try:
                stdout, _ = proc.communicate(timeout=2)
            except Exception:
                proc.kill()
                stdout, _ = proc.communicate()
            
            return {
                "success": True,
                "terminated_naturally": False,
                "note": f"Server was running stably for {timeout_seconds} seconds before being gracefully stopped.",
                "output": stdout
            }
    except Exception as e:
        return {
            "success": False,
            "error": str(e)
        }

def tool_git_status(diff=False, files=None):
    """Queries git status or diff for the workspace."""
    cmd = ["git"]
    if diff:
        cmd.append("diff")
        if files:
            cmd.extend(files)
    else:
        cmd.extend(["status", "--short"])

    try:
        proc = subprocess.Popen(
            cmd,
            cwd=PROJECT_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace"
        )
        stdout, _ = proc.communicate(timeout=30)
        return {
            "success": (proc.returncode == 0),
            "output": stdout
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to execute git: {e}"
        }

def tool_get_game_status():
    """Queries running processes to check if AnimalCrossing.exe or AnimalCrossingServer.exe are active."""
    results = {
        "client": {"running": False},
        "server": {"running": False}
    }
    try:
        proc = subprocess.Popen(
            ["tasklist", "/FO", "CSV", "/V", "/FI", "IMAGENAME eq AnimalCrossing*"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace"
        )
        stdout, _ = proc.communicate(timeout=10)
        lines = [line.strip() for line in stdout.splitlines() if line.strip()]
        if lines:
            import csv
            reader = csv.reader(lines)
            rows = list(reader)
            if rows and len(rows) > 0 and "Image Name" in rows[0]:
                headers = rows[0]
                for row in rows[1:]:
                    if len(row) < len(headers):
                        continue
                    row_dict = dict(zip(headers, row))
                    img_name = row_dict.get("Image Name", "").lower()
                    pid_str = row_dict.get("PID", "")
                    info = {
                        "running": True,
                        "pid": int(pid_str) if pid_str.isdigit() else pid_str,
                        "memory": row_dict.get("Mem Usage", ""),
                        "status": row_dict.get("Status", ""),
                        "cpu_time": row_dict.get("CPU Time", ""),
                        "window_title": row_dict.get("Window Title", "")
                    }
                    if "server" in img_name:
                        results["server"] = info
                    elif "animalcrossing" in img_name:
                        results["client"] = info
        return {
            "success": True,
            "processes": results
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to query process status: {e}"
        }

DEFAULT_EXCLUDE_DIRS = {
    ".git", "build32", "build", "bin", "__pycache__", ".vscode", ".idea", ".agent", ".agents"
}

DEFAULT_EXTENSIONS = {
    ".c", ".h", ".cpp", ".hpp", ".c_inc", ".inc", ".py", ".md", ".json", ".txt", ".cmake"
}

def tool_search_code(
    query,
    path=None,
    file_pattern=None,
    context_lines=0,
    case_sensitive=False,
    is_regex=False,
    max_matches=50
):
    """Searches the codebase for a text string or regex pattern."""
    if not query:
        return {"success": False, "error": "Query string cannot be empty."}

    # Determine base search path
    if path:
        target_path = os.path.abspath(os.path.join(PROJECT_ROOT, path.replace("\\", "/")))
    else:
        target_path = PROJECT_ROOT

    if not os.path.exists(target_path):
        return {"success": False, "error": f"Path does not exist: {target_path}"}

    try:
        if is_regex:
            flags = 0 if case_sensitive else re.IGNORECASE
            pattern = re.compile(query, flags)
            match_fn = lambda line: bool(pattern.search(line))
        else:
            if case_sensitive:
                match_fn = lambda line: query in line
            else:
                q_lower = query.lower()
                match_fn = lambda line: q_lower in line.lower()
    except Exception as e:
        return {"success": False, "error": f"Invalid regex pattern: {e}"}

    file_list = []
    if os.path.isfile(target_path):
        file_list.append(target_path)
    else:
        for root, dirs, files in os.walk(target_path):
            dirs[:] = [d for d in dirs if d not in DEFAULT_EXCLUDE_DIRS and not d.startswith(".")]
            for f in files:
                ext = os.path.splitext(f)[1].lower()
                if file_pattern:
                    if not fnmatch.fnmatch(f, file_pattern):
                        continue
                else:
                    if ext not in DEFAULT_EXTENSIONS and f != "CMakeLists.txt":
                        continue
                file_list.append(os.path.join(root, f))

    matches_by_file = {}
    total_matches = 0
    truncated = False

    for fpath in sorted(file_list):
        try:
            with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                lines = [l.rstrip("\r\n") for l in f]
        except Exception:
            continue

        file_match_indices = [i for i, line in enumerate(lines) if match_fn(line)]
        if not file_match_indices:
            continue

        rel_path = os.path.relpath(fpath, PROJECT_ROOT).replace("\\", "/")

        if context_lines > 0:
            ranges = []
            for idx in file_match_indices:
                start = max(0, idx - context_lines)
                end = min(len(lines), idx + context_lines + 1)
                if ranges and start <= ranges[-1][1]:
                    ranges[-1] = (ranges[-1][0], max(ranges[-1][1], end))
                else:
                    ranges.append((start, end))

            blocks = []
            for r_start, r_end in ranges:
                block_lines = []
                for idx in range(r_start, r_end):
                    line_text = lines[idx]
                    marker = ">" if match_fn(line_text) else " "
                    block_lines.append(f"{marker} {idx+1:5d}: {line_text}")
                blocks.append("\n".join(block_lines))
            matches_by_file[rel_path] = blocks
        else:
            block_lines = []
            for idx in file_match_indices:
                block_lines.append(f"{idx+1:5d}: {lines[idx]}")
            matches_by_file[rel_path] = block_lines

        total_matches += len(file_match_indices)
        if max_matches > 0 and total_matches >= max_matches:
            truncated = True
            break

    formatted_blocks = []
    for rel_path, items in matches_by_file.items():
        formatted_blocks.append(f"--- {rel_path} ---")
        if context_lines > 0:
            for block in items:
                formatted_blocks.append(block)
                formatted_blocks.append("")
        else:
            for line in items:
                formatted_blocks.append(f"  {line}")
            formatted_blocks.append("")

    return {
        "success": True,
        "query": query,
        "searched_path": os.path.relpath(target_path, PROJECT_ROOT).replace("\\", "/") if target_path != PROJECT_ROOT else ".",
        "files_with_matches": len(matches_by_file),
        "total_matches": total_matches,
        "truncated": truncated,
        "content": "\n".join(formatted_blocks).strip()
    }

def tool_find_files(
    pattern="*",
    path=None,
    recursive=True,
    max_results=200,
    include_dirs=False
):
    """Finds files and directories matching a pattern or glob across the repository."""
    if not pattern:
        pattern = "*"

    # Determine base search path
    if path:
        target_path = os.path.abspath(os.path.join(PROJECT_ROOT, path.replace("\\", "/")))
    else:
        target_path = PROJECT_ROOT

    if not os.path.exists(target_path):
        return {"success": False, "error": f"Path does not exist: {target_path}"}

    results = []
    truncated = False

    glob_pattern = pattern
    if not any(c in glob_pattern for c in "*?[]"):
        # Substring match if no wildcards provided
        glob_pattern = f"*{pattern}*"

    glob_pattern_lower = glob_pattern.lower()

    if os.path.isfile(target_path):
        fname = os.path.basename(target_path)
        if fnmatch.fnmatch(fname.lower(), glob_pattern_lower) or fnmatch.fnmatch(fname, glob_pattern):
            rel = os.path.relpath(target_path, PROJECT_ROOT).replace("\\", "/")
            return {"success": True, "count": 1, "total_returned": 1, "truncated": False, "files": [rel]}
        return {"success": True, "count": 0, "total_returned": 0, "truncated": False, "files": []}

    if recursive:
        for root, dirs, files in os.walk(target_path):
            dirs[:] = [d for d in dirs if d not in DEFAULT_EXCLUDE_DIRS and not d.startswith(".")]
            if include_dirs:
                for d in dirs:
                    if fnmatch.fnmatch(d.lower(), glob_pattern_lower) or fnmatch.fnmatch(d, glob_pattern):
                        full = os.path.join(root, d)
                        rel = os.path.relpath(full, PROJECT_ROOT).replace("\\", "/") + "/"
                        results.append(rel)
                        if len(results) >= max_results:
                            truncated = True
                            break
                if truncated:
                    break

            for f in files:
                if fnmatch.fnmatch(f.lower(), glob_pattern_lower) or fnmatch.fnmatch(f, glob_pattern):
                    full = os.path.join(root, f)
                    rel = os.path.relpath(full, PROJECT_ROOT).replace("\\", "/")
                    results.append(rel)
                    if len(results) >= max_results:
                        truncated = True
                        break
            if truncated:
                break
    else:
        try:
            entries = os.listdir(target_path)
        except Exception as e:
            return {"success": False, "error": f"Failed to list directory: {e}"}

        for entry in sorted(entries):
            if entry in DEFAULT_EXCLUDE_DIRS or entry.startswith("."):
                continue
            full = os.path.join(target_path, entry)
            is_dir = os.path.isdir(full)
            if is_dir and not include_dirs:
                continue
            if fnmatch.fnmatch(entry.lower(), glob_pattern_lower) or fnmatch.fnmatch(entry, glob_pattern):
                rel = os.path.relpath(full, PROJECT_ROOT).replace("\\", "/")
                if is_dir:
                    rel += "/"
                results.append(rel)
                if len(results) >= max_results:
                    truncated = True
                    break

    results.sort()
    return {
        "success": True,
        "pattern": pattern,
        "searched_path": os.path.relpath(target_path, PROJECT_ROOT).replace("\\", "/") if target_path != PROJECT_ROOT else ".",
        "count": len(results),
        "total_returned": len(results),
        "truncated": truncated,
        "files": results
    }

def tool_resolve_crash_address(addresses, target="server", binary_path=None):
    """Resolves crash memory addresses using addr2line inside the MSYS2 MinGW32 toolchain."""
    if isinstance(addresses, (int, str)):
        if isinstance(addresses, str):
            # Split whitespace or comma separated addresses or parse from text
            parsed = re.findall(r'(?:0x)?([0-9a-fA-F]{4,8})', addresses)
            if parsed:
                addrs = [f"0x{a}" if not a.startswith("0x") else a for a in parsed]
            else:
                addrs = [addresses.strip()]
        else:
            addrs = [hex(addresses)]
    elif isinstance(addresses, (list, tuple)):
        addrs = []
        for a in addresses:
            if isinstance(a, int):
                addrs.append(hex(a))
            elif isinstance(a, str):
                p = re.findall(r'(?:0x)?([0-9a-fA-F]{4,8})', a)
                if p:
                    addrs.extend(f"0x{x}" for x in p)
                else:
                    addrs.append(a.strip())
    else:
        return {"success": False, "error": f"Invalid addresses format: {type(addresses)}"}

    if not addrs:
        return {"success": False, "error": "No addresses provided to resolve."}

    if not binary_path:
        t = str(target).lower().strip()
        if t in ("server", "dedicated_server", "ac_server"):
            exe_file = SERVER_EXE
            rel_exe = "./AnimalCrossingServer.exe"
        else:
            exe_file = CLIENT_EXE
            rel_exe = "./AnimalCrossing.exe"
    else:
        exe_file = os.path.abspath(binary_path)
        rel_exe = os.path.basename(exe_file)

    if not os.path.isfile(exe_file):
        return {"success": False, "error": f"Binary not found: {exe_file}"}

    addrs_str = " ".join(addrs)
    cmd = f"addr2line -e {rel_exe} -f -C -p {addrs_str}"
    code, out = run_msys2(cmd, timeout=30, cwd=BIN_DIR)

    resolved = []
    lines = [l.strip() for l in out.strip().split("\n") if l.strip()]
    for idx, line in enumerate(lines):
        addr_label = addrs[idx] if idx < len(addrs) else f"addr_{idx}"
        resolved.append({
            "address": addr_label,
            "symbol_and_location": line
        })

    return {
        "success": (code == 0),
        "target": target,
        "binary": os.path.basename(exe_file),
        "raw_output": out.strip(),
        "resolved": resolved
    }

PINNED_LOG_FILES = {
    "client": "game.log",
    "game": "game.log",
    "server": "server.log",
    "player": "server_player.log",
    "server_player": "server_player.log",
    "zone": "server_player.log",
    "zones": "server_player.log",
}

def find_log_file(target="client", custom_path=None):
    """Finds the most appropriate log file path for pinned logs or custom path."""
    if custom_path:
        return custom_path

    target_key = str(target).lower().strip()
    filename = PINNED_LOG_FILES.get(target_key, f"{target_key}.log" if not target_key.endswith(".log") else target_key)
    candidates = [
        os.path.join(BUILD_DIR, "bin", filename),
        os.path.join(PROJECT_ROOT, filename),
        os.path.join(BUILD_DIR, filename),
        os.path.join(PROJECT_ROOT, "pc", filename),
    ]
    existing = [p for p in candidates if os.path.isfile(p)]
    if not existing:
        return candidates[0]
    # Pick the most recently modified log file
    existing.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return existing[0]

def tool_read_game_logs(
    target="client",
    max_lines=100,
    filter_pattern=None,
    search_query=None,
    context_lines=0,
    show_line_numbers=None,
    case_sensitive=False,
    is_regex=False,
    max_matches=50,
    clear=False,
    custom_path=None
):
    """Reads, searches, and filters client, server, or player diagnostic logs."""
    log_path = find_log_file(target=target, custom_path=custom_path)

    if clear:
        if os.path.isfile(log_path):
            try:
                with open(log_path, "w", encoding="utf-8") as f:
                    pass
                return {
                    "success": True,
                    "message": f"Cleared log file at {log_path}",
                    "log_path": log_path,
                    "lines": []
                }
            except Exception as e:
                return {
                    "success": False,
                    "error": f"Failed to clear log file {log_path}: {e}"
                }
        else:
            return {
                "success": True,
                "message": f"Log file does not exist yet at {log_path}. Nothing to clear.",
                "log_path": log_path
            }

    if not os.path.isfile(log_path):
        return {
            "success": False,
            "error": f"No log file found for target '{target}'. Checked location: {log_path}",
            "target": target,
            "log_path": log_path,
            "lines": []
        }

    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            all_lines = [line.rstrip("\r\n") for line in f]

        total_lines = len(all_lines)
        query = search_query if search_query is not None else filter_pattern

        def attach_crash_resolution(r):
            c = r.get("content", "")
            if "[FATAL CRASH]" in c or "Fault Address:" in c or "ExceptionCode" in c:
                addrs = []
                # Match [0x12345678] or Address=12345678 or Fault Address: 12345678
                for m in re.finditer(r'(?:\[0x([0-9a-fA-F]+)\]|Address=([0-9a-fA-F]+)|Fault Address:\s*([0-9a-fA-F]+))', c):
                    addr = m.group(1) or m.group(2) or m.group(3)
                    if addr and addr not in addrs:
                        addrs.append(addr)
                if addrs:
                    t = "server" if ("server" in str(target).lower() or "server" in log_path.lower()) else "client"
                    r["resolved_crash_stack"] = tool_resolve_crash_address(addrs, target=t)
            return r

        if query:
            if show_line_numbers is None:
                show_line_numbers = True

            if is_regex:
                flags = 0 if case_sensitive else re.IGNORECASE
                pattern = re.compile(query, flags)
                match_fn = lambda line: bool(pattern.search(line))
            else:
                if case_sensitive:
                    match_fn = lambda line: query in line
                else:
                    q_lower = query.lower()
                    match_fn = lambda line: q_lower in line.lower()

            match_indices = [i for i, line in enumerate(all_lines) if match_fn(line)]

            if not match_indices:
                return {
                    "success": True,
                    "log_path": log_path,
                    "total_lines": total_lines,
                    "search_query": query,
                    "matches_found": 0,
                    "returned_lines": 0,
                    "content": f"No occurrences found for '{query}' in {log_path} ({total_lines} total lines)."
                }

            if context_lines > 0:
                # Group matches with surrounding context lines
                ranges = []
                for idx in match_indices:
                    start = max(0, idx - context_lines)
                    end = min(total_lines, idx + context_lines + 1)
                    if ranges and start <= ranges[-1][1]:
                        ranges[-1] = (ranges[-1][0], max(ranges[-1][1], end))
                    else:
                        ranges.append((start, end))

                if max_matches > 0 and len(ranges) > max_matches:
                    ranges = ranges[-max_matches:]

                blocks = []
                total_returned = 0
                for r_start, r_end in ranges:
                    block_lines = []
                    for idx in range(r_start, r_end):
                        line_text = all_lines[idx]
                        is_matched_line = match_fn(line_text)
                        marker = ">" if is_matched_line else " "
                        if show_line_numbers:
                            block_lines.append(f"{marker} {idx+1:6d}: {line_text}")
                        else:
                            block_lines.append(f"{marker} {line_text}")
                    blocks.append("\n".join(block_lines))
                    total_returned += (r_end - r_start)

                return attach_crash_resolution({
                    "success": True,
                    "log_path": log_path,
                    "total_lines": total_lines,
                    "search_query": query,
                    "matches_found": len(match_indices),
                    "context_lines": context_lines,
                    "returned_groups": len(ranges),
                    "returned_lines": total_returned,
                    "content": "\n---\n".join(blocks)
                })
            else:
                # Flat list of matched lines
                if max_lines > 0 and len(match_indices) > max_lines:
                    selected_indices = match_indices[-max_lines:]
                else:
                    selected_indices = match_indices

                output_lines = []
                for idx in selected_indices:
                    line_text = all_lines[idx]
                    if show_line_numbers:
                        output_lines.append(f"{idx+1:6d}: {line_text}")
                    else:
                        output_lines.append(line_text)

                return attach_crash_resolution({
                    "success": True,
                    "log_path": log_path,
                    "total_lines": total_lines,
                    "search_query": query,
                    "matches_found": len(match_indices),
                    "returned_lines": len(output_lines),
                    "content": "\n".join(output_lines)
                })

        else:
            # Plain tail (no search query)
            if show_line_numbers is None:
                show_line_numbers = False

            if max_lines > 0 and total_lines > max_lines:
                start_idx = total_lines - max_lines
                selected_lines = all_lines[start_idx:]
            else:
                start_idx = 0
                selected_lines = all_lines

            if show_line_numbers:
                output_lines = [f"{start_idx + i + 1:6d}: {line}" for i, line in enumerate(selected_lines)]
            else:
                output_lines = selected_lines

            return attach_crash_resolution({
                "success": True,
                "log_path": log_path,
                "total_lines": total_lines,
                "returned_lines": len(output_lines),
                "content": "\n".join(output_lines)
            })
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to read log file {log_path}: {e}"
        }

# --- Tool Registry ---

TOOLS = [
    {
        "name": "build_game",
        "description": "Compiles the Animal Crossing PC client (AnimalCrossing.exe), dedicated server (AnimalCrossingServer.exe), or both using MSYS2 Ninja.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {
                    "type": "string",
                    "enum": ["all", "client", "server"],
                    "description": "Which component to build: 'all', 'client', or 'server'. Defaults to 'all'."
                },
                "clean": {
                    "type": "boolean",
                    "description": "Whether to clean build artifacts before building."
                }
            }
        }
    },
    {
        "name": "compile_check",
        "description": "Quickly compiles specific C/C++ files (or currently dirty files) to check for syntax, type, or linker errors without running the game.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "files": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional list of file paths (e.g. ['src/game/m_play.c', 'pc/src/server/server_zone.c']) to check."
                }
            }
        }
    },
    {
        "name": "run_server_test",
        "description": "Starts the headless dedicated server (AnimalCrossingServer.exe) for a few seconds to verify world loading, zone initialization, and memory allocation without crashing.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "timeout_seconds": {
                    "type": "integer",
                    "description": "How many seconds to run the server before terminating (default: 5)."
                },
                "extra_args": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional command line arguments to pass to the server."
                },
                "debug": {
                    "type": "boolean",
                    "description": "Run the server under GDB in the MSYS2 environment to produce a stack trace upon crash."
                }
            }
        }
    },
    {
        "name": "git_status",
        "description": "Queries git status or diff to review changes cleanly without terminal prompts.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "diff": {
                    "type": "boolean",
                    "description": "If true, returns git diff instead of status."
                },
                "files": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional specific file paths for git diff."
                }
            }
        }
    },
    {
        "name": "read_game_logs",
        "description": "Reads, searches, and inspects pinned runtime diagnostic logs for the Animal Crossing PC client (game.log), dedicated server (server.log), or multiplayer player/zone diagnostics (server_player.log). Supports tailing lines, substring/regex searching with context lines, line numbering, or clearing logs.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {
                    "type": "string",
                    "enum": ["client", "server", "player", "server_player"],
                    "description": "Which pinned log to read: 'client' (game.log), 'server' (server.log), or 'player'/'server_player' (server_player.log). Defaults to 'client'."
                },
                "search_query": {
                    "type": "string",
                    "description": "String or regex pattern to search for in the log."
                },
                "context_lines": {
                    "type": "integer",
                    "description": "Number of context lines before and after each match (like grep -C). Defaults to 0 (or set e.g. 3 or 5 for rich context)."
                },
                "filter_pattern": {
                    "type": "string",
                    "description": "Alias for search_query (case-insensitive substring filter)."
                },
                "show_line_numbers": {
                    "type": "boolean",
                    "description": "Prefix lines with 1-indexed line numbers. Defaults to true when searching."
                },
                "case_sensitive": {
                    "type": "boolean",
                    "description": "Whether string matching should be case-sensitive. Defaults to false."
                },
                "is_regex": {
                    "type": "boolean",
                    "description": "Whether to treat search_query as a regular expression. Defaults to false."
                },
                "max_matches": {
                    "type": "integer",
                    "description": "Maximum number of match groups to return when using context_lines. Defaults to 50."
                },
                "max_lines": {
                    "type": "integer",
                    "description": "Maximum number of most recent lines to return. Defaults to 100. Set to 0 to return all lines."
                },
                "clear": {
                    "type": "boolean",
                    "description": "If true, clears (truncates) the specified log file."
                },
                "custom_path": {
                    "type": "string",
                    "description": "Optional explicit file path to a log file."
                }
            }
        }
    },
    {
        "name": "search_code",
        "description": "Searches repository source files (C, C++, headers, includes, etc.) for literal strings or regex patterns, displaying matched files, line numbers, and optional surrounding context.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "String or regex pattern to search for in source code."
                },
                "path": {
                    "type": "string",
                    "description": "Optional subdirectory or file path to search within (e.g. 'src/game', 'include', or 'src/game/m_field_info.c'). Defaults to repository root."
                },
                "file_pattern": {
                    "type": "string",
                    "description": "Optional glob pattern for file matching (e.g. '*.c', '*.h', '*.c_inc')."
                },
                "context_lines": {
                    "type": "integer",
                    "description": "Number of context lines before and after each match (like grep -C). Defaults to 0."
                },
                "case_sensitive": {
                    "type": "boolean",
                    "description": "Whether search is case-sensitive. Defaults to false."
                },
                "is_regex": {
                    "type": "boolean",
                    "description": "Whether to treat query as a regular expression. Defaults to false."
                },
                "max_matches": {
                    "type": "integer",
                    "description": "Maximum number of total matching lines to return. Defaults to 50."
                }
            },
            "required": ["query"]
        }
    },
    {
        "name": "get_game_status",
        "description": "Queries Windows running processes to check if AnimalCrossing.exe or AnimalCrossingServer.exe are active, returning PID, memory usage, status, CPU time, and window title.",
        "inputSchema": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "find_files",
        "description": "Finds files and directories matching a glob pattern (e.g. '*.md', '*phase*', 'm_*.c') within the repository without running shell commands.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string",
                    "description": "Glob pattern or substring to match file or directory names (e.g. '*.md', '*phase*', '*.h', 'settings.*'). Defaults to '*'."
                },
                "path": {
                    "type": "string",
                    "description": "Optional subdirectory or path to search within (e.g. '.', 'src', 'pc', 'include'). Defaults to repository root."
                },
                "recursive": {
                    "type": "boolean",
                    "description": "Whether to recursively search subdirectories. Defaults to true."
                },
                "include_dirs": {
                    "type": "boolean",
                    "description": "Whether to include matching directory paths in the results. Defaults to false."
                },
                "max_results": {
                    "type": "integer",
                    "description": "Maximum number of matching file paths to return. Defaults to 200."
                }
            }
        }
    },
    {
        "name": "resolve_crash_address",
        "description": "Resolves memory addresses / fault addresses from crash callstacks to source file lines and function names using addr2line.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "addresses": {
                    "description": "Address or list of addresses/callstack hex strings (e.g. ['0x00496005', '0x0060739A'] or '0x00496005 0x0060739A').",
                    "type": ["string", "array"]
                },
                "target": {
                    "type": "string",
                    "enum": ["server", "client"],
                    "description": "Target executable: 'server' (AnimalCrossingServer.exe) or 'client' (AnimalCrossing.exe). Defaults to 'server'."
                },
                "binary_path": {
                    "type": "string",
                    "description": "Optional explicit path to the .exe binary."
                }
            },
            "required": ["addresses"]
        }
    }
]

def handle_tool_call(name, arguments):
    if name == "resolve_crash_address":
        return tool_resolve_crash_address(
            addresses=arguments.get("addresses"),
            target=arguments.get("target", "server"),
            binary_path=arguments.get("binary_path")
        )
    elif name == "build_game":
        return tool_build_game(
            target=arguments.get("target", "all"),
            clean=arguments.get("clean", False)
        )
    elif name == "compile_check":
        return tool_compile_check(files=arguments.get("files"))
    elif name == "run_server_test":
        return tool_run_server_test(
            timeout_seconds=arguments.get("timeout_seconds", 5),
            extra_args=arguments.get("extra_args"),
            debug=arguments.get("debug", False)
        )
    elif name == "git_status":
        return tool_git_status(
            diff=arguments.get("diff", False),
            files=arguments.get("files")
        )
    elif name == "read_game_logs":
        return tool_read_game_logs(
            target=arguments.get("target", "client"),
            max_lines=arguments.get("max_lines", 100),
            filter_pattern=arguments.get("filter_pattern"),
            search_query=arguments.get("search_query") or arguments.get("query"),
            context_lines=arguments.get("context_lines", 0),
            show_line_numbers=arguments.get("show_line_numbers"),
            case_sensitive=arguments.get("case_sensitive", False),
            is_regex=arguments.get("is_regex", False),
            max_matches=arguments.get("max_matches", 50),
            clear=arguments.get("clear", False),
            custom_path=arguments.get("custom_path")
        )
    elif name == "search_code":
        return tool_search_code(
            query=arguments.get("query") or arguments.get("pattern") or arguments.get("search_query"),
            path=arguments.get("path") or arguments.get("directory") or arguments.get("file"),
            file_pattern=arguments.get("file_pattern") or arguments.get("pattern_glob"),
            context_lines=arguments.get("context_lines", 0),
            case_sensitive=arguments.get("case_sensitive", False),
            is_regex=arguments.get("is_regex", False),
            max_matches=arguments.get("max_matches", 50)
        )
    elif name == "get_game_status":
        return tool_get_game_status()
    elif name == "find_files":
        return tool_find_files(
            pattern=arguments.get("pattern") or arguments.get("filter") or arguments.get("file_pattern") or "*",
            path=arguments.get("path") or arguments.get("directory"),
            recursive=arguments.get("recursive", True),
            include_dirs=arguments.get("include_dirs", False),
            max_results=arguments.get("max_results", 200)
        )
    else:
        raise ValueError(f"Unknown tool: {name}")

# --- MCP Stdio Protocol Server ---

def send_message(msg):
    data = json.dumps(msg, ensure_ascii=False)
    # Write as newline-delimited JSON
    sys.stdout.write(data + "\n")
    sys.stdout.flush()

def read_message():
    line = sys.stdin.readline()
    if not line:
        return None
    line = line.strip()
    if not line:
        return read_message()

    # Handle Content-Length framing if present
    if line.lower().startswith("content-length:"):
        length = int(line.split(":", 1)[1].strip())
        # Skip empty lines until body
        while True:
            header_line = sys.stdin.readline().strip()
            if not header_line:
                break
        content = sys.stdin.read(length)
        return json.loads(content)

    return json.loads(line)

def main():
    # Ensure binary or clean UTF-8 stdio
    if sys.platform == "win32":
        import msvcrt
        try:
            sys.stdin.reconfigure(encoding="utf-8")
            sys.stdout.reconfigure(encoding="utf-8")
        except Exception:
            pass

    while True:
        try:
            req = read_message()
            if req is None:
                break

            msg_id = req.get("id")
            method = req.get("method")
            params = req.get("params", {})

            # Notification (no id)
            if msg_id is None:
                continue

            if method == "initialize":
                protocol_ver = params.get("protocolVersion", "2024-11-05")
                send_message({
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {
                        "protocolVersion": protocol_ver,
                        "capabilities": {
                            "tools": {}
                        },
                        "serverInfo": {
                            "name": "acgc-tools",
                            "version": "1.0.0"
                        }
                    }
                })

            elif method == "tools/list":
                send_message({
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {
                        "tools": TOOLS
                    }
                })

            elif method == "tools/call":
                tool_name = params.get("name")
                tool_args = params.get("arguments", {})
                try:
                    result = handle_tool_call(tool_name, tool_args)
                    formatted_text = json.dumps(result, indent=2)
                    send_message({
                        "jsonrpc": "2.0",
                        "id": msg_id,
                        "result": {
                            "content": [
                                {
                                    "type": "text",
                                    "text": formatted_text
                                }
                            ],
                            "isError": not result.get("success", True)
                        }
                    })
                except Exception as e:
                    send_message({
                        "jsonrpc": "2.0",
                        "id": msg_id,
                        "result": {
                            "content": [
                                {
                                    "type": "text",
                                    "text": f"Error executing tool {tool_name}: {e}"
                                }
                            ],
                            "isError": True
                        }
                    })

            elif method == "ping":
                send_message({
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {}
                })

            else:
                send_message({
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "error": {
                        "code": -32601,
                        "message": f"Method not found: {method}"
                    }
                })

        except Exception as e:
            # Prevent server from crashing on malformed input
            sys.stderr.write(f"[mcp_server] Error: {e}\n")
            sys.stderr.flush()

def run_cli():
    import argparse
    parser = argparse.ArgumentParser(description="ACGC-PC-Port Dev & MCP Tool")
    subparsers = parser.add_subparsers(dest="command")

    build_p = subparsers.add_parser("build", help="Build client, server, or all")
    build_p.add_argument("target", nargs="?", default="all", choices=["all", "client", "server"])
    build_p.add_argument("--clean", action="store_true", help="Clean build first")

    check_p = subparsers.add_parser("check", help="Check compilation of modified/specified files")
    check_p.add_argument("files", nargs="*", help="Optional file paths to check")

    test_p = subparsers.add_parser("test", help="Test run dedicated server")
    test_p.add_argument("--timeout", type=int, default=5, help="Seconds to run server")

    subparsers.add_parser("status", help="Git status")
    subparsers.add_parser("diff", help="Git diff")

    logs_p = subparsers.add_parser("logs", help="Read game or server logs")
    logs_p.add_argument("target", nargs="?", default="client", choices=["client", "server"], help="Target log ('client' or 'server')")
    logs_p.add_argument("-n", "--lines", type=int, default=100, help="Number of lines to tail (default 100)")
    logs_p.add_argument("-f", "--filter", type=str, default=None, help="Filter lines by substring")
    logs_p.add_argument("--clear", action="store_true", help="Clear/truncate the log file")
    logs_p.add_argument("--path", type=str, default=None, help="Custom log file path")

    search_p = subparsers.add_parser("search", help="Search code files for pattern")
    search_p.add_argument("query", help="Query string or regex")
    search_p.add_argument("-p", "--path", default=None, help="Path or directory to search")
    search_p.add_argument("-f", "--file-pattern", default=None, help="Glob pattern for files")
    search_p.add_argument("-C", "--context", type=int, default=0, help="Context lines")
    search_p.add_argument("-i", "--ignore-case", action="store_true", help="Case insensitive")
    search_p.add_argument("-r", "--regex", action="store_true", help="Treat query as regex")
    search_p.add_argument("-m", "--max-matches", type=int, default=50, help="Max matches")

    find_p = subparsers.add_parser("find", help="Find files matching pattern or glob")
    find_p.add_argument("pattern", nargs="?", default="*", help="File pattern or glob (e.g. *.md, *phase*)")
    find_p.add_argument("-p", "--path", default=None, help="Directory to search within")
    find_p.add_argument("-nr", "--no-recursive", action="store_true", help="Do not search recursively")
    find_p.add_argument("-d", "--include-dirs", action="store_true", help="Include directories in results")
    find_p.add_argument("-m", "--max-results", type=int, default=200, help="Max results")

    args = parser.parse_args()
    if args.command == "build":
        res = tool_build_game(target=args.target, clean=args.clean)
        print(res.get("output", ""))
        sys.exit(0 if res.get("success") else 1)
    elif args.command == "check":
        res = tool_compile_check(files=args.files if args.files else None)
        print(res.get("output", ""))
        sys.exit(0 if res.get("success") else 1)
    elif args.command == "test":
        res = tool_run_server_test(timeout_seconds=args.timeout)
        print(res.get("output", ""))
        if "note" in res:
            print(f"[OK] {res['note']}")
        sys.exit(0 if res.get("success") else 1)
    elif args.command == "status":
        res = tool_git_status(diff=False)
        print(res.get("output", ""))
    elif args.command == "diff":
        res = tool_git_status(diff=True)
        print(res.get("output", ""))
    elif args.command == "logs":
        res = tool_read_game_logs(
            target=args.target,
            max_lines=args.lines,
            filter_pattern=args.filter,
            clear=args.clear,
            custom_path=args.path
        )
        if not res.get("success"):
            print(f"Error: {res.get('error', 'Unknown error')}")
            sys.exit(1)
        if args.clear:
            print(res.get("message"))
        else:
            print(f"=== {res.get('log_path')} ({res.get('returned_lines')}/{res.get('total_lines')} lines) ===")
            print(res.get("content", ""))
    elif args.command == "search":
        res = tool_search_code(
            query=args.query,
            path=args.path,
            file_pattern=args.file_pattern,
            context_lines=args.context,
            case_sensitive=not args.ignore_case,
            is_regex=args.regex,
            max_matches=args.max_matches
        )
        if not res.get("success"):
            print(f"Error: {res.get('error', 'Unknown error')}")
            sys.exit(1)
        print(res.get("content", ""))
    elif args.command == "find":
        res = tool_find_files(
            pattern=args.pattern,
            path=args.path,
            recursive=not args.no_recursive,
            include_dirs=args.include_dirs,
            max_results=args.max_results
        )
        if not res.get("success"):
            print(f"Error: {res.get('error', 'Unknown error')}")
            sys.exit(1)
        for f in res.get("files", []):
            print(f)
        if res.get("truncated"):
            print(f"... (truncated at {res.get('count')} results)")
    else:
        parser.print_help()

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] not in ("--stdio", "-s"):
        run_cli()
    else:
        main()
