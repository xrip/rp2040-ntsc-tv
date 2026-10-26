#pragma once

#include <stdint.h>

enum { DEMO_FRAME_TIME_MS = 33 };

void demo_graphics_init(void);
void demo_graphics_render(uint8_t *framebuffer, uint32_t frame);
