cmake_minimum_required(VERSION 3.10)
project(pm_tiny)

set(CMAKE_CXX_STANDARD 14)

add_definitions(
        -DPM_TINY_VERSION="1.0.1"
)

add_executable(pm_tiny src/pm_tiny.cpp src/pm_tiny_server.cpp
        src/prog.cpp
        src/logger.hpp
        src/signal_util.cpp
        src/session.cpp
        src/session.h
        src/pm_sys.cpp
        src/pm_sys.h src/pm_tiny.h
        src/pm_tiny_helper.cpp src/procinfo.cpp
        src/log.h src/time_util.cpp src/log.cpp src/string_utils.cpp
        src/globals.cpp
        )

target_compile_definitions(pm_tiny
        PUBLIC PM_TINY_SERVER=1
        PM_TINY_PTY_ENABLE=1
        )

add_executable(pm src/pm.cpp src/session.cpp
        src/pm_sys.cpp src/pm_tiny_helper.cpp
        src/frame_stream.hpp
        src/time_util.cpp
        src/log.cpp src/memory_util.h src/memory_util.cpp
        src/globals.cpp
        )

add_executable(string_utils_test
        test/string_utils_test.cpp
        src/string_utils.cpp)
