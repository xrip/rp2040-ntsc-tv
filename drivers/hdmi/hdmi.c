#pragma GCC optimize("O3")
#include <stdalign.h>
#include <stddef.h>
#include <string.h>

#include <hardware/dma.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>

#include "graphics.h"
#include "graphics_modes.h"
#include "hdmi_codec.h"

enum {
    HDMI_FRAME_WIDTH = 320,
    HDMI_FRAME_HEIGHT = 240,
    HDMI_ACTIVE_SCANLINES = 480,
    HDMI_TOTAL_SCANLINES = 525,
    HDMI_VSYNC_FIRST_LINE = 490,
    HDMI_VSYNC_LINES = 2,
    HDMI_TEXT_HSYNC_BYTES = 96,
    HDMI_GRAPHICS_HSYNC_BYTES = 48,
    HDMI_TEXT_ACTIVE_OFFSET = 144,
    HDMI_GRAPHICS_ACTIVE_OFFSET = 72,
    HDMI_CTRL_BASE_INDEX = 252,
    HDMI_DEMO_REMAP_BASE_INDEX = 48
};

// Exactly the old driver's 4,896-byte allocation. The palette, lines and
// cached graphics colours are overlaid only while all HDMI DMA is stopped.
// No text VRAM, font, 640x480 framebuffer, or attribute LUT is allocated here.
static alignas(4096) union hdmi_workspace workspace;
_Static_assert(sizeof(workspace) == HDMI_WORKSPACE_BYTES, "HDMI RAM budget changed");
_Static_assert(sizeof(workspace.text) <= HDMI_WORKSPACE_BYTES, "Text exceeds existing workspace");
_Static_assert(offsetof(union hdmi_workspace, graphics.lines) % 4u == 0u &&
               offsetof(union hdmi_workspace, text.lines) % 4u == 0u &&
               sizeof(workspace.graphics.lines[0]) % 4u == 0u &&
               sizeof(workspace.text.lines[0]) % 4u == 0u,
               "HDMI line helpers require aligned, whole-word buffers");
_Static_assert(HDMI_TEXT_HSYNC_BYTES % 4u == 0u &&
               HDMI_GRAPHICS_HSYNC_BYTES % 4u == 0u &&
               HDMI_TEXT_ACTIVE_OFFSET % 4u == 0u &&
               HDMI_GRAPHICS_ACTIVE_OFFSET % 4u == 0u &&
               HDMI_FRAME_WIDTH % 4u == 0u,
               "HDMI line fill regions must span aligned, whole words");

#ifndef GRAPHICS_NO_BUILTIN_FRAMEBUFFER
static uint8_t hdmi_primary_framebuffer[HDMI_FRAME_WIDTH * HDMI_FRAME_HEIGHT]
        __attribute__((aligned(4)));
#endif
static const uint8_t *graphics_framebuffer;
static const uint8_t *volatile pending_framebuffer;
static volatile uint16_t hdmi_current_scanline;
static bool initialized;
static bool palette_initialized;
static bool text_mode;
static spin_lock_t *hdmi_lock;

static uint pio_program_offset_video;
static uint pio_program_offset_converter;
static int sm_video_output;
static int sm_address_converter;
static int dma_channel_control;
static int dma_channel_data;
static int dma_channel_palette_control;
static int dma_channel_palette_data;

// The control channel advances through this ring itself. The IRQ fills the
// next buffer, never the buffer whose pointer has just been consumed by DMA.
static alignas(8) uint32_t dma_buffer_addresses[2];

static inline uint32_t hdmi_enter(void) {
    return hdmi_lock != NULL ? spin_lock_blocking(hdmi_lock) : 0;
}

static inline void hdmi_leave(uint32_t state) {
    if (hdmi_lock != NULL) spin_unlock(hdmi_lock, state);
}

static const uint16_t pio_instructions_address_converter[] = {
    0x80a0, // pull block
    0x40e8, // in osr, 8
    0x4034, // in x, 20 (patched to 21 for 8-byte, single-pixel text entries)
    0x8020, // push block
};
static const struct pio_program pio_program_address_converter = {
    .instructions = pio_instructions_address_converter,
    .length = 4,
    .origin = -1,
};

