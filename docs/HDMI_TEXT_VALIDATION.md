# HDMI text command transport: validation and RAM accounting

This report covers the first PIO revision. For the subsequent RP2350/M33
optimization against commit `231f1fa`, see
[HDMI_TEXT_RP2350.md](HDMI_TEXT_RP2350.md).

Baseline: `xrip/rp2040-ntsc-tv`, branch `vga-tv-hybrid`, commit
`34f401e07bb4ade91ec604512b086d9b368efad3`. Date: 2026-10-26.
This report supersedes the earlier byte-index text implementation's measurements.

Current upstream's GPIO12..19 assignment, RP2350 flash fixes, text descriptor
API, application VRAM/font and text/3D cycle are preserved. `CMakeLists.txt`,
`hdmi-text-demo.c`, `ntsc-tv.c`, `demo-graphics.h` and
`drivers/hdmi/hdmi.h` are byte-identical to the baseline. NTSC is unchanged.

The previous driver produced working output on the user's board.
**The new command-based PIO transport has not yet been tested on a physical
board.** Timing below is an instruction model, not a hardware measurement.

## Local integration review

The review removed four unused byte-index expansion helpers and shared the
word-fill loop between graphics bytes and text commands. The PIO program,
command format and assembly kernels are unchanged.

Arm GCC 15.2.1 builds both text/3D demos. RAM use is 210,932 bytes on RP2040
and 210,728 bytes on RP2350, plus the separate 2,048-byte scratch-Y stack.
Clang 23.1.1 passes the host tests with AddressSanitizer and
UndefinedBehaviorSanitizer. The PIO interpreter passes all 495,712 commands
and backpressure checks; both generated SDK headers match its instruction
words. Both linked IRQ audits have no flash code targets or unresolved calls.
The linked RP2040 IRQ cases also pass under WSL with the pinned trace tools,
checking output pixels, register preservation and memory bounds. Native Windows
Unicorn failed during memory mapping, so that trace run used Linux instead.

The toolchain and benchmark tables below describe the supplied patch archive.

## Supplied toolchain and builds

- Pico SDK 2.3.1, commit `079c6f39023649b154152db30f1d781e884879bc`.
- Arm GNU Toolchain 14.2.Rel1, GCC 14.2.1 (20241119).
- CMake 4.4.4, Ninja 1.13.2, picotool 2.3.1.
- `MinSizeRel` with repository O3/LTO settings; RP2040 M0+ and RP2350 M33.

The current text/3D application, original graphics application and controlled
same-buffer harness build for both chips, before and after optimization.
GPIO22 profiling also builds. No compiler warnings were reported.
Normal UF2s in the archive are the current text/3D demos with profiling disabled.

## Memory

| Application | RP2040 baseline → optimized | RP2350 baseline → optimized | Separate stack reservations |
|---|---:|---:|---:|
| Controlled full-feature harness | 97,568 → 97,568 B | 97,360 → 97,360 B | 2,048 B |
| Current text/3D demo | 210,928 → 210,928 B | 210,720 → 210,720 B | 2,048 B |
| Original graphics demo | 201,996 → 201,996 B | 201,788 → 201,788 B | 4,096 B |

RAM-region values run from SRAM origin to the end of the fixed 2,048-byte heap
reservation. Scratch-bank stack reservations are separate. The driver adds no
runtime heap allocation. These are linked sizes, not dynamic heap/stack peaks.

The workspace remains **4,896 B**. The controlled harness retains identical
76,800-byte graphics, 4,800-byte text and 4,096-byte font allocations on both
sides, with runtime-variable text features kept reachable through LTO.
The current demo's two application graphics buffers already occupy 153,600 B
in the baseline; they were not added by this optimization.

| Controlled build | RP2040 before → after | RP2350 before → after |
|---|---:|---:|
| Occupied `.data`, including SRAM code/font | 5,776 → 6,064 B | 5,652 → 6,016 B |
| `.bss` | 87,328 → 87,328 B | 87,120 → 87,120 B |
| Existing alignment gap | 2,224 → 1,936 B | 2,268 → 1,904 B |

Occupied sections grow **288 B / 364 B**, absorbed by existing padding before
the aligned workspace. Zero linked growth is established for these applications;
another emulator's link may expose the occupied-code increase or cross an
alignment boundary. Fixed workspace size and no extra VRAM/font copy are the
implementation guarantees.

Linked RP2040 tracing records software IRQ stack peaks of **184 B at 80 columns
and 188 B at 40 columns**, versus 112 B normally and 132 B with dirty-divider
preservation in the baseline. The sampled maximum grows 56 B. Hardware exception
stacking is excluded; fixed stack reservations were not increased.

## Host and PIO tests

`bash tests/run_host_tests.sh` passes with AddressSanitizer/UndefinedBehaviorSanitizer.
LeakSanitizer is disabled for the sandbox; the renderer does not allocate heap
buffers. Production text helpers pass **52,341 checks**, including live
VRAM/font edits, geometries, font heights/strides/bit orders, page offsets,
wrapping, attributes, cursor, invalid descriptors and allocation bounds.

