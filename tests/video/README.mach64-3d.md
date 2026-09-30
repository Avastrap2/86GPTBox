# Mach64 3D command regressions

`mach64_3d_triangle_test` links the production renderer and programs
`mach64_3d_write` / `mach64_3d_read`. It checks framebuffer and 16-bit Z-buffer
contents, including pixels outside each primitive. Shared legacy registers are
seeded directly, and platform/FIFO dependencies are stubbed. This is not a CPU
MMIO, asynchronous FIFO, guest-driver, or physical-hardware integration test.

Enable `MACH64_ACCEL_TESTS` in an existing configured build, build the test, and
run it with CTest. The option defaults to OFF for normal builds. For example:

```text
cmake -S . -B build -DMACH64_ACCEL_TESTS=ON
cmake --build build --target mach64_3d_triangle_test
ctest --test-dir build -R "^mach64_3d_triangle$" --output-on-failure
```

Use `ctest --test-dir build --output-on-failure` after building all Mach64 test
targets to run the full suite. Prefer CTest over direct invocation on Windows:
build configurations that set `CMAKE_WIN32_EXECUTABLE` may make PowerShell
return before a directly launched test finishes.

## Coverage

The command test currently runs 1,071 cases.

| Cases | Checks |
|---:|---|
| 288 | Two-trapezoid triangles: ARGB1555/RGB565/ARGB8888, shaded/textured, both X/Y directions, vertical/sloping leading edges, unclipped/partly clipped/fully clipped, opaque/additive rendering. |
| 24 | No-draw trailing-edge preload, through both length aliases, byte/word/dword programming, shaded/legacy fallback paths, fresh/stale edges. Continuation uses the opposite alias and must not draw before the completing write. |
| 288 | All eight Z comparisons for source less/equal/greater, with Z enabled/disabled and writes enabled/disabled, in all three destination formats. |
| 9 | Texel color-key, texture alpha-mask, and destination-compare inhibition; rejected pixels must leave Z untouched. Disabling inhibition must restore drawing without resetting the engine. |
| 3 | Opaque to additive to opaque state transitions, including a read-only Z pass. |
| 1 | Hidden S fractional state crossing a texel boundary after a split, guest-visible readback, and replacement by a new START write. |
| 4 | Shaded-line to trapezoid transitions, both aliases and last-pixel settings. |
| 1 | No additional FIFO-wait calls for ordinary shared line-state writes; retain the existing shared-to-3D barrier. This checks barrier bookkeeping, not thread execution. |
| 32 | Synthetic triangles using recorded Pixie mode words, RGB555 textures/destination, negative color and Z gradients, clipping, both directions, and white mipmaps. Quantization is checked against neighboring-code bounds, not an assumed hardware dither table. |
| 24 | Eight numeric command fixtures produced by ATI3DCIF 4.03.2510's setup code, replayed in three destination formats. Strict interior color and Z expectations come from the original input vertices, not the renderer. Two fixtures exercise color-field crossings and two exercise depth-field crossings on thin triangles. |
| 16 | Positive initial lead/trail errors, both X/Y directions and zero-error conventions, quadratic S/T, hidden fractional bits, and split/unsplit rendering. Initial X steps must not advance Y. |
| 144 | RGB/alpha field crossings in three destination formats, shaded/white-texture-modulated/source-alpha-blended output, both X/Y directions, clipping, split/unsplit commands, START readback, and unchanged coverage/depth. |
| 192 | Z-field crossings through all eight comparisons, conditional writes, three destination formats, both diagonal directions, clipped spans, split/unsplit commands, and START readback. |
| 27 | Signed, inverted, negative, single-pixel and outside-pitch scissors through trapezoids, shaded lines and front-end scaler rectangles; GUI status, write inhibition and command continuation. |
| 8 | Shaded/legacy line dispatch after delayed DP_SRC/DST_CNTL updates, including polygon mode, through both length aliases. This uses a deterministic barrier callback, not an asynchronous worker. |
| 6 | Ordered-dither blending in RGB555/RGB565: black additive, fully transparent and red-only sources over every table phase and component level must leave unchanged destination components untouched. |
| 4 | Bilinear weights: all 256 binary coordinate fractions on both axes, within a map and across its wrap boundary. |

