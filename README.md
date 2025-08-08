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

## How it Works

The generation of the NTSC signal is a complex process that relies on precise timing and the clever use of the RP2040's peripherals. Here's a breakdown of how it's achieved:

### 1. System Clock Configuration

The foundation of the NTSC signal is a very specific system clock frequency. The code sets the system clock to **315 MHz**. This isn't an arbitrary number. The NTSC color subcarrier frequency is **3.579545... MHz**.  The chosen system clock of 315 MHz is mathematically related to this frequency:

*   `315 MHz / 88 = 3.579545... MHz` (The exact NTSC color subcarrier frequency)
*   `315 MHz / 22 = 14.318181... MHz` (Exactly 4 times the color subcarrier frequency)

This precise clock configuration is the key to generating a stable and accurate NTSC signal with zero frequency error.

### 2. PWM for Analog Voltage Levels

The project uses a Pulse-Width Modulation (PWM) peripheral to generate the analog voltage levels of the NTSC signal. The PWM output on `GPIO27` is connected to a simple resistor-capacitor (RC) circuit which acts as a Digital-to-Analog Converter (DAC). This circuit smooths the PWM signal into a continuous analog voltage.

The PWM is configured with a wrap value of 11. This means that the PWM counter will count from 0 to 10, and the duty cycle can be set to one of 12 levels. These levels are used to represent the different voltage levels of the NTSC signal, such as the sync level, blanking level, and the various color levels.

### 3. DMA for High-Speed Data Transfer

The CPU is not fast enough to feed the PWM peripheral with the data for each pixel in real-time. This is where the Direct Memory Access (DMA) comes in. The DMA is configured to read data from a scanline buffer in memory and write it directly to the PWM's compare register. This happens without any CPU intervention, freeing up the CPU for other tasks.

The project uses two DMA channels in a "ping-pong" configuration. While one DMA channel is busy sending the data for the current scanline, the other DMA channel's buffer is being filled with the data for the next scanline. When the first DMA channel finishes, the second one starts, and the first one's buffer is then refilled. This process repeats continuously, ensuring a seamless and uninterrupted video signal.

### 4. Generating a Scanline

The `ntsc_generate_scanline` function is the heart of the signal generation. It's responsible for creating the data for a single scanline of the NTSC signal. This function is called by the DMA interrupt handler.

A complete NTSC frame consists of 262 scanlines. The `ntsc_generate_scanline` function handles three types of scanlines:

*   **Sync and Blanking Lines:** These lines contain the vertical and horizontal sync pulses that tell the television where to position the electron beam. They also include the color burst, which is a reference signal for the color information.
*   **Active Video Lines:** These lines contain the actual image data. For each pixel in the framebuffer, the function looks up the corresponding color in the `ntsc_palette` and writes the four NTSC phase values to the scanline buffer.
*   **Vertical Blanking Lines:** These are blank lines that follow the active video lines, before the start of the next frame.

### 5. Color Encoding

Colors are encoded in the NTSC signal by modulating a color subcarrier signal. The `ntsc_set_color` function takes an RGB color and converts it into the four NTSC phase values that are stored in the `ntsc_palette`. This is done by calculating the luminance (brightness) and chrominance (color) components of the signal. The phase of the chrominance signal determines the hue, and the amplitude determines the saturation.

By combining these techniques, the project is able to generate a stable and colorful NTSC video signal using the Raspberry Pi Pico's peripherals, with minimal load on the CPU.

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
