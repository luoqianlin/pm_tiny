#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR="${PROJECT_ROOT}/build-linux-debug"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPM_TINY_BUILD_TESTS=ON \
    -DPM_TINY_SANITIZER_ENABLE=OFF
cmake --build "${BUILD_DIR}" --target pm_tiny pm pm_sdk --parallel
cmake --install "${BUILD_DIR}"
