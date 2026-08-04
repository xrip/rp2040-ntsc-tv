#include <graphics.h>

#if defined(VGA) && !defined(NTSC_TV)

#include <stddef.h>
#include <string.h>

#include <hardware/dma.h>

#include "vga.h"

static uint8_t graphics_framebuffer[GRAPHICS_FRAME_WIDTH * GRAPHICS_FRAME_HEIGHT]
        __attribute__((aligned(4)));

uint8_t *text_buffer = NULL;

void vga_graphics_init(void) {
    vga_init(graphics_framebuffer);
    dma_start_channel_mask(vga_start_mask());
    vga_enable();
}

void graphics_set_mode(const enum graphics_mode_t mode) {
    (void)mode;
}

void graphics_set_buffer(uint8_t *buffer,
                         const uint16_t width,
                         const uint16_t height) {
    if (width == GRAPHICS_FRAME_WIDTH && height == GRAPHICS_FRAME_HEIGHT) {
        vga_set_framebuffer(buffer);
    }
}

void graphics_set_offset(const int x, const int y) {
    (void)x;
    (void)y;
}

void graphics_set_palette(const uint8_t index, const uint32_t color) {
    vga_set_palette(index, color);
}

void graphics_set_textbuffer(uint8_t *buffer) {
    text_buffer = buffer;
}

void graphics_set_bgcolor(const uint32_t color888) {
    (void)color888;
}

void graphics_set_flashmode(const bool flash_line, const bool flash_frame) {
    (void)flash_line;
    (void)flash_frame;
}

uint8_t *graphics_get_framebuffer(void) {
    return graphics_framebuffer;
}

void graphics_present_framebuffer(const uint8_t *framebuffer) {
    vga_request_framebuffer(framebuffer);
    vga_wait_framebuffer();
}

void clrScr(const uint8_t color) {
    if (text_buffer == NULL) {
        return;
    }

    uint16_t *output = (uint16_t *)text_buffer;
    int size = TEXTMODE_COLS * TEXTMODE_ROWS;
    while (size--) {
        *output++ = (uint16_t)(color << 4 | ' ');
    }
}

#endif
