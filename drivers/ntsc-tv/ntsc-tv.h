#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NTSC_PIN_OUTPUT
#define NTSC_PIN_OUTPUT 27
#endif

#ifndef GRAPHICS_FRAME_WIDTH
#define GRAPHICS_FRAME_WIDTH 320
#endif

#ifndef GRAPHICS_FRAME_HEIGHT
#define GRAPHICS_FRAME_HEIGHT 240
#endif

#ifndef GRAPHICS_COLOR_COUNT
#define GRAPHICS_COLOR_COUNT 256
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

void ntsc_tv_init(const uint8_t *framebuffer);
uint32_t ntsc_tv_start_mask(void);
void ntsc_tv_set_framebuffer(const uint8_t *framebuffer);
void ntsc_tv_request_framebuffer(const uint8_t *framebuffer);
void ntsc_tv_wait_framebuffer(void);
void ntsc_tv_set_palette(uint8_t index, uint32_t color888);

#ifdef __cplusplus
}
#endif
