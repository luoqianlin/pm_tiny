#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR=${PM_TINY_ANDROID_RELEASE_BUILD_DIR:-"$ROOT/build-release-android-arm64-v8a"}
OUTPUT_DIR=${PM_TINY_ANDROID_RELEASE_OUTPUT_DIR:-"$BUILD_DIR/release-bin"}
NDK_DIR=${PM_TINY_ANDROID_NDK:-${ANDROID_NDK:-${HOME}/Android/Sdk/ndk/25.1.8937393}}
EXPECTED_NDK=25.1.8937393
JOBS=${PM_TINY_BUILD_JOBS:-4}

[[ -f "$NDK_DIR/source.properties" ]] || { echo "Android NDK not found: $NDK_DIR" >&2; exit 1; }
grep -q "Pkg.Revision = $EXPECTED_NDK" "$NDK_DIR/source.properties" || {
    echo "Android release requires NDK $EXPECTED_NDK" >&2
    exit 1
}
READELF="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
STRIP="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK_DIR/build/cmake/android.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-30 \
    -DANDROID_STL=c++_static \
    -DCMAKE_ANDROID_STL_TYPE=c++_static \
    -DPM_TINY_BUILD_TESTS=OFF \
    -DPM_TINY_SANITIZER_ENABLE=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build "$BUILD_DIR" --target pm_tiny pm pm_sdk pm_sdk_ready_tick_probe --parallel "$JOBS"

test -x "$BUILD_DIR/pm_tiny"
test -x "$BUILD_DIR/pm2"
test ! -e "$BUILD_DIR/pm"
mkdir -p "$OUTPUT_DIR"
for name in pm_tiny pm2; do
    cp "$BUILD_DIR/$name" "$OUTPUT_DIR/$name"
    "$STRIP" "$OUTPUT_DIR/$name"
    ! "$READELF" -d "$OUTPUT_DIR/$name" | grep -F libc++_shared.so
done
printf 'ndk=%s\nclang=%s\nandroid_api=30\n' "$EXPECTED_NDK" \
    "$("$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" --version | head -n 1)" \
    > "$OUTPUT_DIR/toolchain.txt"
printf 'android release binaries: %s\n' "$OUTPUT_DIR"
