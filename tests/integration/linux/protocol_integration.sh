#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
EXPECTED_VERSION=$(tr -d '\r\n' < "$ROOT/VERSION")
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
    if [[ ${INNOCENT_PID:-0} -gt 0 ]]; then
        kill "$INNOCENT_PID" 2>/dev/null || true
    fi
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
export PM_TINY_TEST_SAVE_DELAY_MS=300
export PM_TINY_LOG_LEVEL=debug

"$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" > "$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [[ -S "$TMP/pm.sock" ]] && break; sleep .05; done
[[ -S "$TMP/pm.sock" ]]

signal_mask_before=$(awk '/^SigBlk:/ {print $2}' "/proc/$DAEMON_PID/status")
set +e
"$BIN/pm" start missing_executable --no-daemon -- "$TMP/does-not-exist" > "$TMP/missing-exec.out" 2>&1
missing_exec_status=$?
set -e
[[ $missing_exec_status -ne 0 ]]
signal_mask_after=$(awk '/^SigBlk:/ {print $2}' "/proc/$DAEMON_PID/status")
[[ "$signal_mask_after" == "$signal_mask_before" ]]

"$BIN/pm" start relative_exec --no-daemon --cwd /tmp -- ../bin/sleep 30 >/dev/null
relative_exec_inspect=$("$BIN/pm" inspect relative_exec)
[[ "$relative_exec_inspect" == *"/bin/sleep"* ]]
"$BIN/pm" stop relative_exec --no-list >/dev/null
"$BIN/pm" delete relative_exec --no-list >/dev/null

if id nobody >/dev/null 2>&1 && [[ $(id -u nobody) != "$(id -u)" ]]; then
    set +e
    "$BIN/pm" start forbidden_identity --user nobody -- /bin/true \
        >"$TMP/forbidden-identity.out" 2>&1
    forbidden_identity_status=$?
    set -e
    [[ $forbidden_identity_status -eq 1 ]]
    grep -q "pm_tiny uid $(id -u) cannot start.*as user.*nobody" "$TMP/forbidden-identity.out"
fi

/usr/bin/sleep 30 &
INNOCENT_PID=$!
"$BIN/pm" start ownership_probe --no-daemon --no-pty -- /usr/bin/sleep 30 >/dev/null
kill -0 "$INNOCENT_PID"
"$BIN/pm" delete ownership_probe --no-list >/dev/null
kill -0 "$INNOCENT_PID"
kill "$INNOCENT_PID"
wait "$INNOCENT_PID" 2>/dev/null || true
INNOCENT_PID=0

python3 - "$TMP/pm.sock" <<'PY'
import socket
import struct
import sys

HEADER = struct.Struct(">4sBBHII")

def request(request_id):
    return HEADER.pack(b"PMT3", 3, 0, 0x29, request_id, 0)

def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise SystemExit("connection closed before both sticky frames were returned")
        data.extend(chunk)
    return bytes(data)

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.settimeout(2)
client.connect(sys.argv[1])
client.sendall(request(101) + request(102))
for expected_id in (101, 102):
    magic, version, flags, msg_type, request_id, size = HEADER.unpack(read_exact(client, HEADER.size))
    assert magic == b"PMT3" and version == 3
    assert flags & 1 and msg_type == 0x29 and request_id == expected_id
    read_exact(client, size)
client.close()
PY

baseline_fds=$(find "/proc/$DAEMON_PID/fd" -mindepth 1 -maxdepth 1 | wc -l)
run_connection_churn() {
python3 - "$TMP/pm.sock" <<'PY'
import socket
import sys

for _ in range(5000):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(sys.argv[1])
    client.close()
PY
}
wait_for_fd_recovery() {
for _ in $(seq 1 100); do
    current_fds=$(find "/proc/$DAEMON_PID/fd" -mindepth 1 -maxdepth 1 | wc -l)
    (( current_fds <= baseline_fds + 2 )) && break
    sleep .02
done
(( current_fds <= baseline_fds + 2 ))
}
wait_for_control_recovery() {
for _ in $(seq 1 100); do
    "$BIN/pm" --version >/dev/null 2>&1 && return 0
    sleep .02
done
return 1
}
run_connection_churn
wait_for_fd_recovery
wait_for_control_recovery
warmed_rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$DAEMON_PID/status")
run_connection_churn
wait_for_fd_recovery
wait_for_control_recovery
current_rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$DAEMON_PID/status")
if [[ "${PM_TINY_TEST_SANITIZED:-0}" != 1 ]]; then
    (( current_rss <= warmed_rss + 4096 ))
fi

if command -v sudo >/dev/null && sudo -n true >/dev/null 2>&1 && id nobody >/dev/null 2>&1; then
    chmod 711 "$TMP"
    chmod 777 "$TMP/pm.sock"
    sudo -n -u nobody python3 - "$TMP/pm.sock" <<'PY'
