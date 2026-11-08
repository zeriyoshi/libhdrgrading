/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "format.h"

#include <string.h>

#include "../core/yuv.h"
#include "../priv.h"

/* a conversion's rows */
typedef struct {
    const hdrg_format_layout_t *layout;
    const hdrg_format_frame_t *frame;
    const uint16_t *rgb; /* row y0 */
    uint16_t *out_rgb;   /* row y0 */
    size_t rgb_stride;
    uint32_t y0;
} hdrg_format_job_t;

static inline uint64_t hdrg_format_round(uint64_t v, uint64_t to)
{
    return (v + to - 1u) / to * to;
}

/* IEEE 754 binary32 -> binary16, rounded to the nearest even (v in [0, 1]) */
static inline uint16_t hdrg_half_from(float v)
{
    uint32_t bits, mant, exp, half, rest;

    memcpy(&bits, &v, sizeof(bits));
    exp = (bits >> 23) & 0xFFu;
    mant = bits & 0x7FFFFFu;

    /* a zero, or below the smallest half subnormal's half */
    if (exp < 102u) {
        return 0;
    }

    /* a subnormal half */
    if (exp < 113u) {
        mant |= 0x800000u;
        half = mant >> (126u - exp);
        rest = mant & ((1u << (126u - exp)) - 1u);

        if (rest > (1u << (125u - exp)) || (rest == (1u << (125u - exp)) && (half & 1u) != 0)) {
            half++;
        }

        return (uint16_t)half;
    }

    half = ((exp - 112u) << 10) | (mant >> 13);
    rest = mant & 0x1FFFu;

    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0)) {
        half++;
    }

    return (uint16_t)half;
}

/* binary16 -> binary32 */
static inline float hdrg_half_to(uint16_t h)
{
    uint32_t bits, exp, mant;
    float v;

    exp = ((uint32_t)h >> 10) & 0x1Fu;
    mant = (uint32_t)h & 0x3FFu;

    if (exp == 0x1Fu) {
        bits = ((uint32_t)(h & 0x8000u) << 16) | 0x7F800000u | (mant << 13);
    } else if (exp != 0) {
        bits = ((uint32_t)(h & 0x8000u) << 16) | ((exp + 112u) << 23) | (mant << 13);
    } else {
        v = (float)mant * (1.0f / 16777216.0f);

        return ((h & 0x8000u) != 0) ? -v : v;
    }

    memcpy(&v, &bits, sizeof(v));

    return v;
}

/* a half's code: clamped to [0, 1], NaN = 0 */
static inline uint16_t hdrg_half_code(uint16_t h)
{
    float v;

    v = hdrg_half_to(h);
    v = (v > 0.0f) ? v : 0.0f;
    v = (v < 1.0f) ? v : 1.0f;

    return (uint16_t)(v * 65535.0f + 0.5f);
}

/* a single float's code: clamped to [0, 1], NaN = 0 */
static inline uint16_t hdrg_float_code(float v)
{
    v = (v > 0.0f) ? v : 0.0f;
    v = (v < 1.0f) ? v : 1.0f;

    return (uint16_t)(v * 65535.0f + 0.5f);
}

static inline uint8_t hdrg_code8(uint16_t c)
{
    return (uint8_t)(((uint32_t)c * 255u + 32767u) / 65535u);
}

static inline uint32_t hdrg_code10(uint16_t c)
{
    return ((uint32_t)c * 1023u + 32767u) / 65535u;
}

static inline uint16_t hdrg_code10_back(uint32_t c)
{
    return (uint16_t)((c * 65535u + 511u) / 1023u);
}

