#include "graphics.h"
#include <string.h>

#if defined(HDMI) || defined(NTSC_TV)
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <hardware/vreg.h>
#include <pico/time.h>
#endif

#if defined(HDMI) && PICO_RP2040
#include <hardware/regs/vreg_and_chip_reset.h>
#include <hardware/structs/vreg_and_chip_reset.h>
#endif

enum {
    GRAPHICS_NTSC_SYSTEM_CLOCK_KHZ = 315000,
    GRAPHICS_HDMI_SYSTEM_CLOCK_KHZ = 378000
};

void graphics_init(void) {
#if defined(HDMI)
#if PICO_RP2040
    hw_set_bits(&vreg_and_chip_reset_hw->vreg,
                VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(33);
#else
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
#endif
    set_sys_clock_khz(GRAPHICS_HDMI_SYSTEM_CLOCK_KHZ, true);
    hdmi_graphics_init();
#elif defined(NTSC_TV)
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    set_sys_clock_khz(GRAPHICS_NTSC_SYSTEM_CLOCK_KHZ, true);
    ntsc_tv_graphics_init();
#elif defined(TFT)
    tft_graphics_init();
#elif defined(VGA)
    vga_graphics_init();
#else
#error "No graphics backend selected"
#endif
}

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