import socket
import sys

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.settimeout(2)
client.connect(sys.argv[1])
client.sendall(b"PMT3")
try:
    data = client.recv(1)
except ConnectionResetError:
    data = b""
if data:
    raise SystemExit("unauthorized peer was not disconnected")
PY
    grep -q 'reject control connection.*uid=' "$TMP/pm.log"
fi
wait_for_control_recovery

"$BIN/pm" --version | grep -qF "pm_tiny: $EXPECTED_VERSION"
"$BIN/pm" ls | grep -q 'Total: 0'
"$BIN/pm" ls --json | python3 -c 'import json,sys; data=json.load(sys.stdin); assert data == {"schema_version": 5, "total": 0, "processes": []}'
mkdir -p "$TMP/yaml-cwd"
cat > "$TMP/yaml-cwd/runner" <<'EOF'
#!/bin/sh
sleep "$1"
EOF
chmod +x "$TMP/yaml-cwd/runner"
cat > "$TMP/prog.yaml" <<EOF
- name: yaml_relative
  executable: ./runner
  args: ["30"]
  cwd: $TMP/yaml-cwd
  daemon: false
- name: yaml_path
  executable: sleep
  args: ["30"]
  cwd: $TMP/yaml-cwd
  daemon: false
EOF
[[ -z "$("$BIN/pm" reload --no-list)" ]]
grep -q './runner' <<< "$("$BIN/pm" inspect yaml_relative)"
grep -q 'executable.*sleep' <<< "$("$BIN/pm" inspect yaml_path)"
"$BIN/pm" stop yaml_relative --no-list >/dev/null
"$BIN/pm" stop yaml_path --no-list >/dev/null
"$BIN/pm" delete yaml_relative --no-list >/dev/null
"$BIN/pm" delete yaml_path --no-list >/dev/null
"$BIN/pm" save >/dev/null
grep -qxF '[]' "$TMP/prog.yaml"
"$BIN/pm" save > "$TMP/save.out" &
SAVE_PID=$!
sleep .05
if "$BIN/pm" save > "$TMP/save-busy.out" 2>&1; then
    echo "concurrent save unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'persistence operation busy' "$TMP/save-busy.out"
wait "$SAVE_PID"
[[ ! -s "$TMP/save.out" ]]
grep -qxF '[]' "$TMP/prog.yaml"
"$BIN/pm" reload | grep -q 'Total: 0'

"$BIN/pm" start persisted --no-daemon --no-pty -- /usr/bin/sleep 5 >/dev/null
"$BIN/pm" save >/dev/null
grep -q 'name: persisted' "$TMP/prog.yaml"
grep -q 'pty: false' "$TMP/prog.yaml"
grep -q 'log_mode: split' "$TMP/prog.yaml"
grep -q 'log_archive_count: 3' "$TMP/prog.yaml"
[[ -f "$TMP/environ/persisted.yaml" ]]
grep -q '^schema: 1$' "$TMP/environ/persisted.yaml"
"$BIN/pm" stop persisted --no-list >/dev/null
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
"$BIN/pm" delete persisted --no-list >/dev/null
"$BIN/pm" save >/dev/null

"$BIN/pm" start app --no-daemon -- /usr/bin/sleep 5 | grep -q started
APP_INSPECT=$("$BIN/pm" inspect app)
grep -q '/usr/bin/sleep' <<< "$APP_INSPECT"
grep -q 'generation.*[1-9]' <<< "$APP_INSPECT"
grep -q 'config_source.*runtime' <<< "$APP_INSPECT"
grep -q 'log_mode.*split' <<< "$APP_INSPECT"
grep -q 'log_degraded.*N' <<< "$APP_INSPECT"
grep -Eq 'process_tree_backend.*(process_group|cgroup)' <<< "$APP_INSPECT"
"$BIN/pm" restart app --no-list >/dev/null
"$BIN/pm" stop app --no-list >/dev/null
"$BIN/pm" delete app --no-list >/dev/null

"$BIN/pm" start generation_app --no-daemon --no-pty -- /usr/bin/sleep 30 >/dev/null
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
assert item["generation"] > 0
assert item["ready"] is True
assert item["heartbeat_enabled"] is False
assert item["process_tree_backend"] in ("cgroup", "process_group")
assert item["config_source"] == "runtime"
'
if "$BIN/pm" list --wide --json >/dev/null 2>&1; then
    echo "list unexpectedly accepted --wide with --json" >&2
    exit 1
fi
for _ in $(seq 1 10); do
    "$BIN/pm" restart generation_app --no-list >/dev/null
done
"$BIN/pm" ls | grep generation_app | grep -q online
"$BIN/pm" delete generation_app --no-list >/dev/null

