#include <math.h>
#include <string.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <pico/multicore.h>
#include <pico/time.h>

#include "ntsc-tv-out.h"

enum {
    DEMO_HORIZON = 68,
    DEMO_FRAME_TIME_MS = 33,
    DEMO_X_TRAVEL_FRAMES = 157,
    DEMO_DEPTH_TRAVEL_FRAMES = 211,
    DEMO_BOUNCE_FRAMES = 44,
    DEMO_JUMP_HEIGHT = 42,
    DEMO_SCENE_FRAMES = 240,
    DEMO_SCENE_TRANSITION_FRAMES = 24,
    DEMO_SCENE_COUNT = 3,

    DEMO_SKY_BASE = 0,
    DEMO_SKY_SHADES = 16,
    DEMO_FLOOR_DARK_BASE = 16,
    DEMO_FLOOR_LIGHT_BASE = 32,
    DEMO_FLOOR_SHADES = 16,
    DEMO_WAVE_COORD_SHIFT = 10,
    DEMO_WAVE_SCALE = 96,
    DEMO_BALL_BASE = 64,
    DEMO_BALL_SHADES = 32,
    DEMO_BALL_COLOR_COUNT = 6,
    DEMO_BALL_CROSSFADE_FRAMES = 32,

    DEMO_BALL_SIZE_COUNT = 3,
    DEMO_BALL_MAX_RADIUS = 38,
    DEMO_BALL_MAX_DIAMETER = DEMO_BALL_MAX_RADIUS * 2 + 1,
    DEMO_TRANSPARENT = 255,
    DEMO_SHADOW_MAX_HEIGHT = 12,

    DEMO_OBJECT_SIZE = 128,
    DEMO_OBJECT_HALF = DEMO_OBJECT_SIZE / 2,
    DEMO_TORUS_MAJOR_STEPS = 64,
    DEMO_TORUS_MINOR_STEPS = 24,
    DEMO_HELIX_POINTS = 384
};

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
} demo_vec3_t;

static constexpr uint8_t demo_ball_radii[DEMO_BALL_SIZE_COUNT] = {26, 32, 38};
static constexpr uint8_t demo_ball_colors[DEMO_BALL_COLOR_COUNT][3] = {
        {255, 65, 25},
        {255, 215, 25},
        {55, 255, 75},
        {25, 235, 255},
        {70, 90, 255},
        {255, 50, 225}
};
static constexpr uint8_t demo_bayer_4x4[16] = {
        0, 8, 2, 10,
        12, 4, 14, 6,
        3, 11, 1, 9,
        15, 7, 13, 5
};

static uint8_t demo_ball_sprites[DEMO_BALL_SIZE_COUNT]
                                [DEMO_BALL_MAX_DIAMETER * DEMO_BALL_MAX_DIAMETER];
static uint8_t demo_backbuffer[NTSC_FRAME_WIDTH * NTSC_FRAME_HEIGHT]
                              __attribute__ ((aligned (4)));
static uint8_t demo_shadow_half_width[DEMO_BALL_SIZE_COUNT]
                                     [DEMO_SHADOW_MAX_HEIGHT * 2 + 1];
static uint32_t demo_floor_x_step[NTSC_FRAME_HEIGHT - DEMO_HORIZON];
static uint32_t demo_floor_z[NTSC_FRAME_HEIGHT - DEMO_HORIZON];
static uint8_t demo_floor_shade[NTSC_FRAME_HEIGHT - DEMO_HORIZON];
static int8_t demo_wave_lut[256];
static int16_t demo_sine_lut[256];
static uint8_t demo_object_depth[DEMO_OBJECT_SIZE * DEMO_OBJECT_SIZE];

static inline void demo_set_rgb(const uint8_t index,
                                const uint8_t red,
                                const uint8_t green,
                                const uint8_t blue) {
    ntsc_set_color(index, blue, red, green);
}

