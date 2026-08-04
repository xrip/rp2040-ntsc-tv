/*----------------------------------------------------------------------------
Copyright (C) 2026, xrip, all right reserved.
Copyright (C) 2024, KenKen, all right reserved.

This program supplied herewith by KenKen is free software; you can
redistribute it and/or modify it under the terms of the same license written
here and only for non-commercial purpose.

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of FITNESS FOR A PARTICULAR
PURPOSE. The copyright owner and contributors are NOT LIABLE for any damages
caused by using this program.

----------------------------------------------------------------------------*/

// This signal generation program (using PWM and DMA) is the idea of @lovyan03.
// https://github.com/lovyan03/

#ifndef RP2040_PWM_NTSC_H
#define RP2040_PWM_NTSC_H

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("O3")
#endif

#include <hardware/dma.h>
#include <hardware/pwm.h>
#include <hardware/sync.h>
#include <hardware/vreg.h>

/* ===========================================================================
 * NTSC Video Format Constants
 * =========================================================================== */

// Frame dimensions
#define NTSC_FRAME_WIDTH    320
#define NTSC_FRAME_HEIGHT   240

// NTSC timing parameters
#define NTSC_SAMPLES_PER_LINE  908   // 227 * 4 samples per scanline
#define NTSC_TOTAL_LINES       262   // Total scanlines in NTSC frame
#define NTSC_VSYNC_LINES       10    // Vertical sync pulse lines
#define NTSC_VBLANK_TOP        12    // Top blanking interval lines
#define NTSC_HSYNC_WIDTH       68    // Horizontal sync width in samples (~4.7μs)
#define NTSC_BACK_PORCH_SAMPLES 8
#define NTSC_COLOR_BURST_CYCLES 9
#define NTSC_ACTIVE_START      (NTSC_HSYNC_WIDTH + NTSC_BACK_PORCH_SAMPLES + \
                                NTSC_COLOR_BURST_CYCLES * 4 + 60)

// NTSC composite video signal levels (0-7 range for 3-bit PWM)
#define NTSC_LEVEL_SYNC          0    // Sync pulse level (lowest)
#define NTSC_LEVEL_BLANK         2    // Blanking/black level
#define NTSC_LEVEL_BLACK         2    // Black level (same as blanking)
#define NTSC_LEVEL_BURST_LOW     1    // Color burst low level
#define NTSC_LEVEL_BURST_HIGH    3    // Color burst high level

/* ===========================================================================
 * Hardware Pin Configuration
 * =========================================================================== */

// Pin for NTSC composite video signal output via PWM
#ifndef NTSC_PIN_OUTPUT
#define NTSC_PIN_OUTPUT 27
#endif

// Graphics framebuffer - stores raw pixel data for the display
// Aligned to the 4-byte boundary for efficient DMA transfers
static uint8_t ntsc_framebuffer[NTSC_FRAME_WIDTH * NTSC_FRAME_HEIGHT] __attribute__ ((aligned (4)));

// Scanout reads only the active buffer. A producer may prepare another
// framebuffer and request an atomic swap at the start of the next frame.
static const uint8_t *ntsc_active_framebuffer = ntsc_framebuffer;
static const uint8_t *volatile ntsc_pending_framebuffer = nullptr;

// Ping-pong buffers for DMA double-buffering
// While one buffer is being transmitted, the other is prepared
// Size aligned to the 4-byte boundary for DMA efficiency
static uint16_t ntsc_scanline_buffers[2][NTSC_SAMPLES_PER_LINE + 3 & ~3u] __attribute__ ((aligned (4)));

// NTSC color palette lookup table
// Each color has 4 entries for the 4 phases of NTSC color subcarrier (0°, 90°, 180°, 270°)
// This allows proper color encoding at 3.579545 MHz
#if defined(NTSC_USE_SCRATCH_Y) && NTSC_USE_SCRATCH_Y
#define NTSC_PALETTE_PLACEMENT(name) __scratch_y(name)
#else
#define NTSC_PALETTE_PLACEMENT(name)
#endif

