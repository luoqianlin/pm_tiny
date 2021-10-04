#!/bin/bash
BUILD_DIR=".build_android"
rm -rf ${BUILD_DIR}
mkdir ${BUILD_DIR} && cd ${BUILD_DIR}

export ANDROID_NDK=~/Android/Sdk/ndk/25.1.8937393
cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
 -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=30 -DPM_TINY_SANITIZER_ENABLE=OFF -DANDROID_STL=c++_shared ..

cmake --build . --target pm_tiny --target pm --target pm_sdk
cmake --install .