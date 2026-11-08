/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "defaults.h"

#include <stdio.h>
#include <string.h>

#define HDRG_DEFAULT_KINDS 3u  /* the GPU kinds of the built-in classes: Metal, Vulkan, WebGPU */
#define HDRG_DEFAULT_ID    48u /* a GPU version's id's bytes */

const hdrg_default_t hdrg_defaults[HDRG_DEFAULTS] = {
    {"hdrg.deblock", "deblock"},
    {"hdrg.dering", "dering"},
    {"hdrg.defringe", "defringe"},
    {"hdrg.deband", "deband"},
    {"hdrg.mctf", "mctf"},
    {"hdrg.denoise.spatial", "denoise"},
    {"hdrg.scale.lanczos", "lanczos"},
    {"hdrg.scale.edge", "edge"},
    {"hdrg.scale.fsr1", "fsr1"},
    {"hdrg.scale.nis", "nis"},
    {"hdrg.interp.mc", "interp.mc"},
    {"hdrg.stats.temporal", NULL},
    {"hdrg.analyze.predict", NULL},
    {"hdrg.jpeg", NULL},
};

extern const hdrg_class_t *hdrg_default_find(const hdrg_plugin_host_t *host, uint8_t d)
{
    return hdrg_host_find_class(host, hdrg_defaults[d].id, strlen(hdrg_defaults[d].id));
}

extern const hdrg_class_t *hdrg_default_class(const hdrg_plugin_host_t *host, uint8_t d, bool gpu,
                                              uint32_t kind, uint8_t *at, bool *cpu)
{
    const hdrg_default_t *def = &hdrg_defaults[d];
    const hdrg_class_t *cls;
    static const uint32_t kinds[HDRG_DEFAULT_KINDS] = {HDRG_DEVICE_METAL, HDRG_DEVICE_VULKAN,
                                                       HDRG_DEVICE_WEBGPU};
    static const char *const prefixes[HDRG_DEFAULT_KINDS] = {"hdrg.metal.", "hdrg.vulkan.",
                                                             "hdrg.webgpu."};
    uint32_t order[HDRG_DEFAULT_KINDS];
    uint8_t n, k;
    int length;
    char id[HDRG_DEFAULT_ID];

    *cpu = false;

    /* the kinds in order: kind first, then the rest */
    n = 0;

    for (k = 0; k < HDRG_DEFAULT_KINDS; k++) {
        if (kinds[k] == kind) {
            order[n++] = k;
        }
    }

    for (k = 0; k < HDRG_DEFAULT_KINDS; k++) {
        if (kinds[k] != kind) {
            order[n++] = k;
        }
    }

    for (; gpu && def->gpu != NULL && *at < HDRG_DEFAULT_KINDS; (*at)++) {
        length = snprintf(id, sizeof(id), "%s%s", prefixes[order[*at]], def->gpu);
        cls = (length > 0 && (size_t)length < sizeof(id))
                  ? hdrg_host_find_class(host, id, (size_t)length)
                  : NULL;

        if (cls != NULL) {
            (*at)++;

            return cls;
        }
    }

    if (*at > HDRG_DEFAULT_KINDS) {
        return NULL;
    }

    *at = HDRG_DEFAULT_KINDS + 1u;
    *cpu = true;

    return hdrg_default_find(host, d);
}