The triangle expectations use an explicit coverage predicate and closed-form
color, depth, and quadratic S/T expressions. They do not call the renderer's
edge, clipping, interpolation, or packing helpers. Additive triangles start on
black with Z disabled so depth rejection cannot conceal duplicate coverage.
No emulator tracing is enabled by these tests.

The Pixie-profile cases use `SCALE_3D_CNTL=0x06410287/0x064102c7` and
`DP_PIX_WIDTH=0x30030203`, observed in saved Pixie captures. They are not a
replay of the reported faulty scene. Their fully white mipmaps make the
expected lit color independent of LOD choice and fractional filter rounding.
The original triangle cases still use distinct colors in a single nearest map
to test texture-coordinate continuation.

## Trailing-edge preload regression

ATI's *RRG-G02700 Rev. 0.10, mach64 ATI-264VT and 3D RAGE Register Reference
Guide*, printed page 4-46 (PDF page 74), defines DST_BRES_LNTH as follows:

| Bit 31 | Bit 15 | Operation | TRAIL_X |
|---:|---:|---|---|
| 0 | 0 | Bresenham line | Load |
| 0 | 1 | Trapezoid | Retain |
| 1 | 0 | No draw | Load |
| 1 | 1 | Trapezoid | Load |

The same page identifies the 0x120 and 0x144 aliases. A minimal failing sequence
in the old renderer was a no-draw command `0x800c0002` followed by a trapezoid
command `0x00028002`: the second command must retain X=12, not load X=2 or use
a stale edge. With the leading edge at X=8 this draws four pixels per scanline.

Previously, TRAIL_X was loaded only in the trapezoid walker. The front end now
loads its trailing-edge shadow on completing line/preload commands as well.
This update does not drain the legacy FIFO; ordering of shared state remains
at the existing draw barriers. Ordinary legacy lines still fall through.

## Evidence limits

### Initial edge correction

The numeric fixtures in `mach64_3d_driver_fixture.h` were obtained by running
the supplied ATI3DCIF 4.03.2510 triangle arithmetic in an isolated x86
interpreter, with synthetic vertices and device state. No native ATI DLL,
Windows imports, guest VM, or runtime trace is used by the regression suite.
The DLL SHA256 and entry address are recorded in the fixture header. The
internal device code is a driver input, not an assertion about the user's
currently installed driver or a PCI device ID.

At addresses `0x100115d5` and `0x10011715`, that driver's setup checks its
internal device code. For older codes it resolves positive Bresenham errors
with X-only steps before submitting commands; the other path submits the
unresolved errors. The production renderer previously consumed these errors
only after drawing its first row. It now resolves both edges before the first
span and advances the leading-edge interpolants without a Y step. The same
operation handles a newly programmed trailing edge at a triangle split.

The 12 fixture cases produced 297 failing checks before this correction and
zero afterward. All original 650 cases still pass, as do the 16 added texture
trajectory cases. An offline 200-triangle driver-generated check improved
from 51 failing triangles to zero, checking 35,377 strict interior samples.
Those comparisons establish a setup/renderer discrepancy, not a visual result
for the user's current VM. Existing Pixie and Final Reality snapshots contain
positive initial edge errors, but lack original vertex buffers and a complete
synchronized frame for identifying the exact circled polygon.

After the initial-edge correction, an extended 2,000-triangle experiment still
had 26 thin-triangle color failures (99 of 392,444 checked interior samples).
The user also confirmed that the black facets remained in Pixie and Final
Reality. Initial-edge normalization alone was therefore not a visual fix.

### Color-field conversion

RRG-G02700, printed pages 6-12 through 6-14 (PDF pages 186 through 188), places
the RGB/alpha S.8.12 fields in bits 24:4. Original driver setup can emit START
or derivatives outside that signed range for a narrow triangle even though
its vertex colors, and therefore its strict interior color plane, are valid.
The driver fixtures make this independently testable. In offline case 258,
pixel (33,20) previously rendered RGB (0,0,160), while the input vertices
require approximately (101.20,186.90,160.62).

The renderer decoded register writes to the physical width but then clamped
wide host interpolation sums directly. It now converts trapezoid RGB/alpha
samples through the same physical signed field before producing an 8-bit
component. The implementation extracts the sign and eight integer bits;
negative field values still clamp to zero. It does not wrap to unsigned
8-bit color, discard accumulator fractions, alter coverage or Z comparison,
or add any scene-specific condition. The line path is unchanged.

