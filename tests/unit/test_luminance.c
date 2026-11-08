/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/video.h"

/*
 * The luminance rules on synthetic pictures of
 * parts whose intent is known, through a session's 16-bit PQ output. The
 * gain of a part is the median of log2(output / SDR luminance at 203 cd/m^2)
 * over its flat pixels.
 * - A street (sunlit facades only, its brightest content diffuse: white
 *   plaster, white window frames), with a photograph's grain (+-3 codes): a
 *   yellow and a green wall, the frames on the yellow one and the plaster
 *   between red timbers keep their ratios, within 0.1 stop.
 * - A scene of the green ground (the diffuse gain) with a white wall
 *   outlined in black, a sun with a glow, white HUD text on a dark panel and
 *   a small specular highlight: the wall within 0.1 stop of the ground; the
 *   ground next to the wall within 2% of the ground far away (no halo); the
 *   sun at least 1 stop above the ground (twice the diffuse white); the text
 *   at most log2(1.2) above it; the highlight above log2(1.7) (where a
 *   surface starts to look luminous).
 * - A light entering at the frame's edge: its gain changes by at most 0.1
 *   stop a frame, and under OFFLINE (its lights steadied place by place) it
 *   stays within 0.1 stop of PLAYBACK's (a light moving over a still
 *   background keeps its boost).
 * - A lamp losing its glow in one frame of four (a light judged diffuse for
 *   a frame, as a noisy video's borderline light is): with the session's
 *   window (its lights steadied) its gain changes by at most
 *   0.1 stop a frame; without it, it drops in those frames (printed). In
 *   two frames in a row of six, under TWO_PASS (its lights tracked over the
 *   shot of the first pass) likewise, a second pass started in the middle
 *   rendering as the whole one; without the first pass's lights it drops.
 * - Fine parts (QUALITY): sparkles of a few pixels on a gray
 *   stone wall, a thin neon tube with its glow on a dark wall, a lamp with a
 *   white panel 12 pixels from its disc: the sparkles above log2(1.7) of the
 *   stone (diffuse), the tube and the lamp (a white core in a warm glow) at
 *   least 1 stop above it, the panel's side
 *   next to the lamp within 2% of its far side (the light's boost does not
 *   reach a surface its glow does not). REALTIME's values are printed.
 * - Rapids panning slowly (0.7 pixel a frame): blobs of white foam with
 *   soft edges in a textured froth on textured water. Foam is a surface
 *   whose surround rises and falls, not a glow: a blob's gain changes by
 *   more than 0.2 stop from one frame to the next in at most 2% of the
 *   pairs (the river of the demo video flickered as its foam's judgments
 *   followed the cell grid's phase).
 * - Colored lights (hdrg_params_t.colored_lights, ANIME's default): a red
 *   neon tube with its red glow on a dark wall and a red panel outlined in
 *   black. Turning them on raises the tube by 1 stop and more and leaves
 *   the panel and the wall within 0.1 stop.
 * The parts keep their size in pixels on frames of 1x, 2x and 3x the size
 * (a part's share of the frame must not change its rule). The grading that
 * changes ratios on its own (contrast, clarity, saturation, gamut
 * expansion, highlight desaturation) and the prefilters are neutral, so the
 * parts show the luminance rules alone. A criterion missed is printed
 * before the test fails.
 */
#define TL_W      640u
#define TL_H      360u
#define TL_WHITE  203.0f
#define TL_EW     320u /* the edge-entry sequence */
#define TL_EH     180u
#define TL_FRAMES 36u
#define TL_SPEED  4.0f /* pixels a frame */
#define TL_SUN_R  12.0f
#define TL_DRIFT  0.7f  /* the rapids: pixels a frame */
#define TL_FOAM   16.0f /* ... a blob of foam per lattice cell of this many pixels at most */
#define TL_EDGE   2.5f  /* ... its soft edge */
#define TL_RIPPLE 8.0f  /* ... the water's texture: a lattice of this many pixels */
#define TL_FROTH  4.0f  /* ... the froth around a blob: its falloff (pixels) ... */
#define TL_FROTHY 1.0f  /* ... and its strength at the blob */
#define TL_DROP   4u    /* the flip sequence: a frame in this many loses its glow ... */
#define TL_PERIOD 6u    /* ... and for TWO_PASS, TL_RUN frames in a row of this many */
#define TL_RUN    2u
#define TL_KEPT   1024u /* the first passes' lights tl_first_pass keeps */

/* the flip sequence's sessions */
#define TL_ALONE     0u  /* every frame on its own */
#define TL_WINDOW    1u  /* WINDOWED, its window */
#define TL_TWO_PASS  2u  /* TWO_PASS, the first pass's lights tracked over the shot */
#define TL_UNTRACKED 3u  /* TWO_PASS, the first pass's lights not given */
#define TL_OFFLINE   4u  /* WINDOWED under OFFLINE: the lights' long window, the camera followed */
#define TL_LONG      9u  /* the long dropout: the lamp without its glow 9 frames in a row ... */
#define TL_LONG_OF   24u /* ... of this many */
#define TL_BLINK_OF  12u /* the blink: the lamp (its glow too) in a frame of this many ... */
#define TL_BLINK     UINT32_MAX /* ... (tl_flip's drops) */
#define TL_PAN       6.0f       /* the panning flip sequence: pixels a frame */

typedef struct {
    uint32_t x0, y0, x1, y1;
} tl_box_t;

static hdrg_session_light_t tl_lights[TL_KEPT]; /* tl_first_pass's (tl_kept of them) */
static uint32_t tl_kept;

