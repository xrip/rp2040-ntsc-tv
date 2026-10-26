#pragma once

// Pure packing helpers, also exercised by the host tests. One physical pixel
// occupies two words; the serializer consumes only the low 30 bits of each.
#include <stdint.h>
#include <stddef.h>

enum { HDMI_WORKSPACE_BYTES = 4896, HDMI_PALETTE_COLORS = 252 };

// The old allocation is shared by two mutually exclusive transport layouts.
// Keep this definition and the in-place conversion helpers visible to host
// tests: a mirrored test layout would miss real offset/overlap regressions.
union hdmi_workspace {
    struct {
        uint32_t palette[256][4];
        uint8_t lines[2][400];
    } graphics;
    struct {
        uint32_t palette[256][2];
        uint8_t lines[2][800];
        uint8_t graphics_rgb[256][3];
    } text;
};
_Static_assert(sizeof(union hdmi_workspace) == HDMI_WORKSPACE_BYTES, "HDMI RAM budget changed");
_Static_assert(offsetof(union hdmi_workspace, text.graphics_rgb) == 3648,
               "Recheck in-place palette conversion order after changing the layout");

static inline uint16_t hdmi_graphics_symbol(uint8_t value) {
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) ones += (value >> i) & 1u;
    const unsigned xnor = ones > 4u || (ones == 4u && !(value & 1u));
    uint16_t q = value & 1u;
    unsigned previous = q;
    for (unsigned i = 1; i < 8; ++i) {
        previous ^= (value >> i) & 1u;
        previous ^= xnor;
        q |= previous << i;
    }
    return q | (xnor ? 0x200u : 0x100u);
}

static inline uint64_t hdmi_pack_symbols(uint16_t red, uint16_t green,
                                          uint16_t blue) {
    uint64_t packed = 0;
    for (unsigned bit = 0; bit < 10; ++bit) {
        const unsigned offset = bit < 5 ? bit * 6 : 32 + (bit - 5) * 6;
        unsigned r = (red >> bit) & 1u;
        unsigned g = (green >> bit) & 1u;
        unsigned b = (blue >> bit) & 1u;
#if HDMI_PIN_invert_diffpairs
        r ^= 1u; g ^= 1u; b ^= 1u;
#endif
        r |= (r ^ 1u) << 1;
        g |= (g ^ 1u) << 1;
        b |= (b ^ 1u) << 1;
#if HDMI_PIN_RGB_notBGR
        packed |= (uint64_t)(r << 4 | g << 2 | b) << offset;
#else
        packed |= (uint64_t)(b << 4 | g << 2 | r) << offset;
#endif
    }
    return packed;
}

static inline void hdmi_store_pixel(uint32_t *words, uint64_t packed) {
    words[0] = (uint32_t)packed;
    words[1] = (uint32_t)(packed >> 32);
}

static inline void hdmi_encode_graphics_rgb(uint32_t *words, uint32_t rgb) {
    const uint64_t first = hdmi_pack_symbols(
            hdmi_graphics_symbol((uint8_t)(rgb >> 16)),
            hdmi_graphics_symbol((uint8_t)(rgb >> 8)),
            hdmi_graphics_symbol((uint8_t)rgb));
    hdmi_store_pixel(words, first);
    // Original low-bit modulation: the paired symbols have zero net disparity.
    hdmi_store_pixel(words + 2, first ^ UINT64_C(0x0003ffffffffffff));
}

static inline uint32_t hdmi_decode_graphics_rgb(const uint32_t *words) {
    const uint64_t packed = (uint64_t)words[0] | (uint64_t)words[1] << 32;
    unsigned channels[3] = {0, 0, 0}; // Physical B/G/R or R/G/B pin order.
    for (unsigned bit = 0; bit < 10; ++bit) {
        const unsigned offset = bit < 5 ? bit * 6 : 32 + (bit - 5) * 6;
        const unsigned sample = (unsigned)(packed >> offset) & 0x3fu;
        for (unsigned channel = 0; channel < 3; ++channel) {
            unsigned data = (sample >> (channel * 2)) & 1u;
#if HDMI_PIN_invert_diffpairs
            data ^= 1u;
#endif
            channels[channel] |= data << bit;
        }
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
        const unsigned q = channels[channel];
        // Inverse of hdmi_graphics_symbol(), not the receiver's TMDS decode:
        // this recovers even the original least significant input colour bit.
        channels[channel] = (q ^ (q << 1) ^ ((q & 0x100u) ? 0 : 0xfeu)) & 0xffu;
    }
#if HDMI_PIN_RGB_notBGR
    return channels[2] << 16 | channels[1] << 8 | channels[0];
#else
    return channels[0] << 16 | channels[1] << 8 | channels[2];
#endif
}

