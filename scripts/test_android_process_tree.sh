#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "usage: $0 <adb-serial> [android-install-dir]" >&2
    exit 2
}

[[ $# -ge 1 && $# -le 2 ]] || usage
SERIAL=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/scripts/android_production_guard.sh"
INSTALL_DIR=${2:-"$ROOT/.build_android/_install/Release"}
DAEMON_BIN="$INSTALL_DIR/bin/pm_tiny"
CLIENT_BIN="$INSTALL_DIR/bin/pm2"
FIXTURE="$ROOT/scripts/android_process_tree_fixture.sh"

for file in "$DAEMON_BIN" "$CLIENT_BIN" "$FIXTURE"; do
    [[ -f "$file" ]] || { echo "missing required file: $file" >&2; exit 1; }
done

ADB=(adb -s "$SERIAL")
REMOTE_BASE="/data/local/tmp/pm_tiny_process_tree_test_$$"
[[ "$REMOTE_BASE" =~ ^/data/local/tmp/pm_tiny_process_tree_test_[0-9]+$ ]] || exit 1
STAMP=$(date +%Y%m%d-%H%M%S)
ARTIFACT_DIR="$ROOT/build/test-artifacts/android/${STAMP}-${SERIAL}"
mkdir -p "$ARTIFACT_DIR"

ACTIVE_TAG=""
PRODUCTION_DAEMON_PID=""
FINAL_STATUS=FAIL
cleanup() {
    local exit_code=$?
    set +e
    if [[ -n "$ACTIVE_TAG" ]]; then
        remote_pm "$ACTIVE_TAG" quit >/dev/null 2>&1
    fi
    "${ADB[@]}" shell "for file in '$REMOTE_BASE'/*_home/pm_tiny.pid '$REMOTE_BASE'/*-pids/*.pid; do test -f \"\$file\" || continue; kill -9 \"\$(cat \"\$file\")\" >/dev/null 2>&1 || true; done"
    if [[ ! -d "$ARTIFACT_DIR/remote" ]]; then
        "${ADB[@]}" pull "$REMOTE_BASE" "$ARTIFACT_DIR/remote" >/dev/null 2>&1
    fi
    "${ADB[@]}" shell "rm -rf '$REMOTE_BASE'"
    printf 'status=%s\nexit_code=%d\n' "$FINAL_STATUS" "$exit_code" > "$ARTIFACT_DIR/result.txt"
    return "$exit_code"
}
trap cleanup EXIT

remote_pm() {
    local tag=$1
    shift
    "${ADB[@]}" shell \
        "env PM_TINY_HOME='$REMOTE_BASE/${tag}_home' PM_TINY_SOCK_FILE='$REMOTE_BASE/${tag}.sock' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 '$REMOTE_BASE/pm2_test' $*"
}

wait_pm() {
    local tag=$1
    for _ in $(seq 1 80); do
        if remote_pm "$tag" ls >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    echo "daemon did not become ready: $tag" >&2
    return 1
}

write_config() {
    local tag=$1 mode=$2
    local cfg="$ARTIFACT_DIR/${tag}.yaml"
    local prog="$ARTIFACT_DIR/${tag}-prog.yaml"
    printf '%s\n' '[]' > "$prog"
    printf '%s\n' \
        "pm_tiny_home_dir: $REMOTE_BASE/${tag}_home" \
        "pm_tiny_sock_file: $REMOTE_BASE/${tag}.sock" \
        "pm_tiny_log_file: $REMOTE_BASE/${tag}.log" \
        "pm_tiny_prog_cfg_file: $REMOTE_BASE/${tag}-prog.yaml" \
        "pm_tiny_app_log_dir: $REMOTE_BASE/${tag}-logs" \
        "pm_tiny_app_environ_dir: $REMOTE_BASE/${tag}-environ" \
        "pm_tiny_uds_abstract_namespace: false" \
        "pm_tiny_process_tree_mode: $mode" > "$cfg"
    "${ADB[@]}" push "$cfg" "$REMOTE_BASE/${tag}.yaml" >/dev/null
    "${ADB[@]}" push "$prog" "$REMOTE_BASE/${tag}-prog.yaml" >/dev/null
}

start_daemon() {
    local tag=$1 mode=$2
    write_config "$tag" "$mode"
    ACTIVE_TAG=$tag
    "${ADB[@]}" shell \
        "mkdir -p '$REMOTE_BASE/${tag}_home' '$REMOTE_BASE/${tag}-logs' '$REMOTE_BASE/${tag}-environ'; env PM_TINY_HOME='$REMOTE_BASE/${tag}_home' PM_TINY_SOCK_FILE='$REMOTE_BASE/${tag}.sock' PM_TINY_UDS_ABSTRACT_NAMESPACE=0 PM_TINY_PROCESS_TREE_MODE='$mode' nohup '$REMOTE_BASE/pm_tiny_test' -c '$REMOTE_BASE/${tag}.yaml' </dev/null >'$REMOTE_BASE/${tag}.out' 2>&1 &"
    wait_pm "$tag"
}

stop_daemon() {
    local tag=$1
    remote_pm "$tag" quit >/dev/null
    ACTIVE_TAG=""
    for _ in $(seq 1 50); do
        if ! "${ADB[@]}" shell "test -e '$REMOTE_BASE/${tag}_home/pm_tiny.pid'"; then return 0; fi
        sleep 0.1
    done
    echo "daemon did not exit cleanly: $tag" >&2
    return 1
}

capture_pids() {
    local pid_dir=$1 output=$2
    "${ADB[@]}" shell "cat '$pid_dir'/*.pid 2>/dev/null | sort -nu" | tr -d '\r' > "$output"
    [[ -s "$output" ]] || { echo "fixture did not record PIDs: $pid_dir" >&2; return 1; }
}

assert_all_gone() {
    local pid_file=$1
    local pid
    while read -r pid; do
        [[ -n "$pid" ]] || continue
        if "${ADB[@]}" shell "kill -0 '$pid' 2>/dev/null"; then
            echo "process-tree survivor: $pid" >&2
            return 1
        fi
    done < "$pid_file"
}

assert_any_alive() {
    local pid_file=$1
    local pid
    while read -r pid; do
        [[ -n "$pid" ]] || continue
        if "${ADB[@]}" shell "kill -0 '$pid' 2>/dev/null"; then return 0; fi
    done < "$pid_file"
    echo "expected at least one surviving descendant" >&2
    return 1
}

wait_all_gone() {
    local pid_file=$1
    for _ in $(seq 1 60); do
        if assert_all_gone "$pid_file" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    assert_all_gone "$pid_file"
}

start_fixture() {
    local tag=$1 name=$2 mode=$3 pid_dir=$4
    remote_pm "$tag" start \
        "$name --kill-timeout 2 --user root --no-daemon --no-pty -- $REMOTE_BASE/fixture.sh $mode $pid_dir" >/dev/null
    for _ in $(seq 1 30); do
        if "${ADB[@]}" shell "test -f '$pid_dir/grandchild.pid'"; then return 0; fi
        sleep 0.1
    done
    echo "fixture did not start: $name" >&2
    return 1
}

run_mode_cases() {
    local tag=$1 mode=$2
    local fork_dir="$REMOTE_BASE/${tag}-fork-pids"
    local orphan_dir="$REMOTE_BASE/${tag}-orphan-pids"
    local fork_pids="$ARTIFACT_DIR/${tag}-fork.pids"
    local orphan_pids="$ARTIFACT_DIR/${tag}-orphan.pids"

    start_daemon "$tag" "$mode"
    start_fixture "$tag" "${tag}_fork" fork "$fork_dir"
    capture_pids "$fork_dir" "$fork_pids"
    local stop_started_ms stop_finished_ms
    stop_started_ms=$(date +%s%3N)
    remote_pm "$tag" stop "${tag}_fork" >/dev/null
    stop_finished_ms=$(date +%s%3N)
    if ((stop_finished_ms - stop_started_ms < 1800)); then
        echo "kill timeout was not observed for $tag" >&2
        return 1
    fi
    wait_all_gone "$fork_pids"
    "${ADB[@]}" shell "grep -q '${tag}_fork.*SIGKILL' '$REMOTE_BASE/${tag}.log'"
    remote_pm "$tag" start "${tag}_fork" >/dev/null
    remote_pm "$tag" restart "${tag}_fork" >/dev/null
    remote_pm "$tag" delete "${tag}_fork" >/dev/null

    start_fixture "$tag" "${tag}_orphan" orphan "$orphan_dir"
    capture_pids "$orphan_dir" "$orphan_pids"
    wait_all_gone "$orphan_pids"
    "${ADB[@]}" shell "grep -q '${tag}_orphan.*root exited while descendants remain' '$REMOTE_BASE/${tag}.log'"
    remote_pm "$tag" delete "${tag}_orphan" >/dev/null 2>&1 || true
    remote_pm "$tag" reload >/dev/null
    "${ADB[@]}" shell "grep -q 'effective=$mode' '$REMOTE_BASE/${tag}.log'"
    stop_daemon "$tag"
}

run_sigkill_recovery() {
    local tag=crash mode=cgroup
    local pid_dir="$REMOTE_BASE/crash-pids"
    local pid_file="$ARTIFACT_DIR/crash.pids"
    start_daemon "$tag" "$mode"
    start_fixture "$tag" crash_tree fork "$pid_dir"
    capture_pids "$pid_dir" "$pid_file"
    local daemon_pid
    daemon_pid=$("${ADB[@]}" shell "cat '$REMOTE_BASE/${tag}_home/pm_tiny.pid'" | tr -d '\r')
    "${ADB[@]}" shell "kill -9 '$daemon_pid'"
    ACTIVE_TAG=""
    sleep 0.5
    assert_any_alive "$pid_file"
    start_daemon "$tag" "$mode"
    wait_all_gone "$pid_file"
    local init_count
    init_count=$("${ADB[@]}" shell "grep -c 'effective=cgroup' '$REMOTE_BASE/${tag}.log'" | tr -d '\r')
    ((init_count >= 2))
    stop_daemon "$tag"
}

"${ADB[@]}" get-state | grep -qx device
PRODUCT=$("${ADB[@]}" shell getprop ro.product.device | tr -d '\r')
ANDROID=$("${ADB[@]}" shell getprop ro.build.version.release | tr -d '\r')
printf 'serial=%s\nproduct=%s\nandroid=%s\n' "$SERIAL" "$PRODUCT" "$ANDROID" | tee "$ARTIFACT_DIR/device.txt"

"${ADB[@]}" root >/dev/null
"${ADB[@]}" wait-for-device
PRODUCTION_DAEMON_PID=$("${ADB[@]}" shell "pidof pm_tiny" | tr -d '\r')
[[ "$PRODUCTION_DAEMON_PID" =~ ^[0-9]+$ ]]
capture_production_state "$ARTIFACT_DIR/production-before.json"
"${ADB[@]}" shell "mkdir -p '$REMOTE_BASE'"
"${ADB[@]}" push "$DAEMON_BIN" "$REMOTE_BASE/pm_tiny_test" >/dev/null
"${ADB[@]}" push "$CLIENT_BIN" "$REMOTE_BASE/pm2_test" >/dev/null
"${ADB[@]}" push "$FIXTURE" "$REMOTE_BASE/fixture.sh" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE_BASE/pm_tiny_test' '$REMOTE_BASE/pm2_test' '$REMOTE_BASE/fixture.sh'"

run_mode_cases cgroup cgroup
run_mode_cases process_group process_group
run_sigkill_recovery

"${ADB[@]}" shell "kill -0 '$PRODUCTION_DAEMON_PID'"
capture_production_state "$ARTIFACT_DIR/production-after.json"
assert_production_unchanged "$ARTIFACT_DIR/production-before.json" "$ARTIFACT_DIR/production-after.json"

"${ADB[@]}" pull "$REMOTE_BASE" "$ARTIFACT_DIR/remote" >/dev/null
FINAL_STATUS=PASS
printf 'android process tree regression: PASS\nartifacts: %s\n' "$ARTIFACT_DIR"