#if defined(NTSC_LOW_RAM) && NTSC_LOW_RAM
// Compact palette: two 4-bit phase values packed per byte (512 bytes total)
// PWM compare values 0..11 fit in a nibble; phases 0°,90° in _even, 180°,270° in _odd
static uint8_t NTSC_PALETTE_PLACEMENT("ntsc_palette_even") ntsc_palette_even[256];
static uint8_t NTSC_PALETTE_PLACEMENT("ntsc_palette_odd") ntsc_palette_odd[256];
#else
// Packed palette: two 16-bit phase values per uint32_t (2048 bytes total)
// phases 0°,90° in _even, 180°,270° in _odd
static uint32_t NTSC_PALETTE_PLACEMENT("ntsc_palette_even") ntsc_palette_even[256] __attribute__ ((aligned (4)));
static uint32_t NTSC_PALETTE_PLACEMENT("ntsc_palette_odd") ntsc_palette_odd[256]  __attribute__ ((aligned (4)));
#endif

#undef NTSC_PALETTE_PLACEMENT

static inline void ntsc_present_framebuffer(const uint8_t *framebuffer) {
    // Publish all completed pixel writes before the IRQ sees the pointer.
    __mem_fence_release();
    ntsc_pending_framebuffer = framebuffer;

    // The IRQ clears this only after switching buffers at a frame boundary.
    while (ntsc_pending_framebuffer != nullptr) {
        tight_loop_contents();
    }
    __mem_fence_acquire();
}

// The data channel sends a scanline. The control channel selects the next buffer.
static uint8_t ntsc_dma_chan_data;
static uintptr_t ntsc_dma_read_addresses[2] __attribute__ ((aligned (8)));

#if defined(NTSC_LOW_RAM) && NTSC_LOW_RAM
// Unpack a byte holding two 4-bit phase values into a 32-bit value
// suitable for a single 32-bit write to the scanline buffer
static inline uint32_t ntsc_unpack_pair(uint32_t pair) {
    return (pair * 0x1001u) & 0x000f000fu;
}
#endif

/* ===========================================================================
 * Function: ntsc_generate_scanline
 * Purpose: Generate NTSC composite video signal data for one scanline
 * =========================================================================== */
