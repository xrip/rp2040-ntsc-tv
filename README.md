# RP2040/RP2350 VGA, NTSC, HDMI, and TFT video output

This project gives VGA, color NTSC composite, HDMI, or TFT output from one
Raspberry Pi Pico framebuffer. VGA and NTSC can run together. HDMI and TFT are
single-output forms. The project builds for both RP2040/Cortex-M0+ and
RP2350/Cortex-M33.

The current dual-output path is made for one exact source format:

| Property | Current value |
|---|---:|
| Width | 320 pixels |
| Height | 240 pixels |
| Pixel format | 8-bit indexed color |
| Palette size | 256 RGB888 colors |
| Framebuffer stride | 320 bytes |
| Framebuffer size | 76,800 bytes |

VGA and NTSC read the same framebuffer. They do not read the same scanline
buffer. Each output has its own palette tables, line buffers, DMA channels,
IRQ, and signal generator.

The demo has three old-school scenes: a jumping ball, a rotating torus, and a
3D helix. They are drawn over a wavy black-and-white perspective checkerboard.
The objects use six color ramps and crossfades.

## Dual VGA and NTSC system map

```text
                         core 1
                    demo_render_frame()
                             |
                    320 x 240 index-8
                    front/back buffers
                             |
             graphics_present_framebuffer()
                      /              \
             VGA pending pointer   NTSC pending pointer
                    |                    |
             VGA frame boundary   NTSC frame boundary
                    |                    |
          VGA scanline generator  NTSC scanline generator
                    |                    |
             VGA line buffers     NTSC line buffers
                    |                    |
              DMA data/control     DMA data/control
                    |                    |
               PIO0 state machine       PWM
                    |                    |
                GPIO 6..13             GPIO 27
```

In the demo, core 0 starts the hardware and owns both DMA IRQ handlers. Core 1
draws frames. The graphics library itself does not start core 1; that is done
by `ntsc-tv.c`.

## Source files and ownership

### `ntsc-tv.c`

This is the application and demo layer. It:

- makes the RGB palette;
- makes lookup data for the floor, wave, ball, shadows, and 3D objects;
- draws a complete frame into a backbuffer;
- asks the graphics layer to present that buffer;
- changes the draw buffer after both outputs accept the request;
- runs the renderer on core 1;
- leaves core 0 free for the VGA and NTSC IRQ work.

The renderer uses two full framebuffers:

- the primary framebuffer, owned by the selected graphics adapter and returned
  by `graphics_get_framebuffer()`;
- `demo_backbuffer`, owned by the demo.

No full-frame copy is made during presentation. Only framebuffer pointers are
changed.

### `drivers/graphics`

This folder has the common public `graphics.h` interface, fonts, text/window
drawing helpers, and the one public `graphics_init()` function. The selected
output module supplies the other `graphics_*` display functions.

The same public calls are used for VGA, NTSC, dual output, HDMI, and TFT:

- `graphics_init()` sets any clock required by the selected form, then starts
  the selected output or outputs;
- `graphics_set_palette()` makes the selected output palette entries from one
  RGB888 color;
- `graphics_get_framebuffer()` returns the built-in 320 x 240 primary buffer;
- `graphics_present_framebuffer()` sends a buffer-swap request to every
  selected output and waits until all have accepted it;
- `graphics_set_buffer()` changes every selected active framebuffer pointer,
  but only when width is 320 and height is 240.

`graphics_modes.c` in the same folder is the shared scanline composer. It holds
the mode, the application buffer and its size, the offset, and the text-buffer
pointer, and turns them into one 320-byte line of palette indices for any
output row. VGA and NTSC call it once for each source row, each with its own
320-byte scratch line, because the two run from separate interrupts. The mode
work is therefore written once, not once for each backend.

Two API calls have no effect in the fixed 320 x 240 output forms:

- `graphics_set_bgcolor()`;
- `graphics_set_flashmode()`.

The border and every off-picture row use palette index 0, as the older driver
did.

### Modes

`graphics_set_mode()` selects how the composer reads the application buffer.
Any mode value not listed below keeps the direct path: the built-in 320 x 240
framebuffer is scanned out with no copy, exactly as before.

