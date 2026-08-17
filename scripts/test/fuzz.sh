#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODE=${1:-smoke}
ENGINE=${2:-all}
case "$MODE" in
    smoke) DURATION=${PM_TINY_FUZZ_DURATION:-30} ;;
    nightly) DURATION=${PM_TINY_FUZZ_DURATION:-600} ;;
    *) echo "usage: $0 [smoke|nightly] [all|libfuzzer|afl]" >&2; exit 2 ;;
esac
[[ $ENGINE == all || $ENGINE == libfuzzer || $ENGINE == afl ]] || {
    echo "usage: $0 [smoke|nightly] [all|libfuzzer|afl]" >&2
    exit 2
}

RUN_TAG=$(date +%Y%m%d-%H%M%S)-$$
ARTIFACT_ROOT=${PM_TINY_FUZZ_ARTIFACT_DIR:-"$ROOT/build/test-artifacts/fuzz/$RUN_TAG"}
mkdir -p "$ARTIFACT_ROOT"
ARTIFACT_ROOT=$(cd "$ARTIFACT_ROOT" && pwd)

run_libfuzzer() {
    command -v clang++ >/dev/null || { echo 'missing clang++' >&2; return 1; }
    local build="$ROOT/build-fuzz-libfuzzer"
    cmake -S "$ROOT" -B "$build" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DPM_TINY_BUILD_TESTS=OFF -DPM_TINY_BUILD_FUZZERS=ON \
        -DPM_TINY_FUZZ_ENGINE=libfuzzer -DFETCHCONTENT_FULLY_DISCONNECTED=ON
    cmake --build "$build" --parallel "${PM_TINY_BUILD_JOBS:-4}"
    for subject in protocol config log; do
        local artifacts="$ARTIFACT_ROOT/libfuzzer-$subject"
        local corpus="$artifacts/corpus"
        mkdir -p "$corpus" "$artifacts/crashes"
        cp -a "$ROOT/tests/fuzz/corpus/$subject/." "$corpus/"
        local dictionary=()
        if [[ $subject == protocol ]]; then
            dictionary=("-dict=$ROOT/tests/fuzz/protocol.dict")
        fi
        ASAN_OPTIONS=symbolize=0:abort_on_error=1 \
        "$build/tests/fuzz/pm_tiny_fuzz_$subject" \
            -max_total_time="$DURATION" -timeout=5 -rss_limit_mb=2048 \
            -artifact_prefix="$artifacts/crashes/" "${dictionary[@]}" "$corpus"
    done
}

run_afl() {
    for tool in afl-clang-fast afl-clang-fast++ afl-fuzz; do
        command -v "$tool" >/dev/null || { echo "missing AFL++ tool: $tool" >&2; return 1; }
    done
    local build="$ROOT/build-fuzz-afl"
    CC=afl-clang-fast CXX=afl-clang-fast++ cmake -S "$ROOT" -B "$build" \
        -DCMAKE_BUILD_TYPE=Debug -DPM_TINY_BUILD_TESTS=OFF \
        -DPM_TINY_BUILD_FUZZERS=ON -DPM_TINY_FUZZ_ENGINE=afl \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
    cmake --build "$build" --parallel "${PM_TINY_BUILD_JOBS:-4}"
    for subject in protocol config log; do
        local output="$ARTIFACT_ROOT/afl-$subject"
        ASAN_OPTIONS=symbolize=0:abort_on_error=1 \
        AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
            afl-fuzz -V "$DURATION" -i "$ROOT/tests/fuzz/corpus/$subject" -o "$output" -- \
            "$build/tests/fuzz/pm_tiny_afl_$subject" @@
        tar -czf "$ARTIFACT_ROOT/afl-$subject.tar.gz" \
            -C "$ARTIFACT_ROOT" "afl-$subject"
        if find "$output" -path '*/crashes/id:*' -type f -print -quit | grep -q .; then
            echo "AFL++ found a crash for $subject" >&2
            return 1
        fi
    done
}

[[ $ENGINE == all || $ENGINE == libfuzzer ]] && run_libfuzzer
[[ $ENGINE == all || $ENGINE == afl ]] && run_afl
echo "fuzz artifacts: $ARTIFACT_ROOT"
