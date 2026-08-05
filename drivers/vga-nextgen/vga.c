#include "vga.h"

#include <stddef.h>
#include <string.h>

#include <graphics_modes.h>

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/pio.h>
#include <hardware/sync.h>
#include <pico/platform.h>

enum {
    VGA_DUAL_LINE_SAMPLES = 800,
    VGA_DUAL_HSYNC_SAMPLES = 96,
    VGA_DUAL_ACTIVE_OFFSET = 144,
    VGA_DUAL_ACTIVE_LINES = 480,
    VGA_DUAL_TOTAL_LINES = 525,
    VGA_DUAL_VSYNC_FIRST_LINE = 490,
    VGA_DUAL_VSYNC_LINES = 2,
    VGA_DUAL_SOURCE_WIDTH = 320,
    VGA_DUAL_LINE_WORDS = VGA_DUAL_LINE_SAMPLES / sizeof(uint32_t),
    VGA_DUAL_BLANK_BUFFER = 0,
    VGA_DUAL_VSYNC_BUFFER = 1,
    VGA_DUAL_ACTIVE_BUFFER_0 = 2,
    VGA_DUAL_ACTIVE_BUFFER_1 = 3,
    VGA_DUAL_BLANK_LEVEL = 0xc0,
    VGA_DUAL_HSYNC_LEVEL = 0x80,
    VGA_DUAL_VSYNC_LEVEL = 0x40,
    VGA_DUAL_COMBINED_SYNC_LEVEL = 0x00,
    VGA_DUAL_PIXEL_CLOCK_HZ = 25175000,
    // 480 active lines / 16 = the 30 text rows of TEXTMODE_ROWS.
    VGA_DUAL_TEXT_FONT_HEIGHT = 16
};

static const uint16_t vga_dual_program_instructions[] = {
        0x6008 // out pins, 8
};

static const struct pio_program vga_dual_program = {
        .instructions = vga_dual_program_instructions,
        .length = 1,
        .origin = -1
};

// Text colors as VGA bus values. The RGB parts of the CGA set are only 0x00,
// 0x55, 0xaa, or 0xff, and for those four the lower and upper dither levels of
// vga_set_palette() agree, so one value serves and text never shimmers.
#define VGA_DUAL_TEXT_LEVEL(part) ((part) / 85u)
#define VGA_DUAL_TEXT_DAC(color) ((uint16_t)(VGA_DUAL_BLANK_LEVEL | \
        VGA_DUAL_TEXT_LEVEL(GRAPHICS_CGA_RED(color)) << 4 | \
        VGA_DUAL_TEXT_LEVEL(GRAPHICS_CGA_GREEN(color)) << 2 | \
        VGA_DUAL_TEXT_LEVEL(GRAPHICS_CGA_BLUE(color))))

// Two pixels for one 16-bit store. The low byte is the left pixel, and the
// index is the next two glyph bits, lowest bit first.
#define VGA_DUAL_TEXT_QUAD(foreground, background) \
    (uint16_t)(VGA_DUAL_TEXT_DAC(background) | VGA_DUAL_TEXT_DAC(background) << 8), \
    (uint16_t)(VGA_DUAL_TEXT_DAC(foreground) | VGA_DUAL_TEXT_DAC(background) << 8), \
    (uint16_t)(VGA_DUAL_TEXT_DAC(background) | VGA_DUAL_TEXT_DAC(foreground) << 8), \
    (uint16_t)(VGA_DUAL_TEXT_DAC(foreground) | VGA_DUAL_TEXT_DAC(foreground) << 8),

// The generated table is the initial data only; vga_init() copies it into the
// 2 KB RAM table below, which is where the older driver kept it too.
//
// This one table cannot stay in flash. A text line reads it four times for
// every character cell, so 320 flash reads land inside one 31.7 us VGA line,
// and each shares the single QSPI bus with whatever the other core is running.
// A late line buffer then shows up as a shaking picture. The font is read once
// per cell, an eighth as often, and stays in flash as before; VGA and NTSC read
// their fonts from there without trouble.
static const uint16_t vga_dual_text_pairs_source[256 * 4] = {
        GRAPHICS_TEXT_ATTRIBUTES(VGA_DUAL_TEXT_QUAD)
};

static uint16_t vga_dual_text_pairs[256 * 4] __attribute__((aligned(8)));

#if VGA_ENABLE_DITHER
static uint16_t vga_dual_palette[2][256] __attribute__((aligned(4)));
static uint8_t vga_dual_frame_phase;
#else
static uint16_t vga_dual_palette[256] __attribute__((aligned(4)));
#endif
static uint32_t vga_dual_scanline_buffers[4][VGA_DUAL_LINE_WORDS]
        __attribute__((aligned(16)));
