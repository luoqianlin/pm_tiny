#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR=${PM_TINY_COVERAGE_BUILD_DIR:-"$ROOT/build-coverage"}
ARTIFACT_DIR=${PM_TINY_COVERAGE_ARTIFACT_DIR:-"$ROOT/build/test-artifacts/coverage"}
PROFILE_DIR="$BUILD_DIR/profiles"

for tool in clang clang++ llvm-profdata llvm-cov; do
    command -v "$tool" >/dev/null || { echo "missing coverage tool: $tool" >&2; exit 1; }
done

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DPM_TINY_BUILD_TESTS=ON \
    -DPM_TINY_ENABLE_COVERAGE=ON \
    -DPM_TINY_SANITIZER_ENABLE=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build "$BUILD_DIR" --parallel "${PM_TINY_BUILD_JOBS:-4}"
rm -rf "$PROFILE_DIR" "$ARTIFACT_DIR"
mkdir -p "$PROFILE_DIR" "$ARTIFACT_DIR/html"
LLVM_PROFILE_FILE="$PROFILE_DIR/%p-%m.profraw" \
    cmake -E chdir "$BUILD_DIR" ctest --output-on-failure
llvm-profdata merge -sparse "$PROFILE_DIR"/*.profraw -o "$ARTIFACT_DIR/coverage.profdata"

mapfile -t OBJECTS < <(find "$BUILD_DIR" -type f -perm -111 -print0 | \
    xargs -0 file | awk -F: '/ELF .*executable/ {print $1}' | sort -u)
[[ ${#OBJECTS[@]} -gt 0 ]] || { echo 'no instrumented coverage objects found' >&2; exit 1; }
OBJECT_ARGS=()
for object in "${OBJECTS[@]:1}"; do OBJECT_ARGS+=("-object=$object"); done
IGNORE='(^|/)(dependencies|tests)(/|$)'
llvm-cov report "${OBJECTS[0]}" "${OBJECT_ARGS[@]}" \
    -instr-profile="$ARTIFACT_DIR/coverage.profdata" \
    -ignore-filename-regex="$IGNORE" | tee "$ARTIFACT_DIR/summary.txt"
llvm-cov show "${OBJECTS[0]}" "${OBJECT_ARGS[@]}" \
    -instr-profile="$ARTIFACT_DIR/coverage.profdata" \
    -ignore-filename-regex="$IGNORE" -format=html -output-dir="$ARTIFACT_DIR/html"
llvm-cov export "${OBJECTS[0]}" "${OBJECT_ARGS[@]}" \
    -instr-profile="$ARTIFACT_DIR/coverage.profdata" \
    -ignore-filename-regex="$IGNORE" -format=lcov > "$ARTIFACT_DIR/coverage.lcov"
echo "coverage artifacts: $ARTIFACT_DIR"
