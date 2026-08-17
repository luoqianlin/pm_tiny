#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_dependency.XXXXXX)"
DAEMON_PID=0

cleanup() {
    status=$?
    trap - EXIT
    if [[ "$DAEMON_PID" -gt 0 ]]; then kill "$DAEMON_PID" 2>/dev/null || true; fi
    if [[ $status -ne 0 ]]; then
        [[ ! -f "$TMP/daemon.out" ]] || tail -200 "$TMP/daemon.out" >&2
        [[ ! -f "$TMP/pm.log" ]] || tail -200 "$TMP/pm.log" >&2
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

wait_for_state() {
    local name=$1
    local expected=$2
    local state=
    for _ in $(seq 1 100); do
        state=$("$BIN/pm" list --json | python3 -c '
import json, sys
name = sys.argv[1]
items = [item for item in json.load(sys.stdin)["processes"] if item["name"] == name]
print(items[0]["state"] if items else "missing")
' "$name")
        [[ "$state" == "$expected" ]] && return 0
        sleep .05
    done
    echo "timed out waiting for $name state $expected; last state: $state" >&2
    return 1
}

dependency_runtime_snapshot() {
    "$BIN/pm" list --json | python3 -c '
import json, sys
items = {item["name"]: item for item in json.load(sys.stdin)["processes"]}
snapshot = {
    name: {
        "state": items[name]["state"],
        "pid": items[name]["pid"],
        "generation": items[name]["generation"],
        "depends_on": items[name]["depends_on"],
    }
    for name in ("root", "child")
}
print(json.dumps(snapshot, sort_keys=True, separators=(",", ":")))
'
}

dependency_graph_snapshot() {
    "$BIN/pm" graph root --json | python3 -c '
import json, sys
graph = json.load(sys.stdin)
snapshot = {
    "focus": graph["focus"],
    "nodes": sorted(node["name"] for node in graph["nodes"]),
    "edges": sorted((edge["from"], edge["to"]) for edge in graph["edges"]),
}
print(json.dumps(snapshot, sort_keys=True, separators=(",", ":")))
'
}

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
cat > "$TMP/prog.yaml" <<EOF
- name: root
  cwd: $TMP
  executable: $BIN/pm_sdk_ready_tick_probe
  args: ["--ready-delay-ms", "1500", "--final-wait-ms", "60000", "--marker", "$TMP/root-ready.marker"]
  start_timeout: 3
  daemon: false
  pty: false
- name: child
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [root]
  daemon: false
  pty: false
- name: right
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [root]
  daemon: false
  pty: false
- name: left
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [root]
  daemon: false
  pty: false
- name: leaf
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [left, right]
  daemon: false
  pty: false
- name: restart_root
  cwd: $TMP
  executable: $BIN/pm_sdk_ready_tick_probe
  args: ["--ready-delay-ms", "100", "--first-ready-delay-ms", "5000", "--generation-counter", "$TMP/restart-generation.count", "--final-wait-ms", "60000", "--marker", "$TMP/restart-ready.marker"]
  start_timeout: 1
  failure_action: restart
  daemon: false
  pty: false
- name: restart_child
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [restart_root]
  daemon: false
  pty: false
- name: bad
  cwd: $TMP
  executable: /not/found/pm_tiny_dependency
  daemon: false
  pty: false
- name: blocked_child
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [bad]
  daemon: false
  pty: false
- name: blocked_grandchild
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [blocked_child]
  daemon: false
  pty: false
- name: side
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  daemon: false
  pty: false
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
    status="$($BIN/pm list --json)"
    stable=$(python3 -c '
import json, sys
items = {item["name"]: item for item in json.load(sys.stdin)["processes"]}
print(items["bad"]["state"] == "failed" and items["blocked_child"]["state"] == "blocked"
      and items["blocked_grandchild"]["state"] == "blocked" and items["side"]["state"] == "online")
' <<< "$status")
    [[ "$stable" == True ]] && break
    sleep .05
done
[[ "$stable" == True ]]
python3 -c '
import json, sys
items = {item["name"]: item for item in json.load(sys.stdin)["processes"]}
assert items["root"]["state"] == "starting"
assert items["child"]["state"] == "waiting"
assert items["bad"]["state"] == "failed"
assert items["blocked_child"]["state"] == "blocked"
assert items["blocked_grandchild"]["state"] == "blocked"
assert items["side"]["state"] == "online"
' <<< "$status"
[[ ! -e "$TMP/root-ready.marker" ]]

graph="$($BIN/pm graph --no-color)"
[[ "$graph" == *'Dependency graph: 11 nodes, 8 edges'* ]]
[[ "$graph" == *'blocked_child [blocked; blocked_by=bad] <- bad'* ]]
[[ "$graph" == *'blocked_grandchild [blocked; blocked_by=bad] <- blocked_child'* ]]

graph_json="$($BIN/pm graph root --json)"
python3 -c '
import json, sys
graph = json.load(sys.stdin)
assert graph["focus"] == "root"
assert {node["name"] for node in graph["nodes"]} == {"root", "child", "right", "left", "leaf"}
assert {tuple((edge["from"], edge["to"])) for edge in graph["edges"]} == {
    ("root", "child"), ("root", "right"), ("root", "left"),
    ("right", "leaf"), ("left", "leaf")}
' <<< "$graph_json"

graph_dot="$($BIN/pm dag --dot)"
[[ "$graph_dot" == *'digraph pm_tiny {'* ]]
[[ "$graph_dot" == *'"root" -> "child";'* ]]

if "$BIN/pm" graph missing >"$TMP/missing-graph.out" 2>&1; then
    echo "missing graph focus unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq 'process not found: missing' "$TMP/missing-graph.out"

wait_for_state child online
wait_for_state leaf online
wait_for_state restart_child online
"$BIN/pm" list --json | python3 -c '
import json,sys
items={item["name"]:item for item in json.load(sys.stdin)["processes"]}
assert all(items[name]["state"] == "online" for name in ("right","left","leaf"))
assert all(items[name]["generation"] == 1 for name in ("right","left","leaf"))
assert items["restart_root"]["state"] == "online" and items["restart_root"]["generation"] >= 2
assert items["restart_child"]["state"] == "online" and items["restart_child"]["generation"] == 1
'
[[ $(wc -l <"$TMP/restart-ready.marker") -eq 1 ]]
[[ $(<"$TMP/restart-generation.count") -ge 2 ]]

# Start a dependency closure, replace its root while still starting, and prove
# that the killed generation cannot unlock the new closure with a late ready.
for name in leaf left right child root; do
    "$BIN/pm" stop "$name" --no-list >/dev/null
    wait_for_state "$name" stopped
done
ready_count_before=$(wc -l <"$TMP/root-ready.marker")
"$BIN/pm" start leaf >/dev/null
wait_for_state root starting
starting_generation=$("$BIN/pm" list --json | python3 -c '
import json,sys
items={item["name"]:item for item in json.load(sys.stdin)["processes"]}
assert items["leaf"]["state"] == "waiting"
print(items["root"]["generation"])
')
"$BIN/pm" restart root --no-list >/dev/null
for _ in $(seq 1 100); do
    status=$("$BIN/pm" list --json)
    if python3 -c '
import json,sys
items={item["name"]:item for item in json.load(sys.stdin)["processes"]}
assert items["root"]["state"] == "starting" and items["root"]["generation"] > int(sys.argv[1])
assert items["leaf"]["state"] == "waiting"
' "$starting_generation" <<<"$status" 2>/dev/null; then break; fi
    sleep .05
done
sleep .4
[[ $(wc -l <"$TMP/root-ready.marker") -eq $ready_count_before ]]
wait_for_state leaf online
[[ $(wc -l <"$TMP/root-ready.marker") -eq $((ready_count_before + 1)) ]]

shutdown_log_start=$(wc -l <"$TMP/pm.log")
"$BIN/pm" reload --no-list >/dev/null
wait_for_state child online
shutdown_log=$(tail -n +"$((shutdown_log_start + 1))" "$TMP/pm.log")
child_shutdown_line=$(grep -n 'dependency shutdown request `child`' <<<"$shutdown_log" | head -n1 | cut -d: -f1)
root_shutdown_line=$(grep -n 'dependency shutdown request `root`' <<<"$shutdown_log" | head -n1 | cut -d: -f1)
[[ -n "$child_shutdown_line" && -n "$root_shutdown_line" && $child_shutdown_line -lt $root_shutdown_line ]]
"$BIN/pm" save >/dev/null
cp "$TMP/prog.yaml" "$TMP/prog.before-rejected-delete.yaml"

cp "$TMP/prog.yaml" "$TMP/prog.before-invalid-reload.yaml"
runtime_before_invalid_reload=$(dependency_runtime_snapshot)
graph_before_invalid_reload=$(dependency_graph_snapshot)
cat > "$TMP/prog.yaml" <<EOF
- name: root
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  daemon: false
  pty: false
- name: child
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [root]
  daemon: false
  pty: false
- name: invalid
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [missing]
  daemon: false
  pty: false
EOF
set +e
"$BIN/pm" reload --no-list >"$TMP/invalid-reload.stdout" 2>"$TMP/invalid-reload.stderr"
invalid_reload_status=$?
set -e
[[ $invalid_reload_status -eq 1 ]]
grep -Eq 'missing|invalid configuration|cannot load program configuration' "$TMP/invalid-reload.stderr"
[[ "$(dependency_runtime_snapshot)" == "$runtime_before_invalid_reload" ]]
[[ "$(dependency_graph_snapshot)" == "$graph_before_invalid_reload" ]]
cp "$TMP/prog.before-invalid-reload.yaml" "$TMP/prog.yaml"
cmp "$TMP/prog.before-invalid-reload.yaml" "$TMP/prog.yaml"

runtime_before=$(dependency_runtime_snapshot)
graph_before=$(dependency_graph_snapshot)

set +e
"$BIN/pm" delete root --no-list >"$TMP/delete-root.stdout" 2>"$TMP/delete-root.stderr"
delete_status=$?
set -e
[[ $delete_status -eq 1 ]]
[[ ! -s "$TMP/delete-root.stdout" ]]
grep -q 'required by: child' "$TMP/delete-root.stderr"
[[ "$(dependency_runtime_snapshot)" == "$runtime_before" ]]
[[ "$(dependency_graph_snapshot)" == "$graph_before" ]]
cmp "$TMP/prog.before-rejected-delete.yaml" "$TMP/prog.yaml"

"$BIN/pm" stop child --no-list >/dev/null
wait_for_state child stopped
"$BIN/pm" start child | grep -q started
wait_for_state child online

"$BIN/pm" stop root --no-list >/dev/null
wait_for_state root stopped
set +e
"$BIN/pm" delete root --no-list >"$TMP/delete-stopped-root.stdout" 2>"$TMP/delete-stopped-root.stderr"
stopped_delete_status=$?
set -e
[[ $stopped_delete_status -eq 1 ]]
[[ ! -s "$TMP/delete-stopped-root.stdout" ]]
grep -q 'required by: child' "$TMP/delete-stopped-root.stderr"
cmp "$TMP/prog.before-rejected-delete.yaml" "$TMP/prog.yaml"

"$BIN/pm" delete child --no-list >/dev/null
"$BIN/pm" delete leaf --no-list >/dev/null
"$BIN/pm" delete left --no-list >/dev/null
"$BIN/pm" delete right --no-list >/dev/null
"$BIN/pm" delete root --no-list >/dev/null
"$BIN/pm" save >/dev/null
! grep -q 'name: root' "$TMP/prog.yaml"
! grep -q 'name: child' "$TMP/prog.yaml"
! grep -q 'name: leaf' "$TMP/prog.yaml"
"$BIN/pm" reload >/dev/null
"$BIN/pm" list --json | python3 -c '
import json, sys
names = {item["name"] for item in json.load(sys.stdin)["processes"]}
assert not ({"root", "child", "right", "left", "leaf"} & names)
'

"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
