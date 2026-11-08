/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* readlink (modules.h) */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "hdrgrading/plugin.h"
#include "modules.h"

#define CLI_MAX_PLUGINS     16u
#define CLI_MAX_FORMAT_OPTS 64u
#define CLI_MAX_KEY         64u
#define CLI_MAX_ID          128u
#define CLI_MAX_SETS        16u

static void usage(void)
{
    fprintf(stderr,
            "usage: hdrg-cli [options] <input.{jpg,png,gif,...}> <output.{jpg,jxl,...}>\n"
            "\n"
            "output is an HDR file of a codec (--format, else the output's extension, else\n"
            "JPEG; --list-formats lists them), in one of three forms:\n"
            "\n"
            "  --gainmap two-layer gain map: SDR base + monochrome gain map\n"
            "            (ISO 21496-1; a JPEG's with Apple HDRGainMap and hdrgm XMP).\n"
            "            Gain-map readers (iOS 18+/macOS 15+, Android 15+, Chrome,\n"
            "            libultrahdr) show HDR; everything else shows the SDR.\n"
            "            The default of JPEG (and AVIF / HEIF)\n"
            "  --hybrid  PQ pixels (a JPEG's with a \"P3 PQ\" ICC profile) plus an\n"
            "            SDR-recovering gain map (ISO 21496-1, HDR base). PQ readers show\n"
            "            HDR from the base as well. Caveat: color-unmanaged SDR viewers\n"
            "            show it dark and flat. The default of JPEG XL\n"
            "  --pq      bare single-layer PQ, no gain map.\n"
            "\n");
    fprintf(stderr,
            "options (defaults of the photo preset):\n"
            "  --preset <name>          photo (default), anime or game: the starting\n"
            "                           point every other option adjusts\n"
            "  --strength <0..1>        global strength (default 1.0)\n"
            "  --diffuse-max <1..4>     how far diffuse content rises in a normally\n"
            "                           exposed picture, x SDR (default 2.0); only\n"
            "                           lights and reflections rise above it\n"
            "  --scale-shift <0..4>     stored gain map resolution 1/2^n (default 1)\n"
            "  --format <name>          the output's codec: an id (hdrg.jxl) or its name\n"
            "                           (jpeg, jxl, avif, heif; default: the output's\n"
            "                           extension, else jpeg). --jpeg / --jxl / --avif /\n"
            "                           --heif are the same\n"
            "  --format-opt <key>=<v>   an option of the codec (repeatable; --list-formats\n"
            "                           shows them: quality, gainmap_quality, ...)\n"
            "  --quality <n>            the codec's quality (JPEG 1..100, default 98)\n"
            "  --gm-quality <n>         ... of the gain map (JPEG default 90)\n"
            "  --peak-nits <nits>       composite luminance ceiling, <=0 = auto\n"
            "  --display-headroom <stops>  the display's headroom (log2 of peak over\n"
            "                           SDR white, < 0 = no limit; default -1)\n"
            "  --sdr-white <nits>       the display's SDR white (0 = 203, default)\n"
            "  --full-white <nits>      full-white (ABL) limit of the average (0 = none)\n");
    fprintf(stderr,
            "  --apl-budget <nits>      average luminance ceiling in nits (default\n"
            "                           0 = no nits limit)\n"
            "  --apl-peak-fraction <0..1>  average luminance ceiling as a fraction\n"
            "                           of the peak (default 0.25, 0 = none; both 0 =\n"
            "                           no APL budget)\n"
            "  --colored-lights / --no-colored-lights  also judge saturated lights\n"
            "                           (a red neon tube, a colored LED: their channel\n"
            "                           clips, their luminance stays low; preset\n"
            "                           default: anime)\n"
            "  --light-predictor <0..1> the learned check of the judged lights: each\n"
            "                           light's boost scaled toward its predicted chance\n"
            "                           of a light (default 0: the rules alone)\n"
            "  --contrast <0.25..4>     log-luma S-slope around mid-gray (default 1:\n"
            "                           the surfaces keep their ratios)\n"
            "  --clarity <0..1>         local contrast strength (default 0)\n"
            "  --saturation <0..2>      perceptual chroma scale (Oklab), hue- and\n"
            "                           lightness-preserving (default 1.05)\n"
            "  --highlight-desat <0..1> bleach saturated highlights toward white near\n"
            "                           peak for extra visual brightness (default 0.4)\n"
            "  --expand <0..1>          wide-gamut use: saturated colors near the\n"
            "                           source gamut boundary reach toward Display P3;\n"
            "                           neutrals, pastels and skin stay (default 0.7,\n"
            "                           0 = colorimetric)\n"
            "  --expand-reach <0..1>    how far toward the P3 boundary (default 0.85)\n"
            "  --expand-skin <0..1>     skin protection of the expansion (default 0.9)\n"
            "  --expand-memory <0..1>   foliage / sky protection (default 0.5)\n"
            "  --sdr-eotf <curve>       decoding of sRGB-tagged input: auto (preset),\n"
            "                           srgb, 2.2 or bt1886 (2.4)\n"
            "  --chroma-fit <fit>       how a 4:2:0 / 4:2:2 JPEG's chroma follows the\n"
            "                           luma: linear or segmented (bent at an outline\n"
            "                           between two fills; preset default: anime)\n"
            "  --icc-lumi <nits>        (JPEG, PQ modes) the ICC luminance tag, 0 = omit\n"
            "  --gainmap                SDR base + gain map (see above)\n"
            "  --hybrid                 PQ primary + SDR-recovering gain map (see above)\n"
            "  --pq                     bare single-layer PQ output (see above)\n"
            "  --tonemap-knee <0.1..0.99>  SDR rendition tone-map knee (default 0.75)\n"
            "  --no-hagc                (JPEG, hybrid / --pq) omit the ICC HAGC tag (SMPTE\n"
            "                           ST 2094-50 gain curves for lower headroom)\n"
            "  --gm-cap <stops>         (gain map) declared headroom cap (default\n"
            "                           2.0, 0 = uncap). Viewers scale the whole\n"
            "                           gain map by display/declared headroom;\n"
            "                           capping near panel reality keeps the opened/\n"
            "                           thumbnail look as bright as PQ. Higher =\n"
            "                           more dark-room peak, dimmer casual viewing\n"
            "  --gm-gamma <0.2..5>      gain map encoding gamma (default 1.0)\n"
            "  --no-apple-xmp           (JPEG gain map) omit Apple HDRGainMap XMP\n"
            "  --no-hdrgm-xmp           (JPEG gain map) omit hdrgm XMP + GContainer\n"
            "  --threads <n>            0 = auto (default), 1 = single\n"
            "  --gpu <mode>             auto / filters: the built-in prefilters and\n"
            "                           Lanczos with a GPU version on the GPU (Metal,\n"
            "                           else Vulkan, else WebGPU; default off)\n"
            "  --no-dither              disable output dithering\n");
    fprintf(stderr,
            "  --grain-policy <name>    preserve (default: the boost does not\n"
            "                           amplify grain and noise) or reduce (the\n"
            "                           PREFILTER stage's noise reduction first)\n"
            "  --denoise <0..4>         (reduce) the built-in noise reduction's\n"
            "                           threshold over the measured noise (default 1)\n"
            "  --deblock <0..4>         the PREFILTER stage's deblocking of the 8-pixel\n"
            "                           grid, its thresholds over the measured block step\n"
            "                           (0 = off; preset default: photo / anime 1)\n"
            "  --dering <0..3>          the deringing after it, its window's radius\n"
            "                           1 + 2 x this (0 = off; anime 2)\n"
            "  --defringe <0..3>        the defringing after it (the false color around\n"
            "                           edges), its window's radius 1 + 2 x this (0 = off;\n"
            "                           anime 3)\n"
            "  --deband <0..3>          the debanding after it (8-bit steps in gradients),\n"
            "                           its residual threshold x this (0 = off; photo\n"
            "                           0.5, anime 2.5)\n"
            "  --effort <e>             quality (default: lights judged on finer cells, no\n"
            "                           time budget) or realtime (a video's: 4x4 cells)\n"
            "  --stats                  print analysis stats to stderr (and the SIMD\n"
            "                           tier; HDRG_SIMD_MAX=scalar|sse41|avx2|neon caps it,\n"
            "                           avx512|sve select the provisional tiers)\n");
    fprintf(stderr,
            "\nsize, filters and codecs (plugin ABI: include/hdrgrading/filter.h, codec.h):\n"
            "  --output-size <W>x<H>    output pixels (one side 0 = keep the aspect\n"
            "                           ratio). Runs the SCALE stage: the --filter\n"
            "                           given for it, else a built-in scaler\n"
            "  --upscaler <name>        the built-in scaler of an output larger on\n"
            "                           both sides: lanczos, edge (the library's\n"
            "                           edge-adaptive), fsr1 or nis (preset default:\n"
            "                           photo lanczos, anime / game edge); any other\n"
            "                           size is lanczos\n"
            "  --plugin <path>          load a plugin module: filters, codecs\n"
            "                           (repeatable; nothing is searched)\n"
            "  --filter <stage>=<id>    use filter <id> on a stage: scale, or\n"
            "                           prefilter (repeatable: a chain in that order,\n"
            "                           in place of --deblock / --grain-policy reduce)\n"
            "  --filter-opt <key>=<v>   option of the preceding --filter, checked\n"
            "                           against the type and range it declares\n"
            "  --set <name>=<value>     a setting by name (the library's table: the\n"
            "                           options' names above without the dashes, an\n"
            "                           enumerated one by its values' names), after\n"
            "                           the preset and the other options (repeatable)\n"
            "  --list-filters           list plugins, filters, codecs and their options\n"
            "  --list-formats           the same\n");
}

