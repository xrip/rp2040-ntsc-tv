# ntsc-tv

A C-based application for the Raspberry Pi Pico that generates a color composite NTSC video signal using single DAC pin.

### Schematic

```
GPIO27 -- 75 ohm --- Video Out
　　　　　　　　|
　　　　　　　=== 560 pF
　　　　　　　　|
　　　　　　　GND
```

## Project Overview

This project demonstrates the use of the Pico's PWM and DMA peripherals to create a stable, color video output with minimal CPU intervention. The main application generates a dynamic, wavy checkerboard pattern with a 256-color gradient.

The core of the project lies in `ntsc-tv-out.h`, which handles the low-level NTSC signal generation, and `ntsc-tv.c`, which provides the video content to be displayed.

## Key Technologies

*   **Language:** C
*   **Platform:** Raspberry Pi Pico (RP2040)
*   **Build System:** CMake
*   **Core Pico SDK Libraries:**
    *   `hardware_dma`
    *   `hardware_pwm`
    *   `pico_multicore`

## Hardware Required

*   Raspberry Pi Pico RP2040/RP2350
*   A display with a composite video input (e.g., an old CRT TV).
*   Resistor and capacitor for the DAC.


## Building and Running

### Prerequisites

*   A configured Raspberry Pi Pico SDK environment.
*   The `PICO_SDK_PATH` environment variable must be set.

### Building

The project is built using CMake. The following commands can be used to build the project:

```bash
mkdir build
cd build
cmake ..
make
```

The build output, including the `.uf2` file for flashing, will be located in the `bin/rp2040/MinSizeRel/` directory.

### Running

1.  Connect the Raspberry Pi Pico to your computer in BOOTSEL mode.
2.  Copy the `ntsc-tv.uf2` file to the Pico's mass storage device.
3.  The Pico will reboot and start generating the NTSC signal on GPIO 27.

## Credits

This project is based on the work of KenKenMkIISR and their project [rp2040_pwm_ntsc_textgraph2](https://github.com/KenKenMkIISR/rp2040_pwm_ntsc_textgraph2).