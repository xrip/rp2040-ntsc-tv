#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "stdbool.h"
#include "stdio.h"
#include "stdint.h"

#define GRAPHICS_FRAME_WIDTH 320
#define GRAPHICS_FRAME_HEIGHT 240
#define GRAPHICS_COLOR_COUNT 256

// Graphics modes give 320 pixels and every output sends each pixel twice.
// Text modes give 640 samples and every output sends each sample once, so the
// full horizontal resolution of the line is kept.
#define GRAPHICS_TEXT_SAMPLES 640

#ifdef TFT
#include "st7789.h"
#endif
#ifdef HDMI
#include "hdmi.h"
#endif
#ifdef VGA
#include "vga.h"
#endif
#ifdef NTSC_TV
#include "ntsc-tv.h"
#endif
#ifdef TV
#include "tv.h"
#endif
#ifdef SOFTTV
#include "tv-software.h"
#endif


#include "font4x6.h"
#include "font6x8.h"
#include "font8x8.h"
#include "font8x16.h"

enum graphics_mode_t {
    TEXTMODE_40x25_BW,
    TEXTMODE_40x25_COLOR,
    TEXTMODE_80x25_BW,
    TEXTMODE_80x25_COLOR,
    CGA_320x200x4 = 4,
    CGA_320x200x4_BW = 5,

    CGA_640x200x2 = 6,
    HERC_640x480x2 = 7,
    HERC_640x480x2_90 = 0x1e,
    TGA_160x200x16 = 8,

    TGA_320x200x16 = 9,
    TGA_640x200x16 = 0xa,

    EGA_320x200x16x4 = 0x0d,
    EGA_640x200x16x4 = 0x0e,
    EGA_640x350x16x4 = 0x10,

    VGA_640x480x2 = 0x11,
    VGA_640x480x16 = 0x12,

    VGA_320x200x256 = 0x13,
    VGA_320x240x256 = 0x14,
    VGA_320x200x256x4 = 0xff,

    COMPOSITE_160x200x16_force = 0x74,
    COMPOSITE_160x200x16 = 0x76,
    // planar VGA

    // Modes kept for the Murmulator emulator family. The scanline composer in
    // drivers/graphics/graphics_modes.c gives these their picture layout.
    TEXTMODE_DEFAULT = 0x100,   // 80 x 30 text, font 4x6 on an 8-pixel row pitch
    TEXTMODE_53x30 = 0x101,     // 40 x 30 text, same font at double width
    TEXTMODE_160x100 = 0x102,   // same layout as TEXTMODE_DEFAULT
    GRAPHICSMODE_DEFAULT = 0x103, // app buffer, 1x, centred on the 320-pixel line
    GG_160x144 = 0x104,         // Game Gear window, 1x, centred
    GG_160x144x4x3 = 0x105,     // Game Gear window, 2x wide, 2:3 line scale
};

// Буффер текстового режима
extern uint8_t *text_buffer;

void graphics_init(void);

void graphics_set_mode(enum graphics_mode_t mode);

void graphics_set_buffer(uint8_t *buffer, uint16_t width, uint16_t height);

void graphics_set_offset(int x, int y);

void graphics_set_palette(uint8_t index, uint32_t color);

void graphics_set_textbuffer(uint8_t *buffer);

void graphics_set_bgcolor(uint32_t color888);

void graphics_set_flashmode(bool flash_line, bool flash_frame);

uint8_t *graphics_get_framebuffer(void);

void graphics_present_framebuffer(const uint8_t *framebuffer);

void draw_text(const char string[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor);
void draw_window(const char title[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint32_t width, uint32_t height);

void clrScr(uint8_t color);

#ifdef __cplusplus
}
#endif
