#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_protocol.XXXXXX)"
cleanup() {
    status=$?
    trap - EXIT
    if [[ $status -ne 0 ]]; then
        if kill -0 "${DAEMON_PID:-0}" 2>/dev/null; then
            echo "daemon was still alive when the test failed" >&2
        else
            set +e
            wait "${DAEMON_PID:-0}"
            daemon_status=$?
            set -e
            echo "daemon had exited with status $daemon_status" >&2
        fi
    fi
    kill "${DAEMON_PID:-0}" 2>/dev/null || true
    if [[ $status -ne 0 ]]; then
        [[ ! -f "$TMP/daemon.out" ]] || { echo "--- daemon.out ---" >&2; tail -200 "$TMP/daemon.out" >&2; }
        if [[ -f "$TMP/restart.out" ]]; then
            echo "--- restart.out diagnostics ---" >&2
            grep -E -i -C 4 'fatal|sanitizer|runtime error|heap-use|stack-use|double-free|assert|bad file descriptor|terminate called' "$TMP/restart.out" >&2 || true
            echo "--- restart.out tail ---" >&2
            tail -80 "$TMP/restart.out" >&2
        fi
        [[ ! -f "$TMP/pm.log" ]] || { echo "--- pm.log ---" >&2; tail -200 "$TMP/pm.log" >&2; }
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT
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
for _ in $(seq 1 50); do [[ -S "$TMP/pm.sock" ]] && break; sleep .05; done
[[ -S "$TMP/pm.sock" ]]

"$BIN/pm" --version | grep -q 'pm_tiny: 2.0.0'
"$BIN/pm" ls | grep -q 'Total: 0'
"$BIN/pm" ls --json | python3 -c 'import json,sys; data=json.load(sys.stdin); assert data == {"schema_version": 2, "total": 0, "processes": []}'
"$BIN/pm" save
grep -qxF '[]' "$TMP/prog.yaml"
"$BIN/pm" reload | grep -q 'Total: 0'

"$BIN/pm" start '/usr/bin/sleep 5' --name persisted --no_daemon --no_pty >/dev/null
"$BIN/pm" save >/dev/null
grep -q 'name: persisted' "$TMP/prog.yaml"
grep -q 'pty: false' "$TMP/prog.yaml"
"$BIN/pm" stop persisted >/dev/null
"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
"$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" > "$TMP/restart.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [[ -S "$TMP/pm.sock" ]] && break; sleep .05; done
for _ in $(seq 1 20); do
    kill -HUP "$DAEMON_PID"
    kill -ALRM "$DAEMON_PID"
done
sleep .2
"$BIN/pm" ls >/dev/null
"$BIN/pm" ls | grep persisted
"$BIN/pm" delete persisted >/dev/null
"$BIN/pm" save >/dev/null

"$BIN/pm" start '/usr/bin/sleep 5' --name app --no_daemon | grep -q Success
"$BIN/pm" inspect app | grep -q '/usr/bin/sleep 5'
"$BIN/pm" restart app | grep -q Success
"$BIN/pm" stop app | grep -q Success
"$BIN/pm" delete app | grep -q Success

"$BIN/pm" start '/usr/bin/sleep 30' --name generation_app --no_daemon --no_pty >/dev/null
for _ in $(seq 1 20); do
    "$BIN/pm" list --json | python3 -c '
import json, sys
data = json.load(sys.stdin)
sys.exit(0 if any(item["name"] == "generation_app" and item["state"] == "online" for item in data["processes"]) else 1)
' && break
    sleep .05
done
"$BIN/pm" list --wide --no-color | grep -q 'command'
"$BIN/pm" list --json | python3 -c '
import json, sys
data = json.load(sys.stdin)
item = next(item for item in data["processes"] if item["name"] == "generation_app")
assert item["state"] == "online"
assert item["uptime_ms"] is not None and item["uptime_ms"] >= 0
assert item["memory_kib"] is not None and item["memory_kib"] > 0
assert item["pty"] is False
'
if "$BIN/pm" list --wide --json >/dev/null 2>&1; then
    echo "list unexpectedly accepted --wide with --json" >&2
    exit 1
fi
for _ in $(seq 1 10); do
    "$BIN/pm" restart generation_app >/dev/null
done
"$BIN/pm" ls | grep generation_app | grep -q online
"$BIN/pm" delete generation_app >/dev/null

"$BIN/pm" start '/usr/bin/printf integration' --name logapp --no_daemon --log > "$TMP/log.out"
grep -q integration "$TMP/log.out"
grep -q 'PM_TINY MESSAGE' "$TMP/log.out"
"$BIN/pm" delete logapp >/dev/null

"$BIN/pm" start '/usr/bin/sleep 30' --name log_interrupt --no_daemon >/dev/null
"$BIN/pm" start 'yes' --name log_interrupt_busy --no_daemon >/dev/null
timeout --kill-after=1s 5s bash -c '
set -e
pm="$1"
tmp_dir="$2"
client_pid=
cleanup() {
    if [[ -n "$client_pid" ]]; then
        kill "$client_pid" 2>/dev/null || true
        wait "$client_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM
interrupt_log() {
    app_name="$1"
    log_out="$2"
    "$pm" log "$app_name" > "$log_out" &
    client_pid=$!
    sleep .2
    for _ in $(seq 1 50); do
        kill -0 "$client_pid" 2>/dev/null || break
        kill -INT "$client_pid" 2>/dev/null || true
        sleep .02
    done
    wait "$client_pid"
    client_pid=
}
interrupt_log log_interrupt "$tmp_dir/log-interrupt.out"
interrupt_log log_interrupt_busy /dev/null
' bash "$BIN/pm" "$TMP"
"$BIN/pm" delete log_interrupt >/dev/null
"$BIN/pm" delete log_interrupt_busy >/dev/null

python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect(sys.argv[1]); s.sendall(b"BAD!"); s.close()' "$TMP/pm.sock"
"$BIN/pm" ls | grep -q 'Total: 0'

"$BIN/pm" start "$BIN/test/test_sdk" --name sdkapp --no_daemon --start_timeout 10 >/dev/null
sleep 2
"$BIN/pm" ls | grep sdkapp | grep -q online
grep -q 'app `sdkapp` ready' "$TMP/pm.log"
grep -q 'recv `sdkapp` tick' "$TMP/pm.log"
"$BIN/pm" stop sdkapp >/dev/null

"$BIN/pm" inspect missing 2>&1 | grep -q 'not found'
"$BIN/pm" stop missing 2>&1 | grep -q 'not found'
"$BIN/pm" start '/usr/bin/sleep 30' --name signal_exit_app --no_daemon --no_pty >/dev/null
SIGNAL_EXIT_PID=$("$BIN/pm" ls | awk -F'|' '/signal_exit_app/ {gsub(/ /, "", $2); print $2}')
[[ -n "$SIGNAL_EXIT_PID" ]]
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID"
grep -q 'rev signal:SIGHUP' "$TMP/pm.log"
grep -q 'rev signal:SIGALRM' "$TMP/pm.log"
grep -q 'rev signal:SIGTERM' "$TMP/pm.log"
for _ in $(seq 1 50); do
    kill -0 "$SIGNAL_EXIT_PID" 2>/dev/null || break
    sleep .05
done
! kill -0 "$SIGNAL_EXIT_PID" 2>/dev/null

export PM_TINY_SOCK_FILE="pm_tiny_protocol_abstract"
export PM_TINY_UDS_ABSTRACT_NAMESPACE=1
"$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" > "$TMP/abstract.out" 2>&1 &
DAEMON_PID=$!
sleep .2
"$BIN/pm" ls | grep -q 'Total: 0'
"$BIN/pm" start "$BIN/test/test_sdk" --name abstract_sdk --no_daemon --start_timeout 10 >/dev/null
sleep 2
"$BIN/pm" ls | grep abstract_sdk | grep -q online
grep -q 'app `abstract_sdk` ready' "$TMP/pm.log"
grep -q 'recv `abstract_sdk` tick' "$TMP/pm.log"
"$BIN/pm" stop abstract_sdk >/dev/null
"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
echo 'protocol integration: PASS'
