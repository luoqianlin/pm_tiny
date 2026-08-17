#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "usage: $0 <adb-serial> [android-install-dir] [sdk-probe]" >&2
    exit 2
fi

SERIAL=$1
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
EXPECTED_VERSION=$(tr -d '\r\n' < "$ROOT/VERSION")
source "$ROOT/scripts/android_production_guard.sh"
INSTALL_DIR=${2:-"$ROOT/.build_android/_install/Release"}
BIN_DIR="$INSTALL_DIR/bin"
PROBE_BIN=${3:-"$ROOT/build-android-arm64/pm_sdk_ready_tick_probe"}
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

[[ -x "$BIN_DIR/pm_tiny" && -x "$BIN_DIR/pm2" && -x "$PROBE_BIN" ]]
adb -s "$SERIAL" shell "mkdir -p '$REMOTE/home/logs' '$REMOTE/home/environ'"
adb -s "$SERIAL" push "$BIN_DIR/pm_tiny" "$REMOTE/pm_tiny" >/dev/null
adb -s "$SERIAL" push "$BIN_DIR/pm2" "$REMOTE/pm2_test" >/dev/null
adb -s "$SERIAL" push "$PROBE_BIN" "$REMOTE/sdk_probe" >/dev/null
sed "s|@WORK_DIR@|$REMOTE|g" "$ROOT/tests/data/android_dependency_graph.yaml" >"$ARTIFACT_DIR/prog.yaml"
adb -s "$SERIAL" push "$ARTIFACT_DIR/prog.yaml" "$REMOTE/prog.yaml" >/dev/null
adb -s "$SERIAL" shell "chmod 755 '$REMOTE/pm_tiny' '$REMOTE/pm2_test' '$REMOTE/sdk_probe'"

remote_pm() {
    adb -s "$SERIAL" shell \
        "PM_TINY_HOME='$REMOTE/home' PM_TINY_SOCK_FILE='$SOCKET' '$REMOTE/pm2_test' $*" | tr -d '\r'
}

adb -s "$SERIAL" shell "PM_TINY_HOME=$REMOTE/home PM_TINY_SOCK_FILE=$SOCKET PM_TINY_PROG_CFG_FILE=$REMOTE/prog.yaml nohup $REMOTE/pm_tiny >$REMOTE/daemon.log 2>&1 &"
for _ in $(seq 1 100); do
    if remote_pm version 2>/dev/null | grep -q "$EXPECTED_VERSION"; then
        break
    fi
    sleep .1
done

info=$(remote_pm info --json)
printf '%s\n' "$info" > "$ARTIFACT_DIR/info.json"
python3 -c 'import json,sys; data=json.load(sys.stdin); assert data["schema_version"] == 1; assert data["identity"]["version"] == sys.argv[2]; assert data["identity"]["platform"] == "android"; assert data["identity"]["pid"] > 0; assert data["identity"]["uptime_ms"] >= 0; assert data["runtime"]["mode"] == "foreground"; assert data["runtime"]["state"] == "running"; assert data["config"]["home_dir"]["source"] == "environment"; assert data["ipc"]["uds_address"] == {"value": sys.argv[1], "source": "environment"}; assert data["capabilities"]["pty"] is True' "$SOCKET" "$EXPECTED_VERSION" <<< "$info"

