include_guard(GLOBAL)

# Detect the target architecture by trying to compile `src/arch_detect.c`
try_compile(RESULT_VAR ${CMAKE_BINARY_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/src/arch_detect.c" OUTPUT_VARIABLE ARCH)
string(REGEX MATCH "ARCH ([a-zA-Z0-9_]+)" ARCH "${ARCH}")
string(REPLACE "ARCH " "" ARCH "${ARCH}")

if(NOT ARCH)
    # CMake 4.4 may omit compiler diagnostics from the old try_compile()
    # signature's OUTPUT_VARIABLE. Fall back to the compiler target tuple.
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
        OUTPUT_VARIABLE COMPILER_TARGET
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(COMPILER_TARGET MATCHES "^(x86_64|amd64)")
        set(ARCH x86_64)
    elseif(COMPILER_TARGET MATCHES "^(aarch64|arm64)")
        set(ARCH arm64)
    else()
        set(ARCH unknown)
    endif()
endif()

if (ARCH STREQUAL "x86_64")
    set(ARCH_X64 1)
elseif (ARCH STREQUAL "arm64")
    set(ARCH_ARM64 1)
endif()
