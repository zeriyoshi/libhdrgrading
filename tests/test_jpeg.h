/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_TEST_JPEG_H
#define HDRG_TEST_JPEG_H

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lib/jpegli/encode.h>

#include "priv.h"

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
} hdrg_test_jpeg_error_t;

static inline void hdrg_test_jpeg_error_exit(j_common_ptr cinfo)
{
    longjmp(((hdrg_test_jpeg_error_t *)cinfo->err)->jump, 1);
}

/*
 * An 8-bit RGB JPEG of pixels (jpegli as the library's writer sets it: 4:4:4, no adaptive
 * quantization, a JFIF APP0) with an ICC profile and an EXIF block (with its "Exif\0\0" prefix)
 * when given, after the APP0: the inputs of the tests and of fuzz/fuzz_seeds.c, *out
 * hdrg_mem_alloc'd from alloc
 */
static inline hdrg_status_t hdrg_test_jpeg_rgb(const hdrg_allocator_t *alloc, const uint8_t *pixels,
                                               uint32_t width, uint32_t height, size_t stride,
                                               uint8_t quality, const uint8_t *icc, size_t icc_size,
                                               const uint8_t *exif, size_t exif_size, uint8_t **out,
                                               size_t *out_size)
{
    struct jpeg_compress_struct cinfo;
    hdrg_test_jpeg_error_t error;
    JSAMPROW row;
    unsigned long mem_size;
    uint32_t y;
    unsigned char *mem;

    *out = NULL;
    *out_size = 0;
    mem = NULL;
    mem_size = 0;

    cinfo.err = jpegli_std_error(&error.mgr);
    error.mgr.error_exit = hdrg_test_jpeg_error_exit;

    if (setjmp(error.jump)) {
        jpegli_destroy_compress(&cinfo);
        free(mem);

        return HDRG_ERR_ENCODE;
    }

    jpegli_create_compress(&cinfo);
    jpegli_mem_dest(&cinfo, &mem, &mem_size);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpegli_set_defaults(&cinfo);
    jpegli_set_quality(&cinfo, quality, TRUE);
    cinfo.write_JFIF_header = TRUE;
    cinfo.comp_info[0].h_samp_factor = 1;
    cinfo.comp_info[0].v_samp_factor = 1;
    jpegli_enable_adaptive_quantization(&cinfo, FALSE);
    jpegli_start_compress(&cinfo, TRUE);

    if (icc != NULL && icc_size > 0) {
        jpegli_write_icc_profile(&cinfo, icc, (unsigned int)icc_size);
    }

    if (exif != NULL && exif_size > 0) {
        jpegli_write_marker(&cinfo, JPEG_APP0 + 1, exif, (unsigned int)exif_size);
    }

    for (y = 0; y < height; y++) {
        row = (JSAMPROW)(pixels + (size_t)y * stride);
        (void)jpegli_write_scanlines(&cinfo, &row, 1);
    }

    jpegli_finish_compress(&cinfo);
    jpegli_destroy_compress(&cinfo);

    *out = (uint8_t *)hdrg_mem_alloc(alloc, mem_size);
    if (*out != NULL) {
        memcpy(*out, mem, mem_size);
        *out_size = mem_size;
    }

    free(mem);

    return (*out != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
}

#endif