static const uint8_t tl_font[4][7] = {
    {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, /* H */
    {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, /* P */
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}, /* 1 */
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, /* 0 */
};
static const float tl_sky_top[3] = {120.0f, 170.0f, 230.0f},
                   tl_sky_low[3] = {185.0f, 215.0f, 240.0f}, tl_ground[3] = {90.0f, 140.0f, 90.0f},
                   tl_wall[3] = {244.0f, 238.0f, 224.0f}, tl_ink[3] = {30.0f, 30.0f, 45.0f},
                   tl_yellow[3] = {248.0f, 226.0f, 178.0f}, tl_green[3] = {130.0f, 145.0f, 123.0f},
                   tl_frame[3] = {240.0f, 242.0f, 236.0f}, tl_glass[3] = {40.0f, 50.0f, 60.0f},
                   tl_shutter[3] = {60.0f, 80.0f, 65.0f}, tl_plaster[3] = {235.0f, 238.0f, 230.0f},
                   tl_timber[3] = {156.0f, 111.0f, 108.0f}, tl_shade[3] = {59.0f, 59.0f, 59.0f},
                   tl_panel[3] = {20.0f, 20.0f, 30.0f}, tl_text[3] = {250.0f, 250.0f, 250.0f},
                   tl_metal[3] = {110.0f, 112.0f, 120.0f}, tl_sun[3] = {255.0f, 250.0f, 235.0f},
                   tl_stone[3] = {110.0f, 105.0f, 100.0f}, tl_dark[3] = {40.0f, 40.0f, 52.0f},
                   tl_neon[3] = {255.0f, 235.0f, 245.0f},
                   tl_neon_glow[3] = {255.0f, 120.0f, 200.0f},
                   tl_lamp[3] = {255.0f, 250.0f, 238.0f}, tl_white[3] = {238.0f, 238.0f, 232.0f},
                   tl_lamp_glow[3] = {255.0f, 200.0f, 120.0f}, tl_water[3] = {70.0f, 95.0f, 105.0f},
                   tl_foam[3] = {250.0f, 252.0f, 250.0f}, tl_red_neon[3] = {255.0f, 40.0f, 40.0f},
                   tl_red_glow[3] = {255.0f, 0.0f, 0.0f}, tl_red_fill[3] = {255.0f, 30.0f, 30.0f};

static inline float tl_clamp(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* coverage of pixel (x, y) by the box [x0, x1) x [y0, y1), one pixel of antialiasing */
static inline float tl_box_cover(uint32_t x, uint32_t y, float x0, float y0, float x1, float y1)
{
    const float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float d;

    d = fmaxf(fmaxf(x0 - px, px - x1), fmaxf(y0 - py, py - y1));

    return tl_clamp(0.5f - d, 0.0f, 1.0f);
}

static inline void tl_mix(float *px, const float color[3], float a)
{
    uint8_t c;

    for (c = 0; c < 3; c++) {
        px[c] += (color[c] - px[c]) * a;
    }
}

/* a pixel into the picture, with a grain of +-grain codes (a fixed hash of the sample) */
static inline void tl_store(uint8_t *rgb, size_t i, const float px[3], uint32_t grain)
{
    uint32_t h;
    uint8_t c;
    float n;

    for (c = 0; c < 3; c++) {
        h = (uint32_t)(i * 3u + c) * 2654435761u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        n = (grain != 0) ? (float)(h % (2u * grain + 1u)) - (float)grain : 0.0f;
        rgb[i * 3u + c] = (uint8_t)lrintf(tl_clamp(px[c] + n, 0.0f, 255.0f));
    }
}

static inline float tl_srgb(uint8_t code)
{
    const float v = (float)code / 255.0f;

    return (v <= 0.04045f) ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static inline float tl_pq(uint16_t code)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f,
                c3 = 18.6875f, p = powf((float)code / 65535.0f, 1.0f / m2);

    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

/* the luminance gain at a pixel, stops: BT.2020 PQ output over the sRGB input at 203 cd/m^2 */
static inline float tl_gain(const uint8_t *in, const uint16_t *out, size_t i)
{
    const float sdr =
                    TL_WHITE * (0.2126f * tl_srgb(in[i * 3u]) + 0.7152f * tl_srgb(in[i * 3u + 1u]) +
                                0.0722f * tl_srgb(in[i * 3u + 2u])),
                hdr = 0.2627f * tl_pq(out[i * 3u]) + 0.6780f * tl_pq(out[i * 3u + 1u]) +
                      0.0593f * tl_pq(out[i * 3u + 2u]);

    return log2f(fmaxf(hdr, 1e-4f) / fmaxf(sdr, 1e-4f));
}

/* the pixel holds color within tol codes in every channel */
static inline bool tl_is(const uint8_t *in, size_t i, const float color[3], float tol)
{
    return fabsf((float)in[i * 3u] - color[0]) <= tol &&
           fabsf((float)in[i * 3u + 1u] - color[1]) <= tol &&
           fabsf((float)in[i * 3u + 2u] - color[2]) <= tol;
}

/* a fixed hash of lattice point (x, y) and k, 0..1 */
static inline float tl_hash(uint32_t x, uint32_t y, uint32_t k)
{
    uint32_t h;

    h = (x * 73856093u) ^ (y * 19349663u) ^ (k * 83492791u);
    h ^= h >> 13;
    h *= 2654435761u;
    h ^= h >> 16;

    return (float)(h & 0xffffu) / 65535.0f;
}

/* the rapids' blob of lattice cell (i, j): false = none; its center and radius */
static inline bool tl_blob(uint32_t i, uint32_t j, float *cx, float *cy, float *r)
{
    *cx = ((float)i + 0.2f + 0.6f * tl_hash(i, j, 2u)) * TL_FOAM;
    *cy = ((float)j + 0.2f + 0.6f * tl_hash(i, j, 3u)) * TL_FOAM;
    *r = 1.5f + 3.5f * tl_hash(i, j, 4u);

    return tl_hash(i, j, 1u) < 0.5f;
}

/* a smooth texture at (u, v): the hash on a lattice of cell pixels (key k), bilinear, 0..1 */
static inline float tl_noise(float u, float v, float cell, uint32_t k)
{
    const uint32_t gx = (uint32_t)(u / cell), gy = (uint32_t)(v / cell);
    const float fx = u / cell - (float)gx, fy = v / cell - (float)gy;

    return (tl_hash(gx, gy, k) * (1.0f - fx) + tl_hash(gx + 1u, gy, k) * fx) * (1.0f - fy) +
           (tl_hash(gx, gy + 1u, k) * (1.0f - fx) + tl_hash(gx + 1u, gy + 1u, k) * fx) * fy;
}

static int tl_compare(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

static float tl_median(float *values, uint32_t count)
{
    if (count == 0) {
        return NAN;
    }

    qsort(values, count, sizeof(float), tl_compare);

    return values[count / 2u];
}

/* the sky, a sun of radius r at (sx, sy) with a slow glow; h: the height of the sky */
static void tl_sky(float *px, uint32_t x, uint32_t y, uint32_t h, float sx, float sy, float r)
{
    const float t = tl_clamp((float)y / (float)h, 0.0f, 1.0f), dx = (float)x + 0.5f - sx,
                dy = (float)y + 0.5f - sy, d = sqrtf(dx * dx + dy * dy);
    uint8_t c;

    for (c = 0; c < 3; c++) {
        px[c] = tl_sky_top[c] + (tl_sky_low[c] - tl_sky_top[c]) * t;
    }

    if (d > r) {
        tl_mix(px, tl_sun, 0.55f * expf(-(d - r) / (4.0f * r)));
    }

    tl_mix(px, tl_sun, tl_clamp(r + 0.5f - d, 0.0f, 1.0f));
}

/* "HP 100" in a 5 x 7 font, 3 pixels a dot, from (tx, ty) */
static void tl_glyphs(float *px, uint32_t x, uint32_t y, uint32_t tx, uint32_t ty)
{
    const int8_t glyphs[] = {0, 1, -1, 2, 3, 3};
    uint32_t col, row;
    uint8_t g;

    if (x < tx || y < ty || y >= ty + 21u) {
        return;
    }

    g = (uint8_t)((x - tx) / 18u);
    col = ((x - tx) % 18u) / 3u;
    row = (y - ty) / 3u;

    if (g >= sizeof(glyphs) || glyphs[g] < 0 || col >= 5u) {
        return;
    }

    if ((tl_font[(uint8_t)glyphs[g]][row] >> (4u - col) & 1u) != 0) {
        tl_mix(px, tl_text, 1.0f);
    }
}

/* a window at (wx, wy): shutters, a white frame, the glass */
static void tl_window(float *px, uint32_t x, uint32_t y, float wx, float wy)
{
    tl_mix(px, tl_shutter, tl_box_cover(x, y, wx - 16.0f, wy, wx, wy + 56.0f));
    tl_mix(px, tl_shutter, tl_box_cover(x, y, wx + 40.0f, wy, wx + 56.0f, wy + 56.0f));
    tl_mix(px, tl_frame, tl_box_cover(x, y, wx, wy, wx + 40.0f, wy + 56.0f));
    tl_mix(px, tl_glass, tl_box_cover(x, y, wx + 5.0f, wy + 5.0f, wx + 35.0f, wy + 51.0f));
}

/*
 * The street: a yellow and a green facade with windows, white plaster
 * between red timbers, at the left of a frame width pixels wide (the rest in
 * shade)
 */
static void tl_street(uint8_t *rgb, uint32_t width, uint32_t height)
{
    uint32_t x, y, row, col;
    float px[3];

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            memcpy(px, tl_shade, sizeof(px));

            if (y < TL_H) {
                tl_mix(px, tl_yellow, tl_box_cover(x, y, 0.0f, 0.0f, 220.0f, (float)TL_H));
                tl_mix(px, tl_green, tl_box_cover(x, y, 220.0f, 0.0f, 420.0f, (float)TL_H));
                tl_mix(px, tl_plaster, tl_box_cover(x, y, 420.0f, 0.0f, (float)TL_W, (float)TL_H));

                for (row = 0; row < 3u; row++) {
                    for (col = 0; col < 2u; col++) {
                        tl_window(px, x, y, 40.0f + 100.0f * (float)col,
                                  30.0f + 110.0f * (float)row);
                        tl_window(px, x, y, 250.0f + 100.0f * (float)col,
                                  30.0f + 110.0f * (float)row);
                    }
                }

                /* the timbers: posts every 40 pixels, beams every 60 */
                if (x >= 420u && x < TL_W && ((x - 420u) % 40u < 8u || y % 60u < 8u)) {
                    tl_mix(px, tl_timber, 1.0f);
                }
            }

            tl_store(rgb, (size_t)y * width + x, px, 3u);
        }
    }
}

/*
 * The scene: sky and sun, the ground, a white wall outlined in black, a gray
 * object with a specular highlight, a HUD panel with white text, at the top
 * left of a frame width pixels wide (the rest ground)
 */
static void tl_scene(uint8_t *rgb, uint32_t width, uint32_t height)
{
    uint32_t x, y;
    float px[3], d;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            if (y < 140u && x < TL_W) {
                tl_sky(px, x, y, 140u, 520.0f, 60.0f, 16.0f);
            } else {
                memcpy(px, tl_ground, sizeof(px));
            }

            tl_mix(px, tl_ink, tl_box_cover(x, y, 57.0f, 187.0f, 203.0f, 293.0f));
            tl_mix(px, tl_wall, tl_box_cover(x, y, 60.0f, 190.0f, 200.0f, 290.0f));

            tl_mix(px, tl_metal, tl_box_cover(x, y, 500.0f, 170.0f, 620.0f, 260.0f));
            d = ((float)x + 0.5f - 560.0f) * ((float)x + 0.5f - 560.0f) +
                ((float)y + 0.5f - 200.0f) * ((float)y + 0.5f - 200.0f);
            tl_mix(px, tl_sun,
                   tl_box_cover(x, y, 500.0f, 170.0f, 620.0f, 260.0f) * expf(-d / (2.0f * 16.0f)));

            tl_mix(px, tl_panel, tl_box_cover(x, y, 480.0f, 290.0f, 630.0f, 340.0f));
            tl_glyphs(px, x, y, 492u, 304u);

            tl_store(rgb, (size_t)y * width + x, px, 0u);
        }
    }
}

