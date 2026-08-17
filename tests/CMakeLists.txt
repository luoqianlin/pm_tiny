include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/daemon
)

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif ()

add_executable(string_utils_test
        unit/string_utils_test.cpp
        ../src/core/string_utils.cpp)

add_executable(pm_tiny_helper_test
        unit/pm_tiny_helper_test.cpp
        ../src/core/pm_tiny_helper.cpp)
target_link_libraries(pm_tiny_helper_test yaml-cpp)

add_executable(daemon_config_test unit/daemon_config_test.cpp)
target_link_libraries(daemon_config_test pm_tiny_daemon_config)

add_executable(pm_tiny_utility_test
        unit/pm_tiny_utility_test.cpp
        ../src/core/pm_tiny_utility.cpp)

add_executable(pm_sys_test
        unit/pm_sys_test.cpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp)
target_link_libraries(pm_sys_test pm_tiny_logging)

add_executable(launch_environment_test
        unit/launch_environment_test.cpp
        ../src/daemon/launch_environment.cpp)

add_executable(posix_privilege_wrapper_test
        unit/posix_privilege_wrapper_test.cpp
        ../src/cli/posix_privilege_wrapper.cpp)
target_include_directories(posix_privilege_wrapper_test PRIVATE ../src/cli)

add_executable(session_test
        unit/session_test.cpp
        ../src/core/session.cpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp)
target_link_libraries(session_test pm_tiny_protocol pm_tiny_logging)

add_executable(prog_cfg_test
        unit/prog_cfg_test.cpp
        ../src/daemon/prog_cfg.cpp
        ../src/core/prog_cfg_yaml_helper.cpp
        ../src/core/string_utils.cpp
        ../src/core/globals.cpp
        ../src/core/time_util.cpp
        ../src/core/pm_sys.cpp)
target_link_libraries(prog_cfg_test
        yaml-cpp pm_tiny_dependency_graph pm_tiny_program_name pm_tiny_logging)
target_compile_definitions(prog_cfg_test PRIVATE PM_TINY_TESTING)

add_executable(memory_util_test
        unit/memory_util_test.cpp
        ../src/core/memory_util.cpp)

add_executable(persistence_worker_test
        unit/persistence_worker_test.cpp
        ../src/core/persistence_worker.cpp)
target_link_libraries(persistence_worker_test Threads::Threads)

add_executable(sdk_heartbeat_fixture fixtures/sdk_heartbeat_fixture.cpp)
target_link_libraries(sdk_heartbeat_fixture pm_sdk)

add_executable(sdk_client_test unit/sdk_client_test.cpp)
target_link_libraries(sdk_client_test pm_sdk pm_tiny_protocol)
add_test(NAME sdk_client_test COMMAND sdk_client_test)
set_tests_properties(sdk_client_test PROPERTIES TIMEOUT 60 LABELS "unit;sdk")

