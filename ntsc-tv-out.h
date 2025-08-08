/*----------------------------------------------------------------------------
Copyright (C) 2025, xrip, all right reserved.
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

#pragma GCC optimize ("O3")

#include <hardware/dma.h>
#include <hardware/pwm.h>
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
#define NTSC_VBLANK_TOP        26    // Top blanking interval lines
#define NTSC_HSYNC_WIDTH       68    // Horizontal sync width in samples (~4.7μs)
#define NTSC_ACTIVE_START      (NTSC_HSYNC_WIDTH + 8 + 9 * 4 + 60)  // Start of active video

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

#if !NDEBUG
// Flag indicating active video region processing
//  1: Currently generating visible scanlines
//  0: In a vertical blanking interval
static volatile uint8_t ntsc_is_rendering_active;

// Frame counter - increments after each complete frame
// Application code should reset this to track frame timing
static volatile uint16_t ntsc_frame_counter = 0;
#endif
// Ping-pong buffers for DMA double-buffering
// While one buffer is being transmitted, the other is prepared
// Size aligned to the 4-byte boundary for DMA efficiency
static uint16_t ntsc_scanline_buffers[2][NTSC_SAMPLES_PER_LINE + 3 & ~3u] __attribute__ ((aligned (4)));

// NTSC color palette lookup table
// Each color has 4 entries for the 4 phases of NTSC color subcarrier (0°, 90°, 180°, 270°)
// This allows proper color encoding at 3.579545 MHz
static uint16_t ntsc_palette[4 * 256] __attribute__ ((aligned (4)));

// DMA channel handles for ping-pong operation
static uint ntsc_dma_chan_primary, ntsc_dma_chan_secondary;

/* ===========================================================================
 * Function: ntsc_generate_scanline
 * Purpose: Generate NTSC composite video signal data for one scanline
 * =========================================================================== */
