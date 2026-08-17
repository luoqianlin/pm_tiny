#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_failure_action.XXXXXX)"
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

wait_for_check() {
    local description=$1
    local expression=$2
    for _ in $(seq 1 240); do
        if "$BIN/pm" list --json | python3 -c "$expression"; then return 0; fi
        sleep .05
    done
    echo "timed out waiting for $description" >&2
    "$BIN/pm" list --json >&2
    return 1
}

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
cat > "$TMP/prog.yaml" <<EOF
- name: heartbeat_skip
  cwd: $TMP
  executable: $BIN/pm_sdk_ready_tick_probe
  args: ["--generation-counter", "$TMP/skip.count", "--final-wait-ms", "60000"]
  daemon: false
  start_timeout: 2
  heartbeat_timeout: 1
  failure_action: skip
  pty: false
- name: heartbeat_restart
  cwd: $TMP
  executable: $BIN/pm_sdk_ready_tick_probe
  args: ["--generation-counter", "$TMP/restart.count", "--final-wait-ms", "60000"]
  daemon: false
  start_timeout: 2
  heartbeat_timeout: 1
  failure_action: restart
  restart_delay_ms: 0
  restart_max_delay_ms: 0
  pty: false
- name: startup_suppressed
  cwd: $TMP
  executable: $BIN/pm_sdk_ready_tick_probe
  args: ["--ready-delay-ms", "5000", "--final-wait-ms", "60000"]
  daemon: false
  start_timeout: 1
  failure_action: restart
  restart_delay_ms: 0
  restart_max_delay_ms: 0
  restart_window_ms: 10000
  restart_max_attempts: 1
  pty: false
- name: suppressed_child
  cwd: $TMP
  executable: /bin/sleep
  args: ["60"]
  daemon: false
  depends_on: [startup_suppressed]
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

export PM_TINY_HOME="$TMP/home"
export PM_TINY_SOCK_FILE="$TMP/pm.sock"
export PM_TINY_LOG_FILE="$TMP/pm.log"
export PM_TINY_PROG_CFG_FILE="$TMP/prog.yaml"
export PM_TINY_APP_LOG_DIR="$TMP/logs"
export PM_TINY_APP_ENVIRON_DIR="$TMP/environ"
export PM_TINY_UDS_ABSTRACT_NAMESPACE=false

"$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" >"$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 100); do [[ -S "$TMP/pm.sock" ]] && break; sleep .05; done
[[ -S "$TMP/pm.sock" ]]

wait_for_check "initial SDK readiness" '
import json,sys
i={x["name"]:x for x in json.load(sys.stdin)["processes"]}
raise SystemExit(0 if i["heartbeat_skip"]["state"]=="online" and i["heartbeat_restart"]["state"]=="online" else 1)
'
skip_identity=$("$BIN/pm" list --json | python3 -c '
import json,sys
i=next(x for x in json.load(sys.stdin)["processes"] if x["name"]=="heartbeat_skip")
print("{}:{}".format(i["pid"], i["generation"]))
')

wait_for_check "heartbeat restart generation" '
import json,sys
i=next(x for x in json.load(sys.stdin)["processes"] if x["name"]=="heartbeat_restart")
raise SystemExit(0 if i["state"]=="online" and i["generation"]>=2 else 1)
'
wait_for_check "startup restart suppression" '
import json,sys
i={x["name"]:x for x in json.load(sys.stdin)["processes"]}
ok=i["startup_suppressed"]["state"]=="failed" and i["startup_suppressed"]["restart_suppressed"] and i["suppressed_child"]["state"]=="blocked"
raise SystemExit(0 if ok else 1)
'

skip_after=$("$BIN/pm" list --json | python3 -c '
import json,sys
i=next(x for x in json.load(sys.stdin)["processes"] if x["name"]=="heartbeat_skip")
print("{}:{}".format(i["pid"], i["generation"]))
')
[[ "$skip_after" == "$skip_identity" ]]
skip_log_count=$(grep -c 'heartbeat timeout ignored because failure_action is skip' "$TMP/pm.log" || true)
[[ "$skip_log_count" -ge 1 && "$skip_log_count" -le 4 ]]

suppressed_generation=$("$BIN/pm" list --json | python3 -c '
import json,sys
print(next(x for x in json.load(sys.stdin)["processes"] if x["name"]=="startup_suppressed")["generation"])
')
"$BIN/pm" start startup_suppressed >/dev/null
wait_for_check "manual suppression reset" "
import json,sys
i=next(x for x in json.load(sys.stdin)[\"processes\"] if x[\"name\"]==\"startup_suppressed\")
raise SystemExit(0 if i[\"state\"]==\"starting\" and i[\"generation\"]>$suppressed_generation and not i[\"restart_suppressed\"] and i[\"restart_attempts_in_window\"]==0 else 1)
"

"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
