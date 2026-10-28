// Host verification of the production HSTX pixel helpers and shared source
// descriptor. The reference reads application VRAM/font directly, one pixel
// at a time; it does not use the production cell/glyph or packing helpers.
#include <inttypes.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics_modes.h"
#include "graphics_text.h"
#include "hstx_render.h"

enum { PIXELS = 640, GUARD = 16, BORROWED_BYTES = 8192, BYTE_GUARD = 17 };
static uint64_t checks;
static unsigned rendered_rows;
static unsigned frames;
static alignas(4) uint16_t guarded_output[GUARD + PIXELS + GUARD];
static uint16_t expected[PIXELS];
static uint16_t text_colors[16];
// Odd offsets deliberately exercise unaligned byte-addressed font and VRAM.
static uint8_t vram_storage[BORROWED_BYTES + 2 * BYTE_GUARD];
static uint8_t font_storage[BORROWED_BYTES + 2 * BYTE_GUARD];

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
        abort(); \
    } \
} while (0)

static uint16_t reference_color(uint32_t rgb) {
    const unsigned r = (rgb >> 16) & 255u;
    const unsigned g = (rgb >> 8) & 255u;
    const unsigned b = rgb & 255u;
    return (uint16_t)(r / 8u + 32u * (g / 8u) + 1024u * (b / 8u));
}

static uint16_t *clear_output(void) {
    for (unsigned i = 0; i < GUARD + PIXELS + GUARD; ++i) {
        guarded_output[i] = 0xa55au;
    }
    uint16_t *output = guarded_output + GUARD;
    CHECK(((uintptr_t)output & 3u) == 0u);
    return output;
}

static void check_output(unsigned length) {
    for (unsigned i = 0; i < GUARD; ++i) CHECK(guarded_output[i] == 0xa55au);
    for (unsigned x = 0; x < length; ++x) CHECK(guarded_output[GUARD + x] == expected[x]);
    for (unsigned i = GUARD + length; i < GUARD + PIXELS + GUARD; ++i) {
        CHECK(guarded_output[i] == 0xa55au);
    }
}

static void check_borrowed_guards(void) {
    for (unsigned i = 0; i < BYTE_GUARD; ++i) {
        CHECK(vram_storage[i] == 0xa5u && font_storage[i] == 0xa5u);
        CHECK(vram_storage[BYTE_GUARD + BORROWED_BYTES + i] == 0xa5u);
        CHECK(font_storage[BYTE_GUARD + BORROWED_BYTES + i] == 0xa5u);
    }
}

static void set_hidden_phase(bool hidden) {
    while (((frames & 16u) != 0u) != hidden) {
        graphics_text_frame_tick();
        ++frames;
    }
}

static graphics_text_config_t make_config(unsigned columns, unsigned font_height,
                                          unsigned font_stride) {
    return (graphics_text_config_t){
        .vram = vram_storage + BYTE_GUARD,
        .font = font_storage + BYTE_GUARD,
        .vram_size = BORROWED_BYTES,
        .font_size = BORROWED_BYTES,
        .font_height = (uint8_t)font_height,
        .font_stride = (uint16_t)font_stride,
        .row_stride = (uint16_t)(columns * 2u)
    };
}

