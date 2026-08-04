#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIO_VGA (pio0)

#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN 6
#endif

#ifndef TEXTMODE_COLS
#define TEXTMODE_COLS 80
#endif

#ifndef TEXTMODE_ROWS
#define TEXTMODE_ROWS 30
#endif

#ifndef RGB888
#define RGB888(r, g, b) ((r << 16) | (g << 8) | b)
#endif

void vga_init(const uint8_t *framebuffer);
void vga_graphics_init(void);
uint32_t vga_start_mask(void);
void vga_enable(void);
void vga_set_framebuffer(const uint8_t *framebuffer);
void vga_request_framebuffer(const uint8_t *framebuffer);
void vga_wait_framebuffer(void);
void vga_set_palette(uint8_t index, uint32_t color888);

#ifdef __cplusplus
}
#endif
