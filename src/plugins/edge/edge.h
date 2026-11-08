/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_EDGE_H
#define HDRG_PLUGINS_EDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * hdrg.scale.edge (the library's edge-adaptive upscale, scale_edge.c, and
 * its GPU versions): at each output pixel's place in the source,
 *
 * - the luma's structure tensor (Sobel gradients of the four source pixels
 *   around the place, weighted bilinearly) gives the edge's direction and
 *   its coherence c (0 flat or a corner, 1 a straight edge);
 * - a 3-lobe kernel (Lanczos-3's shape as a polynomial of x^2, no sine)
 *   over the 6 x 6 source pixels around, in the frame of the edge: along it
 *   stretched by 1 + stretch c (smoother lines), across it narrowed by
 *   1 + sharpen c (steeper edges; narrowing much more aliases);
 * - the result held within the four nearest pixels' range (no ringing: the
 *   HDR boost would show the overshoot brighter).
 *
 * 32-bit floats; the luma in integers (the CPU's and the GPU's alike); the
 * edges clamp; upscaling only
 */
#define HDRG_EDGE_TAPS    6u
#define HDRG_EDGE_FLAT    (1.0f / 4096.0f) /* the tensor's trace below which the place is flat */
#define HDRG_EDGE_NORMAL  1e-12f           /* a direction's squared length below this: none */
#define HDRG_EDGE_GATE    2.0f /* the tensor's trace at which the coherence counts half */
#define HDRG_EDGE_STRETCH 1.0  /* the options' defaults (the CPU's and the GPU's classes) */
#define HDRG_EDGE_SHARPEN 0.25

typedef struct {
    float scale_x, scale_y; /* output pixel centre to source: (x + 0.5) scale - 0.5 */
    float stretch, sharpen;
} hdrg_edge_config_t;

static inline float hdrg_edge_min(float a, float b)
{
    return (b < a) ? b : a;
}

static inline float hdrg_edge_max(float a, float b)
{
    return (b > a) ? b : a;
}

/* the luma from 16-bit codes in integers (BT.709 weights), one rounding */
static inline float hdrg_edge_luma(uint32_t r, uint32_t g, uint32_t b)
{
    return (float)(2126u * r + 7152u * g + 722u * b) * (1.0f / 655350000.0f);
}

/*
 * the 3-lobe kernel at x^2: Lanczos-3's zeros (1, 4 and 9) times a quartic
 * fitted to the rest (within 0.005 of Lanczos-3), 0 from 9 on
 */
static inline float hdrg_edge_kernel(float t)
{
    float g;

    if (t >= 9.0f) {
        return 0.0f;
    }

    g = 0.99495652f +
        t * (-0.448686746f + t * (0.0832075298f + t * (-0.00742809153f + t * 0.000262519738f)));

    return (1.0f - t) * (1.0f - 0.25f * t) * (1.0f - t / 9.0f) * g;
}

static inline float hdrg_edge_sqrt(float v)
{
    uint32_t bits;
    uint8_t i;
    float r;

    if (!(v > 0.0f)) {
        return 0.0f;
    }

    /* Newton's iterations from the exponent's half: exact basic operations only */
    memcpy(&bits, &v, sizeof(bits));
    bits = (bits >> 1) + UINT32_C(0x1fbd1df5);
    memcpy(&r, &bits, sizeof(r));

    for (i = 0; i < 4u; i++) {
        r = 0.5f * (r + v / r);
    }

    return r;
}

/*
 * the edge frame of the luma l (the 6 x 6 taps' luma) at (fx, fy) past tap
 * (2, 2): the gradient's unit direction (gx, gy) and the coherence. The
 * structure tensor of the Sobel gradients of the inner 4 x 4 taps, each
 * weighted by (1 - d^2 / 4)^2 of its distance d from the place; the
 * coherence (the eigenvalues' difference over their sum) squared and
 * gated by the tensor's strength (texture's weak gradients do not count)
 */
