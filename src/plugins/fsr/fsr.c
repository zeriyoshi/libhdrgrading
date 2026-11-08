/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * hdrg.scale.fsr1 ports AMD FidelityFX Super Resolution 1.0.2 (MIT; its
 * notice is in fsr.h).
 */

#include <math.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "parts.h"
#include "fsr.h"

#define HDRG_LIT(s)      s, sizeof(s) - 1u
#define HDRG_FSR_PLUGIN  "hdrg.scale.fsr"
#define HDRG_FSR_ID      "hdrg.scale.fsr1"
#define HDRG_FSR_LICENSE "MIT"
#define HDRG_FSR_COPY "Copyright (c) 2021 Advanced Micro Devices, Inc.; Copyright (c) 2026 Go Kudo"
#define HDRG_FSR_UNIT (1.0f / 65535.0f)
#define HDRG_FSR_SHARPNESS 0.2 /* stops: FSR's default */

/*
 * FSR 1 on RGB16 code values (fsr.h): EASU upscales, RCAS sharpens its
 * output when sharpen is set. Upscaling only (a smaller output is not
 * FSR's: HDRG_ERR_UNSUPPORTED; the same size is copied). Bands of
 * output rows; the EASU rows a band's RCAS reads are its own and one more
 * each side, so any band split gives the same output
 */
typedef struct {
    uint32_t in_w, in_h, out_w, out_h;
    bool sharpen, same; /* same: the input's size, copied */
    float scale[2], offset[2], sharpness;
} hdrg_fsr1_t;

static inline int64_t hdrg_fsr1_clamp(int64_t v, int64_t hi)
{
    return (v < 0) ? 0 : (v > hi) ? hi : v;
}