/* <W>x<H>, each 0..65535 */
static bool parse_size(const char *s, uint32_t *width, uint32_t *height)
{
    unsigned long w, h;
    char *end;

    w = strtoul(s, &end, 10);
    if (end == s || (*end != 'x' && *end != 'X')) {
        return false;
    }

    s = end + 1;
    h = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || w > 65535ul || h > 65535ul) {
        return false;
    }

    *width = (uint32_t)w;
    *height = (uint32_t)h;

    return true;
}

static const char *stage_name(uint32_t stages)
{
    switch (stages) {
    case HDRG_STAGE_ANALYZE:
        return "analyze";
    case HDRG_STAGE_PREFILTER:
        return "prefilter";
    case HDRG_STAGE_INTERP:
        return "interp";
    case HDRG_STAGE_SCALE:
        return "scale";
    default:
        return "several";
    }
}

static const char *plugin_state(const hdrg_plugin_info_t *info)
{
    if (info->accepted) {
        return (info->status == HDRG_PLUGIN_DEGRADED) ? "degraded" : "ready";
    }

    return (info->reason == HDRG_REASON_REJECTED || info->reason == HDRG_REASON_HOST_TOO_OLD ||
            info->reason == HDRG_REASON_PLUGIN_TOO_OLD)
               ? "rejected"
               : "unavailable";
}