/* RGB16 rows into the frame (a banded format) */
static void hdrg_format_pack_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_format_job_t *job = (const hdrg_format_job_t *)arg;
    const hdrg_format_layout_t *layout = job->layout;
    const uint16_t *rgb;
    uint32_t r, y, x, word;
    uint16_t *p16[3];
    uint8_t *row, *p8;
    float *p32[3];

    for (r = row_begin; r < row_end; r++) {
        y = job->y0 + r;
        rgb = (const uint16_t *)(const void *)((const uint8_t *)job->rgb +
                                               (size_t)r * job->rgb_stride);
        row = job->frame->planes[0] + (size_t)y * job->frame->strides[0];

        switch (layout->format) {
        case HDRG_FORMAT_RGB16:
            memcpy(row, rgb, (size_t)layout->width * 3u * sizeof(uint16_t));

            break;
        case HDRG_FORMAT_RGBA16:
            p16[0] = (uint16_t *)(void *)row;

            for (x = 0; x < layout->width; x++) {
                p16[0][x * 4u] = rgb[x * 3u];
                p16[0][x * 4u + 1u] = rgb[x * 3u + 1u];
                p16[0][x * 4u + 2u] = rgb[x * 3u + 2u];
                p16[0][x * 4u + 3u] = 65535u;
            }

            break;
        case HDRG_FORMAT_RGB16_PLANAR:
        case HDRG_FORMAT_RGBF16_PLANAR:
            for (x = 0; x < 3u; x++) {
                p16[x] = (uint16_t *)(void *)(job->frame->planes[x] +
                                              (size_t)y * job->frame->strides[x]);
            }

            for (x = 0; x < layout->width; x++) {
                if (layout->format == HDRG_FORMAT_RGB16_PLANAR) {
                    p16[0][x] = rgb[x * 3u];
                    p16[1][x] = rgb[x * 3u + 1u];
                    p16[2][x] = rgb[x * 3u + 2u];
                } else {
                    p16[0][x] = hdrg_half_from((float)rgb[x * 3u] / 65535.0f);
                    p16[1][x] = hdrg_half_from((float)rgb[x * 3u + 1u] / 65535.0f);
                    p16[2][x] = hdrg_half_from((float)rgb[x * 3u + 2u] / 65535.0f);
                }
            }

            break;
        case HDRG_FORMAT_RGBF32_PLANAR:
            for (x = 0; x < 3u; x++) {
                p32[x] =
                    (float *)(void *)(job->frame->planes[x] + (size_t)y * job->frame->strides[x]);
            }

            for (x = 0; x < layout->width; x++) {
                p32[0][x] = (float)rgb[x * 3u] / 65535.0f;
                p32[1][x] = (float)rgb[x * 3u + 1u] / 65535.0f;
                p32[2][x] = (float)rgb[x * 3u + 2u] / 65535.0f;
            }

            break;
        case HDRG_FORMAT_RGBA8:
            p8 = row;

            for (x = 0; x < layout->width; x++) {
                p8[x * 4u] = hdrg_code8(rgb[x * 3u]);
                p8[x * 4u + 1u] = hdrg_code8(rgb[x * 3u + 1u]);
                p8[x * 4u + 2u] = hdrg_code8(rgb[x * 3u + 2u]);
                p8[x * 4u + 3u] = 255u;
            }

            break;
        default:
            for (x = 0; x < layout->width; x++) {
                word = hdrg_code10(rgb[x * 3u]) | (hdrg_code10(rgb[x * 3u + 1u]) << 10) |
                       (hdrg_code10(rgb[x * 3u + 2u]) << 20) | (UINT32_C(3) << 30);
                memcpy(row + (size_t)x * 4u, &word, sizeof(word));
            }

            break;
        }
    }
}