/* frame t of the edge-entry sequence: a sun with its glow moving in from the right edge */
static void tl_entry(uint8_t *rgb, uint32_t t)
{
    const float sx = (float)TL_EW + TL_SUN_R - TL_SPEED * (float)t;
    uint32_t x, y;
    float px[3];

    for (y = 0; y < TL_EH; y++) {
        for (x = 0; x < TL_EW; x++) {
            tl_sky(px, x, y, TL_EH, sx, 60.0f, TL_SUN_R);
            tl_store(rgb, (size_t)y * TL_EW + x, px, 0u);
        }
    }
}

/*
 * The fine parts at the top left of a frame width pixels wide (the rest
 * stone): sparkles, a neon tube on a dark band, a lamp and a white panel
 */
static void tl_fine(uint8_t *rgb, uint32_t width, uint32_t height)
{
    uint32_t x, y, k;
    float px[3], dx, dy, d;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            memcpy(px, tl_stone, sizeof(px));

            /* sparkles: gaussian glints of sigma 1.5 pixels */
            for (k = 0; k < 6u; k++) {
                dx = (float)x + 0.5f - (60.5f + 60.0f * (float)k);
                dy = (float)y + 0.5f - 60.5f;
                d = dx * dx + dy * dy;

                if (d < 64.0f) {
                    tl_mix(px, tl_sun, expf(-d / (2.0f * 1.5f * 1.5f)));
                }
            }

            /* the neon tube: a 2-pixel core and a glow of sigma 4 on the dark band */
            tl_mix(px, tl_dark, tl_box_cover(x, y, 40.0f, 110.0f, 380.0f, 190.0f));
            dx = (float)x + 0.5f - tl_clamp((float)x + 0.5f, 60.0f, 360.0f);
            dy = (float)y + 0.5f - 150.0f;
            d = fmaxf(sqrtf(dx * dx + dy * dy) - 1.0f, 0.0f);

            if (x < TL_W && y >= 110u && y < 190u && x >= 40u && x < 380u) {
                tl_mix(px, tl_neon_glow, 0.6f * expf(-d * d / (2.0f * 4.0f * 4.0f)));
            }

            tl_mix(px, tl_neon, tl_box_cover(x, y, 60.0f, 149.0f, 360.0f, 151.0f));

            /* the lamp: a disc of radius 6 with a glow of sigma 5, the panel 12 pixels away */
            dx = (float)x + 0.5f - 480.5f;
            dy = (float)y + 0.5f - 90.5f;
            d = sqrtf(dx * dx + dy * dy);

            if (d > 6.0f && d < 40.0f) {
                tl_mix(px, tl_lamp_glow,
                       0.7f * expf(-(d - 6.0f) * (d - 6.0f) / (2.0f * 5.0f * 5.0f)));
            }

            tl_mix(px, tl_lamp, tl_clamp(6.5f - d, 0.0f, 1.0f));
            tl_mix(px, tl_white, tl_box_cover(x, y, 498.5f, 50.0f, 620.0f, 130.0f));

            tl_store(rgb, (size_t)y * width + x, px, 3u);
        }
    }
}

/*
 * Colored lights on a dark wall: a red neon tube (a core of half-width 2.5
 * pixels from x 80 to 560 at y 120, a red glow of sigma 8) and a red panel
 * outlined in black (220..420 x 230..290), no grain
 */
static void tl_colored(uint8_t *rgb, uint32_t width, uint32_t height)
{
    uint32_t x, y;
    float px[3], dx, dy, d;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            memcpy(px, tl_dark, sizeof(px));
            dx = (float)x + 0.5f - tl_clamp((float)x + 0.5f, 80.0f, 560.0f);
            dy = (float)y + 0.5f - 120.0f;
            d = sqrtf(dx * dx + dy * dy);

            if (d > 2.5f) {
                tl_mix(px, tl_red_glow, 0.75f * expf(-(d - 2.5f) * (d - 2.5f) / 128.0f));
            }

            tl_mix(px, tl_red_neon, tl_clamp(3.0f - d, 0.0f, 1.0f));
            tl_mix(px, tl_ink, tl_box_cover(x, y, 217.0f, 227.0f, 423.0f, 293.0f));
            tl_mix(px, tl_red_fill, tl_box_cover(x, y, 220.0f, 230.0f, 420.0f, 290.0f));

            tl_store(rgb, (size_t)y * width + x, px, 0u);
        }
    }
}

/* the codes' scale of frame t of a fade-out: 1 to frame 5, then falling linearly to 0.3 */
static float tl_fade(uint32_t t)
{
    return (t > 5u) ? 1.0f - 0.7f * (float)(t - 5u) / (float)(TL_FRAMES - 6u) : 1.0f;
}

/*
 * frame t of the flip sequence: a lamp on stone, without its glow in drops
 * frames in a row of every period; fade: its codes scaled by tl_fade
 */
static void tl_flip(uint8_t *rgb, uint32_t t, uint32_t period, uint32_t drops, bool fade, float pan)
{
    const bool shown = drops != TL_BLINK || t % period == period / 2u,
               glow = (drops == TL_BLINK)
                          ? shown
                          : (t % period < period / 2u || t % period >= period / 2u + drops);
    const float k = fade ? tl_fade(t) : 1.0f,
                cx = 160.0f + pan * ((float)t - (float)(TL_FRAMES / 2u));
    uint32_t x, y;
    uint8_t c;
    float px[3], dx, dy, d;

    for (y = 0; y < TL_EH; y++) {
        for (x = 0; x < TL_EW; x++) {
            memcpy(px, tl_stone, sizeof(px));
            dx = (float)x + 0.5f - cx;
            dy = (float)y + 0.5f - 90.0f;
            d = sqrtf(dx * dx + dy * dy);

            if (glow && d > 8.0f && d < 48.0f) {
                tl_mix(px, tl_lamp_glow,
                       0.7f * expf(-(d - 8.0f) * (d - 8.0f) / (2.0f * 6.0f * 6.0f)));
            }

            if (shown) {
                tl_mix(px, tl_lamp, tl_clamp(8.5f - d, 0.0f, 1.0f));
            }

            for (c = 0; c < 3u; c++) {
                px[c] *= k;
            }

            tl_store(rgb, (size_t)y * TL_EW + x, px, 3u);
        }
    }
}