static void demo_init_palette(void) {
    for (int shade = 0; shade < DEMO_SKY_SHADES; ++shade) {
        demo_set_rgb((uint8_t)(DEMO_SKY_BASE + shade),
                     (uint8_t)(3 + shade),
                     (uint8_t)(5 + shade * 2),
                     (uint8_t)(20 + shade * 5));
    }

    for (int shade = 0; shade < DEMO_FLOOR_SHADES; ++shade) {
        const uint8_t dark = (uint8_t)(2 + shade * 45 /
                                           (DEMO_FLOOR_SHADES - 1));
        const uint8_t light = (uint8_t)(8 + shade * 220 /
                                            (DEMO_FLOOR_SHADES - 1));
        demo_set_rgb((uint8_t)(DEMO_FLOOR_DARK_BASE + shade), dark, dark, dark);
        demo_set_rgb((uint8_t)(DEMO_FLOOR_LIGHT_BASE + shade), light, light, light);
    }

    for (int color = 0; color < DEMO_BALL_COLOR_COUNT; ++color) {
        for (int shade = 0; shade < DEMO_BALL_SHADES; ++shade) {
            demo_set_rgb((uint8_t)(DEMO_BALL_BASE +
                                   color * DEMO_BALL_SHADES + shade),
                         (uint8_t)(demo_ball_colors[color][0] *
                                   (shade + 2) / 33),
                         (uint8_t)(demo_ball_colors[color][1] *
                                   (shade + 2) / 33),
                         (uint8_t)(demo_ball_colors[color][2] *
                                   (shade + 2) / 33));
        }
    }
}

static void demo_init_floor(void) {
    const int floor_height = NTSC_FRAME_HEIGHT - DEMO_HORIZON;

    for (int index = 0; index < floor_height; ++index) {
        const int screen_depth = index + 1;
        // 16.16 world coordinates keep nearby scanlines from collapsing
        // onto the same checker row after the perspective division.
        const uint32_t x_step = (192u << 8) / (uint32_t)screen_depth;

        demo_floor_x_step[index] = x_step;
        demo_floor_z[index] = 144u * x_step;
        demo_floor_shade[index] =
                (uint8_t)(screen_depth * (DEMO_FLOOR_SHADES - 1) / floor_height);
    }

}

static void demo_init_wave(void) {
    const float two_pi = 6.283185307179586f;

    for (int phase = 0; phase < 256; ++phase) {
        const float sine = sinf(two_pi * (float)phase / 256.0f);
        demo_wave_lut[phase] = (int8_t)lrintf(127.0f * sine);
        demo_sine_lut[phase] = (int16_t)lrintf(32767.0f * sine);
    }
}

static inline demo_vec3_t demo_rotate(const demo_vec3_t point,
                                      const uint8_t angle_y,
                                      const uint8_t angle_x) {
    const int32_t sine_y = demo_sine_lut[angle_y];
    const int32_t cosine_y = demo_sine_lut[(uint8_t)(angle_y + 64u)];
    const int32_t sine_x = demo_sine_lut[angle_x];
    const int32_t cosine_x = demo_sine_lut[(uint8_t)(angle_x + 64u)];
    const int32_t rotated_x =
            (point.x * cosine_y + point.z * sine_y) >> 15;
    const int32_t rotated_z =
            (-point.x * sine_y + point.z * cosine_y) >> 15;

    const demo_vec3_t result = {
            rotated_x,
            (point.y * cosine_x - rotated_z * sine_x) >> 15,
            (point.y * sine_x + rotated_z * cosine_x) >> 15
    };
    return result;
}

static inline uint8_t demo_normal_shade(const demo_vec3_t normal) {
    int shade = 8 +
            (int)((-normal.x * 9 - normal.y * 13 + normal.z * 18) >> 15);
    if (shade < 1) {
        shade = 1;
    } else if (shade >= DEMO_BALL_SHADES) {
        shade = DEMO_BALL_SHADES - 1;
    }
    return (uint8_t)shade;
}

