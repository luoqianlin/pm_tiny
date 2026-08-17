#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MANIFEST_TOOL="$SCRIPT_DIR/release_manifest.py"
ACTION=deploy
ROOT=
RELEASE_ID=
SOURCE_DIR=
TARGET_RELEASE_ID=

usage() {
    cat <<'EOF'
Usage:
  deploy-linux.sh --root ROOT --release-id ID --source-dir DIR
  deploy-linux.sh --root ROOT --action rollback --target-release-id ID

ROOT is an isolated release root containing releases/, current, state/, and journal.json.
Application configuration, state, and logs remain under ROOT/state and never under a release.
EOF
}

die() { echo "deploy-linux: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --root) ROOT=${2:-}; shift 2 ;;
        --release-id) RELEASE_ID=${2:-}; shift 2 ;;
        --source-dir) SOURCE_DIR=${2:-}; shift 2 ;;
        --action) ACTION=${2:-}; shift 2 ;;
        --target-release-id) TARGET_RELEASE_ID=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ $ACTION == deploy || $ACTION == rollback ]] || die "action must be deploy or rollback"
[[ -n $ROOT ]] || die "--root is required"
ROOT=$(realpath -m -- "$ROOT")
[[ $ROOT != / && $ROOT != /usr && $ROOT != /usr/local && $ROOT != /opt ]] || die "unsafe release root: $ROOT"
validate_id() {
    [[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ && $1 != *..* ]] || die "unsafe release id: $1"
}
if [[ $ACTION == deploy ]]; then
    [[ -n $RELEASE_ID && -n $SOURCE_DIR ]] || die "deploy requires --release-id and --source-dir"
    validate_id "$RELEASE_ID"
    SOURCE_DIR=$(realpath -- "$SOURCE_DIR")
    [[ -x $SOURCE_DIR/pm_tiny && -x $SOURCE_DIR/pm ]] || die "source directory must contain executable pm_tiny and pm"
else
    [[ -n $TARGET_RELEASE_ID ]] || die "rollback requires --target-release-id"
    validate_id "$TARGET_RELEASE_ID"
fi

RELEASES="$ROOT/releases"
STATE="$ROOT/state"
CURRENT="$ROOT/current"
JOURNAL="$ROOT/journal.json"
mkdir -p "$RELEASES" "$STATE"

current_id() {
    if [[ -L $CURRENT ]]; then basename -- "$(readlink -- "$CURRENT")"; fi
}

write_journal() {
    local phase=$1 old_id=$2 new_id=$3 temporary="$JOURNAL.tmp.$$"
    python3 - "$temporary" "$phase" "$old_id" "$new_id" <<'PY'
import json, os, sys
path, phase, old_id, new_id = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({"schema_version": 1, "phase": phase, "old_release": old_id,
               "new_release": new_id}, stream, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
PY
    mv -f -- "$temporary" "$JOURNAL"
}

switch_current() {
    local release_id=$1 temporary="$ROOT/.current.$$"
    ln -s "releases/$release_id" "$temporary"
    mv -Tf -- "$temporary" "$CURRENT"
}

clear_current() {
    if [[ -L $CURRENT ]]; then rm -f -- "$CURRENT"; fi
}

restore_old() {
    local old_id=$1
    if [[ -n $old_id ]]; then switch_current "$old_id"; else clear_current; fi
}

recover_journal() {
    [[ -f $JOURNAL ]] || return 0
    local values phase old_id new_id
    values=$(python3 - "$JOURNAL" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
if data.get("schema_version") != 1:
    raise SystemExit("unsupported release journal")
for key in ("phase", "old_release", "new_release"):
    print(str(data.get(key, "")))
PY
) || die "cannot read release journal"
    phase=$(sed -n '1p' <<<"$values")
    old_id=$(sed -n '2p' <<<"$values")
    new_id=$(sed -n '3p' <<<"$values")
    [[ -z $old_id ]] || validate_id "$old_id"
    [[ -z $new_id ]] || validate_id "$new_id"
    case "$phase" in
        prepared) ;;
        switched) restore_old "$old_id" ;;
        *) die "unsupported release journal phase: $phase" ;;
    esac
    rm -f -- "$JOURNAL"
    echo "recovered interrupted transaction: $new_id -> ${old_id:-none}"
}