| Mode | Picture |
|---|---|
| `TEXTMODE_DEFAULT`, `TEXTMODE_160x100` | 80 x 30 text at the full line width |
| `TEXTMODE_53x30` | 40 x 30 text, every pixel sent twice |
| `GRAPHICSMODE_DEFAULT` | application buffer, 1x, centred, `y` offset applied |
| `GG_160x144` | 160-pixel window at source x 48, 1x, centred |
| `GG_160x144x4x3` | the same window, 2x wide, with a 2:3 line scale |

Text colors are the fixed 16 CGA colors. They do not go through
`graphics_set_palette()`, so an application keeps all 256 of its own colors and
cannot recolor text, exactly as in the older driver.

### Text is 640 samples wide

A graphics row is 320 indices and every output sends each one twice. Text does
not go through that path, because 80 columns of 8-pixel glyphs need 640
samples. Each backend writes those 640 samples straight into its own scanline
buffer, one sample for each pixel:

| Output | Text picture | Font | Sample |
|---|---|---|---|
| VGA | 640 x 480 | 8x16, 480 / 16 = 30 rows | one 8-bit DAC value |
| NTSC | 640 x 240 | 8x8, 240 / 8 = 30 rows | one PWM luminance value |

VGA therefore builds a line for every one of its 480 physical lines while a
text mode is selected, instead of one line for every two.

One NTSC sample is too short to carry a complete four-phase color cycle, so a
text pixel is sent as luminance only and the receiver makes the color back from
the pixel pattern itself. This is NTSC artifact color, the same result the older
driver gave, and it keeps all 640 samples instead of dropping to 320
double-width pixels.

The pixel loop stays in each backend rather than in the composer: the two have
different sample sizes and very different time budgets for one line. The
composer supplies the shared parts — the CGA colors, which mode is text, the
column count, and the address of the text row.

In both fonts bit 0 of a glyph byte is the leftmost pixel.

### Text lookup tables

Both text tables are built by the preprocessor from the fixed CGA colors, so
they are `const` and cost no RAM at all:

| Table | Size | Use |
|---|---:|---|
| `vga_dual_text_pairs` | 2,048 B | attribute and two glyph bits to two VGA bus values |
| `ntsc_text_sample` | 32 B | CGA color to one PWM luminance value |

VGA writes two pixels for each 16-bit store, so a text line costs four stores
for each character cell.

`vga_init()` copies the table and the 8x16 font into RAM, which costs 6 KB.
They cannot be read from flash while the picture is live: a text line makes
four table reads and one font read for each character cell, so 400 flash reads
land inside one 31.7 us VGA line, and each one shares the single QSPI bus with
whatever the other core is doing. The result was a shaking VGA text picture
while NTSC stayed steady, because an NTSC line is twice as long and reads only
the font. Every line builder, and the small mode helpers they call, are marked
`__time_critical_func` for the same reason.

The 6 KB is taken even by a build which never selects a text mode.

The RGB parts of the CGA set are only `0x00`, `0x55`, `0xaa`, or `0xff`. For
those four values the lower and upper dither levels of `vga_set_palette()` are
equal, so one bus value serves both frame phases and text never shimmers.

`GRAPHICSMODE_DEFAULT` and the two `GG_*` modes mask every source byte with
`0x1f`. This is what the older Master System driver did, because that emulator
keeps a priority bit in the upper bits of each picture byte.

### `drivers/vga-nextgen`

This is a portable VGA module with the CMake target `vga-nextgen`.

- `vga.c` is the optimized fixed 640 x 480 VGA scanout for a 320 x 240 indexed
  source;
- `vga.h` gives its internal backend API;
- `graphics.c` implements the public `graphics.h` API when VGA is used alone.

Linking this target adds `VGA=1` to the final program. The old emulator-specific
VGA driver and its large mode switch have been removed.

The VGA resistor output has two bits for each RGB channel. By default,
`VGA_ENABLE_DITHER=1` uses two 256-entry palette tables. The source-row and
frame bits change the order of the two DAC samples, as in the old VGA driver.
The phase is selected before the pixel loop; the loop still has one table read
for each source pixel. Set `VGA_ENABLE_DITHER=0` at compile time to remove the
second 512-byte table and all phase work.