static inline void ntsc_generate_scanline(uint16_t *output_buffer, const size_t scanline_number) {
    uint16_t *buffer_ptr = output_buffer;
    const size_t active_line = scanline_number - (NTSC_VSYNC_LINES + NTSC_VBLANK_TOP);

    // Generate active video scanlines
    if (active_line < NTSC_FRAME_HEIGHT) {
        // Skip horizontal blanking interval
        buffer_ptr += NTSC_ACTIVE_START;

        const uint8_t *pixel_ptr =
                ntsc_active_framebuffer + active_line * NTSC_FRAME_WIDTH;
        uint32_t *output_ptr = (uint32_t *)buffer_ptr;
#if defined(NTSC_LOW_RAM) && NTSC_LOW_RAM && PICO_RP2040
        uint32_t pixel_groups = NTSC_FRAME_WIDTH / 2;
#else
        uint32_t pixel_groups = NTSC_FRAME_WIDTH / 4;
#endif

        // Process all pixels in the scanline
#if defined(NTSC_LOW_RAM) && NTSC_LOW_RAM
        // Compact palette: unpack four pixels per iteration.
        do {
#if PICO_RP2350
            uint32_t source_pixels;
            __builtin_memcpy(&source_pixels, pixel_ptr, sizeof(source_pixels));
            output_ptr[0] = ntsc_unpack_pair(ntsc_palette_even[(uint8_t)source_pixels]);
            output_ptr[1] = ntsc_unpack_pair(ntsc_palette_odd[(uint8_t)(source_pixels >> 8)]);
            output_ptr[2] = ntsc_unpack_pair(ntsc_palette_even[(uint8_t)(source_pixels >> 16)]);
            output_ptr[3] = ntsc_unpack_pair(ntsc_palette_odd[source_pixels >> 24]);
            pixel_ptr += 4;
            output_ptr += 4;
#else
            output_ptr[0] = ntsc_unpack_pair(ntsc_palette_even[pixel_ptr[0]]);
            output_ptr[1] = ntsc_unpack_pair(ntsc_palette_odd[pixel_ptr[1]]);
            pixel_ptr += 2;
            output_ptr += 2;
#endif
        } while (--pixel_groups);
#else
        // Packed palette: process four pixels per iteration.
        do {
#if PICO_RP2350
            uint32_t source_pixels;
            __builtin_memcpy(&source_pixels, pixel_ptr, sizeof(source_pixels));

            const uint32_t pixel0 = ntsc_palette_even[(uint8_t)source_pixels];
            const uint32_t pixel1 = ntsc_palette_odd[(uint8_t)(source_pixels >> 8)];
            output_ptr[0] = pixel0;
            output_ptr[1] = pixel1;

            const uint32_t pixel2 = ntsc_palette_even[(uint8_t)(source_pixels >> 16)];
            const uint32_t pixel3 = ntsc_palette_odd[source_pixels >> 24];
            output_ptr[2] = pixel2;
            output_ptr[3] = pixel3;
#else
            output_ptr[0] = ntsc_palette_even[pixel_ptr[0]];
            output_ptr[1] = ntsc_palette_odd[pixel_ptr[1]];
            output_ptr[2] = ntsc_palette_even[pixel_ptr[2]];
            output_ptr[3] = ntsc_palette_odd[pixel_ptr[3]];
#endif
            pixel_ptr += 4;
            output_ptr += 4;
        } while (--pixel_groups);
#endif
    }
    // Generate equalizing pulses for the first two scanlines
    else if (scanline_number < 2) {
        // Fill most of the line with sync level (black)
        for (int j = 0; j < NTSC_SAMPLES_PER_LINE - NTSC_HSYNC_WIDTH; j++)
            *buffer_ptr++ = NTSC_LEVEL_SYNC;

        // Add horizontal sync pulse at the end
        while (buffer_ptr < output_buffer + NTSC_SAMPLES_PER_LINE)
            *buffer_ptr++ = NTSC_LEVEL_BLANK;
    }
    // Generate vertical sync pulses
    else if (scanline_number == NTSC_VSYNC_LINES || scanline_number == NTSC_VSYNC_LINES + 1) {
        // Horizontal sync pulse
        for (int j = 0; j < NTSC_HSYNC_WIDTH; j++)
            *buffer_ptr++ = NTSC_LEVEL_SYNC;

        // Back porch before color burst
        for (int j = 0; j < NTSC_BACK_PORCH_SAMPLES; j++)
            *buffer_ptr++ = NTSC_LEVEL_BLANK;

        // Color burst signal at 3.579545 MHz
        // Alternates between levels to create a reference signal for color decoding
        for (int j = 0; j < NTSC_COLOR_BURST_CYCLES; j++) {
            *buffer_ptr++ = NTSC_LEVEL_BLANK;      // Phase 0°
            *buffer_ptr++ = NTSC_LEVEL_BURST_LOW;  // Phase 90°
            *buffer_ptr++ = NTSC_LEVEL_BLANK;      // Phase 180°
            *buffer_ptr++ = NTSC_LEVEL_BURST_HIGH; // Phase 270°
        }

        // Fill remainder with blanking level
        while (buffer_ptr < output_buffer + NTSC_SAMPLES_PER_LINE)
            *buffer_ptr++ = NTSC_LEVEL_BLANK;
    }
    // Generate vertical blanking lines after active video
    else if (scanline_number == NTSC_VSYNC_LINES + NTSC_VBLANK_TOP + NTSC_FRAME_HEIGHT ||
             scanline_number == NTSC_VSYNC_LINES + NTSC_VBLANK_TOP + NTSC_FRAME_HEIGHT + 1) {
        // Skip horizontal blanking interval
        buffer_ptr += NTSC_ACTIVE_START;

        // Fill the active portion with blanking level
        // Width = NTSC_FRAME_WIDTH * 2 because each pixel generates 2 samples
        for (int i = 0; i < NTSC_FRAME_WIDTH * 2; i++)
            *buffer_ptr++ = NTSC_LEVEL_BLANK;
    }
}

/* ===========================================================================
 * Function: ntsc_set_color
 * Purpose: Configure a color palette entry for NTSC encoding
 * =========================================================================== */
