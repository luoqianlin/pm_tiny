#!/usr/bin/env bash
set -euo pipefail

if ! command -v sudo >/dev/null || ! sudo -n true >/dev/null 2>&1 || ! id nobody >/dev/null 2>&1; then
    echo "privilege drop integration: SKIP (passwordless sudo or nobody unavailable)"
    exit 0
fi

CALLER_UID="$(id -u)"
CALLER_GID="$(id -g)"
if [[ $CALLER_UID -eq 0 ]]; then
    echo "privilege drop integration: SKIP (test requires a non-root CLI peer)"
    exit 0
fi

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${PM_TINY_TEST_BIN:-$ROOT/build}"
PYTHON="$(command -v python3)"
TMP="$(mktemp -d /tmp/pm_tiny_privilege.XXXXXX)"
DAEMON_PID=0
stage() {
    echo "privilege drop integration: $1"
}
cleanup() {
    status=$?
    trap - EXIT
    if [[ $status -ne 0 ]]; then
        for path in "$TMP/daemon.out" "$TMP/pm.log" "$TMP/prog.yaml" \
            "$TMP/wrapper.stderr" "$TMP/peer.stderr" "$TMP/rejected.stderr"; do
            [[ -e $path ]] || continue
            echo "--- $path ---" >&2
            sudo -n tail -200 "$path" >&2 || true
        done
        if [[ -d "$TMP/logs" ]]; then
            sudo -n find "$TMP/logs" -maxdepth 1 -type f -print -exec tail -100 {} \; >&2 || true
        fi
    fi
    if [[ $DAEMON_PID -gt 0 ]]; then
        sudo -n env PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
            PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" quit >/dev/null 2>&1 || true
        sudo -n kill "$DAEMON_PID" >/dev/null 2>&1 || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    sudo -n rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$TMP/home" "$TMP/logs" "$TMP/environ"
chmod 777 "$TMP"
printf '%s\n' '[]' > "$TMP/prog.yaml"
printf '%s\n' \
    "pm_tiny_home_dir: $TMP/home" \
    "pm_tiny_sock_file: $TMP/pm.sock" \
    "pm_tiny_log_file: $TMP/pm.log" \
    "pm_tiny_prog_cfg_file: $TMP/prog.yaml" \
    "pm_tiny_app_log_dir: $TMP/logs" \
    "pm_tiny_app_environ_dir: $TMP/environ" \
    'pm_tiny_uds_abstract_namespace: false' \
    "pm_tiny_allowed_uids: [$CALLER_UID]" \
    'pm_tiny_process_tree_mode: process_group' > "$TMP/pm_tiny.yaml"

# The invoking test user intentionally owns the diagnostic file.
# shellcheck disable=SC2024
sudo -n "$BIN/pm_tiny" -c "$TMP/pm_tiny.yaml" > "$TMP/daemon.out" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 100); do
    [[ -S "$TMP/pm.sock" ]] && break
    sleep .05
done
[[ -S "$TMP/pm.sock" ]]
sudo -n chmod 777 "$TMP/pm.sock"

stage "CLI privilege-wrapper warning"
set +e
PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/missing.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" start wrapper_warning -- /usr/bin/sudo -n /bin/true \
    >"$TMP/wrapper.stdout" 2>"$TMP/wrapper.stderr"
wrapper_status=$?
set -e
[[ $wrapper_status -eq 1 ]]
grep -q 'interactive privilege elevation is unsupported' "$TMP/wrapper.stderr"

peer_probe="printf '%s\\n' \"\$(id -u)\" \"\$(id -g)\" > '$TMP/peer_identity'"
stage "peer-credential default identity"
env HOME=/tmp PATH=/usr/bin:/bin \
    PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" start peer_identity --cwd "$TMP" \
    --no-daemon --no-pty -- sh -c "$peer_probe" >"$TMP/peer.stdout" 2>"$TMP/peer.stderr"
for _ in $(seq 1 100); do
    [[ -s "$TMP/peer_identity" ]] && break
    sleep .02
done
[[ $(sed -n '1p' "$TMP/peer_identity") == "$CALLER_UID" ]]
[[ $(sed -n '2p' "$TMP/peer_identity") == "$CALLER_GID" ]]
if grep -q 'interactive privilege elevation is unsupported' "$TMP/peer.stderr"; then
    echo "ordinary executable produced a privilege-wrapper warning" >&2
    exit 1
fi

stage "cross-user path validation"
set +e
# The invoking test user intentionally owns the captured CLI output.
# shellcheck disable=SC2024
env HOME=/tmp PATH=/usr/bin:/bin \
    PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" start rejected_path --cwd "$TMP" \
    --user root -- sh -c true >"$TMP/rejected.stdout" 2>"$TMP/rejected.stderr"
rejected_status=$?
set -e
[[ $rejected_status -eq 1 ]]
grep -q 'cross-user start requires executable containing' "$TMP/rejected.stderr"

