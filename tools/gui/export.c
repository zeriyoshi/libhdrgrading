/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* hdrg-ffmpeg's arguments: the program, the input, the output, the settings (--set each) */
#define HDRG_GUI_ARGS 160u

struct hdrg_gui_export {
    SDL_Thread *thread;
    SDL_Mutex *lock;
    char *input, *output;
    hdrg_gui_settings_t settings;
    hdrg_gui_progress_t progress;
    bool cancel;
};

/* a frame sent: the medium's, given back when the session releases it */
typedef struct {
    hdrg_gui_media_t *media;
    hdrg_gui_frame_t frame;
} hdrg_gui_held_t;

static void hdrg_gui_report(hdrg_gui_export_t *job, bool failed, const char *what)
{
    SDL_LockMutex(job->lock);
    job->progress.finished = true;
    job->progress.failed = failed;

    if (what != NULL) {
        snprintf(job->progress.error, sizeof(job->progress.error), "%s", what);
    }

    SDL_UnlockMutex(job->lock);
}

static void hdrg_gui_give_back(void *opaque)
{
    hdrg_gui_held_t *held;

    held = (hdrg_gui_held_t *)opaque;
    hdrg_gui_media_release(held->media, &held->frame);
    free(held);
}

static bool hdrg_gui_cancelled(hdrg_gui_export_t *job)
{
    bool cancel;

    SDL_LockMutex(job->lock);
    cancel = job->cancel;
    SDL_UnlockMutex(job->lock);

    return cancel;
}

/* a still: hdrg_convert of the file into the settings' writer, else the output's extension's */
static void hdrg_gui_still(hdrg_gui_export_t *job)
{
    hdrg_encode_params_t encode;
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint8_t *output;
    size_t size, output_size;
    void *input;

    input = SDL_LoadFile(job->input, &size);
    if (input == NULL) {
        hdrg_gui_report(job, true, SDL_GetError());

        return;
    }

    encode = job->settings.encode;
    hdrg_context_desc_init(&desc);
    desc.gpu = job->settings.gpu;
    status = hdrg_context_create(&desc, &ctx);

    /* the writer: the settings', else the output's extension's (the modules beside the program) */
    if (status == HDRG_OK) {
        hdrg_gui_load_modules(ctx);
        status = hdrg_gui_set_writer(ctx, &job->settings, job->output);

        if (status != HDRG_OK) {
            hdrg_context_destroy(ctx);
        }
    }

    if (status == HDRG_OK) {
        output = NULL;
        status = hdrg_convert(ctx, (const uint8_t *)input, size, &job->settings.params,
                              &job->settings.display, &encode, &output, &output_size);

        if (status == HDRG_OK && !SDL_SaveFile(job->output, output, output_size)) {
            hdrg_free(output);
            hdrg_context_destroy(ctx);
            SDL_free(input);
            hdrg_gui_report(job, true, SDL_GetError());

            return;
        }

        hdrg_free(output);
        hdrg_context_destroy(ctx);
    }

    SDL_free(input);
    hdrg_gui_report(job, status != HDRG_OK,
                    (status != HDRG_OK) ? hdrg_status_string(status) : NULL);
}

/* the session's outputs into the encoder until the end (false: an error or the cancel) */
static bool hdrg_gui_encode(hdrg_gui_export_t *job, hdrg_gui_media_t *media,
                            hdrg_session_t *session, hdrg_gui_encoder_t *encoder,
                            const hdrg_gui_info_t *info, uint8_t *luma, uint8_t *chroma)
{
    hdrg_session_output_t out;
    hdrg_session_frame_t frame;
    hdrg_gui_held_t *held;
    hdrg_status_t status;
    uint8_t read;
    bool ended;

    ended = false;

    for (;;) {
        if (hdrg_gui_cancelled(job)) {
            return false;
        }

        memset(&out, 0, sizeof(out));
        out.planes[0] = luma;
        out.planes[1] = chroma;
        out.strides[0] = (size_t)info->width * 2u;
        out.strides[1] = (size_t)((info->width + 1u) / 2u) * 4u;
        status = hdrg_session_receive_frame(session, &out);

        if (status == HDRG_EOF) {
            return true;
        }

        if (status == HDRG_OK) {
            if (!hdrg_gui_encoder_write(encoder, luma, chroma, out.pts)) {
                return false;
            }

            SDL_LockMutex(job->lock);
            job->progress.done_us = out.pts;
            SDL_UnlockMutex(job->lock);

            continue;
        }

        if (status != HDRG_ERR_AGAIN || ended) {
            return false;
        }

        held = (hdrg_gui_held_t *)calloc(1, sizeof(*held));
        if (held == NULL) {
            return false;
        }

        held->media = media;
        read = hdrg_gui_media_read(media, &held->frame);

        if (read != HDRG_GUI_READ_FRAME) {
            free(held);
            ended = true;

            if (read == HDRG_GUI_READ_ERROR || hdrg_session_send_frame(session, NULL) != HDRG_OK) {
                return false;
            }

            continue;
        }

        hdrg_gui_session_frame(&held->frame, &frame);
        frame.release = hdrg_gui_give_back;
        frame.opaque = held;
        status = hdrg_session_send_frame(session, &frame);
        if (status != HDRG_OK) {
            hdrg_gui_give_back(held);

            return false;
        }
    }
}