/* an option's line (every string printed here was validated by the library: no controls) */
static void print_option(const hdrg_option_info_t *option)
{
    if (option->type == HDRG_OPTION_INT) {
        printf("    %s (int, default %lld, %lld..%lld) %s\n", option->key,
               (long long)option->default_int, (long long)option->min_int,
               (long long)option->max_int, option->description);
    } else if (option->type == HDRG_OPTION_STRING) {
        printf("    %s (string, default \"%s\") %s\n", option->key, option->default_text,
               option->description);
    } else {
        printf("    %s (float, default %g, %g..%g) %s\n", option->key, option->default_float,
               option->min_float, option->max_float, option->description);
    }
}

static const char *form_name(uint32_t form)
{
    switch (form) {
    case HDRG_OUTPUT_GAINMAP:
        return "gainmap";
    case HDRG_OUTPUT_HYBRID:
        return "hybrid";
    case HDRG_OUTPUT_PQ:
        return "pq";
    default:
        return "?";
    }
}

static void list_filters(const hdrg_context_t *ctx)
{
    hdrg_plugin_info_t plugin;
    hdrg_filter_info_t filter;
    hdrg_codec_info_t codec;
    hdrg_option_info_t option;
    uint32_t i, j;

    printf("plugins:\n");

    for (i = 0; i < hdrg_context_plugin_count(ctx); i++) {
        plugin.struct_size = sizeof(plugin);
        if (hdrg_context_plugin_info(ctx, i, &plugin) != HDRG_OK) {
            continue;
        }

        printf("  %s %s (%s) license: %s [%s%s]%s%s\n", plugin.id, plugin.version, plugin.name,
               plugin.license, plugin.internal ? "built-in, " : "", plugin_state(&plugin),
               plugin.status_message[0] != '\0' ? " " : "", plugin.status_message);
    }

    printf("filters:\n");

    for (i = 0; i < hdrg_context_filter_count(ctx); i++) {
        filter.struct_size = sizeof(filter);
        if (hdrg_context_filter_info(ctx, i, &filter) != HDRG_OK) {
            continue;
        }

        printf("  %s  stage: %s%s%s%s\n", filter.id, stage_name(filter.stages),
               (filter.flags & HDRG_FILTER_PURE) ? " pure" : "",
               (filter.flags & HDRG_FILTER_BANDS) ? " bands" : "",
               (filter.flags & HDRG_FILTER_DETERMINISTIC) ? " deterministic" : "");

        for (j = 0; j < filter.option_count; j++) {
            option.struct_size = sizeof(option);
            if (hdrg_context_filter_option(ctx, i, j, &option) == HDRG_OK) {
                print_option(&option);
            }
        }
    }

    printf("codecs:\n");

    for (i = 0; i < hdrg_context_codec_count(ctx); i++) {
        codec.struct_size = sizeof(codec);
        if (hdrg_context_codec_info(ctx, i, &codec) != HDRG_OK) {
            continue;
        }

        printf("  %s  .%s%s%s", codec.id, codec.extensions,
               (codec.roles & HDRG_CODEC_READ) ? " read" : "",
               (codec.roles & HDRG_CODEC_WRITE) ? " write:" : "");

        for (j = HDRG_OUTPUT_GAINMAP; j <= HDRG_OUTPUT_PQ; j++) {
            if ((codec.forms & (UINT32_C(1) << j)) != 0) {
                printf(" %s%s", form_name(j), (j == codec.default_form) ? " (default)" : "");
            }
        }

        printf("\n");

        for (j = 0; j < codec.option_count; j++) {
            option.struct_size = sizeof(option);
            if (hdrg_context_codec_option(ctx, i, j, &option) == HDRG_OK) {
                print_option(&option);
            }
        }
    }
}

