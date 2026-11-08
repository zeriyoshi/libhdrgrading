/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_IO_H
#define HDRG_IO_H

#include <stdint.h>

#include "hdrgrading/filter.h"

#include "../core/color.h"
#include "../core/exec.h"
#include "../priv.h"

/*
 * Decoded image: interleaved RGB (no alpha) code values of `color`,
 * bit_depth 8 or 16 (16 = native-endian uint16_t). All buffers are owned
 * and allocated through the given allocator. A codec's reader fills it
 * (src/host/codec_host.c resolves the color: ICC (lcms2 converts the pixels
 * of a profile needing a CMM into BT.2020 primaries with a 2.2 gamma at 16
 * bits), else the reader's, else EXIF ColorSpace / Interop "R03" (Adobe
 * RGB), else sRGB). exif keeps the "Exif\0\0" prefix of a JPEG's APP1.
 */
typedef struct {
    hdrg_color_t color;
    uint8_t *pixels;
    uint8_t *icc;
    uint8_t *exif;
    uint32_t width, height, bit_depth;
    uint16_t orientation; /* EXIF 1..8, 0 = absent */
    size_t stride, icc_size, exif_size;
} hdrg_decoded_t;

extern void hdrg_decoded_free(const hdrg_allocator_t *alloc, hdrg_decoded_t *decoded);

/*
 * convert.c: input through the built-in readers (JPEG, PNG, GIF) of a host made for the call (the
 * codecs' plugins alone registered), on exec (NULL: the calling thread): tools and tests.
 * hdrg_decode_with: through the readers of ctx's host (its plugins registered already, the
 * modules it loaded too), on exec
 */
extern hdrg_status_t hdrg_decode_any(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                     const uint8_t *input, size_t input_size, uint64_t max_pixels,
                                     uint8_t fit, hdrg_decoded_t *decoded);
extern hdrg_status_t hdrg_decode_with(hdrg_context_t *ctx, const hdrg_exec_t *exec,
                                      const uint8_t *input, size_t input_size, uint64_t max_pixels,
                                      uint8_t fit, hdrg_decoded_t *decoded);

/*
 * icc.c. hdrg_icc_color reads the color of a profile without a CMM: the
 * v4.4 cicp tag when its code points are supported, else RGB matrix / TRC
 * or gray TRC; HDRG_ERR_ICC_UNSUPPORTED when it needs one (LUT-based).
 * hdrg_decoded_icc_color resolves decoded->icc into decoded->color,
 * converting the pixels with lcms2 when a CMM is needed.
 */
extern hdrg_status_t hdrg_icc_color(const hdrg_allocator_t *alloc, const uint8_t *icc, size_t size,
                                    hdrg_color_t *out);
extern hdrg_status_t hdrg_decoded_icc_color(const hdrg_allocator_t *alloc, hdrg_decoded_t *decoded);
/* exif: TIFF stream (after the "Exif\0\0" header) */
extern uint16_t hdrg_exif_orientation(const uint8_t *exif, size_t size);
extern bool hdrg_exif_set_orientation(uint8_t *exif, size_t size, uint16_t value);
/* ColorSpace 0xFFFF with Interop index "R03" (DCF Adobe RGB), or the non-standard 2 */
extern bool hdrg_exif_adobe_rgb(const uint8_t *exif, size_t size);
#ifdef HDRG_WITH_LCMS2
/* Serialized Display P3 profile for P3 base encodes. */
extern hdrg_status_t hdrg_icc_create_p3(const hdrg_allocator_t *alloc, uint8_t **out,
                                        size_t *out_size);
/* Serialized "P3 PQ" HDR profile (PQ curve + cicp tag). lumi_nits > 0 adds
 * a luminance tag (Apple-only EDR reference white hint); hagc != NULL adds
 * the ST 2094-50 payload as an ICC 'HAGC' tag. */
extern hdrg_status_t hdrg_icc_create_p3_pq(const hdrg_allocator_t *alloc, float lumi_nits,
                                           const uint8_t *hagc, size_t hagc_size, uint8_t **out,
                                           size_t *out_size);
#endif

/*
 * gainmeta.c: the gain map's ISO 21496-1 GainMapMetadata (1 channel, the base's color space,
 * rationals over 10^6) of its values (HDRG_GAINMAP_*): a JPEG's after its urn, a HEIF tmap item's
 * after its version byte, a JPEG XL jhgm box's; and Apple's HDRGainMap XMP packet (no signature)
 * into out, 0 when it does not fit
 */
#define HDRG_ISO_BODY_BYTES 61u
extern void hdrg_gainmap_iso_body(const double values[HDRG_GAINMAP_VALUES],
                                  uint8_t out[HDRG_ISO_BODY_BYTES]);
extern size_t hdrg_gainmap_apple_xmp(double alternate_headroom, char *out, size_t size);

#endif /* HDRG_IO_H */
