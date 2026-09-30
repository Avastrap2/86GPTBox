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
