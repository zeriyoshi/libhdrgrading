/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "common.h"

#include <stdio.h>
#include <string.h>

/* the part of the setting name (UINT8_MAX: none of that name) */
static uint8_t tools_part(const char *name)
{
    hdrg_setting_t setting;
    uint32_t i;

    for (i = 0; hdrg_setting_at(i, &setting); i++) {
        if (strcmp(setting.name, name) == 0) {
            return setting.part;
        }
    }

    return UINT8_MAX;
}

/* the names of setting name's values, "a, b or c" */
static void tools_expected(const char *name, char *out, size_t size)
{
    const char *text;
    uint32_t i, count;
    size_t at;
    double value;

    for (count = 0; hdrg_setting_value_at(name, count, &text, &value); count++) {
    }

    at = 0;
    out[0] = '\0';

    for (i = 0; i < count && hdrg_setting_value_at(name, i, &text, &value); i++) {
        at += (size_t)snprintf(out + at, (at < size) ? size - at : 0, "%s%s",
                               (i == 0) ? "" : ((i + 1u == count) ? " or " : ", "), text);
    }
}

extern bool tools_filter(tools_filter_t *filters, uint32_t *count, const char *spec)
{
    if (*count >= TOOLS_MAX_FILTERS) {
        fprintf(stderr, "--filter: at most %u filters\n", TOOLS_MAX_FILTERS);

        return false;
    }

    filters[*count].spec = spec;
    filters[*count].opt_count = 0;
    (*count)++;

    return true;
}

extern bool tools_filter_opt(tools_filter_t *filters, uint32_t count, const char *opt)
{
    if (count == 0 || filters[count - 1u].opt_count >= TOOLS_MAX_OPTS) {
        fprintf(stderr, "--filter-opt: after a --filter, at most %u each\n", TOOLS_MAX_OPTS);

        return false;
    }

    filters[count - 1u].opts[filters[count - 1u].opt_count++] = opt;

    return true;
}

extern bool tools_add_filters(hdrg_context_t *ctx, const tools_filter_t *filters, uint32_t count,
                              uint32_t stages)
{
    static const uint32_t kinds[3] = {HDRG_STAGE_SCALE, HDRG_STAGE_PREFILTER, HDRG_STAGE_INTERP};
    static const char *const prefixes[3] = {"scale=", "prefilter=", "interp="};
    const char *id, *value;
    hdrg_options_t *options;
    hdrg_status_t status;
    uint32_t i, j, k, stage;
    size_t key_len;
    char key[TOOLS_MAX_KEY + 1];

    for (i = 0; i < count; i++) {
        stage = 0;
        id = NULL;

        for (k = 0; k < 3u && id == NULL; k++) {
            if ((stages & kinds[k]) != 0 &&
                strncmp(filters[i].spec, prefixes[k], strlen(prefixes[k])) == 0) {
                stage = kinds[k];
                id = filters[i].spec + strlen(prefixes[k]);
            }
        }

        if (id == NULL) {
            fprintf(stderr, "--filter: expected scale=<id>, prefilter=<id>%s: %s\n",
                    ((stages & HDRG_STAGE_INTERP) != 0) ? " or interp=<id>" : "", filters[i].spec);

            return false;
        }

        status = hdrg_options_create(ctx, &options);
        if (status != HDRG_OK) {
            fprintf(stderr, "--filter: %s\n", hdrg_status_string(status));

            return false;
        }

        for (j = 0; j < filters[i].opt_count; j++) {
            value = strchr(filters[i].opts[j], '=');
            key_len = (value != NULL) ? (size_t)(value - filters[i].opts[j]) : 0;
            if (value == NULL || key_len == 0 || key_len > TOOLS_MAX_KEY) {
                fprintf(stderr, "--filter-opt: expected <key>=<value>: %s\n", filters[i].opts[j]);
                hdrg_options_destroy(options);

                return false;
            }

            memcpy(key, filters[i].opts[j], key_len);
            key[key_len] = '\0';

            status = hdrg_options_set(options, key, value + 1);
            if (status != HDRG_OK) {
                fprintf(stderr, "--filter-opt %s: %s\n", filters[i].opts[j],
                        hdrg_context_error(ctx));
                hdrg_options_destroy(options);

                return false;
            }
        }

        status = hdrg_context_add_filter(ctx, stage, id, options);
        hdrg_options_destroy(options);

        if (status != HDRG_OK) {
            fprintf(stderr, "--filter %s: %s (%s)\n", filters[i].spec, hdrg_context_error(ctx),
                    hdrg_status_string(status));

            return false;
        }
    }

    return true;
}

extern bool tools_set(const char *arg, hdrg_params_t *params, hdrg_display_caps_t *display,
                      hdrg_encode_params_t *encode, hdrg_session_config_t *session)
{
    const char *eq;
    hdrg_status_t status;
    uint8_t part;
    size_t length;
    char name[TOOLS_MAX_KEY + 1];
    double value;

    eq = strchr(arg, '=');
    if (eq == NULL || eq == arg || (size_t)(eq - arg) >= sizeof(name)) {
        fprintf(stderr, "--set: expected <name>=<value>: %s\n", arg);

        return false;
    }

    length = (size_t)(eq - arg);
    memcpy(name, arg, length);
    name[length] = '\0';
    part = tools_part(name);

    status = hdrg_setting_parse(name, eq + 1, &value);
    if (status == HDRG_OK) {
        status = (part == HDRG_SETTING_SESSION)
                     ? hdrg_session_setting_set(name, value, session)
                     : hdrg_setting_set(name, value, params, display, encode);
    }

    if (status != HDRG_OK) {
        fprintf(stderr, "--set %s: %s\n", arg,
                (status == HDRG_ERR_NOT_FOUND) ? "no such setting"
                                               : "not a value of the setting here");

        return false;
    }

    return true;
}

extern bool tools_named(const char *option, const char *name, const char *text, uint8_t *out)
{
    char expected[256];
    double value;

    if (hdrg_setting_parse(name, text, &value) == HDRG_OK && value >= 0.0 && value <= 255.0 &&
        value == (double)(uint8_t)value) {
        *out = (uint8_t)value;

        return true;
    }

    tools_expected(name, expected, sizeof(expected));
    fprintf(stderr, "%s: expected %s: %s\n", option, expected, text);

    return false;
}