static bool reference_text(const graphics_text_config_t *cfg, unsigned columns,
                           unsigned rows, unsigned output_y, unsigned output_height,
                           bool hidden) {
    const unsigned scale_y = cfg->font_height <= 8u ? 2u : 1u;
    const unsigned cell_height = cfg->font_height * scale_y;
    const unsigned top = (480u - rows * cell_height) / 2u;
    const unsigned y = output_height == 240u ? output_y * 2u : output_y;
    if (output_y >= output_height || y < top || y >= top + rows * cell_height) {
        return false;
    }
    const unsigned row = (y - top) / cell_height;
    const unsigned glyph_y = ((y - top) % cell_height) / scale_y;
    const size_t stride = cfg->row_stride ? cfg->row_stride : columns * 2u;
    const size_t row_address = cfg->start_offset + row * stride;
    if (!cfg->wrap_vram && row_address + columns * 2u > cfg->vram_size) return false;
    const unsigned horizontal_scale = PIXELS / (columns * 8u);
    for (unsigned x = 0; x < PIXELS; ++x) {
        const unsigned column = x / (8u * horizontal_scale);
        const unsigned glyph_x = (x / horizontal_scale) % 8u;
        size_t address = row_address + column * 2u;
        if (cfg->wrap_vram) address %= cfg->vram_size;
        const uint8_t ch = cfg->vram[address];
        const uint8_t attribute = cfg->vram[address + 1u];
        const uint8_t glyph = cfg->font[(size_t)ch * cfg->font_stride + glyph_y];
        const unsigned bit = cfg->msb_first ? 7u - glyph_x : glyph_x;
        bool foreground = ((glyph >> bit) & 1u) != 0u;
        if (cfg->blink_enabled && hidden && (attribute & 128u)) foreground = false;
        const size_t cursor = cfg->wrap_vram ? cfg->cursor_offset % cfg->vram_size
                                           : cfg->cursor_offset;
        if (cfg->cursor_enabled && !hidden && cursor == address &&
            glyph_y >= cfg->cursor_start && glyph_y <= cfg->cursor_end) {
            foreground = true;
        }
        const unsigned background = cfg->blink_enabled ? (attribute / 16u) % 8u
                                                       : attribute / 16u;
        expected[x] = text_colors[foreground ? attribute % 16u : background];
    }
    return true;
}

static void check_row(const graphics_text_config_t *cfg, unsigned columns,
                      unsigned rows, unsigned y, unsigned height, bool hidden) {
    struct graphics_text_line line;
    const bool want_visible = reference_text(cfg, columns, rows, y, height, hidden);
    const bool visible = graphics_text_line(y, height, &line);
    CHECK(visible == want_visible);
    uint16_t *output = clear_output();
    if (visible) {
        CHECK(line.columns == columns);
        hstx_render_text(output, &line, text_colors);
        check_output(PIXELS);
        ++rendered_rows;
    } else {
        // No renderer call is allowed for a blank/border descriptor.
        check_output(0);
    }
}

static void test_color_conversion(void) {
    CHECK(hstx_rgb888_to_bgr555(0xff0000u) == 0x001fu);
    CHECK(hstx_rgb888_to_bgr555(0x00ff00u) == 0x03e0u);
    CHECK(hstx_rgb888_to_bgr555(0x0000ffu) == 0x7c00u);
    CHECK(hstx_rgb888_to_bgr555(0xffffffu) == 0x7fffu);
    CHECK(hstx_rgb888_to_bgr555(0u) == 0u);
    for (unsigned channel = 0; channel < 3u; ++channel) {
        for (unsigned value = 0; value < 256u; ++value) {
            const uint32_t rgb = value << (channel * 8u);
            CHECK(hstx_rgb888_to_bgr555(rgb) == reference_color(rgb));
        }
    }
    // Every BGR555 value, at both ends of each eight-value RGB888 bin.
    for (unsigned b = 0; b < 32u; ++b) {
        for (unsigned g = 0; g < 32u; ++g) {
            for (unsigned r = 0; r < 32u; ++r) {
                for (unsigned low = 0; low < 8u; ++low) {
                    const uint32_t rgb = ((r * 8u + ((low & 1u) ? 7u : 0u)) << 16) |
                                        ((g * 8u + ((low & 2u) ? 7u : 0u)) << 8) |
                                        (b * 8u + ((low & 4u) ? 7u : 0u));
                    CHECK(hstx_rgb888_to_bgr555(rgb) == reference_color(rgb));
                    CHECK(hstx_rgb888_to_bgr555(rgb | 0xaa000000u) == reference_color(rgb));
                }
            }
        }
    }
}

static void test_all_indexed_colors(void) {
    uint8_t source_storage[320u + 2u * BYTE_GUARD];
    uint16_t palette[256];
    memset(source_storage, 0xa5, sizeof(source_storage));
    uint8_t *source = source_storage + BYTE_GUARD;
    for (unsigned i = 0; i < 256u; ++i) palette[i] = (uint16_t)((i * 73u) & 0x7fffu);
    for (unsigned x = 0; x < 320u; ++x) source[x] = (uint8_t)x;
    for (unsigned pass = 0; pass < 2u; ++pass) {
        for (unsigned x = 0; x < 320u; ++x) expected[x] = palette[source[x]];
        hstx_render_indexed(clear_output(), source, palette);
        check_output(320);
        for (unsigned i = 252; i < 256u; ++i) {
            CHECK(guarded_output[GUARD + i] == palette[i]);
            CHECK(guarded_output[GUARD + i] != palette[48u + i - 252u]);
        }
        palette[253] = 0x7fffu;
    }
    for (unsigned i = 0; i < BYTE_GUARD; ++i) {
        CHECK(source_storage[i] == 0xa5u);
        CHECK(source_storage[BYTE_GUARD + 320u + i] == 0xa5u);
    }
    for (unsigned x = 0; x < 320u; ++x) CHECK(source[x] == (uint8_t)x);
}