### `drivers/ntsc-tv`

This is a portable NTSC module with the CMake target `ntsc-tv-driver`.

- `ntsc-tv.c` owns the PWM, DMA, IRQ, scanline buffers, palette tables, and
  framebuffer pointers;
- `ntsc-tv.h` gives its internal backend API and the default GPIO setting;
- `graphics.c` implements the public `graphics.h` API for NTSC-only and dual
  output.

Linking this target adds `NTSC_TV=1` to the final program. If `VGA=1` is also
present, this module's graphics adapter sends every graphics API action to both
drivers. The VGA-only adapter compiles to an empty unit in that form.

### `drivers/hdmi`

This is the fixed 320 x 240 indexed HDMI module. It uses one PIO state machine
for TMDS output, one PIO state machine for palette-address conversion, four DMA
channels, and an exclusive DMA IRQ. The default HDMI base pin is GPIO6. HDMI
uses a 378 MHz system clock and is not combined with VGA or NTSC.

Four TMDS control symbols use palette slots 252 through 255. Logical colors in
those four slots are remapped to slots 48 through 51, so the HDMI form has 252
independent palette colors.

### `drivers/st7789`

This is the fixed 320 x 240 indexed TFT module for the ST7789-compatible PIO
interface. It converts each palette entry to RGB565 and sends a complete frame
when `graphics_present_framebuffer()` is called. TFT does not change the system
clock.

## Clock tree

The selected form owns any required chip-wide clock change:

```text
NTSC or dual : clk_sys = 315 MHz
HDMI         : clk_sys = 378 MHz
VGA or TFT   : no clock change
```

NTSC and HDMI ask for a higher core voltage before making their clock change.

In dual output this clock is used by both outputs. VGA-only does not change the
system clock or core voltage; its PIO divider is made from the clock set by the
application.

### NTSC sample clock

The PWM uses:

```text
divider = 2
TOP + 1 = 11
sample rate = 315 MHz / 2 / 11
            = 14.318181... MHz
color subcarrier = sample rate / 4
                 = 3.579545... MHz
```

The DMA gets one request on every PWM wrap and sends one 16-bit compare value
per request.

### VGA pixel clock

The PIO divider is set from:

```text
315 MHz / 25.175 MHz
```

The PIO state machine sends one 8-bit VGA bus value for every pixel-clock
step.

The two outputs use the same system clock, but their scanline and frame counts
are different. They are not driven by one shared scanline counter.

## NTSC path

### Signal layout

The NTSC generator uses 908 samples per line and 262 lines per frame.

| Part | Value |
|---|---:|
| PWM sample rate | 14.318181... MHz |
| Samples per line | 908 |
| Lines per frame | 262 |
| Horizontal sync width | 68 samples |
| Back porch before burst | 8 samples |
| Color burst | 9 cycles, 36 samples |
| Active picture start | sample 172 |
| Active picture width | 640 samples |
| Source picture | 320 x 240 |

This is a 262-line, non-interlaced signal. The code does not make two 262.5-line
fields or an NTSC half-line. At the clock and line size above, the calculated
line rate is about 15.769 kHz and the frame rate is about 60.19 Hz.

The current line states are:

| Lines | State |
|---:|---|
| 0..9 | equalizing/sync pattern |
| 10..21 | horizontal sync, color burst, and blank level |
| 22..261 | 240 active source rows |

Only lines 0, 1, 10, and 11 make the complete base patterns. The other lines
in those two ranges get the same pattern by ping-pong buffer reuse.

Every source pixel makes two PWM samples. Two adjacent source pixels make one
complete four-phase color-subcarrier cycle:

```text
even pixel -> phase 0, phase 90
odd pixel  -> phase 180, phase 270
```

The active picture starts on NTSC line 22. Source row `y` is read from:

```c
framebuffer + y * 320
```

### NTSC palette

`graphics_set_palette(index, RGB888)` calls `ntsc_tv_set_palette()`. The driver
uses integer math to get luminance and two chroma parts. It then makes four
PWM values for the four subcarrier phases.

The normal palette form has two tables:

```text
ntsc_palette_even[256] : phase 0 and phase 90 packed into uint32_t
ntsc_palette_odd[256]  : phase 180 and phase 270 packed into uint32_t
```

