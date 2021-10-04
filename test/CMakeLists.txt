
add_executable(string_utils_test
        string_utils_test.cpp
        ../src/string_utils.cpp)

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
        ../src/prog_cfg.cpp
        ../src/log.cpp
        ../src/logger.hpp
        ../src/pm_sys.cpp
        ../src/time_util.cpp
        ../src/globals.cpp
        ../src/string_utils.cpp
)
target_include_directories(test_prog PRIVATE src)
target_link_libraries(test_prog yaml-cpp)

add_executable(
        yaml_test yaml_test.cpp
)

target_link_libraries(yaml_test yaml-cpp)

add_executable(test_string_utils test_string_utils.cpp
        ../src/string_utils.cpp

)
add_executable(test_frame_stream test_frame_stream.cpp)
add_executable(poll_input poll_input.cpp)
add_executable(poll_server poll_server.cpp)
add_executable(poll_client poll_client.cpp)
add_executable(test_waitpid test_waitpid.cpp)
add_executable(sigpromask_test sigpromask_test.cpp)
add_executable(color_table color_table.cpp)
add_executable(test_graph test_graph.cpp)
add_executable(test_getopt test_getopt.cpp)
add_executable(test_bus_error test_bus_error.cpp)
set(Boost_INCLUDE_DIR "$ENV{HOME}/Downloads/boost_1_82_0/_install/include")
message(STATUS "${Boost_INCLUDE_DIR}")
find_package(Boost)
message(STATUS "Boost_FOUND:${Boost_FOUND}")
if (Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
    add_executable(test_boost test_boost.cpp)
endif ()
add_executable(test_list test_list.cpp)
add_executable(test_oom test_oom.cpp)