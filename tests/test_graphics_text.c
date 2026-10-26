// Exercise production code, not a duplicate renderer. From the repository root:
// gcc -std=c2x -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
//   -DHDMI=1 -Itests/stubs -Idrivers/graphics -Idrivers/hdmi \
//   tests/test_graphics_text.c drivers/graphics/graphics_text.c \
//   drivers/graphics/graphics_modes.c -o /tmp/test_graphics_text
// /tmp/test_graphics_text
// Sandboxes without readable /proc thread lists may require
// ASAN_OPTIONS=detect_leaks=0 /tmp/test_graphics_text (ASAN/UBSAN remain enabled).

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics_modes.h"
#include "graphics_text.h"

static unsigned checks;
static unsigned frame_counter;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
        abort(); \
    } \
} while (0)

static graphics_text_config_t descriptor(uint8_t *vram, size_t size,
                                         const uint8_t *font,
                                         unsigned height, unsigned stride) {
    return (graphics_text_config_t){
        .vram = vram,
        .vram_size = size,
        .font = font,
        .font_size = 255u * stride + height,
        .font_height = (uint8_t)height,
        .font_stride = (uint16_t)stride
    };
}

static void select_mode(enum graphics_mode_t mode) {
    graphics_source_set_mode(mode);
    CHECK(graphics_source_is_text());
}

static struct graphics_text_line get_line(unsigned y) {
    struct graphics_text_line line;
    CHECK(graphics_text_line(y, 480, &line));
    return line;
}

static void tick_frame(void) {
    graphics_text_frame_tick();
    ++frame_counter;
}

static void set_hidden_phase(bool hidden) {
    while (((frame_counter & 16u) != 0u) != hidden) {
        tick_frame();
    }
}

static void test_borrowing_and_80x30(void) {
    uint8_t *vram = calloc(4800, 1);
    uint8_t *font = calloc(4096, 1);
    CHECK(vram != NULL && font != NULL);
    select_mode(TEXTMODE_DEFAULT);
    graphics_text_config_t cfg = descriptor(vram, 4800, font, 16, 16);
    CHECK(graphics_configure_text(&cfg));
    CHECK(text_buffer == vram);
    CHECK(graphics_text_columns_current() == 80);
    CHECK(graphics_text_columns() == 80);

    // Changing the caller's descriptor does not change the copied descriptor.
    cfg.vram = NULL;
    cfg.font = NULL;
    struct graphics_text_line line = get_line(3);
    CHECK(line.cells == vram);
    CHECK(line.font == font);
    CHECK(line.glyph_line == 3 && line.columns == 80);

    // Both allocations remain live: no snapshot/copy may hide emulator writes.
    vram[0] = 'A';
    vram[1] = 0x1e;
    font['A' * 16 + 3] = 0x96;
    CHECK(graphics_text_cell(&line, 0) == 0x1e41);
    CHECK(graphics_text_glyph(&line, 0, 'A', 0x1e) == 0x96);
    font['A' * 16 + 3] = 0x42;
    vram[0] = 'B';
    vram[1] = 0x4f;
    font['B' * 16 + 3] = 0x81;
    CHECK(graphics_text_cell(&line, 0) == 0x4f42);
    CHECK(graphics_text_glyph(&line, 0, 'B', 0x4f) == 0x81);

    // Walk the entire visible image; its final cell must be in this 4800B page.
    for (unsigned y = 0; y < 480; ++y) {
        line = get_line(y);
        CHECK(line.cells == vram + (y / 16) * 160);
        CHECK(line.glyph_line == y % 16);
        for (unsigned x = 0; x < 80; ++x) {
            const uint16_t cell = graphics_text_cell(&line, x);
            CHECK(cell == (uint16_t)(line.cells[2*x] |
                    (uint16_t)line.cells[2*x+1] << 8));
            (void)graphics_text_glyph(&line, x, (uint8_t)cell,
                                      (uint8_t)(cell >> 8));
        }
    }
    CHECK(!graphics_text_line(480, 480, &line));
    CHECK(!graphics_text_line(UINT_MAX, 480, &line));
    CHECK(!graphics_text_line(0, 479, &line));
    CHECK(!graphics_text_line(0, 480, NULL));

    graphics_set_textbuffer(NULL);
    CHECK(!graphics_text_line(0, 480, &line));
    free(font);
    free(vram);
}

