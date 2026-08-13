#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <adb-serial> [android-install-dir]" >&2
    exit 2
fi

SERIAL=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/scripts/android_production_guard.sh"
INSTALL_DIR=${2:-"$ROOT/.build_android/_install/Release"}
DAEMON_BIN="$INSTALL_DIR/bin/pm_tiny"
CLIENT_BIN="$INSTALL_DIR/bin/pm2"
for file in "$DAEMON_BIN" "$CLIENT_BIN"; do
    [[ -f "$file" ]] || { echo "missing required file: $file" >&2; exit 1; }
done

ADB=(adb -s "$SERIAL")
REMOTE_BASE="/data/local/tmp/pm_tiny_restart_policy_test_$$"
ARTIFACT_DIR="$ROOT/build/test-artifacts/android/$(date +%Y%m%d-%H%M%S)-${SERIAL}-restart"
mkdir -p "$ARTIFACT_DIR"

DAEMON_PID=""
PRODUCTION_DAEMON_PID=""
FINAL_STATUS=FAIL
cleanup() {
    local exit_code=$?
    set +e
    if [[ -n "$DAEMON_PID" ]]; then
        remote_pm quit >/dev/null 2>&1
    fi
    "${ADB[@]}" pull "$REMOTE_BASE" "$ARTIFACT_DIR/remote" >/dev/null 2>&1
    "${ADB[@]}" shell "rm -rf '$REMOTE_BASE'"
    printf 'status=%s\nexit_code=%d\n' "$FINAL_STATUS" "$exit_code" > "$ARTIFACT_DIR/result.txt"
    return "$exit_code"
}
trap cleanup EXIT

remote_pm() {
    "${ADB[@]}" shell "env PM_TINY_HOME='$REMOTE_BASE/home' PM_TINY_SOCK_FILE='$REMOTE_BASE/pm.sock' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 '$REMOTE_BASE/pm2_test' $*"
}

wait_for_suppression() {
    local expected_restarts=$1 output
    for _ in $(seq 1 120); do
        output=$(remote_pm list --json 2>/dev/null || true)
        if python3 -c '
import json, sys
expected = int(sys.argv[1])
try:
    data = json.load(sys.stdin)
    item = next(item for item in data["processes"] if item["name"] == "crash_loop")
except Exception:
    raise SystemExit(1)
assert data["schema_version"] == 2
assert item["state"] == "stopped"
assert item["restart_count"] == expected
assert item["restart_pending"] is False
assert item["restart_attempts_in_window"] == 2
assert item["restart_suppressed"] is True
assert item["restart_suppression_reason"] == "restart attempt limit reached"
' "$expected_restarts" <<< "$output" 2>/dev/null; then
            printf '%s\n' "$output" > "$ARTIFACT_DIR/list-${expected_restarts}.json"
            return 0
        fi
        sleep 0.1
    done
    echo "restart suppression did not reach restart_count=$expected_restarts" >&2
    printf '%s\n' "$output" >&2
    return 1
}

"${ADB[@]}" get-state | grep -qx device
"${ADB[@]}" root >/dev/null
"${ADB[@]}" wait-for-device
PRODUCTION_DAEMON_PID=$("${ADB[@]}" shell "pidof pm_tiny" | tr -d '\r')
[[ "$PRODUCTION_DAEMON_PID" =~ ^[0-9]+$ ]]
capture_production_state "$ARTIFACT_DIR/production-before.json"

cat > "$ARTIFACT_DIR/crash_once.sh" <<'EOF'
#!/system/bin/sh
sleep 0.3
exit 1
EOF
cat > "$ARTIFACT_DIR/prog.yaml" <<EOF
- name: crash_loop
  cwd: $REMOTE_BASE
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
cat > "$ARTIFACT_DIR/pm_tiny.yaml" <<EOF
pm_tiny_home_dir: $REMOTE_BASE/home
pm_tiny_sock_file: $REMOTE_BASE/pm.sock
pm_tiny_log_file: $REMOTE_BASE/pm.log
pm_tiny_prog_cfg_file: $REMOTE_BASE/prog.yaml
pm_tiny_app_log_dir: $REMOTE_BASE/logs
pm_tiny_app_environ_dir: $REMOTE_BASE/environ
pm_tiny_uds_abstract_namespace: false
pm_tiny_process_tree_mode: process_group
EOF

"${ADB[@]}" shell "mkdir -p '$REMOTE_BASE/home' '$REMOTE_BASE/logs' '$REMOTE_BASE/environ'"
"${ADB[@]}" push "$DAEMON_BIN" "$REMOTE_BASE/pm_tiny_test" >/dev/null
"${ADB[@]}" push "$CLIENT_BIN" "$REMOTE_BASE/pm2_test" >/dev/null
"${ADB[@]}" push "$ARTIFACT_DIR/crash_once.sh" "$REMOTE_BASE/crash_once.sh" >/dev/null
"${ADB[@]}" push "$ARTIFACT_DIR/prog.yaml" "$REMOTE_BASE/prog.yaml" >/dev/null
"${ADB[@]}" push "$ARTIFACT_DIR/pm_tiny.yaml" "$REMOTE_BASE/pm_tiny.yaml" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE_BASE/pm_tiny_test' '$REMOTE_BASE/pm2_test' '$REMOTE_BASE/crash_once.sh'; env PM_TINY_HOME='$REMOTE_BASE/home' PM_TINY_SOCK_FILE='$REMOTE_BASE/pm.sock' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 nohup '$REMOTE_BASE/pm_tiny_test' -c '$REMOTE_BASE/pm_tiny.yaml' </dev/null >'$REMOTE_BASE/daemon.out' 2>&1 &"

for _ in $(seq 1 100); do
    if remote_pm version >/dev/null 2>&1; then break; fi
    sleep 0.1
done
DAEMON_PID=$("${ADB[@]}" shell "cat '$REMOTE_BASE/home/pm_tiny.pid'" | tr -d '\r')
[[ "$DAEMON_PID" =~ ^[0-9]+$ ]]

wait_for_suppression 2
remote_pm inspect crash_loop > "$ARTIFACT_DIR/inspect-first.txt"
grep -q 'restart_attempts_in_window.*2' "$ARTIFACT_DIR/inspect-first.txt"
grep -q 'restart_suppressed.*Y' "$ARTIFACT_DIR/inspect-first.txt"
grep -q 'restart attempt limit reached' "$ARTIFACT_DIR/inspect-first.txt"

remote_pm start crash_loop >/dev/null
wait_for_suppression 4
remote_pm inspect crash_loop > "$ARTIFACT_DIR/inspect-second.txt"
grep -q 'restart_suppressed.*Y' "$ARTIFACT_DIR/inspect-second.txt"

remote_pm quit >/dev/null
DAEMON_PID=""
"${ADB[@]}" shell "kill -0 '$PRODUCTION_DAEMON_PID'"
capture_production_state "$ARTIFACT_DIR/production-after.json"
assert_production_unchanged "$ARTIFACT_DIR/production-before.json" "$ARTIFACT_DIR/production-after.json"

"${ADB[@]}" pull "$REMOTE_BASE" "$ARTIFACT_DIR/remote" >/dev/null
FINAL_STATUS=PASS
printf 'android restart policy regression: PASS\nartifacts: %s\n' "$ARTIFACT_DIR"
