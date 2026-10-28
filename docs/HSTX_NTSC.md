# RP2350: HSTX HDMI and color NTSC together, without USB

This change is based on `vga-tv-hybrid` commit
`0a01d6283b7c24cfafc71908ac506076d2763e4f`. It adds `HDMI_HSTX=ON` as a separate
backend. `HDMI=ON` still selects the existing PIO implementation.

The new configuration drives both connectors continuously from the same
application framebuffer or text VRAM. It targets RP2350 Arm with a 12 MHz
crystal and the GPIO12..19 HDMI layout used by M2P2. RP2040 does not have HSTX.
The existing VGA/NTSC/PIO-HDMI configurations remain available.

## Build and apply

From a checkout of the base revision:

```sh
git apply --check rp2350-hstx-ntsc-no-usb.patch
git apply rp2350-hstx-ntsc-no-usb.patch
```

Set `PICO_SDK_PATH` to your Pico SDK directory. Tested with Pico SDK **2.3.1**
and Arm GNU Toolchain **13.3.Rel1** (GCC 13.3.1), with CMake and Ninja.

```sh
cmake --preset rp2350-hstx-ntsc
cmake --build cmake-build-rp2350-hstx-ntsc --target ntsc-tv
```

Output: `bin/rp2350-arm-s/MinSizeRel/ntsc-tv-hstx-composite.uf2`.
The demo starts with color graphics, then cycles through its text and graphics
parts. The graphics part is useful for checking composite color lock first.

For the shared VRAM/font demonstration (80x30, 80x25, 40x25, then 3D graphics):

```sh
cmake --preset rp2350-hstx-ntsc-text
cmake --build cmake-build-rp2350-hstx-ntsc-text --target ntsc-tv
```

Output: `bin/rp2350-arm-s/MinSizeRel/ntsc-tv-hstx-composite-text.uf2`.

This preset uses application-owned text and graphics buffers and disables the
driver's built-in framebuffer. It exercises text-to-graphics-to-text changes,
all 16 HDMI text foreground/background colors, font edits, and the cursor.

The equivalent output switches are:

```sh
cmake -S . -B build-hstx-ntsc -G Ninja \
  -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=pico2 \
  -DHDMI_HSTX=ON -DNTSC_TV=ON -DVGA=OFF -DHDMI=OFF -DTFT=OFF
cmake --build build-hstx-ntsc --target ntsc-tv
```

`NTSC_LOW_RAM=ON` is also supported. `NTSC_TV=OFF` selects HSTX alone with the
same clock profile. HSTX with VGA, TFT, or legacy PIO HDMI is rejected by CMake.
UF2 loading through BOOTSEL remains available; USB is disabled in the running
application, not in the boot ROM.

## Clock plan: preserve the existing NTSC timebase

| Consumer | Source and divider | Nominal frequency |
|---|---|---:|
| CPU, bus, PWM input | PLL_SYS, clk_sys /1 | 315 MHz |
| PWM counter | clk_sys /2 | 157.5 MHz |
| PWM wrap / NTSC samples | counter /11, TOP=10 | 14.318181818 MHz |
| NTSC color subcarrier | four samples per cycle | 3.579545455 MHz |
| HSTX | PLL_USB, clk_hstx /1 | 126 MHz |
| TMDS serial data | HSTX DDR | 252 Mbit/s per data lane |
| HDMI pixel clock | HSTX /5 | 25.2 MHz |
| UART/SPI peripheral clock | clk_sys /3 | 105 MHz |
| USB and ADC | clocks stopped, peripherals held in reset | unavailable |

PLL_USB is reprogrammed to `12 MHz * 105 /5 /2 = 126 MHz` (VCO 1260 MHz).
There is no fractional system-clock divider and no change to NTSC's PWM
divider, wrap, sample count, burst, or analog palette calculations. Actual
frequency accuracy retains the dependence on the board's existing crystal.

`graphics_init()` runs once on core 0, before launching core 1 or initializing
UART/SPI. It raises the regulator to the project's existing 1.30 V setting,
sets SYS to 315 MHz, moves PERI away from PLL_USB, stops USB/ADC, then sets the
independent HSTX clock. HSTX is stopped before changing SYS and its peripheral
is reset before starting scanout. The RP2350 XIP setup uses flash divider 4
and RX delay 4, as in the existing RP2350 HDMI configuration.

