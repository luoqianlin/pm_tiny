if (CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.0")
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5...4.0")
endif ()

cmake_minimum_required(VERSION 3.10)
if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set(ANDROID_STL c++_static CACHE STRING
            "Android C++ runtime linkage" FORCE)
    set(CMAKE_ANDROID_STL_TYPE c++_static CACHE STRING
            "Android C++ runtime linkage" FORCE)
endif ()
project(pm_tiny VERSION 4.1.0 LANGUAGES C CXX)
message(STATUS "PROJECT_VERSION:${PROJECT_VERSION}")
message(STATUS "CMAKE_SYSTEM_NAME:${CMAKE_SYSTEM_NAME}")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_INSTALL_PREFIX ${PROJECT_BINARY_DIR}/_install)
option(PM_TINY_SANITIZER_ENABLE "enable sanitizer" OFF)
option(PM_TINY_ENABLE_COVERAGE "enable Clang source-based coverage" OFF)
option(PM_TINY_BUILD_FUZZERS "build protocol/config/log fuzz targets" OFF)
set(PM_TINY_FUZZ_ENGINE "libfuzzer" CACHE STRING "fuzz engine: libfuzzer or afl")
set_property(CACHE PM_TINY_FUZZ_ENGINE PROPERTY STRINGS libfuzzer afl)
option(PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT_ENABLE
        "Enable the use of Unix Domain Socket's abstract namespace by default" OFF)
option(PM_TINY_BUILD_TESTS "Build pm_tiny tests" ON)

if (MSVC)
    add_compile_options(/W4 /utf-8)
else ()
    add_compile_options(-Wall -Wextra)
endif ()