The two tables use 2,048 bytes in total. One lookup gives the two 16-bit PWM
samples for one pixel.

With `NTSC_LOW_RAM=ON`, each phase pair is kept as two 4-bit values in one
byte. The two tables then use 512 bytes. The hot loop must unpack every table
result, so this setting trades CPU time for RAM.

With `NTSC_USE_SCRATCH_Y=ON`, the NTSC palette tables are put in scratch Y.
This is off in both supplied presets. The option does not check what other
code uses scratch Y and does not set core affinity.

### NTSC line buffers

There are two aligned line buffers:

```text
2 buffers x 908 samples x 2 bytes = 3,632 bytes
```

The buffers are used in ping-pong order. While the data DMA sends one buffer,
the IRQ writes the next NTSC line into the other buffer.

Active-line generation writes only the active 640-sample picture area. The
sync, burst, and blank parts already in that buffer are kept. Early frame lines
make the base sync and blank patterns before the buffers are used for active
video.

### NTSC DMA chain

NTSC uses two DMA channels:

1. The data channel sends 908 16-bit values to the PWM compare register. It is
   paced by the PWM-wrap DREQ.
2. The control channel writes the next scanline-buffer address into the data
   channel `READ_ADDR`, then chains back to the data channel.

The control channel reads from a two-entry address array with an 8-byte read
ring, so it changes between the two scanline buffers.

Only the data channel has a DMA IRQ. It uses `DMA_IRQ_0`. The handler:

1. clears the DMA interrupt;
2. selects the free line buffer from the scanline number;
3. accepts a pending framebuffer at line 0;
4. advances the line counter;
5. makes the selected scanline.

RP2350 DMA self-trigger is not used. A self-trigger would restart the data
channel before its `READ_ADDR` is changed to the other buffer. The one-word
control DMA gives the required address change first.

## VGA path

### Signal layout

The dual VGA driver makes standard-size 640 x 480 timing from the 320 x 240
source:

| Part | Value |
|---|---:|
| Pixel clock request | 25.175 MHz |
| Samples per line | 800 |
| Horizontal sync | samples 0..95 |
| Active picture start | sample 144 |
| Active picture width | 640 samples |
| Lines per frame | 525 |
| Active lines | 480 |
| Vertical sync | lines 490 and 491 |

Each source pixel becomes two VGA samples. Each source row is sent on two VGA
lines. The result is a 2 x nearest-size increase from 320 x 240 to 640 x 480.

### VGA 8-bit bus

The driver uses `PIO_VGA`, which is `pio0` by default, and one one-instruction
state machine:

```text
out pins, 8
```

With the default `VGA_BASE_PIN=6`, the bus is:

| GPIO | Bus bit | Use |
|---:|---:|---|
| 6..7 | 0..1 | blue level |
| 8..9 | 2..3 | green level |
| 10..11 | 4..5 | red level |
| 12 | 6 | horizontal sync |
| 13 | 7 | vertical sync |

The repository does not give the VGA resistor-network schematic. The code only
defines the digital pin values and timing.

### VGA palette

The VGA palette has 256 `uint16_t` entries and uses 512 bytes. One entry has
two adjacent 8-bit VGA bus values. The two values use lower and upper 2-bit RGB
levels. This gives a spatial two-sample approximation of the requested RGB888
color while keeping the physical output at two bits per RGB channel.

The sync bits are set in every active palette value, so palette output cannot
change sync state.

### VGA line buffers

VGA has four aligned 800-byte buffers:

- one blank-line template;
- one vertical-sync template;
- two active line buffers.

The total is 3,200 bytes for all four buffers.
Only the 640-byte active area of an active buffer is changed for each source
row. The sync and porch data come from the template copied during init.

An active line buffer is sent twice. During the second physical line, the CPU
makes the other active buffer for the next source row.

### VGA DMA chain

VGA uses two DMA channels:

1. The data channel sends 200 32-bit words to the PIO TX FIFO. It is paced by
   the PIO TX DREQ.
2. The control channel writes `vga_dual_next_line_address` into the data
   channel `READ_ADDR`, then chains back to the data channel.