static inline uint8_t demo_shaded_color(const uint32_t frame,
                                        const uint8_t shade,
                                        const int x,
                                        const int y) {
    const uint32_t color_phase = frame / DEMO_BALL_CROSSFADE_FRAMES;
    const uint8_t first_color = (uint8_t)(color_phase % DEMO_BALL_COLOR_COUNT);
    const uint8_t second_color =
            (uint8_t)((first_color + 1u) % DEMO_BALL_COLOR_COUNT);
    const uint8_t blend = (uint8_t)(frame % DEMO_BALL_CROSSFADE_FRAMES);
    const uint8_t threshold =
            (uint8_t)(demo_bayer_4x4[(y & 3) * 4 + (x & 3)] * 2u);
    const uint8_t color = threshold < blend ? second_color : first_color;
    return (uint8_t)(DEMO_BALL_BASE + color * DEMO_BALL_SHADES + shade);
}

static inline bool demo_pixel_visible(const int x,
                                      const int y,
                                      const uint8_t visibility) {
    return demo_bayer_4x4[(y & 3) * 4 + (x & 3)] < visibility;
}

static void demo_init_ball(void) {
    for (int size = 0; size < DEMO_BALL_SIZE_COUNT; ++size) {
        const int radius = demo_ball_radii[size];
        uint8_t *sprite = demo_ball_sprites[size];
        memset(sprite, DEMO_TRANSPARENT,
               DEMO_BALL_MAX_DIAMETER * DEMO_BALL_MAX_DIAMETER);

        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                const int distance_squared = x * x + y * y;
                if (distance_squared > radius * radius) {
                    continue;
                }

                const float z = sqrtf((float)(radius * radius - distance_squared));
                const float normal_x = (float)x / (float)radius;
                const float normal_y = (float)y / (float)radius;
                const float normal_z = z / (float)radius;
                float light = -0.42f * normal_x - 0.55f * normal_y + 0.72f * normal_z;
                if (light < 0.0f) {
                    light = 0.0f;
                }

                float highlight = light * light;
                highlight *= highlight;
                highlight *= highlight;

                int shade = (int)lrintf(2.0f + light * 23.0f + highlight * 6.0f);
                if (shade >= DEMO_BALL_SHADES) {
                    shade = DEMO_BALL_SHADES - 1;
                }

                const int sprite_x = x + DEMO_BALL_MAX_RADIUS;
                const int sprite_y = y + DEMO_BALL_MAX_RADIUS;
                sprite[sprite_y * DEMO_BALL_MAX_DIAMETER + sprite_x] =
                        (uint8_t)shade;
            }
        }

        const int shadow_height = 5 + radius / 6;
        for (int y = -shadow_height; y <= shadow_height; ++y) {
            const float row = (float)y / (float)shadow_height;
            const int half_width = (int)lrintf((float)radius * sqrtf(1.0f - row * row));
            demo_shadow_half_width[size][y + DEMO_SHADOW_MAX_HEIGHT] =
                    (uint8_t)half_width;
        }
    }
}

static void demo_draw_background(uint8_t *framebuffer, const uint32_t frame) {
    for (int y = 0; y < DEMO_HORIZON; ++y) {
        const uint8_t color = (uint8_t)(DEMO_SKY_BASE +
                y * (DEMO_SKY_SHADES - 1) / (DEMO_HORIZON - 1));
        memset(&framebuffer[y * NTSC_FRAME_WIDTH], color, NTSC_FRAME_WIDTH);
    }

    const uint32_t floor_scroll = frame * (8u << 8);
    for (int y = DEMO_HORIZON; y < NTSC_FRAME_HEIGHT; ++y) {
        const int floor_index = y - DEMO_HORIZON;
        const uint32_t x_step = demo_floor_x_step[floor_index];
        uint32_t world_x = (128u << 16) - (NTSC_FRAME_WIDTH / 2u) * x_step;
        const uint32_t world_z = demo_floor_z[floor_index] + floor_scroll;
        const uint8_t wave_time = (uint8_t)(frame * 2u);
        const int32_t row_wave_x =
                demo_wave_lut[(uint8_t)((world_z >> DEMO_WAVE_COORD_SHIFT) + wave_time)] *
                DEMO_WAVE_SCALE;
        const uint8_t shade = demo_floor_shade[floor_index];
        const uint8_t dark = (uint8_t)(DEMO_FLOOR_DARK_BASE + shade);
        const uint8_t light = (uint8_t)(DEMO_FLOOR_LIGHT_BASE + shade);
        uint8_t *row = &framebuffer[y * NTSC_FRAME_WIDTH];

        for (int x = 0; x < NTSC_FRAME_WIDTH; ++x) {
            const int32_t column_wave_z =
                    demo_wave_lut[(uint8_t)((world_x >> DEMO_WAVE_COORD_SHIFT) -
                                            wave_time)] * DEMO_WAVE_SCALE;
            const uint32_t x_tile = (world_x + row_wave_x) >> 16;
            const uint32_t z_tile = (world_z + column_wave_z) >> 16;
            row[x] = ((x_tile ^ z_tile) & 1u) ? light : dark;
            world_x += x_step;
        }
    }
}

