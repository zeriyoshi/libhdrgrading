/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_IMAGE_H
#define HDRG_CORE_IMAGE_H

#include <stdint.h>

#include "../priv.h"
#include "color.h"
#include "exec.h"

#define HDRG_MAXRGB_WEIGHT 0.7f

/*
 * Source pixels as the caller's interleaved code values plus one EOTF lookup
 * table per channel (from the source's color). Every pass decodes the
 * pixels it needs on the fly, so no full-resolution float copy of the image
 * exists.
 */
typedef struct {
    const uint8_t *pixels; /* row row0 */
    float *lut;            /* code value -> linear light, `entries` per channel */
    size_t stride;
    uint32_t width, height;
    uint32_t row0;    /* first row pixels holds (a band of a larger image) */
    uint32_t entries; /* 256 or 65536 */
    uint8_t comps;    /* 3, or 4 with an ignored alpha */
    bool wide;        /* 16-bit samples */
    float coeffs[3];  /* luminance coefficients of the source primaries */
} hdrg_source_t;

typedef struct {
    float *data;
    uint32_t width, height;
} hdrg_plane_t;

/* bilinear sampling position on a reduced grid */
typedef struct {
    uint32_t x0, x1, y0, y1;
    float tx, ty;
} hdrg_grid_pos_t;

static inline const uint8_t *hdrg_source_row(const hdrg_source_t *src, uint32_t y)
{
    return src->pixels + (size_t)(y - src->row0) * src->stride;
}

/* code values of pixel x of a row returned by hdrg_source_row */
static inline void hdrg_source_codes(const hdrg_source_t *src, const uint8_t *row, uint32_t x,
                                     uint32_t codes[3])
{
    const uint16_t *wide;
    const size_t i = (size_t)x * src->comps;

    if (src->wide) {
        wide = (const uint16_t *)(const void *)row + i;
        codes[0] = wide[0];
        codes[1] = wide[1];
        codes[2] = wide[2];
    } else {
        codes[0] = row[i];
        codes[1] = row[i + 1];
        codes[2] = row[i + 2];
    }
}

/* linear RGB of code values */
static inline void hdrg_source_linear(const hdrg_source_t *src, const uint32_t codes[3],
                                      float rgb[3])
{
    rgb[0] = src->lut[codes[0]];
    rgb[1] = src->lut[src->entries + codes[1]];
    rgb[2] = src->lut[2u * src->entries + codes[2]];
}

/* linear RGB of pixel x of a row returned by hdrg_source_row */
static inline void hdrg_source_rgb(const hdrg_source_t *src, const uint8_t *row, uint32_t x,
                                   float rgb[3])
{
    uint32_t codes[3];

    hdrg_source_codes(src, row, x, codes);
    hdrg_source_linear(src, codes, rgb);
}

/* relative luminance Y */
static inline float hdrg_rgb_y(const float coeffs[3], const float rgb[3])
{
    return coeffs[0] * rgb[0] + coeffs[1] * rgb[1] + coeffs[2] * rgb[2];
}

/* analysis luminance La = max(Y, 0.7 * maxRGB) */
static inline float hdrg_rgb_la(float y, const float rgb[3])
{
    return HDRG_MAX(y, HDRG_MAX(rgb[0], HDRG_MAX(rgb[1], rgb[2])) * HDRG_MAXRGB_WEIGHT);
}

/*
 * Position of full-resolution pixel (x, y) on a grid of grid_w x grid_h
 * cells, pixel centers aligned; scale_x / scale_y are grid cells per pixel
 */
static inline void hdrg_grid_pos(float scale_x, float scale_y, uint32_t grid_w, uint32_t grid_h,
                                 uint32_t x, uint32_t y, hdrg_grid_pos_t *pos)
{
    float fx, fy;

    fx = HDRG_MAX(((float)x + 0.5f) * scale_x - 0.5f, 0.0f);
    fy = HDRG_MAX(((float)y + 0.5f) * scale_y - 0.5f, 0.0f);
    pos->x0 = HDRG_MIN((uint32_t)fx, grid_w - 1);
    pos->y0 = HDRG_MIN((uint32_t)fy, grid_h - 1);
    pos->x1 = HDRG_MIN(pos->x0 + 1, grid_w - 1);
    pos->y1 = HDRG_MIN(pos->y0 + 1, grid_h - 1);
    pos->tx = fx - (float)pos->x0;
    pos->ty = fy - (float)pos->y0;
}

static inline float hdrg_grid_lerp(const float *grid, uint32_t width, const hdrg_grid_pos_t *pos)
{
    return (grid[(size_t)pos->y0 * width + pos->x0] * (1.0f - pos->tx) +
            grid[(size_t)pos->y0 * width + pos->x1] * pos->tx) *
               (1.0f - pos->ty) +
           (grid[(size_t)pos->y1 * width + pos->x0] * (1.0f - pos->tx) +
            grid[(size_t)pos->y1 * width + pos->x1] * pos->tx) *
               pos->ty;
}

/* raw must be validated (8/16-bit, stride); color describes its code values */
extern hdrg_status_t hdrg_source_init(const hdrg_allocator_t *alloc, const hdrg_raw_image_t *raw,
                                      const hdrg_color_t *color, hdrg_source_t *out);
/*
 * A source over RGB16 bands (the SCALE stage's output, still in the
 * source's code values): pixels and row0 are set per band
 */
extern hdrg_status_t hdrg_source_init_rgb16(const hdrg_allocator_t *alloc,
                                            const hdrg_color_t *color, uint32_t width,
                                            uint32_t height, hdrg_source_t *out);
extern void hdrg_source_free(const hdrg_allocator_t *alloc, hdrg_source_t *src);
/* row y as RGB16 code values (8-bit samples widened as v * 257, alpha dropped) */
extern void hdrg_source_rgb16_row(const hdrg_source_t *src, uint32_t y, uint16_t *dst);

extern hdrg_status_t hdrg_plane_alloc(const hdrg_allocator_t *alloc, uint32_t width,
                                      uint32_t height, hdrg_plane_t *out);
extern void hdrg_plane_free(const hdrg_allocator_t *alloc, hdrg_plane_t *plane);

/*
 * Separable box blur src -> dst using tmp as scratch (all width x height).
 * Deterministic regardless of the worker count.
 */
extern void hdrg_box_blur_plane(const hdrg_exec_t *exec, const float *src, float *tmp, float *dst,
                                uint32_t width, uint32_t height, uint32_t radius);

#endif /* HDRG_CORE_IMAGE_H */
