/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * Stage timings of one still image: decode, pass A
 * (analysis), pass B (plan), render, the color LUT, pass C (apply, without
 * the encoder) and the whole conversion from pixels to JPEG bytes, in three
 * parts: opening the pipeline (the color LUT, the PREFILTER chain's set-up
 * and probe), the frame (the noise estimate, the PREFILTER chain, passes A
 * and B, the render) and writing the file (pass C and the encoder). Each
 * figure is the median of the runs. --set name=value adjusts any hdrg-cli
 * setting after the preset. Uses the library's internals, like hdrg-inspect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "container.h"
#include "io/io.h"
#include "pipeline.h"

#define BENCH_MAX_RUNS 64u
#define BENCH_DECODE   0u
#define BENCH_PASS_A   1u
#define BENCH_PASS_B   2u
#define BENCH_RENDER   3u
#define BENCH_LUT      4u
#define BENCH_PASS_C   5u
#define BENCH_CONVERT  6u
#define BENCH_OPEN     7u
#define BENCH_FRAME    8u
#define BENCH_WRITE    9u
#define BENCH_FIGURES  10u
#define BENCH_MAX_SETS 16u

static double bench_now(void)
{
    struct timespec ts;

    (void)timespec_get(&ts, TIME_UTC);

    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int bench_compare(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;

    return (x > y) - (x < y);
}

static uint8_t *bench_read(const char *path, size_t *size)
{
    FILE *fp;
    uint8_t *data;
    long file_size;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);

        return NULL;
    }

    data = (uint8_t *)malloc((size_t)file_size + 1u);
    if (data == NULL || fread(data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        free(data);
        fclose(fp);

        return NULL;
    }

    fclose(fp);
    *size = (size_t)file_size;

    return data;
}

