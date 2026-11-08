/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stddef.h>
#include <string.h>

#include "core/render.h"
#include "hdrgrading/video.h"
#include "host/validate.h"
#include "priv.h"

#define HDRG_SETTING_PLAIN        NULL, 0u
#define HDRG_SETTING_NAMES(array) array, (uint8_t)(sizeof(array) / sizeof(array[0]))
/* a preset's built-in filter: its strength where the build has it, else off */
#define HDRG_PARAMS_BUILTIN(name, strength) ((HDRG_BUILTIN_##name != 0) ? (strength) : 0.0f)
/* ANIME's and GAME's upscaler: the edge-adaptive one where the build has it */
#define HDRG_PARAMS_UPSCALER ((HDRG_BUILTIN_EDGE != 0) ? HDRG_UPSCALER_EDGE : HDRG_UPSCALER_LANCZOS)

/* a named value of a setting */
typedef struct {
    const char *name;
    uint32_t value;
} hdrg_setting_name_t;

typedef struct {
    const char *name;
    size_t offset;
    uint8_t part; /* HDRG_SETTING_INTENT / DISPLAY / ENCODE / SESSION */
    uint8_t kind;
    const hdrg_setting_name_t *values; /* its values' names (NULL: a number alone) */
    uint8_t count;
} hdrg_setting_entry_t;

static const hdrg_setting_name_t hdrg_names_preset[] = {
    {"photo", HDRG_PRESET_PHOTO}, {"anime", HDRG_PRESET_ANIME}, {"game", HDRG_PRESET_GAME}};
static const hdrg_setting_name_t hdrg_names_mode[] = {{"auto", HDRG_OUTPUT_AUTO},
                                                      {"gainmap", HDRG_OUTPUT_GAINMAP},
                                                      {"hybrid", HDRG_OUTPUT_HYBRID},
                                                      {"pq", HDRG_OUTPUT_PQ}};
static const hdrg_setting_name_t hdrg_names_sdr_eotf[] = {{"auto", HDRG_SDR_EOTF_AUTO},
                                                          {"srgb", HDRG_SDR_EOTF_SRGB},
                                                          {"2.2", HDRG_SDR_EOTF_GAMMA22},
                                                          {"bt1886", HDRG_SDR_EOTF_BT1886}};
static const hdrg_setting_name_t hdrg_names_chroma_fit[] = {
    {"linear", HDRG_CHROMA_FIT_LINEAR}, {"segmented", HDRG_CHROMA_FIT_SEGMENTED}};
static const hdrg_setting_name_t hdrg_names_upscaler[] = {{"lanczos", HDRG_UPSCALER_LANCZOS},
                                                          {"edge", HDRG_UPSCALER_EDGE},
                                                          {"fsr1", HDRG_UPSCALER_FSR1},
                                                          {"nis", HDRG_UPSCALER_NIS}};
static const hdrg_setting_name_t hdrg_names_grain[] = {{"preserve", HDRG_GRAIN_PRESERVE},
                                                       {"reduce", HDRG_GRAIN_REDUCE},
                                                       {"resynth", HDRG_GRAIN_RESYNTH},
                                                       {"auto", HDRG_GRAIN_AUTO}};
static const hdrg_setting_name_t hdrg_names_effort[] = {{"auto", HDRG_EFFORT_AUTO},
                                                        {"realtime", HDRG_EFFORT_REALTIME},
                                                        {"quality", HDRG_EFFORT_QUALITY}};
static const hdrg_setting_name_t hdrg_names_temporal[] = {{"windowed", HDRG_TEMPORAL_WINDOWED},
                                                          {"recursive", HDRG_TEMPORAL_RECURSIVE},
                                                          {"two-pass", HDRG_TEMPORAL_TWO_PASS},
                                                          {"static", HDRG_TEMPORAL_STATIC}};
static const hdrg_setting_name_t hdrg_names_latency[] = {{"playback", HDRG_LATENCY_PLAYBACK},
                                                         {"low", HDRG_LATENCY_LOW},
                                                         {"offline", HDRG_LATENCY_OFFLINE}};
static const hdrg_setting_name_t hdrg_names_window[] = {{"auto", HDRG_SESSION_AUTO}};