stage "root launch environment"
root_probe="import os; keys=['HOME','USER','LOGNAME','SHELL','PATH','SUDO_USER','LD_LIBRARY_PATH']; \
open('$TMP/root_environment','w').write('\\n'.join(os.environ.get(key, 'unset') for key in keys) + '\\n')"
env HOME=/fake-home USER=fake-user LOGNAME=fake-logname SHELL=/fake-shell \
    PATH=/usr/bin:/bin SUDO_USER=fake-sudo LD_LIBRARY_PATH=/tmp/fake-lib \
    PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" start root_environment --cwd "$TMP" \
    --user root --no-daemon --no-pty -- "$PYTHON" -c "$root_probe" >/dev/null
for _ in $(seq 1 100); do
    [[ -s "$TMP/root_environment" ]] && break
    sleep .02
done
root_home="$(getent passwd root | cut -d: -f6)"
root_shell="$(getent passwd root | cut -d: -f7)"
[[ $(sed -n '1p' "$TMP/root_environment") == "$root_home" ]]
[[ $(sed -n '2p' "$TMP/root_environment") == root ]]
[[ $(sed -n '3p' "$TMP/root_environment") == root ]]
[[ $(sed -n '4p' "$TMP/root_environment") == "$root_shell" ]]
[[ $(sed -n '5p' "$TMP/root_environment") == unset ]]
[[ $(sed -n '6p' "$TMP/root_environment") == unset ]]
[[ $(sed -n '7p' "$TMP/root_environment") == unset ]]

stage "explicit environment overrides"
override_probe="import os; keys=['HOME','PATH','LD_LIBRARY_PATH']; \
open('$TMP/root_override','w').write('\\n'.join(os.environ[key] for key in keys) + '\\n')"
env HOME=/fake-home PATH=/usr/bin:/bin \
    PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" start root_override --cwd "$TMP" \
    --user root --env HOME=/explicit-home --env PATH=/explicit/bin \
    --env LD_LIBRARY_PATH=/explicit/lib --env AUDIT_SECRET=must-not-appear \
    --no-daemon --no-pty -- "$PYTHON" -c "$override_probe" >/dev/null
for _ in $(seq 1 100); do
    [[ -s "$TMP/root_override" ]] && break
    sleep .02
done
[[ $(sed -n '1p' "$TMP/root_override") == /explicit-home ]]
[[ $(sed -n '2p' "$TMP/root_override") == /explicit/bin ]]
[[ $(sed -n '3p' "$TMP/root_override") == /explicit/lib ]]

stage "explicit nobody identity"
probe="id -u > '$TMP/identity'; id -g >> '$TMP/identity'; id -G >> '$TMP/identity'; sleep 1"
sudo -n env PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" start identity_probe --no-daemon --no-pty \
    --user nobody -- /bin/sh -c "$probe" >/dev/null
for _ in $(seq 1 100); do
    [[ -s "$TMP/identity" ]] && [[ $(wc -l < "$TMP/identity") -ge 3 ]] && break
    sleep .02
done
[[ $(sed -n '1p' "$TMP/identity") == "$(id -u nobody)" ]]
[[ $(sed -n '2p' "$TMP/identity") == "$(id -g nobody)" ]]
if sed -n '3p' "$TMP/identity" | tr ' ' '\n' | grep -qx '0'; then
    echo "supplementary groups retained root" >&2
    exit 1
fi

stage "save and reload environment snapshots"
sudo -n env PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" save >/dev/null
sudo -n rm -f "$TMP/peer_identity" "$TMP/root_environment" "$TMP/root_override"
sudo -n env PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" reload >/dev/null
for _ in $(seq 1 100); do
    [[ -s "$TMP/peer_identity" && -s "$TMP/root_environment" && -s "$TMP/root_override" ]] && break
    sleep .02
done
[[ $(sed -n '1p' "$TMP/peer_identity") == "$CALLER_UID" ]]
[[ $(sed -n '5p' "$TMP/root_environment") == unset ]]
[[ $(sed -n '6p' "$TMP/root_environment") == unset ]]
[[ $(sed -n '7p' "$TMP/root_environment") == unset ]]
[[ $(sed -n '1p' "$TMP/root_override") == /explicit-home ]]
[[ $(sed -n '2p' "$TMP/root_override") == /explicit/bin ]]
[[ $(sed -n '3p' "$TMP/root_override") == /explicit/lib ]]

stage "audit log validation"
sudo -n grep -q "start request peer_pid=.*peer_uid=$CALLER_UID.*name=root_environment.*target_user=root" "$TMP/pm.log"
if sudo -n grep -q 'AUDIT_SECRET\|must-not-appear\|fake-home\|explicit-home' "$TMP/pm.log"; then
    echo "audit log leaked command arguments or environment" >&2
    exit 1
fi

stage "cleanup managed definitions"
sudo -n env PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" delete identity_probe >/dev/null
for name in peer_identity root_environment root_override; do
    sudo -n env PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
        PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" delete "$name" >/dev/null
done
sudo -n env PM_TINY_HOME="$TMP/home" PM_TINY_SOCK_FILE="$TMP/pm.sock" \
    PM_TINY_UDS_ABSTRACT_NAMESPACE=0 "$BIN/pm" quit >/dev/null
wait "$DAEMON_PID"
DAEMON_PID=0
echo "privilege drop integration: PASS"