static const uint16_t pio_instructions_hdmi_output[] = {
    0x7006, 0x7006, 0x7006, 0x7006, 0x7006,
    0x6806, 0x6806, 0x6806, 0x6806, 0x6806,
};
static const struct pio_program pio_program_hdmi_output = {
    .instructions = pio_instructions_hdmi_output,
    .length = 10,
    .origin = -1,
};

static inline uint8_t hdmi_color_code(uint8_t index) {
    return index < HDMI_CTRL_BASE_INDEX ? index :
            (uint8_t)(HDMI_DEMO_REMAP_BASE_INDEX + index - HDMI_CTRL_BASE_INDEX);
}

static void hdmi_initialize_palette(void) {
    if (palette_initialized) return;
    for (unsigned i = 0; i < HDMI_CTRL_BASE_INDEX; ++i)
        hdmi_encode_graphics_rgb(workspace.graphics.palette[i], 0);
    palette_initialized = true;
}

static void hdmi_write_controls(void) {
    static const uint16_t symbols[4] = {0x354, 0x0ab, 0x154, 0x2ab};
    for (unsigned i = 0; i < 4; ++i) {
        // Negative H/V polarity: blank uses CTL=3; H pulse uses CTL=2.
        const uint64_t packed = hdmi_pack_symbols(symbols[0], symbols[0], symbols[3 - i]);
        if (text_mode) {
            hdmi_store_pixel(workspace.text.palette[HDMI_CTRL_BASE_INDEX + i], packed);
        } else {
            uint32_t *entry = workspace.graphics.palette[HDMI_CTRL_BASE_INDEX + i];
            hdmi_store_pixel(entry, packed);
            hdmi_store_pixel(entry + 2, packed);
        }
    }
}

static void hdmi_change_workspace_mode(bool want_text) {
    hdmi_initialize_palette();
    if (want_text && !text_mode) {
        hdmi_cache_graphics_palette(&workspace);
        for (unsigned color = 0; color < 16; ++color) {
            for (unsigned phase = 0; phase < 2; ++phase)
                hdmi_store_pixel(workspace.text.palette[color + phase * 16],
                                  hdmi_text_pixel(GRAPHICS_CGA_RGB(color), phase));
        }
    } else if (!want_text && text_mode) {
        hdmi_restore_graphics_palette(&workspace);
    }
    text_mode = want_text;
    hdmi_write_controls();
}

// Keep all scanline memory operations in the SRAM-resident caller. In
// particular, do not let GCC turn these loops back into XIP libc calls.
// Every caller supplies a four-byte aligned range and a multiple of four
// bytes. may_alias permits word accesses to the uint8_t line arrays.
typedef uint32_t hdmi_line_word_t __attribute__((may_alias));

static __force_inline void hdmi_fill_line(uint8_t *destination, uint8_t value,
                                         unsigned bytes) {
    volatile hdmi_line_word_t *output = (volatile hdmi_line_word_t *)destination;
    const uint32_t word = (uint32_t)value * UINT32_C(0x01010101);
#pragma GCC unroll 1
    for (unsigned i = 0; i < bytes / 4u; ++i) output[i] = word;
}

static __force_inline void hdmi_copy_line(uint8_t *destination,
                                         const uint8_t *source, unsigned bytes) {
    volatile hdmi_line_word_t *output = (volatile hdmi_line_word_t *)destination;
    const hdmi_line_word_t *input = (const hdmi_line_word_t *)source;
#pragma GCC unroll 1
    for (unsigned i = 0; i < bytes / 4u; ++i) output[i] = input[i];
}

