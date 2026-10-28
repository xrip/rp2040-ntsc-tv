# Native HDMI text with borrowed VRAM and font

This performance revision targets the HDMI-only build of `vga-tv-hybrid`, at
commit `231f1fa7e50368ee07fb72330377a15c0208ede5`. It preserves the current
RP2350 flash setup, GPIO12..19 assignment, and text/3D demo cycle. It does not
modify the NTSC driver or enable simultaneous HDMI/NTSC output.

This is the PIO HDMI path. The tree also has a second HDMI backend,
`drivers/hdmi-hstx/`, which does run HDMI and NTSC together on RP2350; see
[HSTX_NTSC.md](HSTX_NTSC.md). The two paths are separate, and this document
describes the PIO one.

## Output modes

The physical signal is 640 x 480 at 60 Hz: 25.2 MHz pixels, 800 total pixels per
line, and 525 lines per frame. The existing 378 MHz system clock, GPIO assignment,
two PIO state machines, and four DMA channels remain in use.
The fast text path uses 22 PIO instruction words: the glyph expander reserves
absolute addresses 0..11 and the serializer uses ten more. Integrations sharing
PIO0 must leave that instruction range free. Other PIO programs are not erased.

| API mode | Text cells | Cell size with an 8 x 16 font | Visible area |
|---|---:|---:|---|
| `TEXTMODE_80x30` / `TEXTMODE_DEFAULT` | 80 x 30 | 8 x 16 pixels | 640 x 480 |
| `TEXTMODE_80x25_COLOR` / `TEXTMODE_80x25_BW` | 80 x 25 | 8 x 16 pixels | 640 x 400, centered; 40-pixel top/bottom borders |
| `TEXTMODE_40x25_COLOR` / `TEXTMODE_40x25_BW` | 40 x 25 | 16 x 16 pixels | 640 x 400, centered; each glyph pixel doubled horizontally |

`TEXTMODE_53x30` keeps the repository's legacy 40 x 30 layout;
`TEXTMODE_160x100` aliases the 80 x 30 layout. The BW mode identifiers choose
geometry; the supplied attribute bytes still determine colors.

Graphics remains 320 x 240 indexed data, doubled in both directions. Selecting
another graphics mode identifier does not add a packed/planar VGA graphics
decoder to HDMI. The source buffer must still contain 320 x 240 eight-bit
palette indices.

This is the scanout portion of a DOS text display. It supports character and
attribute bytes, a supplied font, page start, row pitch, cursor shape, blink,
bright backgrounds, and optional address wrapping. The emulator remains
responsible for BIOS/CRTC state and for publishing changes through this API.
It does not implement a complete VGA register machine, 9-dot character cells,
font-map A/B selection, smooth panning, or an arbitrary VGA DAC text palette.

## No copied screen or font

Include `graphics.h` in an HDMI build. It exposes `graphics_text_config_t` and
`graphics_configure_text()`. The driver copies only this small descriptor; both
data pointers remain owned by the emulator. The expected cell representation is
the usual interleaved pair `{ character, attribute }`.

The following example binds an existing color-text aperture and an existing VGA
font map. The sizes are the actual allocations supplied by the caller; the
example does not allocate either one.

```c
static bool bind_dos_80x25(uint8_t *existing_b800, size_t b800_bytes,
                          const uint8_t *existing_font, size_t font_bytes,
                          size_t display_start_bytes, size_t cursor_bytes) {
    graphics_set_mode(TEXTMODE_80x25_COLOR);
    const graphics_text_config_t config = {
        .vram = existing_b800,
        .vram_size = b800_bytes,
        .font = existing_font,
        .font_size = font_bytes,
        .start_offset = display_start_bytes,
        .cursor_offset = cursor_bytes,
        .row_stride = 80 * 2,
        .font_stride = 32,     // 32-byte glyph slots in this VGA font map
        .font_height = 16,     // 16 scanlines displayed from each slot
        .cursor_start = 14,
        .cursor_end = 15,
        .cursor_enabled = true,
        .msb_first = true,     // VGA bit 7 is the leftmost pixel
        .blink_enabled = false,
        .wrap_vram = true,
    };
    return graphics_configure_text(&config);
}
```

Call this before `graphics_init()` for a valid first frame, or after
initialization when changing the emulator's text mode. Check the boolean return:
an invalid descriptor is rejected without replacing the old one. Call the same
function again when the start address, row pitch, font mapping, or geometry
changes. Cursor and blink also have individual setters.

