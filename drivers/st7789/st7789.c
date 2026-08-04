#pragma GCC optimize("Ofast")
/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#include "graphics.h"

#include <pico/multicore.h>

#include "st7789.pio.h"

#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 320
#endif

#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 240
#endif

// 126MHz SPI
#define SERIAL_CLK_DIV 2.5f
#define MADCTL_BGR_PIXEL_ORDER (1<<3)
#define MADCTL_ROW_COLUMN_EXCHANGE (1<<5)
#define MADCTL_COLUMN_ADDRESS_ORDER_SWAP (1<<6)


#define CHECK_BIT(var, pos) (((var)>>(pos)) & 1)

static uint sm_video_output = 0;
static PIO pio = pio0;
uint16_t palette[256];

uint8_t *text_buffer = NULL;
static uint8_t tft_primary_framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT]
        __attribute__((aligned(4)));
static const uint8_t *graphics_framebuffer;

static const uint8_t init_seq[] = {
    1, 20, 0x01, // Software reset
    1, 10, 0x11, // Exit sleep mode
    2, 2, 0x3a, 0x55, // Set colour mode to 16 bit
#ifdef ILI9341
    // ILI9341
    2, 0, 0x36, MADCTL_ROW_COLUMN_EXCHANGE | MADCTL_BGR_PIXEL_ORDER, // Set MADCTL
#else
    // ST7789
    2, 0, 0x36, MADCTL_COLUMN_ADDRESS_ORDER_SWAP | MADCTL_ROW_COLUMN_EXCHANGE, // Set MADCTL
#endif
    5, 0, 0x2a, 0x00, 0x00, SCREEN_WIDTH >> 8, SCREEN_WIDTH & 0xff, // CASET: column addresses
    5, 0, 0x2b, 0x00, 0x00, SCREEN_HEIGHT >> 8, SCREEN_HEIGHT & 0xff, // RASET: row addresses
    1, 2, 0x20, // Inversion OFF
    1, 2, 0x13, // Normal display on, then 10 ms delay
    1, 2, 0x29, // Main screen turn on, then wait 500 ms
    0 // Terminate list
};
// Format: cmd length (including cmd byte), post delay in units of 5 ms, then cmd payload
// Note the delays have been shortened a little

static inline void lcd_set_dc_cs(const bool dc, const bool cs) {
    sleep_us(5);
    gpio_put_masked((1u << TFT_DC_PIN) | (1u << TFT_CS_PIN), !!dc << TFT_DC_PIN | !!cs << TFT_CS_PIN);
    sleep_us(5);
}

static inline void lcd_write_cmd(const uint8_t *cmd, size_t count) {
    st7789_lcd_wait_idle(pio, sm_video_output);
    lcd_set_dc_cs(0, 0);
    st7789_lcd_put(pio, sm_video_output, *cmd++);
    if (count >= 2) {
        st7789_lcd_wait_idle(pio, sm_video_output);
        lcd_set_dc_cs(1, 0);
        for (size_t i = 0; i < count - 1; ++i)
            st7789_lcd_put(pio, sm_video_output, *cmd++);
    }
    st7789_lcd_wait_idle(pio, sm_video_output);
    lcd_set_dc_cs(1, 1);
}

static inline void lcd_set_window(const uint16_t x,
                                  const uint16_t y,
                                  const uint16_t width,
                                  const uint16_t height) {
    static uint8_t screen_width_cmd[] = {0x2a, 0x00, 0x00, SCREEN_WIDTH >> 8, SCREEN_WIDTH & 0xff};
    static uint8_t screen_height_command[] = {0x2b, 0x00, 0x00, SCREEN_HEIGHT >> 8, SCREEN_HEIGHT & 0xff};
    screen_width_cmd[2] = x;
    screen_width_cmd[4] = x + width - 1;

    screen_height_command[2] = y;
    screen_height_command[4] = y + height - 1;
    lcd_write_cmd(screen_width_cmd, 5);
    lcd_write_cmd(screen_height_command, 5);
}

