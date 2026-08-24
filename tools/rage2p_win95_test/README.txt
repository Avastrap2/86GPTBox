ATI Rage II+ / Mach64 GTB Windows 95 Validation Suite (R2PTEST)
================================================================

Purpose
-------
R2PTEST.EXE is a dedicated DirectX 5-era validation utility for the ATI
3D Rage II+ emulation in 86GPTBox. It targets Windows 95 and exercises the
paths changed during the document-backed emulator work.

Recommended guest setup
-----------------------
- Windows 95
- ATI 3D Rage II+ driver installed and working
- 800x600, 16 bpp desktop for the baseline run
- DirectX 5 runtime / the runtime used by the ATI driver package

Tests
-----
Automatic:
- DirectDraw hardware capability reporting
- video-memory surface creation
- 2D color fill
- SRCCOPY BLT
- SRCINVERT ROP
- source color key
- destination color key
- RGB565 2x StretchBlt
- RGB555 2x StretchBlt
- RGB32 2x StretchBlt
- YUY2 1:1 conversion
- YUY2 2x scaling
- YUY2 non-integer scaling
- YUY2 odd-X crop acceptance
- UYVY conversion/scaling (SKIP if driver does not expose it through BLT)
- Direct3D 5 HAL enumeration / device creation
- Gouraud triangle
- line primitive
- point primitive
- 16-bit Z surface / depth test
- source-alpha blending
- texture-format enumeration for RGB332, ARGB1555, RGB565, ARGB4444,
  RGB32, CI8/P8 and CI4/P4
- dither and fog render-state acceptance

Visual/manual:
- YUY2 hardware overlay scaling and chroma alignment
- fullscreen 640x480x16 page flip

Result policy
-------------
PASS = operation succeeded and automated pixel checks matched, or the user
       explicitly confirmed a visual test.
FAIL = advertised/required Rage II+ path failed or produced incorrect pixels.
SKIP = runtime/driver rejected an optional path or the user cancelled a visual
       test. A SKIP should be investigated against the driver's advertised caps
       before treating it as an emulator bug.

Log
---
Press "Save Log" to create R2PTEST.LOG next to R2PTEST.EXE.

Build
-----
Open Watcom 2.0 (Win32 target):
  wcl386 -q -bt=nt -l=nt_win -3r -os -s -w4 rage2p_test.c -fe=R2PTEST.EXE

The GitHub workflow included with the repository uses open-watcom/setup-watcom
and produces R2PTEST.EXE as an Actions artifact.
