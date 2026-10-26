# HDMI text: validation and RAM accounting

Base repository: `xrip/rp2040-ntsc-tv`, branch `vga-tv-hybrid`, commit
`7debfc46844b24d5c1979167b0e91847601ca68b`.

Validation date: 2026-10-26. These results describe the supplied source and
toolchain. The original delivery was checked without a board. The user later
confirmed working RP2350 M2P2 HDMI output with GPIO12..19 and the embedded flash
setup (clock divider 4, read delay 4). Scanline timing has not been measured.

## Current checkout checks

After the YAGNI review, the host runner passes with Clang 23.1.1 on Windows
and AddressSanitizer/UndefinedBehaviorSanitizer: 52,341 text checks and all four
codec pin-order/polarity combinations. All three software previews also render.
Shell scripts use LF line endings on Windows checkouts.

Arm GCC 15.2.1 builds both HDMI text and graphics demos for RP2040 and RP2350.
The RP2350 VGA/NTSC build also passes. Text demo RAM usage is 20,768 bytes on
RP2040 and 20,564 bytes on RP2350, plus a separate 2,048-byte scratch-Y stack.
Both text IRQ call audits have no XIP targets, indirect calls, or unresolved
symbols. The final RP2350 image includes the 256-byte flash setup section.

The following toolchain and controlled memory tables describe the original
delivery, rather than these later checkout builds.

## Original delivery toolchain and builds

- Raspberry Pi Pico SDK 2.3.1,
  commit `079c6f39023649b154152db30f1d781e884879bc`.
- Arm GNU Toolchain 14.2.Rel1, GCC 14.2.1 (20241119).
- CMake 4.4.4; Ninja 1.13.2; picotool 2.3.1.
- `MinSizeRel` with the repository's O3/LTO settings.
- Real Arm builds for RP2040 / Cortex-M0+ and RP2350 Arm / Cortex-M33.

The text palette demo and the existing HDMI graphics demo both build for both
chips. UF2 images in the delivery archive are the text demos. The normal
graphics demos retain their original source. No NTSC backend source is modified.

## Memory: what is unchanged

The production workspace is a union checked by `_Static_assert`:

| Allocation | Before | After |
|---|---:|---:|
| Aligned HDMI palette + two scanlines | 4,896 B | 4,896 B |
| Driver-owned text framebuffer | 0 B | 0 B |
| Driver-owned font cache | 0 B | 0 B |
| 640 x 480 framebuffer | 0 B | 0 B |

The 36-byte Arm text descriptor stores two borrowed pointers and their geometry.
Additional small state and SRAM-resident code are also present. The 4,096-byte
workspace alignment can hide code growth inside an existing linker gap or move
the next aligned boundary. Therefore neither summing buffers nor comparing two
unrelated demo programs is a correct total-memory measurement.

### Controlled comparison with identical application storage

The validation harness keeps the same allocations live in both baseline and
updated builds: a 76,800-byte built-in graphics buffer, a 4,800-byte application
text page, and one 4,096-byte writable bundled font. The updated build uses
runtime-variable mode/font/blink/cursor/addressing settings so LTO cannot simply
discard native text or its optional features.

| Chip | Baseline RAM region | Updated RAM region | Difference |
|---|---:|---:|---:|
| RP2040 | 97,520 B | 97,568 B | **+48 B** |
| RP2350 Arm | 97,312 B | 97,360 B | **+48 B** |

For these exact links, the additional SRAM code fits into space that was already
lost to workspace alignment. **The +48 B result is specific to these linked
applications and toolchain.** It is not a guarantee for an arbitrary emulator
link: inspect that application's map and include SRAM code, alignment padding,
SDK reservations, and stacks. The constant 4,896-byte workspace and the absence
of copied VRAM/font are properties of the implementation itself.

Excluding the reduction of this pre-existing alignment gap, the occupied
sections grow by **1,232 B on RP2040 and 1,144 B on RP2350** in this comparison,
mostly executable SRAM code. This is the cost to budget when an emulator's link
does not have a suitable unused gap; its own alignment can also affect the
final RAM-region delta.

The comparison harness and a portable reproduction script are supplied in the
archive's `validation/` directory. The baseline uses its existing text-pointer
API, which does not render text; equal application storage makes the added
backend cost measurable without crediting the new implementation for deleting
a different demo's buffers.

### Standalone text demo

