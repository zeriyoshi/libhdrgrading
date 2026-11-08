/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_DR_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HDRG_DR_ID        "hdrg.dering"
#define HDRG_DR_LICENSE   "Apache-2.0 OR MIT"
#define HDRG_DR_COPY      "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_DR_GRID      8u         /* the blocks' grid, from the frame's origin */
#define HDRG_DR_EDGE      (32 * 257) /* a block's Y' range from which it has an edge */
#define HDRG_DR_TOP       65535.0
#define HDRG_DR_RATIO     2.0f /* the ringing above which the blocks with an edge are worked on */
#define HDRG_DR_LEVEL (0.8f / 255.0f) /* and its level (a clean anti-aliased edge stays below) */
#define HDRG_DR_FLOOR 6.0             /* the range's floor over the measured ringing */
#define HDRG_DR_SHARE 4               /* its ceiling: the block's Y' range over this */
#define HDRG_DR_SLOPE 2               /* and its cap: this times the local slope */
#define HDRG_DR_STEEP 5      /* a slope over the block's Y' range over this: on the edge itself */
#define HDRG_DR_KR    13933u /* BT.709 luma weights in 1/65536 for the decisions */
#define HDRG_DR_KG    46871u
#define HDRG_DR_KB    4732u

/*
 * Deringing of the PREFILTER stage, after the deblocking:
 * the ringing (mosquito noise) a coarse quantizer leaves around edges stays
 * inside the 8 x 8 block of the edge. In each block of the grid whose Y'
 * range reaches HDRG_DR_EDGE, every sample takes the mean of the samples of
 * its window whose R, G and B all lie within range of its own: the ringing's
 * swings are averaged out, while an edge, a line, a highlight or a color edge
 * of the same luminance differ by more and stay apart. The window's radius is
 * 1 + 2 x strength (the window is cut at the frame's borders).
 *
 * The range scales with what the ringing can reach without taking in what
 * is not ringing: at least HDRG_DR_FLOOR times the measured ringing level,
 * up to the block's Y' range over HDRG_DR_SHARE (the swings next to a strong
 * line grow with it, a thin line or a cel shade boundary still differs by
 * more), and at most HDRG_DR_SLOPE times the local slope (the minmod of the
 * differences to the previous and the next sample, the largest over the
 * channels and axes: zero on the ringing's peaks and next to edges, the
 * gradient's own slope on a ramp). A steep gradient so keeps its shape: a
 * wide range would take in the ramp on one side of a line only and flatten
 * it there or round its peaks, a range as narrow as one sample's slope would
 * let the noise pick the neighbors and stripe it. A sample steeper than the
 * block's Y' range over HDRG_DR_STEEP sits on the edge itself (a ramp spreads
 * the range over the block): it keeps the floor, so the edge's anti-aliased
 * samples stay between its sides.
 *
 * There is no quantizer to read: the frame's HDRG_SD_NOISE side data
 * compares the variation away from the edge in blocks with one against the
 * blocks without one; at most HDRG_DR_RATIO of the latter, or an excess of
 * at most HDRG_DR_LEVEL, there is no ringing to remove and the input passes
 * (reported as passed through). Integer arithmetic on the 16-bit codes, each
 * output sample from the input alone, so any band split or worker count gives
 * the same output.
 */
typedef struct {
    uint32_t width, height, reach;
    int32_t floor;
    bool active;
    double strength;
} hdrg_dering_t;

/* Y' of the sample at s */
static inline int32_t hdrg_dr_luma(const uint16_t *s)
{
    return (int32_t)(((uint32_t)s[0] * HDRG_DR_KR + (uint32_t)s[1] * HDRG_DR_KG +
                      (uint32_t)s[2] * HDRG_DR_KB + 32768u) >>
                     16);
}