/*
 * the hdrg-cli options without the dashes (a session's: hdrg-video's), the struct's own for
 * switches; preset first (setting it initializes the intent)
 */
static const hdrg_setting_entry_t hdrg_setting_table[] = {
    {"preset", offsetof(hdrg_params_t, preset), HDRG_SETTING_INTENT, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_preset)},
    {"strength", offsetof(hdrg_params_t, global_strength), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"diffuse-max", offsetof(hdrg_params_t, diffuse_max), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"peak-nits", offsetof(hdrg_display_caps_t, peak_nits), HDRG_SETTING_DISPLAY,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"display-headroom", offsetof(hdrg_display_caps_t, headroom_stops), HDRG_SETTING_DISPLAY,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"sdr-white", offsetof(hdrg_display_caps_t, sdr_white_nits), HDRG_SETTING_DISPLAY,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"full-white", offsetof(hdrg_display_caps_t, max_full_frame_nits), HDRG_SETTING_DISPLAY,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"apl-budget", offsetof(hdrg_params_t, apl_budget_nits), HDRG_SETTING_INTENT,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"apl-peak-fraction", offsetof(hdrg_params_t, apl_peak_fraction), HDRG_SETTING_INTENT,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"colored-lights", offsetof(hdrg_params_t, colored_lights), HDRG_SETTING_INTENT,
     HDRG_SETTING_BOOL, HDRG_SETTING_PLAIN},
    {"light-predictor", offsetof(hdrg_params_t, light_predictor), HDRG_SETTING_INTENT,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"contrast", offsetof(hdrg_params_t, contrast), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"clarity", offsetof(hdrg_params_t, clarity), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"saturation", offsetof(hdrg_params_t, saturation), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"highlight-desat", offsetof(hdrg_params_t, highlight_desat), HDRG_SETTING_INTENT,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"expand", offsetof(hdrg_params_t, expand_strength), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"expand-reach", offsetof(hdrg_params_t, expand_reach), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"expand-skin", offsetof(hdrg_params_t, expand_skin), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"expand-memory", offsetof(hdrg_params_t, expand_memory), HDRG_SETTING_INTENT,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"tonemap-knee", offsetof(hdrg_encode_params_t, tonemap_knee), HDRG_SETTING_ENCODE,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"gm-cap", offsetof(hdrg_encode_params_t, gainmap_headroom_cap_stops), HDRG_SETTING_ENCODE,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"gm-gamma", offsetof(hdrg_encode_params_t, gainmap_gamma), HDRG_SETTING_ENCODE,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"scale-shift", offsetof(hdrg_encode_params_t, gainmap_scale_shift), HDRG_SETTING_ENCODE,
     HDRG_SETTING_U8, HDRG_SETTING_PLAIN},
    {"mode", offsetof(hdrg_encode_params_t, output_mode), HDRG_SETTING_ENCODE, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_mode)},
    {"sdr-eotf", offsetof(hdrg_params_t, sdr_eotf), HDRG_SETTING_INTENT, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_sdr_eotf)},
    {"chroma-fit", offsetof(hdrg_params_t, chroma_fit), HDRG_SETTING_INTENT, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_chroma_fit)},
    {"upscaler", offsetof(hdrg_params_t, upscaler), HDRG_SETTING_INTENT, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_upscaler)},
    {"output-width", offsetof(hdrg_encode_params_t, output_width), HDRG_SETTING_ENCODE,
     HDRG_SETTING_U32, HDRG_SETTING_PLAIN},
    {"output-height", offsetof(hdrg_encode_params_t, output_height), HDRG_SETTING_ENCODE,
     HDRG_SETTING_U32, HDRG_SETTING_PLAIN},
    {"grain-policy", offsetof(hdrg_params_t, grain_policy), HDRG_SETTING_INTENT, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_grain)},
    {"denoise", offsetof(hdrg_params_t, denoise_strength), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"grain-amount", offsetof(hdrg_params_t, grain_amount), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"deblock", offsetof(hdrg_params_t, deblock_strength), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"dering", offsetof(hdrg_params_t, dering_strength), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"defringe", offsetof(hdrg_params_t, defringe_strength), HDRG_SETTING_INTENT,
     HDRG_SETTING_FLOAT, HDRG_SETTING_PLAIN},
    {"deband", offsetof(hdrg_params_t, deband_strength), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"mctf", offsetof(hdrg_params_t, mctf_strength), HDRG_SETTING_INTENT, HDRG_SETTING_FLOAT,
     HDRG_SETTING_PLAIN},
    {"effort", offsetof(hdrg_params_t, effort), HDRG_SETTING_INTENT, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_effort)},
    {"dither", offsetof(hdrg_encode_params_t, dither), HDRG_SETTING_ENCODE, HDRG_SETTING_BOOL,
     HDRG_SETTING_PLAIN},
    {"temporal", offsetof(hdrg_session_config_t, temporal), HDRG_SETTING_SESSION, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_temporal)},
    {"latency", offsetof(hdrg_session_config_t, latency), HDRG_SETTING_SESSION, HDRG_SETTING_U8,
     HDRG_SETTING_NAMES(hdrg_names_latency)},
    {"past", offsetof(hdrg_session_config_t, past), HDRG_SETTING_SESSION, HDRG_SETTING_U16,
     HDRG_SETTING_NAMES(hdrg_names_window)},
    {"future", offsetof(hdrg_session_config_t, future), HDRG_SETTING_SESSION, HDRG_SETTING_U16,
     HDRG_SETTING_NAMES(hdrg_names_window)},
    {"proxy", offsetof(hdrg_session_config_t, proxy), HDRG_SETTING_SESSION, HDRG_SETTING_U16,
     HDRG_SETTING_PLAIN},
    {"start", offsetof(hdrg_session_config_t, start), HDRG_SETTING_SESSION, HDRG_SETTING_U32,
     HDRG_SETTING_PLAIN},
    {"preroll", offsetof(hdrg_session_config_t, preroll), HDRG_SETTING_SESSION, HDRG_SETTING_U32,
     HDRG_SETTING_PLAIN},
    {"rate-num", offsetof(hdrg_session_config_t, rate_num), HDRG_SETTING_SESSION, HDRG_SETTING_U32,
     HDRG_SETTING_PLAIN},
    {"rate-den", offsetof(hdrg_session_config_t, rate_den), HDRG_SETTING_SESSION, HDRG_SETTING_U32,
     HDRG_SETTING_PLAIN},
    {"out-rate-num", offsetof(hdrg_session_config_t, out_rate_num), HDRG_SETTING_SESSION,
     HDRG_SETTING_U32, HDRG_SETTING_PLAIN},
    {"out-rate-den", offsetof(hdrg_session_config_t, out_rate_den), HDRG_SETTING_SESSION,
     HDRG_SETTING_U32, HDRG_SETTING_PLAIN},
    {"out-width", offsetof(hdrg_session_config_t, output_width), HDRG_SETTING_SESSION,
     HDRG_SETTING_U32, HDRG_SETTING_PLAIN},
    {"out-height", offsetof(hdrg_session_config_t, output_height), HDRG_SETTING_SESSION,
     HDRG_SETTING_U32, HDRG_SETTING_PLAIN},
    {"flash-off", offsetof(hdrg_session_config_t, flash_off), HDRG_SETTING_SESSION,
     HDRG_SETTING_BOOL, HDRG_SETTING_PLAIN},
    {"shed", offsetof(hdrg_session_config_t, shed), HDRG_SETTING_SESSION, HDRG_SETTING_BOOL,
     HDRG_SETTING_PLAIN},
    {"dither-off", offsetof(hdrg_session_config_t, dither_off), HDRG_SETTING_SESSION,
     HDRG_SETTING_BOOL, HDRG_SETTING_PLAIN},
};