/* frame t of the rapids: textured water and blobs of foam, the camera drifting TL_DRIFT a frame */
static void tl_rapids(uint8_t *rgb, uint32_t t)
{
    uint32_t x, y, i, j, ci, cj;
    uint8_t c;
    float px[3], u, v, cx, cy, r, d, froth;

    for (y = 0; y < TL_EH; y++) {
        for (x = 0; x < TL_EW; x++) {
            u = (float)x + 0.5f + TL_DRIFT * (float)t;
            v = (float)y + 0.5f;

            for (c = 0; c < 3u; c++) {
                px[c] = tl_water[c] * (0.75f + 0.5f * tl_noise(u, v, TL_RIPPLE, 0u));
            }

            /* the froth around the blobs, textured, then their cores */
            ci = (uint32_t)(u / TL_FOAM);
            cj = (uint32_t)(v / TL_FOAM);
            froth = 0.0f;

            for (j = (cj > 1u) ? cj - 2u : 0u; j <= cj + 2u; j++) {
                for (i = (ci > 1u) ? ci - 2u : 0u; i <= ci + 2u; i++) {
                    if (tl_blob(i, j, &cx, &cy, &r)) {
                        d = fmaxf(sqrtf((u - cx) * (u - cx) + (v - cy) * (v - cy)) - r, 0.0f);
                        froth = fmaxf(froth, expf(-d * d / (2.0f * TL_FROTH * TL_FROTH)));
                    }
                }
            }

            tl_mix(px, tl_foam, TL_FROTHY * froth * tl_noise(u, v, TL_RIPPLE / 2.0f, 5u));

            for (j = (cj > 0u) ? cj - 1u : 0u; j <= cj + 1u; j++) {
                for (i = (ci > 0u) ? ci - 1u : 0u; i <= ci + 1u; i++) {
                    if (tl_blob(i, j, &cx, &cy, &r)) {
                        d = sqrtf((u - cx) * (u - cx) + (v - cy) * (v - cy));
                        tl_mix(px, tl_foam, tl_clamp((r + TL_EDGE - d) / TL_EDGE, 0.0f, 1.0f));
                    }
                }
            }

            tl_store(rgb, (size_t)y * TL_EW + x, px, 2u);
        }
    }
}

static void tl_params(hdrg_params_t *params, uint8_t preset, uint8_t effort)
{
    hdrg_params_init(params, preset);
    params->effort = effort;
    params->contrast = 1.0f;
    params->clarity = 0.0f;
    params->saturation = 1.0f;
    params->expand_strength = 0.0f;
    params->highlight_desat = 0.0f;
    params->grain_policy = HDRG_GRAIN_PRESERVE;
    params->deblock_strength = 0.0f;
    params->dering_strength = 0.0f;
    params->defringe_strength = 0.0f;
    params->deband_strength = 0.0f;
    params->mctf_strength = 0.0f;
}

/* 16-bit output without dither, analysis at full resolution; no window: every frame on its own */
static void tl_config(hdrg_session_config_t *config, bool window)
{
    hdrg_session_config_init(config);
    config->output.bits = 16;
    config->dither_off = true;
    config->proxy = 0;

    if (!window) {
        config->past = 0;
        config->future = 0;
    }
}

static hdrg_session_t *tl_session(hdrg_context_t *ctx, uint32_t width, uint32_t height,
                                  const hdrg_params_t *params, const hdrg_session_config_t *config)
{
    hdrg_display_caps_t caps;
    hdrg_session_format_t format;
    hdrg_session_t *session;

    memset(&format, 0, sizeof(format));
    format.width = width;
    format.height = height;
    format.bit_depth = 8;
    format.stride = (size_t)width * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, params, &caps, config, &session));

    return session;
}

/* one picture through a session of its own: 16-bit PQ BT.2020 rows */
static void tl_still(const uint8_t *rgb, uint32_t width, uint32_t height,
                     const hdrg_params_t *params, uint16_t *out)
{
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t info;
    hdrg_context_t *ctx;
    hdrg_session_t *session;

    tl_config(&config, true);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    session = tl_session(ctx, width, height, params, &config);

    memset(&in, 0, sizeof(in));
    in.planes[0] = rgb;
    in.strides[0] = (size_t)width * 3u;
    memset(&info, 0, sizeof(info));
    info.planes[0] = (uint8_t *)out;
    info.strides[0] = (size_t)width * 6u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, &info));

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/* the median gain over the pixels of box that hold color within tol codes */
static float tl_part(const uint8_t *in, const uint16_t *out, uint32_t width, const tl_box_t *box,
                     const float color[3], float tol, float *scratch)
{
    uint32_t x, y, n;
    size_t i;

    n = 0;
    for (y = box->y0; y < box->y1; y++) {
        for (x = box->x0; x < box->x1; x++) {
            i = (size_t)y * width + x;

            if (tl_is(in, i, color, tol)) {
                scratch[n++] = tl_gain(in, out, i);
            }
        }
    }

    return tl_median(scratch, n);
}

/* the specular core: the pixels where the highlight covers 80% and more */
static float tl_specular(const uint8_t *in, const uint16_t *out, uint32_t width, float *scratch)
{
    uint32_t x, y, n;
    float d;

    n = 0;
    for (y = 190; y < 210u; y++) {
        for (x = 550; x < 570u; x++) {
            d = ((float)x + 0.5f - 560.0f) * ((float)x + 0.5f - 560.0f) +
                ((float)y + 0.5f - 200.0f) * ((float)y + 0.5f - 200.0f);

            if (expf(-d / (2.0f * 16.0f)) >= 0.8f) {
                scratch[n++] = tl_gain(in, out, (size_t)y * width + x);
            }
        }
    }

    return tl_median(scratch, n);
}

/* the sparkles' cores: the pixels each glint covers 80% and more */
static float tl_sparkles(const uint8_t *in, const uint16_t *out, uint32_t width, float *scratch)
{
    uint32_t x, y, k, n;
    float dx, dy;

    n = 0;
    for (k = 0; k < 6u; k++) {
        for (y = 56; y < 66u; y++) {
            for (x = 56u + 60u * k; x < 66u + 60u * k; x++) {
                dx = (float)x + 0.5f - (60.5f + 60.0f * (float)k);
                dy = (float)y + 0.5f - 60.5f;

                if (expf(-(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f)) >= 0.8f) {
                    scratch[n++] = tl_gain(in, out, (size_t)y * width + x);
                }
            }
        }
    }

    return tl_median(scratch, n);
}

/* the mean output luminance of the box, cd/m^2 */
static float tl_light(const uint16_t *out, uint32_t width, const tl_box_t *box)
{
    uint32_t x, y;
    size_t i;
    double sum;

    sum = 0.0;
    for (y = box->y0; y < box->y1; y++) {
        for (x = box->x0; x < box->x1; x++) {
            i = ((size_t)y * width + x) * 3u;
            sum +=
                0.2627 * tl_pq(out[i]) + 0.6780 * tl_pq(out[i + 1u]) + 0.0593 * tl_pq(out[i + 2u]);
        }
    }

    return (float)(sum / ((double)(box->x1 - box->x0) * (double)(box->y1 - box->y0)));
}

/* a criterion: printed when it fails; the test fails at its end */
static void tl_check(bool pass, const char *what, uint32_t *failed)
{
    char line[160];

    if (!pass) {
        snprintf(line, sizeof(line), "not met: %s", what);
        TEST_MESSAGE(line);
        (*failed)++;
    }
}

static void tl_verdict(uint32_t failed)
{
    TEST_ASSERT_EQUAL_UINT32(0, failed);
}

