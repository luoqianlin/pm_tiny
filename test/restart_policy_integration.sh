#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_restart_policy.XXXXXX)"
DAEMON_PID=0

cleanup() {
    status=$?
    trap - EXIT
    if [[ "$DAEMON_PID" -gt 0 ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
    fi
    if [[ $status -ne 0 ]]; then
        [[ ! -f "$TMP/daemon.out" ]] || tail -200 "$TMP/daemon.out" >&2
        [[ ! -f "$TMP/pm.log" ]] || tail -200 "$TMP/pm.log" >&2
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
cat > "$TMP/crash_once.sh" <<EOF
#!/usr/bin/env bash
echo launch >> "$TMP/launches.txt"
sleep .3
exit 1
EOF
chmod +x "$TMP/crash_once.sh"
cat > "$TMP/prog.yaml" <<EOF
- name: crash_loop
  cwd: $TMP
  command: ./crash_once.sh
  start_timeout: 0
  daemon: true
  pty: false
  restart_delay_ms: 50
  restart_max_delay_ms: 50
  restart_window_ms: 10000
  restart_max_attempts: 2
  restart_reset_after_ms: 10000
EOF
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

for _ in $(seq 1 100); do
    status=$("$BIN/pm" list --json)
    state=$(python3 -c '
import json, sys
items = json.load(sys.stdin)["processes"]
print(next((item["state"] for item in items if item["name"] == "crash_loop"), "missing"))
' <<< "$status")
    [[ "$state" == stopped ]] && break
    sleep .05
done
[[ "$state" == stopped ]]
python3 -c '
import json, sys
item = next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "crash_loop")
assert item["restart_pending"] is False
assert item["restart_attempts_in_window"] == 2
assert item["restart_suppressed"] is True
assert item["restart_suppression_reason"] == "restart attempt limit reached"
' <<< "$status"
inspect=$("$BIN/pm" inspect crash_loop)
grep -q 'restart_attempts_in_window.*2' <<< "$inspect"
grep -q 'restart_suppressed.*Y' <<< "$inspect"
grep -q 'restart attempt limit reached' <<< "$inspect"
grep -q 'automatic restart suppressed after 2 attempts' "$TMP/pm.log"

before=$(wc -l < "$TMP/launches.txt")
[[ "$before" -eq 3 ]]
"$BIN/pm" start crash_loop >/dev/null
for _ in $(seq 1 100); do
    after=$(wc -l < "$TMP/launches.txt")
    [[ "$after" -ge 6 ]] && break
    sleep .05
done
[[ "$after" -eq 6 ]]
sleep .3
[[ $(wc -l < "$TMP/launches.txt") -eq 6 ]]

"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
