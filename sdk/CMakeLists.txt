if (WIN32)
add_library(pm_sdk
        AppClient.h
        AppClient_win.cpp
        ../src/core/protocol_v2.cpp
        PM_Tiny_app_client.cpp
        )
else()
add_library(pm_sdk
        AppClient.h AppClient.cpp ../src/core/session.cpp ../src/core/protocol_v2.cpp
        ../src/core/pm_sys.cpp ../src/core/frame_stream.hpp ../src/core/time_util.cpp
        ../src/core/log.cpp ../src/core/memory_util.h ../src/core/memory_util.cpp
        ../src/core/globals.cpp PM_Tiny_app_client.cpp)
endif()
target_include_directories(pm_sdk PUBLIC
        ../src
        ../src/core
        ../src/core/graph
)
target_compile_definitions(pm_sdk PRIVATE PM_TINY_API_EXPORTS)
target_link_libraries(pm_sdk PRIVATE pm_tiny_asio)
if (WIN32)
target_compile_definitions(pm_sdk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pm_sdk PRIVATE ws2_32)
endif()
if (NOT MSVC)
    target_compile_options(pm_sdk PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()
set_target_properties(pm_sdk
        PROPERTIES PUBLIC_HEADER AppClient.h)

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
