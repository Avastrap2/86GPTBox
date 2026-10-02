if(BUILD_TESTING)
    # ide_task_file_test.c includes hdc_ide.c; the device code it does not
    # reach goes with the sections the linker drops.
    add_executable(ide_task_file_test ide_task_file_test.c)
    target_include_directories(ide_task_file_test PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/cpu
        ${PROJECT_BINARY_DIR}/src/include
    )
    target_compile_options(ide_task_file_test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-ffunction-sections;-fdata-sections>
    )
    target_link_options(ide_task_file_test PRIVATE
        $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
        $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<C_COMPILER_ID:GNU,Clang>>:-Wl,--gc-sections>
    )
    add_test(NAME ide_task_file COMMAND ide_task_file_test)
endif()
