/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * A program of the installed library: built by tests/install/run.cmake through the CMake package
 * and through pkg-config, from the installed headers alone. It reaches a function of each public
 * header and converts an image (the bundled decoders and encoder are in the library).
 */
#include <stdio.h>
#include <string.h>

#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/plugin.h"
#include "hdrgrading/version.h"
#include "hdrgrading/video.h"

#define CONSUMER_SIZE 16u

int main(void)
{
    hdrg_raw_image_t image;
    hdrg_context_desc_t desc;
    hdrg_session_config_t config;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t x, y;
    uint8_t pixels[CONSUMER_SIZE * CONSUMER_SIZE * 3u], *output;
    size_t output_size;

    if (strcmp(hdrg_version(), HDRG_VERSION_STRING) != 0) {
        fprintf(stderr, "version %s, header %s\n", hdrg_version(), HDRG_VERSION_STRING);

        return 1;
    }

    hdrg_context_desc_init(&desc);
    desc.thread_count = 1;
    status = hdrg_context_create(&desc, &ctx);
    if (status != HDRG_OK) {
        fprintf(stderr, "hdrg_context_create: %s\n", hdrg_status_string(status));

        return 1;
    }

    if (hdrg_context_filter_count(ctx) == 0) {
        fprintf(stderr, "no built-in filters\n");
        hdrg_context_destroy(ctx);

        return 1;
    }

    hdrg_session_config_init(&config);

    for (y = 0; y < CONSUMER_SIZE; y++) {
        for (x = 0; x < CONSUMER_SIZE; x++) {
            pixels[(y * CONSUMER_SIZE + x) * 3u] = (uint8_t)(x * 16u);
            pixels[(y * CONSUMER_SIZE + x) * 3u + 1u] = (uint8_t)(y * 16u);
            pixels[(y * CONSUMER_SIZE + x) * 3u + 2u] = (uint8_t)((x + y) * 8u);
        }
    }

    memset(&image, 0, sizeof(image));
    image.pixels = pixels;
    image.width = CONSUMER_SIZE;
    image.height = CONSUMER_SIZE;
    image.bit_depth = 8;
    image.stride = CONSUMER_SIZE * 3u;
    image.primaries = HDRG_PRIMARIES_BT709;
    image.transfer = HDRG_TRANSFER_SRGB;

    status = hdrg_convert_raw(ctx, &image, NULL, NULL, NULL, &output, &output_size);
    if (status != HDRG_OK || output_size < 4 || output[0] != 0xFF || output[1] != 0xD8) {
        fprintf(stderr, "hdrg_convert_raw: %s\n", hdrg_status_string(status));
        hdrg_context_destroy(ctx);

        return 1;
    }

    printf("%s: %zu bytes of JPEG\n", hdrg_version(), output_size);

    hdrg_free(output);
    hdrg_context_destroy(ctx);

    return 0;
}