static void test_25_row_borders_and_40_column_stride(void) {
    uint8_t *vram = malloc(4000);
    uint8_t *font = calloc(4096, 1);
    CHECK(vram != NULL && font != NULL);
    memset(vram, 0x5a, 4000);
    select_mode(TEXTMODE_80x25_COLOR);
    graphics_text_config_t cfg = descriptor(vram, 4000, font, 16, 16);
    CHECK(graphics_configure_text(&cfg));
    struct graphics_text_line line;
    for (unsigned y = 0; y < 480; ++y) {
        const bool visible = graphics_text_line(y, 480, &line);
        CHECK(visible == (y >= 40 && y < 440));
        if (visible) {
            CHECK(line.cells == vram + (y - 40) / 16 * 160);
            CHECK(graphics_text_cell(&line, 79) == 0x5a5a);
        }
    }
    // The 240-line mapping is useful for another output, without another font.
    CHECK(!graphics_text_line(19, 240, &line));
    CHECK(graphics_text_line(20, 240, &line));
    CHECK(line.cells == vram && line.glyph_line == 0);
    CHECK(graphics_text_line(219, 240, &line));
    CHECK(line.cells == vram + 24 * 160 && line.glyph_line == 14);
    CHECK(!graphics_text_line(220, 240, &line));

    select_mode(TEXTMODE_40x25_COLOR);
    cfg.vram_size = 2000;
    CHECK(graphics_configure_text(&cfg));
    CHECK(graphics_text_columns() == 40);
    line = get_line(56);
    CHECK(line.columns == 40);
    CHECK(line.cells == vram + 80);
    line = get_line(439);
    CHECK(line.cells == vram + 1920);
    CHECK(graphics_text_cell(&line, 39) == 0x5a5a);

    // An undersized page may show complete rows only; the renderer must never
    // read the final partial row or invent storage beyond the provided size.
    cfg.vram_size = 1922;
    CHECK(graphics_configure_text(&cfg));
    CHECK(!graphics_text_line(424, 480, &line));
    cfg.vram_size = 78;
    CHECK(graphics_configure_text(&cfg));
    CHECK(!graphics_text_line(40, 480, &line));
    cfg.vram_size = 2000;
    cfg.row_stride = 78;
    CHECK(graphics_configure_text(&cfg));
    CHECK(!graphics_text_line(40, 480, &line));

    graphics_set_textbuffer(NULL);
    free(font);
    free(vram);
}

static void test_font_layouts_and_vertical_geometry(void) {
    uint8_t vram[4800] = {0};
    uint8_t *font = calloc(8192, 1);
    CHECK(font != NULL);
    select_mode(TEXTMODE_DEFAULT);
    graphics_text_config_t cfg = descriptor(vram, sizeof(vram), font, 16, 32);
    cfg.msb_first = true;
    CHECK(graphics_configure_text(&cfg));
    font[255 * 32 + 15] = 0x96;
    struct graphics_text_line line = get_line(15);
    CHECK(line.font_stride == 32 && line.glyph_line == 15);
    CHECK(graphics_text_glyph(&line, 0, 255, 0) == 0x69);
    cfg.msb_first = false;
    CHECK(graphics_configure_text(&cfg));
    line = get_line(15);
    CHECK(graphics_text_glyph(&line, 0, 255, 0) == 0x96);

    cfg = descriptor(vram, sizeof(vram), font, 8, 8);
    CHECK(graphics_configure_text(&cfg));
    for (unsigned y = 0; y < 32; ++y) {
        line = get_line(y);
        CHECK(line.cells == vram + y / 16 * 160);
        CHECK(line.glyph_line == (y % 16) / 2);
    }
    CHECK(graphics_text_line(239, 240, &line));
    CHECK(line.cells == vram + 29 * 160 && line.glyph_line == 7);

    cfg = descriptor(vram, sizeof(vram), font, 14, 32);
    CHECK(graphics_configure_text(&cfg));
    CHECK(!graphics_text_line(29, 480, &line));
    line = get_line(30);
    CHECK(line.glyph_line == 0 && line.cells == vram);
    line = get_line(449);
    CHECK(line.glyph_line == 13 && line.cells == vram + 29 * 160);
    CHECK(!graphics_text_line(450, 480, &line));

    // Valid font storage can still describe a display taller than 480 lines.
    cfg = descriptor(vram, sizeof(vram), font, 32, 32);
    CHECK(graphics_configure_text(&cfg));
    CHECK(!graphics_text_line(0, 480, &line));
    graphics_set_textbuffer(NULL);
    free(font);
}

