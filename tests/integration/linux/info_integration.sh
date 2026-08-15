#!/usr/bin/env bash
set -euo pipefail

BIN=${PM_TINY_TEST_BIN:?PM_TINY_TEST_BIN is required}
TEST_ROOT=$(mktemp -d /tmp/pm_tiny_info_integration.XXXXXX)
FOREGROUND_PID=

cleanup() {
    PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" quit >/dev/null 2>&1 || true
    if [ -n "$FOREGROUND_PID" ]; then
        wait "$FOREGROUND_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

printf '[]\n' > "$TEST_ROOT/prog.yaml"
printf '%s\n' \
    'pm_tiny_prog_cfg_file: prog.yaml' \
    'pm_tiny_log_file: daemon.log' \
    'pm_tiny_app_log_dir: logs' \
    'pm_tiny_app_environ_dir: environ' > "$TEST_ROOT/pm_tiny.yaml"

wait_for_socket() {
    for _ in $(seq 1 100); do
        if [ -S "$TEST_ROOT/pm_tinyd.sock" ]; then return 0; fi
        sleep 0.05
    done
    return 1
}

assert_info() {
    local expected_mode=$1
    PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" info --json > "$TEST_ROOT/info.json"
    python3 - "$TEST_ROOT/info.json" "$TEST_ROOT" "$expected_mode" <<'PY'
import json, os, sys
path, root, mode = sys.argv[1:]
with open(path, encoding="utf-8") as stream:
    data = json.load(stream)
assert data["schema_version"] == 1
assert data["identity"]["version"] == "4.0.0"
assert data["identity"]["protocol_version"] == 3
assert data["identity"]["pid"] > 0
assert data["identity"]["uptime_ms"] >= 0
assert data["runtime"]["mode"] == mode
assert data["runtime"]["state"] == "running"
assert data["runtime"]["file_config_count"] == 0
assert data["runtime"]["runtime_definition_count"] == 0
assert data["config"]["home_dir"] == {"value": root, "source": "command_line"}
assert data["config"]["program_config_file"]["value"] == os.path.join(root, "prog.yaml")
assert data["config"]["program_config_file"]["source"] == "config_file"
assert data["ipc"]["uds_address"]["value"] == os.path.join(root, "pm_tinyd.sock")
assert data["process_tree"]["requested_mode"]["value"] == "process_group"
assert data["process_tree"]["requested_mode"]["source"] == "environment"
assert data["process_tree"]["effective_mode"] == "process_group"
assert isinstance(data["ipc"]["allowed_uids"]["value"], list)
PY
    PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" info | grep -q '^Configuration$'
}

PM_TINY_HOME="$TEST_ROOT" PM_TINY_PROCESS_TREE_MODE=process_group \
    "$BIN/pm_tiny" -c "$TEST_ROOT/pm_tiny.yaml" --home "$TEST_ROOT" &
FOREGROUND_PID=$!
wait_for_socket
assert_info foreground
PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" quit >/dev/null
wait "$FOREGROUND_PID"
FOREGROUND_PID=

PM_TINY_HOME="$TEST_ROOT" PM_TINY_PROCESS_TREE_MODE=process_group \
    "$BIN/pm_tiny" -d -c "$TEST_ROOT/pm_tiny.yaml" --home "$TEST_ROOT"
wait_for_socket
assert_info daemon
PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" quit >/dev/null
