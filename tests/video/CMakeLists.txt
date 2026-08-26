add_executable(mach64_keyed_blit_test
    mach64_keyed_blit_test.c
    ${PROJECT_SOURCE_DIR}/src/video/vid_ati_mach64_accel.c
)

target_compile_definitions(mach64_keyed_blit_test PRIVATE
    mach64_queue=mach64_queue_legacy
)

target_include_directories(mach64_keyed_blit_test PRIVATE
    ${PROJECT_BINARY_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/cpu
    ${PROJECT_SOURCE_DIR}/src/codegen
    ${PROJECT_SOURCE_DIR}/src/codegen_new
)

add_test(NAME mach64_keyed_blit COMMAND mach64_keyed_blit_test)

add_executable(mach64_dp_set_engine_test
    mach64_dp_set_engine_test.c
    ${PROJECT_SOURCE_DIR}/src/video/vid_ati_mach64_accel.c
)

target_compile_definitions(mach64_dp_set_engine_test PRIVATE
    mach64_queue=mach64_queue_legacy
)

target_include_directories(mach64_dp_set_engine_test PRIVATE
    ${PROJECT_BINARY_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/cpu
    ${PROJECT_SOURCE_DIR}/src/codegen
    ${PROJECT_SOURCE_DIR}/src/codegen_new
)

add_test(NAME mach64_dp_set_engine COMMAND mach64_dp_set_engine_test)

add_executable(mach64_scaler_math_test
    mach64_scaler_math_test.c
)

add_test(NAME mach64_scaler_math COMMAND mach64_scaler_math_test)

add_executable(mach64_texture_palette_test
    mach64_texture_palette_test.c
)

add_test(NAME mach64_texture_palette COMMAND mach64_texture_palette_test)

add_executable(mach64_texture_key_test
    mach64_texture_key_test.c
)

add_test(NAME mach64_texture_key COMMAND mach64_texture_key_test)

add_executable(mach64_3d_data_path_test
    mach64_3d_data_path_test.c
)

add_test(NAME mach64_3d_data_path COMMAND mach64_3d_data_path_test)

add_executable(mach64_3d_trapezoid_test
    mach64_3d_trapezoid_test.c
)

add_test(NAME mach64_3d_trapezoid COMMAND mach64_3d_trapezoid_test)

add_executable(mach64_mipmap_lod_test
    mach64_mipmap_lod_test.c
)

add_test(NAME mach64_mipmap_lod COMMAND mach64_mipmap_lod_test)

add_executable(mach64_3d_reg_fields_test
    mach64_3d_reg_fields_test.c
)

add_test(NAME mach64_3d_reg_fields COMMAND mach64_3d_reg_fields_test)

add_executable(mach64_yuv_math_test
    mach64_yuv_math_test.c
)

add_test(NAME mach64_yuv_math COMMAND mach64_yuv_math_test)

add_executable(mach64_scaler_test
    mach64_scaler_test.c
    ${PROJECT_SOURCE_DIR}/src/video/vid_ati_mach64_3d.c
)

target_include_directories(mach64_scaler_test PRIVATE
    ${PROJECT_BINARY_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/cpu
    ${PROJECT_SOURCE_DIR}/src/codegen
    ${PROJECT_SOURCE_DIR}/src/codegen_new
)

add_test(NAME mach64_scaler COMMAND mach64_scaler_test)