/* the street and the scene at scale x the size, under preset and effort */
static void tl_criteria(uint8_t preset, uint8_t effort, uint32_t scale, uint32_t *failed)
{
    const tl_box_t yellow = {0, 0, 220, TL_H}, green = {220, 0, 420, TL_H},
                   timbered = {420, 0, TL_W, TL_H}, ground = {300, 340, 460, 360},
                   wall = {60, 190, 200, 290}, sun = {500, 40, 540, 80},
                   text = {490, 300, 600, 330}, near = {38, 200, 54, 280}, far = {4, 200, 20, 280};
    const uint32_t width = TL_W * scale, height = TL_H * scale;
    hdrg_params_t params;
    uint16_t *out;
    uint8_t *rgb;
    char line[256];
    float *scratch, g_green, g_yellow, g_frames, g_plaster, g_timber, g_ground, g_wall, g_sun,
        g_text, g_specular, halo;

    rgb = (uint8_t *)malloc((size_t)width * height * 3u);
    out = (uint16_t *)malloc((size_t)width * height * 6u);
    scratch = (float *)malloc((size_t)width * height * sizeof(float));
    TEST_ASSERT_NOT_NULL(rgb);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(scratch);

    tl_street(rgb, width, height);
    tl_params(&params, preset, effort);
    tl_still(rgb, width, height, &params, out);

    g_green = tl_part(rgb, out, width, &green, tl_green, 6.0f, scratch);
    g_yellow = tl_part(rgb, out, width, &yellow, tl_yellow, 6.0f, scratch);
    g_frames = tl_part(rgb, out, width, &yellow, tl_frame, 6.0f, scratch);
    g_plaster = tl_part(rgb, out, width, &timbered, tl_plaster, 6.0f, scratch);
    g_timber = tl_part(rgb, out, width, &timbered, tl_timber, 6.0f, scratch);

    snprintf(line, sizeof(line),
             "preset %u effort %u at %ux, street: green %+.2f, yellow %+.2f, frames %+.2f, "
             "plaster %+.2f, timber %+.2f stops",
             preset, effort, scale, (double)g_green, (double)g_yellow, (double)g_frames,
             (double)g_plaster, (double)g_timber);
    TEST_MESSAGE(line);

    tl_check(fabsf(g_yellow - g_green) <= 0.1f, "two walls under one light keep their ratio",
             failed);
    tl_check(fabsf(g_frames - g_yellow) <= 0.1f, "white frames keep their ratio to the wall",
             failed);
    tl_check(fabsf(g_plaster - g_timber) <= 0.1f, "white plaster keeps its ratio to the timbers",
             failed);

    tl_scene(rgb, width, height);
    tl_params(&params, preset, effort);
    tl_still(rgb, width, height, &params, out);

    g_ground = tl_part(rgb, out, width, &ground, tl_ground, 0.0f, scratch);
    g_wall = tl_part(rgb, out, width, &wall, tl_wall, 0.0f, scratch);
    g_sun = tl_part(rgb, out, width, &sun, tl_sun, 0.0f, scratch);
    g_text = tl_part(rgb, out, width, &text, tl_text, 0.0f, scratch);
    g_specular = tl_specular(rgb, out, width, scratch);
    halo = tl_light(out, width, &near) / tl_light(out, width, &far) - 1.0f;

    snprintf(line, sizeof(line),
             "preset %u effort %u at %ux, scene: ground %+.2f, wall %+.2f, sun %+.2f, text %+.2f, "
             "specular %+.2f stops; halo %+.1f%%",
             preset, effort, scale, (double)g_ground, (double)g_wall, (double)g_sun, (double)g_text,
             (double)g_specular, 100.0 * (double)halo);
    TEST_MESSAGE(line);

    tl_check(fabsf(g_wall - g_ground) <= 0.1f, "a white wall stays diffuse", failed);
    tl_check(fabsf(halo) <= 0.02f, "no halo on the ground next to the outlined wall", failed);
    tl_check(g_sun >= g_ground + 1.0f, "a light with a glow reaches twice the diffuse white",
             failed);
    tl_check(g_text <= g_ground + log2f(1.2f), "white HUD text stays at the diffuse white", failed);
    tl_check(g_specular > g_ground + log2f(1.7f), "a specular highlight looks luminous", failed);

    free(rgb);
    free(out);
    free(scratch);
}

/* the fine parts at scale x the size; strict: the criteria count (else printed) */
static void tl_fine_criteria(uint8_t preset, uint8_t effort, uint32_t scale, bool strict,
                             uint32_t *failed)
{
    const tl_box_t stone = {420, 200, 620, 340}, tube = {100, 149, 320, 151},
                   lamp = {476, 86, 486, 96}, panel_near = {500, 60, 506, 120},
                   panel_far = {596, 60, 616, 120};
    const uint32_t width = TL_W * scale, height = TL_H * scale;
    hdrg_params_t params;
    uint32_t local;
    uint16_t *out;
    uint8_t *rgb;
    char line[256];
    float *scratch, g_stone, g_sparkles, g_tube, g_lamp, leak;

    rgb = (uint8_t *)malloc((size_t)width * height * 3u);
    out = (uint16_t *)malloc((size_t)width * height * 6u);
    scratch = (float *)malloc((size_t)width * height * sizeof(float));
    TEST_ASSERT_NOT_NULL(rgb);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(scratch);

    tl_fine(rgb, width, height);
    tl_params(&params, preset, effort);
    tl_still(rgb, width, height, &params, out);

    g_stone = tl_part(rgb, out, width, &stone, tl_stone, 6.0f, scratch);
    g_sparkles = tl_sparkles(rgb, out, width, scratch);
    g_tube = tl_part(rgb, out, width, &tube, tl_neon, 6.0f, scratch);
    g_lamp = tl_part(rgb, out, width, &lamp, tl_lamp, 6.0f, scratch);
    leak = tl_light(out, width, &panel_near) / tl_light(out, width, &panel_far) - 1.0f;

    snprintf(line, sizeof(line),
             "preset %u effort %u at %ux, fine: stone %+.2f, sparkles %+.2f, tube %+.2f, lamp "
             "%+.2f stops; panel next to the lamp %+.1f%%",
             preset, effort, scale, (double)g_stone, (double)g_sparkles, (double)g_tube,
             (double)g_lamp, 100.0 * (double)leak);
    TEST_MESSAGE(line);

    local = 0;
    tl_check(g_sparkles > g_stone + log2f(1.7f), "sparkles look luminous", &local);
    tl_check(g_tube >= g_stone + 1.0f, "a thin neon tube reaches twice the diffuse white", &local);
    tl_check(g_lamp >= g_stone + 1.0f, "a lamp with a glow reaches twice the diffuse white",
             &local);
    tl_check(fabsf(leak) <= 0.02f, "the lamp's boost does not reach the panel", &local);

    if (strict) {
        *failed += local;
    }

    free(rgb);
    free(out);
    free(scratch);
}

/* the criteria under preset and effort; the fine parts count under QUALITY */
static void tl_preset(uint8_t preset, uint8_t effort)
{
    uint32_t scale, failed;

    failed = 0;
    for (scale = 1; scale <= 3u; scale++) {
        tl_criteria(preset, effort, scale, &failed);
    }

    for (scale = 1; scale <= 2u; scale++) {
        tl_fine_criteria(preset, effort, scale, effort == HDRG_EFFORT_QUALITY, &failed);
    }

    tl_verdict(failed);
}

static void test_photo(void)
{
    tl_preset(HDRG_PRESET_PHOTO, HDRG_EFFORT_REALTIME);
}

static void test_anime(void)
{
    tl_preset(HDRG_PRESET_ANIME, HDRG_EFFORT_REALTIME);
}

static void test_game(void)
{
    tl_preset(HDRG_PRESET_GAME, HDRG_EFFORT_REALTIME);
}

static void test_photo_quality(void)
{
    tl_preset(HDRG_PRESET_PHOTO, HDRG_EFFORT_QUALITY);
}

static void test_anime_quality(void)
{
    tl_preset(HDRG_PRESET_ANIME, HDRG_EFFORT_QUALITY);
}

static void test_game_quality(void)
{
    tl_preset(HDRG_PRESET_GAME, HDRG_EFFORT_QUALITY);
}

/*
 * the colored lights under preset and effort: on against off, the rest of
 * the preset alike (its decoding curve sets the parts' gains)
 */
static void tl_colored_criteria(uint8_t preset, uint8_t effort)
{
    const tl_box_t wall = {20, 310, 200, 350}, tube = {100, 119, 540, 121},
                   panel = {240, 245, 400, 275};
    hdrg_params_t params;
    uint32_t failed;
    uint16_t *on, *off;
    uint8_t *rgb;
    char line[256];
    float *scratch, d_wall, d_tube, d_panel;

    rgb = (uint8_t *)malloc((size_t)TL_W * TL_H * 3u);
    on = (uint16_t *)malloc((size_t)TL_W * TL_H * 6u);
    off = (uint16_t *)malloc((size_t)TL_W * TL_H * 6u);
    scratch = (float *)malloc((size_t)TL_W * TL_H * sizeof(float));
    TEST_ASSERT_NOT_NULL(rgb);
    TEST_ASSERT_NOT_NULL(on);
    TEST_ASSERT_NOT_NULL(off);
    TEST_ASSERT_NOT_NULL(scratch);

    tl_colored(rgb, TL_W, TL_H);
    tl_params(&params, preset, effort);
    params.colored_lights = true;
    tl_still(rgb, TL_W, TL_H, &params, on);
    params.colored_lights = false;
    tl_still(rgb, TL_W, TL_H, &params, off);

    d_wall = tl_part(rgb, on, TL_W, &wall, tl_dark, 2.0f, scratch) -
             tl_part(rgb, off, TL_W, &wall, tl_dark, 2.0f, scratch);
    d_tube = tl_part(rgb, on, TL_W, &tube, tl_red_neon, 2.0f, scratch) -
             tl_part(rgb, off, TL_W, &tube, tl_red_neon, 2.0f, scratch);
    d_panel = tl_part(rgb, on, TL_W, &panel, tl_red_fill, 2.0f, scratch) -
              tl_part(rgb, off, TL_W, &panel, tl_red_fill, 2.0f, scratch);

    snprintf(line, sizeof(line),
             "preset %u effort %u, colored lights on - off: wall %+.2f, red tube %+.2f, red "
             "panel %+.2f stops",
             preset, effort, (double)d_wall, (double)d_tube, (double)d_panel);
    TEST_MESSAGE(line);

    failed = 0;
    tl_check(d_tube >= 1.0f, "a red neon tube rises 1 stop with the colored lights", &failed);
    tl_check(fabsf(d_panel) <= 0.1f, "an outlined red panel stays", &failed);
    tl_check(fabsf(d_wall) <= 0.1f, "the wall stays", &failed);
    tl_verdict(failed);

    free(rgb);
    free(on);
    free(off);
    free(scratch);
}