The control channel uses `DMA_IRQ_1`. Its handler keeps the 525-line counter,
selects an active, blank, or vertical-sync buffer, and makes a future active
line while the present line is being sent.

## RP2040 and RP2350 CPU paths

The source format and output are the same on both chips, but the hot C loops
are different.

### RP2040 / Cortex-M0+

- VGA uses direct byte loads, 256-entry palette lookups, and 16-bit stores.
- The VGA active-line function is forced inline into the IRQ path.
- The VGA loop is unrolled by four groups.
- The normal NTSC loop does four palette lookups per source group and is
  unrolled by four groups.
- The code form is kept suitable for Thumb-1 and its smaller register set.

### RP2350 / Cortex-M33

- VGA loads four source indices together and joins two 16-bit palette results
  into each 32-bit store.
- The VGA active-line function is a separate `__time_critical_func`, so blank
  and repeated lines do not pay its larger register-save cost.
- The VGA loop is unrolled by four groups.
- NTSC loads four indices together and uses a two-group unroll. This is the
  smaller and quicker tested form for the Cortex-M33 output loop.

Both DMA handlers use `__time_critical_func`, so Pico SDK puts the hot IRQ code
in RAM. `drivers/ntsc-tv/ntsc-tv.c` also asks GCC for `O3` on its code. The root
build uses `O3`, LTO, whole-program work, function sections, and data sections.

## Framebuffer presentation

Both output drivers keep two framebuffer pointers:

- `active_framebuffer`, read by scanout;
- `pending_framebuffer`, set by the producer.

Presentation uses release/acquire memory fences so all pixel writes are public
before an IRQ accepts the pointer.

In dual output, `graphics_present_framebuffer()` works in this order:

1. put the new pointer into the VGA pending slot;
2. put it into the NTSC pending slot;
3. wait until NTSC accepts it at NTSC line 0;
4. wait until VGA accepts it when VGA prepares line 0;
5. return to the renderer.

Each output changes its pointer only at its own frame boundary, so each output
is free from a mid-frame pointer change. The function returns only after both
outputs have accepted the buffer.

This is frame-request synchronization, not signal or scanline lock. VGA and
NTSC have separate counters and different frame structures. For a short time,
one output may show the new buffer while the other still shows the old one.

In a single-output build the same call requests and waits for that one output
only.

Palette writes are direct. There is no pending palette and no full-palette
swap at vertical blank. The demo sets its palette before scanout starts.

## Core use

The current demo has this split:

| Core | Work |
|---|---|
| core 0 | init, DMA IRQ 0, DMA IRQ 1, status LED loop |
| core 1 | complete frame rendering and presentation wait |

`graphics_init()` installs and enables each selected output's exclusive DMA IRQ
handler on the core which calls it. In this demo that is core 0. An application
which calls it from another core will change IRQ ownership.

## Hardware resources and integration effects

The default dual form takes or changes these chip resources:

| Resource | Current use |
|---|---|
| System clock | changed to 315 MHz |
| Core voltage | changed to 1.30 V |
| DMA channels | four claimed channels |
| DMA IRQ 0 | exclusive NTSC handler |
| DMA IRQ 1 | exclusive VGA handler |
| PIO | one program word and one state machine on PIO0 |
| PWM | the slice/channel used by GPIO27 |
| GPIO | GPIO27 and VGA GPIO6..13 |
| Core | IRQs enabled on the core which calls `graphics_init()` |

There is no deinit path. Claimed DMA channels, PIO state, IRQ handlers, and the
PWM output stay active for the life of the program.

VGA-only takes two DMA channels, DMA IRQ 1, one PIO state machine, and GPIO
6..13. NTSC-only takes two DMA channels, DMA IRQ 0, one PWM slice/channel, and
GPIO27. Only a form with NTSC changes the system clock and core voltage.

The exclusive IRQ handlers are important when this code is put into a larger
program: another module cannot also install an exclusive handler on the same
DMA IRQ. A shared IRQ design is not present now.

## Main static RAM use

The largest fixed blocks in the default dual demo are:

