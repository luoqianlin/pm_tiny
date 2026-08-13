#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "usage: $0 [build-dir] [process-count] [output-json]" >&2
    exit 2
}

[[ $# -le 3 ]] || usage
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN_DIR=${1:-"$ROOT/build-release"}
PROCESS_COUNT=${2:-20}
OUTPUT=${3:-"$ROOT/build/benchmarks/linux-baseline.json"}
[[ "$PROCESS_COUNT" =~ ^[1-9][0-9]*$ ]] || usage
[[ -x "$BIN_DIR/pm_tiny" && -x "$BIN_DIR/pm" ]] || {
    echo "missing pm_tiny or pm in $BIN_DIR" >&2
    exit 1
}

TMP=$(mktemp -d /tmp/pm_tiny_benchmark.XXXXXX)
DAEMON_PID=0
cleanup() {
    local status=$?
    trap - EXIT
    set +e
    if [[ "$DAEMON_PID" -gt 0 ]]; then
        "$BIN_DIR/pm" quit >/dev/null 2>&1
        kill "$DAEMON_PID" >/dev/null 2>&1
        wait "$DAEMON_PID" >/dev/null 2>&1
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ" "$(dirname "$OUTPUT")"
printf '%s\n' '[]' > "$TMP/prog.yaml"
cat > "$TMP/pm_tiny.yaml" <<EOF
pm_tiny_home_dir: $TMP/home
pm_tiny_sock_file: $TMP/pm.sock
pm_tiny_log_file: $TMP/pm.log
pm_tiny_prog_cfg_file: $TMP/prog.yaml
pm_tiny_app_log_dir: $TMP/logs
pm_tiny_app_environ_dir: $TMP/environ
pm_tiny_uds_abstract_namespace: false
pm_tiny_process_tree_mode: process_group
EOF
cat > "$TMP/restart_probe.sh" <<EOF
#!/usr/bin/env bash
date +%s%N >> "$TMP/restart-timestamps.txt"
exit 1
EOF
chmod +x "$TMP/restart_probe.sh"

export PM_TINY_HOME="$TMP/home"
export PM_TINY_SOCK_FILE="$TMP/pm.sock"
export PM_TINY_PROG_CFG_FILE="$TMP/prog.yaml"
export PM_TINY_LOG_FILE="$TMP/pm.log"
export PM_TINY_APP_LOG_DIR="$TMP/logs"
export PM_TINY_APP_ENVIRON_DIR="$TMP/environ"

"$BIN_DIR/pm_tiny" -c "$TMP/pm_tiny.yaml" > "$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 100); do
    [[ -S "$TMP/pm.sock" ]] && break
    sleep .05
done
[[ -S "$TMP/pm.sock" ]]

idle_rss_kib=$(awk '/VmRSS:/ {print $2}' "/proc/$DAEMON_PID/status")

latency_file="$TMP/list-latency-ns.txt"
for _ in $(seq 1 30); do
    started=$(date +%s%N)
    "$BIN_DIR/pm" list --json >/dev/null
    finished=$(date +%s%N)
    echo $((finished - started)) >> "$latency_file"
done

batch_started=$(date +%s%N)
for index in $(seq 1 "$PROCESS_COUNT"); do
    "$BIN_DIR/pm" start "/usr/bin/sleep 120" --name "bench_$index" --no_daemon --no_pty >/dev/null
done
for _ in $(seq 1 200); do
    online=$("$BIN_DIR/pm" list --json | python3 -c '
import json, sys
print(sum(item["state"] == "online" for item in json.load(sys.stdin)["processes"]))
')
    [[ "$online" -eq "$PROCESS_COUNT" ]] && break
    sleep .02
done
[[ "$online" -eq "$PROCESS_COUNT" ]]
batch_finished=$(date +%s%N)
loaded_rss_kib=$(awk '/VmRSS:/ {print $2}' "/proc/$DAEMON_PID/status")

for index in $(seq 1 "$PROCESS_COUNT"); do
    "$BIN_DIR/pm" delete "bench_$index" >/dev/null
done

cat > "$TMP/prog.yaml" <<EOF
- name: restart_probe
  cwd: $TMP
  command: ./restart_probe.sh
  daemon: true
  pty: false
  start_timeout: 0
  failure_action: restart
  restart_delay_ms: 1000
  restart_max_delay_ms: 1000
  restart_window_ms: 10000
  restart_max_attempts: 2
  restart_reset_after_ms: 10000
EOF
"$BIN_DIR/pm" reload >/dev/null
for _ in $(seq 1 200); do
    [[ -f "$TMP/restart-timestamps.txt" ]] && [[ $(wc -l < "$TMP/restart-timestamps.txt") -ge 2 ]] && break
    sleep .02
done
[[ $(wc -l < "$TMP/restart-timestamps.txt") -ge 2 ]]
first_launch=$(sed -n '1p' "$TMP/restart-timestamps.txt")
second_launch=$(sed -n '2p' "$TMP/restart-timestamps.txt")
restart_delay_ns=$((second_launch - first_launch))
"$BIN_DIR/pm" delete restart_probe >/dev/null 2>&1 || true

python3 - "$latency_file" "$OUTPUT" <<PY
import json
import os
import platform
import statistics
import subprocess
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    latency = sorted(int(line) / 1_000_000 for line in stream if line.strip())

def percentile(values, fraction):
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))]

cpu_model = "unknown"
with open("/proc/cpuinfo", encoding="utf-8") as stream:
    for line in stream:
        if line.startswith("model name"):
            cpu_model = line.split(":", 1)[1].strip()
            break

data = {
    "schema_version": 1,
    "pm_tiny_version": "2.0.0",
    "environment": {
        "architecture": platform.machine(),
        "kernel": platform.release(),
        "cpu_model": cpu_model,
        "logical_cpu_count": os.cpu_count(),
        "compiler": subprocess.check_output(["c++", "--version"], text=True).splitlines()[0],
        "build_type": "Release",
    },
    "binary_size_bytes": {
        "pm_tiny": os.path.getsize("$BIN_DIR/pm_tiny"),
        "pm": os.path.getsize("$BIN_DIR/pm"),
    },
    "daemon_rss_kib": {
        "idle": $idle_rss_kib,
        "with_${PROCESS_COUNT}_managed_processes": $loaded_rss_kib,
    },
    "list_json_latency_ms": {
        "samples": len(latency),
        "median": round(statistics.median(latency), 3),
        "p95": round(percentile(latency, 0.95), 3),
    },
    "start_${PROCESS_COUNT}_processes_ms": round(($batch_finished - $batch_started) / 1_000_000, 3),
    "crash_restart_interval_ms": round($restart_delay_ns / 1_000_000, 3),
}
with open(sys.argv[2], "w", encoding="utf-8") as stream:
    json.dump(data, stream, ensure_ascii=False, indent=2)
    stream.write("\n")
print(json.dumps(data, ensure_ascii=False, indent=2))
PY

"$BIN_DIR/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
