#!/usr/bin/env bash
set -euo pipefail

BIN=${PM_TINY_TEST_BIN:?PM_TINY_TEST_BIN is required}
TEST_ROOT=$(mktemp -d /tmp/pm_tiny_empty_config.XXXXXX)
DAEMON_PID=

cleanup() {
    PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" quit >/dev/null 2>&1 || true
    if [ -n "$DAEMON_PID" ]; then
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

printf '%s\n' \
    'pm_tiny_prog_cfg_file: prog.yaml' \
    'pm_tiny_log_file: daemon.log' \
    'pm_tiny_app_log_dir: logs' \
    'pm_tiny_app_environ_dir: environ' > "$TEST_ROOT/pm_tiny.yaml"

PM_TINY_HOME="$TEST_ROOT" PM_TINY_PROCESS_TREE_MODE=process_group \
    "$BIN/pm_tiny" -c "$TEST_ROOT/pm_tiny.yaml" --home "$TEST_ROOT" &
DAEMON_PID=$!

for _ in $(seq 1 100); do
    if [ -S "$TEST_ROOT/pm_tinyd.sock" ]; then break; fi
    sleep 0.05
done
[ -S "$TEST_ROOT/pm_tinyd.sock" ]
[ ! -e "$TEST_ROOT/prog.yaml" ]

PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" list --json |
    python3 -c 'import json,sys; data=json.load(sys.stdin); assert data["total"] == 0 and data["processes"] == []'
PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" info --json |
    python3 -c 'import json,sys; data=json.load(sys.stdin); assert data["runtime"]["file_config_count"] == 0; assert data["runtime"]["runtime_definition_count"] == 0'

PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" start empty_probe --no-daemon --no-pty -- /bin/sleep 60 >/dev/null
CHILD_PID=$(PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" list --json |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["processes"][0]["pid"])')
kill -0 "$CHILD_PID"

PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" save >/dev/null
grep -q 'name: empty_probe' "$TEST_ROOT/prog.yaml"

printf '  # intentionally empty for reload\n\t# no program definitions\n' > "$TEST_ROOT/prog.yaml"
PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" reload >/dev/null
PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" list --json |
    python3 -c 'import json,sys; data=json.load(sys.stdin); assert data["total"] == 0 and data["processes"] == []'

for _ in $(seq 1 100); do
    if ! kill -0 "$CHILD_PID" 2>/dev/null; then break; fi
    sleep 0.05
done
if kill -0 "$CHILD_PID" 2>/dev/null; then
    echo "reload did not terminate the process removed by empty config" >&2
    exit 1
fi

PM_TINY_HOME="$TEST_ROOT" "$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=
