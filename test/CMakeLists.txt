
include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/daemon
)

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif ()

add_executable(string_utils_test
        string_utils_test.cpp
        ../src/core/string_utils.cpp)

add_executable(pm_tiny_helper_test
        pm_tiny_helper_test.cpp
        ../src/core/pm_tiny_helper.cpp
)
target_link_libraries(pm_tiny_helper_test yaml-cpp)

add_executable(pm_tiny_utility_test
        pm_tiny_utility_test.cpp
        ../src/core/pm_tiny_utility.cpp
)

add_executable(pm_sys_test
        pm_sys_test.cpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp
        ../src/core/log.cpp
)

add_executable(session_test
        session_test.cpp
        ../src/core/session.cpp
        ../src/core/protocol_v2.cpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp
        ../src/core/log.cpp
)

add_executable(prog_cfg_test
        prog_cfg_test.cpp
        ../src/daemon/prog_cfg.cpp
        ../src/core/prog_cfg_yaml_helper.cpp
        ../src/core/log.cpp
        ../src/core/string_utils.cpp
        ../src/core/globals.cpp
        ../src/core/time_util.cpp
        ../src/core/pm_sys.cpp
)
target_link_libraries(prog_cfg_test yaml-cpp)
target_link_libraries(prog_cfg_test pm_tiny_dependency_graph)

add_executable(memory_util_test
        memory_util_test.cpp
        ../src/core/memory_util.cpp
)

add_executable(forcekill_test forcekill_test.cpp)
add_executable(normal_exit_test normal_exit_test.cpp)
add_executable(abnorm_exit_test abnorm_exit_test.cpp)
add_executable(runforever_test runforever_test.cpp)
add_executable(abort_test abort_test.cpp)
add_executable(close_stdout_test close_stdout_test.cpp)
add_executable(print_env print_env.cpp)
add_executable(ScopeGuardTest ScopeGuardTest.cpp)
add_executable(test_sdk test_sdk.cpp
)
target_link_libraries(test_sdk
        pm_sdk
)
add_executable(test_sdk_c test_sdk_c.c
)
target_link_libraries(test_sdk_c
        pm_sdk
)
add_executable(test_prog test_prog.cpp
        ../src/daemon/prog_cfg.cpp
        ../src/core/prog_cfg_yaml_helper.cpp
        ../src/core/log.cpp
        ../src/core/logger.hpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp
        ../src/core/string_utils.cpp
)
target_include_directories(test_prog PRIVATE src)
target_link_libraries(test_prog yaml-cpp)

add_executable(
        yaml_test yaml_test.cpp
)

target_link_libraries(yaml_test yaml-cpp)

add_executable(test_string_utils test_string_utils.cpp
        ../src/core/string_utils.cpp

)
add_executable(test_frame_stream test_frame_stream.cpp)
add_executable(protocol_v2_test protocol_v2_test.cpp ../src/core/protocol_v2.cpp)
add_executable(process_list_test process_list_test.cpp)
target_link_libraries(process_list_test pm_tiny_cli_output)
add_executable(process_list_renderer_test process_list_renderer_test.cpp)
target_link_libraries(process_list_renderer_test pm_tiny_cli_output)
add_executable(dependency_graph_renderer_test dependency_graph_renderer_test.cpp)
target_link_libraries(dependency_graph_renderer_test pm_tiny_cli_output)
add_executable(connection_error_test connection_error_test.cpp)
target_link_libraries(connection_error_test pm_tiny_cli_connection_error)
add_executable(cli_command_test cli_command_test.cpp)
target_link_libraries(cli_command_test pm_tiny_cli_command)
add_executable(prog_cfg_order_test prog_cfg_order_test.cpp)
target_link_libraries(prog_cfg_order_test pm_tiny_dependency_graph)
add_executable(dependency_graph_test dependency_graph_test.cpp)
target_link_libraries(dependency_graph_test pm_tiny_dependency_graph)
add_executable(control_command_test control_command_test.cpp)
target_link_libraries(control_command_test pm_tiny_control_command)
add_executable(restart_policy_test restart_policy_test.cpp)
target_link_libraries(restart_policy_test pm_tiny_restart_policy)
add_test(NAME protocol_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/protocol_integration.sh)
set_tests_properties(protocol_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}")
add_test(NAME cli_signal_integration
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/cli_signal_integration.py $<TARGET_FILE:pm>)
add_test(NAME cli_connection_failure_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_connection_failure_integration.sh)
set_tests_properties(cli_connection_failure_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;cli")
add_test(NAME quit_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/quit_integration.sh)
set_tests_properties(quit_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 60
        LABELS "integration;linux;cli")
add_test(NAME process_tree_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/process_tree_integration.sh)
add_test(NAME restart_policy_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/restart_policy_integration.sh)
set_tests_properties(restart_policy_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;restart")
add_test(NAME dependency_integration
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/dependency_integration.sh)
set_tests_properties(dependency_integration PROPERTIES
        ENVIRONMENT "PM_TINY_TEST_BIN=${CMAKE_BINARY_DIR}"
        TIMEOUT 30
        LABELS "integration;linux;dependency")
add_executable(poll_input poll_input.cpp)
add_executable(poll_server poll_server.cpp)
add_executable(poll_client poll_client.cpp)
add_executable(test_waitpid test_waitpid.cpp)
add_executable(process_tree_fixture process_tree_fixture.cpp)
add_executable(process_tree_controller_test process_tree_controller_test.cpp
        ../src/platform/linux/process_tree_controller.cpp
        ../src/platform/linux/cgroup_fs.cpp
        ../src/platform/linux/process_group_backend.cpp
        ../src/platform/linux/cgroup_v2_backend.cpp)
add_executable(cgroup_v2_backend_test cgroup_v2_backend_test.cpp
        ../src/platform/linux/cgroup_v2_backend.cpp
        ../src/platform/linux/process_tree_controller.cpp
        ../src/platform/linux/process_group_backend.cpp
        ../src/platform/linux/cgroup_fs.cpp)
add_executable(process_reaper_test process_reaper_test.cpp
        ../src/daemon/process_reaper.cpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp
        ../src/core/log.cpp)
add_executable(child_launch_context_test child_launch_context_test.cpp
        ../src/daemon/child_launch_context.cpp
        ../src/core/pm_sys.cpp
        ../src/core/time_util.cpp
        ../src/core/globals.cpp
        ../src/core/log.cpp)
add_executable(managed_process_instance_test managed_process_instance_test.cpp)
add_executable(termination_job_test termination_job_test.cpp
        ../src/daemon/termination_job.cpp)
add_executable(sigpromask_test sigpromask_test.cpp)
add_executable(color_table color_table.cpp)
add_executable(test_getopt test_getopt.cpp)
add_executable(test_bus_error test_bus_error.cpp)
add_executable(test_list test_list.cpp)
add_executable(test_oom test_oom.cpp)

set(PM_TINY_AUTOMATED_TESTS
        pm_tiny_helper_test
        pm_tiny_utility_test
        pm_sys_test
        session_test
        prog_cfg_test
        memory_util_test
        string_utils_test
        test_string_utils
        ScopeGuardTest
        test_frame_stream
        protocol_v2_test
        process_list_test
        process_list_renderer_test
        dependency_graph_renderer_test
        connection_error_test
        cli_command_test
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
        yaml_test
)

foreach(test_target IN LISTS PM_TINY_AUTOMATED_TESTS)
    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()