if ("${CMAKE_BUILD_TYPE}" STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release")
endif ()

if (PM_TINY_SANITIZER_ENABLE AND MSVC)
    message(FATAL_ERROR "PM_TINY_SANITIZER_ENABLE currently requires GCC or Clang")
endif ()
if ("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    set(PM_TINY_SANITIZER_ENABLE OFF)
endif ()
if (PM_TINY_SANITIZER_ENABLE)
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address -fsanitize=undefined")
endif ()
if (PM_TINY_ENABLE_COVERAGE)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PM_TINY_ENABLE_COVERAGE requires Clang")
    endif ()
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fprofile-instr-generate -fcoverage-mapping")
endif ()
if (PM_TINY_BUILD_FUZZERS)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PM_TINY_BUILD_FUZZERS requires Clang or an AFL++ Clang wrapper")
    endif ()
    if (NOT PM_TINY_FUZZ_ENGINE STREQUAL "libfuzzer" AND
        NOT PM_TINY_FUZZ_ENGINE STREQUAL "afl")
        message(FATAL_ERROR "PM_TINY_FUZZ_ENGINE must be libfuzzer or afl")
    endif ()
    if (PM_TINY_FUZZ_ENGINE STREQUAL "libfuzzer")
        add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    else ()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    endif ()
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address,undefined")
endif ()
message(STATUS "PM_TINY_SANITIZER_ENABLE:${PM_TINY_SANITIZER_ENABLE}")

set(PM_TINY_ON_WINDOWS FALSE)
if (WIN32)
    set(PM_TINY_ON_WINDOWS TRUE)
    message(STATUS "Configuring Windows build path.")
endif ()

find_package(Threads REQUIRED)

set(YAML_CPP_LOCAL_DIR "${CMAKE_SOURCE_DIR}/dependencies/yaml-cpp")
if (NOT EXISTS "${YAML_CPP_LOCAL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Bundled yaml-cpp 0.9.0 is missing from ${YAML_CPP_LOCAL_DIR}")
endif ()
file(READ "${YAML_CPP_LOCAL_DIR}/CMakeLists.txt" YAML_CPP_CMAKE_FILE)
if (NOT YAML_CPP_CMAKE_FILE MATCHES "project\\(YAML_CPP VERSION 0\\.9\\.0")
    message(FATAL_ERROR "Bundled yaml-cpp must be version 0.9.0")
endif ()
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
message(STATUS "Using bundled yaml-cpp 0.9.0 from ${YAML_CPP_LOCAL_DIR}")
add_subdirectory("${YAML_CPP_LOCAL_DIR}"
        "${CMAKE_BINARY_DIR}/_deps/yaml-cpp-build" EXCLUDE_FROM_ALL)

set(ASIO_LOCAL_DIR "${CMAKE_SOURCE_DIR}/dependencies/asio")
if (NOT EXISTS "${ASIO_LOCAL_DIR}/asio.hpp" OR
    NOT EXISTS "${ASIO_LOCAL_DIR}/asio/version.hpp")
    message(FATAL_ERROR "Bundled Asio 1.30.2 is missing from ${ASIO_LOCAL_DIR}")
endif ()
file(READ "${ASIO_LOCAL_DIR}/asio/version.hpp" ASIO_VERSION_HEADER)
if (NOT ASIO_VERSION_HEADER MATCHES "#define ASIO_VERSION 103002")
    message(FATAL_ERROR "${ASIO_LOCAL_DIR} must contain Asio 1.30.2 (ASIO_VERSION 103002)")
endif ()
set(PM_TINY_ASIO_INCLUDE_DIR "${ASIO_LOCAL_DIR}")
add_library(pm_tiny_asio INTERFACE)
target_include_directories(pm_tiny_asio INTERFACE "${PM_TINY_ASIO_INCLUDE_DIR}")
target_compile_definitions(pm_tiny_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)

set(NLOHMANN_JSON_DIR "${CMAKE_SOURCE_DIR}/dependencies/nlohmann-json")
set(LIBFORT_DIR "${CMAKE_SOURCE_DIR}/dependencies/libfort")
set(UTF8PROC_DIR "${CMAKE_SOURCE_DIR}/dependencies/utf8proc")
foreach(PM_TINY_DEP_DIR IN ITEMS NLOHMANN_JSON_DIR LIBFORT_DIR UTF8PROC_DIR)
    if (NOT EXISTS "${${PM_TINY_DEP_DIR}}")
        message(FATAL_ERROR "Bundled dependency is missing: ${${PM_TINY_DEP_DIR}}")
    endif ()
endforeach()
file(READ "${NLOHMANN_JSON_DIR}/single_include/nlohmann/json.hpp" NLOHMANN_JSON_HEADER)
if (NOT NLOHMANN_JSON_HEADER MATCHES "#define NLOHMANN_JSON_VERSION_MAJOR 3" OR
    NOT NLOHMANN_JSON_HEADER MATCHES "#define NLOHMANN_JSON_VERSION_MINOR 12" OR
    NOT NLOHMANN_JSON_HEADER MATCHES "#define NLOHMANN_JSON_VERSION_PATCH 0")
    message(FATAL_ERROR "Bundled nlohmann/json must be version 3.12.0")
endif ()
file(READ "${LIBFORT_DIR}/lib/fort.h" LIBFORT_HEADER)
if (NOT LIBFORT_HEADER MATCHES "LIBFORT_VERSION_STR \"0.4.2\"")
    message(FATAL_ERROR "Bundled libfort must be version 0.4.2")
endif ()
file(READ "${UTF8PROC_DIR}/utf8proc.h" UTF8PROC_HEADER)
if (NOT UTF8PROC_HEADER MATCHES "UTF8PROC_VERSION_MAJOR 2" OR
    NOT UTF8PROC_HEADER MATCHES "UTF8PROC_VERSION_MINOR 11" OR
    NOT UTF8PROC_HEADER MATCHES "UTF8PROC_VERSION_PATCH 3")
    message(FATAL_ERROR "Bundled utf8proc must be version 2.11.3")
endif ()

add_library(pm_tiny_fort STATIC "${LIBFORT_DIR}/lib/fort.c")
target_include_directories(pm_tiny_fort PUBLIC "${LIBFORT_DIR}/lib")

add_library(pm_tiny_utf8proc STATIC "${UTF8PROC_DIR}/utf8proc.c")
target_include_directories(pm_tiny_utf8proc PUBLIC "${UTF8PROC_DIR}")
target_compile_definitions(pm_tiny_utf8proc PUBLIC UTF8PROC_STATIC)

add_library(pm_tiny_protocol STATIC src/core/protocol_v3.cpp)
target_include_directories(pm_tiny_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_library(pm_tiny_daemon_config STATIC src/core/daemon_config.cpp)
target_include_directories(pm_tiny_daemon_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(pm_tiny_daemon_config PUBLIC yaml-cpp)
if (PM_TINY_ON_WINDOWS)
    target_compile_definitions(pm_tiny_daemon_config PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif ()

if (PM_TINY_ON_WINDOWS)
    set(PM_TINY_ROTATING_LOG_WRITER_SOURCE src/platform/windows/rotating_log_writer.cpp)
    set(PM_TINY_DAEMON_LOG_PLATFORM_SOURCE)
else ()
    set(PM_TINY_ROTATING_LOG_WRITER_SOURCE src/platform/linux/rotating_log_writer.cpp)
    set(PM_TINY_DAEMON_LOG_PLATFORM_SOURCE src/core/signal_util.cpp)
endif ()
add_library(pm_tiny_logging STATIC
        src/core/program_log.cpp
        src/core/daemon_log.cpp
        ${PM_TINY_DAEMON_LOG_PLATFORM_SOURCE}
        ${PM_TINY_ROTATING_LOG_WRITER_SOURCE})
target_include_directories(pm_tiny_logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
if (PM_TINY_ON_WINDOWS)
    target_compile_definitions(pm_tiny_logging PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif ()

add_library(pm_tiny_runtime STATIC
        src/core/daemon_info.cpp
        src/core/process_list.cpp
        src/core/runtime_snapshot.cpp)
target_include_directories(pm_tiny_runtime PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(pm_tiny_runtime PUBLIC pm_tiny_protocol pm_tiny_daemon_config)

add_library(pm_tiny_cli_output STATIC
        src/cli/daemon_info_renderer.cpp
        src/cli/process_list_renderer.cpp
        src/cli/inspect_renderer.cpp
        src/cli/dependency_graph_renderer.cpp)
target_include_directories(pm_tiny_cli_output PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
        "${NLOHMANN_JSON_DIR}/single_include")
target_link_libraries(pm_tiny_cli_output PUBLIC
        pm_tiny_fort pm_tiny_utf8proc pm_tiny_dependency_graph pm_tiny_runtime pm_tiny_logging)
target_compile_definitions(pm_tiny_cli_output PUBLIC JSON_USE_IMPLICIT_CONVERSIONS=0)

add_library(pm_tiny_cli_connection_error STATIC
        src/cli/connection_error.cpp)
target_include_directories(pm_tiny_cli_connection_error PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)

add_library(pm_tiny_cli_command STATIC
        src/cli/cli_command.cpp
        src/cli/cli_response.cpp)
target_include_directories(pm_tiny_cli_command PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)
target_link_libraries(pm_tiny_cli_command PUBLIC pm_tiny_cli_output pm_tiny_control_command pm_tiny_logging)

add_library(pm_tiny_control_command STATIC
        src/core/control_command.cpp)
target_include_directories(pm_tiny_control_command PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_library(pm_tiny_restart_policy STATIC
        src/core/restart_policy.cpp)
target_include_directories(pm_tiny_restart_policy PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_library(pm_tiny_dependency_graph STATIC src/core/dependency_graph.cpp)
target_include_directories(pm_tiny_dependency_graph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_library(pm_tiny_program_name STATIC src/core/program_name.cpp)
target_include_directories(pm_tiny_program_name PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_library(pm_tiny_lifecycle STATIC
        src/core/control_operation.cpp
        src/core/termination_job.cpp)
target_include_directories(pm_tiny_lifecycle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

if (PM_TINY_ON_WINDOWS)
    add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
    add_executable(pm_tiny
            src/platform/windows/pm_tiny.cpp
            src/platform/windows/win_utils.cpp
            src/platform/windows/win_config_loader.cpp
            src/platform/windows/windows_program_persistence.cpp
            src/platform/windows/process_runner.cpp
            src/platform/windows/control_server.cpp
            src/platform/windows/asio_named_pipe.cpp
            src/core/control_command.cpp
            src/core/restart_policy.cpp
            src/core/persistence_worker.cpp
            src/core/prog_cfg_yaml_helper.cpp
    )
    target_include_directories(pm_tiny
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    target_compile_definitions(pm_tiny PRIVATE
            PM_TINY_VERSION="${PROJECT_VERSION}"
            PM_TINY_WINDOWS_BUILD
            WIN32_LEAN_AND_MEAN
            NOMINMAX)
    if (PM_TINY_BUILD_TESTS)
        target_compile_definitions(pm_tiny PRIVATE PM_TINY_TESTING)
    endif ()
    target_link_libraries(pm_tiny
            PRIVATE yaml-cpp pm_tiny_asio pm_tiny_dependency_graph pm_tiny_program_name pm_tiny_lifecycle
            pm_tiny_protocol pm_tiny_runtime pm_tiny_logging
            pm_tiny_daemon_config
            Threads::Threads
            ws2_32 advapi32 psapi shell32)
else ()
    add_executable(pm_tiny
            src/daemon/pm_tiny.cpp
            src/daemon/pm_tiny_server.cpp
            src/daemon/launch_environment.cpp
            src/daemon/child_launch_context.cpp
            src/daemon/process_reaper.cpp
            src/daemon/asio_daemon_loop.cpp
            src/daemon/prog.cpp
            src/daemon/prog_cfg.cpp
            src/core/session.cpp
            src/core/session.h
            src/core/control_command.cpp
            src/core/restart_policy.cpp
            src/core/persistence_worker.cpp
            src/core/pm_sys.cpp
            src/core/pm_sys.h
            src/core/pm_tiny.h
            src/core/prog_cfg_yaml_helper.cpp
            src/core/time_util.cpp
            src/core/string_utils.cpp
            src/core/string_utils.h
            src/core/globals.cpp
            src/daemon/pm_tiny_funcs.cpp
            src/platform/android/android_lmkd.cpp
            src/core/pm_tiny_utility.cpp
            src/platform/linux/process_tree_controller.cpp
            src/platform/linux/cgroup_fs.cpp
            src/platform/linux/process_group_backend.cpp
            src/platform/linux/cgroup_v2_backend.cpp
    )

    add_compile_definitions(PM_TINY_VERSION="${PROJECT_VERSION}" PM_TINY_DEBUG)

    target_compile_definitions(pm_tiny
            PRIVATE PM_TINY_SERVER=1
            PM_TINY_PTY_ENABLE=1
    )
    if (PM_TINY_BUILD_TESTS)
        target_compile_definitions(pm_tiny PRIVATE PM_TINY_TESTING)
    endif()

    target_include_directories(pm_tiny
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android
    )

    message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

    target_link_libraries(
            pm_tiny
            PRIVATE yaml-cpp pm_tiny_asio pm_tiny_dependency_graph pm_tiny_program_name pm_tiny_lifecycle
            pm_tiny_protocol pm_tiny_runtime pm_tiny_logging
            pm_tiny_daemon_config
            Threads::Threads
    )

    if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
        set(PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT_ENABLE ON)
    endif ()

    if (PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT_ENABLE)
        message(STATUS "Enable the use of Unix Domain Socket's abstract namespace by default")
        add_compile_definitions(PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT)
        target_compile_definitions(pm_tiny_daemon_config PRIVATE PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT)
    endif ()

    get_target_property(PM_TINY_COMPILE_OPTIONS pm_tiny "COMPILE_OPTIONS")
    get_target_property(PM_TINY_LINK_LIBRARIES pm_tiny "LINK_LIBRARIES")
    message(STATUS "PM_TINY_COMPILE_OPTIONS:${PM_TINY_COMPILE_OPTIONS}")
    message(STATUS "PM_TINY_LINK_LIBRARIES:${PM_TINY_LINK_LIBRARIES}")
endif ()

if (PM_TINY_ON_WINDOWS)
    add_executable(pm
            src/platform/windows/pm.cpp
            src/platform/windows/win_utils.cpp)
    target_include_directories(pm PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows)
    target_compile_definitions(pm PRIVATE PM_TINY_VERSION="${PROJECT_VERSION}" WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(pm PRIVATE pm_tiny_cli_output pm_tiny_cli_command pm_tiny_protocol pm_tiny_runtime
            pm_tiny_daemon_config Threads::Threads)
    target_link_libraries(pm PRIVATE pm_tiny_cli_connection_error)
    if (PM_TINY_BUILD_TESTS)
        add_executable(windows_process_tree_fixture tests/fixtures/windows_process_tree_fixture.cpp)
        target_compile_definitions(windows_process_tree_fixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        if (MINGW)
            set_target_properties(windows_process_tree_fixture PROPERTIES LINK_FLAGS "-municode")
        endif ()
        add_executable(windows_protocol_probe
                tests/fixtures/windows_protocol_probe.cpp)
        target_include_directories(windows_protocol_probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
        target_compile_definitions(windows_protocol_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(windows_protocol_probe PRIVATE pm_tiny_protocol)
        add_executable(connection_error_test tests/unit/connection_error_test.cpp)
        target_link_libraries(connection_error_test PRIVATE pm_tiny_cli_connection_error)
        add_executable(dependency_graph_renderer_test tests/unit/dependency_graph_renderer_test.cpp)
        target_link_libraries(dependency_graph_renderer_test PRIVATE pm_tiny_cli_output)
        add_executable(cli_command_test tests/unit/cli_command_test.cpp)
        target_link_libraries(cli_command_test PRIVATE pm_tiny_cli_command)
        add_executable(cli_response_test tests/unit/cli_response_test.cpp)
        target_link_libraries(cli_response_test PRIVATE pm_tiny_cli_command)
        add_executable(prog_cfg_order_test tests/unit/prog_cfg_order_test.cpp)
        target_link_libraries(prog_cfg_order_test PRIVATE pm_tiny_dependency_graph pm_tiny_program_name)
        target_include_directories(prog_cfg_order_test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
        add_executable(control_command_test tests/unit/control_command_test.cpp)
        target_link_libraries(control_command_test PRIVATE pm_tiny_control_command)
        add_executable(restart_policy_test tests/unit/restart_policy_test.cpp)
        target_link_libraries(restart_policy_test PRIVATE pm_tiny_restart_policy)
        add_executable(dependency_graph_test tests/unit/dependency_graph_test.cpp)
        target_link_libraries(dependency_graph_test PRIVATE pm_tiny_dependency_graph)
        add_executable(termination_job_test tests/unit/termination_job_test.cpp)
        target_link_libraries(termination_job_test PRIVATE pm_tiny_lifecycle)
        add_executable(control_operation_test tests/unit/control_operation_test.cpp)
        target_link_libraries(control_operation_test PRIVATE pm_tiny_lifecycle)
        add_executable(runtime_snapshot_test tests/unit/runtime_snapshot_test.cpp)
        target_link_libraries(runtime_snapshot_test PRIVATE pm_tiny_cli_output)
        add_executable(daemon_config_test tests/unit/daemon_config_test.cpp)
        target_link_libraries(daemon_config_test PRIVATE pm_tiny_daemon_config)
        add_executable(program_log_test tests/unit/program_log_test.cpp)
        target_link_libraries(program_log_test PRIVATE pm_tiny_logging)
        add_executable(daemon_log_test tests/unit/daemon_log_test.cpp)
        target_link_libraries(daemon_log_test PRIVATE pm_tiny_logging)
        add_executable(daemon_info_test tests/unit/daemon_info_test.cpp)
        target_link_libraries(daemon_info_test PRIVATE pm_tiny_cli_output)
    endif ()
else ()
    add_executable(pm
            src/cli/pm.cpp
            src/cli/pm_funcs.cpp
            src/cli/posix_privilege_wrapper.cpp
            src/core/session.cpp
            src/core/pm_sys.cpp
            src/core/frame_stream.hpp
            src/core/time_util.cpp
            src/core/memory_util.cpp
            src/core/memory_util.h
            src/core/globals.cpp
    )

    target_include_directories(pm
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
    )
    target_link_libraries(
            pm
            PRIVATE yaml-cpp pm_tiny_asio pm_tiny_cli_output pm_tiny_cli_connection_error pm_tiny_cli_command
            pm_tiny_protocol pm_tiny_runtime pm_tiny_daemon_config pm_tiny_logging
            Threads::Threads
    )
endif ()

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set_target_properties(pm PROPERTIES OUTPUT_NAME pm2)

    if (NOT CMAKE_READELF)
        message(FATAL_ERROR "Android build requires CMAKE_READELF to verify runtime dependencies")
    endif ()
    foreach(PM_TINY_ANDROID_EXECUTABLE IN ITEMS pm_tiny pm)
        add_custom_command(TARGET ${PM_TINY_ANDROID_EXECUTABLE} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                -DPM_TINY_READELF=${CMAKE_READELF}
                -DPM_TINY_EXECUTABLE=$<TARGET_FILE:${PM_TINY_ANDROID_EXECUTABLE}>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_android_no_shared_libcxx.cmake
                VERBATIM)
    endforeach()
endif ()

add_subdirectory(sdk)
if (PM_TINY_BUILD_TESTS)
    add_executable(pm_sdk_ready_tick_probe tests/fixtures/sdk_ready_tick_probe.cpp)
elseif ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    add_executable(pm_sdk_ready_tick_probe EXCLUDE_FROM_ALL tests/fixtures/sdk_ready_tick_probe.cpp)
endif ()
if (PM_TINY_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif ()
if (TARGET pm_sdk_ready_tick_probe)
target_include_directories(pm_sdk_ready_tick_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
target_link_libraries(pm_sdk_ready_tick_probe PRIVATE pm_sdk)
endif ()
if (PM_TINY_BUILD_TESTS)
    enable_testing()
    if (PM_TINY_ON_WINDOWS)
        add_test(NAME connection_error_test COMMAND connection_error_test)
        add_test(NAME dependency_graph_renderer_test COMMAND dependency_graph_renderer_test)
        add_test(NAME cli_command_test COMMAND cli_command_test)
        add_test(NAME cli_response_test COMMAND cli_response_test)
        add_test(NAME prog_cfg_order_test COMMAND prog_cfg_order_test)
        add_test(NAME control_command_test COMMAND control_command_test)
        add_test(NAME restart_policy_test COMMAND restart_policy_test)
        add_test(NAME dependency_graph_test COMMAND dependency_graph_test)
        add_test(NAME termination_job_test COMMAND termination_job_test)
        add_test(NAME control_operation_test COMMAND control_operation_test)
        add_test(NAME runtime_snapshot_test COMMAND runtime_snapshot_test)
        add_test(NAME daemon_config_test COMMAND daemon_config_test)
        add_test(NAME program_log_test COMMAND program_log_test)
        add_test(NAME daemon_log_test COMMAND daemon_log_test)
        add_test(NAME daemon_info_test COMMAND daemon_info_test)
        function(pm_tiny_add_windows_protocol_test test_name scenario artifacts timeout labels)
            add_test(NAME ${test_name}
                    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/windows/windows_protocol_integration.ps1
                    -Daemon $<TARGET_FILE:pm_tiny>
                    -Cli $<TARGET_FILE:pm>
                    -SdkProbe $<TARGET_FILE:pm_sdk_ready_tick_probe>
                    -ProtocolProbe $<TARGET_FILE:windows_protocol_probe>
                    -TreeFixture $<TARGET_FILE:windows_process_tree_fixture>
                    -SourceDir ${CMAKE_CURRENT_SOURCE_DIR}
                    -ArtifactsDir ${CMAKE_BINARY_DIR}/test-artifacts/${artifacts}
                    -Scenario ${scenario})
            set_tests_properties(${test_name} PROPERTIES TIMEOUT ${timeout} LABELS "${labels}")
        endfunction()
        pm_tiny_add_windows_protocol_test(windows_protocol_integration core windows 240
                "integration;windows;protocol")
        pm_tiny_add_windows_protocol_test(windows_lifecycle_transition_integration
                lifecycle-transitions windows-lifecycle 60 "integration;windows;lifecycle")
        pm_tiny_add_windows_protocol_test(windows_dependency_mutation_integration
                dependency-mutations windows-dependency 60 "integration;windows;dependency")
        pm_tiny_add_windows_protocol_test(windows_concurrent_control_integration
                concurrent-controls windows-concurrent 90 "integration;windows;concurrency")
        if (MSVC)
            add_test(NAME windows_service_integration
                    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/windows/windows_service_integration.ps1
                    -Daemon $<TARGET_FILE:pm_tiny>
                    -Client $<TARGET_FILE:pm>
                    -TreeFixture $<TARGET_FILE:windows_process_tree_fixture>
                    -SourceDir ${CMAKE_CURRENT_SOURCE_DIR}
                    -ArtifactsDir ${CMAKE_BINARY_DIR}/test-artifacts/windows-service)
            set_tests_properties(windows_service_integration PROPERTIES TIMEOUT 120)
            add_test(NAME windows_release_transaction_integration
                    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/windows/windows_release_transaction_integration.ps1
                    -Daemon $<TARGET_FILE:pm_tiny>
                    -Client $<TARGET_FILE:pm>
                    -SourceDir ${CMAKE_CURRENT_SOURCE_DIR}
                    -ArtifactsDir ${CMAKE_BINARY_DIR}/test-artifacts/windows-release)
            set_tests_properties(windows_release_transaction_integration PROPERTIES
                    TIMEOUT 240 LABELS "integration;windows;install;upgrade;rollback")
        endif ()
    else ()
        enable_testing()
        add_subdirectory(tests)
    endif ()
endif ()

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