/* ... and back */
static void hdrg_format_unpack_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_format_job_t *job = (const hdrg_format_job_t *)arg;
    const hdrg_format_layout_t *layout = job->layout;
    const uint16_t *p16[3];
    const uint8_t *row;
    const float *p32[3];
    uint32_t r, y, x, word;
    uint16_t *rgb;

    for (r = row_begin; r < row_end; r++) {
        y = job->y0 + r;
        rgb = (uint16_t *)(void *)((uint8_t *)job->out_rgb + (size_t)r * job->rgb_stride);
        row = job->frame->planes[0] + (size_t)y * job->frame->strides[0];

        switch (layout->format) {
        case HDRG_FORMAT_RGB16:
            memcpy(rgb, row, (size_t)layout->width * 3u * sizeof(uint16_t));

            break;
        case HDRG_FORMAT_RGBA16:
            p16[0] = (const uint16_t *)(const void *)row;

            for (x = 0; x < layout->width; x++) {
                rgb[x * 3u] = p16[0][x * 4u];
                rgb[x * 3u + 1u] = p16[0][x * 4u + 1u];
                rgb[x * 3u + 2u] = p16[0][x * 4u + 2u];
            }

            break;
        case HDRG_FORMAT_RGB16_PLANAR:
        case HDRG_FORMAT_RGBF16_PLANAR:
            for (x = 0; x < 3u; x++) {
                p16[x] = (const uint16_t *)(const void *)(job->frame->planes[x] +
                                                          (size_t)y * job->frame->strides[x]);
            }

            for (x = 0; x < layout->width; x++) {
                if (layout->format == HDRG_FORMAT_RGB16_PLANAR) {
                    rgb[x * 3u] = p16[0][x];
                    rgb[x * 3u + 1u] = p16[1][x];
                    rgb[x * 3u + 2u] = p16[2][x];
                } else {
                    rgb[x * 3u] = hdrg_half_code(p16[0][x]);
                    rgb[x * 3u + 1u] = hdrg_half_code(p16[1][x]);
                    rgb[x * 3u + 2u] = hdrg_half_code(p16[2][x]);
                }
            }

            break;
        case HDRG_FORMAT_RGBF32_PLANAR:
            for (x = 0; x < 3u; x++) {
                p32[x] = (const float *)(const void *)(job->frame->planes[x] +
                                                       (size_t)y * job->frame->strides[x]);
            }

            for (x = 0; x < layout->width; x++) {
                rgb[x * 3u] = hdrg_float_code(p32[0][x]);
                rgb[x * 3u + 1u] = hdrg_float_code(p32[1][x]);
                rgb[x * 3u + 2u] = hdrg_float_code(p32[2][x]);
            }

            break;
        case HDRG_FORMAT_RGBA8:
            for (x = 0; x < layout->width; x++) {
                rgb[x * 3u] = (uint16_t)(row[x * 4u] * 257u);
                rgb[x * 3u + 1u] = (uint16_t)(row[x * 4u + 1u] * 257u);
                rgb[x * 3u + 2u] = (uint16_t)(row[x * 4u + 2u] * 257u);
            }

            break;
        default:
            for (x = 0; x < layout->width; x++) {
                memcpy(&word, row + (size_t)x * 4u, sizeof(word));
                rgb[x * 3u] = hdrg_code10_back(word & 0x3FFu);
                rgb[x * 3u + 1u] = hdrg_code10_back((word >> 10) & 0x3FFu);
                rgb[x * 3u + 2u] = hdrg_code10_back((word >> 20) & 0x3FFu);
            }

            break;
        }
    }
}

/* the 4:2:0 picture of a P016 frame: Y' in place, Cb / Cr apart in the scratch */
static void hdrg_format_picture(const hdrg_format_layout_t *layout, const hdrg_format_yuv_t *yuv,
                                const hdrg_format_frame_t *frame, uint16_t *chroma, hdrg_yuv_t *out)
{
    const size_t cw = ((size_t)layout->width + 1u) / 2u, ch = ((size_t)layout->height + 1u) / 2u;

    memset(out, 0, sizeof(*out));
    out->planes[0] = (frame != NULL) ? frame->planes[0] : NULL;
    out->planes[1] = chroma;
    out->planes[2] = (chroma != NULL) ? chroma + cw * ch : NULL;
    out->strides[0] = (frame != NULL) ? frame->strides[0] : (size_t)layout->width * 2u;
    out->strides[1] = cw * sizeof(uint16_t);
    out->strides[2] = cw * sizeof(uint16_t);
    out->width = layout->width;
    out->height = layout->height;
    out->layout = HDRG_YUV_420;
    out->matrix = (yuv->matrix == 5u) ? HDRG_MATRIX_BT601 : yuv->matrix;
    out->bits = 16;
    out->siting = HDRG_CHROMA_LEFT;
    out->fit = HDRG_CHROMA_FIT_LINEAR;
    out->full_range = yuv->full_range;
}