| Block | Bytes |
|---|---:|
| Primary framebuffer | 76,800 |
| Demo backbuffer | 76,800 |
| VGA line buffers | 3,200 |
| NTSC line buffers | 3,632 |
| VGA palette | 512 |
| NTSC palette, normal | 2,048 |
| NTSC palette, `NTSC_LOW_RAM` | 512 |
| Demo depth buffer | 16,384 |

The demo also has ball sprites, floor tables, wave tables, and small object
tables. SDK state, stacks, and code-in-RAM add to the final RAM use.

`NTSC_LOW_RAM` only changes the NTSC palette form. It does not remove either
full framebuffer or the VGA line buffers.

## Current format and mode limits

The scanout is not format-neutral.

- A graphics line is 320 palette indices; only text gets the full 640 samples.
- It treats every source byte as one palette index.
- It has no BPP field and no stride argument in the public API.
- Packed 1-, 2-, or 4-BPP data will be read as index-8 data.
- RGB565 or RGB888 data will also be read as index-8 data.
- The VGA signal is fixed at 640 x 480 timing.
- The NTSC signal and active window are fixed.
- Only the modes listed above crop, scale, or centre; the rest are direct.
- The composer is used by VGA and NTSC only. HDMI and TFT still take the direct
  320 x 240 buffer and ignore `graphics_set_mode()`.

Changing the 320 x 240 line and frame size itself is not enough. The row address
math, scanline loops, palette form, active area, DMA count, and line buffer
sizes are connected to the present 320 x 240 x 8 design.

## NTSC line-state note

The current NTSC generator relies on line-buffer reuse. Some non-active line
numbers do not write a new full line; they keep the pattern already in that
ping-pong buffer. Also, the two conditions named as vertical blanking after the
active picture test line numbers 262 and 263, while the line counter wraps at
262. Those conditions cannot run in the current frame counter.

This does not change the architecture described above, but it is an important
current code limit if the NTSC vertical layout is changed.

## Build system

The project uses CMake 3.21 or newer, Pico SDK, and C23. Four CMake options
select the output modules:

| CMake option | Default | Effect |
|---|---:|---|
| `VGA` | `ON` | link `vga-nextgen` and set `VGA=1` |
| `NTSC_TV` | `ON` | link `ntsc-tv-driver` and set `NTSC_TV=1` |
| `HDMI` | `OFF` | link `hdmi` and set `HDMI=1` |
| `TFT` | `OFF` | link `st7789` and set `TFT=1` |
| `NTSC_LOW_RAM` | `OFF` | use the 512-byte compact NTSC palette |
| `NTSC_USE_SCRATCH_Y` | `OFF` | put NTSC palette tables in scratch Y |
| `GRAPHICS_BUILTIN_FRAMEBUFFER` | `ON` | keep the static 320 x 240 buffer |
| `GRAPHICS_NO_CLOCK_SETUP` | `OFF` | let the application own the system clock |

Turn `GRAPHICS_BUILTIN_FRAMEBUFFER` off when the application gives its own
picture buffer through `graphics_set_buffer()`. This drops 76,800 bytes of RAM
and makes `graphics_get_framebuffer()` give `NULL`.

Turn `GRAPHICS_NO_CLOCK_SETUP` on when the application must fix the system clock
itself, for example before it starts a second core. `graphics_init()` then makes
no clock or core-voltage change, and the application has to supply the clock the
selected form needs: 315 MHz for NTSC or dual, 378 MHz for HDMI.

This gives five supported forms:

| Form | CMake values | Driver targets |
|---|---|---|
| VGA | `VGA=ON`, `NTSC_TV=OFF` | `graphics`, `vga-nextgen` |
| NTSC | `VGA=OFF`, `NTSC_TV=ON` | `graphics`, `ntsc-tv-driver` |
| Dual | `VGA=ON`, `NTSC_TV=ON` | all three targets |
| HDMI | `HDMI=ON`, all other outputs off | `graphics`, `hdmi` |
| TFT | `TFT=ON`, all other outputs off | `graphics`, `st7789` |

CMake stops with an error if every output is off, if HDMI or TFT is combined
with VGA or NTSC, or if HDMI and TFT are both selected.

### Use in another project

The NTSC module is self-contained under `drivers/ntsc-tv`. Copy that folder
next to the existing `drivers/graphics` folder, then add:

```cmake
add_subdirectory(drivers/ntsc-tv)
target_link_libraries(your_program PRIVATE graphics ntsc-tv-driver)
```

The interface target adds the source files, include path, Pico hardware
libraries, and `NTSC_TV=1`. It also pre-includes `ntsc-tv.h`, so an older
`graphics.h` can still get its display-size and `RGB888` definitions from the
selected driver. Application code continues to include only `graphics.h` and
use `graphics_init()`, `graphics_set_buffer()`, and `graphics_set_palette()`.

For dual output, also copy this project's refactored `drivers/vga-nextgen`
folder, then add and link VGA:

```cmake
add_subdirectory(drivers/vga-nextgen)
target_link_libraries(your_program PRIVATE
        graphics
        vga-nextgen
        ntsc-tv-driver
)
```

No driver source has to be included by the application. The two target compile
definitions select the dual graphics adapter automatically.

The old multi-mode VGA driver cannot be used in this dual form because it owns
the same public `graphics_*` names. NTSC-only use needs only the new
`drivers/ntsc-tv` folder.

`pico_add_extra_outputs()` makes ELF, BIN, HEX, disassembly, and UF2 output.

### CLion and command-line presets

Six configure presets are present. The default two build dual VGA and NTSC:

```powershell
cmake --preset rp2040
cmake --build cmake-build-rp2040 --target ntsc-tv
```

```powershell
cmake --preset rp2350
cmake --build cmake-build-rp2350 --target ntsc-tv
```

The presets select:

| Preset | Board | Platform | CPU |
|---|---|---|---|
| `rp2040` | `pico` | `rp2040` | Cortex-M0+ |
| `rp2350` | `pico2` | `rp2350-arm-s` | Cortex-M33 |
| `rp2040-hdmi` | `pico` | `rp2040` | Cortex-M0+ |
| `rp2350-hdmi` | `pico2` | `rp2350-arm-s` | Cortex-M33 |
| `rp2040-tft` | `pico` | `rp2040` | Cortex-M0+ |
| `rp2350-tft` | `pico2` | `rp2350-arm-s` | Cortex-M33 |

Both use `MinSizeRel`, Ninja, UF2 output, VGA plus NTSC, normal NTSC palette
RAM, and no scratch placement. The RP2350 preset also sets
`PICO_NO_COPRO_DIS=ON`.

Picotool is fetched under:

```text
$env{TEMP}/picotool-2.3.0-x64-win
```

Build products are put in:

```text
bin/rp2040/MinSizeRel/
bin/rp2350-arm-s/MinSizeRel/
```

The file to copy to a board is `ntsc-tv.uf2`, `ntsc-tv-hdmi.uf2`, or
`ntsc-tv-tft.uf2` in the matching directory.

## Hardware

### NTSC composite

The default output pin is GPIO27.

```text
GPIO27 -- 75 ohm --+-- composite video out
                   |
                 560 pF
                   |
                  GND
```

The PWM duty values make the sync, blank, luminance, chroma, and color-burst
levels. The RC network changes the PWM into an analog signal.

### VGA

The default VGA bus is GPIO6 through GPIO13. A suitable RGB resistor network,
sync connection, ground connection, and VGA connector are required. Their
electrical schematic is not part of this repository.

### HDMI

The default clock differential pair starts at GPIO6 and the three data pairs
start at GPIO8. The module uses GPIO6 through GPIO13.

### TFT

The default pins are GPIO6 for CS, GPIO8 for reset, GPIO9 for the backlight,
GPIO10 for D/C, GPIO12 for data, and GPIO13 for clock.

## Start flow

The current `main()` order is:

1. make all demo palettes and lookup tables;
2. call `graphics_init()` on core 0;
3. start the selected display hardware;
4. make a short LED start pattern;
5. start `core1_entry()`;
6. draw and present frames on core 1 forever;
7. run a slow status LED pattern on core 0 while IRQs do scanout work.

## Credits

The NTSC signal work is based on KenKenMkIISR's
[`rp2040_pwm_ntsc_textgraph2`](https://github.com/KenKenMkIISR/rp2040_pwm_ntsc_textgraph2)
and on the PWM/DMA signal-generation idea by
[`lovyan03`](https://github.com/lovyan03/).