static inline void hdrg_edge_frame(float l[HDRG_EDGE_TAPS][HDRG_EDGE_TAPS], float fx, float fy,
                                   float *gx, float *gy, float *coherence)
{
    uint8_t i, j;
    float w, jxx, jxy, jyy, dx, dy, d2, trace, diff, root, nx, ny, len, c;

    jxx = 0.0f;
    jxy = 0.0f;
    jyy = 0.0f;

    for (i = 1; i < HDRG_EDGE_TAPS - 1u; i++) {
        for (j = 1; j < HDRG_EDGE_TAPS - 1u; j++) {
            dx = (float)j - 2.0f - fx;
            dy = (float)i - 2.0f - fy;
            d2 = dx * dx + dy * dy;

            if (!(d2 < 4.0f)) {
                continue;
            }

            w = 1.0f - 0.25f * d2;
            w *= w;

            /* Sobel's at tap (i, j) */
            dx = (l[i - 1u][j + 1u] + 2.0f * l[i][j + 1u] + l[i + 1u][j + 1u]) -
                 (l[i - 1u][j - 1u] + 2.0f * l[i][j - 1u] + l[i + 1u][j - 1u]);
            dy = (l[i + 1u][j - 1u] + 2.0f * l[i + 1u][j] + l[i + 1u][j + 1u]) -
                 (l[i - 1u][j - 1u] + 2.0f * l[i - 1u][j] + l[i - 1u][j + 1u]);
            jxx += w * (dx * dx);
            jxy += w * (dx * dy);
            jyy += w * (dy * dy);
        }
    }

    trace = jxx + jyy;
    diff = jxx - jyy;
    root = hdrg_edge_sqrt(diff * diff + 4.0f * jxy * jxy);
    *gx = 1.0f;
    *gy = 0.0f;
    *coherence = 0.0f;

    if (!(trace > HDRG_EDGE_FLAT * HDRG_EDGE_FLAT)) {
        return;
    }

    /* the larger eigenvalue's vector: the gradient's direction */
    nx = diff + root;
    ny = 2.0f * jxy;
    len = nx * nx + ny * ny;

    if (!(len > HDRG_EDGE_NORMAL)) {
        nx = -ny;
        ny = diff - root;
        len = nx * nx + ny * ny;
    }

    if (!(len > HDRG_EDGE_NORMAL)) {
        return;
    }

    len = hdrg_edge_sqrt(len);
    *gx = nx / len;
    *gy = ny / len;
    c = hdrg_edge_min(root / trace, 1.0f);
    *coherence = c * c * (trace / (trace + HDRG_EDGE_GATE));
}

/*
 * the output pixel of the taps (6 x 6 RGB from two rows and columns before
 * the pixel at or before the place) and their luma, at (fx, fy) past tap
 * (2, 2)
 */
static inline void hdrg_edge_pixel(float taps[HDRG_EDGE_TAPS][HDRG_EDGE_TAPS][3],
                                   float l[HDRG_EDGE_TAPS][HDRG_EDGE_TAPS], float fx, float fy,
                                   const hdrg_edge_config_t *c, float out[3])
{
    uint8_t i, j, k;
    float gx, gy, coherence, along, across, sum[3], weight, dx, dy, a, b, w, lo, hi;

    hdrg_edge_frame(l, fx, fy, &gx, &gy, &coherence);
    along = 1.0f / (1.0f + c->stretch * coherence);
    across = 1.0f + c->sharpen * coherence;
    sum[0] = 0.0f;
    sum[1] = 0.0f;
    sum[2] = 0.0f;
    weight = 0.0f;

    for (i = 0; i < HDRG_EDGE_TAPS; i++) {
        for (j = 0; j < HDRG_EDGE_TAPS; j++) {
            dx = (float)j - 2.0f - fx;
            dy = (float)i - 2.0f - fy;
            a = (dx * -gy + dy * gx) * along;
            b = (dx * gx + dy * gy) * across;
            w = hdrg_edge_kernel(a * a) * hdrg_edge_kernel(b * b);

            for (k = 0; k < 3u; k++) {
                sum[k] += taps[i][j][k] * w;
            }

            weight += w;
        }
    }

    for (k = 0; k < 3u; k++) {
        lo = hdrg_edge_min(hdrg_edge_min(taps[2][2][k], taps[2][3][k]),
                           hdrg_edge_min(taps[3][2][k], taps[3][3][k]));
        hi = hdrg_edge_max(hdrg_edge_max(taps[2][2][k], taps[2][3][k]),
                           hdrg_edge_max(taps[3][2][k], taps[3][3][k]));
        out[k] =
            (weight > 0.0f) ? hdrg_edge_min(hi, hdrg_edge_max(lo, sum[k] / weight)) : taps[2][2][k];
    }
}

#endif /* HDRG_PLUGINS_EDGE_H */
