/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrg_wasm.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hdrgrading/foundation/memory.h"
#include "hdrgrading/plugin.h"
#include "hdrgrading/video.h"

struct hdrg_wasm_output {
    uint8_t *data;
    size_t size;
    hdrg_status_t status;
};

struct hdrg_wasm_params {
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t display;
    uint8_t option_count;
    char format[HDRG_WASM_FORMAT];                     /* "": hdrg.jpeg */
    char options[HDRG_WASM_OPTIONS][HDRG_WASM_OPTION]; /* key=value */
};

struct hdrg_wasm_preview {
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t words[HDRG_WASM_PREVIEW_WORDS];
};

/* one frame through the session, its state into the preview's words */
static hdrg_status_t hdrg_wasm_preview_run(hdrg_wasm_preview_t *preview, const uint8_t *rgba,
                                           uint32_t width, const hdrg_display_caps_t *display)
{
    hdrg_session_frame_t frame;
    hdrg_session_output_t out;
    hdrg_apply_state_t state;
    hdrg_status_t status;
    float white;

    memset(&frame, 0, sizeof(frame));
    frame.planes[0] = rgba;
    frame.strides[0] = (size_t)width * 4u;
    status = hdrg_session_send_frame(preview->session, &frame);
    if (status != HDRG_OK) {
        return status;
    }

    status = hdrg_session_send_frame(preview->session, NULL);
    if (status != HDRG_OK) {
        return status;
    }

    memset(&out, 0, sizeof(out));
    memset(&state, 0, sizeof(state));
    state.struct_size = (uint32_t)sizeof(state);
    status = hdrg_session_receive_state(preview->session, &out, &state);
    if (status != HDRG_OK) {
        return status;
    }

    white = (display->sdr_white_nits > 0.0f) ? display->sdr_white_nits : 203.0f;
    preview->words[HDRG_WASM_PREVIEW_PARAMS] = (uint32_t)(uintptr_t)state.params;
    preview->words[HDRG_WASM_PREVIEW_PARAMS + 1u] = (uint32_t)state.params_size;
    preview->words[HDRG_WASM_PREVIEW_LUTS] = (uint32_t)(uintptr_t)state.luts;
    preview->words[HDRG_WASM_PREVIEW_LUTS + 1u] = (uint32_t)state.luts_size;
    preview->words[HDRG_WASM_PREVIEW_FRAME] = (uint32_t)(uintptr_t)state.frame;
    preview->words[HDRG_WASM_PREVIEW_FRAME + 1u] = (uint32_t)state.frame_size;
    preview->words[HDRG_WASM_PREVIEW_SOURCE] = (uint32_t)state.source_pitch;
    preview->words[HDRG_WASM_PREVIEW_DST] = (uint32_t)state.dst_size;
    preview->words[HDRG_WASM_PREVIEW_DST + 1u] = (uint32_t)state.dst_pitch;
    preview->words[HDRG_WASM_PREVIEW_GROUPS] = state.groups[0];
    preview->words[HDRG_WASM_PREVIEW_GROUPS + 1u] = state.groups[1];
    preview->words[HDRG_WASM_PREVIEW_GAMUT] = display->gamut;
    preview->words[HDRG_WASM_PREVIEW_WHITE_MILLI] = (uint32_t)(white * 1000.0f + 0.5f);

    return HDRG_OK;
}

/* the context's writer: the params' with their options (NULL: the default) */
static hdrg_status_t hdrg_wasm_writer(hdrg_context_t *ctx, const hdrg_wasm_params_t *params)
{
    const char *equals;
    hdrg_options_t *options;
    hdrg_status_t status;
    uint8_t i;
    char key[HDRG_WASM_OPTION];

    if (params == NULL) {
        return hdrg_context_set_writer(ctx, NULL, NULL);
    }

    /* (a failed create writes nothing: destroyed below as NULL) */
    options = NULL;
    status = hdrg_options_create(ctx, &options);

    for (i = 0; status == HDRG_OK && i < params->option_count; i++) {
        equals = strchr(params->options[i], '=');
        memcpy(key, params->options[i], (size_t)(equals - params->options[i]));
        key[equals - params->options[i]] = '\0';
        status = hdrg_options_set(options, key, equals + 1);
    }

    if (status == HDRG_OK) {
        status = hdrg_context_set_writer(ctx, (params->format[0] != '\0') ? params->format : NULL,
                                         options);
    }

    hdrg_options_destroy(options);

    return status;
}

extern hdrg_context_t *hdrg_wasm_context_create(uint32_t threads, uint32_t gpu)
{
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;

    hdrg_context_desc_init(&desc);
    desc.thread_count = threads;
    desc.gpu = (uint8_t)gpu;

    if (hdrg_context_create(&desc, &ctx) != HDRG_OK) {
        return NULL;
    }

    return ctx;
}

extern hdrg_wasm_params_t *hdrg_wasm_params_create(uint8_t preset)
{
    hdrg_wasm_params_t *params;

    params = (hdrg_wasm_params_t *)HDRG_MALLOC(sizeof(*params));
    if (params == NULL) {
        return NULL;
    }

    hdrg_params_init(&params->params, preset);
    hdrg_encode_params_init(&params->encode);
    hdrg_display_caps_init(&params->display);
    params->format[0] = '\0';
    params->option_count = 0;

    return params;
}