/* name is one of list's comma-separated words, ignoring case */
static bool in_list(const char *list, const char *name)
{
    const char *word;
    size_t length, i;

    length = strlen(name);
    word = list;

    while (*word != '\0') {
        for (i = 0; i < length && word[i] != '\0' && word[i] != ',' &&
                    (word[i] | 0x20) == (name[i] | 0x20);
             i++) {
        }

        if (i == length && (word[i] == '\0' || word[i] == ',')) {
            return true;
        }

        word = strchr(word, ',');
        if (word == NULL) {
            return false;
        }

        word++;
    }

    return false;
}

/* the writer named: its id, the id after "hdrg.", or one of its extensions; NULL: none */
static const char *find_writer(const hdrg_context_t *ctx, const char *name)
{
    hdrg_codec_info_t codec;
    uint32_t i;

    for (i = 0; i < hdrg_context_codec_count(ctx); i++) {
        codec.struct_size = sizeof(codec);
        if (hdrg_context_codec_info(ctx, i, &codec) != HDRG_OK ||
            (codec.roles & HDRG_CODEC_WRITE) == 0) {
            continue;
        }

        if (strcmp(codec.id, name) == 0 ||
            (strncmp(codec.id, "hdrg.", 5) == 0 && strcmp(codec.id + 5, name) == 0) ||
            in_list(codec.extensions, name)) {
            return codec.id;
        }
    }

    return NULL;
}

