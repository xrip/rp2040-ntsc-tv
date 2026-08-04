#pragma once

#include <hardware/pio.h>

#define st7789_lcd_wrap_target 0
#define st7789_lcd_wrap 1

static const uint16_t st7789_lcd_program_instructions[] = {
        0x6001, // out pins, 1 side 0
        0xb042  // nop side 1
};

static const struct pio_program st7789_lcd_program = {
        .instructions = st7789_lcd_program_instructions,
        .length = 2,
        .origin = -1
};

static inline pio_sm_config st7789_lcd_program_get_default_config(
        const uint offset) {
    pio_sm_config config = pio_get_default_sm_config();
    sm_config_set_wrap(
            &config,
            offset + st7789_lcd_wrap_target,
            offset + st7789_lcd_wrap);
    sm_config_set_sideset(&config, 1, false, false);
    return config;
}

static inline void st7789_lcd_program_init(PIO pio,
                                           const uint sm,
                                           const uint offset,
                                           const uint data_pin,
                                           const uint clk_pin,
                                           const float clk_div) {
    pio_gpio_init(pio, data_pin);
    pio_gpio_init(pio, clk_pin);
    pio_sm_set_consecutive_pindirs(pio, sm, data_pin, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, clk_pin, 1, true);
    pio_sm_config config = st7789_lcd_program_get_default_config(offset);
    sm_config_set_sideset_pins(&config, clk_pin);
    sm_config_set_out_pins(&config, data_pin, 1);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, clk_div);
    sm_config_set_out_shift(&config, false, true, 8);
    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_enabled(pio, sm, true);
}

static inline void st7789_lcd_put(PIO pio,
                                  const uint sm,
                                  const uint8_t value) {
    while (pio_sm_is_tx_fifo_full(pio, sm)) {
        tight_loop_contents();
    }
    *(volatile uint8_t *)&pio->txf[sm] = value;
}

static inline void st7789_set_pixel_mode(PIO pio,
                                         const uint sm,
                                         const bool pixel_mode) {
    uint32_t shiftctrl = pio->sm[sm].shiftctrl;
    shiftctrl &= ~PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS;
    shiftctrl |= (pixel_mode ? 16u : 8u)
                 << PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB;
    pio->sm[sm].shiftctrl = shiftctrl;
}

static inline void st7789_lcd_put_pixel(PIO pio,
                                        const uint sm,
                                        const uint16_t value) {
    while (pio_sm_is_tx_fifo_full(pio, sm)) {
        tight_loop_contents();
    }
    *(volatile uint16_t *)&pio->txf[sm] = value;
}

static inline void st7789_lcd_wait_idle(PIO pio, const uint sm) {
    const uint32_t sm_stall_mask = 1u << (sm + PIO_FDEBUG_TXSTALL_LSB);
    pio->fdebug = sm_stall_mask;
    while ((pio->fdebug & sm_stall_mask) == 0u) {
        tight_loop_contents();
    }
}
