#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TFT_RST_PIN
#define TFT_RST_PIN 8
#endif

#ifndef TFT_CS_PIN
#define TFT_CS_PIN 6
#endif

#ifndef TFT_LED_PIN
#define TFT_LED_PIN 9
#endif

#ifndef TFT_CLK_PIN
#define TFT_CLK_PIN 13
#endif

#ifndef TFT_DATA_PIN
#define TFT_DATA_PIN 12
#endif

#ifndef TFT_DC_PIN
#define TFT_DC_PIN 10
#endif

#define TEXTMODE_COLS 40
#define TEXTMODE_ROWS 25

#define rgb888(r, g, b) ((((r) >> 3) << 11) | \
                         (((g) >> 2) << 5) | ((b) >> 3))

void refresh_lcd(void);
void tft_graphics_init(void);

#ifdef __cplusplus
}
#endif
