#!/bin/bash
BUILD_DIR=".build"
rm -rf ${BUILD_DIR}
mkdir ${BUILD_DIR} && cd ${BUILD_DIR}
cmake -DPM_TINY_SANITIZER_ENABLE=OFF -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . --target pm_tiny --target pm --target pm_sdk
cmake --install .

