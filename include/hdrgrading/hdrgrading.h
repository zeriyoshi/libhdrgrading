/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading — SDR to HDR grading: brightens SDR images naturally on HDR
 * displays and encodes the result with standard HDR signaling.
 *
 * Default output is an ISO/TS 21496-1 gain-map JPEG: a real SDR base (sRGB,
 * or Display P3 when the gamut is expanded) plus a monochrome gain map in a
 * second JPEG stream linked via MPF, with Apple HDRGainMap and Adobe/Google
 * hdrgm XMP alongside the ISO metadata. Gain-map readers (iOS 18+ / macOS
 * 15+, Android 15+, Chrome, libultrahdr-based tools) show HDR; everything
 * else shows the correct SDR image.
 *
 * HDRG_OUTPUT_HYBRID writes a PQ-encoded Display P3 primary ("P3 PQ" ICC
 * profile with cicp and a HAGC tag) plus an SDR-recovering gain map declared
 * as an HDR-base pair (gain map in [-H, 0] stops): viewers that honor only
 * the ICC profile show HDR as well, but color-unmanaged SDR viewers show it
 * dark and flat, and libultrahdr rejects the HDR-base direction.
 * HDRG_OUTPUT_PQ is the bare single-layer PQ form.
 *
 * The default rendition lifts the whole frame to one diffuse
 * level (up to diffuse_max, 2x SDR white by default, for a lit scene, less
 * for a dark one), so the ratios between surfaces stay those of the SDR original
 * (Didyk et al. 2008: the diffuse surfaces keep their ratios). Only what
 * shows evidence of being a light — a bright region whose surround falls
 * off evenly in every direction (a glow, a bloom, a specular's soft edge) —
 * rises further, toward the display's ceiling. Text, UI, white walls,
 * documents and snow carry no such evidence and stay at the diffuse level.
 * An average-picture-level budget (a quarter of the ceiling, BT.2408)
 * bounds the whole frame. The analysis runs on the source values; gamut
 * expansion happens afterwards. Contrast / local contrast / saturation
 * grade on top. All luminance operations are ratio-scales on RGB, so hue
 * is structurally preserved; shadows never drop below the SDR original.
 *
 * Thread safety: a context must not be used concurrently; distinct contexts
 * are fully independent.
 *
 * Determinism: identical input bytes + identical params produce identical
 * output bytes, regardless of the context's thread count.
 */
#ifndef HDRGRADING_H
#define HDRGRADING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/version.h"

#if defined(_WIN32) && defined(HDRG_SHARED)
#ifdef HDRG_BUILDING
#define HDRG_API __declspec(dllexport)
#else
#define HDRG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(HDRG_BUILDING)
#define HDRG_API __attribute__((visibility("default")))
#else
#define HDRG_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hdrg_context hdrg_context_t;

/*
 * Status codes. Fixed values (they also cross the filter plugin ABI, which
 * uses no enums): codes are only ever added.
 */
typedef uint32_t hdrg_status_t;

#define HDRG_OK                     UINT32_C(0)
#define HDRG_ERR_INVALID_ARG        UINT32_C(1)
#define HDRG_ERR_UNSUPPORTED_FORMAT UINT32_C(2)
#define HDRG_ERR_DECODE             UINT32_C(3)
#define HDRG_ERR_ENCODE             UINT32_C(4)
#define HDRG_ERR_NOMEM              UINT32_C(5)
#define HDRG_ERR_ICC_UNSUPPORTED    UINT32_C(6)
#define HDRG_ERR_INTERNAL           UINT32_C(7)
#define HDRG_ERR_INVALID_STATE      UINT32_C(8)  /* e.g. a registration call after the freeze */
#define HDRG_ERR_UNSUPPORTED        UINT32_C(9)  /* not available in this build / version */
#define HDRG_ERR_NOT_FOUND          UINT32_C(10) /* unknown filter id / option key */
#define HDRG_ERR_PLUGIN             UINT32_C(11) /* a plugin was rejected or a filter failed */
#define HDRG_ERR_AGAIN              UINT32_C(12) /* a session: send / receive again later */
#define HDRG_EOF                    UINT32_C(13) /* a session: every frame was received */
#define HDRG_ERR_LIMIT              UINT32_C(14) /* larger than the context's max_pixels */

/*
 * Color of raw pixels: ITU-T H.273 code points for the primaries and the
 * transfer characteristics, plus the two common still-image spaces H.273
 * lacks. Decoders resolve the color of files themselves (ICC, cICP, EXIF).
 */
#define HDRG_PRIMARIES_BT709     UINT8_C(1) /* also sRGB */
#define HDRG_PRIMARIES_BT601_625 UINT8_C(5)
#define HDRG_PRIMARIES_BT601_525 UINT8_C(6)
#define HDRG_PRIMARIES_BT2020    UINT8_C(9)
#define HDRG_PRIMARIES_DCI_P3    UINT8_C(11) /* theater white */
#define HDRG_PRIMARIES_P3_D65    UINT8_C(12) /* Display P3 */
#define HDRG_PRIMARIES_ADOBE_RGB UINT8_C(200)
#define HDRG_PRIMARIES_PROPHOTO  UINT8_C(201) /* ROMM RGB, D50 white */

#define HDRG_TRANSFER_BT709     UINT8_C(1) /* 1 / 6 / 14 / 15: decoded with BT.1886 (gamma 2.4) */
#define HDRG_TRANSFER_GAMMA22   UINT8_C(4)
#define HDRG_TRANSFER_GAMMA28   UINT8_C(5)
#define HDRG_TRANSFER_BT601     UINT8_C(6)
#define HDRG_TRANSFER_LINEAR    UINT8_C(8)
#define HDRG_TRANSFER_SRGB      UINT8_C(13)
#define HDRG_TRANSFER_BT2020_10 UINT8_C(14)
#define HDRG_TRANSFER_BT2020_12 UINT8_C(15)
#define HDRG_TRANSFER_ADOBE_RGB UINT8_C(200) /* gamma 563/256 */
#define HDRG_TRANSFER_PROPHOTO  UINT8_C(201) /* ROMM: gamma 1.8, linear below 1/32 */

/* planar Y'CbCr layouts (hdrg_session_pixfmt_t.layout) */
#define HDRG_YUV_420 UINT8_C(1)
#define HDRG_YUV_422 UINT8_C(2)
#define HDRG_YUV_444 UINT8_C(3)

/*
 * where subsampled chroma sits (ITU-T H.273 chroma_sample_loc_type,
 * hdrg_session_pixfmt_t.chroma_loc): on the even luma columns (LEFT) or
 * between two (CENTER); 4:2:0 between the two rows of a pair, on the first
 * (TOP) or on the second (BOTTOM)
 */
#define HDRG_CHROMA_LEFT       UINT8_C(0) /* the default of MPEG-2, H.264 / HEVC; AV1's VERTICAL */
#define HDRG_CHROMA_CENTER     UINT8_C(1) /* JPEG, MPEG-1 */
#define HDRG_CHROMA_TOPLEFT    UINT8_C(2) /* AV1 colocated, much UHD / HDR content */
#define HDRG_CHROMA_TOP        UINT8_C(3)
#define HDRG_CHROMA_BOTTOMLEFT UINT8_C(4)
#define HDRG_CHROMA_BOTTOM     UINT8_C(5)

/* ITU-T H.273 matrix coefficients the conversions know */
#define HDRG_MATRIX_BT709  UINT8_C(1)
#define HDRG_MATRIX_BT601  UINT8_C(6) /* SMPTE 170M (5, BT.470 BG, is the same) */
#define HDRG_MATRIX_BT2020 UINT8_C(9) /* non-constant luminance */

typedef enum {
    HDRG_OUTPUT_AUTO = 0,    /* the writer's own (hdrg_codec_info_t.default_form) */
    HDRG_OUTPUT_GAINMAP = 1, /* SDR base + HDR gain map (ISO 21496-1) */
    HDRG_OUTPUT_HYBRID = 2,  /* PQ primary + SDR-recovering gain map */
    HDRG_OUTPUT_PQ = 3,      /* single-layer PQ, no gain map */
} hdrg_output_mode_t;

/*
 * The content intent: how the picture is graded, whatever display it is
 * shown on (the display is hdrg_display_caps_t, a still file's form
 * hdrg_encode_params_t). Initialize with hdrg_params_init(); defaults live
 * in the implementation so they can be re-tuned without ABI breakage.
 * Fields added in later minor versions go at the end; struct_size gates
 * their visibility.
 */
typedef struct {
    uint32_t struct_size; /* set to sizeof(hdrg_params_t) */

    /* --- luminance --- */
    float global_strength; /* 0.0-1.0 multiplier on the diffuse level and the lights'
                              boost. default 1.0 */
    float diffuse_max;     /* how far diffuse content rises, in multiples of its SDR
                              level: a normally exposed frame takes all of it, a dark
                              (low-key) one keeps the SDR level and its lights carry
                              the HDR. One level for the frame, so diffuse surfaces
                              (walls, paper, skin, text) keep their ratios; only
                              lights and reflections (a bright region whose surround
                              falls off gradually: a glow, a bloom, a specular's soft
                              edge) rise above it, toward the peak. 1.0-4.0,
                              default 2.0 */

    float apl_budget_nits;   /* average picture level ceiling in nits, 0 = none
                                (BT.2408 comfort in a dim room: BBC finds 5-80
                                comfortable, up to 160 for creative effect). The
                                gain is scaled by one common factor (binary search
                                on a low-res proxy) until the estimated APL fits
                                the stricter of this and apl_peak_fraction * peak.
                                The SDR level itself is never reduced, so content
                                brighter than the limit at SDR (a white document)
                                gets no gain. default 0: the library does not
                                assume dark rooms */
    float apl_peak_fraction; /* APL ceiling as a fraction of the peak (NHK: under
                                25% of peak is not judged "too bright").
                                0 = no peak-relative limit. Both limits 0 =
                                no APL budget at all. default 0.25 */
    bool colored_lights;     /* judge saturated lights too: a region whose
                                brightest channel clips while its luminance
                                stays low (a red neon tube, a colored LED, an
                                orange sun) rises when it glows as a white
                                light must. Off, only regions near white are
                                lights. Saturated surfaces of a photograph
                                (flowers, a blue sky) would be taken for
                                lights now and then. preset default (ANIME
                                on, else off) */
    float light_predictor;   /* the learned check of the judged lights:
                                each light's boost
                                scaled by (1 - this) + this x the small
                                predictor's chance that it is a light (0..1),
                                so it lowers a doubtful light, never raises
                                one past the rules. 0..1, default 0 (off: the
                                rules alone) */

    /* --- grading (all hue-preserving; neutral values disable) --- */
    float contrast;        /* log-luminance S-slope around mid-gray 0.18.
                              >1 deepens shadows / expands highlights
                              (shadows never drop below the SDR
                              original). 1.0 = neutral. default 1.0
                              (the surfaces keep their SDR ratios) */
    float clarity;         /* local contrast (log-luminance unsharp mask)
                              strength. 0 = off. default 0 */
    float saturation;      /* perceptual chroma scale (Oklab, with the
                              gamut mapping in the color LUT). 1.0 =
                              neutral. preset default (ANIME 1.0, else
                              1.05) */
    float highlight_desat; /* progressive desaturation toward white as
                              luminance approaches peak (from the 75%
                              shoulder point). Lets saturated highlights
                              reach near-peak brightness instead of being
                              luminance-limited by one channel — trades
                              color fidelity for visual brightness.
                              0 = off. preset default (PHOTO 0.4, ANIME
                              0.15, GAME 0.3) */
    float expand_strength; /* 0-1 gamut expansion toward the target
                              gamut (Display P3), in Oklab LCh with
                              lightness and hue fixed: only colors near
                              the source boundary and near their hue's
                              cusp move; neutrals, pastels, dark tones
                              and skin stay colorimetric.
                              Sources wider than the target are
                              compressed into it instead. The target
                              is sRGB without lcms2 (no P3 profile),
                              or when expand_strength is 0 for a source
                              within sRGB (base stays in the source's
                              colors). preset default (PHOTO 0.7) */
    float expand_reach;    /* 0-1: how far toward the target boundary
                              the expansion goes. PHOTO 0.85 */
    float expand_skin;     /* 0-1 protection of skin hues. PHOTO 0.9 */
    float expand_memory;   /* 0-1 protection of sky and foliage hues.
                              PHOTO 0.5 */

    /* --- color --- */
    uint8_t preset;     /* HDRG_PRESET_*: set by hdrg_params_init, selects the
                           shape of the gamut mapping (knees, dark-tone
                           protection) the strengths above scale */
    uint8_t sdr_eotf;   /* HDRG_SDR_EOTF_*: how sources tagged with the sRGB
                           curve are decoded to light. AUTO keeps the sRGB
                           piecewise curve; GAMMA22 / BT1886 (2.4) follow
                           displays that ignore the linear toe (darker
                           shadows). preset default */
    uint8_t chroma_fit; /* HDRG_CHROMA_FIT_*: how subsampled chroma (a 4:2:0 /
                           4:2:2 JPEG's, a session's Y'CbCr input) follows
                           the luma when it is upsampled. LINEAR: a straight
                           line in the luma over each chroma sample's 3 x 3
                           neighborhood. SEGMENTED: where a straight line
                           leaves a residual, a line bent at the window's
                           middle luma (an outline between two fills keeps
                           neither fill's color). preset default (ANIME
                           SEGMENTED, else LINEAR) */

    /* --- size (the SCALE stage) --- */
    uint8_t upscaler; /* HDRG_UPSCALER_*: the built-in scaler of an output larger than
                         the input on both sides, when no filter is added to the SCALE
                         stage (any other size: Lanczos). LANCZOS (hdrg.scale.lanczos),
                         EDGE (hdrg.scale.edge: the library's edge-adaptive kernel, lines
                         kept smooth), FSR1 (hdrg.scale.fsr1: AMD FSR 1's EASU), NIS
                         (hdrg.scale.nis: NVIDIA Image Scaling, up to 2x on each side:
                         Lanczos past that); each its GPU version on a
                         GPU context. preset default (PHOTO LANCZOS: the closest to the
                         photographs; ANIME, GAME EDGE: by eye) */

    /* --- noise --- */
    uint8_t grain_policy;    /* HDRG_GRAIN_*. PRESERVE keeps the source's grain and
                                noise as they are: the gain, the clarity detail and
                                the highlight desaturation follow the noise-cored
                                luminance, so the boost does not amplify them.
                                REDUCE runs the PREFILTER stage first: the filters
                                added to it, else the built-in spatial noise
                                reduction (hdrg.denoise.spatial, linked to the
                                measured noise). RESYNTH (a video session's; a still:
                                HDRG_ERR_UNSUPPORTED) reduces as REDUCE, then adds
                                grain to the output in PQ with the source's grain
                                as measured (its level at each luma, its
                                correlation, HDRG_SD_NOISE) and as it looked in
                                SDR: not amplified by the expansion. AUTO (the
                                default): RESYNTH in PHOTO's video sessions,
                                PRESERVE in the rest (stills, ANIME, GAME) */
    float denoise_strength;  /* REDUCE with the built-in filter: its smoothing
                                threshold over the measured noise level, 0-4;
                                < 0 = 1. default -1 */
    float grain_amount;      /* RESYNTH: the grain added over the source's as it
                                looked in SDR, 0-4 (0 = none). default 1 */
    float deblock_strength;  /* the PREFILTER stage's built-in deblocking of the
                                8-pixel grid before the noise reduction
                                (hdrg.deblock, linked to the measured block
                                step; nothing without block artifacts): its
                                thresholds over that step, 0-4; 0 = off. Filters
                                added to the stage replace it. preset default
                                (PHOTO / ANIME 1, GAME 0) */
    float dering_strength;   /* the PREFILTER stage's built-in deringing after
                                the deblocking (hdrg.dering: in the 8-pixel
                                blocks with an edge, the mean of the window's
                                samples within a range set by the measured
                                ringing, the block's contrast and the local
                                slope; nothing without ringing): the window's
                                radius is 1 + 2 x this, 0-3; 0 = off. preset
                                default (ANIME 2, else 0). A frame no filter has
                                anything to do for skips the stage: a still is
                                then converted as without it */
    float defringe_strength; /* the PREFILTER stage's built-in defringing after
                                the deringing (hdrg.defringe: near luma edges,
                                the chroma of the window's samples of a close
                                Y' and chroma; Y' kept; nothing without
                                ringing): the window's radius is 1 + 2 x this,
                                0-3; 0 = off. preset default (ANIME 3,
                                else 0) */
    float deband_strength;   /* the PREFILTER stage's built-in debanding after
                                the defringing (hdrg.deband: each sample toward
                                the plane fitted to its window where the fit's
                                residual is small, away from luma edges):
                                scales that residual (0.8 to 1.6 8-bit codes),
                                0-3; 0 = off. preset default (PHOTO 0.5,
                                ANIME 2.5, GAME 0) */
    float mctf_strength;     /* a video session's PREFILTER stage's built-in
                                temporal noise reduction after the debanding
                                (hdrg.mctf: each pixel averaged with the 2
                                frames before and after along their motion,
                                weighted by its difference over the measured
                                noise; 2 frames of delay, LOW reads only the
                                frames before): scales that noise, 0-4; 0 =
                                off. preset default (PHOTO 1, ANIME 1, GAME 0).
                                Stills have no frames around them */

    /* --- effort --- */
    uint8_t effort; /* HDRG_EFFORT_*: how finely the same rules are solved. REALTIME
                       judges the lights on 4 x 4 pixel cells and keeps their boost on
                       8-pixel cells, within a video session's budget;
                       QUALITY works on finer ones and analyzes a session's frames at
                       full resolution, without a time budget. AUTO: QUALITY for a still
                       image and an OFFLINE session, REALTIME for a watched one.
                       default AUTO */
} hdrg_params_t;

/*
 * A still file's form, whatever the codec writing it (plugin.h's
 * hdrg_context_set_writer: the format and its own options, such as the
 * quality): the output kind, the gain map, the output size. Initialize with
 * hdrg_encode_params_init().
 */
typedef struct {
    uint32_t struct_size;             /* set to sizeof(hdrg_encode_params_t) */
    uint8_t output_mode;              /* hdrg_output_mode_t value. default HDRG_OUTPUT_AUTO: the
                                         writer's (hdrg.jpeg: GAINMAP, real SDR pixels and HDR through
                                         the gain map; hdrg.jxl: HYBRID). HYBRID and PQ show dark, flat
                                         pixels in color-unmanaged SDR viewers. Note: the iOS Photos zoom
                                         pipeline dims single-layer PQ irreversibly; HYBRID carries a
                                         gain map that keeps ISO-aware Apple pipelines on the gain-map
                                         path. A writer refuses a form it does not take
                                         (HDRG_ERR_UNSUPPORTED) */
    uint8_t gainmap_scale_shift;      /* resolution of the stored gain map, 1/(2^n), 0-4 (0: the
                                         image's; the gain itself is rendered at full resolution).
                                         default 1 */
    float gainmap_headroom_cap_stops; /* cap on the gain-map rendition's peak and declared
                                         alternate headroom, log2. ISO 21496-1 viewers scale
                                         the WHOLE gain map by w = display_headroom /
                                         declared, so a display with less headroom shows
                                         every gain scaled down (never below SDR). The file
                                         for an unknown display declares this much (a peak of
                                         SDR white x 2^cap, 1624 nits at 3 stops, not PQ's
                                         1000): 1600-nit phones, tablets and laptops reach it
                                         in dim viewing.
                                         0 = uncapped. GAINMAP mode only: HYBRID
                                         and PQ keep the PQ peak so the ICC-only view matches
                                         --pq. default 3.0 */
    float gainmap_gamma;              /* encoding gamma of the stored gain map. default 1.0 */
    float tonemap_knee;     /* SDR rendition tone-map knee K in diffuse-white units: identity below
                               K, C1 exponential approach to 1.0 above. The scale is applied per
                               pixel to all channels (hue/saturation preserving, exactly invertible
                               by the monochrome gain map). default 0.75 */
    uint32_t output_width;  /* output pixels, at most 65535 and the writer's limit (a JPEG's
                               65500: a larger side, the input's too, is HDRG_ERR_INVALID_ARG).
                               0 = derived from the other side keeping the aspect ratio; both 0 =
                               the input size. A different size runs the SCALE stage: the filter
                               added with hdrg_context_add_filter, else a built-in scaler (larger
                               on both sides: hdrg_params_t.upscaler's, else hdrg.scale.lanczos:
                               3 lobes, anti-ringing). Analysis and plan stay at the input size.
                               default 0 */
    uint32_t output_height; /* default 0 */
    bool dither;            /* triangular dither on 8-bit output rows and the gain map.
                               default true */
} hdrg_encode_params_t;

/* the gamut an output may use (hdrg_display_caps_t.gamut) */
#define HDRG_GAMUT_SRGB   UINT8_C(0)
#define HDRG_GAMUT_P3     UINT8_C(1)
#define HDRG_GAMUT_BT2020 UINT8_C(2) /* a BT.2020 container (video): the colors stay within P3 */

/*
 * What the display shows: the rendition never guesses.
 * Initialize with hdrg_display_caps_init(), the assumed viewing of a file
 * (no headroom limit, 203-nit SDR white, the format's peak: never the
 * content's, Display P3); a still file's form may lower it (the gain map's
 * headroom cap, the gamut the writer can tag: sRGB without lcms2 for one
 * signaling ICC profiles).
 */
typedef struct {
    uint32_t struct_size;      /* set to sizeof(hdrg_display_caps_t) */
    float headroom_stops;      /* log2(peak / SDR white) the display delivers; < 0 = no limit.
                                  default -1 */
    float sdr_white_nits;      /* the display's SDR reference white; 0 = 203 (BT.2408).
                                  default 0 */
    float peak_nits;           /* the rendition ceiling; a soft shoulder starts at 75% of it
                                  so highlight gradation is never clipped flat. 0 = the
                                  format's: 1000 (PQ; a gain map: SDR white x 2^headroom).
                                  default 0 */
    float max_full_frame_nits; /* full-white (ABL) limit of the average; 0 = none. default 0 */
    uint8_t gamut;             /* HDRG_GAMUT_*: the widest gamut the output may use.
                                  default HDRG_GAMUT_P3 */
} hdrg_display_caps_t;

/*
 * Content presets: starting points of the same pipeline.
 * PHOTO: camera pictures (boundary colors are mostly clipped, expansion is
 * best founded). ANIME: the palette is the intent; gentler expansion,
 * strong protection of skin and dark line art, gamma 2.2. GAME: vivid,
 * reach kept below the boundary (HUD colors cannot be told apart from the
 * scene's, so none is pushed to the gamut's edge).
 */
#define HDRG_PRESET_PHOTO UINT8_C(0)
#define HDRG_PRESET_ANIME UINT8_C(1)
#define HDRG_PRESET_GAME  UINT8_C(2)

#define HDRG_SDR_EOTF_AUTO    UINT8_C(0) /* the source's own curve */
#define HDRG_SDR_EOTF_SRGB    UINT8_C(1) /* IEC 61966-2-1 piecewise */
#define HDRG_SDR_EOTF_GAMMA22 UINT8_C(2)
#define HDRG_SDR_EOTF_BT1886  UINT8_C(3) /* gamma 2.4 */

#define HDRG_CHROMA_FIT_LINEAR    UINT8_C(0)
#define HDRG_CHROMA_FIT_SEGMENTED UINT8_C(1)

#define HDRG_UPSCALER_LANCZOS UINT8_C(0)
#define HDRG_UPSCALER_EDGE    UINT8_C(1)
#define HDRG_UPSCALER_FSR1    UINT8_C(2)
#define HDRG_UPSCALER_NIS     UINT8_C(3)

#define HDRG_GRAIN_PRESERVE UINT8_C(0)
#define HDRG_GRAIN_REDUCE   UINT8_C(1)
#define HDRG_GRAIN_RESYNTH  UINT8_C(2) /* remove, grade, resynthesize in PQ (a session's) */
#define HDRG_GRAIN_AUTO     UINT8_C(3) /* PHOTO's video sessions: RESYNTH, else PRESERVE */

#define HDRG_EFFORT_AUTO     UINT8_C(0) /* QUALITY: a still image, an OFFLINE session */
#define HDRG_EFFORT_REALTIME UINT8_C(1)
#define HDRG_EFFORT_QUALITY  UINT8_C(2)

/* defaults of a preset; NULL params is a no-op */
HDRG_API void hdrg_params_init(hdrg_params_t *params, uint8_t preset);
/* the defaults; NULL is a no-op */
HDRG_API void hdrg_encode_params_init(hdrg_encode_params_t *encode);
HDRG_API void hdrg_display_caps_init(hdrg_display_caps_t *caps);
/*
 * The highest rendition peak (cd/m^2) caps allow any picture: the display's
 * peak, else 1000, within its headroom. A stream's mastering display
 * maximum before any frame (hdrg_session_peak gives the same of a session);
 * 0 for invalid caps (NULL = the defaults)
 */
HDRG_API float hdrg_display_caps_peak(const hdrg_display_caps_t *caps);

/*
 * The settings by name, for bindings and tools (WASM, VapourSynth,
 * GStreamer, the FFmpeg filter, the GUI, the CLIs' --set): mostly the
 * hdrg-cli option names without the dashes ("diffuse-max", "peak-nits",
 * "display-headroom", "deband", ...), a session's hdrg-video's ("temporal",
 * "latency", "past", ...), the struct's own where the CLI takes a switch or
 * a pair ("mode", "output-width", "flash-off", "out-rate-num"). Every field
 * of the three structs has one but struct_size and
 * hdrg_display_caps_t.gamut, and the session's those of its numbers,
 * rates, size and switches. "preset" initializes the intent
 * (hdrg_params_init): set it first. A setting's struct is its part's (a
 * session's: hdrg_session_setting_set of video.h); switches take 0 / 1;
 * enumerated ones have their values' names too (hdrg_setting_value_at,
 * hdrg_setting_parse). The ranges are checked where the settings are used.
 */
#define HDRG_SETTING_INTENT  UINT8_C(0) /* hdrg_params_t */
#define HDRG_SETTING_DISPLAY UINT8_C(1) /* hdrg_display_caps_t */
#define HDRG_SETTING_ENCODE  UINT8_C(2) /* hdrg_encode_params_t */
#define HDRG_SETTING_SESSION UINT8_C(3) /* hdrg_session_config_t (video.h) */

#define HDRG_SETTING_FLOAT UINT8_C(0)
#define HDRG_SETTING_U8    UINT8_C(1)
#define HDRG_SETTING_U32   UINT8_C(2)
#define HDRG_SETTING_BOOL  UINT8_C(3)
#define HDRG_SETTING_U16   UINT8_C(4)

typedef struct {
    const char *name;
    uint8_t part;   /* HDRG_SETTING_INTENT / DISPLAY / ENCODE / SESSION */
    uint8_t kind;   /* HDRG_SETTING_FLOAT / U8 / U16 / U32 / BOOL */
    uint8_t values; /* its values' names (hdrg_setting_value_at; 0: a number alone) */
} hdrg_setting_t;

/* the index-th setting, in a fixed order; false past the last */
HDRG_API bool hdrg_setting_at(uint32_t index, hdrg_setting_t *out);
/*
 * One setting into its part's struct (the others may be NULL):
 * HDRG_ERR_NOT_FOUND for an unknown name, HDRG_ERR_INVALID_ARG for a NULL
 * struct of its part or a value outside its kind (NaN, not a whole number
 * of the type's range, a switch other than 0 / 1)
 */
HDRG_API hdrg_status_t hdrg_setting_set(const char *name, double value, hdrg_params_t *params,
                                        hdrg_display_caps_t *display, hdrg_encode_params_t *encode);
/* one setting's value from its part's struct (switches 0 / 1); the errors of hdrg_setting_set */
HDRG_API hdrg_status_t hdrg_setting_get(const char *name, const hdrg_params_t *params,
                                        const hdrg_display_caps_t *display,
                                        const hdrg_encode_params_t *encode, double *value);
/* the index-th of the named values of setting name: *text and *value; false past the last */
HDRG_API bool hdrg_setting_value_at(const char *name, uint32_t index, const char **text,
                                    double *value);
/* the name of setting name's value (NULL: none, or no such setting) */
HDRG_API const char *hdrg_setting_value_name(const char *name, double value);
/*
 * The value of setting name written text: one of its values' names, else a
 * number in the C locale's form. HDRG_ERR_NOT_FOUND for an unknown name,
 * HDRG_ERR_INVALID_ARG for a text that is neither (the kind is checked by
 * hdrg_setting_set)
 */
HDRG_API hdrg_status_t hdrg_setting_parse(const char *name, const char *text, double *value);
HDRG_API const char *hdrg_status_string(hdrg_status_t s);
HDRG_API const char *hdrg_version(void);

/*
 * Custom allocator. The allocations of the library, of its built-in filters
 * and GPU layers, of the plugins through their API table (mem_alloc and the
 * others), and of the dependencies that take hooks (libpng, zlib through
 * it, lcms2) route through these hooks, as do the buffers later released by
 * hdrg_free. jpegli, giflib, Highway and the GPU drivers allocate on their
 * own. Hooks are called from any thread (the pool's, a GPU's completion),
 * and must stay usable until hdrg_context_destroy returns and every buffer
 * of hdrg_free is freed. calloc_fn is optional (NULL = malloc_fn and zero
 * filling); aligned blocks (GPU host memory) are carved from malloc_fn's.
 * Copied at context creation. NULL in hdrg_context_desc_t: the build's
 * defaults (HDRG_MALLOC and the others, malloc unless the build replaces
 * them).
 */
typedef struct {
    void *(*malloc_fn)(size_t size, void *user);
    void *(*calloc_fn)(size_t count, size_t size, void *user);
    void *(*realloc_fn)(void *ptr, size_t size, void *user);
    void (*free_fn)(void *ptr, void *user);
    void *user;
} hdrg_allocator_t;

/*
 * SIMD tiers. A context runs the best tier the CPU supports;
 * the environment variable HDRG_SIMD_MAX (a name of hdrg_simd_name, read at
 * context creation) caps it, and a tier of another architecture caps to
 * scalar. AVX-512 and SVE are provisional: they run only when HDRG_SIMD_MAX
 * names them. The tier that ran is in hdrg_stats_t.simd_tier.
 */
#define HDRG_SIMD_SCALAR  UINT8_C(1)
#define HDRG_SIMD_SSE41   UINT8_C(2)
#define HDRG_SIMD_AVX2    UINT8_C(3) /* with FMA (x86-64-v3) */
#define HDRG_SIMD_AVX512  UINT8_C(4) /* F / DQ / BW / VL + VBMI2 / VNNI / BITALG (Ice Lake) */
#define HDRG_SIMD_NEON    UINT8_C(5)
#define HDRG_SIMD_SVE     UINT8_C(6)
#define HDRG_SIMD_WASM128 UINT8_C(7)

/* "scalar", "sse41", "avx2", "avx512", "neon", "sve", "wasm128"; NULL for others */
HDRG_API const char *hdrg_simd_name(uint8_t tier);

/*
 * How a context runs. thread_count and simd_max never change the output
 * bytes beyond the SIMD tier's own tolerance: the work is split
 * the same way for any thread count.
 */
typedef struct {
    uint32_t struct_size;              /* set to sizeof(hdrg_context_desc_t) */
    const hdrg_allocator_t *allocator; /* NULL = the build's defaults (HDRG_MALLOC and the
                                          others); copied */
    uint32_t thread_count;             /* threads a call may run on, the calling one
                                          included: 0 = one per core, 1 = the calling
                                          thread alone. default 0 */
    uint64_t max_pixels;               /* the largest image (input or output) a call takes:
                                          larger ones fail with HDRG_ERR_LIMIT before their
                                          pixels are allocated. 0 = HDRG_DEFAULT_MAX_PIXELS.
                                          where size_t is 32 bits, capped to 2^28 - 1 (16
                                          bytes a pixel must fit it: 16384 x 16384 does not) */
    uint8_t simd_max;                  /* HDRG_SIMD_*: the best tier to run (as the
                                          environment variable HDRG_SIMD_MAX, which caps
                                          it further); 0 = the CPU's best. default 0 */
    uint8_t gpu;                       /* HDRG_GPU_*: FILTERS runs the built-in PREFILTER
                                          filters, the SCALE stage's built-in scaler and the
                                          INTERP stage's default on their GPU versions, on
                                          Metal, else Vulkan, else WebGPU, when the build has
                                          them and a device is found (else on the CPU): the
                                          CPU's bytes for deblock, dering, defringe, Lanczos,
                                          MCTF and the interpolation, the L2
                                          tolerance for the debanding and the noise
                                          reduction, 2 codes for the edge-adaptive scaler,
                                          FSR 1 and NIS. AUTO also runs a session's Y'CbCr
                                          input's conversion and its apply pass there (within
                                          L2 of the CPU's). default OFF */
    uint8_t gpu_analysis;              /* HDRG_GPU_ANALYSIS_*, with gpu HDRG_GPU_AUTO: where
                                          a session's analysis adds up its pixels. EXACT: on
                                          the CPU (the GPU only moves the pixels: the
                                          statistics are the CPU's bit for bit). FAST: pass
                                          A's and pass B's sums on the GPU (16-bit sources:
                                          Y'CbCr input), within L2 of the CPU's, so a light
                                          near a threshold may be judged otherwise.
                                          default EXACT */
} hdrg_context_desc_t;

#define HDRG_GPU_OFF     UINT8_C(0)
#define HDRG_GPU_AUTO    UINT8_C(1)
#define HDRG_GPU_FILTERS UINT8_C(2)

#define HDRG_GPU_ANALYSIS_EXACT UINT8_C(0)
#define HDRG_GPU_ANALYSIS_FAST  UINT8_C(1)

#define HDRG_DEFAULT_MAX_PIXELS (UINT64_C(1) << 28) /* 16384 x 16384 */

/* the defaults; NULL is a no-op */
HDRG_API void hdrg_context_desc_init(hdrg_context_desc_t *desc);
/* desc NULL = the defaults */
HDRG_API hdrg_status_t hdrg_context_create(const hdrg_context_desc_t *desc, hdrg_context_t **ctx);
HDRG_API void hdrg_context_destroy(hdrg_context_t *ctx);

/*
 * High-level API: bytes in / HDR file bytes out. The input's codec is the
 * first of the context's readers recognizing it (built in: JPEG, PNG, GIF;
 * a plugin's: AVIF): HDRG_ERR_UNSUPPORTED_FORMAT when none does. The output's
 * is the context's writer (plugin.h's hdrg_context_set_writer; default
 * hdrg.jpeg, also hdrg.jxl where built, a plugin's: AVIF, HEIF).
 * params, display and encode NULL = their defaults. A writer signaling its
 * color by ICC profiles (hdrg.jpeg) takes HYBRID and PQ in an lcms2-enabled
 * build alone (profile generation): HDRG_ERR_ICC_UNSUPPORTED otherwise.
 * Output buffers carry their originating allocator, so hdrg_free works after
 * the context is destroyed.
 */
HDRG_API hdrg_status_t hdrg_convert(hdrg_context_t *ctx, const uint8_t *input, size_t input_size,
                                    const hdrg_params_t *params, const hdrg_display_caps_t *display,
                                    const hdrg_encode_params_t *encode, uint8_t **output,
                                    size_t *output_size);
HDRG_API void hdrg_free(uint8_t *buffer);

/*
 * Low-level API: pre-decoded interleaved RGB(A) buffer.
 * bit_depth is 8 or 16 (16 = native-endian uint16_t per sample, e.g. PNG16
 * already expanded; pixels and stride at 2 bytes). stride is in bytes. Alpha,
 * when present, is ignored and the image is treated as opaque. primaries /
 * transfer are HDRG_PRIMARIES_* / HDRG_TRANSFER_* (sRGB: BT709 + SRGB); other
 * values are rejected.
 */
typedef struct {
    const uint8_t *pixels;
    uint32_t width, height;
    uint32_t bit_depth;
    size_t stride;
    uint8_t primaries;
    uint8_t transfer;
    bool has_alpha;
} hdrg_raw_image_t;

HDRG_API hdrg_status_t hdrg_convert_raw(hdrg_context_t *ctx, const hdrg_raw_image_t *image,
                                        const hdrg_params_t *params,
                                        const hdrg_display_caps_t *display,
                                        const hdrg_encode_params_t *encode, uint8_t **output,
                                        size_t *output_size);

/* Stats of the most recent successful convert on this context. */
typedef struct {
    float applied_max_gain_stops;
    float diffuse_stops;    /* the diffuse level applied (the APL budget included) */
    float light_area_ratio; /* the fraction (0..1) of the analysis cells in lights and
                               reflections */
    float sdr_apl_nits;     /* source average luminance at 203-nit white */
    float apl_nits;         /* estimated output average luminance */
    float apl_scale;        /* gain-field factor applied by the APL budget */
    float noise_sigma;      /* the source's Y' noise (standard deviation) in 8-bit codes;
                               < 0 = not measured (too small) */
    float noise_blockiness; /* Y' steps across the 8-pixel grid over those inside it on the
                               quieter areas: 1 = no block artifacts */
    float noise_ringing;    /* in the grid's blocks with an edge, the variation away from the
                               edge over the variation of the blocks without one: 1 = no
                               ringing */
    uint8_t simd_tier;      /* HDRG_SIMD_* of the kernels that ran */
} hdrg_stats_t;

HDRG_API hdrg_status_t hdrg_get_last_stats(const hdrg_context_t *ctx, hdrg_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HDRGRADING_H */