static void test_colored(void)
{
    hdrg_params_t params;

    hdrg_params_init(&params, HDRG_PRESET_ANIME);
    TEST_ASSERT_TRUE(params.colored_lights);

    tl_colored_criteria(HDRG_PRESET_ANIME, HDRG_EFFORT_REALTIME);
    tl_colored_criteria(HDRG_PRESET_ANIME, HDRG_EFFORT_QUALITY);
    tl_colored_criteria(HDRG_PRESET_PHOTO, HDRG_EFFORT_REALTIME);
}

/* a light entering at the frame's edge keeps its gain from one frame to the next */
/*
 * the edge-entry sequence (frames) through a session of latency: per frame,
 * the median gain of the sun's visible disc (NAN under 8 pixels)
 */
static void tl_entry_run(const uint8_t *frames, uint8_t latency, float *gains)
{
    const size_t frame_bytes = (size_t)TL_EW * TL_EH * 3u;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t info;
    hdrg_params_t params;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, got, x, y, n;
    uint16_t *out;
    size_t i;
    float *scratch;

    out = (uint16_t *)malloc(frame_bytes * 2u);
    scratch = (float *)malloc((size_t)TL_EW * TL_EH * sizeof(float));
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(scratch);

    tl_params(&params, HDRG_PRESET_PHOTO, HDRG_EFFORT_REALTIME);
    tl_config(&config, true);
    config.latency = latency;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    session = tl_session(ctx, TL_EW, TL_EH, &params, &config);

    memset(&in, 0, sizeof(in));
    in.strides[0] = (size_t)TL_EW * 3u;
    memset(&info, 0, sizeof(info));
    info.planes[0] = (uint8_t *)out;
    info.strides[0] = (size_t)TL_EW * 6u;
    sent = 0;
    got = 0;

    for (;;) {
        status = hdrg_session_receive_frame(session, &info);
        if (status == HDRG_ERR_AGAIN) {
            in.planes[0] = (uint8_t *)frames + frame_bytes * sent;
            in.pts = (int64_t)sent;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TL_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status != HDRG_OK) {
            break;
        }

        /* the visible part of the sun's disc */
        n = 0;
        for (y = 40; y < 80u; y++) {
            for (x = 0; x < TL_EW; x++) {
                i = (size_t)y * TL_EW + x;

                if (tl_is(frames + frame_bytes * got, i, tl_sun, 0.0f)) {
                    scratch[n++] = tl_gain(frames + frame_bytes * got, out, i);
                }
            }
        }

        gains[got] = (n >= 8u) ? tl_median(scratch, n) : NAN;
        got++;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_EOF, status);
    TEST_ASSERT_EQUAL_UINT32(TL_FRAMES, got);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(out);
    free(scratch);
}

static void test_edge_entry(void)
{
    const size_t frame_bytes = (size_t)TL_EW * TL_EH * 3u;
    uint32_t t, failed;
    uint8_t *frames;
    char line[256];
    float watched[TL_FRAMES], offline[TL_FRAMES], worst, apart;

    frames = (uint8_t *)malloc(frame_bytes * TL_FRAMES);
    TEST_ASSERT_NOT_NULL(frames);

    for (t = 0; t < TL_FRAMES; t++) {
        tl_entry(frames + frame_bytes * t, t);
    }

    tl_entry_run(frames, HDRG_LATENCY_PLAYBACK, watched);
    tl_entry_run(frames, HDRG_LATENCY_OFFLINE, offline);
    worst = 0.0f;
    apart = 0.0f;
    failed = 0;

    for (t = 0; t < TL_FRAMES; t++) {
        if (isnan(watched[t]) || isnan(offline[t])) {
            continue;
        }

        apart = fmaxf(apart, fabsf(offline[t] - watched[t]));

        if (t > 0u && !isnan(watched[t - 1u]) && !isnan(offline[t - 1u])) {
            worst = fmaxf(worst, fmaxf(fabsf(watched[t] - watched[t - 1u]),
                                       fabsf(offline[t] - offline[t - 1u])));
        }
    }

    snprintf(line, sizeof(line),
             "edge entry: largest change of the sun's gain %.2f stop a frame, OFFLINE apart from "
             "PLAYBACK %.2f stop",
             (double)worst, (double)apart);
    TEST_MESSAGE(line);

    free(frames);

    tl_check(worst <= 0.1f, "a light entering at the edge keeps its gain", &failed);
    tl_check(apart <= 0.1f, "a light moving over a still background keeps its gain offline",
             &failed);
    tl_verdict(failed);
}

/*
 * The frames from start to the end through a session of config started
 * there, their outputs into rows at their frames' places; passes: a
 * TWO_PASS first pass's records instead (lights: their lights kept in
 * tl_lights)
 */
/* the preroll a session of config reports */
static uint32_t tl_preroll(const hdrg_params_t *params, hdrg_session_config_t config)
{
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    uint32_t preroll;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    session = tl_session(ctx, TL_EW, TL_EH, params, &config);
    preroll = hdrg_session_preroll(session);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    return preroll;
}