static uint32_t demo_triangle_phase(const uint32_t frame,
                                    const uint32_t travel_frames) {
    const uint32_t period = travel_frames * 2u;
    uint32_t phase = frame % period;
    if (phase > travel_frames) {
        phase = period - phase;
    }
    return phase;
}

static void demo_ball_position(const uint32_t frame,
                               int *center_x,
                               int *center_y,
                               int *ground_y,
                               int *size_index,
                               int *jump_height) {
    const uint32_t x_phase =
            demo_triangle_phase(frame, DEMO_X_TRAVEL_FRAMES);
    const uint32_t depth_phase =
            demo_triangle_phase(frame, DEMO_DEPTH_TRAVEL_FRAMES);
    const uint32_t bounce_phase = frame % DEMO_BOUNCE_FRAMES;

    int size = (int)(depth_phase * DEMO_BALL_SIZE_COUNT /
                     (DEMO_DEPTH_TRAVEL_FRAMES + 1u));
    if (size >= DEMO_BALL_SIZE_COUNT) {
        size = DEMO_BALL_SIZE_COUNT - 1;
    }

    const int radius = demo_ball_radii[size];
    const int margin = 8;
    const int min_x = margin + radius;
    const int max_x = NTSC_FRAME_WIDTH - 1 - margin - radius;
    const int min_y = DEMO_HORIZON + radius + 6;
    const int max_y = NTSC_FRAME_HEIGHT - 1 - margin - radius;
    const int base_y = min_y +
            (max_y - min_y) * (int)depth_phase / DEMO_DEPTH_TRAVEL_FRAMES;
    const int jump = (int)(4u * DEMO_JUMP_HEIGHT * bounce_phase *
            (DEMO_BOUNCE_FRAMES - bounce_phase) /
            (DEMO_BOUNCE_FRAMES * DEMO_BOUNCE_FRAMES));

    *center_x = min_x +
            (max_x - min_x) * (int)x_phase / DEMO_X_TRAVEL_FRAMES;
    *center_y = base_y - jump;
    *ground_y = base_y;
    *size_index = size;
    *jump_height = jump;
}

