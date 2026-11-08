/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrgrading/gpu/apple.h"

#include <string.h>

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>

/* a CoreVideo Y'CbCr pixel format and the pixels it holds */
typedef struct {
    OSType type;
    uint8_t layout, bits;
    bool interleaved, msb, full_range;
} hdrg_apple_format_t;

static const hdrg_apple_format_t hdrg_apple_formats[] = {
    {kCVPixelFormatType_420YpCbCr8Planar, HDRG_YUV_420, 8, false, false, false},
    {kCVPixelFormatType_420YpCbCr8PlanarFullRange, HDRG_YUV_420, 8, false, false, true},
    {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, HDRG_YUV_420, 8, true, false, false},
    {kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, HDRG_YUV_420, 8, true, false, true},
    {kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, HDRG_YUV_422, 8, true, false, false},
    {kCVPixelFormatType_422YpCbCr8BiPlanarFullRange, HDRG_YUV_422, 8, true, false, true},
    {kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange, HDRG_YUV_444, 8, true, false, false},
    {kCVPixelFormatType_444YpCbCr8BiPlanarFullRange, HDRG_YUV_444, 8, true, false, true},
    {kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, HDRG_YUV_420, 10, true, true, false},
    {kCVPixelFormatType_420YpCbCr10BiPlanarFullRange, HDRG_YUV_420, 10, true, true, true},
    {kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, HDRG_YUV_422, 10, true, true, false},
    {kCVPixelFormatType_422YpCbCr10BiPlanarFullRange, HDRG_YUV_422, 10, true, true, true},
    {kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, HDRG_YUV_444, 10, true, true, false},
    {kCVPixelFormatType_444YpCbCr10BiPlanarFullRange, HDRG_YUV_444, 10, true, true, true},
    {kCVPixelFormatType_422YpCbCr16BiPlanarVideoRange, HDRG_YUV_422, 16, true, false, false},
    {kCVPixelFormatType_444YpCbCr16BiPlanarVideoRange, HDRG_YUV_444, 16, true, false, false},
};

extern bool hdrg_apple_surface(void *pixel_buffer, hdrg_apple_surface_t *out)
{
    const hdrg_apple_format_t *format;
    CVPixelBufferRef buffer = (CVPixelBufferRef)pixel_buffer;
    IOSurfaceRef surface;
    OSType type;
    uint32_t i;
    uint8_t p;

    memset(out, 0, sizeof(*out));

    if (buffer == NULL || CFGetTypeID(buffer) != CVPixelBufferGetTypeID()) {
        return false;
    }

    surface = CVPixelBufferGetIOSurface(buffer);
    type = CVPixelBufferGetPixelFormatType(buffer);
    format = NULL;

    for (i = 0; i < sizeof(hdrg_apple_formats) / sizeof(hdrg_apple_formats[0]); i++) {
        if (hdrg_apple_formats[i].type == type) {
            format = &hdrg_apple_formats[i];
        }
    }

    if (surface == NULL || format == NULL ||
        IOSurfaceGetPlaneCount(surface) != (format->interleaved ? 2u : 3u)) {
        return false;
    }

    out->surface = (void *)surface;
    out->pixfmt.layout = format->layout;
    out->pixfmt.bits = format->bits;
    out->pixfmt.interleaved = format->interleaved;
    out->pixfmt.msb = format->msb;
    out->pixfmt.full_range = format->full_range;
    out->planes = format->interleaved ? 2u : 3u;

    for (p = 0; p < out->planes; p++) {
        out->widths[p] = (uint32_t)IOSurfaceGetWidthOfPlane(surface, p);
        out->heights[p] = (uint32_t)IOSurfaceGetHeightOfPlane(surface, p);
        out->bytes[p] = (uint8_t)IOSurfaceGetBytesPerElementOfPlane(surface, p);
        out->components[p] = (format->interleaved && p == 1u) ? 2u : 1u;

        if (out->bytes[p] != out->components[p] * ((format->bits > 8u) ? 2u : 1u)) {
            return false;
        }
    }

    return true;
}
