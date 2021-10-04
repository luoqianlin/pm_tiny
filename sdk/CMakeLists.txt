add_library(pm_sdk
        AppClient.h
        AppClient.cpp
        ../src/session.cpp
        ../src/pm_sys.cpp
        ../src/frame_stream.hpp
        ../src/time_util.cpp
        ../src/log.cpp
        ../src/memory_util.h
        ../src/memory_util.cpp
        ../src/globals.cpp
        PM_Tiny_app_client.cpp
        )
target_include_directories(pm_sdk PUBLIC ../src)
target_compile_definitions(pm_sdk PRIVATE PM_TINY_API_EXPORTS)
target_compile_options(pm_sdk PRIVATE -fvisibility=hidden -Wall -Wextra
        #         -fsanitize=address -ggdb
        )
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