cmake_minimum_required(VERSION 3.10)
if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()
project(pm_tiny VERSION 1.1.3 LANGUAGES C CXX)
message(STATUS "PROJECT_VERSION:${PROJECT_VERSION}")
message(STATUS "CMAKE_SYSTEM_NAME:${CMAKE_SYSTEM_NAME}")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_INSTALL_PREFIX ${PROJECT_BINARY_DIR}/_install)
option(PM_TINY_SANITIZER_ENABLE "enable sanitizer" OFF)
option(PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT_ENABLE
        "Enable the use of Unix Domain Socket's abstract namespace by default" OFF)

add_compile_options(-Wall -Wextra)

if ("${CMAKE_BUILD_TYPE}" STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release")
endif ()

if ("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    set(PM_TINY_SANITIZER_ENABLE OFF)
endif ()
message(STATUS "PM_TINY_SANITIZER_ENABLE:${PM_TINY_SANITIZER_ENABLE}")

include(FetchContent)

FetchContent_Declare(yaml-cpp
        URL https://files.sansi.net/libs/yaml-cpp-yaml-cpp-0.7.0.tar.gz
        DOWNLOAD_DIR ${CMAKE_HOME_DIRECTORY}/dependencies
)
FetchContent_MakeAvailable(yaml-cpp)

add_executable(pm_tiny src/pm_tiny.cpp src/pm_tiny_server.cpp
        src/prog.cpp
        src/prog_cfg.cpp
        src/logger.hpp
        src/signal_util.cpp
        src/session.cpp
        src/session.h
        src/pm_sys.cpp
        src/pm_sys.h src/pm_tiny.h
        src/pm_tiny_helper.cpp src/procinfo.cpp
        src/log.h src/time_util.cpp src/log.cpp src/string_utils.cpp
        src/globals.cpp
        src/pm_tiny_funcs.cpp
        src/android_lmkd.cpp
        src/pm_tiny_utility.cpp
)

add_compile_definitions(PM_TINY_VERSION="${PROJECT_VERSION}" PM_TINY_DEBUG)

target_compile_definitions(pm_tiny
        PRIVATE PM_TINY_SERVER=1
        PM_TINY_PTY_ENABLE=1
)

message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

target_link_libraries(
        pm_tiny
        PRIVATE yaml-cpp
)

if (PM_TINY_SANITIZER_ENABLE)
    target_compile_options(pm_tiny PRIVATE -fsanitize=address -fsanitize=undefined)
    target_link_libraries(pm_tiny PRIVATE asan ubsan)
endif ()

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set(PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT_ENABLE ON)
endif ()

if (PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT_ENABLE)
    message(STATUS "Enable the use of Unix Domain Socket's abstract namespace by default")
    add_compile_definitions(PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT)
endif ()

get_target_property(PM_TINY_COMPILE_OPTIONS pm_tiny "COMPILE_OPTIONS")
get_target_property(PM_TINY_LINK_LIBRARIES pm_tiny "LINK_LIBRARIES")
message(STATUS "PM_TINY_COMPILE_OPTIONS:${PM_TINY_COMPILE_OPTIONS}")
message(STATUS "PM_TINY_LINK_LIBRARIES:${PM_TINY_LINK_LIBRARIES}")

add_executable(pm src/pm.cpp src/session.cpp
        src/pm_sys.cpp src/pm_tiny_helper.cpp
        src/frame_stream.hpp
        src/time_util.cpp
        src/log.cpp src/memory_util.h src/memory_util.cpp
        src/globals.cpp
        src/pm_funcs.cpp
)

target_include_directories(pm PRIVATE libs/tabulate-1.5/include)
target_link_libraries(
        pm
        PRIVATE yaml-cpp
)
if (PM_TINY_SANITIZER_ENABLE)
    target_compile_options(pm PRIVATE -fsanitize=address -fsanitize=undefined)
    target_link_libraries(pm PRIVATE asan ubsan)
endif ()


add_subdirectory(sdk)
#add_subdirectory(test)

install(TARGETS pm_tiny pm
        CONFIGURATIONS Release
        PUBLIC_HEADER DESTINATION Release/include
        RUNTIME DESTINATION Release/bin
        LIBRARY DESTINATION Release/lib
        ARCHIVE DESTINATION Release/lib/static)

install(TARGETS pm_tiny pm
        CONFIGURATIONS Debug
        PUBLIC_HEADER DESTINATION Debug/include
        RUNTIME DESTINATION Debug/bin
        LIBRARY DESTINATION Debug/lib
        ARCHIVE DESTINATION Debug/lib/static)
