#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_dependency_mutation.XXXXXX)"
DAEMON_PID=0

cleanup() {
    status=$?
    trap - EXIT
    if [[ $DAEMON_PID -gt 0 ]]; then kill "$DAEMON_PID" 2>/dev/null || true; fi
    if [[ $status -ne 0 ]]; then
        [[ ! -f "$TMP/daemon.out" ]] || tail -200 "$TMP/daemon.out" >&2
        [[ ! -f "$TMP/pm.log" ]] || tail -200 "$TMP/pm.log" >&2
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
: >"$TMP/prog.yaml"
cat >"$TMP/pm_tiny.yaml" <<EOF
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

"$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" >"$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 100); do [[ -S "$TMP/pm.sock" ]] && break; sleep .05; done
[[ -S "$TMP/pm.sock" ]]

wait_for_state() {
    local name=$1 expected=$2 state=
    for _ in $(seq 1 100); do
        state=$("$BIN/pm" list --json | python3 -c '
import json, sys
name=sys.argv[1]
items=[p for p in json.load(sys.stdin)["processes"] if p["name"] == name]
print(items[0]["state"] if items else "missing")
' "$name")
        [[ "$state" == "$expected" ]] && return 0
        sleep .05
    done
    echo "timed out waiting for $name=$expected; last=$state" >&2
    return 1
}

"$BIN/pm" start base --no-daemon --no-pty -- /bin/sleep 60 | grep -q 'started `base`'
wait_for_state base online
"$BIN/pm" start child --depends-on base --no-daemon --no-pty -- /bin/sleep 60 | grep -q 'started `child`'
wait_for_state child online
"$BIN/pm" save >/dev/null

snapshot() {
    local list graph saved
    list=$("$BIN/pm" list --json | python3 -c '
import json, sys
data=json.load(sys.stdin)
print(json.dumps([(p["name"], p["state"], p["generation"], p["depends_on"])
                  for p in data["processes"]], separators=(",", ":")))
')
    graph=$("$BIN/pm" graph --json | python3 -c '
import json, sys
data=json.load(sys.stdin)
print(json.dumps({"nodes":[n["name"] for n in data["nodes"]],
                  "edges":[(e["from"],e["to"]) for e in data["edges"]]},
                 sort_keys=True,separators=(",", ":")))
')
    saved=$(sha256sum "$TMP/prog.yaml" | cut -d' ' -f1)
    printf '%s|%s|%s\n' "$list" "$graph" "$saved"
}

baseline=$(snapshot)
expect_rejected_without_residue() {
    local label=$1
    shift
    if "$BIN/pm" "$@" >"$TMP/$label.out" 2>"$TMP/$label.err"; then
        echo "$label unexpectedly succeeded" >&2
        return 1
    fi
    [[ ! -s "$TMP/$label.out" ]]
    [[ "$(snapshot)" == "$baseline" ]]
}

expect_rejected_without_residue missing start missing_child --depends-on absent --no-daemon --no-pty -- /bin/sleep 60
grep -q 'depends on missing' "$TMP/missing.err"
expect_rejected_without_residue duplicate start duplicate_child --depends-on base --depends-on base --no-daemon --no-pty -- /bin/sleep 60
grep -q 'duplicate dependency' "$TMP/duplicate.err"
expect_rejected_without_residue spawn_failure start spawn_failure --depends-on base --no-daemon --no-pty -- /not/found/pm_tiny_dependency

"$BIN/pm" list --json | python3 -c '
import json, sys
items={p["name"]:p for p in json.load(sys.stdin)["processes"]}
assert set(items) == {"base", "child"}
assert items["child"]["depends_on"] == ["base"]
assert items["base"]["state"] == "online" and items["child"]["state"] == "online"
'
"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