/* value is a whole number in [0, max] */
static inline bool hdrg_setting_whole(double value, double max)
{
    return value >= 0.0 && value <= max && value == (double)(uint32_t)value;
}

static const hdrg_setting_entry_t *hdrg_setting_find(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(hdrg_setting_table) / sizeof(hdrg_setting_table[0]); i++) {
        if (strcmp(name, hdrg_setting_table[i].name) == 0) {
            return &hdrg_setting_table[i];
        }
    }

    return NULL;
}

/* the field of entry in its part's struct; NULL when that struct is */
static uint8_t *hdrg_setting_field(const hdrg_setting_entry_t *entry, const hdrg_params_t *params,
                                   const hdrg_display_caps_t *display,
                                   const hdrg_encode_params_t *encode,
                                   const hdrg_session_config_t *session)
{
    const void *base;

    base = (entry->part == HDRG_SETTING_ENCODE)    ? (const void *)encode
           : (entry->part == HDRG_SETTING_DISPLAY) ? (const void *)display
           : (entry->part == HDRG_SETTING_SESSION) ? (const void *)session
                                                   : (const void *)params;

    return (base == NULL) ? NULL : (uint8_t *)(uintptr_t)base + entry->offset;
}

/* value into field of entry's kind (the preset: params initialized for it) */
static hdrg_status_t hdrg_setting_write(const hdrg_setting_entry_t *entry, uint8_t *field,
                                        double value)
{
    uint32_t whole;
    uint16_t half;
    uint8_t narrow;
    bool flag;
    float real;

    if (strcmp(entry->name, "preset") == 0) {
        if (!hdrg_setting_whole(value, (double)HDRG_PRESET_GAME)) {
            return HDRG_ERR_INVALID_ARG;
        }

        hdrg_params_init((hdrg_params_t *)(void *)(field - entry->offset), (uint8_t)value);

        return HDRG_OK;
    }

    switch (entry->kind) {
    case HDRG_SETTING_FLOAT:
        if (!(value == value)) {
            return HDRG_ERR_INVALID_ARG;
        }

        real = (float)value;
        memcpy(field, &real, sizeof(real));

        return HDRG_OK;
    case HDRG_SETTING_U8:
        if (!hdrg_setting_whole(value, 255.0)) {
            return HDRG_ERR_INVALID_ARG;
        }

        narrow = (uint8_t)value;
        memcpy(field, &narrow, sizeof(narrow));

        return HDRG_OK;
    case HDRG_SETTING_U16:
        if (!hdrg_setting_whole(value, 65535.0)) {
            return HDRG_ERR_INVALID_ARG;
        }

        half = (uint16_t)value;
        memcpy(field, &half, sizeof(half));

        return HDRG_OK;
    case HDRG_SETTING_U32:
        if (!hdrg_setting_whole(value, 4294967295.0)) {
            return HDRG_ERR_INVALID_ARG;
        }

        whole = (uint32_t)value;
        memcpy(field, &whole, sizeof(whole));

        return HDRG_OK;
    default:
        if (value != 0.0 && value != 1.0) {
            return HDRG_ERR_INVALID_ARG;
        }

        flag = value == 1.0;
        memcpy(field, &flag, sizeof(flag));

        return HDRG_OK;
    }
}

