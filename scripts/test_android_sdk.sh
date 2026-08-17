#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "usage: $0 <adb-serial> [android-install-dir] [sdk-probe]" >&2
    exit 2
fi

SERIAL=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXPECTED_VERSION=$(tr -d '\r\n' < "$ROOT/VERSION")
source "$ROOT/scripts/android_production_guard.sh"
INSTALL_DIR=${2:-"$ROOT/build-android-arm64/_install/Release"}
PROBE_BIN=${3:-"$ROOT/build-android-arm64/pm_sdk_ready_tick_probe"}
DAEMON_BIN="$INSTALL_DIR/bin/pm_tiny"
CLIENT_BIN="$INSTALL_DIR/bin/pm2"
for file in "$DAEMON_BIN" "$CLIENT_BIN" "$PROBE_BIN"; do
    [[ -f "$file" ]] || { echo "missing required file: $file" >&2; exit 1; }
done

ADB=(adb -s "$SERIAL")
REMOTE_BASE="/data/local/tmp/pm_tiny_sdk_test_$$"
[[ "$REMOTE_BASE" =~ ^/data/local/tmp/pm_tiny_sdk_test_[0-9]+$ ]] || exit 1
STAMP=$(date +%Y%m%d-%H%M%S)
ARTIFACT_DIR="$ROOT/build/test-artifacts/android/${STAMP}-${SERIAL}-sdk"
mkdir -p "$ARTIFACT_DIR"
PRODUCTION_PID=$(${ADB[@]} shell "pidof pm_tiny" | tr -d '\r' | xargs)
[[ "$PRODUCTION_PID" =~ ^[0-9]+$ ]] || { echo "cannot identify the single production pm_tiny" >&2; exit 1; }
capture_production_state "$ARTIFACT_DIR/production-before.json"

FINAL_STATUS=FAIL
cleanup() {
    local exit_code=$?
    set +e
    "${ADB[@]}" shell "env PM_TINY_HOME='$REMOTE_BASE/home' PM_TINY_SOCK_FILE='$REMOTE_BASE/pm.sock' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 '$REMOTE_BASE/pm2_test' quit >/dev/null 2>&1 || true"
    "${ADB[@]}" pull "$REMOTE_BASE" "$ARTIFACT_DIR/remote" >/dev/null 2>&1
    "${ADB[@]}" shell "test ! -f '$REMOTE_BASE/home/pm_tiny.pid' || kill -9 \$(cat '$REMOTE_BASE/home/pm_tiny.pid') >/dev/null 2>&1 || true; rm -rf '$REMOTE_BASE'"
    local production_after
    production_after=$(${ADB[@]} shell "pidof pm_tiny" | tr -d '\r' | xargs)
    [[ "$production_after" == "$PRODUCTION_PID" ]] || exit_code=1
    capture_production_state "$ARTIFACT_DIR/production-after.json" || exit_code=1
    assert_production_unchanged "$ARTIFACT_DIR/production-before.json" \
        "$ARTIFACT_DIR/production-after.json" || exit_code=1
    printf 'status=%s\nexit_code=%d\nproduction_pid=%s\n' \
        "$FINAL_STATUS" "$exit_code" "$production_after" > "$ARTIFACT_DIR/result.txt"
    return "$exit_code"
}
trap cleanup EXIT

cat > "$ARTIFACT_DIR/prog.yaml" <<EOF
- name: sdk_probe
  executable: $REMOTE_BASE/sdk_probe
  args: ["10000"]
  cwd: $REMOTE_BASE
  daemon: false
  pty: false
  start_timeout: 3
  heartbeat_timeout: 1
EOF
cat > "$ARTIFACT_DIR/pm_tiny.yaml" <<EOF
pm_tiny_home_dir: $REMOTE_BASE/home
pm_tiny_sock_file: $REMOTE_BASE/pm.sock
pm_tiny_uds_abstract_namespace: false
pm_tiny_prog_cfg_file: $REMOTE_BASE/prog.yaml
pm_tiny_log_file: $REMOTE_BASE/pm_tiny.log
pm_tiny_log_level: debug
pm_tiny_app_log_dir: $REMOTE_BASE/logs
pm_tiny_app_environ_dir: $REMOTE_BASE/environ
EOF

"${ADB[@]}" shell "mkdir -p '$REMOTE_BASE/home' '$REMOTE_BASE/logs' '$REMOTE_BASE/environ'"
"${ADB[@]}" push "$DAEMON_BIN" "$REMOTE_BASE/pm_tiny_test" >/dev/null
"${ADB[@]}" push "$CLIENT_BIN" "$REMOTE_BASE/pm2_test" >/dev/null
"${ADB[@]}" push "$PROBE_BIN" "$REMOTE_BASE/sdk_probe" >/dev/null
"${ADB[@]}" push "$ARTIFACT_DIR/prog.yaml" "$REMOTE_BASE/prog.yaml" >/dev/null
"${ADB[@]}" push "$ARTIFACT_DIR/pm_tiny.yaml" "$REMOTE_BASE/pm_tiny.yaml" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE_BASE/pm_tiny_test' '$REMOTE_BASE/pm2_test' '$REMOTE_BASE/sdk_probe'; env PM_TINY_HOME='$REMOTE_BASE/home' PM_TINY_SOCK_FILE='$REMOTE_BASE/pm.sock' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 nohup '$REMOTE_BASE/pm_tiny_test' -c '$REMOTE_BASE/pm_tiny.yaml' </dev/null >'$REMOTE_BASE/daemon.out' 2>&1 &"

remote_pm() {
    "${ADB[@]}" shell "env PM_TINY_HOME='$REMOTE_BASE/home' PM_TINY_SOCK_FILE='$REMOTE_BASE/pm.sock' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 '$REMOTE_BASE/pm2_test' $*" | tr -d '\r'
}

for _ in $(seq 1 80); do
    if remote_pm version 2>/dev/null | grep -q "$EXPECTED_VERSION"; then break; fi
    sleep 0.1
done
remote_pm version | grep -q "$EXPECTED_VERSION"

for _ in $(seq 1 50); do
    if remote_pm list --json 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); assert any(p["name"] == "sdk_probe" and p["state"] == "online" for p in d["processes"])' 2>/dev/null; then
        ONLINE=1
        break
    fi
    sleep 0.1
done
[[ ${ONLINE:-0} == 1 ]] || { echo "SDK ready did not make probe online" >&2; exit 1; }

for _ in $(seq 1 50); do
    if "${ADB[@]}" shell "grep -q 'app \`sdk_probe\` ready' '$REMOTE_BASE/pm_tiny.log' && grep -q 'recv \`sdk_probe\` tick' '$REMOTE_BASE/pm_tiny.log'"; then
        SIGNALS_RECEIVED=1
        break
    fi
    sleep 0.1
done
[[ ${SIGNALS_RECEIVED:-0} == 1 ]] || { echo "daemon did not receive SDK ready/tick" >&2; exit 1; }
remote_pm list --json | python3 -c 'import json,sys; d=json.load(sys.stdin); p=next(p for p in d["processes"] if p["name"] == "sdk_probe"); assert p["state"] == "online" and p["ready"] is True and p["heartbeat_enabled"] is True'

FINAL_STATUS=PASS
