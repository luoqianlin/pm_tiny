find_package(Threads REQUIRED)

if (WIN32)
    set(PM_TINY_SDK_TRANSPORT client_transport_windows.cpp)
else()
    set(PM_TINY_SDK_TRANSPORT client_transport_posix.cpp)
endif()

add_library(pm_sdk
        pm_tiny_sdk.hpp
        pm_tiny_sdk.h
        pm_tiny_sdk.cpp
        pm_tiny_sdk_c.cpp
        client_transport.h
        ${PM_TINY_SDK_TRANSPORT})
target_include_directories(pm_sdk PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core)
target_compile_definitions(pm_sdk PRIVATE PM_TINY_API_EXPORTS)
target_link_libraries(pm_sdk PRIVATE pm_tiny_protocol Threads::Threads)
if (WIN32)
    target_compile_definitions(pm_sdk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if (NOT MSVC)
    target_compile_options(pm_sdk PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()
set_target_properties(pm_sdk
        PROPERTIES PUBLIC_HEADER "pm_tiny_sdk.hpp;pm_tiny_sdk.h")

install(TARGETS pm_sdk
        CONFIGURATIONS Release
        PUBLIC_HEADER DESTINATION Release/include
        RUNTIME DESTINATION Release/bin
        LIBRARY DESTINATION Release/lib
        ARCHIVE DESTINATION Release/lib/static)

install(TARGETS pm_sdk
        CONFIGURATIONS Debug
        PUBLIC_HEADER DESTINATION Debug/include
        RUNTIME DESTINATION Debug/bin
        LIBRARY DESTINATION Debug/lib
        ARCHIVE DESTINATION Debug/lib/static)
