#!/bin/bash
BUILD_DIR=".ax_build"
rm -rf ${BUILD_DIR}
mkdir ${BUILD_DIR} && cd ${BUILD_DIR}
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchains/ax620a.toolchain.cmake ..
cmake --build . --target pm_tiny --target pm --target pm_sdk
cmake --install .