static void ntsc_set_color(const uint8_t palette_index, const uint8_t blue, const uint8_t red, const uint8_t green) {
    // Calculate NTSC luminance using standard weights
    // Y = 0.587*G + 0.114*B + 0.299*R
    // Using integer math: (150*G + 29*B + 77*R) / 256
    const int32_t luminance = (150 * green + 29 * blue + 77 * red + 128) / 256;

    // Pre-calculated chrominance modulation factors
    // These compensate for phase shifts in the NTSC encoding
    // Original formula: signal = Y + 0.4921*(B-Y)*sin(θ) + 0.8773*(R-Y)*cos(θ)

    // Phase 0° and 180° components
    const int32_t blue_chroma_0 = (blue - luminance) * 441; // (B-Y) * 0.4921 * scale
    const int32_t red_chroma_0 = (red - luminance) * 1361; // (R-Y) * 0.8773 * scale

    // Phase 90° and 270° components
    const int32_t blue_chroma_90 = (blue - luminance) * 764; // (B-Y) * 0.4921 * scale
    const int32_t red_chroma_90 = (red - luminance) * -786; // (R-Y) * 0.8773 * scale

    // Generate composite signal values for each subcarrier phase
    // Phase 0°: Y + chroma
    const int32_t phase0 = (luminance * 1792 + blue_chroma_0 + red_chroma_0 + 2 * 65536 + 32768) / 65536;
    // Phase 90°: Y + chroma(90°)
    const int32_t phase1 = (luminance * 1792 + blue_chroma_90 + red_chroma_90 + 2 * 65536 + 32768) / 65536;
    // Phase 180°: Y - chroma
    const int32_t phase2 = (luminance * 1792 - blue_chroma_0 - red_chroma_0 + 2 * 65536 + 32768) / 65536;
    // Phase 270°: Y - chroma(90°)
    const int32_t phase3 = (luminance * 1792 - blue_chroma_90 - red_chroma_90 + 2 * 65536 + 32768) / 65536;

#if defined(NTSC_LOW_RAM) && NTSC_LOW_RAM
    // Pack as nibble pairs. The current conversion produces values in the
    // 0..11 range; PWM compare values >= TOP + 1 naturally produce 100% duty.
    ntsc_palette_even[palette_index] =
        (uint8_t)(phase0 < 0 ? 0 : phase0) |
        ((uint8_t)(phase1 < 0 ? 0 : phase1) << 4);
    ntsc_palette_odd[palette_index] =
        (uint8_t)(phase2 < 0 ? 0 : phase2) |
        ((uint8_t)(phase3 < 0 ? 0 : phase3) << 4);
#else
    // Pack as 16-bit pairs in uint32_t tables
    ntsc_palette_even[palette_index] =
        (uint32_t)(phase0 < 0 ? 0 : phase0) | ((uint32_t)(phase1 < 0 ? 0 : phase1) << 16);
    ntsc_palette_odd[palette_index] =
        (uint32_t)(phase2 < 0 ? 0 : phase2) | ((uint32_t)(phase3 < 0 ? 0 : phase3) << 16);
#endif
}

/* ===========================================================================
 * Function: ntsc_dma_irq_handler
 * Purpose: Handle DMA transfer completion and prepare next scanline
 * =========================================================================== */
static void __time_critical_func(ntsc_dma_irq_handler)() {
    static size_t current_scanline = 2;
    dma_hw->ints0 = 1u << ntsc_dma_chan_data;

    const size_t scanline = current_scanline;
    uint16_t *output_buffer = ntsc_scanline_buffers[scanline & 1u];

    if (scanline == 0) {
        const uint8_t *pending_framebuffer = ntsc_pending_framebuffer;
        if (pending_framebuffer != nullptr) {
            __mem_fence_acquire();
            ntsc_active_framebuffer = pending_framebuffer;
            __mem_fence_release();
            ntsc_pending_framebuffer = nullptr;
        }
    }

    if (++current_scanline >= NTSC_TOTAL_LINES) {
        current_scanline = 0;
    }

    ntsc_generate_scanline(output_buffer, scanline);
}


/* ===========================================================================
 * Function: ntsc_init
 * Purpose: Initialize the complete NTSC video generation system
 * =========================================================================== */
