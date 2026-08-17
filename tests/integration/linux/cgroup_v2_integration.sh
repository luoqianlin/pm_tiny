#!/usr/bin/env bash
set -euo pipefail

BIN=${PM_TINY_TEST_BIN:?PM_TINY_TEST_BIN is required}
REQUIRE=${PM_TINY_REQUIRE_REAL_CGROUP:-0}
ALLOW_SUDO=${PM_TINY_ALLOW_SUDO:-0}

if [[ ${1:-} != --as-root && $EUID -ne 0 ]]; then
    if [[ $ALLOW_SUDO == 1 ]] && sudo -n true >/dev/null 2>&1; then
        exec sudo -n env PM_TINY_TEST_BIN="$BIN" PM_TINY_REQUIRE_REAL_CGROUP="$REQUIRE" \
            bash "$0" --as-root
    fi
    if [[ $REQUIRE == 1 ]]; then
        echo 'real cgroup v2 regression requires root or PM_TINY_ALLOW_SUDO=1 with non-interactive sudo' >&2
        exit 1
    fi
    echo 'SKIP: real cgroup v2 regression requires delegated cgroup access or root'
    exit 77
fi

if [[ $(stat -fc %T /sys/fs/cgroup 2>/dev/null || true) != cgroup2fs ]]; then
    if [[ $REQUIRE == 1 ]]; then
        echo 'real cgroup v2 regression requires a cgroup2 mount at /sys/fs/cgroup' >&2
        exit 1
    fi
    echo 'SKIP: cgroup v2 is unavailable'
    exit 77
fi

TEST_ROOT=$(mktemp -d /tmp/pm_tiny_real_cgroup.XXXXXX)
CGROUP_BASE="/sys/fs/cgroup/pm_tiny-test-$$"
DAEMON_PID=

cleanup() {
    PM_TINY_HOME="$TEST_ROOT/home" PM_TINY_SOCK_FILE="$TEST_ROOT/pm.sock" \
        PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" quit >/dev/null 2>&1 || true
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" >/dev/null 2>&1 || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    find "$CGROUP_BASE" -depth -type d -exec rmdir {} \; 2>/dev/null || true
    rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

mkdir "$CGROUP_BASE"
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
pm_tiny_process_tree_mode: cgroup
pm_tiny_cgroup_root: $CGROUP_BASE
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

"$BIN/pm" info --json > "$TEST_ROOT/info.json"
CGROUP_ROOT=$(python3 - "$TEST_ROOT/info.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    info = json.load(stream)
assert info["process_tree"]["requested_mode"]["value"] == "cgroup"
assert info["process_tree"]["effective_mode"] == "cgroup"
assert info["process_tree"]["degraded"] is False
print(info["process_tree"]["cgroup_root"]["value"])
PY
)
[[ $CGROUP_ROOT == "$CGROUP_BASE"/pm_tiny-* ]]

"$BIN/pm" start real_tree --no-daemon --no-pty --kill-timeout 1 -- \
    "$BIN/tests/process_tree_fixture" fork "$TEST_ROOT/tree.pids" >/dev/null
for _ in $(seq 1 100); do
    [[ -s "$TEST_ROOT/tree.pids" ]] && break
    sleep 0.05
done
[[ -s "$TEST_ROOT/tree.pids" ]]

APP_GROUP=$(find "$CGROUP_ROOT" -mindepth 1 -maxdepth 1 -type d -name 'app-*' -print -quit)
[[ -n "$APP_GROUP" ]]
while read -r pid; do
    [[ -n "$pid" ]]
    grep -qx "$pid" "$APP_GROUP/cgroup.procs"
done < "$TEST_ROOT/tree.pids"

"$BIN/pm" stop real_tree >/dev/null
for _ in $(seq 1 100); do
    [[ ! -e "$APP_GROUP" ]] && break
    sleep 0.05
done
[[ ! -e "$APP_GROUP" ]]
while read -r pid; do
    if kill -0 "$pid" 2>/dev/null; then
        echo "real cgroup process-tree survivor: $pid" >&2
        exit 1
    fi
done < "$TEST_ROOT/tree.pids"

"$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=
[[ -z $(find "$CGROUP_ROOT" -mindepth 1 -maxdepth 1 -type d -print -quit) ]]
rmdir "$CGROUP_ROOT"
rmdir "$CGROUP_BASE"
echo 'real cgroup v2 integration: PASS'
