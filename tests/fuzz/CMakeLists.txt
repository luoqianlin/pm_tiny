add_library(pm_tiny_fuzz_adapters STATIC fuzz_adapters.cpp)
target_include_directories(pm_tiny_fuzz_adapters PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/daemon)
target_link_libraries(pm_tiny_fuzz_adapters PRIVATE
        yaml-cpp
        pm_tiny_protocol
        pm_tiny_logging
        pm_tiny_dependency_graph
        pm_tiny_program_name)
target_sources(pm_tiny_fuzz_adapters PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core/prog_cfg_yaml_helper.cpp)

foreach(PM_TINY_FUZZ_SUBJECT IN ITEMS protocol config log)
    if (PM_TINY_FUZZ_ENGINE STREQUAL "libfuzzer")
        set(PM_TINY_FUZZ_TARGET pm_tiny_fuzz_${PM_TINY_FUZZ_SUBJECT})
        add_executable(${PM_TINY_FUZZ_TARGET} libfuzzer_entry.cpp)
        target_compile_definitions(${PM_TINY_FUZZ_TARGET} PRIVATE
                PM_TINY_FUZZ_${PM_TINY_FUZZ_SUBJECT}=1)
        target_link_options(${PM_TINY_FUZZ_TARGET} PRIVATE
                -fsanitize=fuzzer,address,undefined)
    else ()
        set(PM_TINY_FUZZ_TARGET pm_tiny_afl_${PM_TINY_FUZZ_SUBJECT})
        add_executable(${PM_TINY_FUZZ_TARGET} afl_entry.cpp)
        target_compile_definitions(${PM_TINY_FUZZ_TARGET} PRIVATE
                PM_TINY_FUZZ_${PM_TINY_FUZZ_SUBJECT}=1)
    endif ()
    target_link_libraries(${PM_TINY_FUZZ_TARGET} PRIVATE pm_tiny_fuzz_adapters)
endforeach()
