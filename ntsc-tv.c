#include <pico/time.h>
#include <math.h>
#include <pico/multicore.h>

#include <hardware/gpio.h>
#include <hardware/clocks.h>
#include <hardware/structs/vreg_and_chip_reset.h>
#include "ntsc-tv-out.h"

// ------------------------------------------------------------
// Wavy checkerboard with 256-color gradient using LUT
// ------------------------------------------------------------
static int8_t wave_lut[256]; // amplitude-scaled sine (cos via +90° phase shift)
static uint8_t step_x;       // phase step per pixel along x
static uint8_t step_y;       // phase step per pixel along y
static uint8_t tstep_1;      // phase step per frame for first wave
static uint8_t tstep_2;      // phase step per frame for second wave (0.8x speed)

// Build LUT and fixed-point steps (called once at startup)
static void init_wave_lut(float amp, float fx, float fy, float t_speed) {
    const float two_pi = 6.283185307179586f;
    // Fill amplitude-scaled sine LUT
    for (int i = 0; i < 256; ++i) {
        float s = sinf((two_pi * i) / 256.0f);
        int v = (int)lrintf(amp * s);
        if (v < -128) v = -128;
        if (v > 127) v = 127;
        wave_lut[i] = (int8_t)v;
    }
    // Convert radians-per-pixel to phase steps in [0..255]
    const float phase_scale = 256.0f / two_pi;
    step_x  = (uint8_t)lrintf(fx       * phase_scale);    // ~4 for fx=0.09
    step_y  = (uint8_t)lrintf(fy       * phase_scale);    // ~5 for fy=0.11
    tstep_1 = (uint8_t)lrintf(t_speed  * phase_scale);    // ~5 for 0.12
    tstep_2 = (uint8_t)lrintf((t_speed * 0.8f) * phase_scale); // ~4
}

static inline uint8_t checker_color_at(int x, int y, int frame) {
    // Phase accumulation (mod 256 via uint8_t wrap)
    const uint8_t phase_y = (uint8_t)(y * step_y + frame * tstep_1);
    const uint8_t phase_x = (uint8_t)(x * step_x + frame * tstep_2 + 64); // cos = sin(+90°), 90° = 64 in 256-cycle

    // Wavy warp via LUT
    const int sx = x + wave_lut[phase_y];
    const int sy = y + wave_lut[phase_x];

    // Checker parity from warped coordinates
    const int cx = sx / 16; // tile size 16
    const int cy = sy / 16;
    const int parity = (cx ^ cy) & 1;

    // Full 256-color gradient across diagonal + time
    const uint8_t base = (uint8_t)(sx + sy + (frame << 1));

    // Opposite squares get shifted gradient to keep contrast while covering all 256 indices
    return parity ? (uint8_t)(base ^ 0x80) : base;
}

// Core 1 entry: fill the framebuffer continuously
static void core1_entry() {
    int frame = 0;
    while (1) {
        for (int y = 0; y < NTSC_FRAME_HEIGHT; y++) {
            uint8_t *row = &ntsc_framebuffer[y * NTSC_FRAME_WIDTH];
            for (int x = 0; x < NTSC_FRAME_WIDTH; x++) {
                row[x] = checker_color_at(x, y, frame);
            }
        }
        frame++;
        // Optional pacing
        // tight_loop_contents();
    }
}