Use these conventions when adapting the example:

- **All sizes and offsets are bytes.** Convert a CRTC word address with `* 2`.
  `cursor_offset` is relative to the bound VRAM allocation, not relative to the
  displayed page. Display start, VRAM size, and row pitch must be even.
- **VRAM size describes accessible memory.** A contiguous 80 x 25 page needs
  4,000 bytes; 80 x 30 needs 4,800; 40 x 25 needs 2,000. A larger existing aperture
  can expose multiple pages. Set `wrap_vram` only if the emulator wants addresses
  to wrap at that supplied size.
- **Row pitch is independent of font pitch.** `row_stride = 0` means
  `columns * 2`; a larger even pitch supports an existing padded text buffer.
  Rows that do not fit a non-wrapping allocation are blanked, never read beyond
  its end.
- **The font has 256 glyphs and one byte per glyph scanline.** Packed 8 x 16 fonts
  use `font_stride = 16`; VGA 32-byte slots use 32. The declared font allocation
  must cover at least `255 * font_stride + font_height` bytes. The bundled
  `font_8x16` uses stride 16 and `msb_first = false`.
- **Font height controls placement.** Heights up to 8 are doubled vertically;
  heights 9 through 32 are used directly. The result is centered in 480 lines.
  If the selected number of rows cannot fit, the renderer outputs a blank field.
  Thus 8 x 8 and 8 x 16 fonts both give 400 displayed lines in an 80 x 25 mode;
  a 14-line font gives 350 centered lines.
- **Use the existing SRAM font and VRAM for deterministic timing.** The driver
  does not copy a flash-resident font into SRAM. Passing a flash font is a
  caller-controlled XIP dependency in the scanline path.

Direct stores into VRAM or the font become visible on subsequent scanlines.
There is no upload step and no dirty-screen copy. Since scanout and the emulator
can access the same bytes concurrently, a multi-byte/full-screen edit is not
atomic. Update during blanking if the application needs to avoid visible tearing.
The existing `graphics_wait_vblank()` API waits for the frame counter to wrap
from line 524 to line 0; it does not return at the start of the blank interval
on line 480. Do not treat its return as a full blanking window for a bulk copy.

### Pointer lifetime and cores

The descriptor and geometry are protected by a short hardware spinlock. A
separate HDMI spinlock protects buffer preparation, palette publication, and
transport changes. Mode changes from the other core do not install a second
DMA interrupt handler there. Configure the clock before starting a second core,
or let `graphics_init()` initialize it before that core is launched, as in the
existing application.

Both allocations must remain alive while bound. An IRQ may have captured the
old descriptor immediately before a rebind. **Do not free the old VRAM or font
as soon as a setter returns.** Retain it for one full frame after the rebind;
two consecutive `graphics_wait_vblank()` calls provide a conservative grace
period. Alternatively stop the application's scanout before releasing memory.
Permanent emulator-owned arrays need no extra allocation or retirement logic.

`graphics_set_textbuffer()` remains available as a convenience: select the
geometry first, then bind the pointer and call
`graphics_set_text_font(font, height, stride, msb_first)`. This legacy setter
assumes exactly one visible page; use the explicit descriptor for page offsets,
larger apertures, or custom row pitch. No default font is linked by the HDMI
driver when the application supplies a different font.

## Attributes, cursor, and palettes

Attribute bits 0..3 select one of 16 foreground colors. With
`blink_enabled = false`, bits 4..7 select all 16 background colors. With blink
enabled, bits 4..6 select one of eight backgrounds and bit 7 blinks the glyph.
This choice should follow the emulator's attribute-controller state.

`graphics_set_text_cursor(byte_offset, first_scanline, last_scanline, enabled)`
selects a cursor relative to the VRAM allocation. It respects page/wrap addressing
and the configured scanline range. Cursor and attribute blink each use a
16-frame on/off phase, approximately 0.267 seconds per half-period at 60 Hz.
`graphics_set_flashmode()` keeps its legacy no-op behavior; it does not select
VGA attribute blinking.