for _ in $(seq 1 100); do
    status=$(remote_pm list --json 2>/dev/null)
    stable=$(python3 -c '
import json, sys
items = {item["name"]: item for item in json.load(sys.stdin)["processes"]}
print(items["bad"]["state"] == "failed" and items["blocked_child"]["state"] == "blocked"
      and items["blocked_grandchild"]["state"] == "blocked"
      and items["root"]["state"] == "starting" and items["child"]["state"] == "waiting"
      and items["grandchild"]["state"] == "waiting"
      and items["timeout_root"]["state"] == "starting" and items["timeout_child"]["state"] == "waiting"
      and items["heartbeat_skip"]["state"] == "online"
      and items["heartbeat_restart"]["state"] == "online")
' <<< "$status")
    [[ "$stable" == True ]] && break
    sleep .1
done
[[ "$stable" == True ]]
adb -s "$SERIAL" shell "test ! -e '$REMOTE/root-ready.marker' && test ! -e '$REMOTE/timeout-ready.marker'"

timeout_identity=$(python3 -c '
import json,sys
item=next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "timeout_root")
print("{}:{}".format(item["pid"], item["generation"]))
' <<<"$status")
heartbeat_skip_identity=$(python3 -c '
import json,sys
item=next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "heartbeat_skip")
print("{}:{}".format(item["pid"], item["generation"]))
' <<<"$status")

graph=$(remote_pm graph --no-color)
[[ "$graph" == *"Dependency graph: 11 nodes, 5 edges"* ]]
[[ "$graph" == *"blocked_child [blocked; blocked_by=bad] <- bad"* ]]
[[ "$graph" == *"blocked_grandchild [blocked; blocked_by=bad] <- blocked_child"* ]]

graph_json=$(remote_pm graph root --json)
python3 -c '
import json, sys
graph = json.load(sys.stdin)
assert graph["focus"] == "root"
assert {node["name"] for node in graph["nodes"]} == {"root", "child", "grandchild"}
assert {(edge["from"], edge["to"]) for edge in graph["edges"]} == {
    ("root", "child"), ("child", "grandchild")}
' <<< "$graph_json"

dot=$(remote_pm dag --dot)
[[ "$dot" == *'"root" -> "child";'* ]]

for _ in $(seq 1 100); do
    status=$(remote_pm list --json)
    gates_ready=$(python3 -c '
import json,sys
items={item["name"]:item for item in json.load(sys.stdin)["processes"]}
print(items["grandchild"]["state"] == "online" and items["timeout_child"]["state"] == "online"
      and items["heartbeat_restart"]["state"] == "online"
      and items["heartbeat_restart"]["generation"] >= 2)
' <<<"$status")
    [[ "$gates_ready" == True ]] && break
    sleep .1
done
[[ "$gates_ready" == True ]]
python3 -c '
import json,sys
item=next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "timeout_root")
assert "{}:{}".format(item["pid"], item["generation"]) == sys.argv[1]
assert item["state"] == "online"
' "$timeout_identity" <<<"$status"
python3 -c '
import json,sys
item=next(item for item in json.load(sys.stdin)["processes"] if item["name"] == "heartbeat_skip")
assert "{}:{}".format(item["pid"], item["generation"]) == sys.argv[1]
assert item["state"] == "online"
' "$heartbeat_skip_identity" <<<"$status"
adb -s "$SERIAL" shell "test ! -e '$REMOTE/timeout-ready.marker'"

for name in grandchild child root; do
    remote_pm stop "$name" --no-list >/dev/null
done
for name in grandchild child root; do
    for _ in $(seq 1 100); do
        state=$(remote_pm list --json | python3 -c '
import json,sys
name=sys.argv[1]
print(next(item["state"] for item in json.load(sys.stdin)["processes"] if item["name"] == name))
' "$name")
        [[ "$state" == stopped ]] && break
        sleep .1
    done
    [[ "$state" == stopped ]]
done
ready_count_before=$(adb -s "$SERIAL" shell "wc -l < '$REMOTE/root-ready.marker'" | tr -d '\r ')
remote_pm start grandchild >/dev/null
for _ in $(seq 1 100); do
    status=$(remote_pm list --json)
    starting_generation=$(python3 -c '
import json,sys
items={item["name"]:item for item in json.load(sys.stdin)["processes"]}
print(items["root"]["generation"] if items["root"]["state"] == "starting" and items["grandchild"]["state"] == "waiting" else 0)
' <<<"$status")
    [[ "$starting_generation" -gt 0 ]] && break
    sleep .1
done
[[ "$starting_generation" -gt 0 ]]
remote_pm restart root --no-list >/dev/null
for _ in $(seq 1 100); do
    status=$(remote_pm list --json)
    replaced=$(python3 -c '
import json,sys
items={item["name"]:item for item in json.load(sys.stdin)["processes"]}
print(items["root"]["state"] == "starting" and items["root"]["generation"] > int(sys.argv[1])
      and items["grandchild"]["state"] == "waiting")
' "$starting_generation" <<<"$status")
    [[ "$replaced" == True ]] && break
    sleep .1
done
[[ "$replaced" == True ]]
sleep .4
[[ $(adb -s "$SERIAL" shell "wc -l < '$REMOTE/root-ready.marker'" | tr -d '\r ') -eq $ready_count_before ]]
for _ in $(seq 1 100); do
    status=$(remote_pm list --json)
    state=$(python3 -c 'import json,sys; print(next(item["state"] for item in json.load(sys.stdin)["processes"] if item["name"] == "grandchild"))' <<<"$status")
    [[ "$state" == online ]] && break
    sleep .1
done
[[ "$state" == online ]]
[[ $(adb -s "$SERIAL" shell "wc -l < '$REMOTE/root-ready.marker'" | tr -d '\r ') -eq $((ready_count_before + 1)) ]]

cat >"$ARTIFACT_DIR/failable.sh" <<'EOF'
#!/system/bin/sh
sleep 30
EOF
adb -s "$SERIAL" push "$ARTIFACT_DIR/failable.sh" "$REMOTE/failable.sh" >/dev/null
adb -s "$SERIAL" shell "chmod 755 '$REMOTE/failable.sh'"
adb -s "$SERIAL" shell \
    "PM_TINY_HOME='$REMOTE/home' PM_TINY_SOCK_FILE='$SOCKET' '$REMOTE/pm2_test' start bad" | grep -q 'started `bad`'
for _ in $(seq 1 100); do
    status=$(adb -s "$SERIAL" shell \
        "PM_TINY_HOME='$REMOTE/home' PM_TINY_SOCK_FILE='$SOCKET' '$REMOTE/pm2_test' list --json" | tr -d '\r')
    recovered=$(python3 -c '
import json,sys
items={item["name"]:item for item in json.load(sys.stdin)["processes"]}
print(all(items[name]["state"] == "online" for name in ("bad","blocked_child","blocked_grandchild")))
' <<<"$status")
    [[ "$recovered" == True ]] && break
    sleep .1
done
[[ "$recovered" == True ]]

remote_pm save >/dev/null
before_mutation=$(python3 -c '
import json,sys
data=json.load(sys.stdin)
print([(item["name"],item["state"],item["pid"],item["generation"],item["depends_on"])
       for item in data["processes"]])
' <<<"$status")
before_graph=$(remote_pm graph --json | python3 -c '
import json,sys
data=json.load(sys.stdin)
print(([(node["name"],node["state"]) for node in data["nodes"]],
       [(edge["from"],edge["to"]) for edge in data["edges"]]))
')
before_saved=$(adb -s "$SERIAL" shell "sha256sum '$REMOTE/prog.yaml'" | awk '{print $1}')
for args in \
    "missing_dynamic --depends-on absent --no-daemon --no-pty -- /system/bin/sleep 30" \
    "duplicate_dynamic --depends-on root --depends-on root --no-daemon --no-pty -- /system/bin/sleep 30" \
    "failed_dynamic --depends-on root --no-daemon --no-pty -- $REMOTE/not-found"; do
    if adb -s "$SERIAL" shell \
        "PM_TINY_HOME='$REMOTE/home' PM_TINY_SOCK_FILE='$SOCKET' '$REMOTE/pm2_test' start $args" \
        >"$ARTIFACT_DIR/rejected-mutation.out" 2>&1; then
        echo "invalid Android dependency mutation unexpectedly succeeded: $args" >&2
        exit 1
    fi
    after_mutation=$(remote_pm list --json | python3 -c '
import json,sys
data=json.load(sys.stdin)
print([(item["name"],item["state"],item["pid"],item["generation"],item["depends_on"])
       for item in data["processes"]])
')
    [[ "$after_mutation" == "$before_mutation" ]]
    [[ "$(remote_pm graph --json | python3 -c '
import json,sys
data=json.load(sys.stdin)
print(([(node["name"],node["state"]) for node in data["nodes"]],
       [(edge["from"],edge["to"]) for edge in data["edges"]]))
')" == "$before_graph" ]]
    [[ $(adb -s "$SERIAL" shell "sha256sum '$REMOTE/prog.yaml'" | awk '{print $1}') == "$before_saved" ]]
done

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