/* field of entry's kind as a number */
static double hdrg_setting_read(const hdrg_setting_entry_t *entry, const uint8_t *field)
{
    uint32_t whole;
    uint16_t half;
    uint8_t narrow;
    bool flag;
    float real;

    switch (entry->kind) {
    case HDRG_SETTING_FLOAT:
        memcpy(&real, field, sizeof(real));

        return (double)real;
    case HDRG_SETTING_U8:
        memcpy(&narrow, field, sizeof(narrow));

        return (double)narrow;
    case HDRG_SETTING_U16:
        memcpy(&half, field, sizeof(half));

        return (double)half;
    case HDRG_SETTING_U32:
        memcpy(&whole, field, sizeof(whole));

        return (double)whole;
    default:
        memcpy(&flag, field, sizeof(flag));

        return flag ? 1.0 : 0.0;
    }
}

extern void hdrg_params_init(hdrg_params_t *params, uint8_t preset)
{
    if (params == NULL) {
        return;
    }

    memset(params, 0, sizeof(*params));
    params->struct_size = (uint32_t)sizeof(*params);

    /*
     * Default philosophy: diffuse content rises as one, so
     * its surfaces keep their ratios (up to diffuse_max in a normally
     * exposed frame), and only lights and reflections rise above it toward
     * the peak. The average output stays within a quarter of the peak.
     * Shadows never drop below the SDR original; all gain is hue-preserving.
     */
    params->global_strength = 1.0f;
    params->diffuse_max = 2.0f;
    params->apl_budget_nits = 0.0f;
    params->apl_peak_fraction = 0.25f;
    params->contrast = 1.0f;
    params->clarity = 0.0f;
    params->saturation = 1.05f;
    params->highlight_desat = 0.4f;
    params->expand_strength = 0.7f;
    params->expand_reach = 0.85f;
    params->expand_skin = 0.9f;
    params->expand_memory = 0.5f;
    params->grain_policy = HDRG_GRAIN_AUTO;
    params->denoise_strength = -1.0f;
    params->grain_amount = 1.0f;
    params->deblock_strength = HDRG_PARAMS_BUILTIN(DEBLOCK, 1.0f);
    params->dering_strength = 0.0f;
    params->defringe_strength = 0.0f;
    params->deband_strength = HDRG_PARAMS_BUILTIN(DEBAND, 0.5f);
    params->mctf_strength = HDRG_PARAMS_BUILTIN(MCTF, 1.0f);
    params->effort = HDRG_EFFORT_AUTO;

    /* the same pipeline, other starting points */
    params->preset = (preset <= HDRG_PRESET_GAME) ? preset : HDRG_PRESET_PHOTO;
    params->sdr_eotf = HDRG_SDR_EOTF_AUTO;
    params->chroma_fit = HDRG_CHROMA_FIT_LINEAR;
    params->upscaler = HDRG_UPSCALER_LANCZOS;

    if (params->preset == HDRG_PRESET_ANIME) {
        params->expand_strength = 0.5f;
        params->expand_reach = 0.7f;
        params->expand_skin = 1.0f;
        params->expand_memory = 0.0f;
        params->saturation = 1.0f;
        params->highlight_desat = 0.15f;
        params->sdr_eotf = HDRG_SDR_EOTF_GAMMA22;
        params->chroma_fit = HDRG_CHROMA_FIT_SEGMENTED;
        params->upscaler = HDRG_PARAMS_UPSCALER;
        params->dering_strength = HDRG_PARAMS_BUILTIN(DERING, 2.0f);
        params->deband_strength = HDRG_PARAMS_BUILTIN(DEBAND, 2.5f);
        params->defringe_strength = HDRG_PARAMS_BUILTIN(DEFRINGE, 3.0f);
        params->colored_lights = true;
    } else if (params->preset == HDRG_PRESET_GAME) {
        params->expand_strength = 0.8f;
        params->expand_reach = 0.9f;
        params->expand_skin = 0.6f;
        params->expand_memory = 0.2f;
        params->highlight_desat = 0.3f;
        params->sdr_eotf = HDRG_SDR_EOTF_GAMMA22;
        params->upscaler = HDRG_PARAMS_UPSCALER;
        params->deblock_strength = 0.0f;
        params->deband_strength = 0.0f;
        params->mctf_strength = 0.0f;
    }
}

