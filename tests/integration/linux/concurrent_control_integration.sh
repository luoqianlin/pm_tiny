#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_concurrent.XXXXXX)"
DAEMON_PID=0

cleanup() {
    status=$?
    trap - EXIT
    if [[ "$DAEMON_PID" -gt 0 ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
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
    for _ in $(seq 1 150); do
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

assert_daemon_healthy() {
    "$BIN/pm" list --json | python3 -c '
import json, sys
data = json.load(sys.stdin)
assert data["schema_version"] == 5
assert isinstance(data["processes"], list)
for item in data["processes"]:
    assert item["state"] in {"online", "starting", "stopped", "failed", "waiting", "blocked"}
'
    "$BIN/pm" version >/dev/null
}

start_async() {
    local label=$1
    shift
    (
        set +e
        "$BIN/pm" "$@" >"$TMP/$label.stdout" 2>"$TMP/$label.stderr"
        printf '%s\n' "$?" >"$TMP/$label.status"
    ) &
    ASYNC_PIDS+=("$!")
}

wait_async() {
    local pid status_file status
    for pid in "${ASYNC_PIDS[@]}"; do
        wait "$pid" || true
    done
    for status_file in "$TMP"/*.status; do
        status=$(<"$status_file")
        if [[ "$status" -gt 1 ]]; then
            echo "concurrent command failed with unexpected exit code $status: $status_file" >&2
            [[ ! -s "${status_file%.status}.stderr" ]] || cat "${status_file%.status}.stderr" >&2
            return 1
        fi
    done
    ASYNC_PIDS=()
}

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
printf '%s\n' '[]' >"$TMP/prog.yaml"
cat >"$TMP/pm_tiny.yaml" <<EOF
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
ASYNC_PIDS=()

"$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" >"$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 150); do [[ -S "$TMP/pm.sock" ]] && break; sleep .05; done
[[ -S "$TMP/pm.sock" ]]

"$BIN/pm" start concurrent_worker --no-daemon --no-pty -- /usr/bin/sleep 60 >/dev/null
"$BIN/pm" start concurrent_delete_target --no-daemon --no-pty -- /usr/bin/sleep 60 >/dev/null
wait_for_state concurrent_worker online
wait_for_state concurrent_delete_target online

for index in $(seq 1 8); do
    start_async "wave1-list-$index" list --json
done
for index in $(seq 1 4); do
    start_async "wave1-inspect-$index" inspect concurrent_worker
    start_async "wave1-save-$index" save
    start_async "wave1-reload-$index" reload --no-list
done
wait_async
assert_daemon_healthy

for index in $(seq 1 4); do
    start_async "wave2-stop-$index" stop concurrent_worker --no-list
    start_async "wave2-start-$index" start concurrent_worker
    start_async "wave2-restart-$index" restart concurrent_worker --no-list
    start_async "wave2-delete-$index" delete concurrent_delete_target --no-list
    start_async "wave2-save-$index" save
    start_async "wave2-reload-$index" reload --no-list
done
wait_async
assert_daemon_healthy

for name in concurrent_worker concurrent_delete_target; do
    "$BIN/pm" stop "$name" --no-list >/dev/null 2>&1 || true
    "$BIN/pm" delete "$name" --no-list >/dev/null 2>&1 || true
done
assert_daemon_healthy
"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
