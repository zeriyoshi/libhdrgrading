/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/plugin.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * the context every input converts in, made at the first (its plugins registered once; a
 * conversion keeps nothing of the one before but the writer, set for each input) and the JPEG XL
 * writer's options
 */
static hdrg_context_t *g_ctx;
static hdrg_options_t *g_jxl;

/*
 * inputs of up to 16 MP: a header claiming more fails before its pixels are allocated. One
 * thread: the work splits the same way for any count, and workers would only contend on the
 * shared coverage counters (ten times slower with 32 threads)
 */
static bool fuzz_context(void)
{
    hdrg_context_desc_t desc;

    if (g_ctx != NULL) {
        return true;
    }

    hdrg_context_desc_init(&desc);
    desc.max_pixels = UINT64_C(1) << 24;
    desc.thread_count = 1;

    if (hdrg_context_create(&desc, &g_ctx) != HDRG_OK) {
        g_ctx = NULL;

        return false;
    }

    /* JPEG XL at its fastest effort where the build has it */
    if (hdrg_options_create(g_ctx, &g_jxl) == HDRG_OK &&
        hdrg_options_set(g_jxl, "effort", "1") != HDRG_OK) {
        hdrg_options_destroy(g_jxl);
        g_jxl = NULL;
    }

    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    hdrg_encode_params_t encode;
    hdrg_params_t params;
    uint8_t *output;
    size_t output_size;
    bool jxl;

    if (size == 0 || size > (16u << 20) || !fuzz_context()) {
        return 0;
    }

    /*
     * odd sizes as ANIME: its segmented chroma fit and its prefilter chain. The size's next two
     * bits pick the form (the writer's own, GAINMAP, HYBRID, PQ), the one after them the writer:
     * JPEG, or JPEG XL where the build has it
     */
    hdrg_params_init(&params, (size % 2u == 1u) ? HDRG_PRESET_ANIME : HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    encode.output_mode = (uint8_t)((size >> 1) % 4u);
    jxl = (size >> 3) % 2u == 1u && g_jxl != NULL &&
          hdrg_context_set_writer(g_ctx, "hdrg.jxl", g_jxl) == HDRG_OK;

    if (!jxl) {
        (void)hdrg_context_set_writer(g_ctx, NULL, NULL);
    }

    if (hdrg_convert(g_ctx, data, size, &params, NULL, &encode, &output, &output_size) == HDRG_OK) {
        hdrg_free(output);
    }

    return 0;
}