add_executable(sdk_c_header_test unit/sdk_c_header_test.c)
set_target_properties(sdk_c_header_test PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(sdk_c_header_test pm_sdk)
add_test(NAME sdk_c_header_test COMMAND sdk_c_header_test)
set_tests_properties(sdk_c_header_test PROPERTIES LABELS "unit;sdk;c-api")

add_executable(frame_stream_test unit/frame_stream_test.cpp)
add_executable(protocol_v3_test unit/protocol_v3_test.cpp)
target_link_libraries(protocol_v3_test pm_tiny_protocol)
add_executable(program_log_test unit/program_log_test.cpp)
target_link_libraries(program_log_test pm_tiny_logging)
add_executable(daemon_log_test unit/daemon_log_test.cpp)
target_link_libraries(daemon_log_test pm_tiny_logging Threads::Threads)
add_executable(daemon_info_test unit/daemon_info_test.cpp)
target_link_libraries(daemon_info_test pm_tiny_cli_output)
add_executable(process_list_test unit/process_list_test.cpp)
target_link_libraries(process_list_test pm_tiny_cli_output)
add_executable(process_list_renderer_test unit/process_list_renderer_test.cpp)
target_link_libraries(process_list_renderer_test pm_tiny_cli_output)
add_executable(runtime_snapshot_test unit/runtime_snapshot_test.cpp)
target_link_libraries(runtime_snapshot_test pm_tiny_cli_output)
add_executable(dependency_graph_renderer_test unit/dependency_graph_renderer_test.cpp)
target_link_libraries(dependency_graph_renderer_test pm_tiny_cli_output)
add_executable(connection_error_test unit/connection_error_test.cpp)
target_link_libraries(connection_error_test pm_tiny_cli_connection_error)
add_executable(cli_command_test unit/cli_command_test.cpp)
target_link_libraries(cli_command_test pm_tiny_cli_command)
add_executable(cli_response_test unit/cli_response_test.cpp)
target_link_libraries(cli_response_test pm_tiny_cli_command)
add_executable(prog_cfg_order_test unit/prog_cfg_order_test.cpp)
target_link_libraries(prog_cfg_order_test pm_tiny_dependency_graph pm_tiny_program_name)
add_executable(dependency_graph_test unit/dependency_graph_test.cpp)
target_link_libraries(dependency_graph_test pm_tiny_dependency_graph)
add_executable(control_command_test unit/control_command_test.cpp)
target_link_libraries(control_command_test pm_tiny_control_command)
add_executable(restart_policy_test unit/restart_policy_test.cpp)
target_link_libraries(restart_policy_test pm_tiny_restart_policy)

add_executable(process_tree_fixture fixtures/process_tree_fixture.cpp)
add_executable(posix_runtime_fixture fixtures/posix_runtime_fixture.cpp)
add_executable(process_tree_controller_test
        unit/process_tree_controller_test.cpp
        ../src/platform/linux/process_tree_controller.cpp
        ../src/platform/linux/cgroup_fs.cpp
        ../src/platform/linux/process_group_backend.cpp
        ../src/platform/linux/cgroup_v2_backend.cpp)
add_executable(cgroup_v2_backend_test
        unit/cgroup_v2_backend_test.cpp
        ../src/platform/linux/cgroup_v2_backend.cpp
        ../src/platform/linux/process_tree_controller.cpp
        ../src/platform/linux/process_group_backend.cpp
        ../src/platform/linux/cgroup_fs.cpp)
add_executable(process_reaper_test
        unit/process_reaper_test.cpp
        ../src/daemon/process_reaper.cpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp)
target_link_libraries(process_reaper_test pm_tiny_logging)
add_executable(child_launch_context_test
        unit/child_launch_context_test.cpp
        ../src/daemon/child_launch_context.cpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp)
target_link_libraries(child_launch_context_test pm_tiny_logging)
add_executable(managed_process_instance_test unit/managed_process_instance_test.cpp)
add_executable(termination_job_test unit/termination_job_test.cpp)
target_link_libraries(termination_job_test pm_tiny_lifecycle)
add_executable(control_operation_test unit/control_operation_test.cpp)
target_link_libraries(control_operation_test pm_tiny_lifecycle)
add_executable(lifecycle_orchestrator_test unit/lifecycle_orchestrator_test.cpp)
target_link_libraries(lifecycle_orchestrator_test pm_tiny_lifecycle)
add_executable(scope_guard_test unit/scope_guard_test.cpp)

set(PM_TINY_AUTOMATED_TESTS
        pm_tiny_helper_test
        daemon_config_test
        pm_tiny_utility_test
        pm_sys_test
        launch_environment_test
        posix_privilege_wrapper_test
        session_test
        prog_cfg_test
        persistence_worker_test
        memory_util_test
        string_utils_test
        scope_guard_test
        frame_stream_test
        protocol_v3_test
        program_log_test
        daemon_log_test
        daemon_info_test
        process_list_test
        process_list_renderer_test
        runtime_snapshot_test
        dependency_graph_renderer_test
        connection_error_test
        cli_command_test
        cli_response_test
        prog_cfg_order_test
        dependency_graph_test
        control_command_test
        restart_policy_test
        process_tree_controller_test
        cgroup_v2_backend_test
        process_reaper_test
        child_launch_context_test
        managed_process_instance_test
        termination_job_test
        control_operation_test
        lifecycle_orchestrator_test)

foreach(test_target IN LISTS PM_TINY_AUTOMATED_TESTS)
    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()

add_test(NAME protocol_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/protocol_integration.sh)
if (PM_TINY_SANITIZER_ENABLE)
    set_tests_properties(protocol_integration PROPERTIES
            ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR};PM_TINY_TEST_SANITIZED=1")
else ()
    set_tests_properties(protocol_integration PROPERTIES
            ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}")
endif ()
set_tests_properties(protocol_integration PROPERTIES
        TIMEOUT 90
        LABELS "integration;linux;protocol")
add_test(NAME lifecycle_transition_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/lifecycle_transition_integration.sh)
set_tests_properties(lifecycle_transition_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;lifecycle")
add_test(NAME cli_signal_integration
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/cli_signal_integration.py
        $<TARGET_FILE:pm>)
add_test(NAME cli_connection_failure_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/cli_connection_failure_integration.sh)
set_tests_properties(cli_connection_failure_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;cli")
add_test(NAME info_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/info_integration.sh)
set_tests_properties(info_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;cli")
add_test(NAME empty_program_config_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/empty_program_config_integration.sh)
set_tests_properties(empty_program_config_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;config")
add_test(NAME quit_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/quit_integration.sh)
set_tests_properties(quit_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 60
        LABELS "integration;linux;cli")
add_test(NAME process_tree_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/process_tree_integration.sh)
set_tests_properties(process_tree_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}")
add_test(NAME restart_policy_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/restart_policy_integration.sh)
set_tests_properties(restart_policy_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;restart")
add_test(NAME failure_action_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/failure_action_integration.sh)
set_tests_properties(failure_action_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;restart;dependency")
add_test(NAME dependency_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/dependency_integration.sh)
set_tests_properties(dependency_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;dependency")
add_test(NAME dependency_mutation_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/dependency_mutation_integration.sh)
set_tests_properties(dependency_mutation_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;dependency")
add_test(NAME concurrent_control_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/concurrent_control_integration.sh)
set_tests_properties(concurrent_control_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 60
        LABELS "integration;linux;concurrency")
add_test(NAME privilege_drop_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/privilege_drop_integration.sh)
set_tests_properties(privilege_drop_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;privilege")
add_test(NAME posix_runtime_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/posix_runtime_integration.sh)
set_tests_properties(posix_runtime_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;pty;logging")
add_test(NAME cgroup_v2_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/cgroup_v2_integration.sh)
set_tests_properties(cgroup_v2_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        SKIP_RETURN_CODE 77
        LABELS "integration;linux;privileged;cgroup")
add_test(NAME release_transaction_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/linux/release_transaction_integration.sh)
set_tests_properties(release_transaction_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR};PM_TINY_SOURCE_DIR=${CMAKE_SOURCE_DIR};PM_TINY_RELEASE_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/test-artifacts/linux-release"
        TIMEOUT 60
        LABELS "integration;linux;install;upgrade;rollback")
