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
  executable: /bin/sleep
  args: ["60"]
  start_timeout: 1
  daemon: false
  pty: false
- name: child
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  depends_on: [root]
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
print(items["bad"]["state"] == "failed" and items["blocked_child"]["state"] == "blocked" and items["side"]["state"] == "online")
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
assert items["side"]["state"] == "online"
' <<< "$status"

graph="$($BIN/pm graph --no-color)"
[[ "$graph" == *'Dependency graph: 5 nodes, 2 edges'* ]]
[[ "$graph" == *'blocked_child [blocked; blocked_by=bad] <- bad'* ]]

graph_json="$($BIN/pm graph root --json)"
python3 -c '
import json, sys
graph = json.load(sys.stdin)
assert graph["focus"] == "root"
assert {node["name"] for node in graph["nodes"]} == {"root", "child"}
assert {tuple((edge["from"], edge["to"])) for edge in graph["edges"]} == {("root", "child")}
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
"$BIN/pm" delete root --no-list >/dev/null
"$BIN/pm" save >/dev/null
! grep -q 'name: root' "$TMP/prog.yaml"
! grep -q 'name: child' "$TMP/prog.yaml"
"$BIN/pm" reload >/dev/null
"$BIN/pm" list --json | python3 -c '
import json, sys
names = {item["name"] for item in json.load(sys.stdin)["processes"]}
assert "root" not in names and "child" not in names
'

"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