| Chip | RAM region used | Separate main stack in scratch Y |
|---|---:|---:|
| RP2040 | 20,764 B | 2,048 B |
| RP2350 Arm | 20,556 B | 2,048 B |

The RAM-region figures include the 4,800-byte application page, the one existing
4,096-byte font, the 4,896-byte workspace, SRAM code/data, linker padding and SDK
reservations (including the 2,048-byte heap reservation). No 76,800-byte graphics
framebuffer is linked into these text-only demos. The main stack is reported
separately and must be counted too. These are linker reservations, not measured
worst-case runtime stack/heap use.

HDMI now honors `GRAPHICS_BUILTIN_FRAMEBUFFER=OFF`. This can eliminate the
previously unconditional 76,800-byte built-in buffer when the emulator already
provides its own graphics buffer. It is a separate integration choice, not the
basis of the controlled +48-byte comparison above.

## Functional tests

`bash tests/run_host_tests.sh` compiles the actual production helpers with
AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer is disabled
because it requires thread information under `/proc` that the build sandbox
does not provide; this code does not allocate text buffers on the heap.

The text suite passes **52,341 checks**, covering:

- writes to borrowed VRAM/font remaining visible without rebinding;
- every visible scanline of the supported geometries and their borders;
- 8-, 14-, and 16-line fonts, 32-byte font slots, and both bit orders;
- page offsets, row pitch, VRAM wrapping, very large start addresses, and bounds;
- foregrounds, bright backgrounds, attribute blink, and cursor shape/address;
- clipped drawing and clearing, with guard bytes around the storage;
- rejection of invalid descriptors without replacing the previous binding.

For **each** of four RGB/BGR and differential-polarity combinations, codec tests
check the actual production functions, not a duplicate implementation:

- 1,024 symbol packing cases;
- 8,960 exact original-graphics RGB round trips;
- all 256 independent pairs of the 16 CGA text colors, decoded as TMDS and
  checked for disparity balance;
- 64 graphics/text/graphics workspace transitions, including full text-line
  overwrites, palette edits while text is active, and guard bytes;
- 262,144 glyph/attribute/width/bit-order combinations, checked against
  individual expected pixel indices, including the odd/even phase bit.

## Scanline code and timing review

The scanline path uses SRAM-resident code. RP2040 divider helpers are also put
in SRAM with `PICO_DIVIDER_IN_RAM=1`. Aligned word fill/copy loops avoid calls to
flash-resident `memset`/`memcpy` on either chip. A recursive inspection of the
linked IRQ's direct calls, tail branches, and linker veneers found no XIP code
targets in either text demo or either runtime-feature comparison build.

The supplied ELF audit reports indirect/unresolved calls for review rather than
assuming they are safe. Its result covers code addresses, not a caller-supplied
font placed in flash. Keep the emulator's existing font in SRAM to preserve that
property for data reads as well.

Pixel expansion packs four output indices per 32-bit store using arithmetic
masks. It adds no font/attribute LUT. Active lines fill only sync and porch
regions before rendering the 640 active pixels, avoiding a second full-line
write. These choices address a concrete RP2040 deadline problem found in the
initial per-byte/O3-unrolled version.

At 378 MHz there are 12,000 system clocks per output line (about 31.75 us).
Native text needs more frequent palette DMA chains than doubled graphics.
Theoretical bandwidth and assembly review do not establish the real deadline
margin under SRAM contention, other DMA traffic, XIP activity, or interrupt
load. That remaining margin and serializer underrun must be measured on the
target board with the real emulator running.

## Preview and patch checks

`bash tests/render_demo.sh OUTPUT_DIR` runs the real demo's screen construction,
production text addressing/glyph helpers, and decoded production TMDS symbols.
It writes 640 x 480 PPM previews for 80 x 30, 80 x 25, and 40 x 25. The archive's
PNGs are lossless conversions of these outputs, not hardware captures.

The delivery patch is checked with `git apply --check` against the exact clean
base commit, then applied in that clean checkout. Its resulting source files
are compared to the packaged source tree. The delivery includes SHA-256 hashes
for the patch, source, firmware, previews, and validation files.

## Hardware acceptance still required

Confirm the three text geometries, one-pixel checker in 80-column modes, all
16 backgrounds with attribute blink disabled, live font changes, blink/cursor,
and graphics/text/graphics palette preservation. Measure the longest scanline
handler duration and check the PIO transmit-stall flag while the emulator is
under representative load. The RP2350 board test above confirms visible output,
not a timing margin under emulator load.
