#include "r2ptest_part_00.inc"
#define IID_IDirect3D2 IID_R2P_IDirect3D2
#define IID_IDirect3DTexture2 IID_R2P_IDirect3DTexture2

#include "r2ptest_native_helpers.inc"

/* Baseline operations must use the current Win95 display format. ATI's
 * DirectDraw driver can reject arbitrary off-screen RGB formats even when
 * the accelerator itself supports them. Keep explicit format probes in the
 * automatic suite separate from the native baseline. */
#define pf_rgb565 r2p_native_rgb16
#include "r2ptest_part_01.inc"
#include "r2ptest_part_02.inc"
#include "r2ptest_part_03.inc"
#undef pf_rgb565

/* The extended texture suite wraps the base automatic suite. */
#define run_automatic_tests r2p_run_automatic_tests_base
#include "r2ptest_part_04.inc"
#undef run_automatic_tests
#include "r2ptest_part_045_texture.inc"
#include "r2ptest_part_05.inc"
