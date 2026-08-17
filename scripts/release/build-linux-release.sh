#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR=${PM_TINY_LINUX_RELEASE_BUILD_DIR:-"$ROOT/build-release-linux-x86_64"}
OUTPUT_DIR=${PM_TINY_LINUX_RELEASE_OUTPUT_DIR:-"$BUILD_DIR/release-bin"}
JOBS=${PM_TINY_BUILD_JOBS:-4}

for tool in cmake c++ strip readelf; do
    command -v "$tool" >/dev/null || { echo "missing Linux release tool: $tool" >&2; exit 1; }
done

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPM_TINY_BUILD_TESTS=ON \
    -DPM_TINY_SANITIZER_ENABLE=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build "$BUILD_DIR" --parallel "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure

mkdir -p "$OUTPUT_DIR"
for name in pm_tiny pm; do
    cp "$BUILD_DIR/$name" "$OUTPUT_DIR/$name"
    strip "$OUTPUT_DIR/$name"
    "$OUTPUT_DIR/$name" --version
done

printf 'compiler=%s\ncmake=%s\n' \
    "$(c++ --version | head -n 1)" "$(cmake --version | head -n 1)" \
    > "$OUTPUT_DIR/toolchain.txt"
printf 'linux release binaries: %s\n' "$OUTPUT_DIR"