static void demo_draw_shadow(uint8_t *framebuffer,
                             const int center_x,
                             const int ground_y,
                             const int size_index,
                             const int jump_height,
                             const uint8_t visibility) {
    if (visibility == 0) {
        return;
    }

    const int radius = demo_ball_radii[size_index];
    const int shadow_height = 5 + radius / 6;
    const int shadow_y = ground_y + radius + 4;
    const int shadow_scale = 256 - jump_height * 128 / DEMO_JUMP_HEIGHT;

    for (int offset_y = -shadow_height; offset_y <= shadow_height; ++offset_y) {
        const int y = shadow_y + offset_y;
        if (y < DEMO_HORIZON || y >= NTSC_FRAME_HEIGHT) {
            continue;
        }

        const int half_width =
                demo_shadow_half_width[size_index][offset_y + DEMO_SHADOW_MAX_HEIGHT] *
                shadow_scale / 256;
        int left = center_x - half_width;
        int right = center_x + half_width;
        if (left < 0) {
            left = 0;
        }
        if (right >= NTSC_FRAME_WIDTH) {
            right = NTSC_FRAME_WIDTH - 1;
        }

        const uint8_t floor_shade = demo_floor_shade[y - DEMO_HORIZON];
        uint8_t shadow_shade =
                (uint8_t)(floor_shade / 4u + jump_height * 4 / DEMO_JUMP_HEIGHT);
        if (shadow_shade >= DEMO_FLOOR_SHADES) {
            shadow_shade = DEMO_FLOOR_SHADES - 1;
        }
        const uint8_t shadow_color =
                (uint8_t)(DEMO_FLOOR_DARK_BASE + shadow_shade);
        uint8_t *destination = &framebuffer[y * NTSC_FRAME_WIDTH + left];
        if (visibility >= 16) {
            memset(destination, shadow_color, (size_t)(right - left + 1));
        } else {
            for (int x = left; x <= right; ++x, ++destination) {
                if (demo_pixel_visible(x, y, visibility)) {
                    *destination = shadow_color;
                }
            }
        }
    }
}

static void demo_draw_ball(uint8_t *framebuffer,
                           const int center_x,
                           const int center_y,
                           const int size_index,
                           const uint32_t frame,
                           const uint8_t visibility) {
    const int radius = demo_ball_radii[size_index];
    const uint8_t *sprite = demo_ball_sprites[size_index];

    for (int offset_y = -radius; offset_y <= radius; ++offset_y) {
        const int y = center_y + offset_y;
        if (y < 0 || y >= NTSC_FRAME_HEIGHT) {
            continue;
        }

        const uint8_t *source = &sprite[
                (offset_y + DEMO_BALL_MAX_RADIUS) * DEMO_BALL_MAX_DIAMETER +
                DEMO_BALL_MAX_RADIUS - radius];
        uint8_t *destination = &framebuffer[y * NTSC_FRAME_WIDTH + center_x - radius];

        for (int offset_x = -radius; offset_x <= radius; ++offset_x) {
            const uint8_t shade = *source++;
            const int x = center_x + offset_x;
            if (shade != DEMO_TRANSPARENT &&
                demo_pixel_visible(x, y, visibility)) {
                *destination = demo_shaded_color(frame, shade, x, y);
            }
            ++destination;
        }
    }
}

static void demo_draw_3d_point(uint8_t *framebuffer,
                               const demo_vec3_t point,
                               const int center_x,
                               const int center_y,
                               const int point_radius,
                               const uint8_t shade,
                               const uint32_t frame,
                               const uint8_t visibility) {
    const int camera_distance = 128;
    const int denominator = camera_distance - point.z;
    if (denominator < 48) {
        return;
    }

    const int screen_x = center_x + point.x * 140 / denominator;
    const int screen_y = center_y + point.y * 140 / denominator;
    const int depth_x = screen_x - center_x + DEMO_OBJECT_HALF;
    const int depth_y = screen_y - center_y + DEMO_OBJECT_HALF;
    int depth = point.z + 96;
    if (depth < 1) {
        depth = 1;
    } else if (depth > 255) {
        depth = 255;
    }

    for (int offset_y = -point_radius; offset_y <= point_radius; ++offset_y) {
        const int y = screen_y + offset_y;
        const int object_y = depth_y + offset_y;
        if (y < 0 || y >= NTSC_FRAME_HEIGHT ||
            object_y < 0 || object_y >= DEMO_OBJECT_SIZE) {
            continue;
        }

        for (int offset_x = -point_radius; offset_x <= point_radius; ++offset_x) {
            if (offset_x * offset_x + offset_y * offset_y >
                point_radius * point_radius + 1) {
                continue;
            }

            const int x = screen_x + offset_x;
            const int object_x = depth_x + offset_x;
            if (x < 0 || x >= NTSC_FRAME_WIDTH ||
                object_x < 0 || object_x >= DEMO_OBJECT_SIZE ||
                !demo_pixel_visible(x, y, visibility)) {
                continue;
            }

            uint8_t *stored_depth =
                    &demo_object_depth[object_y * DEMO_OBJECT_SIZE + object_x];
            if (depth >= *stored_depth) {
                *stored_depth = (uint8_t)depth;
                framebuffer[y * NTSC_FRAME_WIDTH + x] =
                        demo_shaded_color(frame, shade, x, y);
            }
        }
    }
}

