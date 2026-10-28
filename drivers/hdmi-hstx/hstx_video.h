#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HSTX_VIDEO_WIDTH = 640,
    HSTX_VIDEO_HEIGHT = 480,
    HSTX_VIDEO_CLOCK_HZ = 126000000,
    HSTX_VIDEO_PIN_BASE = 12
};

typedef struct {
    /* PS1/BGR555: R in bits 0..4, G in 5..9, B in 10..14; bit 15 ignored.
     * NULL requests a black line without reading a pixel buffer. */
    const uint16_t *pixels;
    /* 320 = hardware horizontal doubling; 640 = native pixels.
     * A non-NULL buffer must be 4-byte aligned, including the 320-pixel case. */
    uint16_t width;
} hstx_video_line_t;

/* Called for output lines 0..479 from DMA_IRQ_1, on the start() core.
 * The callback and everything it reads must be in SRAM. It may render a
 * bounded scanline, but must not block, allocate, log, or disable interrupts
 * for an unbounded time. Returned storage must remain unchanged until the
 * fourth subsequent line callback: four buffers indexed by (line & 3) work.
 * No whole-frame RGB framebuffer is allocated by this driver. */
typedef void (*hstx_video_line_callback_t)(uint32_t output_line,
                                           hstx_video_line_t *line);
typedef void (*hstx_video_vsync_callback_t)(void);

/* Set callbacks only while stopped. init/start/stop/deinit must run on the
 * same core. Calling stop() drains DMA before returning, after which all
 * pixel buffers may immediately be reused or released. */
void hstx_video_set_line_callback(hstx_video_line_callback_t callback);
void hstx_video_set_vsync_callback(hstx_video_vsync_callback_t callback);

/* Claims two unused DMA channels and a shared DMA_IRQ_1 handler. Does not
 * change PLLs, clocks, the system bus priority, or any NTSC resources. */
bool hstx_video_init(void);

/* Requires a previously configured, enabled 126 MHz clk_hstx. Uses GPIO12..19:
 * CLK-/+, D0-/+, D1-/+, D2-/+. Outputs 640x480 at exactly 60 Hz when the
 * supplied clock is exact (800 x 525 totals, 25.2 MHz pixel clock).
 * DVI-compatible TMDS video only: no HDMI audio or data islands. */
bool hstx_video_start(void);
void hstx_video_stop(void);
void hstx_video_deinit(void);
bool hstx_video_is_running(void);

/* Diagnostics: frames scheduled and restarts after a missed DMA deadline.
 * A nonzero restart count means the interrupt latency/render budget was
 * exceeded; recovery is not a substitute for meeting that budget. */
extern volatile uint32_t hstx_video_frame_count;
extern volatile uint32_t hstx_video_restart_count;

#ifdef __cplusplus
}
#endif
