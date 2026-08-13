#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <adb-serial> [android-install-dir]" >&2
    exit 2
fi

SERIAL=$1
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/scripts/android_production_guard.sh"
INSTALL_DIR=${2:-"$ROOT/.build_android/_install/Release"}
BIN_DIR="$INSTALL_DIR/bin"
STAMP=$(date +%Y%m%d-%H%M%S)
REMOTE="/data/local/tmp/pm_tiny_graph_${STAMP}_$$"
SOCKET="pm_tiny_graph_${STAMP}_$$"
PRODUCTION_PID=$(adb -s "$SERIAL" shell pidof pm_tiny | tr -d '\r')
ADB=(adb -s "$SERIAL")
[[ "$PRODUCTION_PID" =~ ^[0-9]+$ ]]
ARTIFACT_DIR="$ROOT/build/test-artifacts/android/${STAMP}-${SERIAL}-graph"
mkdir -p "$ARTIFACT_DIR"
capture_production_state "$ARTIFACT_DIR/production-before.json"

cleanup() {
    adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test quit >/dev/null 2>&1 || true" || true
    adb -s "$SERIAL" shell "rm -rf '$REMOTE'" || true
}
trap cleanup EXIT

[[ -x "$BIN_DIR/pm_tiny" && -x "$BIN_DIR/pm2" ]]
adb -s "$SERIAL" shell "mkdir -p '$REMOTE/home/logs' '$REMOTE/home/environ'"
adb -s "$SERIAL" push "$BIN_DIR/pm_tiny" "$REMOTE/pm_tiny" >/dev/null
adb -s "$SERIAL" push "$BIN_DIR/pm2" "$REMOTE/pm2_test" >/dev/null
adb -s "$SERIAL" push "$ROOT/test/android_dependency_graph.yaml" "$REMOTE/prog.yaml" >/dev/null
adb -s "$SERIAL" shell "chmod 755 '$REMOTE/pm_tiny' '$REMOTE/pm2_test'"

adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET PM_TINY_PROG_CFG_FILE=$REMOTE/prog.yaml nohup $REMOTE/pm_tiny >$REMOTE/daemon.log 2>&1 &"
for _ in $(seq 1 100); do
    if adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test version" 2>/dev/null | grep -q "1.1.4"; then
        break
    fi
    sleep .1
done

for _ in $(seq 1 100); do
    status=$(adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test list --json" 2>/dev/null | tr -d '\r')
    stable=$(python3 -c '
import json, sys
items = {item["name"]: item for item in json.load(sys.stdin)["processes"]}
print(items["bad"]["state"] == "failed" and items["blocked_child"]["state"] == "blocked")
' <<< "$status")
    [[ "$stable" == True ]] && break
    sleep .1
done
[[ "$stable" == True ]]

graph=$(adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test graph --no-color" | tr -d '\r')
[[ "$graph" == *"Dependency graph: 5 nodes, 2 edges"* ]]
[[ "$graph" == *"blocked_child [blocked; blocked_by=bad] <- bad"* ]]

graph_json=$(adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test graph root --json" | tr -d '\r')
python3 -c '
import json, sys
graph = json.load(sys.stdin)
assert graph["focus"] == "root"
assert {node["name"] for node in graph["nodes"]} == {"root", "child"}
assert {(edge["from"], edge["to"]) for edge in graph["edges"]} == {("root", "child")}
' <<< "$graph_json"

dot=$(adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test dag --dot" | tr -d '\r')
[[ "$dot" == *'"root" -> "child";'* ]]

current_pid=$(adb -s "$SERIAL" shell pidof pm_tiny | tr -d '\r')
[[ "$current_pid" == *"$PRODUCTION_PID"* ]]
capture_production_state "$ARTIFACT_DIR/production-after.json"
assert_production_unchanged "$ARTIFACT_DIR/production-before.json" "$ARTIFACT_DIR/production-after.json"
echo "android dependency graph: PASS"
echo "artifacts: $ARTIFACT_DIR"
