/* Host-only test of the production HDMI packing helpers.
 * cc -std=c11 -O2 -Wall -Wextra -Werror tests/test_hdmi_codec.c -o /tmp/test_hdmi_codec
 * /tmp/test_hdmi_codec
 * Repeat with -DHDMI_PIN_invert_diffpairs=0 and/or -DHDMI_PIN_RGB_notBGR=0.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "The host codec test needs assertions enabled"
#endif
#ifndef HDMI_PIN_invert_diffpairs
#define HDMI_PIN_invert_diffpairs 1
#endif
#ifndef HDMI_PIN_RGB_notBGR
#define HDMI_PIN_RGB_notBGR 1
#endif

#include "../drivers/hdmi/hdmi_codec.h"

/* Independent receiver model: five 6-bit OUTs per 32-bit DMA word. */
static void receive_pixel(const uint32_t words[2], uint16_t rgb[3]) {
    rgb[0] = rgb[1] = rgb[2] = 0;
    for (unsigned word = 0; word != 2; ++word) {
        for (unsigned tick = 0; tick != 5; ++tick) {
            unsigned pins = (words[word] >> (tick * 6)) & 63u;
            for (unsigned lane = 0; lane != 3; ++lane) {
                const unsigned pair = (pins >> (2 * lane)) & 3u;
                assert(pair == 1u || pair == 2u);
                unsigned bit = pair & 1u;
                if (HDMI_PIN_invert_diffpairs) bit ^= 1u;
                const unsigned channel = HDMI_PIN_RGB_notBGR ? 2 - lane : lane;
                rgb[channel] |= bit << (word * 5 + tick);
            }
        }
    }
}

static unsigned ones(unsigned value) {
    unsigned count = 0;
    while (value) { count += value & 1u; value >>= 1; }
    return count;
}

/* TMDS receiver decode, including bit 9 inversion. This is deliberately
 * different from the palette-recovery helper, which recovers the source LSB.
 */
static uint8_t receive_component(uint16_t symbol) {
    unsigned q = symbol & 255u;
    if (symbol & 512u) q ^= 255u;
    unsigned value = q & 1u;
    for (unsigned bit = 1; bit != 8; ++bit) {
        unsigned decoded = ((q >> bit) ^ (q >> (bit - 1))) & 1u;
        if (!(symbol & 256u)) decoded ^= 1u;
        value |= decoded << bit;
    }
    return (uint8_t)value;
}

static void check_graphics(uint32_t color) {
    uint32_t words[4];
    uint16_t first[3], second[3];
    hdmi_encode_graphics_rgb(words, color);
    assert(hdmi_decode_graphics_rgb(words) == color);
    receive_pixel(words, first);
    receive_pixel(words + 2, second);
    for (unsigned channel = 0; channel != 3; ++channel) {
        const uint8_t input = (uint8_t)(color >> (16 - 8 * channel));
        const uint8_t a = receive_component(first[channel]);
        const uint8_t b = receive_component(second[channel]);
        assert((a ^ b) == 1u);
        assert((a & 254u) == (input & 254u));
        assert(ones(first[channel]) + ones(second[channel]) == 10u);
    }
}

static unsigned check_workspace_transitions(void) {
    /* Exercise the production union and conversion helpers in place: the
     * cache overlaps the final graphics LUT entries, then the full text LUT
     * and both 800-byte text lines overwrite the old graphics transport.
     */
    struct {
        uint32_t before[4];
        union hdmi_workspace storage;
        uint32_t after[4];
    } guarded;
    memset(&guarded, 0xa5, sizeof guarded);
    uint32_t expected[HDMI_PALETTE_COLORS];
    uint32_t random = 0x8f5a1b29u;
    for (unsigned i = 0; i < HDMI_PALETTE_COLORS; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        expected[i] = random & 0xffffffu;
        hdmi_encode_graphics_rgb(guarded.storage.graphics.palette[i], expected[i]);
    }

    for (unsigned cycle = 0; cycle < 64; ++cycle) {
        hdmi_cache_graphics_palette(&guarded.storage);
        for (unsigned i = 0; i < HDMI_PALETTE_COLORS; ++i) {
            const uint8_t *cached = guarded.storage.text.graphics_rgb[i];
            const uint32_t actual = (uint32_t)cached[0] << 16 |
                                    (uint32_t)cached[1] << 8 | cached[2];
            assert(actual == expected[i]);
        }
        memset(guarded.storage.text.palette, 0x5a, sizeof guarded.storage.text.palette);
        memset(guarded.storage.text.lines[0], 0x3c, sizeof guarded.storage.text.lines[0]);
        memset(guarded.storage.text.lines[1], 0xc3, sizeof guarded.storage.text.lines[1]);

        /* Model palette writes while text is active, including entries whose
         * backing bytes used to hold the top of the graphics LUT.
         */
        for (unsigned i = 0; i < HDMI_PALETTE_COLORS; ++i) {
            if ((i + cycle) % 7u != 0u) continue;
            expected[i] = (expected[i] * 1664525u + 1013904223u) & 0xffffffu;
            uint8_t *cached = guarded.storage.text.graphics_rgb[i];
            cached[0] = (uint8_t)(expected[i] >> 16);
            cached[1] = (uint8_t)(expected[i] >> 8);
            cached[2] = (uint8_t)expected[i];
        }
        hdmi_restore_graphics_palette(&guarded.storage);
        for (unsigned i = 0; i < HDMI_PALETTE_COLORS; ++i)
            assert(hdmi_decode_graphics_rgb(guarded.storage.graphics.palette[i]) == expected[i]);
        memset(guarded.storage.graphics.lines, (int)cycle,
               sizeof guarded.storage.graphics.lines);
        for (unsigned i = 0; i < 4; ++i) {
            assert(guarded.before[i] == 0xa5a5a5a5u);
            assert(guarded.after[i] == 0xa5a5a5a5u);
        }
    }
    return 64;
}

