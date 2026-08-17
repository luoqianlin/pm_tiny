#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <adb-serial> [android-install-dir]" >&2
    exit 2
fi

SERIAL=$1
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
EXPECTED_VERSION=$(tr -d '\r\n' < "$ROOT/VERSION")
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
FINAL_STATUS=FAIL

cleanup() {
    local exit_code=$?
    set +e
    adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test quit >/dev/null 2>&1 || true" || true
    capture_production_state "$ARTIFACT_DIR/production-after.json"
    assert_production_unchanged "$ARTIFACT_DIR/production-before.json" \
        "$ARTIFACT_DIR/production-after.json" || exit_code=1
    adb -s "$SERIAL" shell "rm -rf '$REMOTE'" || true
    printf 'status=%s\nexit_code=%d\n' "$FINAL_STATUS" "$exit_code" > "$ARTIFACT_DIR/result.txt"
    exit "$exit_code"
}
trap cleanup EXIT

[[ -x "$BIN_DIR/pm_tiny" && -x "$BIN_DIR/pm2" ]]
adb -s "$SERIAL" shell "mkdir -p '$REMOTE/home/logs' '$REMOTE/home/environ'"
adb -s "$SERIAL" push "$BIN_DIR/pm_tiny" "$REMOTE/pm_tiny" >/dev/null
adb -s "$SERIAL" push "$BIN_DIR/pm2" "$REMOTE/pm2_test" >/dev/null
adb -s "$SERIAL" push "$ROOT/tests/data/android_dependency_graph.yaml" "$REMOTE/prog.yaml" >/dev/null
adb -s "$SERIAL" shell "chmod 755 '$REMOTE/pm_tiny' '$REMOTE/pm2_test'"

adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET PM_TINY_PROG_CFG_FILE=$REMOTE/prog.yaml nohup $REMOTE/pm_tiny >$REMOTE/daemon.log 2>&1 &"
for _ in $(seq 1 100); do
    if adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test version" 2>/dev/null | grep -q "$EXPECTED_VERSION"; then
        break
    fi
    sleep .1
done

info=$(adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET $REMOTE/pm2_test info --json" | tr -d '\r')
printf '%s\n' "$info" > "$ARTIFACT_DIR/info.json"
python3 -c 'import json,sys; data=json.load(sys.stdin); assert data["schema_version"] == 1; assert data["identity"]["version"] == sys.argv[2]; assert data["identity"]["platform"] == "android"; assert data["identity"]["pid"] > 0; assert data["identity"]["uptime_ms"] >= 0; assert data["runtime"]["mode"] == "foreground"; assert data["runtime"]["state"] == "running"; assert data["config"]["home_dir"]["source"] == "environment"; assert data["ipc"]["uds_address"] == {"value": sys.argv[1], "source": "environment"}; assert data["capabilities"]["pty"] is True' "$SOCKET" "$EXPECTED_VERSION" <<< "$info"

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

cat > "$ARTIFACT_DIR/argv_fixture.sh" <<EOF
#!/system/bin/sh
{
    printf 'inherited=%s\n' "\$INHERITED_MARKER"
    printf 'explicit=%s\n' "\$EXPLICIT_MARKER"
    printf 'argc=%s\n' "\$#"
    index=0
    for arg in "\$@"; do
        printf 'arg%s=<%s>\n' "\$index" "\$arg"
        index=\$((index + 1))
    done
} > '$REMOTE/argv-result.tmp'
mv '$REMOTE/argv-result.tmp' '$REMOTE/argv-result.txt'
EOF
adb -s "$SERIAL" push "$ARTIFACT_DIR/argv_fixture.sh" "$REMOTE/argv_fixture.sh" >/dev/null
adb -s "$SERIAL" shell "chmod 755 '$REMOTE/argv_fixture.sh'"
adb -s "$SERIAL" shell \
    "env PM_TINY_HOME='$REMOTE/home' PM_TINY_SOCK_FILE='$SOCKET' INHERITED_MARKER='inherited value' \
    '$REMOTE/pm2_test' start argv_env --no-daemon --env EXPLICIT_MARKER=explicit -- \
    '$REMOTE/argv_fixture.sh' 'value with space' '' --dash" | grep -q 'started `argv_env`'
for _ in $(seq 1 50); do
    adb -s "$SERIAL" shell "test -f '$REMOTE/argv-result.txt'" >/dev/null 2>&1 && break
    sleep .1
done
adb -s "$SERIAL" pull "$REMOTE/argv-result.txt" "$ARTIFACT_DIR/argv-result.txt" >/dev/null
grep -qx 'inherited=inherited value' "$ARTIFACT_DIR/argv-result.txt"
grep -qx 'explicit=explicit' "$ARTIFACT_DIR/argv-result.txt"
grep -qx 'argc=3' "$ARTIFACT_DIR/argv-result.txt"
grep -qx 'arg0=<value with space>' "$ARTIFACT_DIR/argv-result.txt"
grep -qx 'arg1=<>' "$ARTIFACT_DIR/argv-result.txt"
grep -qx 'arg2=<--dash>' "$ARTIFACT_DIR/argv-result.txt"
adb -s "$SERIAL" shell \
    "PM_TINY_HOME='$REMOTE/home' PM_TINY_SOCK_FILE='$SOCKET' '$REMOTE/pm2_test' save" >/dev/null
adb -s "$SERIAL" pull "$REMOTE/prog.yaml" "$ARTIFACT_DIR/saved-prog.yaml" >/dev/null
adb -s "$SERIAL" pull "$REMOTE/home/environ/argv_env.yaml" \
    "$ARTIFACT_DIR/argv_env-environ.yaml" >/dev/null
grep -q 'name: argv_env' "$ARTIFACT_DIR/saved-prog.yaml"
grep -q 'value with space' "$ARTIFACT_DIR/saved-prog.yaml"
grep -q 'EXPLICIT_MARKER=explicit' "$ARTIFACT_DIR/saved-prog.yaml"
grep -q 'INHERITED_MARKER=inherited value' "$ARTIFACT_DIR/argv_env-environ.yaml"

current_pid=$(adb -s "$SERIAL" shell pidof pm_tiny | tr -d '\r')
[[ "$current_pid" == *"$PRODUCTION_PID"* ]]
FINAL_STATUS=PASS
echo "android dependency graph: PASS"
echo "artifacts: $ARTIFACT_DIR"
