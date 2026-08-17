#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_lifecycle.XXXXXX)"
DAEMON_PID=0

cleanup() {
    status=$?
    trap - EXIT
    if [[ "$DAEMON_PID" -gt 0 ]]; then kill "$DAEMON_PID" 2>/dev/null || true; fi
    if [[ $status -ne 0 ]]; then
        [[ ! -f "$TMP/daemon.out" ]] || tail -200 "$TMP/daemon.out" >&2
        [[ ! -f "$TMP/pm.log" ]] || tail -200 "$TMP/pm.log" >&2
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

wait_for_state() {
    local name=$1
    local expected=$2
    local state=
    for _ in $(seq 1 100); do
        state=$("$BIN/pm" list --json | python3 -c '
import json, sys
name = sys.argv[1]
items = [item for item in json.load(sys.stdin)["processes"] if item["name"] == name]
print(items[0]["state"] if items else "missing")
' "$name")
        [[ "$state" == "$expected" ]] && return 0
        sleep .05
    done
    echo "timed out waiting for $name state $expected; last state: $state" >&2
    return 1
}

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
printf '%s\n' '[]' > "$TMP/prog.yaml"
cat > "$TMP/pm_tiny.yaml" <<EOF
pm_tiny_home_dir: $TMP/home
pm_tiny_sock_file: $TMP/pm.sock
pm_tiny_log_file: $TMP/pm.log
pm_tiny_prog_cfg_file: $TMP/prog.yaml
pm_tiny_app_log_dir: $TMP/logs
pm_tiny_app_environ_dir: $TMP/environ
pm_tiny_uds_abstract_namespace: false
EOF

export PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock"
export PM_TINY_PROG_CFG_FILE="$TMP/prog.yaml" PM_TINY_LOG_FILE="$TMP/pm.log"
export PM_TINY_APP_LOG_DIR="$TMP/logs" PM_TINY_APP_ENVIRON_DIR="$TMP/environ"

"$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" > "$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 100); do [[ -S "$TMP/pm.sock" ]] && break; sleep .05; done
[[ -S "$TMP/pm.sock" ]]

"$BIN/pm" start lifecycle_peer --no-daemon --no-pty -- /usr/bin/sleep 30 | grep -q started
"$BIN/pm" start lifecycle_target --no-daemon --no-pty -- /usr/bin/sleep 30 | grep -q started
wait_for_state lifecycle_peer online
wait_for_state lifecycle_target online

read -r peer_pid peer_generation target_generation < <("$BIN/pm" list --json | python3 -c '
import json, sys
items = {item["name"]: item for item in json.load(sys.stdin)["processes"]}
print(items["lifecycle_peer"]["pid"], items["lifecycle_peer"]["generation"],
      items["lifecycle_target"]["generation"])
')
"$BIN/pm" stop lifecycle_target --no-list >/dev/null
wait_for_state lifecycle_target stopped
"$BIN/pm" start lifecycle_target | grep -q started
wait_for_state lifecycle_target online
"$BIN/pm" list --json | python3 -c '
import json, sys
items = {item["name"]: item for item in json.load(sys.stdin)["processes"]}
peer = items["lifecycle_peer"]
target = items["lifecycle_target"]
assert peer["state"] == "online"
assert peer["pid"] == int(sys.argv[1]) and peer["generation"] == int(sys.argv[2])
assert target["state"] == "online" and target["generation"] > int(sys.argv[3])
' "$peer_pid" "$peer_generation" "$target_generation"

"$BIN/pm" start startup_probe --no-daemon --no-pty --start-timeout -1 -- /usr/bin/sleep 30 | grep -q started
wait_for_state startup_probe starting
startup_generation=$("$BIN/pm" list --json | python3 -c '
import json, sys
print(next(item["generation"] for item in json.load(sys.stdin)["processes"]
           if item["name"] == "startup_probe"))
')
"$BIN/pm" stop startup_probe --no-list >/dev/null
wait_for_state startup_probe stopped
for _ in $(seq 1 10); do
    "$BIN/pm" list --json | python3 -c '
import json, sys
item = next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "startup_probe")
assert item["state"] == "stopped" and item["pid"] is None
assert item["generation"] == int(sys.argv[1])
' "$startup_generation"
    sleep .05
done

"$BIN/pm" start startup_probe | grep -q started
wait_for_state startup_probe starting
restarted_generation=$("$BIN/pm" list --json | python3 -c '
import json, sys
item = next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "startup_probe")
assert item["generation"] > int(sys.argv[1])
print(item["generation"])
' "$startup_generation")
"$BIN/pm" restart startup_probe --no-list >/dev/null
for _ in $(seq 1 100); do
    status=$("$BIN/pm" list --json)
    if python3 -c '
import json, sys
item = next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "startup_probe")
assert item["state"] == "starting" and item["generation"] > int(sys.argv[1])
' "$restarted_generation" <<< "$status"; then
        break
    fi
    sleep .05
done
python3 -c '
import json, sys
item = next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "startup_probe")
assert item["state"] == "starting" and item["generation"] > int(sys.argv[1])
' "$restarted_generation" <<< "$status"

"$BIN/pm" delete startup_probe --no-list >/dev/null
"$BIN/pm" list --json | python3 -c '
import json, sys
assert all(item["name"] != "startup_probe" for item in json.load(sys.stdin)["processes"])
'
"$BIN/pm" delete lifecycle_target --no-list >/dev/null
"$BIN/pm" delete lifecycle_peer --no-list >/dev/null
"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