"$BIN/pm" start logapp --no-daemon --log -- /bin/sh -c 'printf integration; sleep 5' > "$TMP/log.out"
grep -q integration "$TMP/log.out"
grep -q '\[pm_tiny\] process `logapp` exited:' "$TMP/log.out"
"$BIN/pm" delete logapp --no-list >/dev/null

"$BIN/pm" start log_history --no-daemon -- /usr/bin/printf history-marker >/dev/null
for _ in $(seq 1 100); do
    "$BIN/pm" ls --json | grep -q '"name":"log_history".*"state":"stopped"' && break
    sleep .05
done
if "$BIN/pm" log log_history >"$TMP/log-history-live.out" 2>"$TMP/log-history-live.err"; then
    echo "plain pm log unexpectedly accepted a stopped process" >&2
    exit 1
fi
[[ ! -s "$TMP/log-history-live.out" ]]
grep -q -- '--history' "$TMP/log-history-live.err"
"$BIN/pm" log log_history --history >"$TMP/log-history.out"
head -1 "$TMP/log-history.out" | grep -q 'showing cached log for stopped process `log_history`'
grep -q history-marker "$TMP/log-history.out"
! grep -q 'process `log_history` exited' "$TMP/log-history.out"
"$BIN/pm" log log_history --history >"$TMP/log-history-second.out"
cmp "$TMP/log-history.out" "$TMP/log-history-second.out"
"$BIN/pm" delete log_history --no-list >/dev/null

cat >"$TMP/restart-generation.sh" <<'SH'
#!/bin/sh
counter_file="$1"
generation=$(($(cat "$counter_file" 2>/dev/null || echo 0) + 1))
printf '%s\n' "$generation" >"$counter_file"
printf 'GENERATION_%s\n' "$generation"
if [ "$generation" -eq 1 ]; then sleep 30; else sleep .1; fi
SH
chmod +x "$TMP/restart-generation.sh"
"$BIN/pm" start restart_log_generation --no-daemon -- "$TMP/restart-generation.sh" \
    "$TMP/restart-generation.count" >/dev/null
for _ in $(seq 1 100); do [[ -f "$TMP/restart-generation.count" ]] && break; sleep .05; done
"$BIN/pm" restart restart_log_generation --log >"$TMP/restart-generation.out"
grep -q GENERATION_2 "$TMP/restart-generation.out"
! grep -q GENERATION_1 "$TMP/restart-generation.out"
grep -q 'process `restart_log_generation` exited:' "$TMP/restart-generation.out"
"$BIN/pm" delete restart_log_generation --no-list >/dev/null

cat >"$TMP/automatic-log-generation.sh" <<'SH'
#!/bin/sh
counter_file="$1"
generation=$(($(cat "$counter_file" 2>/dev/null || echo 0) + 1))
printf '%s\n' "$generation" >"$counter_file"
printf 'AUTOMATIC_GENERATION_%s\n' "$generation"
sleep .1
SH
chmod +x "$TMP/automatic-log-generation.sh"
"$BIN/pm" start automatic_log_wait --restart-delay-ms 1500 --restart-max-delay-ms 1500 -- \
    "$TMP/automatic-log-generation.sh" "$TMP/automatic-log-generation.count" >/dev/null
automatic_log_pending=no
for _ in $(seq 1 100); do
    automatic_log_status=$("$BIN/pm" ls --json)
    automatic_log_pending=$(python3 -c '
import json, sys
item = next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "automatic_log_wait")
print("yes" if item["restart_pending"] and item["pid"] is None else "no")
' <<< "$automatic_log_status")
    [[ "$automatic_log_pending" == yes ]] && break
    sleep .05
done
[[ "$automatic_log_pending" == yes ]]
timeout --kill-after=1s 10s "$BIN/pm" log automatic_log_wait >"$TMP/automatic-log-wait.out"
grep -q AUTOMATIC_GENERATION_2 "$TMP/automatic-log-wait.out"
! grep -q AUTOMATIC_GENERATION_1 "$TMP/automatic-log-wait.out"
grep -q 'process `automatic_log_wait` exited:' "$TMP/automatic-log-wait.out"
"$BIN/pm" delete automatic_log_wait --no-list >/dev/null

"$BIN/pm" start log_delete --no-daemon --kill-timeout 1 -- /usr/bin/sleep 30 >/dev/null
timeout --kill-after=1s 5s "$BIN/pm" log log_delete > "$TMP/log-delete.out" &
LOG_DELETE_PID=$!
sleep .2
"$BIN/pm" delete log_delete --no-list >/dev/null
wait "$LOG_DELETE_PID"
grep -q '\[pm_tiny\] process `log_delete` exited:' "$TMP/log-delete.out"
wait_for_fd_recovery