/*
 * the conversion's writer: --format's, else the output's extension's (none of a codec: the
 * default, JPEG), with the --format-opt key=value
 */
static bool set_writer(hdrg_context_t *ctx, const char *format, const char *output_path,
                       const char *const *opts, uint32_t opt_count)
{
    const char *id, *dot, *value;
    hdrg_options_t *options;
    hdrg_status_t status;
    uint32_t i;
    size_t key_len;
    char key[CLI_MAX_KEY + 1];

    id = NULL;

    if (format != NULL) {
        id = find_writer(ctx, format);
        if (id == NULL) {
            fprintf(stderr, "--format: no codec writes %s (--list-formats)\n", format);

            return false;
        }
    } else {
        dot = strrchr(output_path, '.');
        id = (dot != NULL && strchr(dot, '/') == NULL) ? find_writer(ctx, dot + 1) : NULL;
    }

    status = hdrg_options_create(ctx, &options);
    if (status != HDRG_OK) {
        fprintf(stderr, "--format-opt: %s\n", hdrg_status_string(status));

        return false;
    }

    for (i = 0; i < opt_count; i++) {
        value = strchr(opts[i], '=');
        key_len = (value != NULL) ? (size_t)(value - opts[i]) : 0;
        if (value == NULL || key_len == 0 || key_len > CLI_MAX_KEY) {
            fprintf(stderr, "--format-opt: expected <key>=<value>: %s\n", opts[i]);
            hdrg_options_destroy(options);

            return false;
        }

        memcpy(key, opts[i], key_len);
        key[key_len] = '\0';

        status = hdrg_options_set(options, key, value + 1);
        if (status != HDRG_OK) {
            fprintf(stderr, "--format-opt %s: %s\n", opts[i], hdrg_context_error(ctx));
            hdrg_options_destroy(options);

            return false;
        }
    }

    status = hdrg_context_set_writer(ctx, id, options);
    hdrg_options_destroy(options);

    if (status != HDRG_OK) {
        fprintf(stderr, "--format %s: %s (%s)\n", (id != NULL) ? id : "hdrg.jpeg",
                hdrg_context_error(ctx), hdrg_status_string(status));

        return false;
    }

    return true;
}

static uint8_t *read_file(const char *path, size_t *size)
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

    data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(fp);

        return NULL;
    }

    if (fread(data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        free(data);
        fclose(fp);

        return NULL;
    }

    fclose(fp);
    *size = (size_t)file_size;

    return data;
}

static bool write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *fp;
    bool ok;

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return false;
    }

    ok = fwrite(data, 1, size, fp) == size;

    return (fclose(fp) == 0) && ok;
}

