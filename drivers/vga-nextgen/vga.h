#pragma once
#include "stdint.h"

#define PIO_VGA (pio0)
#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN (6)
#endif
#define TEXTMODE_COLS 80
#define TEXTMODE_ROWS 30

#define RGB888(r, g, b) ((r<<16) | (g << 8 ) | b )

#if defined(GRAPHICS_DUAL_OUTPUT) && GRAPHICS_DUAL_OUTPUT

void vga_dual_init(const uint8_t *framebuffer);
uint32_t vga_dual_start_mask(void);
void vga_dual_enable(void);
void vga_dual_set_framebuffer(const uint8_t *framebuffer);
void vga_dual_request_framebuffer(const uint8_t *framebuffer);
void vga_dual_wait_framebuffer(void);
void vga_dual_set_palette(uint8_t index, uint32_t color888);

#endif
