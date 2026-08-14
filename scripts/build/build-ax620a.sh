#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR="${PROJECT_ROOT}/build-ax620a"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${PROJECT_ROOT}/toolchains/ax620a.toolchain.cmake" \
    -DPM_TINY_BUILD_TESTS=OFF
cmake --build "${BUILD_DIR}" --target pm_tiny pm pm_sdk --parallel
cmake --install "${BUILD_DIR}"
