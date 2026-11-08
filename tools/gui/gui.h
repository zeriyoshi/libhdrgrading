/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_GUI_H
#define HDRG_GUI_H

/*
 * hdrg-gui (tools/gui): plays and converts with the library's settings, on an HDR window (SDL3's
 * GPU swapchain in extended linear sRGB). The media come from the platform's decoders (media_*:
 * macOS AVFoundation and ImageIO, Windows Media Foundation and WIC, Linux GStreamer) or, opted
 * in, FFmpeg; a video is written by the platform's encoder (export_*: VideoToolbox, Media
 * Foundation) or hdrg-ffmpeg. The UI (main.cpp, Dear ImGui) is the only C++.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/plugin.h"
#include "hdrgrading/video.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a decoded frame's pixels */
#define HDRG_GUI_RGBA                                                                              \
    UINT8_C(1)                   /* 8-bit R'G'B'A of the info's primaries and transfer (a still)   \
                                  */
#define HDRG_GUI_NV12 UINT8_C(2) /* 8-bit Y', then Cb and Cr alternating, 4:2:0 */
#define HDRG_GUI_P010 UINT8_C(3) /* as NV12 in 16-bit samples, the 10 bits high */

/* hdrg_gui_media_read's results */
#define HDRG_GUI_READ_END   UINT8_C(0)
#define HDRG_GUI_READ_FRAME UINT8_C(1)
#define HDRG_GUI_READ_ERROR UINT8_C(2)

/* what the window shows (hdrg_gui_player_view) */
#define HDRG_GUI_VIEW_OUTPUT UINT8_C(0) /* the graded HDR */
#define HDRG_GUI_VIEW_SOURCE UINT8_C(1) /* the source's SDR */
#define HDRG_GUI_VIEW_BOTH   UINT8_C(2) /* side by side: the SDR left, the HDR right */

/* the error texts' size */
#define HDRG_GUI_ERROR 256u

typedef struct hdrg_gui_media hdrg_gui_media_t;
typedef struct hdrg_gui_player hdrg_gui_player_t;
typedef struct hdrg_gui_export hdrg_gui_export_t;

/* a medium's video (or still) */
typedef struct {
    int64_t duration_us; /* 0: a still or unknown */
    uint32_t width, height;
    uint32_t rate_num, rate_den; /* frames a second (0 / 0: a still) */
    uint8_t format;              /* HDRG_GUI_* */
    uint8_t matrix;              /* Y'CbCr: HDRG_MATRIX_* */
    uint8_t chroma_loc;          /* Y'CbCr: HDRG_CHROMA_* */
    uint8_t primaries;           /* HDRG_PRIMARIES_* */
    uint8_t transfer;            /* HDRG_TRANSFER_* */
    bool full_range;             /* Y'CbCr */
} hdrg_gui_info_t;

typedef struct {
    const uint8_t *planes[2];
    size_t strides[2];
    int64_t pts_us;
    void *handle; /* the backend's, until hdrg_gui_media_release */
} hdrg_gui_frame_t;

#define HDRG_GUI_FORMAT         64u /* a writer's id */
#define HDRG_GUI_FORMAT_OPTIONS 16u /* its options at most ... */
#define HDRG_GUI_FORMAT_OPTION  96u /* ... each "key=value" with its NUL */

/* the grading's settings */
typedef struct {
    hdrg_params_t params;
    hdrg_display_caps_t display;
    hdrg_encode_params_t encode;
    uint8_t format_option_count;
    char format[HDRG_GUI_FORMAT]; /* the still's writer, a codec's id ("": the output's extension's,
                                     else hdrg.jpeg) */
    char format_options[HDRG_GUI_FORMAT_OPTIONS][HDRG_GUI_FORMAT_OPTION]; /* its key=value (format
                                                                             set) */
    uint8_t temporal;                                                     /* HDRG_TEMPORAL_* */
    uint8_t gpu; /* the context's HDRG_GPU_* */
} hdrg_gui_settings_t;

/*
 * the window's picture: half-float R, G, B, A rows of width x 4, linear BT.709 (scRGB / EDR);
 * HDRG_GUI_VIEW_BOTH's twice the medium's width (the SDR left, the HDR right)
 */
typedef struct {
    const uint16_t *pixels;
    uint64_t serial; /* changes with every picture */
    int64_t pts_us;
    uint32_t width, height;
    uint8_t view; /* HDRG_GUI_VIEW_* it was made in */
} hdrg_gui_picture_t;

/* what the player is doing */
typedef struct {
    hdrg_gui_info_t info;
    int64_t position_us;
    float apply_ms;  /* the last frame's grading (send to receive) */
    float peak_nits; /* the session's rendition ceiling */
    bool playing;
    bool ended;
    char error[HDRG_GUI_ERROR];
} hdrg_gui_status_t;

/* an export's progress */
typedef struct {
    int64_t done_us, total_us;
    bool finished;
    bool failed;
    char error[HDRG_GUI_ERROR];
} hdrg_gui_progress_t;