// Scratch line for the mode composer. NTSC has its own, because the two
// backends build their lines from separate interrupts.
static uint8_t vga_dual_staging[VGA_DUAL_SOURCE_WIDTH] __attribute__((aligned(4)));

static PIO vga_dual_pio = PIO_VGA;
static uint vga_dual_sm;
static uint vga_dual_dma_data;
static uint vga_dual_dma_control;
static volatile uintptr_t vga_dual_next_line_address;
static const uint8_t *vga_dual_active_framebuffer;
static const uint8_t *volatile vga_dual_pending_framebuffer;

static inline uint32_t *vga_dual_line_buffer(const size_t index) {
    return vga_dual_scanline_buffers[index];
}

static void vga_dual_make_templates(void) {
    uint8_t *blank = (uint8_t *)vga_dual_line_buffer(VGA_DUAL_BLANK_BUFFER);
    uint8_t *vsync = (uint8_t *)vga_dual_line_buffer(VGA_DUAL_VSYNC_BUFFER);

    memset(blank, VGA_DUAL_BLANK_LEVEL, VGA_DUAL_LINE_SAMPLES);
    memset(blank, VGA_DUAL_HSYNC_LEVEL, VGA_DUAL_HSYNC_SAMPLES);

    memset(vsync, VGA_DUAL_VSYNC_LEVEL, VGA_DUAL_LINE_SAMPLES);
    memset(vsync, VGA_DUAL_COMBINED_SYNC_LEVEL, VGA_DUAL_HSYNC_SAMPLES);

    memcpy(vga_dual_line_buffer(VGA_DUAL_ACTIVE_BUFFER_0),
           blank, VGA_DUAL_LINE_SAMPLES);
    memcpy(vga_dual_line_buffer(VGA_DUAL_ACTIVE_BUFFER_1),
           blank, VGA_DUAL_LINE_SAMPLES);
}