315 MHz is the project's existing system overclock; build success does not
qualify a particular RP2350, flash chip, or board for it.

The driver checks the actual PLL, clock mux, and divider registers, in addition
to SDK-reported frequencies. This catches a misleading setup where generic
`clock_configure()` caches 126 MHz while the integer-only HSTX divider actually
produces another frequency. Do not call `set_sys_clock_khz()` after video
initialization: SDK helpers normally assume PLL_USB is still 48 MHz.

With `GRAPHICS_NO_CLOCK_SETUP=ON`, the application must establish this exact
315/126 MHz profile itself, leave HSTX out of reset, and stop USB/ADC clocks
before `graphics_init()`. Hardware validation still runs. This option does not
make arbitrary system frequencies compatible with the NTSC generator.

## Wiring

Use the board's existing HDMI electrical interface and composite output
circuit. These are RP2350 GPIO numbers, not HDMI connector pin numbers.

| Signal | GPIO |
|---|---:|
| HDMI clock minus / plus | 12 / 13 |
| HDMI data 0 minus / plus (blue/control) | 14 / 15 |
| HDMI data 1 minus / plus (green) | 16 / 17 |
| HDMI data 2 minus / plus (red) | 18 / 19 |
| NTSC composite | 28 by default (`NTSC_PIN_OUTPUT`) |

The old PIO HDMI pin options do not remap HSTX. This backend has the above
fixed pairing and polarity. Its video is DVI-compatible TMDS over the HDMI
connector; audio, data islands, EDID/DDC, and USB are not implemented here.

## Framebuffers, text, and synchronization

- Graphics use the existing 320x240 indexed framebuffer and shared mode
  composer. Every one of the 256 palette indices is available, including
  252..255. A 256-entry RGB888 palette is quantized to BGR555 for HSTX scanout.
  The serializer doubles each horizontal pixel, and each source row supplies
  two HDMI lines. NTSC keeps its existing color encoding.
- Text borrows the same `graphics_text_config_t` VRAM and font for both outputs.
  HDMI renders 640x480; NTSC samples alternate vertical rows for its 240-line
  picture while retaining 640 horizontal samples. Font stride, bit order,
  40/80 columns, blink, cursor, page offsets, and VRAM wrapping are shared.
- NTSC text retains the original luminance/artifact-color rendering. It does
  not reproduce the 16 HDMI RGB text colors exactly. Use the graphics part
  to assess NTSC color lock and palette output.
- `graphics_set_palette()` and the text/font setters may be used before
  `graphics_init()`. The demo supplies its font explicitly; the driver does
  not allocate or copy a font or text VRAM.
- The HSTX adapter uses four 640-pixel BGR555 scanline slots (5120 bytes), a
  512-byte graphics palette, a 32-byte text palette, and a 320-byte staging
  line. It does not allocate a full RGB framebuffer. Low-level command lists
  and small state are additional, as is RAM-resident code.
- Keep VRAM, fonts, palette indices, and framebuffers in SRAM. The IRQ code
  and its internal command/color tables are explicitly placed in SRAM. This
  is not a PSRAM scanout implementation.

`graphics_present_framebuffer()` publishes the new full-size framebuffer to
both outputs, then waits for both to latch it at their respective frame
boundaries. Only then may a producer redraw the previous buffer. The outputs
are not frame locked: HSTX is 60 Hz, while the existing NTSC raster retains
its own frame rate. A frame can reach the two connectors at slightly different
times. `graphics_wait_vblank()` follows HSTX in the combined build.

Use one producer for `graphics_present_framebuffer()` and call it with
interrupts enabled, outside an IRQ. `graphics_set_buffer()` remains an
immediate binding operation. It takes a coherent source/geometry snapshot
with the scanout IRQs, but does not retire the old borrowed allocation for
the caller. After rebinding source/text/font storage, keep the previous
allocation alive for a full frame; two `graphics_wait_vblank()` calls are a
safe allowance for both outputs. Direct writes to bound VRAM are visible
line by line and are not an atomic screen update.

## DMA and interrupt ownership