health_check() {
    local release_dir=$1 tag=$2 daemon_pid=
    local health_root="$STATE/health-$tag-$$"
    local health_socket="${TMPDIR:-/tmp}/pm_tiny-release-$$-$RANDOM.sock"
    mkdir -p "$health_root"
    printf '[]\n' > "$health_root/prog.yaml"
    printf '%s\n' \
        'pm_tiny_prog_cfg_file: prog.yaml' \
        'pm_tiny_log_file: daemon.log' \
        'pm_tiny_app_log_dir: logs' \
        'pm_tiny_app_environ_dir: environ' > "$health_root/pm_tiny.yaml"
    PM_TINY_HOME="$health_root" PM_TINY_SOCK_FILE="$health_socket" \
        PM_TINY_PROCESS_TREE_MODE=process_group \
        "$release_dir/pm_tiny" --home "$health_root" --config "$health_root/pm_tiny.yaml" &
    daemon_pid=$!
    local ready=0
    for _ in $(seq 1 100); do
        if [[ -S $health_socket ]] && PM_TINY_HOME="$health_root" PM_TINY_SOCK_FILE="$health_socket" \
            "$release_dir/pm" info --json > "$health_root/info.json" 2>/dev/null; then
            ready=1
            break
        fi
        kill -0 "$daemon_pid" 2>/dev/null || break
        sleep 0.05
    done
    local result=0
    if [[ $ready -ne 1 ]] || ! python3 - "$health_root/info.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    data = json.load(stream)
assert data["identity"]["protocol_version"] == 3
assert data["runtime"]["state"] == "running"
PY
    then
        result=1
    fi
    PM_TINY_HOME="$health_root" PM_TINY_SOCK_FILE="$health_socket" \
        "$release_dir/pm" quit >/dev/null 2>&1 || kill "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
    rm -f -- "$health_socket"
    rm -rf -- "$health_root"
    return "$result"
}

recover_journal
OLD_ID=$(current_id || true)

if [[ $ACTION == deploy ]]; then
    TARGET="$RELEASES/$RELEASE_ID"
    [[ ! -e $TARGET ]] || die "release already exists: $RELEASE_ID"
    STAGING="$ROOT/.staging-$RELEASE_ID-$$"
    mkdir -p "$STAGING"
    trap 'if [[ -n ${STAGING:-} && -d $STAGING ]]; then rm -rf -- "$STAGING"; fi' EXIT
    cp -p -- "$SOURCE_DIR/pm_tiny" "$SOURCE_DIR/pm" "$STAGING/"
    VERSION=$("$STAGING/pm_tiny" --version | awk '/pm_tiny/ {print $2; exit}')
    [[ -n $VERSION ]] || die "cannot determine pm version"
    python3 "$MANIFEST_TOOL" create --release-dir "$STAGING" --release-id "$RELEASE_ID" \
        --version "$VERSION" --platform linux --arch "$(uname -m)" >/dev/null
    python3 "$MANIFEST_TOOL" verify --release-dir "$STAGING" --release-id "$RELEASE_ID" \
        --platform linux >/dev/null
    "$STAGING/pm_tiny" --version | grep -Fq "$VERSION" || die "daemon version probe failed"
    health_check "$STAGING" pre-switch || die "pre-switch health check failed"
    if [[ ${PM_TINY_RELEASE_TEST_FAIL_BEFORE_SWITCH:-0} == 1 ]]; then
        die "injected pre-switch failure"
    fi
    mv -- "$STAGING" "$TARGET"
    STAGING=
    NEW_ID=$RELEASE_ID
else
    NEW_ID=$TARGET_RELEASE_ID
    TARGET="$RELEASES/$NEW_ID"
    [[ -d $TARGET ]] || die "rollback target does not exist: $NEW_ID"
    python3 "$MANIFEST_TOOL" verify --release-dir "$TARGET" --release-id "$NEW_ID" --platform linux >/dev/null
    health_check "$TARGET" pre-rollback || die "rollback target health check failed"
fi

[[ $OLD_ID != "$NEW_ID" ]] || die "release is already current: $NEW_ID"
write_journal prepared "$OLD_ID" "$NEW_ID"
switch_current "$NEW_ID"
write_journal switched "$OLD_ID" "$NEW_ID"

if [[ ${PM_TINY_RELEASE_TEST_INTERRUPT_AFTER_SWITCH:-0} == 1 ]]; then
    echo "injected interruption after switch" >&2
    exit 91
fi

if [[ ${PM_TINY_RELEASE_TEST_FAIL_AFTER_SWITCH:-0} == 1 ]] || ! health_check "$CURRENT" post-switch; then
    restore_old "$OLD_ID"
    rm -f -- "$JOURNAL"
    die "post-switch health check failed; restored ${OLD_ID:-no release}"
fi
rm -f -- "$JOURNAL"
echo "current release: $NEW_ID"
