#!/usr/bin/env bash
set -euo pipefail

BIN=${PM_TINY_TEST_BIN:?PM_TINY_TEST_BIN is required}
SOURCE_DIR=${PM_TINY_SOURCE_DIR:?PM_TINY_SOURCE_DIR is required}
if [[ -n ${PM_TINY_RELEASE_ARTIFACT_DIR:-} ]]; then
    mkdir -p "$PM_TINY_RELEASE_ARTIFACT_DIR"
    TEST_ROOT="$PM_TINY_RELEASE_ARTIFACT_DIR/run-$$"
    mkdir "$TEST_ROOT"
else
    TEST_ROOT=$(mktemp -d /tmp/pm_tiny_release_integration.XXXXXX)
fi
DEPLOY="$SOURCE_DIR/scripts/release/deploy-linux.sh"

cleanup() {
    local status=$?
    if [[ $status -eq 0 ]]; then
        rm -rf -- "$TEST_ROOT"
    else
        echo "release transaction artifacts: $TEST_ROOT" >&2
    fi
}
trap cleanup EXIT

ROOT="$TEST_ROOT/install"
mkdir -p "$ROOT/state/config" "$ROOT/state/logs"
printf 'keep-config\n' > "$ROOT/state/config/sentinel"
printf 'keep-log\n' > "$ROOT/state/logs/sentinel"

current_id() { basename -- "$(readlink -- "$ROOT/current")"; }
assert_state_isolated() {
    [[ $(cat "$ROOT/state/config/sentinel") == keep-config ]]
    [[ $(cat "$ROOT/state/logs/sentinel") == keep-log ]]
    [[ ! -e "$ROOT/current/state" ]]
}

"$DEPLOY" --root "$ROOT" --release-id release-a --source-dir "$BIN"
[[ $(current_id) == release-a ]]
assert_state_isolated

if PM_TINY_RELEASE_TEST_FAIL_BEFORE_SWITCH=1 \
    "$DEPLOY" --root "$ROOT" --release-id release-before-fail --source-dir "$BIN"; then
    echo "pre-switch failure injection unexpectedly succeeded" >&2
    exit 1
fi
[[ $(current_id) == release-a && ! -f $ROOT/journal.json ]]

"$DEPLOY" --root "$ROOT" --release-id release-b --source-dir "$BIN"
[[ $(current_id) == release-b ]]

if PM_TINY_RELEASE_TEST_FAIL_AFTER_SWITCH=1 \
    "$DEPLOY" --root "$ROOT" --release-id release-after-fail --source-dir "$BIN"; then
    echo "post-switch failure injection unexpectedly succeeded" >&2
    exit 1
fi
[[ $(current_id) == release-b && ! -f $ROOT/journal.json ]]

set +e
PM_TINY_RELEASE_TEST_INTERRUPT_AFTER_SWITCH=1 \
    "$DEPLOY" --root "$ROOT" --release-id release-interrupted --source-dir "$BIN"
interrupt_status=$?
set -e
[[ $interrupt_status -eq 91 && $(current_id) == release-interrupted && -f $ROOT/journal.json ]]

"$DEPLOY" --root "$ROOT" --action rollback --target-release-id release-a
[[ $(current_id) == release-a && ! -f $ROOT/journal.json ]]
assert_state_isolated

cp "$ROOT/releases/release-a/pm" "$ROOT/releases/release-a/pm.tampered"
if python3 "$SOURCE_DIR/scripts/release/release_manifest.py" verify \
    --release-dir "$ROOT/releases/release-a" --release-id release-a --platform linux; then
    echo "manifest verification accepted an untracked file" >&2
    exit 1
fi
rm -f -- "$ROOT/releases/release-a/pm.tampered"
python3 "$SOURCE_DIR/scripts/release/release_manifest.py" verify \
    --release-dir "$ROOT/releases/release-a" --release-id release-a --platform linux >/dev/null
ln -s /etc/passwd "$ROOT/releases/release-a/escape"
if python3 "$SOURCE_DIR/scripts/release/release_manifest.py" verify \
    --release-dir "$ROOT/releases/release-a" --release-id release-a --platform linux; then
    echo "manifest verification accepted a symlink" >&2
    exit 1
fi
rm -f -- "$ROOT/releases/release-a/escape"

echo "linux release transaction integration: PASS"
