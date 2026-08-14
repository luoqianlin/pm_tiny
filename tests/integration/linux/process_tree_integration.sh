#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_tree.XXXXXX)"
DAEMON_PID=""
cleanup() {
    if [[ -n "$DAEMON_PID" ]]; then kill "$DAEMON_PID" 2>/dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
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

export PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock"
export PM_TINY_PROG_CFG_FILE="$TMP/prog.yaml" PM_TINY_LOG_FILE="$TMP/pm.log"
export PM_TINY_APP_LOG_DIR="$TMP/logs" PM_TINY_APP_ENVIRON_DIR="$TMP/environ"
export PM_TINY_PROCESS_TREE_MODE=process_group

"$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" >"$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do [[ -S "$TMP/pm.sock" ]] && break; sleep .05; done
[[ -S "$TMP/pm.sock" ]]

assert_gone() {
    while read -r pid; do
        [[ -z "$pid" ]] && continue
        if kill -0 "$pid" 2>/dev/null; then
            echo "process-tree survivor: $pid" >&2
            return 1
        fi
    done < <(sort -u "$1")
}

all_gone() {
    [[ -f "$1" ]] || return 1
    while read -r pid; do
        [[ -z "$pid" ]] && continue
        if kill -0 "$pid" 2>/dev/null; then return 1; fi
    done < "$1"
    return 0
}

"$BIN/pm" start tree_fork --no-daemon --no-pty --kill-timeout 1 -- \
    "$BIN/tests/process_tree_fixture" fork "$TMP/fork.pids" >/dev/null
sleep .2
"$BIN/pm" stop tree_fork >/dev/null
sleep .2
assert_gone "$TMP/fork.pids"

"$BIN/pm" start tree_orphan --no-daemon --no-pty --kill-timeout 1 -- \
    "$BIN/tests/process_tree_fixture" orphan "$TMP/orphan.pids" >/dev/null
for _ in $(seq 1 30); do
    all_gone "$TMP/orphan.pids" && break
    sleep .1
done
assert_gone "$TMP/orphan.pids"

"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=""
grep -q 'effective=process_group' "$TMP/pm.log"
echo 'process tree integration: PASS'