static __force_inline void hdmi_render_text(unsigned line, uint8_t *output) {
    struct graphics_text_line text;
    volatile hdmi_line_word_t *words = (volatile hdmi_line_word_t *)output;
    if (!graphics_text_line(line, HDMI_ACTIVE_SCANLINES, &text)) {
#pragma GCC unroll 1
        for (unsigned i = 0; i < 160; ++i) *words++ = UINT32_C(0x10001000);
        return;
    }
    const bool wide = text.columns == 40;
    const bool msb_first = text.msb_first;
    // Keep the shared glyph API unchanged: this private snapshot asks for
    // raw font bit order, which the packed emitters handle directly.
    text.msb_first = false;
    for (unsigned column = 0; column < text.columns; ++column) {
        const uint16_t cell = graphics_text_cell(&text, column);
        const uint8_t character = (uint8_t)cell;
        const uint8_t attribute = (uint8_t)(cell >> 8);
        const uint8_t fg = attribute & 15u;
        const uint8_t bg = (attribute >> 4) & (text.blink_enabled ? 7u : 15u);
        uint8_t glyph = graphics_text_glyph(&text, column, character, attribute);
        const uint32_t background = (uint32_t)bg * UINT32_C(0x01010101);
        const uint32_t difference = fg ^ bg;
        if (wide) {
            if (msb_first) {
#pragma GCC unroll 1
                for (unsigned pair = 0; pair < 4; ++pair) {
                    *words++ = hdmi_text_two_doubled_pixels_reversed(glyph, background, difference);
                    glyph <<= 2;
                }
            } else {
#pragma GCC unroll 1
                for (unsigned pair = 0; pair < 4; ++pair) {
                    *words++ = hdmi_text_two_doubled_pixels(glyph, background, difference);
                    glyph >>= 2;
                }
            }
        } else if (msb_first) {
            *words++ = hdmi_text_four_pixels_reversed(glyph >> 4, background, difference);
            *words++ = hdmi_text_four_pixels_reversed(glyph, background, difference);
        } else {
            *words++ = hdmi_text_four_pixels(glyph, background, difference);
            *words++ = hdmi_text_four_pixels(glyph >> 4, background, difference);
        }
    }
}

static void __time_critical_func(hdmi_prepare_line)(unsigned line, unsigned buffer) {
    const bool vsync = line >= HDMI_VSYNC_FIRST_LINE &&
                       line < HDMI_VSYNC_FIRST_LINE + HDMI_VSYNC_LINES;
    const unsigned hsync = text_mode ? HDMI_TEXT_HSYNC_BYTES : HDMI_GRAPHICS_HSYNC_BYTES;
    const unsigned total = text_mode ? sizeof(workspace.text.lines[0]) :
                                       sizeof(workspace.graphics.lines[0]);
    uint8_t *dest = text_mode ? workspace.text.lines[buffer] : workspace.graphics.lines[buffer];
    if (!text_mode && line < HDMI_ACTIVE_SCANLINES && (line & 1u)) {
        // Repeat the exact already-built source row. Besides avoiding a
        // second remap pass, this keeps a graphics pixel doubled vertically
        // even when the emulator changes VRAM between the two scanlines.
        hdmi_copy_line(dest, workspace.graphics.lines[buffer ^ 1u], 400);
        return;
    }
    const uint8_t blank = HDMI_CTRL_BASE_INDEX + (vsync ? 2 : 0);
    hdmi_fill_line(dest, blank + 1u, hsync);
    if (line >= HDMI_ACTIVE_SCANLINES) {
        hdmi_fill_line(dest + hsync, blank, total - hsync);
        return;
    }
    // The renderer writes every active pixel. Fill just the porches instead
    // of writing the entire line twice (160 instead of 800 bytes in text).
    const unsigned active_offset = text_mode ? HDMI_TEXT_ACTIVE_OFFSET :
                                              HDMI_GRAPHICS_ACTIVE_OFFSET;
    const unsigned active_bytes = text_mode ? HDMI_FRAME_WIDTH * 2u : HDMI_FRAME_WIDTH;
    hdmi_fill_line(dest + hsync, blank, active_offset - hsync);
    hdmi_fill_line(dest + active_offset + active_bytes, blank,
                   total - active_offset - active_bytes);
    if (text_mode) {
        hdmi_render_text(line, dest + HDMI_TEXT_ACTIVE_OFFSET);
    } else {
        uint8_t *output = dest + HDMI_GRAPHICS_ACTIVE_OFFSET;
        if (graphics_framebuffer == NULL) {
            hdmi_fill_line(output, 0, HDMI_FRAME_WIDTH);
            return;
        }
        const uint8_t *source = graphics_framebuffer + (line >> 1u) * HDMI_FRAME_WIDTH;
        for (unsigned x = 0; x < HDMI_FRAME_WIDTH; ++x)
            output[x] = hdmi_color_code(source[x]);
    }
}