extern hdrg_status_t hdrg_wasm_params_set(hdrg_wasm_params_t *params, const char *name,
                                          double value)
{
    if (params == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    return hdrg_setting_set(name, value, &params->params, &params->display, &params->encode);
}

extern hdrg_status_t hdrg_wasm_params_format(hdrg_wasm_params_t *params, const char *id)
{
    size_t length;

    length = (id != NULL) ? strlen(id) : 0;

    if (params == NULL || length >= HDRG_WASM_FORMAT) {
        return HDRG_ERR_INVALID_ARG;
    }

    memcpy(params->format, (id != NULL) ? id : "", length + 1u);
    params->option_count = 0;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_wasm_params_format_option(hdrg_wasm_params_t *params, const char *key,
                                                    const char *value)
{
    size_t key_length, value_length;

    if (params == NULL || key == NULL || value == NULL ||
        params->option_count >= HDRG_WASM_OPTIONS) {
        return HDRG_ERR_INVALID_ARG;
    }

    key_length = strlen(key);
    value_length = strlen(value);

    /* a key of its own (the writer splits at the first '=') */
    if (key_length == 0 || strchr(key, '=') != NULL ||
        key_length + 1u + value_length >= HDRG_WASM_OPTION) {
        return HDRG_ERR_INVALID_ARG;
    }

    memcpy(params->options[params->option_count], key, key_length);
    params->options[params->option_count][key_length] = '=';
    memcpy(params->options[params->option_count] + key_length + 1u, value, value_length + 1u);
    params->option_count++;

    return HDRG_OK;
}

extern void hdrg_wasm_params_destroy(hdrg_wasm_params_t *params)
{
    HDRG_FREE(params);
}

extern hdrg_wasm_output_t *hdrg_wasm_convert(hdrg_context_t *ctx, const uint8_t *input,
                                             uint32_t input_size, const hdrg_wasm_params_t *params)
{
    hdrg_wasm_output_t *output;

    output = (hdrg_wasm_output_t *)HDRG_CALLOC(1, sizeof(*output));
    if (output == NULL) {
        return NULL;
    }

    output->status = hdrg_wasm_writer(ctx, params);

    if (output->status == HDRG_OK) {
        output->status =
            hdrg_convert(ctx, input, input_size, (params != NULL) ? &params->params : NULL,
                         (params != NULL) ? &params->display : NULL,
                         (params != NULL) ? &params->encode : NULL, &output->data, &output->size);
    }

    return output;
}

extern hdrg_status_t hdrg_wasm_output_status(const hdrg_wasm_output_t *output)
{
    return output->status;
}

extern const uint8_t *hdrg_wasm_output_data(const hdrg_wasm_output_t *output)
{
    return output->data;
}

extern uint32_t hdrg_wasm_output_size(const hdrg_wasm_output_t *output)
{
    return (uint32_t)output->size;
}

extern void hdrg_wasm_output_free(hdrg_wasm_output_t *output)
{
    if (output == NULL) {
        return;
    }

    hdrg_free(output->data);
    HDRG_FREE(output);
}

extern uint32_t hdrg_wasm_last_tier(const hdrg_context_t *ctx)
{
    hdrg_stats_t stats;

    if (hdrg_get_last_stats(ctx, &stats) != HDRG_OK) {
        return 0;
    }

    return stats.simd_tier;
}

extern hdrg_wasm_preview_t *hdrg_wasm_preview_create(hdrg_context_t *ctx, const uint8_t *rgba,
                                                     uint32_t width, uint32_t height,
                                                     const hdrg_wasm_params_t *params)
{
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_params_t intent;
    hdrg_display_caps_t display;
    hdrg_wasm_preview_t *preview;

    preview = (hdrg_wasm_preview_t *)HDRG_CALLOC(1, sizeof(*preview));
    if (preview == NULL) {
        return NULL;
    }

    if (params != NULL) {
        intent = params->params;
        display = params->display;
    } else {
        hdrg_params_init(&intent, HDRG_PRESET_PHOTO);
        hdrg_display_caps_init(&display);
    }

    intent.grain_policy = HDRG_GRAIN_PRESERVE;
    intent.deblock_strength = 0.0f;
    intent.dering_strength = 0.0f;
    intent.defringe_strength = 0.0f;
    intent.deband_strength = 0.0f;
    intent.mctf_strength = 0.0f;
    memset(&format, 0, sizeof(format));
    format.width = width;
    format.height = height;
    format.bit_depth = 8;
    format.stride = (size_t)width * 4u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.has_alpha = true;
    hdrg_session_config_init(&config);
    config.temporal = HDRG_TEMPORAL_WINDOWED;
    config.past = 0;
    config.future = 0;
    config.latency = HDRG_LATENCY_OFFLINE;
    config.output.bits = 16;
    config.flash_off = true;
    config.dither_off = true;
    preview->status =
        hdrg_session_create(ctx, &format, &intent, &display, &config, &preview->session);

    if (preview->status == HDRG_OK) {
        preview->status = hdrg_wasm_preview_run(preview, rgba, width, &display);
    }

    return preview;
}

extern hdrg_status_t hdrg_wasm_preview_status(const hdrg_wasm_preview_t *preview)
{
    return preview->status;
}

extern const uint32_t *hdrg_wasm_preview_state(const hdrg_wasm_preview_t *preview)
{
    return preview->words;
}

extern void hdrg_wasm_preview_destroy(hdrg_wasm_preview_t *preview)
{
    if (preview == NULL) {
        return;
    }

    hdrg_session_destroy(preview->session);
    HDRG_FREE(preview);
}

extern const char *hdrg_wasm_preview_shader(void)
{
    return (const char *)hdrg_shader(HDRG_SHADER_WGSL, HDRG_SHADER_APPLY, NULL, NULL);
}
