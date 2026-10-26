/* Software preview of the real demo, not a hardware capture or scanout simulation.
 * tests/render_demo.sh OUTPUT_DIR writes three 640x480 binary PPM files.
 * VRAM, font selection and layout come from hdmi-text-demo.c; text addressing,
 * glyphs and TMDS symbols come from the production graphics/HDMI helpers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <setjmp.h>
#include <pico/time.h>
#include "graphics_modes.h"
#include "hdmi_codec.h"

// Replace only firmware entry points that need peripherals. The real text
// implementation is linked below; no renderer/font/screen copy is maintained.
static jmp_buf demo_cycle_done;
static bool checking_cycle;
static unsigned mode_changes;
static uint8_t *graphics_buffer;
static unsigned graphics_frames;

void graphics_set_mode(enum graphics_mode_t mode) {
    if (checking_cycle) {
        static const enum graphics_mode_t expected_modes[] = {
            TEXTMODE_80x30, TEXTMODE_80x25_COLOR, TEXTMODE_40x25_COLOR,
            VGA_320x240x256, TEXTMODE_80x30
        };
        static const unsigned expected_seconds[] = {0, 8, 16, 24, 84};
        assert(mode_changes < 5);
        assert(mode == expected_modes[mode_changes]);
        assert(get_absolute_time() == (uint64_t)expected_seconds[mode_changes] * 1000000u);
        ++mode_changes;
    }
    graphics_source_set_mode(mode);
}
void graphics_init(void) { graphics_text_init(); }
void graphics_set_buffer(uint8_t *buffer, uint16_t width, uint16_t height) {
    assert(width == 320 && height == 240);
    graphics_buffer = buffer;
}
void graphics_present_framebuffer(const uint8_t *buffer) {
    assert(buffer != graphics_buffer);
    graphics_buffer = (uint8_t *)buffer;
    ++graphics_frames;
}
void graphics_set_palette(uint8_t index, uint32_t rgb) {
    (void)index;
    (void)rgb;
}
void draw_text(const char *string, uint32_t x, uint32_t y,
               uint8_t foreground, uint8_t background) {
    graphics_text_draw(string, x, y, foreground, background);
}
_Noreturn void panic(const char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}
static void demo_sleep_ms(unsigned milliseconds) {
    assert(milliseconds == 1000);
    if (get_absolute_time() == 84000000u) longjmp(demo_cycle_done, 1);
    sleep_ms(milliseconds);
}

#define sleep_ms demo_sleep_ms
#define main unused_firmware_main
#include "../hdmi-text-demo.c"
#undef main
#undef sleep_ms

// Decode actual packed differential output as a TMDS receiver would. This
// includes bit-9 inversion; palette recovery in hdmi_decode_graphics_rgb()
// intentionally has different semantics and cannot be used for this preview.
static void receive_rgb(uint64_t packed, uint8_t rgb[3]) {
    for (unsigned lane = 0; lane < 3; ++lane) {
        unsigned symbol = 0;
        for (unsigned bit = 0; bit < 10; ++bit) {
            const unsigned offset = bit < 5 ? 6u * bit : 32u + 6u * (bit - 5u);
            unsigned data = (unsigned)(packed >> (offset + 2u * lane)) & 1u;
            if (HDMI_PIN_invert_diffpairs) data ^= 1u;
            symbol |= data << bit;
        }
        unsigned q = symbol & 255u;
        if (symbol & 512u) q ^= 255u;
        unsigned value = q ^ (q << 1u);
        if (!(symbol & 256u)) value ^= 254u;
        const unsigned channel = HDMI_PIN_RGB_notBGR ? 2u - lane : lane;
        rgb[channel] = (uint8_t)value;
    }
}

static int render_frame(const char *path, unsigned selection) {
    demo_draw(selection);
    FILE *output = fopen(path, "wb");
    if (output == NULL) {
        perror(path);
        return 1;
    }
    if (fprintf(output, "P6\n# Software render; not a hardware capture.\n640 480\n255\n") < 0) {
        fclose(output);
        return 1;
    }
    for (unsigned y = 0; y < 480; ++y) {
        struct graphics_text_line line;
        const bool visible = graphics_text_line(y, 480, &line);
        uint8_t row[640 * 3];
        for (unsigned x = 0; x < 640; ++x) {
            unsigned color = 0;
            if (visible) {
                const unsigned width = line.columns == 40 ? 16u : 8u;
                const unsigned column = x / width;
                const unsigned bit = (x % width) / (width / 8u);
                const uint16_t cell = graphics_text_cell(&line, column);
                const uint8_t attribute = (uint8_t)(cell >> 8u);
                const uint8_t glyph = graphics_text_glyph(&line, column,
                                                          (uint8_t)cell, attribute);
                color = (glyph & (1u << bit)) ? attribute & 15u
                        : (attribute >> 4u) & (line.blink_enabled ? 7u : 15u);
            }
            receive_rgb(hdmi_text_pixel(GRAPHICS_CGA_RGB(color), x & 1u), &row[x * 3u]);
        }
        if (fwrite(row, 1, sizeof row, output) != sizeof row) {
            perror(path);
            fclose(output);
            return 1;
        }
    }
    if (fclose(output) != 0) {
        perror(path);
        return 1;
    }
    printf("%s\n", path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s OUTPUT_DIR\n", argv[0]);
        return 2;
    }
    checking_cycle = true;
    if (setjmp(demo_cycle_done) == 0) unused_firmware_main();
    checking_cycle = false;
    assert(mode_changes == 5);
    assert(graphics_frames > 1000);
    assert(graphics_source_is_text());
    puts("Demo cycle OK: three 8-second text modes, 60-second 3D demo, back to text.");
    static const char *const names[3] = {
        "hdmi-text-80x30.ppm", "hdmi-text-80x25.ppm", "hdmi-text-40x25.ppm"
    };
    graphics_text_init();
    // Same first-frame checker pattern as the firmware's main(). No tick is
    // advanced, so the blinking cursor is shown in its visible phase.
    for (unsigned y = 0; y < 16; ++y)
        font_8x16[0xfeu * 16u + y] = (y & 1u) ? 0xaau : 0x55u;
    for (unsigned mode = 0; mode < 3; ++mode) {
        char path[4096];
        const int length = snprintf(path, sizeof path, "%s/%s", argv[1], names[mode]);
        if (length < 0 || (size_t)length >= sizeof path) {
            fprintf(stderr, "Output path is too long\n");
            return 1;
        }
        if (render_frame(path, mode) != 0) return 1;
    }
    return 0;
}