static inline void hdmi_cache_graphics_palette(union hdmi_workspace *storage) {
    // Destination (3648+3*i) is above source (16*i). Descending order
    // preserves every not-yet-decoded graphics entry despite overlap.
    for (unsigned i = HDMI_PALETTE_COLORS; i--;) {
        const uint32_t rgb = hdmi_decode_graphics_rgb(storage->graphics.palette[i]);
        storage->text.graphics_rgb[i][0] = (uint8_t)(rgb >> 16);
        storage->text.graphics_rgb[i][1] = (uint8_t)(rgb >> 8);
        storage->text.graphics_rgb[i][2] = (uint8_t)rgb;
    }
}

static inline void hdmi_restore_graphics_palette(union hdmi_workspace *storage) {
    // Destination (16*i) is below source (3648+3*i). Ascending order
    // restores the palette without destroying a still-needed cache byte.
    for (unsigned i = 0; i < HDMI_PALETTE_COLORS; ++i) {
        const uint8_t *cached = storage->text.graphics_rgb[i];
        const uint32_t rgb = (uint32_t)cached[0] << 16 |
                             (uint32_t)cached[1] << 8 | cached[2];
        hdmi_encode_graphics_rgb(storage->graphics.palette[i], rgb);
    }
}

// Four adjacent physical pixel indices, little-endian. background is the
// colour repeated into four bytes; difference is the scalar foreground^bg.
// Each byte of bits is 0 or 1, so multiplying by a four-bit colour difference
// selects that colour independently in all four bytes, with no carries.
static __attribute__((always_inline)) inline uint32_t hdmi_text_four_pixels(
        unsigned glyph_bits, uint32_t background, uint32_t difference) {
    const uint32_t bits = ((glyph_bits & 15u) * UINT32_C(0x00204081)) &
                          UINT32_C(0x01010101);
    return (background ^ (bits * difference)) | UINT32_C(0x10001000);
}

// MSB-first fonts are reversed during expansion, saving a separate byte
// bit-reversal for every character on every scanline.
static __attribute__((always_inline)) inline uint32_t hdmi_text_four_pixels_reversed(
        unsigned glyph_bits, uint32_t background, uint32_t difference) {
    const uint32_t bits = (((glyph_bits & 15u) * UINT32_C(0x08040201)) >> 3u) &
                          UINT32_C(0x01010101);
    return (background ^ (bits * difference)) | UINT32_C(0x10001000);
}

// The 40-column mode doubles each source glyph bit horizontally.
static __attribute__((always_inline)) inline uint32_t hdmi_text_two_doubled_pixels(
        unsigned glyph_bits, uint32_t background, uint32_t difference) {
    const uint32_t bits = ((glyph_bits & 1u) | ((glyph_bits & 2u) << 15u)) * 257u;
    return (background ^ (bits * difference)) | UINT32_C(0x10001000);
}

// This variant consumes bits 7 and 6, in that order, from an MSB-first glyph.
static __attribute__((always_inline)) inline uint32_t hdmi_text_two_doubled_pixels_reversed(
        unsigned glyph_bits, uint32_t background, uint32_t difference) {
    const uint32_t bits = (((glyph_bits >> 7u) & 1u) |
                          ((glyph_bits & 64u) << 10u)) * 257u;
    return (background ^ (bits * difference)) | UINT32_C(0x10001000);
}

static inline uint64_t hdmi_text_pixel(uint32_t rgb, unsigned phase) {
    // PicoDVI's full-resolution RGB222 construction: every even symbol has
    // disparity -4 and every odd symbol +4, independently of its colour.
    // Component levels decode to 5/4, 80/81, 175/174, 250/251 (even/odd).
    // This is an approximation of the four CGA component levels, not a
    // stateful encoder for arbitrary VGA DAC values.
    static const uint16_t symbols[2][4] = {
        {0x103, 0x130, 0x230, 0x203},
        {0x1fc, 0x1cf, 0x2cf, 0x2fc}
    };
    return hdmi_pack_symbols(symbols[phase][((rgb >> 16) & 255u) / 85u],
                             symbols[phase][((rgb >> 8) & 255u) / 85u],
                             symbols[phase][(rgb & 255u) / 85u]);
}