static void __time_critical_func(hdmi_scanline_interrupt_handler)(void) {
    // NVIC pending state is per-core. A reconfiguration requested by the
    // other core must not turn a stale pending IRQ into a fictitious line.
    const uint32_t irq_state = hdmi_enter();
    if (!(dma_hw->ints0 & (1u << dma_channel_control))) {
        hdmi_leave(irq_state);
        return;
    }
    dma_hw->ints0 = 1u << dma_channel_control;
    unsigned line = hdmi_current_scanline + 1u;
    if (line == HDMI_TOTAL_SCANLINES) line = 0;
    hdmi_current_scanline = (uint16_t)line;
    unsigned next_line = line + 1u;
    if (next_line == HDMI_TOTAL_SCANLINES) next_line = 0;
    if (next_line == 0) {
        graphics_text_frame_tick();
        if (pending_framebuffer != NULL) {
            __mem_fence_acquire();
            graphics_framebuffer = pending_framebuffer;
            __mem_fence_release();
            pending_framebuffer = NULL;
        }
    }
    // Control DMA has already advanced its ring pointer to the free buffer.
    const unsigned next_buffer = dma_hw->ch[dma_channel_control].read_addr ==
            (uintptr_t)&dma_buffer_addresses[0] ? 0u : 1u;
    hdmi_prepare_line(next_line, next_buffer);
    hdmi_leave(irq_state);
}

static void hdmi_stop_output(void) {
    // Caller holds hdmi_lock, which also drains an IRQ already executing on
    // the scanout core. Never enable/disable another core's NVIC here.
    dma_channel_set_irq0_enabled(dma_channel_control, false);
    // Disable chaining first so an in-flight completion cannot restart a
    // channel after it was aborted. IRQs are disabled throughout the overlay.
    const unsigned channels[4] = {dma_channel_data, dma_channel_control,
                                 dma_channel_palette_data, dma_channel_palette_control};
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned channel = channels[i];
        hw_write_masked(&dma_hw->ch[channel].ctrl_trig,
                         channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB,
                         DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
    }
    dma_hw->abort = (1u << dma_channel_control) | (1u << dma_channel_data) |
                    (1u << dma_channel_palette_control) | (1u << dma_channel_palette_data);
    while (dma_hw->abort) tight_loop_contents();
    pio_sm_set_enabled(PIO_VIDEO, sm_video_output, false);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, sm_address_converter, false);
    dma_channel_acknowledge_irq0(dma_channel_control);
}