HSTX dynamically claims two DMA channels and uses a shared `DMA_IRQ_1` handler
at the highest interrupt priority. NTSC retains its two dynamically claimed
DMA channels and `DMA_IRQ_0`. No PIO state machine is used by either output.
Both scanout handlers run on core 0; the normal demo renders on core 1.

The HSTX pipeline alternates command and pixel transfers for active lines.
Native 640-pixel lines use 32-bit DMA; 320-pixel lines use 16-bit DMA, whose bus
replication supplies horizontal doubling to the encoder. Command-only black
lines need no extra pixel buffer. DMA state is published while the completed
channel is disabled, with a memory barrier before re-enabling it. Only owned
IRQ bits are acknowledged.

The low-level API supports stop/start/deinit on its owning core. Stop disables
both DMA channels before aborting either, and restart resets the HSTX FIFO.
It does not retune PLLs or stop NTSC. The higher-level graphics adapter is
initialized once; call the low-level lifecycle only when managing its callbacks
and application state deliberately.

Debugger-visible diagnostics:

- `hstx_video_frame_count`: scheduled HDMI frames;
- `hstx_video_restart_count`: recovered missed DMA deadlines. This should stay
  at zero during normal continuous operation. A rising value means interrupt
  latency or rendering load needs attention; recovery is not timing validation.

## Validation and board check

Host tests exercise the production renderer and shared text implementation:

```sh
bash tests/run_hstx_tests.sh
bash tests/run_host_tests.sh
```

The new tests use ASan/UBSan and cover BGR555 channel order/quantization, all
256 graphics indices, glyph patterns and attributes, 40/80 columns, font
bit order and stride, wrapping, blink/cursor, borders, and guarded output
buffers. They do not model DMA timing or the analog signal.

The patch was cross-compiled to ELF/UF2 with the toolchain above, including the
ordinary and text HSTX+NTSC demos. Review the ELF/map when changing optimization
flags or compiler versions: even a non-const static lookup table can be moved
to XIP by LTO, and memset-like loops can turn into flash-resident library calls.
The HSTX build explicitly prevents those loop transformations in both output
renderers and places their hot lookup tables in RAM.

Recorded build results (RAM and scratch regions listed separately):

| RP2350 configuration | ELF/UF2 | FLASH | RAM | SCRATCH_X / SCRATCH_Y |
|---|---|---:|---:|---:|
| HSTX + NTSC, ordinary demo | Pass | 28,288 B | 224,556 B | 2048 / 2048 B |
| HSTX + NTSC, shared-text demo | Pass | 29,668 B | 222,604 B | 0 / 2048 B |

The HSTX host run passed 24,178,042 checks over 32,952 rendered text rows;
the existing descriptor suite under HSTX+NTSC passed another 52,341 checks.
An ELF/disassembly audit confirmed that both working IRQ paths, HSTX recovery,
shared IRQ dispatch, command/color tables, and the demo font reside in SRAM.

The old RP2040 VGA+NTSC and PIO-HDMI configurations were also cross-compiled.
This particular SDK/toolchain combination needed the local diagnostic linker
flag `-Wl,-u,__wrap_remquof` to retain a float wrapper under LTO. The identical
link failure without that flag was reproduced on the untouched base commit;
the workaround is not added to this patch's presets or sources. Both legacy
configurations produced ELF/UF2 with the flag. It is not needed for the new
RP2350 HSTX builds.

Physical validation remains required. On the board, check both connectors at
once, NTSC color in the graphics demo, text/graphics transitions, sustained
3D rendering, and a stable zero restart counter. If measuring the composite
signal, the expected sample period is approximately 69.84127 ns and the
color-subcarrier period approximately 279.36508 ns, unchanged from the base
driver. HDMI should be recognized as 640x480 at 60 Hz.

## Origin

The low-level serializer and BGR555 line pipeline were adapted from
[DnCraptor/frank-quake2, psx_video.c at 5d865a0](https://github.com/DnCraptor/frank-quake2/blob/5d865a098f7e927e826c2ae9879e422854495669/drivers/hdmi/psx_video.c),
which credits fliperama86's pico_hdmi. `drivers/hdmi-hstx/LICENSE.pico_hdmi`
preserves its Unlicense text. The game renderer and GPL-licensed Quake code
are not copied into this backend. Existing project source notices are retained.
