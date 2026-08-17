#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <adb-serial> [android-install-dir]" >&2
    exit 2
fi

SERIAL=$1
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
EXPECTED_VERSION=$(tr -d '\r\n' < "$ROOT/VERSION")
INSTALL_DIR=${2:-"$ROOT/build-android-arm64/_install/Release"}
BIN_DIR="$INSTALL_DIR/bin"
STAMP=$(date +%Y%m%d-%H%M%S)
REMOTE="/data/local/tmp/pm_tiny_empty_config_${STAMP}_$$"
HOME_DIR="$REMOTE/home"
SOCKET="$REMOTE/pm.sock"
PROGRAM_CONFIG="$REMOTE/prog.yaml"
ARTIFACT_DIR="$ROOT/build/test-artifacts/android/${STAMP}-${SERIAL}-empty-config"
ADB=(adb -s "$SERIAL")
FINAL_STATUS=FAIL

find_production_pid() {
    "${ADB[@]}" shell '
        for pid in $(pidof pm_tiny 2>/dev/null); do
            if [ "$(readlink /proc/$pid/exe 2>/dev/null)" = /vendor/bin/pm_tiny ]; then
                echo "$pid"
            fi
        done
    ' | tr -d '\r'
}

capture_production_processes() {
    local output=$1 pid=$2
    "${ADB[@]}" shell "ps -A -o PID,PPID,NAME,ARGS | awk 'NR == 1 || \$1 == $pid || \$2 == $pid'" |
        tr -d '\r' > "$output"
}

remote_pm() {
    "${ADB[@]}" shell \
        "env PM_TINY_HOME='$HOME_DIR' PM_TINY_SOCK_FILE='$SOCKET' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 '$REMOTE/pm2_test' $*"
}

start_test_daemon() {
    "${ADB[@]}" shell \
        "env PM_TINY_HOME='$HOME_DIR' PM_TINY_SOCK_FILE='$SOCKET' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 PM_TINY_PROG_CFG_FILE='$PROGRAM_CONFIG' nohup '$REMOTE/pm_tiny_test' >'$REMOTE/daemon.log' 2>&1 &"
    for _ in $(seq 1 100); do
        if remote_pm version 2>/dev/null | grep -q "$EXPECTED_VERSION"; then return 0; fi
        sleep .1
    done
    return 1
}

cleanup() {
    local exit_code=$?
    set +e
    remote_pm quit >/dev/null 2>&1 || true
    local production_after
    production_after=$(find_production_pid)
    capture_production_processes "$ARTIFACT_DIR/production-after.txt" "$PRODUCTION_PID"
    if [[ "$production_after" != "$PRODUCTION_PID" ]] ||
       ! cmp -s "$ARTIFACT_DIR/production-before.txt" "$ARTIFACT_DIR/production-after.txt"; then
        echo "production pm_tiny or its managed processes changed during isolated test" >&2
        exit_code=1
    fi
    "${ADB[@]}" shell "rm -rf '$REMOTE'" >/dev/null 2>&1 || true
    printf 'status=%s\nexit_code=%d\n' "$FINAL_STATUS" "$exit_code" > "$ARTIFACT_DIR/result.txt"
    exit "$exit_code"
}
trap cleanup EXIT

[[ -x "$BIN_DIR/pm_tiny" && -x "$BIN_DIR/pm2" ]]
mkdir -p "$ARTIFACT_DIR"
PRODUCTION_PID=$(find_production_pid)
[[ "$PRODUCTION_PID" =~ ^[0-9]+$ ]]
capture_production_processes "$ARTIFACT_DIR/production-before.txt" "$PRODUCTION_PID"

"${ADB[@]}" shell "mkdir -p '$HOME_DIR/logs' '$HOME_DIR/environ'"
"${ADB[@]}" push "$BIN_DIR/pm_tiny" "$REMOTE/pm_tiny_test" >/dev/null
"${ADB[@]}" push "$BIN_DIR/pm2" "$REMOTE/pm2_test" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE/pm_tiny_test' '$REMOTE/pm2_test'"

start_test_daemon
"${ADB[@]}" shell "test ! -e '$PROGRAM_CONFIG'"
remote_pm list --json | tr -d '\r' > "$ARTIFACT_DIR/missing-list.json"
python3 - "$ARTIFACT_DIR/missing-list.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
assert data["total"] == 0 and data["processes"] == []
PY
remote_pm info --json | tr -d '\r' > "$ARTIFACT_DIR/missing-info.json"
python3 - "$ARTIFACT_DIR/missing-info.json" "$SOCKET" "$EXPECTED_VERSION" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
assert data["identity"]["version"] == sys.argv[3]
assert data["identity"]["platform"] == "android"
assert data["runtime"]["file_config_count"] == 0
assert data["runtime"]["runtime_definition_count"] == 0
assert data["ipc"]["uds_address"] == {"value": sys.argv[2], "source": "environment"}
PY

remote_pm start empty_probe --no-daemon --no-pty -- /system/bin/sleep 60 >/dev/null
CHILD_PID=""
for _ in $(seq 1 100); do
    CHILD_PID=$(remote_pm list --json | tr -d '\r' |
        python3 -c 'import json,sys; pid=json.load(sys.stdin)["processes"][0]["pid"]; print(pid if isinstance(pid, int) else "")')
    if [[ "$CHILD_PID" =~ ^[0-9]+$ ]]; then break; fi
    sleep .05
done
[[ "$CHILD_PID" =~ ^[0-9]+$ ]]
"${ADB[@]}" shell "test -d /proc/$CHILD_PID"
remote_pm save >/dev/null
"${ADB[@]}" shell "grep -q 'name: empty_probe' '$PROGRAM_CONFIG'"

"${ADB[@]}" shell "printf '  # intentionally empty for reload\n' > '$PROGRAM_CONFIG'"
remote_pm reload >/dev/null
remote_pm list --json | tr -d '\r' > "$ARTIFACT_DIR/reload-list.json"
python3 - "$ARTIFACT_DIR/reload-list.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
assert data["total"] == 0 and data["processes"] == []
PY
for _ in $(seq 1 100); do
    if ! "${ADB[@]}" shell "test -d /proc/$CHILD_PID" 2>/dev/null; then break; fi
    sleep .05
done
if "${ADB[@]}" shell "test -d /proc/$CHILD_PID" 2>/dev/null; then
    echo "reload did not terminate empty_probe" >&2
    exit 1
fi
remote_pm quit >/dev/null

"${ADB[@]}" shell ": > '$PROGRAM_CONFIG'"
start_test_daemon
remote_pm list --json | tr -d '\r' > "$ARTIFACT_DIR/zero-byte-list.json"
python3 - "$ARTIFACT_DIR/zero-byte-list.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
assert data["total"] == 0 and data["processes"] == []
PY
remote_pm quit >/dev/null

FINAL_STATUS=PASS
echo "android empty program config: PASS"
echo "artifacts: $ARTIFACT_DIR"