static inline void lcd_init(const uint8_t *init_seq) {
    while (*init_seq) {
        lcd_write_cmd(init_seq + 2, *init_seq);
        sleep_ms(init_seq[1] * 5);
        init_seq += *init_seq + 2;
    }
}

static inline void start_pixels() {
    const uint8_t cmd = 0x2c; // RAMWR
    st7789_lcd_wait_idle(pio, sm_video_output);
    st7789_set_pixel_mode(pio, sm_video_output, false);
    lcd_write_cmd(&cmd, 1);
    st7789_set_pixel_mode(pio, sm_video_output, true);
    lcd_set_dc_cs(1, 0);
}

static inline void stop_pixels(void) {
    st7789_lcd_wait_idle(pio, sm_video_output);
    lcd_set_dc_cs(1, 1);
    st7789_set_pixel_mode(pio, sm_video_output, false);
}

void tft_graphics_init(void) {
    graphics_framebuffer = tft_primary_framebuffer;

    gpio_init(TFT_CS_PIN);
    gpio_init(TFT_DC_PIN);
    gpio_init(TFT_RST_PIN);
    gpio_init(TFT_LED_PIN);

    const uint offset = pio_add_program(pio, &st7789_lcd_program);
    sm_video_output = pio_claim_unused_sm(pio, true);
    st7789_lcd_program_init(pio, sm_video_output, offset, TFT_DATA_PIN, TFT_CLK_PIN, SERIAL_CLK_DIV);


    gpio_set_dir(TFT_CS_PIN, GPIO_OUT);
    gpio_set_dir(TFT_DC_PIN, GPIO_OUT);
    gpio_set_dir(TFT_RST_PIN, GPIO_OUT);
    gpio_set_dir(TFT_LED_PIN, GPIO_OUT);

    gpio_put(TFT_CS_PIN, 1);
    gpio_put(TFT_RST_PIN, 1);
    lcd_init(init_seq);
    gpio_put(TFT_LED_PIN, 1);

    for (int i = 0; i < 256; ++i) {
        graphics_set_palette((uint8_t)i, 0x0000);
    }

    lcd_set_window(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    start_pixels();

    uint32_t pixel_count = SCREEN_WIDTH * SCREEN_HEIGHT;
    while (pixel_count--) {
        st7789_lcd_put_pixel(pio, sm_video_output, 0x0);
    }
    stop_pixels();
}

void graphics_set_mode(const enum graphics_mode_t mode) {
    (void)mode;
}

void graphics_set_buffer(uint8_t *buffer,
                         const uint16_t width,
                         const uint16_t height) {
    if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) {
        graphics_framebuffer = buffer;
    }
}

void graphics_set_textbuffer(uint8_t *buffer) {
    text_buffer = buffer;
}

void graphics_set_offset(const int x, const int y) {
    (void)x;
    (void)y;
}

void graphics_set_bgcolor(const uint32_t color888) {
    (void)color888;
}

void graphics_set_flashmode(const bool flash_line, const bool flash_frame) {
    (void)flash_line;
    (void)flash_frame;
}

void __time_critical_func(refresh_lcd)(void) {
    if (graphics_framebuffer == NULL) {
        return;
    }

    lcd_set_window(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    start_pixels();

    const uint8_t *input = graphics_framebuffer;
    uint32_t pixel_count = SCREEN_WIDTH * SCREEN_HEIGHT;
    while (pixel_count--) {
        st7789_lcd_put_pixel(
                pio,
                sm_video_output,
                palette[*input++]);
    }
    st7789_lcd_wait_idle(pio, sm_video_output);
    stop_pixels();
}

void graphics_set_palette(const uint8_t index, const uint32_t color) {
    palette[index] = rgb888(color >> 16, color >> 8 & 0xff, color & 0xff);
}

uint8_t *graphics_get_framebuffer(void) {
    return tft_primary_framebuffer;
}

void graphics_present_framebuffer(const uint8_t *framebuffer) {
    __mem_fence_release();
    graphics_framebuffer = framebuffer;
    refresh_lcd();
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
