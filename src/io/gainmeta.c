/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * The gain map's metadata the writers carry (the API table's write_iso21496
 * and write_apple_xmp): ISO 21496-1's GainMapMetadata and Apple's
 * HDRGainMap XMP.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hdrgrading/foundation/math.h"
#include "io.h"

#define HDRG_RATIONAL_DEN 1000000

/*
 * a signed / unsigned rational with the fixed 10^6 denominator: 8 bytes, big-endian, at out
 * (the value as a float)
 */
/* value to 6 decimals whatever the locale (printf's %f follows LC_NUMERIC) */
static inline void hdrg_xmp_fixed(double value, char out[32])
{
    uint64_t scaled;
    bool negative;

    negative = value < 0.0;
    scaled = (uint64_t)((negative ? -value : value) * 1000000.0 + 0.5);
    snprintf(out, 32, "%s%" PRIu64 ".%06" PRIu64, negative ? "-" : "", scaled / 1000000u,
             scaled % 1000000u);
}

static void hdrg_put_srational(uint8_t *out, double value)
{
    const uint32_t n = (uint32_t)(int32_t)lrintf((float)value * (float)HDRG_RATIONAL_DEN),
                   d = HDRG_RATIONAL_DEN;
    uint8_t i;

    for (i = 0; i < 4u; i++) {
        out[i] = (uint8_t)(n >> (24u - 8u * i));
        out[4u + i] = (uint8_t)(d >> (24u - 8u * i));
    }
}

extern void hdrg_gainmap_iso_body(const double values[HDRG_GAINMAP_VALUES],
                                  uint8_t out[HDRG_ISO_BODY_BYTES])
{
    out[0] = 0; /* minimum_version */
    out[1] = 0;
    out[2] = 0; /* writer_version */
    out[3] = 0;
    out[4] = 0x40; /* use_base_colour_space, 1ch */
    hdrg_put_srational(out + 5, values[HDRG_GAINMAP_BASE_HEADROOM]);
    hdrg_put_srational(out + 13, values[HDRG_GAINMAP_ALT_HEADROOM]);
    hdrg_put_srational(out + 21, values[HDRG_GAINMAP_MIN]);
    hdrg_put_srational(out + 29, values[HDRG_GAINMAP_MAX]);
    hdrg_put_srational(out + 37, values[HDRG_GAINMAP_GAMMA]);
    hdrg_put_srational(out + 45, values[HDRG_GAINMAP_BASE_OFFSET]);
    hdrg_put_srational(out + 53, values[HDRG_GAINMAP_ALT_OFFSET]);
}

extern size_t hdrg_gainmap_apple_xmp(double alternate_headroom, char *out, size_t size)
{
    int n;
    char headroom[32];

    hdrg_xmp_fixed((double)hdrg_math_exp2f((float)alternate_headroom), headroom);
    n = snprintf(out, size,
                 "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"libhdrgrading\">\n"
                 "   <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                 "      <rdf:Description rdf:about=\"\"\n"
                 "            xmlns:apdi=\"http://ns.apple.com/pixeldatainfo/1.0/\"\n"
                 "            xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">\n"
                 "         <apdi:NativeFormat>1278226488</apdi:NativeFormat>\n"
                 "         <apdi:AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap"
                 "</apdi:AuxiliaryImageType>\n"
                 "         <apdi:StoredFormat>1278226488</apdi:StoredFormat>\n"
                 "         <HDRGainMap:HDRGainMapVersion>131072"
                 "</HDRGainMap:HDRGainMapVersion>\n"
                 "         <HDRGainMap:HDRGainMapHeadroom>%s"
                 "</HDRGainMap:HDRGainMapHeadroom>\n"
                 "      </rdf:Description>\n"
                 "   </rdf:RDF>\n"
                 "</x:xmpmeta>",
                 headroom);

    return (n > 0 && (size_t)n < size) ? (size_t)n : 0u;
}