/* media_*: the platform's decoders, a file's first video track or its image */
extern bool hdrg_gui_media_open(const char *path, hdrg_gui_media_t **out, hdrg_gui_info_t *info,
                                char *error, size_t error_size);
/* the next frame (HDRG_GUI_READ_*); a still gives its one frame, then the end */
extern uint8_t hdrg_gui_media_read(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame);
extern void hdrg_gui_media_release(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame);
/* the frames from the one at or before pts_us */
extern bool hdrg_gui_media_seek(hdrg_gui_media_t *media, int64_t pts_us);
extern void hdrg_gui_media_close(hdrg_gui_media_t *media);

/* grading.c: the preset's settings, WINDOWED, the context's GPU AUTO */
extern void hdrg_gui_settings_init(hdrg_gui_settings_t *settings, uint8_t preset);
/* the codec modules beside the program (tools/modules.h) onto ctx */
extern void hdrg_gui_load_modules(hdrg_context_t *ctx);
/*
 * ctx's writer of the still: settings' format with its options, else the one of path's extension
 * (none of a codec's: hdrg.jpeg, the default) with its defaults
 */
extern hdrg_status_t hdrg_gui_set_writer(hdrg_context_t *ctx, const hdrg_gui_settings_t *settings,
                                         const char *path);
/* a session of a medium's info and the settings (a still: one frame, no window) */
extern hdrg_status_t hdrg_gui_session_open(hdrg_context_t *ctx, const hdrg_gui_info_t *info,
                                           const hdrg_gui_settings_t *settings, uint8_t latency,
                                           const hdrg_session_pixfmt_t *output,
                                           hdrg_session_t **out);
/* a medium's frame as the session's */
extern void hdrg_gui_session_frame(const hdrg_gui_frame_t *frame, hdrg_session_frame_t *out);

/* player.c: plays a file through a session onto the window's pictures (a thread of its own) */
extern bool hdrg_gui_player_open(const char *path, const hdrg_gui_settings_t *settings,
                                 hdrg_gui_player_t **out, char *error, size_t error_size);
/* new settings: the session made again at the position (a still graded again) */
extern void hdrg_gui_player_settings(hdrg_gui_player_t *player,
                                     const hdrg_gui_settings_t *settings);
/* the window's SDR white in linear sRGB (Windows' scRGB: nits / 80; EDR: 1) and the view */
extern void hdrg_gui_player_view(hdrg_gui_player_t *player, float white_level, uint8_t view);
extern void hdrg_gui_player_play(hdrg_gui_player_t *player, bool playing);
extern void hdrg_gui_player_seek(hdrg_gui_player_t *player, int64_t pts_us);
/* the latest picture, held until hdrg_gui_player_unlock (false: none yet) */
extern bool hdrg_gui_player_lock(hdrg_gui_player_t *player, hdrg_gui_picture_t *out);
extern void hdrg_gui_player_unlock(hdrg_gui_player_t *player);
extern void hdrg_gui_player_status(hdrg_gui_player_t *player, hdrg_gui_status_t *out);
extern void hdrg_gui_player_close(hdrg_gui_player_t *player);

/*
 * export.c: a still converted (hdrg_convert: JPEG / AVIF / HEIF by the path's extension) or a
 * video written (HEVC 10-bit PQ BT.2020: export_* or hdrg-ffmpeg), on a thread of its own
 */
extern bool hdrg_gui_export_start(const char *input, const char *output,
                                  const hdrg_gui_settings_t *settings, hdrg_gui_export_t **out,
                                  char *error, size_t error_size);
extern void hdrg_gui_export_progress(hdrg_gui_export_t *job, hdrg_gui_progress_t *out);
/* waits for the thread (cancelled first when unfinished) */
extern void hdrg_gui_export_finish(hdrg_gui_export_t *job);
/* whether this build writes videos */
extern bool hdrg_gui_export_video_supported(void);

/* encoder_*: the platform's HEVC 10-bit encoder (P010 frames, BT.2020 PQ, the peak in the MDCV) */
typedef struct hdrg_gui_encoder hdrg_gui_encoder_t;

extern bool hdrg_gui_encoder_available(void);
extern bool hdrg_gui_encoder_open(const char *path, uint32_t width, uint32_t height,
                                  uint32_t rate_num, uint32_t rate_den, float peak_nits,
                                  hdrg_gui_encoder_t **out, char *error, size_t error_size);
/* a frame: Y' rows of width x 2 bytes, then the Cb, Cr rows ((height + 1) / 2 of them) */
extern bool hdrg_gui_encoder_write(hdrg_gui_encoder_t *encoder, const uint8_t *luma,
                                   const uint8_t *chroma, int64_t pts_us);
/* the file finished (keep) or removed */
extern bool hdrg_gui_encoder_close(hdrg_gui_encoder_t *encoder, bool keep, char *error,
                                   size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* HDRG_GUI_H */
