#include "graphics.h"
#include <string.h>

#if defined(GRAPHICS_DUAL_OUTPUT) && GRAPHICS_DUAL_OUTPUT
#include <hardware/clocks.h>
#include <hardware/vreg.h>

#include "ntsc-tv-out.h"

uint8_t *text_buffer = nullptr;

void graphics_init(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    set_sys_clock_khz(315000, true);

    vga_dual_init(ntsc_framebuffer);
    ntsc_init();
    dma_start_channel_mask(vga_dual_start_mask() | ntsc_start_mask());
    vga_dual_enable();
}

void graphics_set_mode(const enum graphics_mode_t mode) {
    (void)mode;
}

void graphics_set_buffer(uint8_t *buffer,
                         const uint16_t width,
                         const uint16_t height) {
    if (width != GRAPHICS_FRAME_WIDTH || height != GRAPHICS_FRAME_HEIGHT) {
        return;
    }
    ntsc_active_framebuffer = buffer;
    vga_dual_set_framebuffer(buffer);
}

void graphics_set_offset(const int x, const int y) {
    (void)x;
    (void)y;
}

void graphics_set_palette(const uint8_t index, const uint32_t color) {
    const uint8_t red = (uint8_t)(color >> 16);
    const uint8_t green = (uint8_t)(color >> 8);
    const uint8_t blue = (uint8_t)color;

    vga_dual_set_palette(index, color);
    ntsc_set_color(index, blue, red, green);
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
    return ntsc_framebuffer;
}

void graphics_present_framebuffer(const uint8_t *framebuffer) {
    vga_dual_request_framebuffer(framebuffer);
    ntsc_present_framebuffer(framebuffer);
    vga_dual_wait_framebuffer();
}
#endif

void draw_text(const char string[2*TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor) {
    uint8_t *t_buf = text_buffer + TEXTMODE_COLS * 2 * y + 2 * x;
    for (int xi = TEXTMODE_COLS * 2; xi--;) {
        if (!*string) break;
        *t_buf++ = *string++;
        *t_buf++ = bgcolor << 4 | color & 0xF;
    }
}

void draw_window(const char title[2*TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    char line[width + 1];
    memset(line, 0, sizeof line);
    width--;
    height--;
    // Рисуем рамки

    memset(line, 0xCD, width); // ═══


    line[0] = 0xC9; // ╔
    line[width] = 0xBB; // ╗
    draw_text(line, x, y, 11, 1);

    line[0] = 0xC8; // ╚
    line[width] = 0xBC; //  ╝
    draw_text(line, x, height + y, 11, 1);

    memset(line, ' ', width);
    line[0] = line[width] = 0xBA;

    for (int i = 1; i < height; i++) {
        draw_text(line, x, y + i, 11, 1);
    }

    snprintf(line, width - 1, " %s ", title);
    draw_text(line, x + (width - strlen(line)) / 2, y, 14, 3);
}

#if defined(GRAPHICS_DUAL_OUTPUT) && GRAPHICS_DUAL_OUTPUT
void clrScr(const uint8_t color) {
    if (text_buffer == nullptr) {
        return;
    }

    uint16_t *output = (uint16_t *)text_buffer;
    int size = TEXTMODE_COLS * TEXTMODE_ROWS;
    while (size--) {
        *output++ = (uint16_t)(color << 4 | ' ');
    }
}
#endif
