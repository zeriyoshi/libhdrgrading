/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* readlink (modules.h) */
#endif

#include <string.h>

#include "../modules.h"
#include "gui.h"

/* an ASCII letter in lower case */
static inline char hdrg_gui_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* whether extension (no dot) is one of list's (comma-separated), case aside */
static bool hdrg_gui_in_list(const char *list, const char *extension)
{
    size_t n, length, i;

    length = strlen(extension);

    while (*list != '\0') {
        n = strcspn(list, ",");

        for (i = 0; n == length && i < n && hdrg_gui_lower(list[i]) == hdrg_gui_lower(extension[i]);
             i++) {
        }

        if (n == length && i == n) {
            return true;
        }

        list += (list[n] == ',') ? n + 1u : n;
    }

    return false;
}

extern void hdrg_gui_settings_init(hdrg_gui_settings_t *settings, uint8_t preset)
{
    memset(settings, 0, sizeof(*settings));
    hdrg_params_init(&settings->params, preset);
    hdrg_display_caps_init(&settings->display);
    hdrg_encode_params_init(&settings->encode);
    settings->temporal = HDRG_TEMPORAL_WINDOWED;
    settings->gpu = HDRG_GPU_AUTO;
}

/*
 * A still: one frame with no window (OFFLINE: the effort a still's), its grain kept; a video:
 * the settings' temporal mode at the latency, the frames as the medium gives them
 */
extern hdrg_status_t hdrg_gui_session_open(hdrg_context_t *ctx, const hdrg_gui_info_t *info,
                                           const hdrg_gui_settings_t *settings, uint8_t latency,
                                           const hdrg_session_pixfmt_t *output,
                                           hdrg_session_t **out)
{
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_params_t params;
    bool still;

    still = info->rate_num == 0;
    params = settings->params;
    memset(&format, 0, sizeof(format));
    format.width = info->width;
    format.height = info->height;
    format.primaries = info->primaries;
    format.transfer = info->transfer;
    format.bit_depth = 8;
    format.stride = (size_t)info->width * 4u;
    format.has_alpha = true;
    hdrg_session_config_init(&config);
    config.temporal = still ? HDRG_TEMPORAL_WINDOWED : settings->temporal;
    config.latency = still ? HDRG_LATENCY_OFFLINE : latency;
    config.output = *output;

    if (still) {
        config.past = 0;
        config.future = 0;
        params.grain_policy = HDRG_GRAIN_PRESERVE;
    } else {
        config.rate_num = info->rate_num;
        config.rate_den = info->rate_den;
    }

    if (info->format != HDRG_GUI_RGBA) {
        config.input.layout = HDRG_YUV_420;
        config.input.matrix = info->matrix;
        config.input.bits = (info->format == HDRG_GUI_P010) ? 10u : 8u;
        config.input.chroma_loc = info->chroma_loc;
        config.input.full_range = info->full_range;
        config.input.interleaved = true;
        config.input.msb = info->format == HDRG_GUI_P010;
    }

    return hdrg_session_create(ctx, &format, &params, &settings->display, &config, out);
}

extern void hdrg_gui_session_frame(const hdrg_gui_frame_t *frame, hdrg_session_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    out->planes[0] = frame->planes[0];
    out->planes[1] = frame->planes[1];
    out->strides[0] = frame->strides[0];
    out->strides[1] = frame->strides[1];
    out->pts = frame->pts_us;
}

extern void hdrg_gui_load_modules(hdrg_context_t *ctx)
{
    hdrg_tool_load_modules(ctx);
}

extern hdrg_status_t hdrg_gui_set_writer(hdrg_context_t *ctx, const hdrg_gui_settings_t *settings,
                                         const char *path)
{
    const char *dot, *equals;
    hdrg_codec_info_t codec;
    hdrg_options_t *options;
    hdrg_status_t status;
    uint32_t i;
    char key[HDRG_GUI_FORMAT_OPTION];

    if (settings->format[0] != '\0') {
        status = hdrg_options_create(ctx, &options);

        for (i = 0; status == HDRG_OK && i < settings->format_option_count; i++) {
            equals = strchr(settings->format_options[i], '=');
            if (equals == NULL) {
                status = HDRG_ERR_INVALID_ARG;

                break;
            }

            memcpy(key, settings->format_options[i],
                   (size_t)(equals - settings->format_options[i]));
            key[equals - settings->format_options[i]] = '\0';
            status = hdrg_options_set(options, key, equals + 1);
        }

        if (status == HDRG_OK) {
            status = hdrg_context_set_writer(ctx, settings->format, options);
        }

        hdrg_options_destroy(options);

        return status;
    }

    dot = strrchr(path, '.');

    for (i = 0; dot != NULL && i < hdrg_context_codec_count(ctx); i++) {
        codec.struct_size = sizeof(codec);

        if (hdrg_context_codec_info(ctx, i, &codec) == HDRG_OK &&
            (codec.roles & HDRG_CODEC_WRITE) != 0 && hdrg_gui_in_list(codec.extensions, dot + 1)) {
            return hdrg_context_set_writer(ctx, codec.id, NULL);
        }
    }

    return hdrg_context_set_writer(ctx, NULL, NULL);
}
