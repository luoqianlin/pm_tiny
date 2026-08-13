if (NOT PM_TINY_READELF OR NOT PM_TINY_EXECUTABLE)
    message(FATAL_ERROR "Android dependency check requires readelf and executable paths")
endif ()

execute_process(
        COMMAND "${PM_TINY_READELF}" -d "${PM_TINY_EXECUTABLE}"
        RESULT_VARIABLE PM_TINY_READELF_RESULT
        OUTPUT_VARIABLE PM_TINY_DYNAMIC_SECTION
        ERROR_VARIABLE PM_TINY_READELF_ERROR)
if (NOT PM_TINY_READELF_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Failed to inspect Android executable ${PM_TINY_EXECUTABLE}: ${PM_TINY_READELF_ERROR}")
endif ()

if (PM_TINY_DYNAMIC_SECTION MATCHES "libc\\+\\+_shared\\.so")
    message(FATAL_ERROR
            "Android executable ${PM_TINY_EXECUTABLE} must not depend on libc++_shared.so")
endif ()
