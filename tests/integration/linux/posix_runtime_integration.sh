#!/usr/bin/env bash
set -euo pipefail

BIN=${PM_TINY_TEST_BIN:?PM_TINY_TEST_BIN is required}
TEST_ROOT=$(mktemp -d /tmp/pm_tiny_posix_runtime.XXXXXX)
DAEMON_PID=

cleanup() {
    PM_TINY_HOME="$TEST_ROOT/home" PM_TINY_SOCK_FILE="$TEST_ROOT/pm.sock" \
        PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" quit >/dev/null 2>&1 || true
    if [[ -n "$DAEMON_PID" ]]; then
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

mkdir -p "$TEST_ROOT/home" "$TEST_ROOT/logs" "$TEST_ROOT/environ"
printf '[]\n' > "$TEST_ROOT/prog.yaml"
cat > "$TEST_ROOT/pm_tiny.yaml" <<EOF
pm_tiny_home_dir: $TEST_ROOT/home
pm_tiny_sock_file: $TEST_ROOT/pm.sock
pm_tiny_log_file: $TEST_ROOT/daemon.log
pm_tiny_prog_cfg_file: $TEST_ROOT/prog.yaml
pm_tiny_app_log_dir: $TEST_ROOT/logs
pm_tiny_app_environ_dir: $TEST_ROOT/environ
pm_tiny_uds_abstract_namespace: false
pm_tiny_process_tree_mode: process_group
EOF

export PM_TINY_HOME="$TEST_ROOT/home"
export PM_TINY_SOCK_FILE="$TEST_ROOT/pm.sock"
export PM_TINY_UDS_ABSTRACT_NAMESPACE=0
"$BIN/pm_tiny" -c "$TEST_ROOT/pm_tiny.yaml" >"$TEST_ROOT/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 100); do
    [[ -S "$TEST_ROOT/pm.sock" ]] && break
    sleep 0.05
done
[[ -S "$TEST_ROOT/pm.sock" ]]

wait_stopped() {
    local name=$1
    for _ in $(seq 1 100); do
        if "$BIN/pm" list --json | python3 -c '
import json, sys
name = sys.argv[1]
data = json.load(sys.stdin)
item = next((item for item in data["processes"] if item["name"] == name), None)
raise SystemExit(0 if item is not None and item["state"] in {"stopped", "exited"} else 1)
' "$name"; then
            return 0
        fi
        sleep 0.05
    done
    "$BIN/pm" list --json >&2
    return 1
}

"$BIN/pm" start pty_probe --no-daemon --pty --log-max-size-kb 1 \
    --log-archive-count 2 -- "$BIN/tests/posix_runtime_fixture" pty >/dev/null
wait_stopped pty_probe
PTY_LOG="$TEST_ROOT/logs/pty_probe.log"
grep -q 'stdin_tty=1 stdout_tty=1 stderr_tty=1' "$PTY_LOG"
grep -q 'pty-stdout-marker' "$PTY_LOG"
grep -q 'pty-stderr-marker' "$PTY_LOG"
"$BIN/pm" inspect pty_probe | grep -q 'combined'

if "$BIN/pm" start invalid_pty --no-daemon --pty --log-mode split -- \
    "$BIN/tests/posix_runtime_fixture" pty >"$TEST_ROOT/invalid.stdout" 2>"$TEST_ROOT/invalid.stderr"; then
    echo 'pty with split logging unexpectedly succeeded' >&2
    exit 1
fi
grep -Eiq 'pty.*combined|split.*pty' "$TEST_ROOT/invalid.stderr"

"$BIN/pm" start rotation_probe --no-daemon --no-pty --log-mode split \
    --log-max-size-kb 1 --log-archive-count 2 -- \
    "$BIN/tests/posix_runtime_fixture" rotation >/dev/null
wait_stopped rotation_probe

for stream in stdout stderr; do
    base="$TEST_ROOT/logs/rotation_probe_${stream}.log"
    [[ -f "$base" && -f "$base.1" && -f "$base.2" ]]
    [[ ! -e "$base.3" ]]
    [[ $(stat -c %s "$base") -eq 1024 ]]
    [[ $(stat -c %s "$base.1") -eq 1024 ]]
    [[ $(stat -c %s "$base.2") -eq 1024 ]]
done

python3 - "$TEST_ROOT/logs" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
for stream, expected in (("stdout", "DCB"), ("stderr", "dcb")):
    base = root / f"rotation_probe_{stream}.log"
    files = (base, Path(str(base) + ".1"), Path(str(base) + ".2"))
    actual = "".join(path.read_bytes()[:1].decode("ascii") for path in files)
    assert actual == expected, (stream, actual)
PY

"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=
echo 'posix runtime integration: PASS'