static void tl_run(const uint8_t *frames, const hdrg_params_t *params, hdrg_session_config_t config,
                   uint32_t start, uint32_t preroll, uint16_t *rows, hdrg_session_pass_t *passes,
                   bool lights)
{
    const size_t frame_samples = (size_t)TL_EW * TL_EH * 3u;
    hdrg_session_frame_t in;
    hdrg_session_output_t info;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, got;

    config.start = start;
    config.preroll = preroll;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    session = tl_session(ctx, TL_EW, TL_EH, params, &config);

    memset(&in, 0, sizeof(in));
    in.strides[0] = (size_t)TL_EW * 3u;
    sent = start;
    got = start + preroll;

    for (;;) {
        memset(&info, 0, sizeof(info));
        info.planes[0] = (uint8_t *)(rows + frame_samples * got);
        info.strides[0] = (size_t)TL_EW * 6u;

        status = hdrg_session_receive_frame(session, &info);
        if (status == HDRG_ERR_AGAIN) {
            in.planes[0] = frames + frame_samples * sent;
            in.pts = (int64_t)sent;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TL_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status != HDRG_OK) {
            break;
        }

        if (passes != NULL) {
            TEST_ASSERT_TRUE(tl_kept + info.light_count <= TL_KEPT);
            passes[got].lights = (lights && info.light_count > 0u) ? tl_lights + tl_kept : NULL;
            passes[got].light_count = lights ? info.light_count : 0u;
            passes[got].anchor_luma = info.anchor_luma;
            passes[got].key_luma = info.key_luma;
            passes[got].apl_fit = info.apl_fit;
            passes[got].scene_cut = info.scene_cut;

            if (passes[got].lights != NULL) {
                memcpy(tl_lights + tl_kept, info.lights, info.light_count * sizeof(*info.lights));
                tl_kept += info.light_count;
            }
        }

        got++;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_EOF, status);
    TEST_ASSERT_EQUAL_UINT32(TL_FRAMES, got);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/*
 * The flip sequence (the lamp without its glow in drops frames in a row of
 * every period, or TL_BLINK: the lamp in the middle frame of each alone;
 * fading out with fade) through a session of the mode: the
 * largest change of the lamp's gain from one frame to the next, from the
 * third frame, and into *range the largest less the smallest from there.
 * TWO_PASS: without the flash limiter, whose second ends the frames a seek
 * must render first; tracked, a second pass started in the middle renders
 * its frames as the whole one
 */
static float tl_flip_series(uint8_t mode, uint32_t period, uint32_t drops, bool fade, float pan,
                            float *range)
{
    const size_t frame_bytes = (size_t)TL_EW * TL_EH * 3u;
    hdrg_session_pass_t passes[TL_FRAMES];
    hdrg_session_config_t config;
    hdrg_params_t params;
    tl_box_t lamp;
    uint32_t t, start, preroll;
    uint16_t *rows, *seek;
    uint8_t *frames, c;
    int32_t shift;
    float *scratch, color[3], gain, prev, worst, lo, hi;

    frames = (uint8_t *)malloc(frame_bytes * TL_FRAMES);
    rows = (uint16_t *)malloc(frame_bytes * 2u * TL_FRAMES);
    seek = (uint16_t *)malloc(frame_bytes * 2u * TL_FRAMES);
    scratch = (float *)malloc((size_t)TL_EW * TL_EH * sizeof(float));
    TEST_ASSERT_TRUE(frames != NULL && rows != NULL && seek != NULL && scratch != NULL);

    for (t = 0; t < TL_FRAMES; t++) {
        tl_flip(frames + frame_bytes * t, t, period, drops, fade, pan);
    }

    tl_params(&params, HDRG_PRESET_PHOTO, HDRG_EFFORT_REALTIME);
    tl_config(&config, mode != TL_ALONE);

    if (mode == TL_OFFLINE) {
        config.latency = HDRG_LATENCY_OFFLINE;
    }

    if (mode == TL_TWO_PASS || mode == TL_UNTRACKED) {
        config.temporal = HDRG_TEMPORAL_TWO_PASS;
        config.future = 0;
        config.flash_off = true;
        tl_kept = 0;
        tl_run(frames, &params, config, 0, 0, rows, passes, mode == TL_TWO_PASS);
        config.passes = passes;
        config.pass_count = TL_FRAMES;
    }

    tl_run(frames, &params, config, 0, 0, rows, NULL, false);
    prev = NAN;
    worst = 0.0f;
    lo = INFINITY;
    hi = -INFINITY;

    for (t = 0; t < TL_FRAMES; t++) {
        for (c = 0; c < 3u; c++) {
            color[c] = tl_lamp[c] * (fade ? tl_fade(t) : 1.0f);
        }

        shift = (int32_t)lrintf(pan * ((float)t - (float)(TL_FRAMES / 2u)));
        lamp.x0 = (uint32_t)(154 + shift);
        lamp.x1 = (uint32_t)(166 + shift);
        lamp.y0 = 84;
        lamp.y1 = 96;
        gain = tl_part(frames + frame_bytes * t, rows + frame_bytes * t, TL_EW, &lamp, color, 6.0f,
                       scratch);

        if (t >= 2u && !isnan(prev)) {
            worst = fmaxf(worst, fabsf(gain - prev));
        }

        if (t >= 2u && !isnan(gain)) {
            lo = fminf(lo, gain);
            hi = fmaxf(hi, gain);
        }

        prev = gain;
    }

    *range = hi - lo;

    if (mode == TL_TWO_PASS) {
        tl_run(frames, &params, config, TL_FRAMES / 2u + 1u, 0, seek, NULL, false);
        TEST_ASSERT_EQUAL_MEMORY(rows + frame_bytes * (TL_FRAMES / 2u + 1u),
                                 seek + frame_bytes * (TL_FRAMES / 2u + 1u),
                                 frame_bytes * 2u * (TL_FRAMES / 2u - 1u));
    }

    /* OFFLINE: from its preroll before frame 31 (the sequence's own start) as the whole run */
    if (mode == TL_OFFLINE) {
        preroll = tl_preroll(&params, config);
        start = (31u > preroll) ? 31u - preroll : 0u;
        tl_run(frames, &params, config, start, 31u - start, seek, NULL, false);
        TEST_ASSERT_EQUAL_MEMORY(rows + frame_bytes * 31u, seek + frame_bytes * 31u,
                                 frame_bytes * 2u * (TL_FRAMES - 31u));
    }

    free(frames);
    free(rows);
    free(seek);
    free(scratch);

    return worst;
}

/*
 * a window steadies a light judged diffuse for a frame; TWO_PASS's track
 * over the shot, for frames in a row
 */
static void test_light_flip(void)
{
    uint32_t failed;
    char line[256];
    float alone, steadied, bare, tracked, windowed, range;

#if !HDRG_BUILTIN_STATS
    TEST_IGNORE_MESSAGE("built without the temporal filters");
#endif

    alone = tl_flip_series(TL_ALONE, TL_DROP, 1, false, 0.0f, &range);
    steadied = tl_flip_series(TL_WINDOW, TL_DROP, 1, false, 0.0f, &range);
    bare = tl_flip_series(TL_UNTRACKED, TL_PERIOD, TL_RUN, false, 0.0f, &range);
    tracked = tl_flip_series(TL_TWO_PASS, TL_PERIOD, TL_RUN, false, 0.0f, &range);
    windowed = tl_flip_series(TL_WINDOW, TL_PERIOD, TL_RUN, false, 0.0f, &range);
    failed = 0;

    snprintf(line, sizeof(line),
             "light flip: largest change of the lamp's gain %.2f stop a frame (%.2f without the "
             "window); %u frames in a row: TWO_PASS %.2f (%.2f without the first pass's lights), "
             "the default window %.2f",
             (double)steadied, (double)alone, TL_RUN, (double)tracked, (double)bare,
             (double)windowed);
    TEST_MESSAGE(line);

    tl_check(alone >= 0.5f, "the lamp drops without the window (the sequence works)", &failed);
    /* the dropouts fill a quarter of the window: its middle half's mean sways a little there */
    tl_check(steadied <= 0.15f, "the window steadies the lamp", &failed);
    tl_check(bare >= 0.5f, "the lamp drops without the first pass's lights", &failed);
    tl_check(tracked <= 0.1f, "TWO_PASS's tracks steady the lamp", &failed);
    tl_verdict(failed);
}

/*
 * OFFLINE: the lamp without its glow TL_LONG frames in a row of TL_LONG_OF
 * (longer than the default window's half: it falls there) keeps its gain
 * under the lights' long window, still and panning TL_PAN pixels a frame (the camera
 * followed); a session started from its preroll in the middle renders as
 * the whole run
 */
static void test_light_offline(void)
{
    uint32_t failed;
    char line[256];
    float windowed, fallen, still, panned, range;

#if !HDRG_BUILTIN_STATS
    TEST_IGNORE_MESSAGE("built without the temporal filters");
#endif

    windowed = tl_flip_series(TL_WINDOW, TL_LONG_OF, TL_LONG, false, 0.0f, &fallen);
    still = tl_flip_series(TL_OFFLINE, TL_LONG_OF, TL_LONG, false, 0.0f, &range);
    panned = tl_flip_series(TL_OFFLINE, TL_LONG_OF, TL_LONG, false, TL_PAN, &range);
    failed = 0;

    snprintf(line, sizeof(line),
             "light offline: %u frames in a row without the glow: the lamp's largest change a "
             "frame %.2f stop (the default window: %.2f in all), OFFLINE %.2f still, %.2f panning",
             TL_LONG, (double)windowed, (double)fallen, (double)still, (double)panned);
    TEST_MESSAGE(line);

    tl_check(fallen >= 0.5f, "the lamp falls under the default window (the sequence works)",
             &failed);
    tl_check(still <= 0.1f, "OFFLINE steadies the lamp", &failed);
    tl_check(panned <= 0.1f, "OFFLINE steadies the panning lamp", &failed);
    tl_verdict(failed);
}

/* the blink's lamp above the stone in the lamp's second frame (stops) under a session of the mode
 */
static float tl_blink_rise(uint8_t mode)
{
    const uint32_t lit_at = TL_BLINK_OF + TL_BLINK_OF / 2u;
    const size_t frame_bytes = (size_t)TL_EW * TL_EH * 3u;
    hdrg_session_config_t config;
    hdrg_params_t params;
    tl_box_t lamp, ground;
    uint32_t t;
    uint16_t *rows;
    uint8_t *frames;
    float *scratch, rise;

    frames = (uint8_t *)malloc(frame_bytes * TL_FRAMES);
    rows = (uint16_t *)malloc(frame_bytes * 2u * TL_FRAMES);
    scratch = (float *)malloc((size_t)TL_EW * TL_EH * sizeof(float));
    TEST_ASSERT_TRUE(frames != NULL && rows != NULL && scratch != NULL);

    for (t = 0; t < TL_FRAMES; t++) {
        tl_flip(frames + frame_bytes * t, t, TL_BLINK_OF, TL_BLINK, false, 0.0f);
    }

    tl_params(&params, HDRG_PRESET_PHOTO, HDRG_EFFORT_REALTIME);
    tl_config(&config, mode != TL_ALONE);

    if (mode == TL_OFFLINE) {
        config.latency = HDRG_LATENCY_OFFLINE;
    }

    tl_run(frames, &params, config, 0, 0, rows, NULL, false);
    lamp.x0 = 154;
    lamp.x1 = 166;
    lamp.y0 = 84;
    lamp.y1 = 96;
    ground.x0 = 20;
    ground.x1 = 60;
    ground.y0 = 20;
    ground.y1 = 60;
    rise = tl_part(frames + frame_bytes * lit_at, rows + frame_bytes * lit_at, TL_EW, &lamp,
                   tl_lamp, 6.0f, scratch) -
           tl_part(frames + frame_bytes * lit_at, rows + frame_bytes * lit_at, TL_EW, &ground,
                   tl_stone, 6.0f, scratch);

    free(frames);
    free(rows);
    free(scratch);

    return rise;
}

/*
 * A lamp (its glow too) in 1 frame of every TL_BLINK_OF, nothing there in
 * the others (a sparkle): it rises above the stone alone, not under the
 * windows by place (PLAYBACK's, OFFLINE's), where the frames with nothing
 * there are votes of no boost
 */
static void test_light_blink(void)
{
    uint32_t failed;
    char line[256];
    float alone, windowed, offline;

#if !HDRG_BUILTIN_STATS
    TEST_IGNORE_MESSAGE("built without the temporal filters");
#endif

    alone = tl_blink_rise(TL_ALONE);
    windowed = tl_blink_rise(TL_WINDOW);
    offline = tl_blink_rise(TL_OFFLINE);
    failed = 0;

    snprintf(line, sizeof(line),
             "light blink: a lamp in 1 frame of %u above the stone: %.2f stop alone, %.2f under "
             "the default window, %.2f OFFLINE",
             TL_BLINK_OF, (double)alone, (double)windowed, (double)offline);
    TEST_MESSAGE(line);

    tl_check(alone >= 0.5f, "the lamp rises alone (the sequence works)", &failed);
    tl_check(windowed <= 0.2f, "the default window keeps the lamp down", &failed);
    tl_check(offline <= 0.2f, "OFFLINE keeps the lamp down", &failed);
    tl_verdict(failed);
}

/*
 * A lamp fading out to 0.3 of its codes keeps its gain within 0.1 stop
 * (WINDOWED with its window, TWO_PASS): its frames keep the lights'
 * judgment and render in proportion
 */
static void test_light_fade(void)
{
    uint32_t failed;
    char line[256];
    float alone, windowed, two_pass, step;

#if !HDRG_BUILTIN_STATS
    TEST_IGNORE_MESSAGE("built without the temporal filters");
#endif

    (void)tl_flip_series(TL_ALONE, 1, 0, true, 0.0f, &alone);
    step = tl_flip_series(TL_WINDOW, 1, 0, true, 0.0f, &windowed);
    step = fmaxf(step, tl_flip_series(TL_TWO_PASS, 1, 0, true, 0.0f, &two_pass));
    failed = 0;

    snprintf(line, sizeof(line),
             "light fade: the lamp's gain spans %.2f stop (WINDOWED), %.2f (TWO_PASS), %.2f "
             "without the window; the largest change a frame %.2f",
             (double)windowed, (double)two_pass, (double)alone, (double)step);
    TEST_MESSAGE(line);

    tl_check(windowed <= 0.1f, "WINDOWED keeps the fading lamp's gain", &failed);
    tl_check(two_pass <= 0.1f, "TWO_PASS keeps the fading lamp's gain", &failed);
    tl_verdict(failed);
}

/*
 * Rapids drifting slowly through the default window: a blob of foam keeps
 * its gain from one frame to the next (its judgment does not follow the
 * cell grid's phase)
 */
static void test_rapids(void)
{
    const size_t frame_bytes = (size_t)TL_EW * TL_EH * 3u;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t info;
    hdrg_params_t params;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, got, t, i, j, y, pairs, jumps, failed;
    uint16_t *out;
    uint8_t *frames;
    size_t a, b;
    char line[256];
    double foam;
    float cx, cy, r, xa, xb, change, worst;

    frames = (uint8_t *)malloc(frame_bytes * TL_FRAMES);
    out = (uint16_t *)malloc(frame_bytes * 2u * TL_FRAMES);
    TEST_ASSERT_NOT_NULL(frames);
    TEST_ASSERT_NOT_NULL(out);

    for (sent = 0; sent < TL_FRAMES; sent++) {
        tl_rapids(frames + frame_bytes * sent, sent);
    }

    tl_params(&params, HDRG_PRESET_PHOTO, HDRG_EFFORT_REALTIME);
    tl_config(&config, true);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    session = tl_session(ctx, TL_EW, TL_EH, &params, &config);

    memset(&in, 0, sizeof(in));
    in.strides[0] = (size_t)TL_EW * 3u;
    memset(&info, 0, sizeof(info));
    info.strides[0] = (size_t)TL_EW * 6u;
    sent = 0;
    got = 0;

    for (;;) {
        info.planes[0] =
            (uint8_t *)(out + frame_bytes * ((got < TL_FRAMES) ? got : TL_FRAMES - 1u));
        status = hdrg_session_receive_frame(session, &info);
        if (status == HDRG_ERR_AGAIN) {
            in.planes[0] = frames + frame_bytes * sent;
            in.pts = (int64_t)sent;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TL_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status != HDRG_OK) {
            break;
        }

        got++;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_EOF, status);
    TEST_ASSERT_EQUAL_UINT32(TL_FRAMES, got);

    /* each blob whose core covers its center pixel, at that pixel in two frames in a row */
    pairs = 0;
    jumps = 0;
    foam = 0.0;
    worst = 0.0f;

    for (t = 1; t < TL_FRAMES; t++) {
        for (j = 0; (float)j * TL_FOAM < (float)TL_EH; j++) {
            for (i = 0; (float)i * TL_FOAM < (float)TL_EW + TL_DRIFT * (float)TL_FRAMES; i++) {
                xa = 0.0f;
                xb = 0.0f;

                if (tl_blob(i, j, &cx, &cy, &r)) {
                    xa = cx - TL_DRIFT * (float)(t - 1u);
                    xb = cx - TL_DRIFT * (float)t;
                }

                if (r < 2.0f || xb < 8.0f || xa >= (float)TL_EW - 8.0f || cy < 8.0f ||
                    cy >= (float)TL_EH - 8.0f) {
                    continue;
                }

                y = (uint32_t)cy;
                a = (size_t)y * TL_EW + (uint32_t)xa;
                b = (size_t)y * TL_EW + (uint32_t)xb;

                if (!tl_is(frames + frame_bytes * (t - 1u), a, tl_foam, 3.0f) ||
                    !tl_is(frames + frame_bytes * t, b, tl_foam, 3.0f)) {
                    continue;
                }

                change = fabsf(
                    tl_gain(frames + frame_bytes * t, out + frame_bytes * t, b) -
                    tl_gain(frames + frame_bytes * (t - 1u), out + frame_bytes * (t - 1u), a));
                worst = fmaxf(worst, change);
                jumps += (change > 0.2f) ? 1u : 0u;
                foam += (double)tl_gain(frames + frame_bytes * t, out + frame_bytes * t, b);
                pairs++;
            }
        }
    }

    TEST_ASSERT_TRUE(pairs > 100u);

    snprintf(line, sizeof(line),
             "rapids: %u pairs of a blob's frames, %.1f%% change by more than 0.2 stop "
             "(the largest %.2f); the foam +%.2f stops",
             (unsigned)pairs, 100.0 * (double)jumps / (double)pairs, (double)worst,
             foam / (double)pairs);
    TEST_MESSAGE(line);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(frames);
    free(out);

    failed = 0;
    tl_check((float)jumps <= 0.02f * (float)pairs, "the foam keeps its gain as the rapids drift",
             &failed);
    tl_verdict(failed);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_photo);
    RUN_TEST(test_anime);
    RUN_TEST(test_game);
    RUN_TEST(test_photo_quality);
    RUN_TEST(test_anime_quality);
    RUN_TEST(test_game_quality);
    RUN_TEST(test_colored);
    RUN_TEST(test_edge_entry);
    RUN_TEST(test_light_flip);
    RUN_TEST(test_light_offline);
    RUN_TEST(test_light_blink);
    RUN_TEST(test_light_fade);
    RUN_TEST(test_rapids);

    return UNITY_END();
}
