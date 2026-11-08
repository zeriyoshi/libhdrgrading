/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "image.h"

#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "exec.h"

typedef struct {
    const float *src;
    float *dst;
    uint32_t width, height, radius;
} hdrg_box_job_t;

/* per-channel EOTF tables and the luminance coefficients of a color */
static hdrg_status_t hdrg_source_tables(const hdrg_allocator_t *alloc, const hdrg_color_t *color,
                                        uint32_t entries, hdrg_source_t *out)
{
    const float scale = 1.0f / (float)(entries - 1u);
    uint32_t i;
    uint8_t c;
    float *lut;

    memset(out, 0, sizeof(*out));

    out->lut = (float *)hdrg_mem_alloc(alloc, (size_t)entries * 3u * sizeof(float));
    if (out->lut == NULL) {
        return HDRG_ERR_NOMEM;
    }

    for (c = 0; c < 3; c++) {
        lut = out->lut + (size_t)c * entries;

        /* the sRGB curve in float: the same table the analysis has always used */
        if (color->curve[c].type == HDRG_CURVE_SRGB) {
            for (i = 0; i < entries; i++) {
                lut[i] = hdrg_math_srgb_eotf((float)i * scale);
            }
        } else {
            for (i = 0; i < entries; i++) {
                lut[i] =
                    (float)hdrg_curve_eval(&color->curve[c], (double)i / (double)(entries - 1u));
            }
        }
    }

    out->entries = entries;
    hdrg_color_luma(color, out->coeffs);

    return HDRG_OK;
}

static void hdrg_box_h_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_box_job_t *job = (const hdrg_box_job_t *)arg;
    const uint32_t w = job->width, r = job->radius;
    const float *row;
    uint32_t x, y, lo, hi;
    double sum;
    float *out;

    for (y = row_begin; y < row_end; y++) {
        row = job->src + (size_t)y * w;
        out = job->dst + (size_t)y * w;
        sum = 0.0;
        hi = HDRG_MIN(r, w - 1);
        for (x = 0; x <= hi; x++) {
            sum += (double)row[x];
        }

        for (x = 0; x < w; x++) {
            lo = (x > r) ? x - r : 0;
            hi = HDRG_MIN(x + r, w - 1);
            out[x] = (float)(sum / (double)(hi - lo + 1));

            if (hi + 1 < w) {
                sum += (double)row[hi + 1];
            }

            if (x >= r) {
                sum -= (double)row[lo];
            }
        }
    }
}

static void hdrg_box_v_cols(uint32_t col_begin, uint32_t col_end, void *arg)
{
    const hdrg_box_job_t *job = (const hdrg_box_job_t *)arg;
    const uint32_t w = job->width, h = job->height, r = job->radius;
    uint32_t x, y, lo, hi;
    double sum;

    for (x = col_begin; x < col_end; x++) {
        sum = 0.0;
        hi = HDRG_MIN(r, h - 1);
        for (y = 0; y <= hi; y++) {
            sum += (double)job->src[(size_t)y * w + x];
        }

        for (y = 0; y < h; y++) {
            lo = (y > r) ? y - r : 0;
            hi = HDRG_MIN(y + r, h - 1);
            job->dst[(size_t)y * w + x] = (float)(sum / (double)(hi - lo + 1));

            if (hi + 1 < h) {
                sum += (double)job->src[(size_t)(hi + 1) * w + x];
            }

            if (y >= r) {
                sum -= (double)job->src[(size_t)lo * w + x];
            }
        }
    }
}

static void hdrg_box_blur(const hdrg_exec_t *exec, const float *src, float *tmp, float *dst,
                          uint32_t width, uint32_t height, uint32_t radius)
{
    hdrg_box_job_t job;

    job.src = src;
    job.dst = tmp;
    job.width = width;
    job.height = height;
    job.radius = radius;

    hdrg_exec_rows(exec, height, hdrg_box_h_rows, &job);

    job.src = tmp;
    job.dst = dst;

    hdrg_exec_rows(exec, width, hdrg_box_v_cols, &job);
}

extern hdrg_status_t hdrg_source_init(const hdrg_allocator_t *alloc, const hdrg_raw_image_t *raw,
                                      const hdrg_color_t *color, hdrg_source_t *out)
{
    hdrg_status_t status;

    status = hdrg_source_tables(alloc, color, (raw->bit_depth == 16) ? 65536u : 256u, out);
    if (status != HDRG_OK) {
        return status;
    }

    out->pixels = raw->pixels;
    out->stride = raw->stride;
    out->width = raw->width;
    out->height = raw->height;
    out->comps = raw->has_alpha ? 4u : 3u;
    out->wide = raw->bit_depth == 16;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_source_init_rgb16(const hdrg_allocator_t *alloc,
                                            const hdrg_color_t *color, uint32_t width,
                                            uint32_t height, hdrg_source_t *out)
{
    hdrg_status_t status;

    status = hdrg_source_tables(alloc, color, 65536u, out);
    if (status != HDRG_OK) {
        return status;
    }

    out->stride = (size_t)width * 3u * sizeof(uint16_t);
    out->width = width;
    out->height = height;
    out->comps = 3;
    out->wide = true;

    return HDRG_OK;
}

extern void hdrg_source_rgb16_row(const hdrg_source_t *src, uint32_t y, uint16_t *dst)
{
    const uint16_t *wide;
    const uint8_t *row = hdrg_source_row(src, y);
    uint32_t x;
    uint8_t c;

    wide = (const uint16_t *)(const void *)row;
    for (x = 0; x < src->width; x++) {
        for (c = 0; c < 3; c++) {
            dst[(size_t)x * 3u + c] = src->wide
                                          ? wide[(size_t)x * src->comps + c]
                                          : (uint16_t)(row[(size_t)x * src->comps + c] * 257u);
        }
    }
}

extern void hdrg_source_free(const hdrg_allocator_t *alloc, hdrg_source_t *src)
{
    if (src == NULL) {
        return;
    }

    hdrg_mem_free(alloc, src->lut);

    src->lut = NULL;
}

extern hdrg_status_t hdrg_plane_alloc(const hdrg_allocator_t *alloc, uint32_t width,
                                      uint32_t height, hdrg_plane_t *out)
{
    if (width == 0 || height == 0) {
        return HDRG_ERR_INVALID_ARG;
    }

    out->data = (float *)hdrg_mem_alloc(alloc, (size_t)width * height * sizeof(float));
    if (out->data == NULL) {
        return HDRG_ERR_NOMEM;
    }

    out->width = width;
    out->height = height;

    return HDRG_OK;
}

extern void hdrg_plane_free(const hdrg_allocator_t *alloc, hdrg_plane_t *plane)
{
    if (plane == NULL) {
        return;
    }

    hdrg_mem_free(alloc, plane->data);

    plane->data = NULL;
    plane->width = 0;
    plane->height = 0;
}

extern void hdrg_box_blur_plane(const hdrg_exec_t *exec, const float *src, float *tmp, float *dst,
                                uint32_t width, uint32_t height, uint32_t radius)
{
    hdrg_box_blur(exec, src, tmp, dst, width, height, radius);
}
