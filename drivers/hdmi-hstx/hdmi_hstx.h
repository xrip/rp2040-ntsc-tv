#pragma once

#include <stdint.h>

#ifndef TEXTMODE_COLS
#define TEXTMODE_COLS 80
#endif
#ifndef TEXTMODE_ROWS
#define TEXTMODE_ROWS 30
#endif
#ifndef RGB888
#define RGB888(r, g, b) (((r) << 16) | ((g) << 8) | (b))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Called by graphics_init(), on the core which will service scanout IRQs.
// The system and HSTX clocks must already be configured to 315 and 126 MHz.
void hdmi_hstx_graphics_init(void);

#ifdef __cplusplus
}
#endif
