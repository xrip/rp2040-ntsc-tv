
# GEMINI.md

## Project Overview

This project, `ntsc-tv`, is a C application for the Raspberry Pi Pico that
drives a display from one 320 x 240 8-bit indexed framebuffer. Four outputs are
available: VGA, color composite NTSC, HDMI, and an ST7789 TFT. VGA and NTSC can
run at the same time; HDMI and TFT are single-output forms. It builds for both
RP2040 and RP2350.

The demo in the root `ntsc-tv.c` has two parts and swaps between them: a
graphics part with a jumping ball, a rotating torus, and a 3D helix over a wavy
perspective checkerboard, and a text part with a DOOM block-character logo over
the DOOM fire, drawn with CP437 shade characters. The display drivers live under
`drivers/`, and `README.md` is the detailed reference for them.

## Key Technologies

*   **Language:** C23
*   **Platform:** Raspberry Pi Pico and Pico 2 (RP2040 and RP2350)
*   **Build System:** CMake 3.21 or newer, with Ninja presets
*   **Core Pico SDK Libraries:**
    *   `hardware_dma`
    *   `hardware_pwm` (NTSC signal)
    *   `hardware_pio` (VGA, HDMI, TFT signal)
    *   `pico_multicore`

## Building and Running

### Prerequisites

*   A configured Raspberry Pi Pico SDK environment.
*   The `PICO_SDK_PATH` environment variable must be set.

### Building

Use the configure presets rather than a bare `cmake ..`, because the platform,
board, and output selection come from them:

```bash
cmake --preset rp2040
cmake --build cmake-build-rp2040 --target ntsc-tv
```

`rp2040` and `rp2350` build dual VGA plus NTSC. `rp2040-hdmi`, `rp2350-hdmi`,
`rp2040-tft`, and `rp2350-tft` build the single-output forms.

Build output, including the `.uf2` file, is written to
`bin/rp2040/MinSizeRel/` or `bin/rp2350-arm-s/MinSizeRel/`.

### Running

1.  Connect the board in BOOTSEL mode.
2.  Copy `ntsc-tv.uf2`, `ntsc-tv-hdmi.uf2`, or `ntsc-tv-tft.uf2` to the mass
    storage device.
3.  Default pins: composite on GPIO28 (`NTSC_PIN_OUTPUT`), VGA on GPIO6..13
    (`VGA_BASE_PIN`).

## Development Conventions

*   C, following Raspberry Pi Pico SDK conventions.
*   Signal generation (`drivers/vga`, `drivers/ntsc-composite`,
    `drivers/hdmi`, `drivers/st7789`) is kept separate from application logic
    (`ntsc-tv.c`). Mode handling shared by the outputs lives in
    `drivers/graphics/graphics_modes.c`.
*   Applications use only `graphics.h`.
*   The build is optimized for size and speed: `O3`, LTO, whole-program,
    function and data sections.
*   Both cores are used. Core 0 runs init and owns the scanout DMA interrupts;
    core 1 renders frames.
*   Anything a scanout interrupt touches every line must be in RAM, not flash:
    mark code `__time_critical_func` and copy hot tables out of flash at init.
    A flash read from a line interrupt shares the QSPI bus with the other core
    and can stall long enough to break the picture.