static unsigned check_packed_glyphs(void) {
    unsigned cases = 0;
    for (unsigned glyph = 0; glyph < 256; ++glyph) {
        for (unsigned attribute = 0; attribute < 256; ++attribute) {
            const unsigned foreground = attribute & 15u;
            const unsigned background = attribute >> 4;
            const uint32_t packed_background = background * 0x01010101u;
            const uint32_t difference = foreground ^ background;
            for (unsigned wide = 0; wide < 2; ++wide) {
                for (unsigned msb_first = 0; msb_first < 2; ++msb_first) {
                    const unsigned words = wide ? 4 : 2;
                    for (unsigned word = 0; word < words; ++word) {
                        uint32_t packed;
                        if (wide) {
                            packed = msb_first
                                    ? hdmi_text_two_doubled_pixels_reversed(
                                          (uint8_t)(glyph << (word * 2)),
                                          packed_background, difference)
                                    : hdmi_text_two_doubled_pixels(glyph >> (word * 2),
                                          packed_background, difference);
                        } else {
                            packed = msb_first
                                    ? hdmi_text_four_pixels_reversed(glyph >> ((1u - word) * 4),
                                          packed_background, difference)
                                    : hdmi_text_four_pixels(glyph >> (word * 4),
                                          packed_background, difference);
                        }
                        for (unsigned byte = 0; byte < 4; ++byte) {
                            const unsigned pixel = word * 4 + byte;
                            unsigned bit = wide ? pixel / 2 : pixel;
                            if (msb_first) bit = 7u - bit;
                            const unsigned color = (glyph & (1u << bit))
                                    ? foreground : background;
                            const unsigned expected = color + ((pixel & 1u) ? 16u : 0u);
                            assert(((packed >> (byte * 8)) & 255u) == expected);
                        }
                    }
                    ++cases;
                }
            }
        }
    }
    return cases;
}

int main(void) {
    unsigned packing_cases = 0, graphics_cases = 0, text_pairs = 0;
    /* Every bit pattern on each TMDS lane, with different neighbouring lanes. */
    for (unsigned symbol = 0; symbol != 1024; ++symbol) {
        uint16_t expected[3] = {(uint16_t)symbol,
                (uint16_t)((symbol * 53u) & 1023u),
                (uint16_t)((symbol * 211u) & 1023u)};
        uint32_t words[2];
        uint16_t actual[3];
        hdmi_store_pixel(words, hdmi_pack_symbols(expected[0], expected[1], expected[2]));
        receive_pixel(words, actual);
        for (unsigned channel = 0; channel != 3; ++channel)
            assert(actual[channel] == expected[channel]);
        ++packing_cases;
    }

    for (unsigned channel = 0; channel != 3; ++channel) {
        for (unsigned value = 0; value != 256; ++value) {
            const unsigned shift = 8 * channel;
            const uint32_t color = (0x53a7d9u & ~(255u << shift)) | (value << shift);
            check_graphics(color);
            ++graphics_cases;
        }
    }
    uint32_t random = 0x54a31f77u;
    for (unsigned i = 0; i != 8192; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        check_graphics(random & 0xffffffu);
        ++graphics_cases;
    }

    static const uint32_t cga[16] = {
        0x000000, 0x0000aa, 0x00aa00, 0x00aaaa,
        0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
        0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
        0xff5555, 0xff55ff, 0xffff55, 0xffffff
    };
    static const uint8_t decoded_levels[2][4] = {
        {5, 80, 175, 250}, {4, 81, 174, 251}
    };
    for (unsigned left = 0; left != 16; ++left) {
        for (unsigned right = 0; right != 16; ++right) {
            uint16_t symbols[2][3];
            const uint32_t colors[2] = {cga[left], cga[right]};
            for (unsigned phase = 0; phase != 2; ++phase) {
                uint32_t words[2];
                hdmi_store_pixel(words, hdmi_text_pixel(colors[phase], phase));
                receive_pixel(words, symbols[phase]);
                for (unsigned channel = 0; channel != 3; ++channel) {
                    const unsigned nominal = (colors[phase] >> (16 - 8 * channel)) & 255u;
                    const unsigned expected = decoded_levels[phase][nominal / 85u];
                    assert(receive_component(symbols[phase][channel]) == expected);
                    assert(ones(symbols[phase][channel]) == (phase ? 7u : 3u));
                    assert(expected + 5 >= nominal && nominal + 5 >= expected);
                }
            }
            for (unsigned channel = 0; channel != 3; ++channel)
                assert(ones(symbols[0][channel]) + ones(symbols[1][channel]) == 10u);
            ++text_pairs;
        }
    }
    const unsigned workspace_cycles = check_workspace_transitions();
    const unsigned packed_glyphs = check_packed_glyphs();
    printf("HDMI codec OK: %u packing cases, %u exact palette round trips, "
           "%u independent text pairs, %u in-place workspace cycles "
           "and %u packed glyph/attribute cases "
           "(invert=%d, RGB=%d)\n",
           packing_cases, graphics_cases, text_pairs, workspace_cycles, packed_glyphs,
           HDMI_PIN_invert_diffpairs, HDMI_PIN_RGB_notBGR);
    return 0;
}
