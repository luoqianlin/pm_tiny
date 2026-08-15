#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR="${PROJECT_ROOT}/build-android-arm64"
ANDROID_NDK_DIR=${PM_TINY_ANDROID_NDK:-${ANDROID_NDK:-${HOME}/Android/Sdk/ndk/25.1.8937393}}

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_DIR}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=30 \
    -DANDROID_STL=c++_static \
    -DPM_TINY_BUILD_TESTS=OFF \
    -DPM_TINY_SANITIZER_ENABLE=OFF
cmake --build "${BUILD_DIR}" --target pm_tiny pm pm_sdk pm_sdk_ready_tick_probe --parallel
cmake --install "${BUILD_DIR}"