// VGA 256-color palette (0xRRGGBB)
static const uint32_t vga_palette[256] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA, // 0-7
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF, // 8-15
        0x000000, 0x141414, 0x202020, 0x2C2C2C, 0x383838, 0x444444, 0x505050, 0x606060, // 16-23
        0x707070, 0x808080, 0x909090, 0xA0A0A0, 0xB4B4B4, 0xC8C8C8, 0xDCDCDC, 0xF0F0F0, // 24-31
        0x0000FF, 0x4100FF, 0x8200FF, 0xBE00FF, 0xFF00FF, 0xFF00BE, 0xFF0082, 0xFF0041, // 32-39
        0xFF0000, 0xFF4100, 0xFF8200, 0xFFBE00, 0xFFFF00, 0xBEFF00, 0x82FF00, 0x41FF00, // 40-47
        0x00FF00, 0x00FF41, 0x00FF82, 0x00FFBE, 0x00FFFF, 0x00BEFF, 0x0082FF, 0x0041FF, // 48-55
        0x8282FF, 0x9E82FF, 0xBE82FF, 0xDB82FF, 0xFF82FF, 0xFF82DB, 0xFF82BE, 0xFF829E, // 56-63
        0xFF8282, 0xFF9E82, 0xFFBE82, 0xFFDB82, 0xFFFF82, 0xDBFF82, 0xBEFF82, 0x9EFF82, // 64-71
        0x82FF82, 0x82FF9E, 0x82FFBE, 0x82FFDB, 0x82FFFF, 0x82DBFF, 0x82BEFF, 0x829EFF, // 72-79
        0xB6B6FF, 0xC6B6FF, 0xDBB6FF, 0xEBB6FF, 0xFFB6FF, 0xFFB6EB, 0xFFB6DB, 0xFFB6C6, // 80-87
        0xFFB6B6, 0xFFC6B6, 0xFFDBB6, 0xFFEBB6, 0xFFFFB6, 0xEBFFB6, 0xDBFFB6, 0xC6FFB6, // 88-95
        0xB6FFB6, 0xB6FFC6, 0xB6FFDB, 0xB6FFEB, 0xB6FFFF, 0xB6EBFF, 0xB6DBFF, 0xB6C6FF, // 96-103
        0x000071, 0x1C0071, 0x390071, 0x550071, 0x710071, 0x710055, 0x710039, 0x71001C, // 104-111
        0x710000, 0x711C00, 0x713900, 0x715500, 0x717100, 0x557100, 0x397100, 0x1C7100, // 112-119
        0x007100, 0x00711C, 0x007139, 0x007155, 0x007171, 0x005571, 0x003971, 0x001C71, // 120-127
        0x393971, 0x453971, 0x553971, 0x613971, 0x713971, 0x713961, 0x713955, 0x713945, // 128-135
        0x713939, 0x714539, 0x715539, 0x716139, 0x717139, 0x617139, 0x557139, 0x457139, // 136-143
        0x397139, 0x397145, 0x397155, 0x397161, 0x397171, 0x396171, 0x395571, 0x394571, // 144-151
        0x515171, 0x595171, 0x615171, 0x695171, 0x715171, 0x715169, 0x715161, 0x715159, // 152-159
        0x715151, 0x715951, 0x716151, 0x716951, 0x717151, 0x697151, 0x617151, 0x597151, // 160-167
        0x517151, 0x517159, 0x517161, 0x517169, 0x517171, 0x516971, 0x516171, 0x515971, // 168-175
        0x000041, 0x100041, 0x200041, 0x310041, 0x410041, 0x410031, 0x410020, 0x410010, // 176-183
        0x410000, 0x411000, 0x412000, 0x413100, 0x414100, 0x314100, 0x204100, 0x104100, // 184-191
        0x004100, 0x004110, 0x004120, 0x004131, 0x004141, 0x003141, 0x002041, 0x001041, // 192-199
        0x202041, 0x282041, 0x312041, 0x392041, 0x412041, 0x412039, 0x412031, 0x412028, // 200-207
        0x412020, 0x412820, 0x413120, 0x413920, 0x414120, 0x394120, 0x314120, 0x284120, // 208-215
        0x204120, 0x204128, 0x204131, 0x204139, 0x204141, 0x203941, 0x203141, 0x202841, // 216-223
        0x2D2D41, 0x312D41, 0x392D41, 0x3D2D41, 0x412D41, 0x412D3D, 0x412D39, 0x412D31, // 224-231
        0x412D2D, 0x41312D, 0x41392D, 0x413D2D, 0x41412D, 0x3D412D, 0x39412D, 0x31412D, // 232-239
        0x2D412D, 0x2D4131, 0x2D4139, 0x2D413D, 0x2D4141, 0x2D3D41, 0x2D3941, 0x2D3141, // 240-247
        0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000  // 248-255
};

void ntsc_init_palette(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t rgb = vga_palette[i];
        uint8_t r = (rgb >> 16) & 0xFF;
        uint8_t g = (rgb >> 8) & 0xFF;
        uint8_t b = (rgb >> 0) & 0xFF;

        // ntsc_set_color expects parameters in order: (blue, red, green)
        ntsc_set_color((uint8_t)i, b, r, g);
    }
}

void main() {
    ntsc_init_palette();
    ntsc_init();

    // Initialize wave LUT once (amp, fx, fy, t_speed)
    init_wave_lut(8.0f, 0.09f, 0.11f, 0.12f);

    // Initialize onboard LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // LED startup sequence
    for (int i = 0; i < 6; i++) {
        sleep_ms(23);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(23);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

    // Launch rendering on core 1
    multicore_launch_core1(core1_entry);

    // Core 0 heartbeat
    while (1) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(250);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(750);
    }
}