/* the core passes one at a time, then the whole conversion; seconds into out */
static hdrg_status_t bench_once(hdrg_context_t *ctx, const uint8_t *file, size_t file_size,
                                const hdrg_params_t *params, const hdrg_encode_params_t *encode,
                                double out[BENCH_FIGURES])
{
    const hdrg_allocator_t *alloc = &ctx->alloc;
    hdrg_analyze_opts_t opts;
    hdrg_encode_params_t form;
    hdrg_container_t container;
    hdrg_display_caps_t caps;
    hdrg_gamut_map_t map;
    hdrg_decoded_t decoded;
    hdrg_raw_image_t raw;
    hdrg_pipeline_t p;
    hdrg_status_t status;
    uint32_t y0, rows, gm_h;
    uint8_t *band, *output;
    size_t output_size;
    bool opened;
    double t, convert;

    memset(&p, 0, sizeof(p));
    memset(&opts, 0, sizeof(opts));
    opts.bright_scale = 1.0f;
    opts.cell_shift =
        (params->effort == HDRG_EFFORT_QUALITY) ? HDRG_FINE_SHIFT : HDRG_QUARTER_SHIFT;
    opts.colored = params->colored_lights;
    p.frame_alloc = alloc;
    hdrg_context_exec(ctx, &p.exec);

    t = bench_now();
    status =
        hdrg_decode_with(ctx, &p.exec, file, file_size, UINT64_MAX, params->chroma_fit, &decoded);
    out[BENCH_DECODE] = bench_now() - t;

    if (status != HDRG_OK) {
        return status;
    }

    memset(&raw, 0, sizeof(raw));
    raw.pixels = decoded.pixels;
    raw.width = decoded.width;
    raw.height = decoded.height;
    raw.bit_depth = decoded.bit_depth;
    raw.stride = decoded.stride;

    /* the default writer's (hdrg.jpeg) form and rows */
    form = *encode;
    encode = &form;
    status = hdrg_container_select(ctx, &form, &container);
    if (status != HDRG_OK) {
        hdrg_decoded_free(alloc, &decoded);

        return status;
    }

    hdrg_container_caps(&container, NULL, encode, &caps);
    p.width = raw.width;
    p.height = raw.height;

    band = (uint8_t *)malloc((size_t)raw.width * 3u * HDRG_APPLY_BAND_ROWS);
    status = (band != NULL) ? hdrg_color_copy(alloc, &decoded.color, &p.color) : HDRG_ERR_NOMEM;

    if (status == HDRG_OK) {
        hdrg_color_set_sdr_eotf(&p.color, params->sdr_eotf);

        status = hdrg_source_init(alloc, &raw, &p.color, &p.src);
    }

    t = bench_now();

    if (status == HDRG_OK) {
        status = hdrg_analyze(alloc, &p.exec, &p.src, &opts, &p.analysis);
    }

    out[BENCH_PASS_A] = bench_now() - t;

    t = bench_now();

    if (status == HDRG_OK) {
        status =
            hdrg_plan_build(alloc, &p.exec, ctx->kernels, &p.src, &p.analysis, 0, params, &p.plan);
    }

    out[BENCH_PASS_B] = bench_now() - t;

    t = bench_now();

    if (status == HDRG_OK) {
        status = hdrg_render_build(alloc, &p.plan, params, encode, &caps, &p.color, &p.render);
    }

    out[BENCH_RENDER] = bench_now() - t;

    t = bench_now();

    if (status == HDRG_OK) {
        hdrg_gamut_map_init(params, p.render.gamut, &map);

        status = hdrg_color_lut_build(alloc, &p.exec, &p.color, p.src.lut, p.src.entries - 1u, &map,
                                      &p.lut);
    }

    out[BENCH_LUT] = bench_now() - t;

    if (status == HDRG_OK) {
        status = hdrg_apply_setup(alloc, &p.exec, &p.src, false, &p.lut, ctx->kernels, params,
                                  encode, &p.apply);
    }

    if (status == HDRG_OK) {
        hdrg_apply_bind(&p.apply, &p.plan, &p.render, params);
    }

    if (status == HDRG_OK && encode->output_mode != HDRG_OUTPUT_PQ) {
        p.apply.gm_shift = 1;
        p.apply.gm_w = (p.width + 1u) >> 1;
        gm_h = (p.height + 1u) >> 1;
        p.apply.gm_sum =
            (float *)hdrg_mem_calloc(alloc, (size_t)p.apply.gm_w * gm_h, sizeof(float));
        p.apply.chunk_max = (float *)hdrg_mem_calloc(
            alloc, hdrg_pool_row_chunks(p.height, HDRG_EXEC_GRAIN_ROWS), sizeof(float));
        status = (p.apply.gm_sum != NULL && p.apply.chunk_max != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
    }

    t = bench_now();

    for (y0 = 0; status == HDRG_OK && y0 < p.height; y0 += HDRG_APPLY_BAND_ROWS) {
        rows = HDRG_MIN(HDRG_APPLY_BAND_ROWS, p.height - y0);

        hdrg_apply_rows(&p.exec, &p.apply, y0, rows, band);
    }

    out[BENCH_PASS_C] = bench_now() - t;

    hdrg_pipeline_close(ctx, &p);
    free(band);

    convert = bench_now();
    opened = false;

    if (status == HDRG_OK) {
        status = hdrg_pipeline_open(ctx, &raw, &decoded.color, params, encode, &caps, 0,
                                    HDRG_LATENCY_OFFLINE, &p);
        opened = status == HDRG_OK;
    }

    out[BENCH_OPEN] = bench_now() - convert;
    t = bench_now();

    if (status == HDRG_OK) {
        status = hdrg_container_prepare(&container, ctx, &p, encode);
    }

    if (status == HDRG_OK) {
        status = hdrg_pipeline_frame(ctx, &p, raw.pixels);
    }

    out[BENCH_FRAME] = bench_now() - t;
    t = bench_now();

    if (status == HDRG_OK) {
        status = hdrg_container_write(&container, ctx, &p, encode, NULL, 0, &output, &output_size);
    }

    out[BENCH_WRITE] = bench_now() - t;

    if (opened) {
        hdrg_pipeline_close(ctx, &p);
    }

    out[BENCH_CONVERT] = bench_now() - convert;

    if (status == HDRG_OK) {
        hdrg_free(output);
    }

    hdrg_decoded_free(alloc, &decoded);

    return status;
}

int main(int argc, char **argv)
{
    static const char *const labels[BENCH_FIGURES] = {
        "decode",
        "pass A (analysis)",
        "pass B (plan)",
        "render",
        "color LUT",
        "pass C (apply)",
        "convert (pixels -> JPEG)",
        "  open (LUT, prefilter set-up)",
        "  frame (prefilter, A, B)",
        "  write (C, encode)",
    };
    const char *path, *mode, *sets[BENCH_MAX_SETS], *eq;
    hdrg_context_desc_t desc;
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t runs, i, f, set_count;
    uint8_t *file;
    int a;
    size_t file_size, length;
    char name[64];
    double samples[BENCH_FIGURES][BENCH_MAX_RUNS], once[BENCH_FIGURES], median[BENCH_FIGURES];

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    hdrg_context_desc_init(&desc);
    path = NULL;
    mode = "gainmap";
    runs = 5;
    set_count = 0;

    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--threads") == 0 && a + 1 < argc) {
            desc.thread_count = (uint32_t)strtoul(argv[++a], NULL, 10);
        } else if (strcmp(argv[a], "--runs") == 0 && a + 1 < argc) {
            runs = (uint32_t)atoi(argv[++a]);
        } else if (strcmp(argv[a], "--hybrid") == 0) {
            encode.output_mode = HDRG_OUTPUT_HYBRID;
            mode = "hybrid";
        } else if (strcmp(argv[a], "--pq") == 0) {
            encode.output_mode = HDRG_OUTPUT_PQ;
            mode = "pq";
        } else if (strcmp(argv[a], "--preset") == 0 && a + 1 < argc) {
            a++;
            hdrg_params_init(&params, (strcmp(argv[a], "anime") == 0)  ? HDRG_PRESET_ANIME
                                      : (strcmp(argv[a], "game") == 0) ? HDRG_PRESET_GAME
                                                                       : HDRG_PRESET_PHOTO);
        } else if (strcmp(argv[a], "--set") == 0 && a + 1 < argc && set_count < BENCH_MAX_SETS) {
            sets[set_count++] = argv[++a];
        } else if (argv[a][0] != '-' && path == NULL) {
            path = argv[a];
        } else {
            path = NULL;

            break;
        }
    }

    /* the settings after the preset, wherever they came */
    for (i = 0; path != NULL && i < set_count; i++) {
        eq = strchr(sets[i], '=');
        length = (eq != NULL) ? (size_t)(eq - sets[i]) : sizeof(name);

        if (length >= sizeof(name)) {
            path = NULL;

            break;
        }

        memcpy(name, sets[i], length);
        name[length] = '\0';

        if (hdrg_setting_set(name, atof(eq + 1), &params, NULL, &encode) != HDRG_OK) {
            path = NULL;
        }
    }

    /* a still image's (hdrg_convert) */
    if (params.effort == HDRG_EFFORT_AUTO) {
        params.effort = HDRG_EFFORT_QUALITY;
    }

    if (path == NULL || runs == 0 || runs > BENCH_MAX_RUNS) {
        fprintf(stderr, "usage: hdrg-bench [--threads n] [--runs 1..64] [--hybrid | --pq] "
                        "[--preset photo|anime|game] [--set name=value]... "
                        "<input.{jpg,png,gif}>\n");

        return 2;
    }

    file = bench_read(path, &file_size);
    if (file == NULL) {
        fprintf(stderr, "failed to read %s\n", path);

        return 1;
    }

    status = hdrg_context_create(&desc, &ctx);
    if (status != HDRG_OK) {
        free(file);
        fprintf(stderr, "context: %s\n", hdrg_status_string(status));

        return 1;
    }

    /* one warm-up run (pool start, page faults) */
    status = bench_once(ctx, file, file_size, &params, &encode, once);

    for (i = 0; status == HDRG_OK && i < runs; i++) {
        status = bench_once(ctx, file, file_size, &params, &encode, once);

        for (f = 0; f < BENCH_FIGURES; f++) {
            samples[f][i] = once[f];
        }
    }

    free(file);

    if (status != HDRG_OK) {
        fprintf(stderr, "bench: %s\n", hdrg_status_string(status));
        hdrg_context_destroy(ctx);

        return 1;
    }

    for (f = 0; f < BENCH_FIGURES; f++) {
        qsort(samples[f], runs, sizeof(double), bench_compare);
        median[f] = samples[f][runs / 2];
    }

    printf("%s, mode %s, threads %u, simd %s, median of %u runs\n", path, mode,
           (unsigned)desc.thread_count, hdrg_simd_name(ctx->kernels->tier), (unsigned)runs);

    for (f = 0; f < BENCH_FIGURES; f++) {
        printf("  %-30s %8.1f ms\n", labels[f], median[f] * 1e3);
    }

    printf("  %-30s %8.1f ms  (pass A + pass B + render + color LUT + pass C)\n", "core",
           (median[BENCH_PASS_A] + median[BENCH_PASS_B] + median[BENCH_RENDER] + median[BENCH_LUT] +
            median[BENCH_PASS_C]) *
               1e3);

    hdrg_context_destroy(ctx);

    return 0;
}
