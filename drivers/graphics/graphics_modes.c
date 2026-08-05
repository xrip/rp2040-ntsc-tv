#include "graphics_modes.h"

#include <string.h>

#include <pico.h>

enum {
    GRAPHICS_GG_WIDTH = 160,
    GRAPHICS_GG_SOURCE_X = 48
};

static enum graphics_mode_t graphics_mode = VGA_320x240x256;
static uint8_t *graphics_source_buffer = NULL;
static uint16_t graphics_source_width = GRAPHICS_FRAME_WIDTH;
static uint16_t graphics_source_height = GRAPHICS_FRAME_HEIGHT;
static int graphics_shift_x = 0;
static int graphics_shift_y = 0;

// Border and off-picture rows use palette index 0, as the older driver did.
// Kept out of flash so an interrupt never waits on XIP.
static uint8_t graphics_blank_line[GRAPHICS_FRAME_WIDTH] __attribute__((aligned(4)));

// Every backend asks this for each of its lines, so like the line builders it
// belongs in RAM and not in flash.
static bool __time_critical_func(graphics_mode_is_text)(const enum graphics_mode_t mode) {
    return mode == TEXTMODE_DEFAULT ||
           mode == TEXTMODE_53x30 ||
           mode == TEXTMODE_160x100;
}

void graphics_source_set_mode(const enum graphics_mode_t mode) {
    graphics_mode = mode;

    if (graphics_source_buffer != NULL) {
        memset(graphics_source_buffer, 0,
               (size_t)graphics_source_width * graphics_source_height);
    }
}

void graphics_source_set_buffer(uint8_t *buffer,
                                const uint16_t width,
                                const uint16_t height) {
    graphics_source_buffer = buffer;
    graphics_source_width = width;
    graphics_source_height = height;
}

void graphics_source_set_offset(const int x, const int y) {
    graphics_shift_x = x;
    graphics_shift_y = y;
}

bool __time_critical_func(graphics_source_is_text)(void) {
    return graphics_mode_is_text(graphics_mode);
}

unsigned __time_critical_func(graphics_text_columns)(void) {
    return graphics_mode == TEXTMODE_53x30 ? TEXTMODE_COLS / 2u : TEXTMODE_COLS;
}

const uint8_t *__time_critical_func(graphics_text_row)(
        const unsigned line, const unsigned font_height, unsigned *glyph_line) {
    *glyph_line = line % font_height;

    if (text_buffer == NULL) {
        return NULL;
    }
    return text_buffer +
            (size_t)(line / font_height) * graphics_text_columns() * 2u;
}

// One row of the application buffer, centred on the 320-pixel line.
// `source_x` and `pixels` pick the visible window inside the source row.
static void __time_critical_func(graphics_compose_indexed)(
        const int source_row, uint8_t *staging,
        const unsigned source_x, const unsigned pixels, const bool doubled) {
    const unsigned output_pixels = doubled ? pixels * 2u : pixels;
    unsigned left = (GRAPHICS_FRAME_WIDTH - output_pixels) / 2u;

    if (graphics_shift_x > 0) {
        left += (unsigned)graphics_shift_x;
        if (left > GRAPHICS_FRAME_WIDTH - output_pixels) {
            left = GRAPHICS_FRAME_WIDTH - output_pixels;
        }
    }

    const uint8_t *source = graphics_source_buffer +
            (size_t)source_row * graphics_source_width + source_x;
    uint8_t *output = staging;

    for (unsigned i = left; i--;) {
        *output++ = 0;
    }
    for (unsigned i = pixels; i--;) {
        const uint8_t index = *source++ & 0x1fu;
        *output++ = index;
        if (doubled) {
            *output++ = index;
        }
    }
    for (unsigned i = GRAPHICS_FRAME_WIDTH - left - output_pixels; i--;) {
        *output++ = 0;
    }
}

const uint8_t *__time_critical_func(graphics_source_line)(
        const uint8_t *framebuffer, const unsigned row, uint8_t *staging) {
    switch (graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
        case GG_160x144:
        case GG_160x144x4x3: {
            if (graphics_source_buffer == NULL) {
                return graphics_blank_line;
            }

            // The 4:3 Game Gear mode puts three physical lines on one source
            // row. `row` is already half of the physical line, so the source
            // step is two thirds of a row.
            const int scaled = graphics_mode == GG_160x144x4x3
                    ? (int)(row * 2u / 3u)
                    : (int)row;
            const int source_row = scaled - graphics_shift_y;
            if (source_row < 0 || source_row >= (int)graphics_source_height) {
                return graphics_blank_line;
            }

            if (graphics_mode == GRAPHICSMODE_DEFAULT) {
                const unsigned pixels =
                        graphics_source_width < GRAPHICS_FRAME_WIDTH
                        ? graphics_source_width : GRAPHICS_FRAME_WIDTH;
                graphics_compose_indexed(source_row, staging, 0, pixels, false);
            } else {
                graphics_compose_indexed(source_row, staging,
                                         GRAPHICS_GG_SOURCE_X,
                                         GRAPHICS_GG_WIDTH,
                                         graphics_mode == GG_160x144x4x3);
            }
            return staging;
        }

        default:
            if (framebuffer == NULL) {
                return graphics_blank_line;
            }
            return framebuffer + (size_t)row * GRAPHICS_FRAME_WIDTH;
    }
}