/* the minmod of the differences to the previous and the next sample, as a magnitude */
static inline int32_t hdrg_dr_minmod(int32_t back, int32_t ahead)
{
    if (back > 0 && ahead > 0) {
        return HDRG_DR_MIN(back, ahead);
    }

    if (back < 0 && ahead < 0) {
        return HDRG_DR_MIN(-back, -ahead);
    }

    return 0;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_dr_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_dering_t *dr;
    hdrg_status_t status;

    (void)filter;

    dr = (hdrg_dering_t *)api->mem_calloc(api, 1, sizeof(hdrg_dering_t));
    if (dr == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &dr->strength);
    if (status != HDRG_OK) {
        api->mem_free(api, dr);

        return status;
    }

    *inst = dr;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_dr_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_dr_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_dering_t *dr = (hdrg_dering_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &dr->width, &dr->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || dr->width == 0 || dr->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_dering_settings(nego, api, dr->strength, &dr->reach, &dr->floor, &dr->active);
    if (status == HDRG_OK) {
        status = api->nego_set_output(nego, dr->width, dr->height, HDRG_FORMAT_RGB16);
    }

    return (status == HDRG_OK && !dr->active) ? api->nego_set_passthrough(nego) : status;
}

/* the block rows of output rows [out_begin, out_end) and the window's rows on each side */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_dr_input_rows(void *inst, uint32_t out_begin,
                                                         uint32_t out_end, uint32_t *in_begin,
                                                         uint32_t *in_end,
                                                         const hdrg_plugin_api_t *api)
{
    const hdrg_dering_t *dr = (const hdrg_dering_t *)inst;
    uint32_t first, last;

    (void)api;

    if (out_begin >= out_end || out_end > dr->height) {
        return HDRG_ERR_INVALID_ARG;
    }

    first = out_begin / HDRG_DR_GRID * HDRG_DR_GRID;
    last = (out_end + HDRG_DR_GRID - 1u) / HDRG_DR_GRID * HDRG_DR_GRID;
    *in_begin = (first > dr->reach) ? first - dr->reach : 0u;
    *in_end = HDRG_DR_MIN(last + dr->reach, dr->height);

    return HDRG_OK;
}

/* the largest minmod slope of the sample at s (x, y) over the channels and both axes */
static int32_t hdrg_dr_slope(const hdrg_dering_t *dr, const uint16_t *s, size_t step, uint32_t x,
                             uint32_t y)
{
    const uint16_t *prev, *next;
    uint8_t c, axis;
    int32_t slope, back, ahead, g;
    bool has_prev, has_next;

    slope = 0;

    for (axis = 0; axis < 2u; axis++) {
        has_prev = (axis == 0) ? x > 0 : y > 0;
        has_next = (axis == 0) ? x + 1u < dr->width : y + 1u < dr->height;
        prev = has_prev ? s - ((axis == 0) ? 3u : step) : s;
        next = has_next ? s + ((axis == 0) ? 3u : step) : s;

        for (c = 0; c < 3u; c++) {
            back = (int32_t)s[c] - (int32_t)prev[c];
            ahead = (int32_t)next[c] - (int32_t)s[c];
            back = has_prev ? back : ahead;
            ahead = has_next ? ahead : back;
            g = hdrg_dr_minmod(back, ahead);
            slope = (g > slope) ? g : slope;
        }
    }

    return slope;
}

/*
 * output row y from the input row in (rows step samples apart), each block
 * column's Y' range of the block row in contrast (0: no edge)
 */
static void hdrg_dr_row(const hdrg_dering_t *dr, const uint16_t *in, size_t step, uint32_t y,
                        const int32_t *contrast, uint16_t *out)
{
    const uint16_t *s, *n, *line, *top;
    uint32_t x, x0, x1, nx, up, down, count, span;
    uint8_t c;
    int32_t sum[3], range, slope, ceiling;
    size_t ny;
    bool near;

    memcpy(out, in, (size_t)dr->width * 3u * sizeof(uint16_t));

    up = HDRG_DR_MIN(dr->reach, y);
    down = HDRG_DR_MIN(dr->reach, dr->height - 1u - y);
    top = in - (size_t)up * step;

    for (x = 0; x < dr->width; x++) {
        if (contrast[x / HDRG_DR_GRID] == 0) {
            continue;
        }

        s = in + (size_t)x * 3u;
        slope = hdrg_dr_slope(dr, s, step, x, y);
        ceiling = contrast[x / HDRG_DR_GRID] / HDRG_DR_SHARE;
        range = HDRG_DR_MIN(HDRG_DR_SLOPE * slope, ceiling);
        range = (range > dr->floor && slope * HDRG_DR_STEEP <= contrast[x / HDRG_DR_GRID])
                    ? range
                    : dr->floor;
        span = 2u * (uint32_t)range;
        x0 = (x > dr->reach) ? x - dr->reach : 0u;
        x1 = HDRG_DR_MIN(x + dr->reach, dr->width - 1u);
        sum[0] = 0;
        sum[1] = 0;
        sum[2] = 0;
        count = 0;

        for (ny = 0; ny <= (size_t)up + down; ny++) {
            line = top + ny * step;

            for (nx = x0; nx <= x1; nx++) {
                n = line + (size_t)nx * 3u;
                near = true;

                for (c = 0; c < 3u; c++) {
                    near = near && (uint32_t)((int32_t)n[c] - (int32_t)s[c] + range) <= span;
                }

                if (!near) {
                    continue;
                }

                for (c = 0; c < 3u; c++) {
                    sum[c] += (int32_t)n[c];
                }

                count++;
            }
        }

        for (c = 0; c < 3u; c++) {
            out[(size_t)x * 3u + c] = (uint16_t)((sum[c] + (int32_t)(count / 2u)) / (int32_t)count);
        }
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_dr_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_dering_t *dr = (const hdrg_dering_t *)inst;
    const uint16_t *src;
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, x, by, first, blocks, b;
    uint8_t *out;
    int32_t *lo, *hi, *contrast, v;
    size_t in_stride, out_stride;
    void *scratch;

    status = api->job_rows(job, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_input(job, 0, &in, &in_stride, &in_begin, &in_end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &out, &out_stride);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (begin < in_begin || end > in_end) {
        return HDRG_ERR_INTERNAL;
    }

    /* each block row's Y' range per block column, from its eight rows (0 below an edge's) */
    blocks = (dr->width + HDRG_DR_GRID - 1u) / HDRG_DR_GRID;
    status = api->job_scratch(job, (size_t)blocks * 3u * sizeof(int32_t), &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    lo = (int32_t *)scratch;

    hi = lo + blocks;
    contrast = hi + blocks;
    first = UINT32_MAX;

    for (y = begin; y < end; y++) {
        by = y / HDRG_DR_GRID * HDRG_DR_GRID;

        if (by != first) {
            first = by;

            for (b = 0; b < blocks; b++) {
                lo[b] = INT32_MAX;
                hi[b] = INT32_MIN;
            }

            for (by = first; by < HDRG_DR_MIN(first + HDRG_DR_GRID, dr->height); by++) {
                src = (const uint16_t *)(const void *)(in + (size_t)(by - in_begin) * in_stride);

                for (x = 0; x < dr->width; x++) {
                    v = hdrg_dr_luma(src + (size_t)x * 3u);
                    b = x / HDRG_DR_GRID;
                    lo[b] = (v < lo[b]) ? v : lo[b];
                    hi[b] = (v > hi[b]) ? v : hi[b];
                }
            }

            for (b = 0; b < blocks; b++) {
                contrast[b] = (hi[b] - lo[b] < HDRG_DR_EDGE) ? 0 : hi[b] - lo[b];
            }
        }

        hdrg_dr_row(dr, (const uint16_t *)(const void *)(in + (size_t)(y - in_begin) * in_stride),
                    in_stride / sizeof(uint16_t), y, contrast,
                    (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride));
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_dering_settings(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                          double strength, uint32_t *reach, int32_t *floor,
                                          bool *active)
{
    hdrg_status_t status;
    uint32_t version;
    size_t needed;
    float noise[HDRG_SD_NOISE_FLOATS];

    *reach = 1u + (uint32_t)(2.0 * strength + 0.5);
    *floor = 0;
    *active = false;
    status = api->nego_side_data(nego, HDRG_SD_NOISE, &version, noise, sizeof(noise), &needed);

    if (status == HDRG_OK && version == HDRG_SD_NOISE_VERSION && needed == sizeof(noise)) {
        *floor = (int32_t)(HDRG_DR_FLOOR * (double)noise[14] * HDRG_DR_TOP + 0.5);
        *active = noise[13] > HDRG_DR_RATIO && noise[14] > HDRG_DR_LEVEL && strength > 0.0;
    } else if (status != HDRG_OK && status != HDRG_ERR_NOT_FOUND) {
        return status;
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_dering_init(hdrg_plugin_t *plugin,
                                                              const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_DR_ID), HDRG_LIT("Deringing"),
                                  HDRG_LIT("2.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_DR_LICENSE), HDRG_LIT(HDRG_DR_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_DR_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_LIT("strength"), 1.0, 0.0, 3.0,
        HDRG_LIT("sets the window's radius, 1 + 2 x strength (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_dr_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_dr_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_dr_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_dr_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_dr_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_dering_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_dering_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_dering_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}