static void test_page_offsets_and_wrapping(void) {
    uint8_t *vram = malloc(16384);
    uint8_t *font = calloc(4096, 1);
    CHECK(vram != NULL && font != NULL);
    for (unsigned i = 0; i < 16384; ++i) {
        vram[i] = (uint8_t)(i ^ (i >> 8));
    }
    select_mode(TEXTMODE_80x25_COLOR);
    graphics_text_config_t cfg = descriptor(vram, 16384, font, 16, 16);
    cfg.start_offset = 4096;
    cfg.row_stride = 192;
    CHECK(graphics_configure_text(&cfg));
    struct graphics_text_line line = get_line(56);
    CHECK(line.cells == vram + 4288);
    line = get_line(439);
    CHECK(line.cells == vram + 8704);

    // CRTC start addresses may wrap the last columns to VRAM byte zero.
    cfg.start_offset = 16380;
    cfg.wrap_vram = true;
    CHECK(graphics_configure_text(&cfg));
    line = get_line(40);
    CHECK(line.wrap_at == 4);
    CHECK(graphics_text_cell(&line, 0) ==
          (uint16_t)(vram[16380] | (uint16_t)vram[16381] << 8));
    CHECK(graphics_text_cell(&line, 1) ==
          (uint16_t)(vram[16382] | (uint16_t)vram[16383] << 8));
    CHECK(graphics_text_cell(&line, 2) ==
          (uint16_t)(vram[0] | (uint16_t)vram[1] << 8));
    for (unsigned y = 40; y < 440; y += 16) {
        line = get_line(y);
        for (unsigned x = 0; x < 80; ++x) {
            const size_t a = (16380 + ((y - 40) / 16) * 192 + x * 2) % 16384;
            CHECK(graphics_text_cell(&line, x) ==
                  (uint16_t)(vram[a] | (uint16_t)vram[a + 1] << 8));
        }
    }

    // Exercise overflow-safe reduction of an invalidly large emulator address.
    cfg.start_offset = SIZE_MAX - 1;
    CHECK(graphics_configure_text(&cfg));
    line = get_line(40);
    CHECK(line.cells == vram + (SIZE_MAX - 1) % 16384);
    cfg.wrap_vram = false;
    CHECK(!graphics_configure_text(&cfg));

    graphics_set_textbuffer(NULL);
    free(font);
    free(vram);
}

static void test_cursor_blink_and_bright_background(void) {
    uint8_t vram[4800] = {0};
    uint8_t *font = calloc(4096, 1);
    CHECK(font != NULL);
    select_mode(TEXTMODE_DEFAULT);
    graphics_text_config_t cfg = descriptor(vram, sizeof(vram), font, 16, 16);
    CHECK(graphics_configure_text(&cfg));
    memset(font + 'X' * 16, 0x81, 16);
    set_hidden_phase(false);
    graphics_set_text_blink(true);
    graphics_set_text_cursor(3 * 2, 14, 15, true);
    struct graphics_text_line line = get_line(13);
    CHECK(!line.cursor_visible);
    line = get_line(14);
    CHECK(line.cursor_visible && line.cursor_column == 3);
    CHECK(graphics_text_glyph(&line, 3, 'X', 0x07) == 0xff);
    CHECK(graphics_text_glyph(&line, 2, 'X', 0x07) == 0x81);

    set_hidden_phase(true);
    line = get_line(14);
    CHECK(line.blink_enabled && line.blink_hidden);
    CHECK(!line.cursor_visible);
    CHECK(graphics_text_glyph(&line, 3, 'X', 0x87) == 0);
    CHECK(graphics_text_glyph(&line, 3, 'X', 0x07) == 0x81);
    graphics_set_text_blink(false);
    line = get_line(14);
    CHECK(!line.blink_enabled);
    CHECK(graphics_text_glyph(&line, 3, 'X', 0xf0) == 0x81);
    // Attribute bit7 becomes background intensity when blink is disabled.
    CHECK(((0xf0u >> 4) & (line.blink_enabled ? 7u : 15u)) == 15);
    graphics_set_text_blink(true);
    line = get_line(14);
    CHECK(((0xf0u >> 4) & (line.blink_enabled ? 7u : 15u)) == 7);

    set_hidden_phase(false);
    graphics_set_text_cursor(7, 0, 15, true); // Odd byte is not a cell address.
    line = get_line(0);
    CHECK(!line.cursor_visible);
    graphics_set_text_cursor(SIZE_MAX - 1, 0, 15, true);
    line = get_line(0);
    CHECK(!line.cursor_visible);
    graphics_set_text_cursor(0, 15, 14, true); // Disabled/split shape unsupported.
    line = get_line(15);
    CHECK(!line.cursor_visible);
    graphics_set_text_cursor(0, 0, 15, false);
    line = get_line(0);
    CHECK(!line.cursor_visible);

    // Wrapped cursor location follows the same address map as wrapped cells.
    cfg.wrap_vram = true;
    cfg.start_offset = sizeof(vram) - 4;
    cfg.cursor_enabled = true;
    cfg.cursor_offset = 0;
    cfg.cursor_start = 0;
    cfg.cursor_end = 15;
    CHECK(graphics_configure_text(&cfg));
    line = get_line(0);
    CHECK(line.cursor_visible && line.cursor_column == 2);

    // Counter wrap must not change the 16-visible/16-hidden frame cadence.
    for (unsigned i = 0; i < 300; ++i) {
        line = get_line(0);
        CHECK(line.blink_hidden == ((frame_counter & 16u) != 0));
        tick_frame();
    }
    graphics_set_textbuffer(NULL);
    free(font);
}