/* a video by the platform's encoder: the session's 4:2:0 10-bit PQ of the BT.2020 container */
static void hdrg_gui_video(hdrg_gui_export_t *job, hdrg_gui_media_t *media,
                           const hdrg_gui_info_t *info)
{
    hdrg_gui_settings_t settings;
    hdrg_session_pixfmt_t output;
    hdrg_context_desc_t desc;
    hdrg_gui_encoder_t *encoder;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint8_t *luma, *chroma;
    bool ok;
    char error[HDRG_GUI_ERROR];

    settings = job->settings;
    settings.display.gamut = HDRG_GAMUT_BT2020;
    memset(&output, 0, sizeof(output));
    output.layout = HDRG_YUV_420;
    output.matrix = HDRG_MATRIX_BT2020;
    output.bits = 10;
    output.interleaved = true;
    output.msb = true;
    hdrg_context_desc_init(&desc);
    desc.gpu = settings.gpu;
    ctx = NULL;
    session = NULL;
    encoder = NULL;
    luma = (uint8_t *)malloc((size_t)info->width * 2u * info->height);
    chroma = (uint8_t *)malloc((size_t)((info->width + 1u) / 2u) * 4u * ((info->height + 1u) / 2u));
    status = hdrg_context_create(&desc, &ctx);

    if (status == HDRG_OK) {
        status =
            hdrg_gui_session_open(ctx, info, &settings, HDRG_LATENCY_OFFLINE, &output, &session);
    }

    if (status != HDRG_OK || luma == NULL || chroma == NULL) {
        snprintf(error, sizeof(error), "session: %s",
                 hdrg_status_string((status != HDRG_OK) ? status : HDRG_ERR_NOMEM));
        ok = false;
    } else {
        ok = hdrg_gui_encoder_open(job->output, info->width, info->height, info->rate_num,
                                   info->rate_den, hdrg_session_peak(session), &encoder, error,
                                   sizeof(error));
    }

    if (ok) {
        ok = hdrg_gui_encode(job, media, session, encoder, info, luma, chroma);
        snprintf(error, sizeof(error), "%s",
                 hdrg_gui_cancelled(job) ? "cancelled" : "encoding failed");
        ok = hdrg_gui_encoder_close(encoder, ok, ok ? error : NULL, ok ? sizeof(error) : 0) && ok;
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(luma);
    free(chroma);
    hdrg_gui_report(job, !ok, ok ? NULL : error);
}

#if defined(HDRG_GUI_FFMPEG)
/* a video by hdrg-ffmpeg (beside this program) with the settings as --set */
static void hdrg_gui_ffmpeg(hdrg_gui_export_t *job)
{
    static const char *const gpu[3] = {"off", "auto", "filters"};
    const char *args[HDRG_GUI_ARGS];
    hdrg_setting_t setting;
    SDL_Process *process;
    uint32_t i, n, s;
    int exit_code;
    char program[1024], sets[HDRG_GUI_ARGS][96];
    double value;

    snprintf(program, sizeof(program), "%shdrg-ffmpeg%s", SDL_GetBasePath(),
#if defined(_WIN32)
             ".exe"
#else
             ""
#endif
    );
    n = 0;
    s = 0;
    args[n++] = program;
    args[n++] = "--preset";
    args[n++] = hdrg_setting_value_name("preset", (double)job->settings.params.preset);
    args[n++] = "--temporal";
    args[n++] = hdrg_setting_value_name("temporal", (double)job->settings.temporal);
    args[n++] = "--gpu";
    args[n++] = gpu[(job->settings.gpu < 3u) ? job->settings.gpu : 1u];

    for (i = 0; hdrg_setting_at(i, &setting) && n + 4u < HDRG_GUI_ARGS; i++) {
        if (setting.part == HDRG_SETTING_ENCODE || strcmp(setting.name, "preset") == 0 ||
            hdrg_setting_get(setting.name, &job->settings.params, &job->settings.display,
                             &job->settings.encode, &value) != HDRG_OK) {
            continue;
        }

        snprintf(sets[s], sizeof(sets[s]), "%s=%.17g", setting.name, value);
        args[n++] = "--set";
        args[n++] = sets[s++];
    }

    args[n++] = job->input;
    args[n++] = job->output;
    args[n] = NULL;
    process = SDL_CreateProcess(args, false);
    if (process == NULL) {
        hdrg_gui_report(job, true, SDL_GetError());

        return;
    }

    exit_code = 1;
    SDL_WaitProcess(process, true, &exit_code);
    SDL_DestroyProcess(process);
    hdrg_gui_report(job, exit_code != 0, (exit_code != 0) ? "hdrg-ffmpeg failed" : NULL);
}
#endif

static int SDLCALL hdrg_gui_export_run(void *data)
{
    hdrg_gui_export_t *job;
    hdrg_gui_media_t *media;
    hdrg_gui_info_t info;
    char error[HDRG_GUI_ERROR];

    job = (hdrg_gui_export_t *)data;

    if (!hdrg_gui_media_open(job->input, &media, &info, error, sizeof(error))) {
        hdrg_gui_report(job, true, error);

        return 0;
    }

    SDL_LockMutex(job->lock);
    job->progress.total_us = info.duration_us;
    SDL_UnlockMutex(job->lock);

    if (info.rate_num == 0) {
        hdrg_gui_media_close(media);
        hdrg_gui_still(job);
    } else if (hdrg_gui_encoder_available()) {
        hdrg_gui_video(job, media, &info);
        hdrg_gui_media_close(media);
    } else {
        hdrg_gui_media_close(media);
#if defined(HDRG_GUI_FFMPEG)
        hdrg_gui_ffmpeg(job);
#else
        hdrg_gui_report(job, true, "this build writes no video (no encoder, FFmpeg not enabled)");
#endif
    }

    return 0;
}

extern bool hdrg_gui_export_start(const char *input, const char *output,
                                  const hdrg_gui_settings_t *settings, hdrg_gui_export_t **out,
                                  char *error, size_t error_size)
{
    hdrg_gui_export_t *job;

    *out = NULL;
    job = (hdrg_gui_export_t *)calloc(1, sizeof(*job));
    if (job == NULL) {
        snprintf(error, error_size, "out of memory");

        return false;
    }

    job->input = SDL_strdup(input);
    job->output = SDL_strdup(output);
    job->settings = *settings;
    job->lock = SDL_CreateMutex();

    if (job->input == NULL || job->output == NULL || job->lock == NULL) {
        snprintf(error, error_size, "out of memory");
        hdrg_gui_export_finish(job);

        return false;
    }

    job->thread = SDL_CreateThread(hdrg_gui_export_run, "hdrg-gui export", job);
    if (job->thread == NULL) {
        snprintf(error, error_size, "%s", SDL_GetError());
        hdrg_gui_export_finish(job);

        return false;
    }

    *out = job;

    return true;
}

extern void hdrg_gui_export_progress(hdrg_gui_export_t *job, hdrg_gui_progress_t *out)
{
    SDL_LockMutex(job->lock);
    *out = job->progress;
    SDL_UnlockMutex(job->lock);
}

extern void hdrg_gui_export_finish(hdrg_gui_export_t *job)
{
    if (job == NULL) {
        return;
    }

    if (job->thread != NULL) {
        SDL_LockMutex(job->lock);
        job->cancel = true;
        SDL_UnlockMutex(job->lock);
        SDL_WaitThread(job->thread, NULL);
    }

    SDL_DestroyMutex(job->lock);
    SDL_free(job->input);
    SDL_free(job->output);
    free(job);
}

extern bool hdrg_gui_export_video_supported(void)
{
#if defined(HDRG_GUI_FFMPEG)
    return true;
#else
    return hdrg_gui_encoder_available();
#endif
}