"$BIN/pm" start combined_log --no-daemon --log-mode combined -- /usr/bin/printf combined-marker >/dev/null
for _ in $(seq 1 20); do [[ -f "$TMP/logs/combined_log.log" ]] && break; sleep .05; done
grep -q combined-marker "$TMP/logs/combined_log.log"
[[ ! -e "$TMP/logs/combined_log_stdout.log" ]]
"$BIN/pm" delete combined_log --no-list >/dev/null

printf 'not-a-directory\n' > "$TMP/bad-log-parent"
"$BIN/pm" start log_degrade --no-daemon --log-dir "$TMP/bad-log-parent/child" -- \
    /bin/sh -c 'echo first; sleep 2; echo second; sleep 2' >/dev/null
for _ in $(seq 1 20); do
    "$BIN/pm" inspect log_degrade | grep -q 'log_degraded.*Y' && break
    sleep .05
done
"$BIN/pm" inspect log_degrade | grep -q 'log_degraded.*Y'
rm "$TMP/bad-log-parent"
mkdir "$TMP/bad-log-parent"
for _ in $(seq 1 50); do
    [[ -f "$TMP/bad-log-parent/child/log_degrade_stdout.log" ]] && \
        grep -q second "$TMP/bad-log-parent/child/log_degrade_stdout.log" && break
    sleep .1
done
grep -q second "$TMP/bad-log-parent/child/log_degrade_stdout.log"
"$BIN/pm" inspect log_degrade | grep -q 'log_degraded.*N'
"$BIN/pm" delete log_degrade --no-list >/dev/null

"$BIN/pm" start log_interrupt --no-daemon -- /usr/bin/sleep 30 >/dev/null
"$BIN/pm" start log_interrupt_busy --no-daemon -- /usr/bin/yes >/dev/null
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
    set +e
    wait "$client_pid"
    status=$?
    set -e
    [[ $status -eq 130 ]]
    client_pid=
}
interrupt_log log_interrupt "$tmp_dir/log-interrupt.out"
interrupt_log log_interrupt_busy /dev/null
' bash "$BIN/pm" "$TMP"
"$BIN/pm" delete log_interrupt --no-list >/dev/null
"$BIN/pm" delete log_interrupt_busy --no-list >/dev/null

python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect(sys.argv[1]); s.sendall(b"BAD!"); s.close()' "$TMP/pm.sock"
"$BIN/pm" ls | grep -q 'Total: 0'

"$BIN/pm" start sdkapp --no-daemon --start-timeout 10 -- "$BIN/tests/sdk_heartbeat_fixture" >/dev/null
sleep 2
"$BIN/pm" ls | grep sdkapp | grep -q online
grep -q 'app `sdkapp` ready' "$TMP/pm.log"
grep -q 'recv `sdkapp` tick' "$TMP/pm.log"
python3 - "$TMP/pm.sock" <<'PY'
import socket
import struct
import sys

name = b"sdkapp"
payload = struct.pack(">I", len(name)) + name
frame = struct.pack(">4sBBHII", b"PMT3", 3, 0, 0x32, 7001, len(payload)) + payload
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(sys.argv[1])
client.sendall(frame)
try:
    client.recv(1)
except ConnectionResetError:
    pass
client.close()
PY
for _ in $(seq 1 20); do
    grep -q 'reject `sdkapp` tick.*peer is not in current process tree' "$TMP/pm.log" && break
    sleep .05
done
grep -q 'reject `sdkapp` tick.*peer is not in current process tree' "$TMP/pm.log"
"$BIN/pm" stop sdkapp --no-list >/dev/null

set +e
MISSING_INSPECT=$("$BIN/pm" inspect missing 2>&1)
MISSING_INSPECT_STATUS=$?
MISSING_STOP=$("$BIN/pm" stop missing 2>&1)
MISSING_STOP_STATUS=$?
set -e
[[ $MISSING_INSPECT_STATUS -ne 0 ]]
[[ $MISSING_STOP_STATUS -ne 0 ]]
grep -q 'not found' <<<"$MISSING_INSPECT"
grep -q 'not found' <<<"$MISSING_STOP"
"$BIN/pm" start signal_exit_app --no-daemon --no-pty -- /usr/bin/sleep 30 >/dev/null
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
"$BIN/pm" start abstract_sdk --no-daemon --start-timeout 10 -- "$BIN/tests/sdk_heartbeat_fixture" >/dev/null
sleep 2
"$BIN/pm" ls | grep abstract_sdk | grep -q online
grep -q 'app `abstract_sdk` ready' "$TMP/pm.log"
grep -q 'recv `abstract_sdk` tick' "$TMP/pm.log"
"$BIN/pm" stop abstract_sdk --no-list >/dev/null
"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
echo 'protocol integration: PASS'
