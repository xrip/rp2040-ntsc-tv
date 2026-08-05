#pragma once

// Internal interface between the public graphics API and the output backends.
// It is not part of graphics.h and application code does not use it.
//
// Graphics modes give 320 palette indices for one row, and this unit composes
// them, so the mode work is written once instead of once for each backend.
//
// Text modes are different. They give 640 samples for one line, one sample for
// each pixel, and every backend writes them straight into its own scanline
// buffer. That keeps the full 640-wide text picture, which a 320-index line
// cannot hold. The helpers here give a backend the pieces of a text line; the
// pixel loop stays in the backend, because VGA and NTSC have different sample
// sizes and very different time budgets for one line.

#include <stdint.h>

#include "graphics.h"

#ifdef __cplusplus
extern "C" {
#endif

// The fixed 16 CGA text colors as RGB888. Text does not go through the
// application palette, so an application keeps all 256 of its own colors, and
// each backend can turn these into a compile-time lookup table that lives in
// flash instead of RAM.
#define GRAPHICS_CGA_RGB(i) ( \
    (i) ==  0 ? 0x000000u : (i) ==  1 ? 0x0000aau : \
    (i) ==  2 ? 0x00aa00u : (i) ==  3 ? 0x00aaaau : \
    (i) ==  4 ? 0xaa0000u : (i) ==  5 ? 0xaa00aau : \
    (i) ==  6 ? 0xaa5500u : (i) ==  7 ? 0xaaaaaau : \
    (i) ==  8 ? 0x555555u : (i) ==  9 ? 0x5555ffu : \
    (i) == 10 ? 0x55ff55u : (i) == 11 ? 0x55ffffu : \
    (i) == 12 ? 0xff5555u : (i) == 13 ? 0xff55ffu : \
    (i) == 14 ? 0xffff55u : 0xffffffu)

#define GRAPHICS_CGA_RED(i) (GRAPHICS_CGA_RGB(i) >> 16 & 0xffu)
#define GRAPHICS_CGA_GREEN(i) (GRAPHICS_CGA_RGB(i) >> 8 & 0xffu)
#define GRAPHICS_CGA_BLUE(i) (GRAPHICS_CGA_RGB(i) & 0xffu)

// Repeats a macro for all 256 character attributes, low nibble first, so the
// result is indexed by the attribute byte itself.
#define GRAPHICS_TEXT_ATTRIBUTES(X) \
    GRAPHICS_TEXT_NIBBLE(X, 0)  GRAPHICS_TEXT_NIBBLE(X, 1) \
    GRAPHICS_TEXT_NIBBLE(X, 2)  GRAPHICS_TEXT_NIBBLE(X, 3) \
    GRAPHICS_TEXT_NIBBLE(X, 4)  GRAPHICS_TEXT_NIBBLE(X, 5) \
    GRAPHICS_TEXT_NIBBLE(X, 6)  GRAPHICS_TEXT_NIBBLE(X, 7) \
    GRAPHICS_TEXT_NIBBLE(X, 8)  GRAPHICS_TEXT_NIBBLE(X, 9) \
    GRAPHICS_TEXT_NIBBLE(X, 10) GRAPHICS_TEXT_NIBBLE(X, 11) \
    GRAPHICS_TEXT_NIBBLE(X, 12) GRAPHICS_TEXT_NIBBLE(X, 13) \
    GRAPHICS_TEXT_NIBBLE(X, 14) GRAPHICS_TEXT_NIBBLE(X, 15)

// Repeats a macro for the 16 CGA colors. A text renderer uses it to build its
// own color table at compile time, in whatever form its hardware wants.
#define GRAPHICS_TEXT_COLORS(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7) \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15)

#define GRAPHICS_TEXT_NIBBLE(X, background) \
    X(0, background)  X(1, background)  X(2, background)  X(3, background) \
    X(4, background)  X(5, background)  X(6, background)  X(7, background) \
    X(8, background)  X(9, background)  X(10, background) X(11, background) \
    X(12, background) X(13, background) X(14, background) X(15, background)

// Gives one output row of GRAPHICS_FRAME_WIDTH palette indices.
//
// `framebuffer` is the backend's own active 320 x 240 buffer. It is used only
// by the direct modes, so the present/pending handshake keeps working.
// `staging` is a GRAPHICS_FRAME_WIDTH-byte scratch line owned by the calling
// backend; each backend needs its own, because the backends run from separate
// interrupts.
//
// The returned pointer is good until the next call from the same backend.
const uint8_t *graphics_source_line(const uint8_t *framebuffer,
                                    unsigned row,
                                    uint8_t *staging);

// True while a text mode is selected. A backend then leaves the composer above
// alone and builds its line from graphics_text_row() instead.
bool graphics_source_is_text(void);

// Columns in the selected text mode: 80, or 40 for TEXTMODE_53x30. A 40-column
// line sends every pixel twice, so both fill the same 640 samples.
unsigned graphics_text_columns(void);

// Start of the character/attribute pairs for the text row holding output line
// `line`, or NULL when there is no text buffer. `font_height` is 16 for a
// 480-line output and 8 for a 240-line one; `*glyph_line` gets the row inside
// the character cell.
const uint8_t *graphics_text_row(unsigned line,
                                 unsigned font_height,
                                 unsigned *glyph_line);

// State, set from the backend graphics_set_* functions.
void graphics_source_set_mode(enum graphics_mode_t mode);
void graphics_source_set_buffer(uint8_t *buffer, uint16_t width, uint16_t height);
void graphics_source_set_offset(int x, int y);

#ifdef __cplusplus
}
#endif
