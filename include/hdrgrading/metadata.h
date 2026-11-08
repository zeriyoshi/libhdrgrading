/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading — dynamic HDR metadata of an output (a session's frame, a
 * still's HAGC): HDR10+ (SMPTE ST 2094-40) and SMPTE ST 2094-50, and their
 * ITU-T T.35 payloads
 */
#ifndef HDRGRADING_METADATA_H
#define HDRGRADING_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HDRG_HDR10PLUS_PERCENTILES 9u /* at 1, 5, 10, 25, 50, 75, 90, 95 and 99 % */

/*
 * A frame's HDR10+ statistics (SMPTE ST 2094-40 application 4) of its
 * output: one window, the whole frame; linear cd/m² of
 * the output's components (its gamut's R, G, B: BT.2020 in its container)
 */
typedef struct {
    float maxscl[3];                               /* R's, G's and B's largest */
    float average_maxrgb;                          /* the mean max(R, G, B) (avg_nits) */
    float percentiles[HDRG_HDR10PLUS_PERCENTILES]; /* max(R, G, B) at each percentage (of the
                                                       bins of 1/8 octave, linear inside) */
    bool valid; /* the output's light was measured (single-layer PQ, not the state alone) */
} hdrg_hdr10plus_t;

#define HDRG_ST2094_50_POINTS 32u

/*
 * A frame's SMPTE ST 2094-50 adaptive tone map (application 5): the gain
 * curve of the output back to its SDR source, for a
 * display without headroom (alternate headroom 0; one between it and the
 * baseline weighs the gain by its headroom), fitted to the frame's output
 * and source (the log2 ratio of their max(R, G, B) by the output's level):
 * the values of the payload (the ICC 'HAGC' tag's form)
 */
typedef struct {
    float baseline_headroom;        /* log2 of the rendition's peak over the reference white */
    float reference_white;          /* cd/m² of the SDR white in the output (default 203) */
    float x[HDRG_ST2094_50_POINTS]; /* the output's max(R, G, B) over the reference white,
                                       rising (at most 64) */
    float y[HDRG_ST2094_50_POINTS]; /* log2 gain there back to the SDR (-6 to 0; PCHIP
                                       between, held past the ends) */
    uint8_t count;                  /* the control points, 1 to HDRG_ST2094_50_POINTS */
    uint8_t gamut;                  /* the gain's space: HDRG_GAMUT_* (the output's) */
    bool valid; /* fitted (the output's light measured, a headroom above the white) */
} hdrg_st2094_50_t;

#define HDRG_HDR10PLUS_T35_BYTES 43u
#define HDRG_ST2094_50_T35_MAX   139u

/*
 * A frame's HDR10+ as its ITU-T T.35 message's payload after the 6 header
 * bytes (country 0xB5, provider 0x003C, provider oriented 0x0001,
 * application identifier 4): from application_version 1, the bytes of FFmpeg's
 * av_dynamic_hdr_plus_to_t35 and FFMS2's HDR10Plus frame property. One
 * window, no targeted display, no tone mapping curve (the display's own);
 * the values in 1/100000 of 10000 cd/m^2. Returns its size
 * (HDRG_HDR10PLUS_T35_BYTES), written into out when size is at least that;
 * 0: not valid
 */
HDRG_API size_t hdrg_hdr10plus_t35(const hdrg_hdr10plus_t *hdr10plus, uint8_t *out, size_t size);
/*
 * A frame's ST 2094-50 as its T.35 message's payload after the 5 header
 * bytes (country 0xB5, provider 0x0090, provider oriented 0x0001): the bytes of FFmpeg's
 * av_dynamic_hdr_smpte2094_app5_to_t35, the ICC 'HAGC' tag's. One alternate
 * at headroom 0 with PCHIP slopes; the reference white when not 203 cd/m^2.
 * Returns its size (at most HDRG_ST2094_50_T35_MAX), written into out when
 * size is at least that; 0: not valid
 */
HDRG_API size_t hdrg_st2094_50_t35(const hdrg_st2094_50_t *st2094_50, uint8_t *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* HDRGRADING_METADATA_H */
