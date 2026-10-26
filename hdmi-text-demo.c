#include <graphics.h>

#include <hardware/gpio.h>
#include <pico/time.h>

// This is the application's video memory, not a second buffer in the driver.
// An emulator replaces these pointers with its existing B800 text memory and
// font map. All three demo geometries reuse the same 4,800-byte allocation.
static uint8_t demo_vram[80u * 30u * 2u] __attribute__((aligned(4)));

static const enum graphics_mode_t demo_modes[] = {
    TEXTMODE_80x30, TEXTMODE_80x25_COLOR, TEXTMODE_40x25_COLOR
};
static const char hex[] = "0123456789ABCDEF";

static void demo_repeat(unsigned x, unsigned y, unsigned width,
                         uint8_t character, uint8_t foreground,
                         uint8_t background, unsigned columns) {
    uint8_t *cell = demo_vram + (y * columns + x) * 2u;
    for (unsigned i = 0; i < width; ++i) {
        cell[0] = character;
        cell[1] = (uint8_t)(background << 4 | foreground);
        cell += 2;
    }
}

static void demo_draw(unsigned selection) {
    const unsigned columns = selection == 2u ? 40u : 80u;
    const unsigned rows = selection == 0u ? 30u : 25u;
    graphics_set_mode(demo_modes[selection]);

    // font_8x16 is the repository's existing writable font. Both pointers are
    // borrowed directly; no memcpy of the font or screen is needed.
    const graphics_text_config_t text = {
        .vram = demo_vram,
        .vram_size = sizeof demo_vram,
        .font = font_8x16,
        .font_size = sizeof font_8x16,
        .font_height = 16,
        .font_stride = 16,
        .row_stride = (uint16_t)(columns * 2u),
        .msb_first = false,
        .blink_enabled = false, // attribute bit 7 supplies background intensity
        .cursor_offset = ((rows - 1u) * columns + columns - 1u) * 2u,
        .cursor_start = 14,
        .cursor_end = 15,
        .cursor_enabled = true
    };
    if (!graphics_configure_text(&text)) {
        panic("Invalid HDMI text demo configuration");
    }
    clrScr(0);

    draw_text(columns == 80u ? "HDMI 640x480 / SHARED TEXT VRAM + FONT"
                             : "HDMI 640x480 / SHARED VRAM + FONT", 0, 0, 15, 1);
    draw_text(selection == 0u ? "80x30, 8x16 font, full 640x480"
              : selection == 1u ? "80x25, 8x16 font, 40px top/bottom"
                                 : "40x25, 16x16 cells, 40px borders", 0, 1, 7, 0);

    const unsigned background_label = columns / 2u;
    const unsigned background_x = background_label + 2u;
    const unsigned sample_width = columns == 80u ? 14u : 7u;
    const unsigned background_width = columns - background_x - 1u;
    draw_text("FG: black / gray", 0, 3, 15, 0);
    draw_text("BG: black text", background_label, 3, 15, 0);

    for (unsigned color = 0; color < 16; ++color) {
        const unsigned row = color + 4u;
        const char label[2] = {hex[color], 0};
        draw_text(label, 0, row, 15, 0);
        draw_text(label, background_label, row, 15, 0);
        demo_repeat(2, row, sample_width, ' ', color, 0, columns);
        demo_repeat(2u + sample_width, row, sample_width, ' ', color, 7, columns);
        const char *sample = columns == 80u ? "Aa0 HELLO DOS" : "Aa0 DOS";
        draw_text(sample, 2, row, color, 0);
        draw_text(sample, 2u + sample_width, row, color, 7);
        demo_repeat(background_x, row, background_width, ' ', 0, color, columns);
        draw_text(columns == 80u ? "BLACK TEXT ON BACKGROUND" : "BLACK Aa0", background_x,
                  row, 0, color);
    }

    draw_text("16 backgrounds (0-F); blink off", 0, 21, 14, 0);
    // Glyph FE is changed in place in the already borrowed font, demonstrating
    // that later font/VRAM writes remain visible without a rebind or upload.
    demo_repeat(0, 22, columns, 0xfeu, 15, 0, columns);
    if (rows == 30u) {
        draw_text("One-pixel checker above: adjacent pixels and rows must differ.",
                  0, 24, 7, 0);
        draw_text("16 foreground colors on black/gray; 16 backgrounds with black text.",
                  0, 25, 7, 0);
        draw_text("Modes rotate every 8 s: 80x30 -> 80x25 -> 40x25.", 0, 26, 7, 0);
        draw_text("No graphics framebuffer and no driver copy of VRAM/font.", 0, 27, 7, 0);
    }
    draw_text("LIVE FONT / CURSOR", 0, rows - 1u, 11, 0);
    demo_repeat(columns - 1u, rows - 1u, 1, ' ', 15, 0, columns);
}

int main(void) {
    for (unsigned y = 0; y < 16u; ++y) {
        font_8x16[0xfeu * 16u + y] = (y & 1u) ? 0xaau : 0x55u;
    }
    // Build the first screen before enabling DMA: the first visible frame is
    // already valid. The graphics framebuffer is disabled in this demo build.
    demo_draw(0);
    graphics_init();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    unsigned selection = 0;
    unsigned seconds = 0;
    while (true) {
        sleep_ms(1000);
        gpio_put(PICO_DEFAULT_LED_PIN, (seconds & 1u) != 0u);
        // Swap the checker phase once per second. This edits the same font map
        // that scanout sees, with no copying into a driver-owned font cache.
        for (unsigned y = 0; y < 16u; ++y) {
            font_8x16[0xfeu * 16u + y] ^= 0xffu;
        }
        if (++seconds == 8u) {
            seconds = 0;
            selection = (selection + 1u) % 3u;
            demo_draw(selection);
        }
    }
}