int main(int argc, char **argv)
{
    const char *input_path, *output_path, *arg, *format, *plugins[CLI_MAX_PLUGINS],
        *format_opts[CLI_MAX_FORMAT_OPTS], *sets[CLI_MAX_SETS];
    hdrg_context_desc_t desc;
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t display;
    hdrg_stats_t stats;
    hdrg_status_t status;
    hdrg_context_t *ctx;
    tools_filter_t filters[TOOLS_MAX_FILTERS];
    uint32_t plugin_count, filter_count, format_opt_count, value_count, set_count, k;
    uint8_t *input, *output, preset;
    int i;
    size_t input_size, output_size;
    bool show_stats, list;
    char values[4][CLI_MAX_KEY + 64];

    /* the preset is the base every other option adjusts, wherever it appears */
    hdrg_context_desc_init(&desc);
    hdrg_encode_params_init(&encode);
    hdrg_display_caps_init(&display);
    preset = HDRG_PRESET_PHOTO;
    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--preset") == 0 &&
            !tools_named("--preset", "preset", argv[i + 1], &preset)) {
            return 2;
        }
    }

    hdrg_params_init(&params, preset);
    input_path = NULL;
    output_path = NULL;
    show_stats = false;
    list = false;
    format = NULL;
    plugin_count = 0;
    filter_count = 0;
    format_opt_count = 0;
    value_count = 0;
    set_count = 0;

    for (i = 1; i < argc; i++) {
        arg = argv[i];
        if (strcmp(arg, "--stats") == 0) {
            show_stats = true;
        } else if (strcmp(arg, "--preset") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(arg, "--no-dither") == 0) {
            encode.dither = false;
        } else if (strcmp(arg, "--colored-lights") == 0) {
            params.colored_lights = true;
        } else if (strcmp(arg, "--no-colored-lights") == 0) {
            params.colored_lights = false;
        } else if (strcmp(arg, "--strength") == 0 && i + 1 < argc) {
            params.global_strength = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--light-predictor") == 0 && i + 1 < argc) {
            params.light_predictor = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--diffuse-max") == 0 && i + 1 < argc) {
            params.diffuse_max = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--scale-shift") == 0 && i + 1 < argc) {
            encode.gainmap_scale_shift = (uint8_t)atoi(argv[++i]);
        } else if ((strcmp(arg, "--quality") == 0 || strcmp(arg, "--gm-quality") == 0 ||
                    strcmp(arg, "--icc-lumi") == 0) &&
                   i + 1 < argc) {
            /* the codec's options of these names */
            if (format_opt_count >= CLI_MAX_FORMAT_OPTS || value_count >= 4u) {
                fprintf(stderr, "%s: too many codec options\n", arg);

                return 2;
            }

            snprintf(values[value_count], sizeof(values[value_count]), "%s=%s",
                     (arg[2] == 'q')   ? "quality"
                     : (arg[2] == 'g') ? "gainmap_quality"
                                       : "icc_lumi",
                     argv[++i]);
            format_opts[format_opt_count++] = values[value_count++];
        } else if (strcmp(arg, "--peak-nits") == 0 && i + 1 < argc) {
            display.peak_nits = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--display-headroom") == 0 && i + 1 < argc) {
            display.headroom_stops = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--sdr-white") == 0 && i + 1 < argc) {
            display.sdr_white_nits = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--full-white") == 0 && i + 1 < argc) {
            display.max_full_frame_nits = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--contrast") == 0 && i + 1 < argc) {
            params.contrast = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--clarity") == 0 && i + 1 < argc) {
            params.clarity = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--saturation") == 0 && i + 1 < argc) {
            params.saturation = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--highlight-desat") == 0 && i + 1 < argc) {
            params.highlight_desat = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--apl-budget") == 0 && i + 1 < argc) {
            params.apl_budget_nits = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--apl-peak-fraction") == 0 && i + 1 < argc) {
            params.apl_peak_fraction = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--expand") == 0 && i + 1 < argc) {
            params.expand_strength = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--expand-reach") == 0 && i + 1 < argc) {
            params.expand_reach = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--expand-skin") == 0 && i + 1 < argc) {
            params.expand_skin = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--expand-memory") == 0 && i + 1 < argc) {
            params.expand_memory = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--sdr-eotf") == 0 && i + 1 < argc) {
            if (!tools_named(arg, "sdr-eotf", argv[++i], &params.sdr_eotf)) {
                return 2;
            }
        } else if (strcmp(arg, "--chroma-fit") == 0 && i + 1 < argc) {
            if (!tools_named(arg, "chroma-fit", argv[++i], &params.chroma_fit)) {
                return 2;
            }
        } else if (strcmp(arg, "--upscaler") == 0 && i + 1 < argc) {
            if (!tools_named(arg, "upscaler", argv[++i], &params.upscaler)) {
                return 2;
            }
        } else if (strcmp(arg, "--grain-policy") == 0 && i + 1 < argc) {
            if (!tools_named(arg, "grain-policy", argv[++i], &params.grain_policy)) {
                return 2;
            }
        } else if (strcmp(arg, "--denoise") == 0 && i + 1 < argc) {
            params.denoise_strength = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--deblock") == 0 && i + 1 < argc) {
            params.deblock_strength = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--dering") == 0 && i + 1 < argc) {
            params.dering_strength = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--defringe") == 0 && i + 1 < argc) {
            params.defringe_strength = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--deband") == 0 && i + 1 < argc) {
            params.deband_strength = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--effort") == 0 && i + 1 < argc) {
            if (!tools_named(arg, "effort", argv[++i], &params.effort)) {
                return 2;
            }
        } else if (strcmp(arg, "--gainmap") == 0) {
            encode.output_mode = HDRG_OUTPUT_GAINMAP;
        } else if (strcmp(arg, "--pq") == 0) {
            encode.output_mode = HDRG_OUTPUT_PQ;
        } else if (strcmp(arg, "--hybrid") == 0) {
            encode.output_mode = HDRG_OUTPUT_HYBRID;
        } else if (strcmp(arg, "--jpeg") == 0 || strcmp(arg, "--jxl") == 0 ||
                   strcmp(arg, "--avif") == 0 || strcmp(arg, "--heif") == 0) {
            format = arg + 2;
        } else if (strcmp(arg, "--format") == 0 && i + 1 < argc) {
            format = argv[++i];
        } else if ((strcmp(arg, "--format-opt") == 0 && i + 1 < argc) ||
                   strcmp(arg, "--no-hagc") == 0 || strcmp(arg, "--no-apple-xmp") == 0 ||
                   strcmp(arg, "--no-hdrgm-xmp") == 0) {
            if (format_opt_count >= CLI_MAX_FORMAT_OPTS) {
                fprintf(stderr, "%s: too many codec options\n", arg);

                return 2;
            }

            format_opts[format_opt_count++] = (strcmp(arg, "--no-hagc") == 0)        ? "hagc=0"
                                              : (strcmp(arg, "--no-apple-xmp") == 0) ? "apple_xmp=0"
                                              : (strcmp(arg, "--no-hdrgm-xmp") == 0) ? "hdrgm_xmp=0"
                                                                                     : argv[++i];
        } else if (strcmp(arg, "--tonemap-knee") == 0 && i + 1 < argc) {
            encode.tonemap_knee = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--gm-cap") == 0 && i + 1 < argc) {
            encode.gainmap_headroom_cap_stops = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--gm-gamma") == 0 && i + 1 < argc) {
            encode.gainmap_gamma = (float)atof(argv[++i]);
        } else if (strcmp(arg, "--threads") == 0 && i + 1 < argc) {
            desc.thread_count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--gpu") == 0 && i + 1 < argc) {
            desc.gpu = (strcmp(argv[++i], "auto") == 0)    ? HDRG_GPU_AUTO
                       : (strcmp(argv[i], "filters") == 0) ? HDRG_GPU_FILTERS
                                                           : HDRG_GPU_OFF;
        } else if (strcmp(arg, "--output-size") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &encode.output_width, &encode.output_height)) {
                fprintf(stderr, "--output-size: expected <W>x<H>: %s\n", argv[i]);

                return 2;
            }
        } else if (strcmp(arg, "--plugin") == 0 && i + 1 < argc) {
            if (plugin_count >= CLI_MAX_PLUGINS) {
                fprintf(stderr, "--plugin: too many plugins\n");

                return 2;
            }

            plugins[plugin_count++] = argv[++i];
        } else if (strcmp(arg, "--filter") == 0 && i + 1 < argc) {
            if (!tools_filter(filters, &filter_count, argv[++i])) {
                return 2;
            }
        } else if (strcmp(arg, "--filter-opt") == 0 && i + 1 < argc) {
            if (!tools_filter_opt(filters, filter_count, argv[++i])) {
                return 2;
            }
        } else if (strcmp(arg, "--set") == 0 && i + 1 < argc) {
            if (set_count >= CLI_MAX_SETS) {
                fprintf(stderr, "--set: at most %u\n", CLI_MAX_SETS);

                return 2;
            }

            sets[set_count++] = argv[++i];
        } else if (strcmp(arg, "--list-filters") == 0 || strcmp(arg, "--list-formats") == 0) {
            list = true;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage();

            return 0;
        } else if (arg[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", arg);
            usage();

            return 2;
        } else if (input_path == NULL) {
            input_path = arg;
        } else if (output_path == NULL) {
            output_path = arg;
        } else {
            usage();

            return 2;
        }
    }

    if (!list && (input_path == NULL || output_path == NULL)) {
        usage();

        return 2;
    }

    /* --set after the preset and the other options */
    for (k = 0; k < set_count; k++) {
        if (!tools_set(sets[k], &params, &display, &encode, NULL)) {
            return 2;
        }
    }

    status = hdrg_context_create(&desc, &ctx);
    if (status != HDRG_OK) {
        fprintf(stderr, "context: %s\n", hdrg_status_string(status));

        return 1;
    }

    /* the codec modules beside the program (AVIF, HEIF), then the ones given */
    hdrg_tool_load_modules(ctx);

    for (k = 0; k < plugin_count; k++) {
        status = hdrg_context_load_plugin(ctx, plugins[k]);
        if (status != HDRG_OK) {
            fprintf(stderr, "--plugin %s: %s (%s)\n", plugins[k], hdrg_context_error(ctx),
                    hdrg_status_string(status));
        }
    }

    if (list) {
        list_filters(ctx);
        hdrg_context_destroy(ctx);

        return 0;
    }

    if (!tools_add_filters(ctx, filters, filter_count, HDRG_STAGE_SCALE | HDRG_STAGE_PREFILTER) ||
        !set_writer(ctx, format, output_path, format_opts, format_opt_count)) {
        hdrg_context_destroy(ctx);

        return 2;
    }

    input = read_file(input_path, &input_size);
    if (input == NULL) {
        fprintf(stderr, "failed to read %s\n", input_path);
        hdrg_context_destroy(ctx);

        return 1;
    }

    status =
        hdrg_convert(ctx, input, input_size, &params, &display, &encode, &output, &output_size);
    free(input);

    if (status != HDRG_OK) {
        fprintf(stderr, "convert: %s%s%s\n", hdrg_status_string(status),
                hdrg_context_error(ctx)[0] != '\0' ? ": " : "", hdrg_context_error(ctx));
        hdrg_context_destroy(ctx);

        return 1;
    }

    if (show_stats && hdrg_get_last_stats(ctx, &stats) == HDRG_OK) {
        fprintf(stderr,
                "max_gain_stops=%.4f diffuse_stops=%.4f light_ratio=%.5f\n"
                "sdr_apl_nits=%.1f apl_nits=%.1f apl_scale=%.4f simd=%s\n"
                "noise_sigma=%.3f noise_blockiness=%.3f noise_ringing=%.3f\n",
                (double)stats.applied_max_gain_stops, (double)stats.diffuse_stops,
                (double)stats.light_area_ratio, (double)stats.sdr_apl_nits, (double)stats.apl_nits,
                (double)stats.apl_scale, hdrg_simd_name(stats.simd_tier), (double)stats.noise_sigma,
                (double)stats.noise_blockiness, (double)stats.noise_ringing);
    }

    if (!write_file(output_path, output, output_size)) {
        fprintf(stderr, "failed to write %s\n", output_path);
        hdrg_free(output);
        hdrg_context_destroy(ctx);

        return 1;
    }

    printf("%s (%zu bytes, hdrgrading %s)\n", output_path, output_size, hdrg_version());

    hdrg_free(output);
    hdrg_context_destroy(ctx);

    return 0;
}