extern bool hdrg_params_resolve(const hdrg_params_t *params, hdrg_params_t *out)
{
    hdrg_params_init(out, HDRG_PRESET_PHOTO);

    if (params == NULL) {
        return true;
    }

    if (params->struct_size != sizeof(*out)) {
        return false;
    }

    memcpy(out, params, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);

    if (!(out->global_strength >= 0.0f && out->global_strength <= 1.0f)) {
        return false;
    }

    if (!(out->diffuse_max >= 1.0f && out->diffuse_max <= 4.0f)) {
        return false;
    }

    if (!(out->apl_budget_nits >= 0.0f && out->apl_budget_nits <= 10000.0f)) {
        return false;
    }

    if (!(out->apl_peak_fraction >= 0.0f && out->apl_peak_fraction <= 1.0f)) {
        return false;
    }

    if (!(out->contrast >= 0.25f && out->contrast <= 4.0f)) {
        return false;
    }

    if (!(out->clarity >= 0.0f && out->clarity <= 1.0f)) {
        return false;
    }

    if (!(out->saturation >= 0.0f && out->saturation <= 2.0f)) {
        return false;
    }

    if (!(out->expand_strength >= 0.0f && out->expand_strength <= 1.0f) ||
        !(out->expand_reach >= 0.0f && out->expand_reach <= 1.0f) ||
        !(out->expand_skin >= 0.0f && out->expand_skin <= 1.0f) ||
        !(out->expand_memory >= 0.0f && out->expand_memory <= 1.0f)) {
        return false;
    }

    if (out->preset > HDRG_PRESET_GAME || out->sdr_eotf > HDRG_SDR_EOTF_BT1886 ||
        out->chroma_fit > HDRG_CHROMA_FIT_SEGMENTED || out->upscaler > HDRG_UPSCALER_NIS) {
        return false;
    }

    if (!(out->light_predictor >= 0.0f && out->light_predictor <= 1.0f)) {
        return false;
    }

    if (!(out->highlight_desat >= 0.0f && out->highlight_desat <= 1.0f)) {
        return false;
    }

    if (out->grain_policy > HDRG_GRAIN_AUTO || !(out->denoise_strength <= 4.0f) ||
        !(out->grain_amount >= 0.0f && out->grain_amount <= 4.0f) ||
        !(out->deblock_strength >= 0.0f && out->deblock_strength <= 4.0f) ||
        !(out->dering_strength >= 0.0f && out->dering_strength <= 3.0f) ||
        !(out->defringe_strength >= 0.0f && out->defringe_strength <= 3.0f) ||
        !(out->deband_strength >= 0.0f && out->deband_strength <= 3.0f) ||
        !(out->mctf_strength >= 0.0f && out->mctf_strength <= 4.0f)) {
        return false;
    }

    return out->effort <= HDRG_EFFORT_QUALITY;
}