static void test_draw_clipping_clear_and_page_preservation(void) {
    enum { GUARD = 32, VRAM_SIZE = 8192 };
    uint8_t *allocation = malloc(VRAM_SIZE + 2*GUARD);
    uint8_t *font = calloc(4096, 1);
    CHECK(allocation != NULL && font != NULL);
    memset(allocation, 0xa5, VRAM_SIZE + 2*GUARD);
    uint8_t *vram = allocation + GUARD;
    select_mode(TEXTMODE_40x25_COLOR);
    graphics_text_config_t cfg = descriptor(vram, VRAM_SIZE, font, 16, 16);
    cfg.start_offset = 2048;
    cfg.row_stride = 96;
    CHECK(graphics_configure_text(&cfg));
    graphics_text_draw("ABCDE", 38, 24, 0x23, 0x4e);
    const size_t last_row = 2048 + 24*96;
    CHECK(vram[last_row + 76] == 'A');
    CHECK(vram[last_row + 77] == 0xe3);
    CHECK(vram[last_row + 78] == 'B');
    CHECK(vram[last_row + 79] == 0xe3);
    CHECK(vram[last_row + 80] == 0xa5);
    graphics_text_draw("BAD", 40, 0, 1, 2);
    graphics_text_draw("BAD", 0, 25, 1, 2);
    graphics_text_draw("BAD", UINT_MAX, UINT_MAX, 1, 2);
    graphics_text_draw(NULL, 0, 0, 1, 2);
    CHECK(vram[2048] == 0xa5);
    clrScr(3);
    for (unsigned y = 0; y < 25; ++y) {
        for (unsigned x = 0; x < 40; ++x) {
            CHECK(vram[2048 + y*96 + x*2] == ' ');
            CHECK(vram[2048 + y*96 + x*2 + 1] == 0x30);
        }
        for (unsigned x = 80; x < 96; ++x) {
            CHECK(vram[2048 + y*96 + x] == 0xa5);
        }
    }
    for (unsigned i = 0; i < 2048; ++i) {
        CHECK(vram[i] == 0xa5);
    }
    for (unsigned i = 2048 + 25*96; i < VRAM_SIZE; ++i) {
        CHECK(vram[i] == 0xa5);
    }
    for (unsigned i = 0; i < GUARD; ++i) {
        CHECK(allocation[i] == 0xa5);
        CHECK(allocation[GUARD + VRAM_SIZE + i] == 0xa5);
    }

    cfg.start_offset = VRAM_SIZE - 2;
    cfg.row_stride = 0;
    cfg.wrap_vram = true;
    CHECK(graphics_configure_text(&cfg));
    graphics_text_draw("AB", 0, 0, 14, 1);
    CHECK(vram[VRAM_SIZE-2] == 'A' && vram[VRAM_SIZE-1] == 0x1e);
    CHECK(vram[0] == 'B' && vram[1] == 0x1e);

    // Non-wrapping writes stop when supplied storage ends, including clear.
    cfg.wrap_vram = false;
    CHECK(graphics_configure_text(&cfg));
    graphics_text_draw("XYZ", 0, 0, 7, 4);
    CHECK(vram[VRAM_SIZE-2] == 'X' && vram[VRAM_SIZE-1] == 0x47);
    CHECK(vram[0] == 'B' && vram[1] == 0x1e);
    clrScr(2);
    CHECK(vram[VRAM_SIZE-2] == ' ' && vram[VRAM_SIZE-1] == 0x20);
    for (unsigned i = 0; i < GUARD; ++i) {
        CHECK(allocation[i] == 0xa5);
        CHECK(allocation[GUARD + VRAM_SIZE + i] == 0xa5);
    }

    graphics_set_textbuffer(NULL);
    graphics_text_draw("NO VRAM", 0, 0, 15, 0);
    clrScr(0);
    free(font);
    free(allocation);
}

