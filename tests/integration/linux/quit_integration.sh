#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_quit.XXXXXX)"
DAEMON_PID=
WRAPPER_PID=
FAKE_PID=

cleanup() {
    status=$?
    trap - EXIT
    for pid in "$DAEMON_PID" "$WRAPPER_PID" "$FAKE_PID"; do
        [[ -n "$pid" ]] || continue
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if [[ $status -ne 0 ]]; then
        for log_file in "$TMP"/*.out "$TMP"/*.err "$TMP"/*.log; do
            [[ -f "$log_file" ]] || continue
            echo "--- $log_file ---" >&2
            tail -100 "$log_file" >&2
        done
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

configure() {
    name=$1
    mkdir -p "$TMP/$name/home" "$TMP/$name/logs" "$TMP/$name/environ"
    printf '%s\n' '[]' > "$TMP/$name/prog.yaml"
    cat > "$TMP/$name/pm_tiny.yaml" <<EOF
pm_tiny_home_dir: $TMP/$name/home
pm_tiny_sock_file: $TMP/$name/pm.sock
pm_tiny_log_file: $TMP/$name/pm.log
pm_tiny_prog_cfg_file: $TMP/$name/prog.yaml
pm_tiny_app_log_dir: $TMP/$name/logs
pm_tiny_app_environ_dir: $TMP/$name/environ
pm_tiny_uds_abstract_namespace: false
pm_tiny_process_tree_mode: process_group
EOF
}

use_instance() {
    name=$1
    export PM_TINY_HOME="$TMP/$name/home"
    export PM_TINY_SOCK_FILE="$TMP/$name/pm.sock"
    export PM_TINY_PROG_CFG_FILE="$TMP/$name/prog.yaml"
    export PM_TINY_LOG_FILE="$TMP/$name/pm.log"
    export PM_TINY_APP_LOG_DIR="$TMP/$name/logs"
    export PM_TINY_APP_ENVIRON_DIR="$TMP/$name/environ"
    export PM_TINY_UDS_ABSTRACT_NAMESPACE=0
    export PM_TINY_PROCESS_TREE_MODE=process_group
}

wait_socket() {
    socket=$1
    for _ in $(seq 1 100); do
        [[ -S "$socket" ]] && return
        sleep .05
    done
    echo "socket did not become ready: $socket" >&2
    return 1
}

configure direct
use_instance direct
"$BIN/pm_tiny" -c "$TMP/direct/pm_tiny.yaml" >"$TMP/direct-daemon.out" 2>&1 &
DAEMON_PID=$!
wait_socket "$PM_TINY_SOCK_FILE"
"$BIN/pm" start long_running -- /usr/bin/sleep 300 >/dev/null
CHILD_PID=$("$BIN/pm" list --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["processes"][0]["pid"])')
timeout --kill-after=1s 10s "$BIN/pm" quit >"$TMP/direct-quit.out" 2>"$TMP/direct-quit.err"
wait "$DAEMON_PID"
DAEMON_PID=
! kill -0 "$CHILD_PID" 2>/dev/null
[[ ! -s "$TMP/direct-quit.out" ]]
[[ ! -s "$TMP/direct-quit.err" ]]

configure zombie
use_instance zombie
python3 - "$BIN/pm_tiny" "$TMP/zombie/pm_tiny.yaml" "$TMP/zombie/daemon.pid" "$TMP/zombie-daemon.out" <<'PY' &
import os, sys, time
binary, config, pid_file, output = sys.argv[1:]
pid = os.fork()
if pid == 0:
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.dup2(fd, 1)
    os.dup2(fd, 2)
    os.execv(binary, [binary, '-c', config])
with open(pid_file, 'w', encoding='ascii') as stream:
    stream.write(str(pid))
time.sleep(60)
PY
WRAPPER_PID=$!
for _ in $(seq 1 100); do
    [[ -f "$TMP/zombie/daemon.pid" ]] && break
    sleep .05
done
DAEMON_PID=$(cat "$TMP/zombie/daemon.pid")
wait_socket "$PM_TINY_SOCK_FILE"
"$BIN/pm" start zombie_child -- /usr/bin/sleep 300 >/dev/null
timeout --kill-after=1s 10s "$BIN/pm" quit >"$TMP/zombie-quit.out" 2>"$TMP/zombie-quit.err"
[[ ! -s "$TMP/zombie-quit.out" ]]
[[ ! -s "$TMP/zombie-quit.err" ]]
grep -q "^State:.*Z" "/proc/$DAEMON_PID/status"
kill "$WRAPPER_PID"
wait "$WRAPPER_PID" 2>/dev/null || true
WRAPPER_PID=
DAEMON_PID=

start_fake_server() {
    name=$1
    use_instance "$name"
    python3 - "$PM_TINY_SOCK_FILE" <<'PY' &
import os, socket, struct, sys, time
path = sys.argv[1]
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)
connection, _ = server.accept()
header = connection.recv(16)
while len(header) < 16:
    header += connection.recv(16 - len(header))
payload_size = struct.unpack('>I', header[12:16])[0]
while payload_size:
    chunk = connection.recv(payload_size)
    if not chunk:
        break
    payload_size -= len(chunk)
payload = struct.pack('>iI7si', 0, 7, b'success', os.getpid())
response = b'PMT3' + bytes((3, 1)) + header[6:8] + header[8:12] + struct.pack('>I', len(payload)) + payload
connection.sendall(response)
time.sleep(60)
PY
    FAKE_PID=$!
    wait_socket "$PM_TINY_SOCK_FILE"
}

configure interrupted
start_fake_server interrupted
set +e
"$BIN/pm" quit >"$TMP/interrupted.out" 2>"$TMP/interrupted.err" &
CLIENT_PID=$!
sleep .2
kill -INT "$CLIENT_PID"
wait "$CLIENT_PID"
INTERRUPTED_STATUS=$?
set -e
[[ $INTERRUPTED_STATUS -eq 130 ]]
grep -q 'interrupted while waiting' "$TMP/interrupted.err"
kill "$FAKE_PID" 2>/dev/null || true
wait "$FAKE_PID" 2>/dev/null || true
FAKE_PID=

configure timeout
start_fake_server timeout
set +e
"$BIN/pm" quit >"$TMP/timeout.out" 2>"$TMP/timeout.err"
TIMEOUT_STATUS=$?
set -e
[[ $TIMEOUT_STATUS -eq 1 ]]
grep -q 'timed out after 30 seconds' "$TMP/timeout.err"
grep -q "pid $FAKE_PID" "$TMP/timeout.err"
kill "$FAKE_PID" 2>/dev/null || true
wait "$FAKE_PID" 2>/dev/null || true
FAKE_PID=

echo 'quit integration: PASS'