extern void hdrg_format_view(const hdrg_format_layout_t *layout, uint8_t *base,
                             hdrg_format_frame_t *out)
{
    uint8_t p;

    memset(out, 0, sizeof(*out));

    for (p = 0; p < layout->planes; p++) {
        out->planes[p] = base + layout->offsets[p];
        out->strides[p] = layout->strides[p];
    }
}

extern size_t hdrg_format_row_bytes(const hdrg_format_layout_t *layout, uint8_t plane)
{
    hdrg_format_layout_t packed;

    (void)hdrg_format_layout(layout->format, layout->width, 1, false, &packed);

    return packed.strides[plane];
}

extern bool hdrg_format_known(uint32_t format)
{
    return format >= HDRG_FORMAT_RGB16 && format <= HDRG_FORMAT_RGBF32_PLANAR;
}

extern bool hdrg_format_banded(uint32_t format)
{
    return hdrg_format_known(format) && format != HDRG_FORMAT_P016;
}

extern bool hdrg_format_layout(uint32_t format, uint32_t width, uint32_t height, bool aligned,
                               hdrg_format_layout_t *out)
{
    const uint64_t align = aligned ? HDRG_FORMAT_ALIGN : 1u;
    uint64_t row, rows, bytes;
    uint8_t p;

    memset(out, 0, sizeof(*out));

    if (!hdrg_format_known(format) || width == 0 || height == 0 || width > HDRG_MAX_DIMENSION ||
        height > HDRG_MAX_DIMENSION) {
        return false;
    }

    out->format = format;
    out->width = width;
    out->height = height;
    out->planes = hdrg_format_planes(format);
    bytes = 0;

    for (p = 0; p < out->planes; p++) {
        switch (format) {
        case HDRG_FORMAT_RGB16:
            row = (uint64_t)width * 6u;

            break;
        case HDRG_FORMAT_RGBA16:
            row = (uint64_t)width * 8u;

            break;
        case HDRG_FORMAT_P016:
            row = (p == 0) ? (uint64_t)width * 2u : ((uint64_t)width + 1u) / 2u * 4u;

            break;
        case HDRG_FORMAT_RGBA8:
        case HDRG_FORMAT_RGB10A2:
        case HDRG_FORMAT_RGBF32_PLANAR:
            row = (uint64_t)width * 4u;

            break;
        default:
            row = (uint64_t)width * 2u;

            break;
        }

        rows = (format == HDRG_FORMAT_P016 && p == 1u) ? ((uint64_t)height + 1u) / 2u : height;
        row = hdrg_format_round(row, align);
        out->strides[p] = (size_t)row;
        out->offsets[p] = (size_t)bytes;
        bytes += hdrg_format_round(row * rows, align);
    }

    bytes = aligned ? hdrg_format_round(bytes, HDRG_FORMAT_PAGE) : bytes;

    /* where size_t is 32 bits, a frame past it (and the page its block is aligned with) has none */
    if (bytes > SIZE_MAX - HDRG_FORMAT_PAGE) {
        memset(out, 0, sizeof(*out));

        return false;
    }

    out->bytes = (size_t)bytes;

    return true;
}

extern size_t hdrg_format_sample_bytes(uint32_t format)
{
    return (format == HDRG_FORMAT_RGBA8)
               ? 1u
               : ((format == HDRG_FORMAT_RGB10A2 || format == HDRG_FORMAT_RGBF32_PLANAR) ? 4u : 2u);
}

extern uint8_t hdrg_format_planes(uint32_t format)
{
    return (format == HDRG_FORMAT_RGB16_PLANAR || format == HDRG_FORMAT_RGBF16_PLANAR ||
            format == HDRG_FORMAT_RGBF32_PLANAR)
               ? 3u
               : ((format == HDRG_FORMAT_P016) ? 2u : 1u);
}