static inline void ntsc_generate_scanline(uint16_t *output_buffer, const size_t scanline_number) {
    // Static pointer maintains position between function calls
    static uint8_t *current_pixel_ptr = ntsc_framebuffer;

    uint16_t *buffer_ptr = output_buffer;

    // Generate equalizing pulses for the first two scanlines
    if (scanline_number < 2) {
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
        for (int j = 0; j < 8; j++)
            *buffer_ptr++ = NTSC_LEVEL_BLANK;

        // Color burst signal - 9 cycles at 3.579545 MHz
        // Alternates between levels to create a reference signal for color decoding
        for (int j = 0; j < 9; j++) {
            *buffer_ptr++ = 2; // Phase 0°
            *buffer_ptr++ = 1; // Phase 90°
            *buffer_ptr++ = 2; // Phase 180°
            *buffer_ptr++ = 3; // Phase 270°
        }

        // Fill remainder with blanking level
        while (buffer_ptr < output_buffer + NTSC_SAMPLES_PER_LINE)
            *buffer_ptr++ = NTSC_LEVEL_BLANK;
    }
    // Generate active video scanlines
    else if (scanline_number >= NTSC_VSYNC_LINES + NTSC_VBLANK_TOP &&
             scanline_number < NTSC_VSYNC_LINES + NTSC_VBLANK_TOP + NTSC_FRAME_HEIGHT) {
        // Skip horizontal blanking interval
        buffer_ptr += NTSC_ACTIVE_START;

        // Reset framebuffer pointer at start of first visible scanline
        if (scanline_number == NTSC_VSYNC_LINES + NTSC_VBLANK_TOP) {
            current_pixel_ptr = ntsc_framebuffer;
#if !NDEBUG
            ntsc_is_rendering_active = 1;
#endif
        }

        // Process all pixels in the scanline
        for (int pixel_index = 0; pixel_index < NTSC_FRAME_WIDTH; pixel_index++) {
            // Read one graphics pixel
            const uint8_t pixel_color = *current_pixel_ptr++;

            // Write 4 NTSC phase values for this pixel
            // Using 32-bit writes for efficiency (2 phase values at once)
            // Phase offset alternates: 0,2,0,2... for proper color encoding
            const uint32_t phase_offset = pixel_index & 1 ? 2 : 0;
            *(uint32_t *) buffer_ptr = *(uint32_t *) (ntsc_palette + pixel_color * 4 + phase_offset);
            buffer_ptr += 2;
        }
    }
    // Generate vertical blanking lines after active video
    else if (scanline_number == NTSC_VSYNC_LINES + NTSC_VBLANK_TOP + NTSC_FRAME_HEIGHT ||
             scanline_number == NTSC_VSYNC_LINES + NTSC_VBLANK_TOP + NTSC_FRAME_HEIGHT + 1) {
#if !NDEBUG
        // Mark end of active video on first blanking line
        if (scanline_number == NTSC_VSYNC_LINES + NTSC_VBLANK_TOP + NTSC_FRAME_HEIGHT) {
            ntsc_is_rendering_active = 0;
            ntsc_frame_counter++;
        }
#endif
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
    int32_t composite_signal = (luminance * 1792 + blue_chroma_0 + red_chroma_0 + 2 * 65536 + 32768) / 65536;
    ntsc_palette[palette_index * 4] = composite_signal < 0 ? 0 : composite_signal;

    // Phase 90°: Y + chroma(90°)
    composite_signal = (luminance * 1792 + blue_chroma_90 + red_chroma_90 + 2 * 65536 + 32768) / 65536;
    ntsc_palette[palette_index * 4 + 1] = composite_signal < 0 ? 0 : composite_signal;

    // Phase 180°: Y - chroma
    composite_signal = (luminance * 1792 - blue_chroma_0 - red_chroma_0 + 2 * 65536 + 32768) / 65536;
    ntsc_palette[palette_index * 4 + 2] = composite_signal < 0 ? 0 : composite_signal;

    // Phase 270°: Y - chroma(90°)
    composite_signal = (luminance * 1792 - blue_chroma_90 - red_chroma_90 + 2 * 65536 + 32768) / 65536;
    ntsc_palette[palette_index * 4 + 3] = composite_signal < 0 ? 0 : composite_signal;
}

/* ===========================================================================
 * Function: ntsc_dma_irq_handler
 * Purpose: Handle DMA transfer completion and prepare next scanline
 * =========================================================================== */
static void __time_critical_func(ntsc_dma_irq_handler)() {
    static size_t current_scanline = 0;
    // Read and clear DMA interrupt flags
    const volatile uint32_t interrupt_flags = dma_hw->ints0;
    dma_hw->ints0 = interrupt_flags;

    const uint8_t scanline_buffer_index = interrupt_flags & (1u << ntsc_dma_chan_secondary) ? 1 : 0;
    // Determine which channel completed and prepare its buffer
    ntsc_generate_scanline(ntsc_scanline_buffers[scanline_buffer_index], current_scanline);
    if (scanline_buffer_index) {
        dma_channel_set_read_addr(ntsc_dma_chan_secondary, ntsc_scanline_buffers[scanline_buffer_index], false);
    } else {
        dma_channel_set_read_addr(ntsc_dma_chan_primary, ntsc_scanline_buffers[scanline_buffer_index], false);
    }

    // Advance to the next scanline with wraparound
    if (++current_scanline >= NTSC_TOTAL_LINES) {
        current_scanline = 0;
    }
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
    const uint32_t system_clock_khz = 315000;
    const uint32_t pwm_period_cycles = 11;

    vreg_set_voltage(VREG_VOLTAGE_1_30);
    set_sys_clock_khz(system_clock_khz, true);

    // Configure PWM output pin
    gpio_set_function(NTSC_PIN_OUTPUT, GPIO_FUNC_PWM);
    const uint pwm_slice = pwm_gpio_to_slice_num(NTSC_PIN_OUTPUT);

    // Configure PWM for video signal generation
    pwm_config pwm_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&pwm_cfg, 2.0f); // No clock division

    pwm_init(pwm_slice, &pwm_cfg, true);
    pwm_set_wrap(pwm_slice, pwm_period_cycles - 1);

    // Get PWM compare register address for DMA writes
    volatile void *pwm_compare_addr = &pwm_hw->slice[pwm_slice].cc;
    // Offset by 2 bytes to write to the upper 16 bits (channel B)
    pwm_compare_addr = (volatile void *) ((uintptr_t) pwm_compare_addr + 2);

    // Allocate DMA channels for ping-pong operation
    ntsc_dma_chan_primary = dma_claim_unused_channel(true);
    ntsc_dma_chan_secondary = dma_claim_unused_channel(true);

    // Configure primary DMA channel
    dma_channel_config primary_config = dma_channel_get_default_config(ntsc_dma_chan_primary);
    channel_config_set_transfer_data_size(&primary_config, DMA_SIZE_16);
    channel_config_set_read_increment(&primary_config, true); // Increment source
    channel_config_set_write_increment(&primary_config, false); // Fixed destination
    channel_config_set_dreq(&primary_config, DREQ_PWM_WRAP0 + pwm_slice);
    channel_config_set_chain_to(&primary_config, ntsc_dma_chan_secondary); // Chain to secondary

    dma_channel_configure(
        ntsc_dma_chan_primary,
        &primary_config,
        pwm_compare_addr, // Destination: PWM register
        ntsc_scanline_buffers[0], // Source: Buffer 0
        NTSC_SAMPLES_PER_LINE, // Transfer count
        false // Don't start yet
    );

    // Configure a secondary DMA channel (mirrors primary, chains back)
    dma_channel_config secondary_config = dma_channel_get_default_config(ntsc_dma_chan_secondary);
    channel_config_set_transfer_data_size(&secondary_config, DMA_SIZE_16);
    channel_config_set_read_increment(&secondary_config, true);
    channel_config_set_write_increment(&secondary_config, false);
    channel_config_set_dreq(&secondary_config, DREQ_PWM_WRAP0 + pwm_slice);
    channel_config_set_chain_to(&secondary_config, ntsc_dma_chan_primary); // Chain back

    dma_channel_configure(
        ntsc_dma_chan_secondary,
        &secondary_config,
        pwm_compare_addr, // Destination: PWM register
        ntsc_scanline_buffers[1], // Source: Buffer 1
        NTSC_SAMPLES_PER_LINE, // Transfer count
        false // Don't start yet
    );

    // Pre-fill buffers with the first two scanlines
    ntsc_generate_scanline(ntsc_scanline_buffers[0], 0);
    ntsc_generate_scanline(ntsc_scanline_buffers[1], 1);

    // Enable DMA completion interrupts for both channels
    dma_set_irq0_channel_mask_enabled(1u << ntsc_dma_chan_primary | 1u << ntsc_dma_chan_secondary,true);

    // Install and enable interrupt handler
    irq_set_exclusive_handler(DMA_IRQ_0, ntsc_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Start video generation by triggering the first DMA transfer
    dma_start_channel_mask(1u << ntsc_dma_chan_primary);
}
#endif // RP2040_PWM_NTSC_H
