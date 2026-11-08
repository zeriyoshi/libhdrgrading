/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/* no platform encoder (Linux): a video is written by hdrg-ffmpeg when FFmpeg is opted in */

#include <stdio.h>

#include "gui.h"

extern bool hdrg_gui_encoder_available(void)
{
    return false;
}

extern bool hdrg_gui_encoder_open(const char *path, uint32_t width, uint32_t height,
                                  uint32_t rate_num, uint32_t rate_den, float peak_nits,
                                  hdrg_gui_encoder_t **out, char *error, size_t error_size)
{
    (void)path;
    (void)width;
    (void)height;
    (void)rate_num;
    (void)rate_den;
    (void)peak_nits;
    *out = NULL;
    snprintf(error, error_size, "no video encoder on this platform");

    return false;
}

extern bool hdrg_gui_encoder_write(hdrg_gui_encoder_t *encoder, const uint8_t *luma,
                                   const uint8_t *chroma, int64_t pts_us)
{
    (void)encoder;
    (void)luma;
    (void)chroma;
    (void)pts_us;

    return false;
}

extern bool hdrg_gui_encoder_close(hdrg_gui_encoder_t *encoder, bool keep, char *error,
                                   size_t error_size)
{
    (void)encoder;
    (void)keep;
    (void)error;
    (void)error_size;

    return false;
}