static void test_glyph_attribute_matrix(void) {
    for (unsigned columns = 40; columns <= 80u; columns += 40u) {
        const unsigned rows = columns == 40u ? 25u : 30u;
        graphics_source_set_mode(columns == 40u ? TEXTMODE_40x25_COLOR : TEXTMODE_DEFAULT);
        graphics_text_config_t cfg = make_config(columns, 16, 16);
        const unsigned y = columns == 40u ? 40u : 0u;
        for (unsigned order = 0; order < 2u; ++order) {
            cfg.msb_first = order != 0u;
            for (unsigned blink = 0; blink < 3u; ++blink) {
                cfg.blink_enabled = blink != 0u;
                const bool hidden = blink == 2u;
                set_hidden_phase(hidden);
                CHECK(graphics_configure_text(&cfg));
                for (unsigned glyph = 0; glyph < 256u; ++glyph) {
                    font_storage[BYTE_GUARD + 'X' * 16u] = (uint8_t)glyph;
                    for (unsigned base = 0; base < 256u; base += columns) {
                        for (unsigned x = 0; x < columns; ++x) {
                            cfg.vram[x * 2u] = 'X';
                            cfg.vram[x * 2u + 1u] = (uint8_t)(base + x);
                        }
                        check_row(&cfg, columns, rows, y, 480, hidden);
                    }
                }
            }
        }
    }
    check_borrowed_guards();
}

static void fill_live_storage(void) {
    uint8_t *vram = vram_storage + BYTE_GUARD;
    uint8_t *font = font_storage + BYTE_GUARD;
    for (unsigned i = 0; i < BORROWED_BYTES; i += 2u) {
        vram[i] = (uint8_t)(i / 2u);
        vram[i + 1u] = (uint8_t)((i / 2u) * 73u + 19u);
    }
    for (unsigned i = 0; i < BORROWED_BYTES; ++i) {
        font[i] = (uint8_t)(i * 37u ^ (i >> 5) * 53u);
    }
}

static void test_geometry_borders_and_fonts(void) {
    const enum graphics_mode_t modes[] = {
        TEXTMODE_DEFAULT, TEXTMODE_80x25_COLOR, TEXTMODE_40x25_COLOR, TEXTMODE_53x30
    };
    const unsigned heights[] = {8, 14, 16};
    fill_live_storage();
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
        const unsigned columns = mode >= 2u ? 40u : 80u;
        const unsigned rows = (mode == 1u || mode == 2u) ? 25u : 30u;
        graphics_source_set_mode(modes[mode]);
        for (unsigned h = 0; h < sizeof(heights) / sizeof(heights[0]); ++h) {
            graphics_text_config_t cfg = make_config(columns, heights[h], 32);
            cfg.row_stride += 16u;
            cfg.cursor_enabled = true;
            cfg.cursor_start = 0;
            cfg.cursor_end = (uint8_t)(heights[h] - 1u);
            for (unsigned wrap = 0; wrap < 2u; ++wrap) {
                cfg.wrap_vram = wrap != 0u;
                cfg.start_offset = wrap ? BORROWED_BYTES - 2u : 64u;
                cfg.cursor_offset = wrap ? 0u : 68u;
                for (unsigned order = 0; order < 2u; ++order) {
                    cfg.msb_first = order != 0u;
                    for (unsigned phase = 0; phase < 3u; ++phase) {
                        cfg.blink_enabled = phase != 0u;
                        const bool hidden = phase == 2u;
                        set_hidden_phase(hidden);
                        CHECK(graphics_configure_text(&cfg));
                        for (unsigned output_height = 240; output_height <= 480u; output_height += 240u) {
                            // Include borders, cell transitions and the final physical row.
                            for (unsigned y = 0; y < output_height; y += 7u) {
                                check_row(&cfg, columns, rows, y, output_height, hidden);
                            }
                            check_row(&cfg, columns, rows, output_height - 1u, output_height, hidden);
                        }
                    }
                }
            }
        }
    }
    check_borrowed_guards();
}