#if PICO_RP2350
static void __time_critical_func(vga_dual_generate_active_line)(
        const size_t source_line, uint32_t *line_buffer) {
#else
static __force_inline void vga_dual_generate_active_line(
        const size_t source_line, uint32_t *line_buffer) {
#endif
    const uint8_t *source = graphics_source_line(vga_dual_active_framebuffer,
                                                 source_line,
                                                 vga_dual_staging);
    uint16_t *output = (uint16_t *)((uint8_t *)line_buffer +
                                  VGA_DUAL_ACTIVE_OFFSET);
    uint32_t groups = VGA_DUAL_SOURCE_WIDTH / 4u;
#if VGA_ENABLE_DITHER
    const uint16_t *palette =
            vga_dual_palette[(source_line ^ vga_dual_frame_phase) & 1u];
#else
    const uint16_t *palette = vga_dual_palette;
#endif

#pragma GCC unroll 4
    do {
#if PICO_RP2350
        uint32_t pixels;
        __builtin_memcpy(&pixels, source, sizeof(pixels));
        const uint32_t pixel0 = palette[(uint8_t)pixels];
        const uint32_t pixel1 = palette[(uint8_t)(pixels >> 8)];
        const uint32_t pixel2 = palette[(uint8_t)(pixels >> 16)];
        const uint32_t pixel3 = palette[(uint8_t)(pixels >> 24)];
        ((uint32_t *)output)[0] = pixel0 | pixel1 << 16;
        ((uint32_t *)output)[1] = pixel2 | pixel3 << 16;
#else
        output[0] = palette[source[0]];
        output[1] = palette[source[1]];
        output[2] = palette[source[2]];
        output[3] = palette[source[3]];
#endif
        source += 4;
        output += 4;
    } while (--groups);
}

// One text scanline: 80 columns of 8 pixels fill the whole 640-sample active
// area, so the picture keeps its full 640 x 480 size. Two pixels are written
// per 16-bit store; the four (left, right) color pairs of a cell are built once
// per column. The low byte of a 16-bit store is the left pixel.
static __force_inline void vga_dual_generate_text_line(
        const size_t line, uint32_t *line_buffer) {
    unsigned glyph_line;
    const uint8_t *cell = graphics_text_row(line, VGA_DUAL_TEXT_FONT_HEIGHT,
                                            &glyph_line);
    uint16_t *output = (uint16_t *)((uint8_t *)line_buffer +
                                    VGA_DUAL_ACTIVE_OFFSET);

    if (cell == NULL) {
        memset(output, (uint8_t)vga_dual_text_pairs[0], GRAPHICS_TEXT_SAMPLES);
        return;
    }

    const unsigned columns = graphics_text_columns();
    const bool wide = columns * 8u < GRAPHICS_TEXT_SAMPLES;

    for (unsigned column = columns; column--;) {
        const uint8_t character = *cell++;
        const uint8_t attribute = *cell++;
        uint8_t glyph = font_8x16[character * VGA_DUAL_TEXT_FONT_HEIGHT +
                                  glyph_line];
        const uint16_t *pair = &vga_dual_text_pairs[attribute * 4u];

        for (unsigned step = 4; step--;) {
            const uint16_t both = pair[glyph & 3u];
            glyph >>= 2;
            if (wide) {
                // A 40-column cell is twice as wide, so each pixel is sent
                // twice: the pair becomes two same-color pairs.
                *output++ = (uint16_t)((both & 0xffu) * 0x0101u);
                *output++ = (uint16_t)((both >> 8) * 0x0101u);
            } else {
                *output++ = both;
            }
        }
    }
}

static inline void vga_dual_prepare_line(const size_t line) {
    if (line < VGA_DUAL_ACTIVE_LINES) {
        // Text has one source line for every physical line, so unlike the
        // graphics modes it cannot skip the odd ones.
        if (graphics_source_is_text()) {
            uint32_t *line_buffer = (line & 1u) != 0u
                                    ? vga_dual_line_buffer(VGA_DUAL_ACTIVE_BUFFER_1)
                                    : vga_dual_line_buffer(VGA_DUAL_ACTIVE_BUFFER_0);
            vga_dual_generate_text_line(line, line_buffer);
            vga_dual_next_line_address = (uintptr_t)line_buffer;
            if (line == 0u && vga_dual_pending_framebuffer != NULL) {
                __mem_fence_acquire();
                vga_dual_active_framebuffer = vga_dual_pending_framebuffer;
                __mem_fence_release();
                vga_dual_pending_framebuffer = NULL;
            }
            return;
        }

        if ((line & 1u) != 0u) {
            return;
        }

        const size_t source_line = line >> 1u;
        const uint8_t *pending = NULL;
        if (line == 0u) {
            pending = vga_dual_pending_framebuffer;
            if (pending != NULL) {
                __mem_fence_acquire();
                vga_dual_active_framebuffer = pending;
            }
        }
        uint32_t *line_buffer = (source_line & 1u) != 0u
                                ? vga_dual_line_buffer(VGA_DUAL_ACTIVE_BUFFER_1)
                                : vga_dual_line_buffer(VGA_DUAL_ACTIVE_BUFFER_0);
        vga_dual_generate_active_line(source_line, line_buffer);
        if (pending != NULL) {
            __mem_fence_release();
            vga_dual_pending_framebuffer = NULL;
        }
        vga_dual_next_line_address = (uintptr_t)line_buffer;
        return;
    }

    if (line >= VGA_DUAL_VSYNC_FIRST_LINE &&
        line < VGA_DUAL_VSYNC_FIRST_LINE + VGA_DUAL_VSYNC_LINES) {
        vga_dual_next_line_address =
                (uintptr_t)vga_dual_line_buffer(VGA_DUAL_VSYNC_BUFFER);
        return;
    }
    vga_dual_next_line_address =
            (uintptr_t)vga_dual_line_buffer(VGA_DUAL_BLANK_BUFFER);
}

static void __time_critical_func(vga_dual_dma_handler)(void) {
    static size_t current_line;

    dma_hw->ints1 = 1u << vga_dual_dma_control;

    if (++current_line == VGA_DUAL_TOTAL_LINES) {
        current_line = 0;
    }
    size_t following_line = current_line + 1u;
    if (following_line == VGA_DUAL_TOTAL_LINES) {
        following_line = 0;
    }
#if VGA_ENABLE_DITHER
    if (following_line == 0u) {
        vga_dual_frame_phase ^= 1u;
    }
#endif
    vga_dual_prepare_line(following_line);
}

void vga_set_palette(const uint8_t index, const uint32_t color888) {
    static const uint8_t lower_level[8] = {
            0, 0, 1, 2, 2, 2, 3, 3
    };
    static const uint8_t upper_level[8] = {
            0, 1, 1, 1, 2, 3, 3, 3
    };

    const uint8_t red = (uint8_t)((color888 >> 16) & 0xffu) / 42u;
    const uint8_t green = (uint8_t)((color888 >> 8) & 0xffu) / 42u;
    const uint8_t blue = (uint8_t)(color888 & 0xffu) / 42u;
    const uint8_t first = (uint8_t)(VGA_DUAL_BLANK_LEVEL |
            lower_level[red] << 4 |
            lower_level[green] << 2 |
            lower_level[blue]);
    const uint8_t second = (uint8_t)(VGA_DUAL_BLANK_LEVEL |
            upper_level[red] << 4 |
            upper_level[green] << 2 |
            upper_level[blue]);

#if VGA_ENABLE_DITHER
    vga_dual_palette[0][index] =
            (uint16_t)(second | (uint16_t)first << 8);
    vga_dual_palette[1][index] =
            (uint16_t)(first | (uint16_t)second << 8);
#else
    vga_dual_palette[index] = (uint16_t)(second | (uint16_t)first << 8);
#endif
}

void vga_init(const uint8_t *framebuffer) {
    // Take the busiest text lookup out of flash, so a text line does not wait
    // on the QSPI bus while the other core is also reading flash.
    memcpy(vga_dual_text_pairs, vga_dual_text_pairs_source,
           sizeof vga_dual_text_pairs);

    vga_dual_active_framebuffer = framebuffer;
    vga_dual_pending_framebuffer = NULL;
#if VGA_ENABLE_DITHER
    vga_dual_frame_phase = 0;
#endif
    vga_dual_make_templates();
    vga_dual_generate_active_line(
            0, vga_dual_line_buffer(VGA_DUAL_ACTIVE_BUFFER_0));
    vga_dual_next_line_address = (uintptr_t)vga_dual_line_buffer(
            VGA_DUAL_ACTIVE_BUFFER_0);

    const uint program_offset = pio_add_program(vga_dual_pio,
                                                &vga_dual_program);
    vga_dual_sm = pio_claim_unused_sm(vga_dual_pio, true);

    for (uint pin = 0; pin < 8; pin++) {
        const uint gpio = VGA_BASE_PIN + pin;
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_OUT);
        pio_gpio_init(vga_dual_pio, gpio);
    }
    pio_sm_set_consecutive_pindirs(vga_dual_pio, vga_dual_sm,
                                   VGA_BASE_PIN, 8, true);

    pio_sm_config sm_config = pio_get_default_sm_config();
    sm_config_set_wrap(&sm_config, program_offset, program_offset);
    sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&sm_config, true, true, 32);
    sm_config_set_out_pins(&sm_config, VGA_BASE_PIN, 8);
    sm_config_set_clkdiv(&sm_config,
            (float)clock_get_hz(clk_sys) / VGA_DUAL_PIXEL_CLOCK_HZ);
    pio_sm_init(vga_dual_pio, vga_dual_sm, program_offset, &sm_config);

    vga_dual_dma_data = dma_claim_unused_channel(true);
    vga_dual_dma_control = dma_claim_unused_channel(true);

    dma_channel_config data_config =
            dma_channel_get_default_config(vga_dual_dma_data);
    channel_config_set_transfer_data_size(&data_config, DMA_SIZE_32);
    channel_config_set_read_increment(&data_config, true);
    channel_config_set_write_increment(&data_config, false);
    channel_config_set_dreq(&data_config,
                            pio_get_dreq(vga_dual_pio, vga_dual_sm, true));
    channel_config_set_chain_to(&data_config, vga_dual_dma_control);
    dma_channel_configure(
            vga_dual_dma_data,
            &data_config,
            &vga_dual_pio->txf[vga_dual_sm],
            vga_dual_line_buffer(VGA_DUAL_ACTIVE_BUFFER_0),
            VGA_DUAL_LINE_WORDS,
            false);

    dma_channel_config control_config =
            dma_channel_get_default_config(vga_dual_dma_control);
    channel_config_set_transfer_data_size(&control_config, DMA_SIZE_32);
    channel_config_set_read_increment(&control_config, false);
    channel_config_set_write_increment(&control_config, false);
    channel_config_set_chain_to(&control_config, vga_dual_dma_data);
    dma_channel_configure(
            vga_dual_dma_control,
            &control_config,
            &dma_hw->ch[vga_dual_dma_data].read_addr,
            (const void *)&vga_dual_next_line_address,
            1,
            false);

    irq_set_exclusive_handler(DMA_IRQ_1, vga_dual_dma_handler);
    dma_channel_set_irq1_enabled(vga_dual_dma_control, true);
    irq_set_enabled(DMA_IRQ_1, true);
}

uint32_t vga_start_mask(void) {
    return 1u << vga_dual_dma_data;
}

void vga_enable(void) {
    pio_sm_set_enabled(vga_dual_pio, vga_dual_sm, true);
}

void vga_set_framebuffer(const uint8_t *framebuffer) {
    vga_dual_active_framebuffer = framebuffer;
}

void vga_request_framebuffer(const uint8_t *framebuffer) {
    __mem_fence_release();
    vga_dual_pending_framebuffer = framebuffer;
}

void vga_wait_framebuffer(void) {
    while (vga_dual_pending_framebuffer != NULL) {
        tight_loop_contents();
    }
    __mem_fence_acquire();
}