For each of four RGB/BGR and differential-polarity combinations, production
codec functions pass:

- 1,024 symbol packing cases and 8,960 exact original graphics RGB round trips;
- 256 independent foreground/background pairs, decoded and disparity-checked;
- 64 in-place graphics/text/graphics workspace transitions with palette edits;
- **786,432** glyph/attribute/width/bit-order/blink command combinations;
- 20 solid-color/control commands.

The independent PIO instruction simulator verifies **495,712 commands** and
**3,965,696 palette addresses**. It also processes 8,000 pixels with randomized
FIFO backpressure, including 147 empty-TX and 42,511 full-RX stalls, without
loss/reordering. Unstalled throughput is exactly 59 instructions per eight
pixels. All current SDK-generated PIO headers match the independently assembled
12-instruction program. This is functional validation, not DMA/bus timing proof.

## Linked ARM execution and CPU comparison

The tools in [tests/performance](../tests/performance/README.md) execute the
actual linked RP2040 ELF in Unicorn and price instructions with the Arm M0+
TRM. They do not simulate bus contention, peripheral timing, higher-priority
interrupts or lock contention. Hardware exception entry/return is excluded.

All **39 baseline and 67 optimized cases** pass independent pixel, memory-guard
and r4..r11/SP preservation checks. Cases include font heights 8/14/16, stride
32, both bit orders, unaligned byte-addressed VRAM/font, wrap, cursor, repeated
blink transitions, and retained graphics-line duplication. Two further
baseline cases establish dirty-divider stack use.

Seven complete frames check **3,675 IRQ returns** and every physical output
pixel with the same guards. No traced hot path fetches code or data from flash;
the caller-provided test font and VRAM are in SRAM.

| Mode | Baseline cycles/frame | Optimized cycles/frame | Before → after, one 378 MHz core |
|---|---:|---:|---:|
| 80×30 | 4,046,765 | 1,145,686 | 64.23% → **18.19%** |
| 80×25 | 3,511,965 | 1,024,166 | 55.75% → **16.26%** |
| 40×25 | 3,431,965 | 886,166 | 54.48% → **14.07%** |

The feature-heavy 80×30 frame with dirty-divider preservation on every IRQ uses
1,257,738 ideal cycles, **19.96%**. Its longest sampled line is 4,126 cycles,
10.92 us, versus a 31.746 us line period. These M0+ estimates do not apply to M33.
See [HDMI_TEXT_PERFORMANCE.md](HDMI_TEXT_PERFORMANCE.md) for interpretation,
RAM/stack limits, DMA counts, the XIP decision and profiling instructions.

## IRQ residency and firmware

Seven optimized linked images pass recursive IRQ call-target audits: both
text/3D demos, both controlled builds, both graphics demos and GPIO22 profiling.
No reachable XIP function targets, unresolved targets or indirect calls require
review. Code residency does not move a caller's flash font into SRAM.

The GPIO build uses inline SIO set/clear operations and clears the marker on
every exit. The marker measures the IRQ body; hardware exception handling and
the function prologue/epilogue remain outside it.

| Normal image | ELF SHA-256 | UF2 bytes |
|---|---|---:|
| RP2040 text/3D | `e17978ef4280295d62a40862b05a92f9c84fa9cbcdd70c697cdd71a5b168f7af` | 57,344 |
| RP2350 text/3D | `0cf9785d1c12d0269980ded5c4202c26f1aa6468f6b2f2bbe9017c315f09fc13` | 58,880 |

UF2 structure, chip families and image types pass validation. Current RP2350
definitions include `PICO_EMBED_XIP_SETUP=1`, `PICO_FLASH_SPI_CLKDIV=4` and
`PICO_FLASH_SPI_RXDELAY=4`. The archive contains detailed build reports, maps,
metrics, logs, a controlled-memory harness and portable reproduction scripts.

## Preview, patch and hardware check

`bash tests/render_demo.sh OUTPUT_DIR` renders all three 640×480 previews using
the actual demo and production helpers. Its virtual-clock test confirms changes
at 0, 8, 16, 24 and 84 seconds and alternating graphics buffers. Supplied PNGs
are software renders, not HDMI captures.

The delivery workflow runs `git apply --check` against the exact baseline,
applies the patch in an isolated checkout, and compares every resulting source
file and executable bit with the packaged source. Results and SHA-256 hashes
are included with the deliverables.

On hardware, check the pixel checkerboard, foreground/background palette,
live font changes, blink/cursor and graphics/text transitions. Measure profiling
GPIO duty and peak duration with the actual emulator and inspect serializer
underrun/stall behavior. The PIO programs now use 22 instruction words and
require absolute addresses 0..11 on the address-converter PIO, normally PIO0.
