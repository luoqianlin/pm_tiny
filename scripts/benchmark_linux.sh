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
        PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
            "$BIN_DIR/pm" quit >/dev/null 2>&1
        kill "$DAEMON_PID" >/dev/null 2>&1
        wait "$DAEMON_PID" >/dev/null 2>&1
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ" "$(dirname "$OUTPUT")"
: > "$TMP/prog.yaml"

export PM_TINY_HOME="$TMP/home"
export PM_TINY_SOCK_FILE="$TMP/pm.sock"
export PM_TINY_PROG_CFG_FILE="$TMP/prog.yaml"
export PM_TINY_LOG_FILE="$TMP/pm.log"
export PM_TINY_APP_LOG_DIR="$TMP/logs"
export PM_TINY_APP_ENVIRON_DIR="$TMP/environ"
export PM_TINY_UDS_ABSTRACT_NAMESPACE=false
export PM_TINY_PROCESS_TREE_MODE=process_group

"$BIN_DIR/pm_tiny" > "$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 100); do
    [[ -S "$TMP/pm.sock" ]] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || {
        cat "$TMP/daemon.out" >&2
        exit 1
    }
    sleep .05
done
[[ -S "$TMP/pm.sock" ]] || {
    echo "daemon control socket was not created" >&2
    cat "$TMP/daemon.out" >&2
    exit 1
}

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
    "$BIN_DIR/pm" start "bench_$index" --no-daemon --no-pty -- /usr/bin/sleep 120 >/dev/null
done
online=0
for _ in $(seq 1 200); do
    online=$("$BIN_DIR/pm" list --json | python3 -c '
import json, sys
print(sum(item["name"].startswith("bench_") and item["state"] == "online"
          for item in json.load(sys.stdin)["processes"]))
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

restart_probe="$TMP/restart_probe.sh"
apply_restart_config() {
    printf '%s\n' \
        '#!/usr/bin/env bash' \
        "date +%s%N >> '$TMP/restart-timestamps.txt'" \
        'exit 1' > "$restart_probe"
    chmod +x "$restart_probe"
    printf '%s\n' \
        '- name: restart_probe' \
        "  cwd: $TMP" \
        '  executable: ./restart_probe.sh' \
        '  args: []' \
        '  daemon: true' \
        '  pty: false' \
        '  start_timeout: 0' \
        '  failure_action: restart' \
        '  restart_delay_ms: 1000' \
        '  restart_max_delay_ms: 1000' \
        '  restart_window_ms: 10000' \
        '  restart_max_attempts: 2' \
        '  restart_reset_after_ms: 10000' > "$TMP/prog.yaml"
}
apply_restart_config
"$BIN_DIR/pm" reload >/dev/null
for _ in $(seq 1 200); do
    [[ -f "$TMP/restart-timestamps.txt" ]] && \
        [[ $(wc -l < "$TMP/restart-timestamps.txt") -ge 2 ]] && break
    sleep .02
done
[[ -f "$TMP/restart-timestamps.txt" ]]
[[ $(wc -l < "$TMP/restart-timestamps.txt") -ge 2 ]]
first_launch=$(sed -n '1p' "$TMP/restart-timestamps.txt")
second_launch=$(sed -n '2p' "$TMP/restart-timestamps.txt")
restart_delay_ns=$((second_launch - first_launch))
"$BIN_DIR/pm" delete restart_probe >/dev/null 2>&1 || true

pm_tiny_version=$("$BIN_DIR/pm_tiny" --version | awk 'NR == 1 {print $2}')
[[ -n "$pm_tiny_version" ]]

python3 - "$latency_file" "$OUTPUT" "$BIN_DIR" "$PROCESS_COUNT" \
    "$pm_tiny_version" "$idle_rss_kib" "$loaded_rss_kib" \
    "$batch_started" "$batch_finished" "$restart_delay_ns" <<'PY'
import json
import os
import platform
import statistics
import subprocess
import sys

latency_path, output, bin_dir, process_count, version = sys.argv[1:6]
idle_rss, loaded_rss = map(int, sys.argv[6:8])
batch_started, batch_finished, restart_delay_ns = map(int, sys.argv[8:11])

with open(latency_path, encoding="utf-8") as stream:
    latency = sorted(int(line) / 1_000_000 for line in stream if line.strip())

def percentile(values, fraction):
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))]

cpu_model = "unknown"
with open("/proc/cpuinfo", encoding="utf-8") as stream:
    for line in stream:
        if line.startswith("model name"):
            cpu_model = line.split(":", 1)[1].strip()
            break

cache = {}
cache_path = os.path.join(bin_dir, "CMakeCache.txt")
if os.path.isfile(cache_path):
    with open(cache_path, encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if "=" in line and not line.startswith(("//", "#")):
                key_and_type, value = line.rstrip("\n").split("=", 1)
                cache[key_and_type.split(":", 1)[0]] = value

compiler = cache.get("CMAKE_CXX_COMPILER", "unknown")
compiler_version = cache.get("CMAKE_CXX_COMPILER_VERSION", "unknown")
if compiler_version != "unknown":
    compiler = f"{compiler} {compiler_version}"
elif compiler != "unknown":
    try:
        compiler = subprocess.check_output(
            [compiler, "--version"], text=True, stderr=subprocess.STDOUT
        ).splitlines()[0]
    except (OSError, subprocess.SubprocessError):
        pass

data = {
    "schema_version": 1,
    "pm_tiny_version": version,
    "environment": {
        "architecture": platform.machine(),
        "kernel": platform.release(),
        "cpu_model": cpu_model,
        "logical_cpu_count": os.cpu_count(),
        "compiler": compiler,
        "build_type": cache.get("CMAKE_BUILD_TYPE", "unknown"),
    },
    "binary_size_bytes": {
        "pm_tiny": os.path.getsize(os.path.join(bin_dir, "pm_tiny")),
        "pm": os.path.getsize(os.path.join(bin_dir, "pm")),
    },
    "daemon_rss_kib": {
        "idle": idle_rss,
        f"with_{process_count}_managed_processes": loaded_rss,
    },
    "list_json_latency_ms": {
        "samples": len(latency),
        "median": round(statistics.median(latency), 3),
        "p95": round(percentile(latency, 0.95), 3),
    },
    f"start_{process_count}_processes_ms": round((batch_finished - batch_started) / 1_000_000, 3),
    "crash_restart_interval_ms": round(restart_delay_ns / 1_000_000, 3),
}
with open(output, "w", encoding="utf-8") as stream:
    json.dump(data, stream, ensure_ascii=False, indent=2)
    stream.write("\n")
print(json.dumps(data, ensure_ascii=False, indent=2))
PY

"$BIN_DIR/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