static inline void ntsc_init() {
    /* Clock Configuration
     * 315 MHz is the PERFECT frequency for NTSC video generation!
     * NTSC color burst is exactly 315/88 MHz = 3.579545... MHz
     * 315 MHz / 22 = 315/22 MHz = 14.318181... MHz (exactly 4x color burst)
     * 14.318181 MHz / 4 = 3.579545 MHz (EXACT NTSC color burst frequency)
     * This configuration provides PERFECT NTSC timing with 0% error! */
    constexpr uint32_t system_clock_khz = 315000;
    constexpr uint32_t pwm_period_cycles = 11;

    vreg_set_voltage(VREG_VOLTAGE_1_30);
    set_sys_clock_khz(system_clock_khz, true);

    // Configure PWM output pin
    gpio_set_function(NTSC_PIN_OUTPUT, GPIO_FUNC_PWM);
    const uint pwm_slice = pwm_gpio_to_slice_num(NTSC_PIN_OUTPUT);

    // Configure PWM for video signal generation
    pwm_config pwm_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&pwm_cfg, 2.0f); // 2x clock division

    pwm_init(pwm_slice, &pwm_cfg, true);
    pwm_set_wrap(pwm_slice, pwm_period_cycles - 1);

    // Get PWM compare register address for DMA writes
    volatile void *pwm_compare_addr = &pwm_hw->slice[pwm_slice].cc;
    // Offset by 2 bytes to write to the upper 16 bits (channel B)
    pwm_compare_addr = (volatile void *) ((uintptr_t) pwm_compare_addr + 2);

    // Allocate one paced data channel and one one-word control channel.
    ntsc_dma_chan_data = dma_claim_unused_channel(true);
    const uint dma_chan_control = dma_claim_unused_channel(true);

    // The control channel reads these in a ring. The first completed transfer
    // changes the data source from buffer 0 to buffer 1.
    ntsc_dma_read_addresses[0] = (uintptr_t)ntsc_scanline_buffers[1];
    ntsc_dma_read_addresses[1] = (uintptr_t)ntsc_scanline_buffers[0];

    dma_channel_config data_config = dma_channel_get_default_config(ntsc_dma_chan_data);
    channel_config_set_transfer_data_size(&data_config, DMA_SIZE_16);
    channel_config_set_read_increment(&data_config, true);
    channel_config_set_write_increment(&data_config, false);
    channel_config_set_dreq(&data_config, DREQ_PWM_WRAP0 + pwm_slice);
    channel_config_set_chain_to(&data_config, dma_chan_control);

    dma_channel_configure(
        ntsc_dma_chan_data,
        &data_config,
        pwm_compare_addr, // Destination: PWM register
        ntsc_scanline_buffers[0], // Source: Buffer 0
        NTSC_SAMPLES_PER_LINE, // Transfer count
        false // Don't start yet
    );

    // RP2350 self-trigger cannot be used here: it would restart before the
    // next scanline address is loaded. This one-word transfer loads READ_ADDR,
    // then chains to the data channel. The 8-byte read ring alternates buffers.
    dma_channel_config control_config = dma_channel_get_default_config(dma_chan_control);
    channel_config_set_transfer_data_size(&control_config, DMA_SIZE_32);
    channel_config_set_read_increment(&control_config, true);
    channel_config_set_write_increment(&control_config, false);
    channel_config_set_ring(&control_config, false, 3);
    channel_config_set_dreq(&control_config, DREQ_FORCE);
    channel_config_set_chain_to(&control_config, ntsc_dma_chan_data);

    dma_channel_configure(
        dma_chan_control,
        &control_config,
        &dma_hw->ch[ntsc_dma_chan_data].read_addr,
        ntsc_dma_read_addresses,
        1,
        false // Don't start yet
    );

    // Pre-fill buffers with the first two scanlines
    ntsc_generate_scanline(ntsc_scanline_buffers[0], 0);
    ntsc_generate_scanline(ntsc_scanline_buffers[1], 1);

    // Only completed scanline transfers need an interrupt.
    dma_set_irq0_channel_mask_enabled(1u << ntsc_dma_chan_data, true);

    // Install and enable interrupt handler
    irq_set_exclusive_handler(DMA_IRQ_0, ntsc_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Start video generation by triggering the first DMA transfer
    dma_start_channel_mask(1u << ntsc_dma_chan_data);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

#endif // RP2040_PWM_NTSC_H