/* the input row at or before output row y's position */
static inline int64_t hdrg_fsr1_row(const hdrg_fsr1_t *fsr, uint32_t y)
{
    return (int64_t)floorf((float)y * fsr->scale[1] + fsr->offset[1]);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_fsr1_create(hdrg_filter_t *filter,
                                                       const hdrg_options_t *options, void **inst,
                                                       const hdrg_plugin_api_t *api)
{
    hdrg_fsr1_t *fsr;
    hdrg_status_t status;
    int64_t sharpen;
    double stops;

    (void)filter;

    fsr = (hdrg_fsr1_t *)api->mem_calloc(api, 1, sizeof(hdrg_fsr1_t));
    if (fsr == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_int(options, HDRG_LIT("sharpen"), &sharpen);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_LIT("sharpness"), &stops);
    }

    if (status != HDRG_OK) {
        api->mem_free(api, fsr);

        return status;
    }

    /* FsrRcasCon */
    fsr->sharpen = sharpen != 0;
    fsr->sharpness = hdrg_math_exp2f(-(float)stops);
    *inst = fsr;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_fsr1_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_fsr1_configure(void *inst, hdrg_negotiation_t *nego,
                                                          const hdrg_plugin_api_t *api)
{
    hdrg_fsr1_t *fsr = (hdrg_fsr1_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &fsr->in_w, &fsr->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &fsr->out_w, &fsr->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || fsr->in_w == 0 || fsr->in_h == 0 || fsr->out_w < fsr->in_w ||
        fsr->out_h < fsr->in_h) {
        return HDRG_ERR_UNSUPPORTED;
    }

    fsr->same = fsr->out_w == fsr->in_w && fsr->out_h == fsr->in_h;
    hdrg_fsr_easu_con(fsr->in_w, fsr->out_w, &fsr->scale[0], &fsr->offset[0]);
    hdrg_fsr_easu_con(fsr->in_h, fsr->out_h, &fsr->scale[1], &fsr->offset[1]);

    return api->nego_set_output(nego, fsr->out_w, fsr->out_h, HDRG_FORMAT_RGB16);
}

/* the EASU rows of output rows [begin, end): one more each side with RCAS */
static void hdrg_fsr1_easu_rows(const hdrg_fsr1_t *fsr, uint32_t begin, uint32_t end,
                                uint32_t *first, uint32_t *last)
{
    *first = (fsr->sharpen && begin > 0) ? begin - 1u : begin;
    *last = (fsr->sharpen && end < fsr->out_h) ? end + 1u : end;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_fsr1_input_rows(void *inst, uint32_t out_begin,
                                                           uint32_t out_end, uint32_t *in_begin,
                                                           uint32_t *in_end,
                                                           const hdrg_plugin_api_t *api)
{
    const hdrg_fsr1_t *fsr = (const hdrg_fsr1_t *)inst;
    uint32_t first, last;

    (void)api;

    if (out_begin >= out_end || out_end > fsr->out_h) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (fsr->same) {
        *in_begin = out_begin;
        *in_end = out_end;

        return HDRG_OK;
    }

    /* EASU reads the rows from one before its position to two after */
    hdrg_fsr1_easu_rows(fsr, out_begin, out_end, &first, &last);
    *in_begin = (uint32_t)hdrg_fsr1_clamp(hdrg_fsr1_row(fsr, first) - 1, fsr->in_h - 1u);
    *in_end = (uint32_t)hdrg_fsr1_clamp(hdrg_fsr1_row(fsr, last - 1u) + 2, fsr->in_h - 1u) + 1u;

    return HDRG_OK;
}

/* EASU's output row y into row (R, G, B floats) from the input rows from in_begin */
static void hdrg_fsr1_easu_row(const hdrg_fsr1_t *fsr, const uint8_t *in, size_t stride,
                               uint32_t in_begin, uint32_t y, float *row)
{
    const uint16_t *src;
    uint32_t x;
    uint8_t t, c;
    int64_t fx, fy, sx, sy;
    float taps[12][3], px, py;

    py = (float)y * fsr->scale[1] + fsr->offset[1];
    fy = (int64_t)floorf(py);
    py -= (float)fy;

    for (x = 0; x < fsr->out_w; x++) {
        px = (float)x * fsr->scale[0] + fsr->offset[0];
        fx = (int64_t)floorf(px);
        px -= (float)fx;

        for (t = 0; t < 12u; t++) {
            sx = hdrg_fsr1_clamp(fx + hdrg_fsr_taps[t][0], fsr->in_w - 1u);
            sy = hdrg_fsr1_clamp(fy + hdrg_fsr_taps[t][1], fsr->in_h - 1u);
            src = (const uint16_t *)(const void *)(in + (size_t)(sy - in_begin) * stride) +
                  (size_t)sx * 3u;

            for (c = 0; c < 3u; c++) {
                taps[t][c] = (float)src[c] * HDRG_FSR_UNIT;
            }
        }

        hdrg_fsr_easu(taps, px, py, row + (size_t)x * 3u);
    }
}

/* floats in [0, 1] into codes */
static void hdrg_fsr1_pack(const float *row, uint32_t width, uint16_t *dst)
{
    size_t i;

    for (i = 0; i < (size_t)width * 3u; i++) {
        dst[i] = (uint16_t)(hdrg_fsr_sat(row[i]) * 65535.0f + 0.5f);
    }
}

/* RCAS over the EASU rows (from first) into output row y */
static void hdrg_fsr1_rcas_row(const hdrg_fsr1_t *fsr, const float *easu, uint32_t first,
                               uint32_t last, uint32_t y, float *row)
{
    const float *above, *here, *below;
    uint32_t x, left, right;

    above = easu + (size_t)(((y > first) ? y - 1u : y) - first) * fsr->out_w * 3u;
    here = easu + (size_t)(y - first) * fsr->out_w * 3u;
    below = easu + (size_t)(((y + 1u < last) ? y + 1u : y) - first) * fsr->out_w * 3u;

    for (x = 0; x < fsr->out_w; x++) {
        left = (x > 0) ? x - 1u : x;
        right = (x + 1u < fsr->out_w) ? x + 1u : x;
        hdrg_fsr_rcas(above + (size_t)x * 3u, here + (size_t)left * 3u, here + (size_t)x * 3u,
                      here + (size_t)right * 3u, below + (size_t)x * 3u, fsr->sharpness,
                      row + (size_t)x * 3u);
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_fsr1_process(void *inst, hdrg_job_t *job,
                                                        const hdrg_plugin_api_t *api)
{
    const hdrg_fsr1_t *fsr = (const hdrg_fsr1_t *)inst;
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, first, last, y;
    uint8_t *out;
    size_t in_stride, out_stride, row;
    float *easu, *sharp;
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

    if (fsr->same) {
        if (in_begin > begin || in_end < end) {
            return HDRG_ERR_INTERNAL;
        }

        for (y = begin; y < end; y++) {
            memcpy(out + (size_t)(y - begin) * out_stride, in + (size_t)(y - in_begin) * in_stride,
                   (size_t)fsr->out_w * 6u);
        }

        return HDRG_OK;
    }

    hdrg_fsr1_easu_rows(fsr, begin, end, &first, &last);
    if (hdrg_fsr1_clamp(hdrg_fsr1_row(fsr, first) - 1, fsr->in_h - 1u) < (int64_t)in_begin ||
        hdrg_fsr1_clamp(hdrg_fsr1_row(fsr, last - 1u) + 2, fsr->in_h - 1u) >= (int64_t)in_end) {
        return HDRG_ERR_INTERNAL;
    }

    row = (size_t)fsr->out_w * 3u;
    status = api->job_scratch(job, (size_t)(last - first + 1u) * row * sizeof(float), &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    easu = (float *)scratch;

    sharp = easu + (size_t)(last - first) * row;

    for (y = first; y < last; y++) {
        hdrg_fsr1_easu_row(fsr, in, in_stride, in_begin, y, easu + (size_t)(y - first) * row);
    }

    for (y = begin; y < end; y++) {
        if (fsr->sharpen) {
            hdrg_fsr1_rcas_row(fsr, easu, first, last, y, sharp);
        }

        hdrg_fsr1_pack(fsr->sharpen ? sharp : easu + (size_t)(y - first) * row, fsr->out_w,
                       (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride));
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_fsr_init(hdrg_plugin_t *plugin,
                                                           const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_FSR_PLUGIN),
                                  HDRG_LIT("AMD FidelityFX Super Resolution 1"), HDRG_LIT("1.0.2"),
                                  HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status =
            api->plugin_set_license(plugin, HDRG_LIT(HDRG_FSR_LICENSE), HDRG_LIT(HDRG_FSR_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_FSR_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_int(cls, HDRG_LIT("sharpen"), 0, 0, 1,
                                       HDRG_LIT("RCAS: sharpen EASU's output"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_LIT("sharpness"), HDRG_FSR_SHARPNESS, 0.0, 2.0,
            HDRG_LIT("RCAS's sharpness in stops below the strongest (0 = the strongest)"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_fsr1_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_fsr1_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_fsr1_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_fsr1_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_fsr1_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_fsr_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_fsr_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_fsr_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}