Text uses the standard 16 CGA/DOS color indices, including brown at index 6.
The single-pixel TMDS encoding alternates negative/positive disparity symbols
independently of each pixel's color. This follows the full-resolution RGB222
construction described by [PicoDVI](https://github.com/Wren6991/PicoDVI).
It makes adjacent text pixels independent without a large attribute/pixel-pair
lookup table. Component levels are close approximations of the four CGA levels:

| Requested component | Even pixel | Odd pixel |
|---:|---:|---:|
| 0 | 5 | 4 |
| 85 | 80 | 81 |
| 170 | 175 | 174 |
| 255 | 250 | 251 |

The maximum component difference is 5 out of 255. This is not a stateful TMDS
encoder for arbitrary VGA DAC colors. `graphics_set_palette()` retains its
graphics meaning: while text is visible it updates the saved graphics palette,
which is restored when returning to graphics. It does not replace the fixed
text palette. The original graphics remapping of indices 252..255 to 48..51 is
preserved.

## Memory layout and mode transitions

The original aligned **4,896-byte** HDMI working allocation is kept exactly the
same size. A union uses it differently for the two mutually exclusive modes:

| Contents | Graphics layout | Text layout |
|---|---:|---:|
| TMDS palette address space | 4,096 B: 256 x 4 words | 512 B: 64 x 2 words |
| Attribute-to-command lookup | within graphics entries | 1,024 B |
| Glyph-bit/phase patterns, both bit orders | within graphics entries | 1,024 B |
| Horizontally doubled nibble patterns | within graphics entries | 64 B |
| Two scanlines, including blanking | 800 B: 2 x 400 indices | 800 B: 2 x 100 commands |
| Attribute-table state and reserved gap | within graphics entries | 224 B |
| Saved original graphics RGB values | within graphics entries | 768 B: 256 x 3 |
| Layout occupied | **4,896 B** | **4,416 B** |
| Actual union allocation | **4,896 B** | **4,896 B** |

There is no 640 x 480 framebuffer, driver-owned text page, copied font, or heap
allocation for text. The small lookup tables describe bit patterns and colors;
they contain no cached characters or font bytes. They reuse existing workspace
space instead of increasing the allocation. Compile-time assertions enforce the
workspace size and overlap assumptions. The 4,096-byte alignment is inherited
from the palette-address converter and can introduce linker padding; account
for it in the actual ELF rather than summing C arrays alone.

There is still a small cost outside this array: descriptor/state, spinlock
pointers, and additional scanline code placed in SRAM. **Unchanged workspace
does not mean unchanged total RAM.** Exact measured builds, including a controlled
comparison with the same application buffers, are in
[HDMI_TEXT_VALIDATION.md](HDMI_TEXT_VALIDATION.md).

Before changing graphics/text transport, the driver waits for vertical blank,
stops all four DMA channels and both PIO machines, saves/restores the graphics
palette in place, and restarts with blank lines. Descending save and ascending
restore order prevent overlapping writes from destroying palette entries.
The HDMI receiver may briefly resynchronize at a graphics/text class change.
Changing between text geometries keeps the same transport.

In graphics there are 400 byte indices per complete line, each expanding to two
physical pixels. In text there are 100 32-bit commands, each expanding to eight
physical pixels. An 80-column cell uses one command per glyph scanline; a
40-column cell uses two. The CPU reads the current VRAM cell and font byte,
combines two small lookup results, and writes the command. The PIO program
selects foreground/background and the alternating TMDS phase for every pixel.
The serializer clock is unchanged. Two DMA buffer addresses form a ring;
the IRQ prepares the buffer that is free while DMA consumes the other buffer.

Blink is folded into the attribute lookup, with only its upper 128 entries
updated when the blink state changes. A cursor patches one or two commands
after the character loop. A row wrapping at the VRAM boundary is rendered as
two contiguous spans. The inner loop has no cursor, blink, or wrapping branch.
RP2040 uses short Thumb-1 loops. RP2350 Arm uses dedicated M33 Thumb-2 loops:
four cells per loop at 80 columns, fixed nibble extraction at 40 columns, and
paired word stores for text output and blank fills. Caller VRAM/font still use
byte loads, including unaligned bases. Other targets retain the C fallback.

See [HDMI_TEXT_PERFORMANCE.md](HDMI_TEXT_PERFORMANCE.md) for before/after CPU
traces, RAM accounting, the XIP decision, and on-board GPIO profiling.
The later M33-specific comparison against the PIO implementation is in
[HDMI_TEXT_RP2350.md](HDMI_TEXT_RP2350.md); its instruction counts are not a
cycle model or a measured CPU percentage.

### Reusing an existing graphics framebuffer too

Build the emulator with `GRAPHICS_BUILTIN_FRAMEBUFFER=OFF`, then provide its
existing 320 x 240 index buffer with
`graphics_set_buffer(existing_buffer, 320, 240)`. HDMI now honors this option;
previously its built-in 76,800-byte allocation was unconditional. A text-only
application does not need to bind a graphics buffer at all. A graphics mode
without one renders black.

Do not count the removal of an unused 76,800-byte buffer as a universal saving:
it applies when the application already has its own graphics storage or needs
text only. The linked firmware's memory map remains the final authority.

## Palette demo and build

The separate `hdmi-text-demo.c` owns one 4,800-byte text page and uses the
repository's existing mutable `font_8x16`. Every eight seconds it cycles through
80 x 30, 80 x 25, and 40 x 25. Each screen contains:

- all 16 foreground colors, shown on black and gray so black text is visible;
- all 16 backgrounds with black text and an external hexadecimal color label;
- a checker glyph and a blinking cursor;
- a live font change once a second, written directly into glyph `0xFE`.

Black text on black background at index 0 is intentionally invisible; its label
is outside the sample. The checker has one-pixel alternation in 80-column modes
and the expected two-pixel horizontal expansion in 40-column mode.

Use an installed C23-capable Arm GNU toolchain, Pico SDK, CMake, and Ninja:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
export PICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain

cmake --preset rp2040-hdmi-text
cmake --build cmake-build-rp2040-hdmi-text --target ntsc-tv

# Raspberry Pi Pico 2 / RP2350 Arm:
cmake --preset rp2350-hdmi-text
cmake --build cmake-build-rp2350-hdmi-text --target ntsc-tv
```

The UF2 files are `bin/rp2040/MinSizeRel/ntsc-tv-hdmi-text.uf2` and
`bin/rp2350-arm-s/MinSizeRel/ntsc-tv-hdmi-text.uf2`. Select the file for the actual
chip. The default pin assignment is clock GPIO12/13, data GPIO14..19. NTSC and VGA
must be off in these builds. If picotool is already installed, CMake accepts its
package directory through `-Dpicotool_DIR=/path/to/lib/cmake/picotool`.

The original `rp2040-hdmi` and `rp2350-hdmi` presets retain the original graphics
demo. The text demo shows each text mode for eight seconds, followed by a
60-second run of the original ball, torus, and helix scenes, then repeats.
It shares the renderer in `ntsc-tv.c`, supplies two 76,800-byte graphics buffers,
and disables the driver's built-in framebuffer. The SRAM
divider setting is supplied by the HDMI interface target; carry that target
setting over when integrating individual driver files into another build.
The HDMI target also runs `pico_generate_pio_header()` for `hdmi_text.pio`;
carry that generation step and its include directory into an external build.

## Reproducible checks and hardware limits

```sh
bash tests/run_host_tests.sh
bash tests/render_demo.sh /tmp/hdmi-text-preview
```

Host tests exercise the actual text implementation with AddressSanitizer and
UndefinedBehaviorSanitizer, and actual codec/workspace helpers for all four
pin-order/polarity combinations. The preview command compiles the real demo
and uses production text helpers plus decoded production TMDS symbols to write
three 640 x 480 PPM images. These are **software renders**, not HDMI captures.

ARM builds, instruction traces and host tests do not measure physical scanout
deadlines. The command transport requires 2,501 DMA transfers per complete line,
including one line-control transfer: about 78.78 million transfers/second,
compared with 100.83 million for the previous byte-per-pixel text implementation
and 75.63 million for doubled graphics. A complete line lasts about 31.75
microseconds (12,000 system clocks at 378 MHz). Hardware validation must check
serializer underrun and line preparation time under the emulator's actual CPU,
SRAM, flash, and DMA load. `HDMI_PROFILE_GPIO` optionally exposes the IRQ duration
without adding counters, buffers, or logging inside the interrupt.

The user confirmed working RP2350 M2P2 HDMI output on 2026-10-26 after enabling
the embedded flash setup with divider 4 and read delay 4. This applies before
the 378 MHz clock change; the default HDMI base pin is 12. That board result
belongs to the previous byte-index transport. The new command transport has
been built and model-checked, but has not yet been tested on a physical board.
Still check all
three geometries, the one-pixel checker in 80-column modes,
the 16 backgrounds, live font edits, cursor/blink, and graphics/text/graphics
transitions with a changed graphics palette. The system clock and voltage are
unchanged; this work does not establish a new overclock stability guarantee.