The two added numeric driver fixtures produced 33 failing color checks before
this correction and zero afterward across the three destination formats.
The extended isolated-driver check now passes all 1,975,723 strict interior
samples from 10,000 inputs (including one degenerate input with no interior).
This includes the earlier 2,000-triangle set and its 26 color failures.
The 144 pipeline cases independently check signed modulo-512 arithmetic
through clipping and command continuation. The separate register-field test
checks 21,504 combinations of integer bits, fractional boundaries, and small
or large positive/negative host periods, plus explicit signed boundaries.
Source-alpha blend expectations check the existing software blend contract,
not an independently established hardware rounding rule.

These results establish a reproducible color arithmetic discrepancy. The
register diagrams alone do not specify every intermediate ASIC operation,
and the synthetic driver inputs are not the original vertices of the user's
circled polygons. The user subsequently confirmed that this color correction
removed Pixie's black triangles. Final Reality still had tunnel speckles, a
purple line and a remaining logo triangle. Rapid Pixie camera movement also
produced a corrupted Windows 95 desktop, without an error message; this is
not a confirmed host crash.

### Depth-field conversion

Extending synthetic vertex Z from 500..1499 to 0..59999 exposed 26 failing
triangles among 2,000 inputs: 62 missing interior samples and 58 wrong-depth
samples. The incoming Z is strictly below the replay buffer's initial 60000,
so these failures are not legitimate LEQUAL rejection. Cases 23 and 219 of
that sequence supply the new depth fixtures, which produced 169 failing
checks before the correction across the three destination formats.

The physical Z field is signed S.16.12, bits 28:0 (RRG-G02700 PDF pages
187..188). Like RGB, the emulator decoded registers but then clamped wide
host sums directly. Trapezoid fragments now pass through the physical Z sign
and integer bits before the existing comparison/write logic. No comparison
function, depth-write enable, or line-depth arithmetic is changed.

The corrected renderer passes 10,000 wider-depth inputs with 1,945,188 strict
interior samples, plus 5,000 inputs spanning coordinates -64..255.75 against a
64x64 scissor/pitch, with 2,474,356 checked interior samples. The separate
field test checks 1,835,008 signed-period/integer/fraction combinations and
eight explicit depth boundaries. These remain offline tests, not a claim
that the remaining Final Reality screenshot artifacts have been eliminated.

### Scissors and shared-state dispatch

RRG-G02700 PDF pages 110..111 and 113..114 specify inclusive signed 13-bit X
and signed 15-bit Y scissors. The 3D paths previously treated them as unsigned;
trapezoids and shaded lines also expanded inverted bounds to a full drawing
area. They now retain empty bounds, suppress writes, and still advance live
trajectory state. GUI scissor comparisons and front-end scaler clipping use
the same signed interpretation. This changes the custom 3D/scaler paths,
not the inherited legacy 2D worker's scissor implementation.

Line dispatch also previously inspected DP_SRC/DST_CNTL before the pending
legacy FIFO updates were consumed. Tests reproduce a lost legacy line and a
shaded line incorrectly sent to the legacy fallback. Completing drawing-line
commands in shading mode now synchronize pending shared state before path
selection. Non-drawing preloads and ordinary 2D mode add no wait, and already
clean state uses the existing no-op barrier. Actual worker-thread scheduling
and the user's desktop corruption are not reproduced by this stubbed test.

### Remaining limits

The preload truth table and Z comparison modes are documented register
contracts. The triangle tests additionally establish internal consistency of
the implemented coverage and interpolation across command boundaries. They
are not independent proof of every Rage II+ accumulator, rounding, overflow,
or pixel-ownership rule. Texture tests use either a single nearest map or
identical white texels at every mip level; blending checks avoid unresolved
fractional-rounding choices. These constraints isolate supported properties
without treating the current LOD/dither approximations as a hardware oracle.

The original GT register guide is evidence for the shared command contract,
not a substitute for a complete GTB silicon specification. These tests do not
establish that Final Reality or Pixie emits the failing preload sequence, nor
that this change fixes the reported black triangles. Retest those scenes with
the same guest configuration and driver before assigning a visual outcome.

Mip selection, dither coefficients, the deferred 8-bit write-mask discrepancy,
and asynchronous 2D/3D ordering are outside this patch.