static void test_invalid_configuration_and_legacy_binding(void) {
    uint8_t vram[4800] = {0};
    uint8_t font[4096] = {0};
    select_mode(TEXTMODE_DEFAULT);
    graphics_text_config_t good = descriptor(vram, sizeof(vram), font, 16, 16);
    CHECK(graphics_configure_text(&good));
    CHECK(!graphics_configure_text(NULL));
    graphics_text_config_t bad = good;
#define REJECT(field, value) do { \
    bad = good; bad.field = (value); \
    CHECK(!graphics_configure_text(&bad)); \
} while (0)
    REJECT(vram, NULL);
    REJECT(vram_size, 0);
    REJECT(vram_size, 1);
    REJECT(vram_size, 4799);
    REJECT(start_offset, 1);
    REJECT(start_offset, 4800);
    REJECT(row_stride, 161);
    REJECT(font, NULL);
    REJECT(font_height, 0);
    REJECT(font_height, 33);
    REJECT(font_stride, 15);
    REJECT(font_size, 4095);
#undef REJECT
    struct graphics_text_line line = get_line(0);
    CHECK(line.cells == vram && line.font == font);
    CHECK(!graphics_set_text_font(NULL, 16, 16, false));
    CHECK(!graphics_set_text_font(font, 0, 16, false));
    CHECK(!graphics_set_text_font(font, 33, 33, false));
    CHECK(!graphics_set_text_font(font, 16, 15, false));

    uint8_t graphics_bytes[17];
    memset(graphics_bytes, 0x7b, sizeof(graphics_bytes));
    graphics_source_set_buffer(graphics_bytes, sizeof(graphics_bytes), 1);
    select_mode(TEXTMODE_80x25_BW);
    for (unsigned i = 0; i < sizeof(graphics_bytes); ++i) {
        CHECK(graphics_bytes[i] == 0x7b); // Mode switch must not clear borrowed VRAM.
    }
    graphics_source_set_buffer(NULL, 320, 240);
    graphics_set_textbuffer(vram);
    CHECK(graphics_set_text_font(font, 16, 16, false));
    line = get_line(439);
    CHECK(line.cells == vram + 3840);
    select_mode(TEXTMODE_DEFAULT);
    CHECK(!graphics_text_line(400, 480, &line)); // Legacy binding size stays 4000B.
    graphics_set_textbuffer(vram);              // Rebind after changing geometry.
    CHECK(graphics_text_line(479, 480, &line));

    // Both DOS monochrome/color aliases select the same bounded geometry.
    select_mode(TEXTMODE_40x25_BW);
    CHECK(graphics_text_columns_current() == 40);
    select_mode(TEXTMODE_53x30);
    CHECK(graphics_text_columns_current() == 40);
    select_mode(TEXTMODE_160x100);
    CHECK(graphics_text_columns_current() == 80);
    graphics_source_set_mode(VGA_320x240x256);
    CHECK(!graphics_source_is_text());
    graphics_set_textbuffer(NULL);
}

int main(void) {
    // Setters support pre-init use; initializing twice must not claim two locks.
    test_borrowing_and_80x30();
    graphics_text_init();
    graphics_text_init();
    test_25_row_borders_and_40_column_stride();
    test_font_layouts_and_vertical_geometry();
    test_page_offsets_and_wrapping();
    test_cursor_blink_and_bright_background();
    test_draw_clipping_clear_and_page_preservation();
    test_invalid_configuration_and_legacy_binding();
    printf("graphics_text: %u checks passed (borrowed buffers, geometry, bounds, fonts, pages, attributes)\n", checks);
    return 0;
}