static void test_all_wrap_splits_and_cursor(void) {
    fill_live_storage();
    for (unsigned columns = 40; columns <= 80u; columns += 40u) {
        graphics_source_set_mode(columns == 40u ? TEXTMODE_40x25_COLOR : TEXTMODE_80x25_COLOR);
        graphics_text_config_t cfg = make_config(columns, 16, 32);
        cfg.vram_size = columns * 2u;
        cfg.wrap_vram = true;
        cfg.cursor_enabled = true;
        cfg.cursor_start = 2;
        cfg.cursor_end = 14;
        cfg.cursor_offset = 0;
        for (unsigned prefix_cells = 1; prefix_cells <= columns; ++prefix_cells) {
            cfg.start_offset = cfg.vram_size - prefix_cells * 2u;
            for (unsigned order = 0; order < 2u; ++order) {
                cfg.msb_first = order != 0u;
                for (unsigned phase = 0; phase < 3u; ++phase) {
                    cfg.blink_enabled = phase != 0u;
                    const bool hidden = phase == 2u;
                    set_hidden_phase(hidden);
                    CHECK(graphics_configure_text(&cfg));
                    check_row(&cfg, columns, 25, 41, 480, hidden); // before cursor shape
                    check_row(&cfg, columns, 25, 42, 480, hidden); // cursor first row
                    check_row(&cfg, columns, 25, 54, 480, hidden); // cursor last row
                    check_row(&cfg, columns, 25, 55, 480, hidden); // after cursor shape
                }
            }
        }
    }
    check_borrowed_guards();
}

static void test_source_binding_before_and_after_init(void) {
    // Exercise actual composed indexed rows as well as direct framebuffer rows.
    uint8_t small[] = {1, 2, 3, 4, 5, 6};
    uint8_t staging[GRAPHICS_FRAME_WIDTH];
    uint8_t frame[GRAPHICS_FRAME_WIDTH * 2u];
    memset(frame, 253, sizeof(frame));
    graphics_source_set_buffer(small, 3, 2);
    graphics_source_set_offset(0, 0);
    graphics_source_set_mode(GRAPHICSMODE_DEFAULT);
    const uint8_t *row = graphics_source_line(NULL, 1, staging);
    CHECK(row == staging && row[158] == 4 && row[159] == 5 && row[160] == 6);
    graphics_source_init();
    graphics_source_set_offset(1, 1);
    row = graphics_source_line(NULL, 2, staging);
    CHECK(row == staging && row[159] == 4 && row[160] == 5 && row[161] == 6);
    row = graphics_source_line(NULL, 0, staging);
    for (unsigned x = 0; x < GRAPHICS_FRAME_WIDTH; ++x) CHECK(row[x] == 0u);
    graphics_source_set_mode(VGA_320x240x256);
    CHECK(graphics_source_line(frame, 1, staging) == frame + GRAPHICS_FRAME_WIDTH);
    CHECK(!graphics_source_is_text());
    graphics_source_set_buffer(NULL, GRAPHICS_FRAME_WIDTH, GRAPHICS_FRAME_HEIGHT);
    graphics_source_set_offset(0, 0);
}

int main(void) {
    memset(vram_storage, 0xa5, sizeof(vram_storage));
    memset(font_storage, 0xa5, sizeof(font_storage));
    for (unsigned i = 0; i < 16u; ++i) text_colors[i] = reference_color(GRAPHICS_CGA_RGB(i));
    graphics_text_init();
    test_color_conversion();
    test_all_indexed_colors();
    test_source_binding_before_and_after_init();
    test_glyph_attribute_matrix();
    test_geometry_borders_and_fonts();
    test_all_wrap_splits_and_cursor();
    graphics_set_textbuffer(NULL);
    printf("HSTX renderer: %" PRIu64 " checks, %u text rows; all 256 palette indices, "
           "BGR555 order, borrowed fonts/VRAM, text geometry, attributes, wrap and guards passed.\n",
           checks, rendered_rows);
    puts("Host semantics only: no DMA, IRQ deadline, clock, or inter-core timing claim.");
    return 0;
}