extern void hdrg_encode_params_init(hdrg_encode_params_t *encode)
{
    if (encode == NULL) {
        return;
    }

    memset(encode, 0, sizeof(*encode));
    encode->struct_size = (uint32_t)sizeof(*encode);
    encode->output_mode = HDRG_OUTPUT_AUTO;
    encode->gainmap_scale_shift = 1;
    encode->gainmap_headroom_cap_stops = 3.0f;
    encode->gainmap_gamma = 1.0f;
    encode->tonemap_knee = 0.75f;
    encode->dither = true;
}

extern void hdrg_display_caps_init(hdrg_display_caps_t *caps)
{
    if (caps == NULL) {
        return;
    }

    memset(caps, 0, sizeof(*caps));
    caps->struct_size = (uint32_t)sizeof(*caps);
    caps->headroom_stops = -1.0f;
    caps->gamut = HDRG_GAMUT_P3;
}

extern bool hdrg_encode_resolve(const hdrg_encode_params_t *encode, hdrg_encode_params_t *out)
{
    hdrg_encode_params_init(out);

    if (encode == NULL) {
        return true;
    }

    if (encode->struct_size != sizeof(*out)) {
        return false;
    }

    memcpy(out, encode, sizeof(*out));

    if (out->output_mode > HDRG_OUTPUT_PQ || out->gainmap_scale_shift > 4) {
        return false;
    }

    if (!(out->gainmap_headroom_cap_stops >= 0.0f && out->gainmap_headroom_cap_stops <= 5.62f) ||
        !(out->gainmap_gamma >= 0.2f && out->gainmap_gamma <= 5.0f) ||
        !(out->tonemap_knee >= 0.1f && out->tonemap_knee <= 0.99f)) {
        return false;
    }

    return out->output_width <= HDRG_MAX_DIMENSION && out->output_height <= HDRG_MAX_DIMENSION;
}

extern bool hdrg_display_resolve(const hdrg_display_caps_t *caps, hdrg_display_caps_t *out)
{
    hdrg_display_caps_init(out);

    if (caps == NULL) {
        return true;
    }

    if (caps->struct_size != sizeof(*out)) {
        return false;
    }

    memcpy(out, caps, sizeof(*out));

    return out->headroom_stops <= 16.0f && out->sdr_white_nits >= 0.0f &&
           out->sdr_white_nits <= 10000.0f && out->peak_nits >= 0.0f &&
           out->peak_nits <= 10000.0f && out->max_full_frame_nits >= 0.0f &&
           out->max_full_frame_nits <= 10000.0f && out->gamut <= HDRG_GAMUT_BT2020;
}

