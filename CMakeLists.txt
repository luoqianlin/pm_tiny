cmake_minimum_required(VERSION 3.10)
project(pm_tiny)

set(CMAKE_CXX_STANDARD 14)

add_executable(pm_tiny src/pm_tiny.cpp
        src/logger.hpp
        src/signal_util.cpp
        src/session.cpp
        src/session.h
        src/pm_sys.cpp
        src/pm_sys.h src/pm_tiny.h
        src/pm_tiny_helper.cpp)

add_executable(pm src/pm.cpp src/session.cpp
        src/pm_sys.cpp src/pm_tiny_helper.cpp
        src/frame_stream.hpp)