extern uint32_t hdrg_format_plane_row(uint32_t format, uint8_t plane, uint32_t y)
{
    return (format == HDRG_FORMAT_P016 && plane == 1u) ? y / 2u : y;
}

extern size_t hdrg_format_scratch(const hdrg_format_layout_t *layout)
{
    hdrg_format_yuv_t yuv;
    hdrg_yuv_t picture;
    size_t chroma;

    if (layout->format != HDRG_FORMAT_P016) {
        return 0;
    }

    yuv.matrix = HDRG_MATRIX_BT709;
    yuv.full_range = false;
    chroma = ((size_t)layout->width + 1u) / 2u * (((size_t)layout->height + 1u) / 2u);
    hdrg_format_picture(layout, &yuv, NULL, NULL, &picture);

    return chroma * 2u * sizeof(uint16_t) + hdrg_yuv_scratch(&picture) * sizeof(float);
}

extern void hdrg_format_pack(const hdrg_exec_t *exec, const hdrg_format_layout_t *layout,
                             const hdrg_format_yuv_t *yuv, const uint16_t *rgb, size_t rgb_stride,
                             uint32_t y0, uint32_t rows, const hdrg_format_frame_t *frame,
                             void *scratch)
{
    const size_t cw = ((size_t)layout->width + 1u) / 2u, ch = ((size_t)layout->height + 1u) / 2u;
    hdrg_format_job_t job;
    hdrg_yuv_t picture;
    uint16_t *chroma, *pair;
    size_t x, y;

    if (layout->format != HDRG_FORMAT_P016) {
        memset(&job, 0, sizeof(job));
        job.layout = layout;
        job.frame = frame;
        job.rgb = rgb;
        job.rgb_stride = rgb_stride;
        job.y0 = y0;

        hdrg_exec_rows(exec, rows, hdrg_format_pack_rows, &job);

        return;
    }

    /* the whole frame: RGB16 rows tight, as the 4:2:0 conversion reads them */
    chroma = (uint16_t *)scratch;
    hdrg_format_picture(layout, yuv, frame, chroma, &picture);
    hdrg_rgb16_to_yuv420(exec, rgb, 0, layout->height, &picture);

    for (y = 0; y < ch; y++) {
        pair = (uint16_t *)(void *)(frame->planes[1] + y * frame->strides[1]);

        for (x = 0; x < cw; x++) {
            pair[x * 2u] = chroma[y * cw + x];
            pair[x * 2u + 1u] = chroma[cw * ch + y * cw + x];
        }
    }
}

extern void hdrg_format_unpack(const hdrg_exec_t *exec, const hdrg_format_layout_t *layout,
                               const hdrg_format_yuv_t *yuv, const hdrg_format_frame_t *frame,
                               uint32_t y0, uint32_t rows, uint16_t *rgb, size_t rgb_stride,
                               void *scratch)
{
    const uint16_t *pair;
    const size_t cw = ((size_t)layout->width + 1u) / 2u, ch = ((size_t)layout->height + 1u) / 2u;
    hdrg_format_job_t job;
    hdrg_yuv_t picture;
    uint16_t *chroma;
    size_t x, y;

    if (layout->format != HDRG_FORMAT_P016) {
        memset(&job, 0, sizeof(job));
        job.layout = layout;
        job.frame = frame;
        job.out_rgb = rgb;
        job.rgb_stride = rgb_stride;
        job.y0 = y0;

        hdrg_exec_rows(exec, rows, hdrg_format_unpack_rows, &job);

        return;
    }

    chroma = (uint16_t *)scratch;

    for (y = 0; y < ch; y++) {
        pair = (const uint16_t *)(const void *)(frame->planes[1] + y * frame->strides[1]);

        for (x = 0; x < cw; x++) {
            chroma[y * cw + x] = pair[x * 2u];
            chroma[cw * ch + y * cw + x] = pair[x * 2u + 1u];
        }
    }

    hdrg_format_picture(layout, yuv, frame, chroma, &picture);
    hdrg_yuv_to_rgb16(exec, &picture, (float *)(void *)(chroma + cw * ch * 2u), rgb, rgb_stride);
}