extern bool hdrg_setting_at(uint32_t index, hdrg_setting_t *out)
{
    if (out == NULL || index >= sizeof(hdrg_setting_table) / sizeof(hdrg_setting_table[0])) {
        return false;
    }

    out->name = hdrg_setting_table[index].name;
    out->part = hdrg_setting_table[index].part;
    out->kind = hdrg_setting_table[index].kind;
    out->values = hdrg_setting_table[index].count;

    return true;
}

extern hdrg_status_t hdrg_setting_set(const char *name, double value, hdrg_params_t *params,
                                      hdrg_display_caps_t *display, hdrg_encode_params_t *encode)
{
    const hdrg_setting_entry_t *entry;
    uint8_t *field;

    if (name == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    entry = hdrg_setting_find(name);
    if (entry == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    field = hdrg_setting_field(entry, params, display, encode, NULL);

    return (field != NULL) ? hdrg_setting_write(entry, field, value) : HDRG_ERR_INVALID_ARG;
}

extern hdrg_status_t hdrg_setting_get(const char *name, const hdrg_params_t *params,
                                      const hdrg_display_caps_t *display,
                                      const hdrg_encode_params_t *encode, double *value)
{
    const hdrg_setting_entry_t *entry;
    const uint8_t *field;

    if (name == NULL || value == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    entry = hdrg_setting_find(name);
    if (entry == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    field = hdrg_setting_field(entry, params, display, encode, NULL);
    if (field == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *value = hdrg_setting_read(entry, field);

    return HDRG_OK;
}

extern bool hdrg_setting_value_at(const char *name, uint32_t index, const char **text,
                                  double *value)
{
    const hdrg_setting_entry_t *entry;

    entry = (name != NULL) ? hdrg_setting_find(name) : NULL;
    if (entry == NULL || index >= entry->count || text == NULL || value == NULL) {
        return false;
    }

    *text = entry->values[index].name;
    *value = (double)entry->values[index].value;

    return true;
}

extern const char *hdrg_setting_value_name(const char *name, double value)
{
    const hdrg_setting_entry_t *entry;
    uint8_t i;

    entry = (name != NULL) ? hdrg_setting_find(name) : NULL;

    for (i = 0; entry != NULL && i < entry->count; i++) {
        if ((double)entry->values[i].value == value) {
            return entry->values[i].name;
        }
    }

    return NULL;
}

extern hdrg_status_t hdrg_setting_parse(const char *name, const char *text, double *value)
{
    const hdrg_setting_entry_t *entry;
    uint8_t i;
    size_t len;

    if (name == NULL || text == NULL || value == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    entry = hdrg_setting_find(name);
    if (entry == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    for (i = 0; i < entry->count; i++) {
        if (strcmp(text, entry->values[i].name) == 0) {
            *value = (double)entry->values[i].value;

            return HDRG_OK;
        }
    }

    /* a number otherwise (the C locale's: hdrg_parse_float) */
    len = strlen(text);

    return hdrg_parse_float(text, len, value) ? HDRG_OK : HDRG_ERR_INVALID_ARG;
}

extern hdrg_status_t hdrg_session_setting_set(const char *name, double value,
                                              hdrg_session_config_t *config)
{
    const hdrg_setting_entry_t *entry;
    uint8_t *field;

    if (name == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    entry = hdrg_setting_find(name);
    if (entry == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    field = hdrg_setting_field(entry, NULL, NULL, NULL, config);

    return (field != NULL) ? hdrg_setting_write(entry, field, value) : HDRG_ERR_INVALID_ARG;
}

extern hdrg_status_t hdrg_session_setting_get(const char *name, const hdrg_session_config_t *config,
                                              double *value)
{
    const hdrg_setting_entry_t *entry;
    const uint8_t *field;

    if (name == NULL || value == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    entry = hdrg_setting_find(name);
    if (entry == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    field = hdrg_setting_field(entry, NULL, NULL, NULL, config);
    if (field == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *value = hdrg_setting_read(entry, field);

    return HDRG_OK;
}

extern float hdrg_display_caps_peak(const hdrg_display_caps_t *caps)
{
    hdrg_display_caps_t resolved;

    if (!hdrg_display_resolve(caps, &resolved)) {
        return 0.0f;
    }

    return hdrg_render_peak_bound(&resolved);
}
