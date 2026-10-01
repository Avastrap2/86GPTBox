if(BUILD_TESTING)
    add_executable(row_boundary_test row_boundary_test.c)
    target_include_directories(row_boundary_test PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/cpu
        ${PROJECT_BINARY_DIR}/src/include
    )
    target_compile_options(row_boundary_test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-ffunction-sections;-fdata-sections>
    )
    target_link_options(row_boundary_test PRIVATE
        $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
        $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<C_COMPILER_ID:GNU,Clang>>:-Wl,--gc-sections>
    )
    add_test(NAME row_boundary_test COMMAND row_boundary_test)
endif()

# Memory write-lookup regressions are opt-in through MEM_LOOKUP_TESTS and do not use GTest.
# This directory is also added by tests/CMakeLists.txt for BUILD_TESTING.
if(NOT MEM_LOOKUP_TESTS)
    return()
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT mem_test_ipo_supported OUTPUT mem_test_ipo_error LANGUAGES C)
if(NOT mem_test_ipo_supported)
    message(FATAL_ERROR "Memory lookup tests require IPO: ${mem_test_ipo_error}")
endif()

# These tests include mem.c directly. IPO removes unreachable device/platform
# dependencies, so the checks run the real lookup functions without a full VM.
foreach(mode IN ITEMS old new old_interpreter new_interpreter)
    set(test_target mem_write_lookup_${mode}_test)
    add_executable(${test_target} mem_write_lookup_test.c)
    set_property(TARGET ${test_target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    target_compile_options(${test_target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:-O2>)
    target_include_directories(${test_target} PRIVATE
        ${PROJECT_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/cpu
    )
    if(mode STREQUAL "old" OR mode STREQUAL "new")
        target_compile_definitions(${test_target} PRIVATE USE_DYNAREC)
    endif()
    if(mode MATCHES "^new")
        target_compile_definitions(${test_target} PRIVATE USE_NEW_DYNAREC)
    endif()
    add_test(NAME mem_write_lookup_${mode} COMMAND ${test_target})
endforeach()
