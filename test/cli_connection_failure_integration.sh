#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
TMP="$(mktemp -d /tmp/pm_tiny_cli_connection_failure.XXXXXX)"
DAEMON_PID=

cleanup() {
    status=$?
    trap - EXIT
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    if [[ $status -ne 0 ]]; then
        for log_file in "$TMP"/*.out "$TMP"/*.stdout "$TMP"/*.stderr; do
            [[ -f "$log_file" ]] || continue
            echo "--- $log_file ---" >&2
            tail -100 "$log_file" >&2
        done
    fi
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
printf '%s\n' '[]' > "$TMP/prog.yaml"
cat > "$TMP/pm_tiny.yaml" <<EOF
pm_tiny_home_dir: $TMP/home
pm_tiny_sock_file: $TMP/daemon.sock
pm_tiny_log_file: $TMP/pm.log
pm_tiny_prog_cfg_file: $TMP/prog.yaml
pm_tiny_app_log_dir: $TMP/logs
pm_tiny_app_environ_dir: $TMP/environ
pm_tiny_uds_abstract_namespace: false
EOF

expect_connect_failure() {
    name=$1
    socket_file=$2
    abstract_namespace=$3
    expected_endpoint=$4
    expected_transport=$5
    stdout_file="$TMP/$name.stdout"
    stderr_file="$TMP/$name.stderr"

    set +e
    PM_TINY_HOME="$TMP/home" \
    PM_TINY_SOCK_FILE="$socket_file" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE="$abstract_namespace" \
        timeout --kill-after=1s 5s "$BIN/pm" list >"$stdout_file" 2>"$stderr_file"
    command_status=$?
    set -e

    if [[ $command_status -ne 1 ]]; then
        echo "$name: expected exit status 1, got $command_status" >&2
        return 1
    fi
    if [[ -s "$stdout_file" ]]; then
        echo "$name: expected empty stdout" >&2
        return 1
    fi
    if ! grep -qxF 'pm: cannot connect to pm_tiny' "$stderr_file"; then
        echo "$name: missing connection failure summary" >&2
        return 1
    fi
    if ! grep -qxF "  endpoint: $expected_endpoint" "$stderr_file"; then
        echo "$name: missing expected endpoint '$expected_endpoint'" >&2
        return 1
    fi
    if ! grep -qxF "  transport: $expected_transport" "$stderr_file"; then
        echo "$name: missing expected transport '$expected_transport'" >&2
        return 1
    fi
    if ! grep -qE '^  reason: .+ \(errno=[0-9]+\)$' "$stderr_file"; then
        echo "$name: missing reason or errno" >&2
        return 1
    fi
    if ! grep -qE '^  hint: .+' "$stderr_file"; then
        echo "$name: missing connection hint" >&2
        return 1
    fi
}

start_daemon() {
    name=$1
    socket_file=$2
    abstract_namespace=$3

    PM_TINY_HOME="$TMP/home" \
    PM_TINY_SOCK_FILE="$socket_file" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE="$abstract_namespace" \
    PM_TINY_PROG_CFG_FILE="$TMP/prog.yaml" \
    PM_TINY_LOG_FILE="$TMP/pm.log" \
    PM_TINY_APP_LOG_DIR="$TMP/logs" \
    PM_TINY_APP_ENVIRON_DIR="$TMP/environ" \
        "$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" >"$TMP/$name-daemon.out" 2>&1 &
    DAEMON_PID=$!

    if [[ $abstract_namespace -eq 0 ]]; then
        for _ in $(seq 1 50); do
            [[ -S "$socket_file" ]] && return
            kill -0 "$DAEMON_PID" 2>/dev/null || break
            sleep .05
        done
        echo "$name: daemon file socket was not created" >&2
        return 1
    fi

    for _ in $(seq 1 50); do
        PM_TINY_HOME="$TMP/home" \
        PM_TINY_SOCK_FILE="$socket_file" \
        PM_TINY_UDS_ABSTRACT_NAMESPACE=1 \
            timeout --kill-after=1s 5s "$BIN/pm" list >/dev/null 2>&1 && return
        kill -0 "$DAEMON_PID" 2>/dev/null || break
        sleep .05
    done
    echo "$name: daemon abstract socket did not become ready" >&2
    return 1
}

stop_daemon() {
    socket_file=$1
    abstract_namespace=$2
    PM_TINY_HOME="$TMP/home" \
    PM_TINY_SOCK_FILE="$socket_file" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE="$abstract_namespace" \
        timeout --kill-after=1s 5s "$BIN/pm" quit >/dev/null
    wait "$DAEMON_PID"
    DAEMON_PID=
}

expect_connect_failure daemon_not_running "$TMP/not-running.sock" 0 \
    "$TMP/not-running.sock" 'unix socket (filesystem)'

file_socket="$TMP/daemon.sock"
start_daemon file "$file_socket" 0
expect_connect_failure wrong_file_socket "$TMP/wrong.sock" 0 \
    "$TMP/wrong.sock" 'unix socket (filesystem)'
expect_connect_failure file_daemon_abstract_client "$file_socket" 1 \
    "@$file_socket" 'unix socket (abstract)'
stop_daemon "$file_socket" 0

stale_socket="$TMP/stale.sock"
python3 -c 'import socket,sys; sock=socket.socket(socket.AF_UNIX); sock.bind(sys.argv[1]); sock.close()' "$stale_socket"
expect_connect_failure stale_file_socket "$stale_socket" 0 \
    "$stale_socket" 'unix socket (filesystem)'

abstract_socket="pm_tiny_cli_failure_$$"
start_daemon abstract "$abstract_socket" 1
expect_connect_failure abstract_daemon_file_client "$abstract_socket" 0 \
    "$abstract_socket" 'unix socket (filesystem)'
expect_connect_failure wrong_abstract_socket "${abstract_socket}_wrong" 1 \
    "@${abstract_socket}_wrong" 'unix socket (abstract)'
stop_daemon "$abstract_socket" 1

echo 'cli connection failure integration: PASS'