static void hdmi_start_output(void) {
    const unsigned stride_shift = text_mode ? 11 : 12;
    PIO_VIDEO_ADDR->instr_mem[pio_program_offset_converter + 2] =
            pio_encode_in(pio_x, text_mode ? 21 : 20);
    pio_sm_config config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, pio_program_offset_converter, pio_program_offset_converter + 3);
    sm_config_set_in_shift(&config, true, false, 32);
    pio_sm_init(PIO_VIDEO_ADDR, sm_address_converter, pio_program_offset_converter, &config);
    pio_sm_put_blocking(PIO_VIDEO_ADDR, sm_address_converter,
                         (uint32_t)(uintptr_t)&workspace >> stride_shift);
    pio_sm_exec(PIO_VIDEO_ADDR, sm_address_converter, pio_encode_pull(false, true));
    pio_sm_exec(PIO_VIDEO_ADDR, sm_address_converter, pio_encode_mov(pio_x, pio_osr));

    config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, pio_program_offset_video, pio_program_offset_video + 9);
    sm_config_set_sideset_pins(&config, HDMI_PIN_CLOCK);
    sm_config_set_sideset(&config, 2, false, false);
    sm_config_set_out_pins(&config, HDMI_PIN_DATA, 6);
    sm_config_set_out_shift(&config, true, true, 30);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, clock_get_hz(clk_sys) / 252000000.0f);
    pio_sm_init(PIO_VIDEO, sm_video_output, pio_program_offset_video, &config);

    // Begin with a complete vertical blank interval. Both buffers are valid
    // before the first DMA trigger; the first IRQ describes line 480.
    hdmi_current_scanline = HDMI_ACTIVE_SCANLINES - 1;
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *line = text_mode ? workspace.text.lines[i] : workspace.graphics.lines[i];
        dma_buffer_addresses[i] = (uint32_t)(uintptr_t)line;
        hdmi_prepare_line(HDMI_ACTIVE_SCANLINES + i, i);
    }

    dma_channel_config dma_config = dma_channel_get_default_config(dma_channel_data);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_8);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_chain_to(&dma_config, dma_channel_control);
    channel_config_set_dreq(&dma_config, pio_get_dreq(PIO_VIDEO_ADDR, sm_address_converter, true));
    channel_config_set_high_priority(&dma_config, true);
    dma_channel_configure(dma_channel_data, &dma_config,
                          &PIO_VIDEO_ADDR->txf[sm_address_converter],
                          (const void *)(uintptr_t)dma_buffer_addresses[0],
                          text_mode ? 800 : 400, false);

    dma_config = dma_channel_get_default_config(dma_channel_control);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_ring(&dma_config, false, 3);
    channel_config_set_chain_to(&dma_config, dma_channel_data);
    channel_config_set_high_priority(&dma_config, true);
    dma_channel_configure(dma_channel_control, &dma_config,
                          &dma_hw->ch[dma_channel_data].read_addr,
                          dma_buffer_addresses, 1, false);

    dma_config = dma_channel_get_default_config(dma_channel_palette_data);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_chain_to(&dma_config, dma_channel_palette_control);
    channel_config_set_dreq(&dma_config, pio_get_dreq(PIO_VIDEO, sm_video_output, true));
    channel_config_set_high_priority(&dma_config, true);
    dma_channel_configure(dma_channel_palette_data, &dma_config,
                          &PIO_VIDEO->txf[sm_video_output], &workspace,
                          text_mode ? 2 : 4, false);

    dma_config = dma_channel_get_default_config(dma_channel_palette_control);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_chain_to(&dma_config, dma_channel_palette_data);
    channel_config_set_dreq(&dma_config, pio_get_dreq(PIO_VIDEO_ADDR, sm_address_converter, false));
    channel_config_set_high_priority(&dma_config, true);
    dma_channel_configure(dma_channel_palette_control, &dma_config,
                          &dma_hw->ch[dma_channel_palette_data].read_addr,
                          &PIO_VIDEO_ADDR->rxf[sm_address_converter], 1, false);

    dma_channel_acknowledge_irq0(dma_channel_control);
    dma_channel_set_irq0_enabled(dma_channel_control, true);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, sm_address_converter, true);
    dma_start_channel_mask((1u << dma_channel_palette_control) | (1u << dma_channel_control));
    // Pre-fill the serializer FIFO before enabling its continuously clocked SM.
    while (!pio_sm_is_tx_fifo_full(PIO_VIDEO, sm_video_output)) tight_loop_contents();
    pio_sm_set_enabled(PIO_VIDEO, sm_video_output, true);
}

void graphics_set_mode(enum graphics_mode_t mode) {
    if (initialized) {
        // Geometry changes also happen outside the active image. A class
        // change then restarts the serial pipeline (a sink may briefly relock).
        while (hdmi_current_scanline < HDMI_ACTIVE_SCANLINES) tight_loop_contents();
    }
    graphics_source_set_mode(mode);
    const bool want_text = graphics_source_is_text();
    const uint32_t irq_state = hdmi_enter();
    if (initialized && want_text != text_mode) {
        hdmi_stop_output();
        hdmi_change_workspace_mode(want_text);
        hdmi_start_output();
    }
    hdmi_leave(irq_state);
}

void graphics_wait_vblank(void) {
    if (!initialized) return;
    uint16_t previous = hdmi_current_scanline;
    while (true) {
        const uint16_t line = hdmi_current_scanline;
        if (line < previous) return;
        previous = line;
        tight_loop_contents();
    }
}

