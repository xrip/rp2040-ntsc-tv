#include "vga.h"

#include <string.h>

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
    VGA_DUAL_PIXEL_CLOCK_HZ = 25175000
};

static constexpr uint16_t vga_dual_program_instructions[] = {
        0x6008 // out pins, 8
};

static const struct pio_program vga_dual_program = {
        .instructions = vga_dual_program_instructions,
        .length = 1,
        .origin = -1
};

static uint16_t vga_dual_palette[256] __attribute__((aligned(4)));
static uint32_t vga_dual_scanline_buffers[4][VGA_DUAL_LINE_WORDS]
        __attribute__((aligned(16)));

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

static void vga_dual_generate_active_line(const size_t source_line) {
    const uint8_t *source =
            vga_dual_active_framebuffer + source_line * VGA_DUAL_SOURCE_WIDTH;
    uint16_t *output = (uint16_t *)((uint8_t *)vga_dual_line_buffer(
            VGA_DUAL_ACTIVE_BUFFER_0 + (source_line & 1u)) +
            VGA_DUAL_ACTIVE_OFFSET);
    uint32_t groups = VGA_DUAL_SOURCE_WIDTH / 4u;

    do {
#if PICO_RP2350
        uint32_t pixels;
        __builtin_memcpy(&pixels, source, sizeof(pixels));
        output[0] = vga_dual_palette[(uint8_t)pixels];
        output[1] = vga_dual_palette[(uint8_t)(pixels >> 8)];
        output[2] = vga_dual_palette[(uint8_t)(pixels >> 16)];
        output[3] = vga_dual_palette[(uint8_t)(pixels >> 24)];
#else
        output[0] = vga_dual_palette[source[0]];
        output[1] = vga_dual_palette[source[1]];
        output[2] = vga_dual_palette[source[2]];
        output[3] = vga_dual_palette[source[3]];
#endif
        source += 4;
        output += 4;
    } while (--groups);
}

static inline uintptr_t vga_dual_prepare_line(const size_t line) {
    if (line < VGA_DUAL_ACTIVE_LINES) {
        const size_t source_line = line >> 1u;
        if ((line & 1u) == 0u) {
            const uint8_t *pending = nullptr;
            if (line == 0u) {
                pending = vga_dual_pending_framebuffer;
                if (pending != nullptr) {
                    __mem_fence_acquire();
                    vga_dual_active_framebuffer = pending;
                }
            }
            vga_dual_generate_active_line(source_line);
            if (pending != nullptr) {
                __mem_fence_release();
                vga_dual_pending_framebuffer = nullptr;
            }
        }
        return (uintptr_t)vga_dual_line_buffer(
                VGA_DUAL_ACTIVE_BUFFER_0 + (source_line & 1u));
    }

    if (line >= VGA_DUAL_VSYNC_FIRST_LINE &&
        line < VGA_DUAL_VSYNC_FIRST_LINE + VGA_DUAL_VSYNC_LINES) {
        return (uintptr_t)vga_dual_line_buffer(VGA_DUAL_VSYNC_BUFFER);
    }
    return (uintptr_t)vga_dual_line_buffer(VGA_DUAL_BLANK_BUFFER);
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
    vga_dual_next_line_address = vga_dual_prepare_line(following_line);
}

void vga_dual_set_palette(const uint8_t index, const uint32_t color888) {
    static constexpr uint8_t lower_level[8] = {
            0, 0, 1, 2, 2, 2, 3, 3
    };
    static constexpr uint8_t upper_level[8] = {
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

    vga_dual_palette[index] = (uint16_t)(first | (uint16_t)second << 8);
}

void vga_dual_init(const uint8_t *framebuffer) {
    vga_dual_active_framebuffer = framebuffer;
    vga_dual_pending_framebuffer = nullptr;
    vga_dual_make_templates();
    vga_dual_generate_active_line(0);
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

uint32_t vga_dual_start_mask(void) {
    return 1u << vga_dual_dma_data;
}

void vga_dual_enable(void) {
    pio_sm_set_enabled(vga_dual_pio, vga_dual_sm, true);
}

void vga_dual_set_framebuffer(const uint8_t *framebuffer) {
    vga_dual_active_framebuffer = framebuffer;
}

void vga_dual_request_framebuffer(const uint8_t *framebuffer) {
    __mem_fence_release();
    vga_dual_pending_framebuffer = framebuffer;
}

void vga_dual_wait_framebuffer(void) {
    while (vga_dual_pending_framebuffer != nullptr) {
        tight_loop_contents();
    }
    __mem_fence_acquire();
}