static void demo_render_torus(uint8_t *framebuffer,
                              const uint32_t frame,
                              const uint8_t visibility) {
    if (visibility == 0) {
        return;
    }

    const int center_x = NTSC_FRAME_WIDTH / 2;
    const int center_y = 116 + demo_wave_lut[(uint8_t)(frame * 2u)] / 12;
    const uint8_t angle_y = (uint8_t)(frame * 2u);
    const uint8_t angle_x = (uint8_t)(frame + 24u);
    memset(demo_object_depth, 0, sizeof(demo_object_depth));
    demo_draw_shadow(framebuffer, center_x, 148, 2, 12, visibility);

    for (int major_index = 0;
         major_index < DEMO_TORUS_MAJOR_STEPS;
         ++major_index) {
        const uint8_t major_phase = (uint8_t)(major_index * 4u);
        const int32_t sine_major = demo_sine_lut[major_phase];
        const int32_t cosine_major =
                demo_sine_lut[(uint8_t)(major_phase + 64u)];

        for (int minor_index = 0;
             minor_index < DEMO_TORUS_MINOR_STEPS;
             ++minor_index) {
            const uint8_t minor_phase =
                    (uint8_t)(minor_index * 256u / DEMO_TORUS_MINOR_STEPS);
            const int32_t sine_minor = demo_sine_lut[minor_phase];
            const int32_t cosine_minor =
                    demo_sine_lut[(uint8_t)(minor_phase + 64u)];
            const int32_t ring_radius = 34 + ((13 * cosine_minor) >> 15);
            const demo_vec3_t point = {
                    (ring_radius * cosine_major) >> 15,
                    (13 * sine_minor) >> 15,
                    (ring_radius * sine_major) >> 15
            };
            const demo_vec3_t normal = {
                    (cosine_minor * cosine_major) >> 15,
                    sine_minor,
                    (cosine_minor * sine_major) >> 15
            };
            const demo_vec3_t rotated_point =
                    demo_rotate(point, angle_y, angle_x);
            const demo_vec3_t rotated_normal =
                    demo_rotate(normal, angle_y, angle_x);
            const int point_radius = rotated_point.z > 8 ? 3 : 2;

            demo_draw_3d_point(framebuffer, rotated_point,
                               center_x, center_y, point_radius,
                               demo_normal_shade(rotated_normal),
                               frame, visibility);
        }
    }
}

static void demo_render_helix(uint8_t *framebuffer,
                              const uint32_t frame,
                              const uint8_t visibility) {
    if (visibility == 0) {
        return;
    }

    const int center_x = NTSC_FRAME_WIDTH / 2;
    const int center_y = 116 + demo_wave_lut[(uint8_t)(frame * 2u)] / 14;
    const uint8_t angle_y = (uint8_t)(frame * 3u);
    const uint8_t angle_x =
            (uint8_t)(34 + demo_wave_lut[(uint8_t)frame] / 6);
    memset(demo_object_depth, 0, sizeof(demo_object_depth));
    demo_draw_shadow(framebuffer, center_x, 148, 2, 18, visibility);

    for (int point_index = 0; point_index < DEMO_HELIX_POINTS; ++point_index) {
        const uint8_t phase =
                (uint8_t)(point_index * 1024u / DEMO_HELIX_POINTS);
        const int32_t sine = demo_sine_lut[phase];
        const int32_t cosine = demo_sine_lut[(uint8_t)(phase + 64u)];
        const demo_vec3_t point = {
                (28 * cosine) >> 15,
                (point_index - DEMO_HELIX_POINTS / 2) * 76 /
                        DEMO_HELIX_POINTS,
                (28 * sine) >> 15
        };
        const demo_vec3_t normal = {cosine, 0, sine};
        const demo_vec3_t rotated_point = demo_rotate(point, angle_y, angle_x);
        const demo_vec3_t rotated_normal = demo_rotate(normal, angle_y, angle_x);
        const int point_radius = rotated_point.z > 0 ? 3 : 2;

        demo_draw_3d_point(framebuffer, rotated_point,
                           center_x, center_y, point_radius,
                           demo_normal_shade(rotated_normal),
                           frame, visibility);
    }
}