void graphics_set_palette(uint8_t index, uint32_t color888) {
    const uint8_t code = hdmi_color_code(index);
    uint32_t encoded[4];
    // Encoding does not hold the IRQ lock. Only the short final publication
    // is serialized against scanout and in-place workspace repacking.
    hdmi_encode_graphics_rgb(encoded, color888);
    const uint32_t irq_state = hdmi_enter();
    hdmi_initialize_palette();
    if (text_mode) {
        uint8_t *cached = workspace.text.graphics_rgb[code];
        cached[0] = (uint8_t)(color888 >> 16);
        cached[1] = (uint8_t)(color888 >> 8);
        cached[2] = (uint8_t)color888;
    } else {
        memcpy(workspace.graphics.palette[code], encoded, sizeof(encoded));
    }
    hdmi_leave(irq_state);
}

void graphics_set_buffer(uint8_t *buffer, uint16_t width, uint16_t height) {
    const uint32_t irq_state = hdmi_enter();
    if (width == HDMI_FRAME_WIDTH && height == HDMI_FRAME_HEIGHT)
        graphics_framebuffer = buffer;
    hdmi_leave(irq_state);
}

void hdmi_graphics_init(void) {
    if (initialized) return;
    hdmi_lock = spin_lock_init(spin_lock_claim_unused(true));
#ifndef GRAPHICS_NO_BUILTIN_FRAMEBUFFER
    if (graphics_framebuffer == NULL) graphics_framebuffer = hdmi_primary_framebuffer;
#endif
    pending_framebuffer = NULL;
    hdmi_change_workspace_mode(graphics_source_is_text());
    sm_video_output = pio_claim_unused_sm(PIO_VIDEO, true);
    sm_address_converter = pio_claim_unused_sm(PIO_VIDEO_ADDR, true);
    dma_channel_control = dma_claim_unused_channel(true);
    dma_channel_data = dma_claim_unused_channel(true);
    dma_channel_palette_control = dma_claim_unused_channel(true);
    dma_channel_palette_data = dma_claim_unused_channel(true);
    pio_program_offset_converter = pio_add_program(PIO_VIDEO_ADDR, &pio_program_address_converter);
    pio_program_offset_video = pio_add_program(PIO_VIDEO, &pio_program_hdmi_output);

    for (unsigned i = 0; i < 2; ++i) {
        pio_gpio_init(PIO_VIDEO, HDMI_PIN_CLOCK + i);
        gpio_set_drive_strength(HDMI_PIN_CLOCK + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(HDMI_PIN_CLOCK + i, GPIO_SLEW_RATE_FAST);
    }
    for (unsigned i = 0; i < 6; ++i) {
        pio_gpio_init(PIO_VIDEO, HDMI_PIN_DATA + i);
        gpio_set_drive_strength(HDMI_PIN_DATA + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(HDMI_PIN_DATA + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_pins_with_mask(PIO_VIDEO, sm_video_output,
                             3u << HDMI_PIN_CLOCK, 3u << HDMI_PIN_CLOCK);
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, sm_video_output, HDMI_PIN_CLOCK, 2, true);
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, sm_video_output, HDMI_PIN_DATA, 6, true);
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, hdmi_scanline_interrupt_handler);
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    irq_clear(VIDEO_DMA_IRQ);
    irq_set_enabled(VIDEO_DMA_IRQ, true);
    initialized = true;
    hdmi_start_output();
}

uint8_t *graphics_get_framebuffer(void) {
#ifndef GRAPHICS_NO_BUILTIN_FRAMEBUFFER
    if (graphics_framebuffer == NULL) return hdmi_primary_framebuffer;
#endif
    return (uint8_t *)graphics_framebuffer;
}

void graphics_present_framebuffer(const uint8_t *framebuffer) {
    if (!initialized) {
        graphics_framebuffer = framebuffer;
        return;
    }
    __mem_fence_release();
    pending_framebuffer = framebuffer;
    while (pending_framebuffer != NULL) tight_loop_contents();
    __mem_fence_acquire();
}

void graphics_set_bgcolor(uint32_t color888) {
    (void)color888;
}

void graphics_set_offset(int x, int y) {
    (void)x;
    (void)y;
}