static void demo_render_ball(uint8_t *framebuffer,
                             const uint32_t frame,
                             const uint8_t visibility) {
    int center_x;
    int center_y;
    int ground_y;
    int size_index;
    int jump_height;

    demo_ball_position(frame, &center_x, &center_y, &ground_y,
                       &size_index, &jump_height);
    demo_draw_shadow(framebuffer, center_x, ground_y,
                     size_index, jump_height, visibility);
    demo_draw_ball(framebuffer, center_x, center_y,
                   size_index, frame, visibility);
}

static void demo_render_scene(uint8_t *framebuffer,
                              const uint32_t scene,
                              const uint32_t frame,
                              const uint8_t visibility) {
    switch (scene) {
        case 0:
            demo_render_ball(framebuffer, frame, visibility);
            break;
        case 1:
            demo_render_torus(framebuffer, frame, visibility);
            break;
        default:
            demo_render_helix(framebuffer, frame, visibility);
            break;
    }
}

static void demo_render_frame(uint8_t *framebuffer, const uint32_t frame) {
    const uint32_t scene = (frame / DEMO_SCENE_FRAMES) % DEMO_SCENE_COUNT;
    const uint32_t scene_frame = frame % DEMO_SCENE_FRAMES;
    const uint32_t transition_start =
            DEMO_SCENE_FRAMES - DEMO_SCENE_TRANSITION_FRAMES;
    const uint32_t half_transition = DEMO_SCENE_TRANSITION_FRAMES / 2u;

    demo_draw_background(framebuffer, frame);

    if (scene_frame < transition_start) {
        demo_render_scene(framebuffer, scene, frame, 16);
        return;
    }

    const uint32_t transition_frame = scene_frame - transition_start;
    if (transition_frame < half_transition) {
        const uint8_t visibility = (uint8_t)(16u -
                transition_frame * 16u / half_transition);
        demo_render_scene(framebuffer, scene, frame, visibility);
    } else {
        const uint8_t visibility = (uint8_t)(
                (transition_frame - half_transition + 1u) * 16u /
                half_transition);
        const uint32_t next_scene = (scene + 1u) % DEMO_SCENE_COUNT;
        demo_render_scene(framebuffer, next_scene, frame, visibility);
    }
}

[[noreturn]] static void core1_entry(void) {
    uint32_t frame = 0;
    uint8_t *draw_buffer = demo_backbuffer;
    absolute_time_t next_frame = get_absolute_time();

    while (true) {
        demo_render_frame(draw_buffer, frame++);
        ntsc_present_framebuffer(draw_buffer);
        draw_buffer = draw_buffer == demo_backbuffer
                      ? ntsc_framebuffer
                      : demo_backbuffer;

        next_frame = delayed_by_ms(next_frame, DEMO_FRAME_TIME_MS);
        if (absolute_time_diff_us(get_absolute_time(), next_frame) > 0) {
            sleep_until(next_frame);
        } else {
            next_frame = get_absolute_time();
        }
    }
}

[[noreturn]] int main(void) {
    demo_init_palette();
    demo_init_floor();
    demo_init_wave();
    demo_init_ball();
    ntsc_init();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    for (int i = 0; i < 6; ++i) {
        sleep_ms(23);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(23);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

    multicore_launch_core1(core1_entry);

    while (true) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(250);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(750);
    }
}
