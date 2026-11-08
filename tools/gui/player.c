/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* the frames sent that the source view keeps a copy of, beyond the session's latency */
#define HDRG_GUI_SPARE 4u
/* the pictures: the window's, the latest finished, the one being written */
#define HDRG_GUI_PICTURES 3u
/* the PQ codes' table */
#define HDRG_GUI_CODES 65536u
/* the source view's table of a Y'CbCr sample's transfer (index: the value x 4095) */
#define HDRG_GUI_LEVELS 4096u

/* a frame sent: its backend's frame, given back when the session releases it */
typedef struct {
    hdrg_gui_player_t *player;
    hdrg_gui_frame_t frame;
} hdrg_gui_sent_t;

/* a copy of a sent frame's pixels for the source view */
typedef struct {
    uint8_t *planes[2];
    size_t strides[2];
    int64_t pts_us;
    bool used;
} hdrg_gui_copy_t;

struct hdrg_gui_player {
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *wake;
    hdrg_context_t *ctx;
    hdrg_gui_media_t *media;
    hdrg_session_t *session;
    hdrg_gui_info_t info;
    hdrg_gui_settings_t settings, wanted;
    hdrg_gui_copy_t *copies;
    uint16_t *pq, *pictures[HDRG_GUI_PICTURES];
    float *lut, levels[HDRG_GUI_LEVELS], rgba_levels[256];
    uint64_t serial, serials[HDRG_GUI_PICTURES], started_ns;
    int64_t pts[HDRG_GUI_PICTURES], seek_us, position_us, first_pts;
    uint32_t widths[HDRG_GUI_PICTURES];
    uint8_t views[HDRG_GUI_PICTURES];
    uint32_t copy_count, copy_live; /* the copies made, and those the session's latency needs */
    uint8_t latest, locked, view;
    float white_level, lut_white, apply_ms, peak_nits; /* peak_nits: the session's, under lock */
    bool quit, playing, restart, seeking, regrade, ended, shown, view_changed, eos_sent;
    char error[HDRG_GUI_ERROR];
};

/* a float as IEEE half (round to nearest even), the infinities and NaN as such */
static inline uint16_t hdrg_gui_half(float value)
{
    uint32_t bits, sign, exponent, mantissa, shifted;

    memcpy(&bits, &value, sizeof(bits));
    sign = (bits >> 16) & 0x8000u;
    exponent = (bits >> 23) & 0xffu;
    mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu) {
        return (uint16_t)(sign | 0x7c00u | ((mantissa != 0) ? 0x200u : 0u));
    }

    if (exponent > 142u) {
        return (uint16_t)(sign | 0x7c00u);
    }

    if (exponent < 113u) {
        if (exponent < 102u) {
            return (uint16_t)sign;
        }

        mantissa |= 0x800000u;
        shifted = mantissa >> (126u - exponent);

        if (((mantissa >> (125u - exponent)) & 1u) != 0 &&
            ((mantissa & ((1u << (125u - exponent)) - 1u)) != 0 || (shifted & 1u) != 0)) {
            shifted++;
        }

        return (uint16_t)(sign | shifted);
    }

    shifted = ((exponent - 112u) << 10) | (mantissa >> 13);

    if ((mantissa & 0x1000u) != 0 && ((mantissa & 0x2fffu) != 0)) {
        shifted++;
    }

    return (uint16_t)(sign | shifted);
}

static inline float hdrg_gui_pq_nits(float code)
{
    const float e = powf(code, 1.0f / 78.84375f), n = fmaxf(e - 0.8359375f, 0.0f);

    return 10000.0f * powf(n / (18.8515625f - 18.6875f * e), 1.0f / 0.1593017578125f);
}

static inline void hdrg_gui_put(uint16_t *out, float r, float g, float b)
{
    out[0] = hdrg_gui_half(r);
    out[1] = hdrg_gui_half(g);
    out[2] = hdrg_gui_half(b);
    out[3] = 0x3c00u;
}

/* the PQ table: a code's linear light, the SDR white at the window's white level */
static void hdrg_gui_tables(hdrg_gui_player_t *player)
{
    uint32_t i;
    float white;

    white = (player->settings.display.sdr_white_nits > 0.0f)
                ? player->settings.display.sdr_white_nits
                : 203.0f;

    for (i = 0; i < HDRG_GUI_CODES; i++) {
        player->lut[i] = hdrg_gui_pq_nits((float)i / 65535.0f) / white * player->white_level;
    }

    for (i = 0; i < HDRG_GUI_LEVELS; i++) {
        player->levels[i] =
            powf((float)i / (float)(HDRG_GUI_LEVELS - 1u), 2.4f) * player->white_level;
    }

    for (i = 0; i < 256u; i++) {
        player->rgba_levels[i] = ((float)i <= 10.31475f)
                                     ? (float)i / 255.0f / 12.92f
                                     : powf(((float)i / 255.0f + 0.055f) / 1.055f, 2.4f);
        player->rgba_levels[i] *= player->white_level;
    }

    player->lut_white = player->white_level;
}

/* the session's PQ R'G'B' (the display's gamut) as linear BT.709, rows of pitch pixels */
static void hdrg_gui_show_output(hdrg_gui_player_t *player, uint16_t *out, size_t pitch)
{
    const uint32_t width = player->info.width, height = player->info.height;
    const uint16_t *in;
    const bool wide = player->settings.display.gamut == HDRG_GAMUT_P3;
    /* Display P3 to BT.709 (BT.2020's container is not asked for) */
    static const float p3[9] = {1.224940f, -0.224940f, 0.0f,       -0.042057f, 1.042057f,
                                0.0f,      -0.019638f, -0.078636f, 1.098274f};
    uint32_t x, y;
    float r, g, b;

    for (y = 0; y < height; y++) {
        in = player->pq + (size_t)y * width * 3u;

        for (x = 0; x < width; x++) {
            r = player->lut[in[x * 3u]];
            g = player->lut[in[x * 3u + 1u]];
            b = player->lut[in[x * 3u + 2u]];

            if (wide) {
                hdrg_gui_put(out + ((size_t)y * pitch + x) * 4u, p3[0] * r + p3[1] * g + p3[2] * b,
                             p3[3] * r + p3[4] * g + p3[5] * b, p3[6] * r + p3[7] * g + p3[8] * b);
            } else {
                hdrg_gui_put(out + ((size_t)y * pitch + x) * 4u, r, g, b);
            }
        }
    }
}

/*
 * a source frame (its copy) as linear BT.709: R'G'B' by the matrix, gamma 2.4 (video) / sRGB,
 * rows of pitch pixels
 */
static void hdrg_gui_show_source(hdrg_gui_player_t *player, const hdrg_gui_copy_t *copy,
                                 uint16_t *out, size_t pitch)
{
    const uint32_t width = player->info.width, height = player->info.height;
    const uint8_t format = player->info.format, *row, *pair;
    uint32_t x, y, c;
    float kr, kb, kg, yv, cb, cr, rgb[3], scale, offset, chroma;

    kr = (player->info.matrix == HDRG_MATRIX_BT601)    ? 0.299f
         : (player->info.matrix == HDRG_MATRIX_BT2020) ? 0.2627f
                                                       : 0.2126f;
    kb = (player->info.matrix == HDRG_MATRIX_BT601)    ? 0.114f
         : (player->info.matrix == HDRG_MATRIX_BT2020) ? 0.0593f
                                                       : 0.0722f;
    kg = 1.0f - kr - kb;
    scale = player->info.full_range ? 1.0f / 255.0f : 1.0f / 219.0f;
    chroma = player->info.full_range ? 1.0f / 255.0f : 1.0f / 224.0f;
    offset = player->info.full_range ? 0.0f : 16.0f;

    for (y = 0; y < height; y++) {
        row = copy->planes[0] + (size_t)y * copy->strides[0];

        for (x = 0; x < width; x++) {
            if (format == HDRG_GUI_RGBA) {
                hdrg_gui_put(out + ((size_t)y * pitch + x) * 4u, player->rgba_levels[row[x * 4u]],
                             player->rgba_levels[row[x * 4u + 1u]],
                             player->rgba_levels[row[x * 4u + 2u]]);

                continue;
            }

            pair = copy->planes[1] + (size_t)(y / 2u) * copy->strides[1];

            if (format == HDRG_GUI_P010) {
                yv = (float)(((const uint16_t *)row)[x] >> 8);
                cb = (float)(((const uint16_t *)pair)[(x / 2u) * 2u] >> 8);
                cr = (float)(((const uint16_t *)pair)[(x / 2u) * 2u + 1u] >> 8);
            } else {
                yv = (float)row[x];
                cb = (float)pair[(x / 2u) * 2u];
                cr = (float)pair[(x / 2u) * 2u + 1u];
            }

            yv = (yv - offset) * scale;
            cb = (cb - 128.0f) * chroma;
            cr = (cr - 128.0f) * chroma;
            rgb[0] = yv + 2.0f * (1.0f - kr) * cr;
            rgb[2] = yv + 2.0f * (1.0f - kb) * cb;
            rgb[1] = (yv - kr * rgb[0] - kb * rgb[2]) / kg;

            for (c = 0; c < 3u; c++) {
                rgb[c] = player->levels[(uint32_t)(fminf(fmaxf(rgb[c], 0.0f), 1.0f) *
                                                       (float)(HDRG_GUI_LEVELS - 1u) +
                                                   0.5f)];
            }

            hdrg_gui_put(out + ((size_t)y * pitch + x) * 4u, rgb[0], rgb[1], rgb[2]);
        }
    }
}

static void hdrg_gui_release(void *opaque)
{
    hdrg_gui_sent_t *sent;

    sent = (hdrg_gui_sent_t *)opaque;
    hdrg_gui_media_release(sent->player->media, &sent->frame);
    free(sent);
}

static void hdrg_gui_fail(hdrg_gui_player_t *player, const char *what, hdrg_status_t status)
{
    SDL_LockMutex(player->lock);
    snprintf(player->error, sizeof(player->error), "%s: %s", what, hdrg_status_string(status));
    player->playing = false;
    SDL_UnlockMutex(player->lock);
}

/* a copy of the frame for the source view, in the oldest free slot */
static void hdrg_gui_keep(hdrg_gui_player_t *player, const hdrg_gui_frame_t *frame)
{
    const uint32_t rows[2] = {player->info.height, (player->info.height + 1u) / 2u};
    hdrg_gui_copy_t *copy;
    uint32_t i, p, y;
    size_t bytes;

    copy = &player->copies[0];

    for (i = 0; i < player->copy_live; i++) {
        if (!player->copies[i].used || player->copies[i].pts_us < copy->pts_us) {
            copy = &player->copies[i];

            if (!copy->used) {
                break;
            }
        }
    }

    for (p = 0; p < ((player->info.format == HDRG_GUI_RGBA) ? 1u : 2u); p++) {
        bytes = (size_t)player->info.width * ((player->info.format == HDRG_GUI_RGBA)   ? 4u
                                              : (player->info.format == HDRG_GUI_P010) ? 2u
                                                                                       : 1u);
        bytes += (p == 1u && (player->info.width & 1u) != 0)
                     ? ((player->info.format == HDRG_GUI_P010) ? 2u : 1u)
                     : 0u;

        if (copy->planes[p] == NULL) {
            copy->planes[p] = (uint8_t *)malloc(bytes * rows[p]);
            if (copy->planes[p] == NULL) {
                return;
            }
        }

        copy->strides[p] = bytes;

        for (y = 0; y < rows[p]; y++) {
            memcpy(copy->planes[p] + (size_t)y * bytes,
                   frame->planes[p] + (size_t)y * frame->strides[p], bytes);
        }
    }

    copy->pts_us = frame->pts_us;
    copy->used = true;
}

/* the session made again with the wanted settings, the medium from seek_us (or the position) */
static void hdrg_gui_restart(hdrg_gui_player_t *player, bool seek, int64_t seek_us)
{
    hdrg_session_pixfmt_t output;
    hdrg_context_desc_t desc;
    hdrg_status_t status;
    uint32_t i;

    /* the status reads the peak under the lock, never the session the thread replaces */
    SDL_LockMutex(player->lock);
    player->peak_nits = 0.0f;
    SDL_UnlockMutex(player->lock);
    hdrg_session_destroy(player->session);
    player->session = NULL;

    if (player->ctx == NULL || player->settings.gpu != player->wanted.gpu) {
        hdrg_context_destroy(player->ctx);
        hdrg_context_desc_init(&desc);
        desc.gpu = player->wanted.gpu;
        status = hdrg_context_create(&desc, &player->ctx);
        if (status != HDRG_OK) {
            player->ctx = NULL;
            hdrg_gui_fail(player, "context", status);

            return;
        }
    }

    player->settings = player->wanted;
    hdrg_gui_media_seek(player->media, seek ? seek_us : player->position_us);
    memset(&output, 0, sizeof(output));
    output.bits = 16;
    status = hdrg_gui_session_open(player->ctx, &player->info, &player->settings,
                                   HDRG_LATENCY_PLAYBACK, &output, &player->session);
    if (status != HDRG_OK) {
        player->session = NULL;
        hdrg_gui_fail(player, "session", status);

        return;
    }

    SDL_LockMutex(player->lock);
    player->peak_nits = hdrg_session_peak(player->session);
    SDL_UnlockMutex(player->lock);
    player->copy_live = hdrg_session_latency(player->session) + 1u + HDRG_GUI_SPARE;
    player->copy_live =
        (player->copy_live < player->copy_count) ? player->copy_live : player->copy_count;

    for (i = 0; i < player->copy_count; i++) {
        player->copies[i].used = false;
    }

    player->eos_sent = false;
    player->ended = false;
    player->first_pts = INT64_MIN;
    player->error[0] = '\0';
    hdrg_gui_tables(player);
}

/* the next output: frames sent until one comes (false: the end or an error) */
static bool hdrg_gui_next(hdrg_gui_player_t *player, hdrg_session_output_t *out)
{
    hdrg_session_frame_t frame;
    hdrg_gui_sent_t *sent;
    hdrg_status_t status;
    uint8_t read;

    for (;;) {
        memset(out, 0, sizeof(*out));
        out->planes[0] = (uint8_t *)player->pq;
        out->strides[0] = (size_t)player->info.width * 6u;
        status = hdrg_session_receive_frame(player->session, out);

        if (status == HDRG_OK) {
            return true;
        }

        if (status == HDRG_EOF) {
            return false;
        }

        if (status != HDRG_ERR_AGAIN || player->eos_sent) {
            hdrg_gui_fail(player, "receive", status);

            return false;
        }

        sent = (hdrg_gui_sent_t *)calloc(1, sizeof(*sent));
        if (sent == NULL) {
            hdrg_gui_fail(player, "memory", HDRG_ERR_NOMEM);

            return false;
        }

        sent->player = player;
        read = hdrg_gui_media_read(player->media, &sent->frame);

        if (read != HDRG_GUI_READ_FRAME) {
            free(sent);
            status = hdrg_session_send_frame(player->session, NULL);
            player->eos_sent = true;

            if (read == HDRG_GUI_READ_ERROR || status != HDRG_OK) {
                hdrg_gui_fail(player, "decode", HDRG_ERR_DECODE);

                return false;
            }

            continue;
        }

        hdrg_gui_keep(player, &sent->frame);
        hdrg_gui_session_frame(&sent->frame, &frame);
        frame.release = hdrg_gui_release;
        frame.opaque = sent;
        status = hdrg_session_send_frame(player->session, &frame);
        if (status != HDRG_OK) {
            hdrg_gui_release(sent);
            hdrg_gui_fail(player, "send", status);

            return false;
        }
    }
}

/*
 * the picture of the view into a free slot, then the latest: the output, the source's copy (the
 * output when it has none), or both side by side (the source left, the output right: rows of
 * twice the width)
 */
static void hdrg_gui_publish(hdrg_gui_player_t *player, int64_t pts_us)
{
    const hdrg_gui_copy_t *copy;
    const uint32_t width = player->info.width;
    uint32_t i;
    uint16_t *out;
    uint8_t slot, view;

    SDL_LockMutex(player->lock);
    slot = (uint8_t)((player->latest + 1u) % HDRG_GUI_PICTURES);

    if (slot == player->locked) {
        slot = (uint8_t)((slot + 1u) % HDRG_GUI_PICTURES);
    }

    view = player->view;
    SDL_UnlockMutex(player->lock);
    out = player->pictures[slot];
    copy = NULL;

    for (i = 0; view != HDRG_GUI_VIEW_OUTPUT && i < player->copy_count; i++) {
        if (player->copies[i].used && player->copies[i].pts_us == pts_us) {
            copy = &player->copies[i];
        }
    }

    if (view == HDRG_GUI_VIEW_BOTH) {
        if (copy != NULL) {
            hdrg_gui_show_source(player, copy, out, 2u * (size_t)width);
        } else {
            hdrg_gui_show_output(player, out, 2u * (size_t)width);
        }

        hdrg_gui_show_output(player, out + (size_t)width * 4u, 2u * (size_t)width);
    } else if (view == HDRG_GUI_VIEW_SOURCE && copy != NULL) {
        hdrg_gui_show_source(player, copy, out, width);
    } else {
        hdrg_gui_show_output(player, out, width);
    }

    SDL_LockMutex(player->lock);
    player->serials[slot] = ++player->serial;
    player->pts[slot] = pts_us;
    player->widths[slot] = (view == HDRG_GUI_VIEW_BOTH) ? 2u * width : width;
    player->views[slot] = view;
    player->latest = slot;
    player->position_us = pts_us;
    player->shown = true;
    SDL_UnlockMutex(player->lock);
}

/* waits until the frame's time from the first frame shown (playing) */
static void hdrg_gui_pace(hdrg_gui_player_t *player, int64_t pts_us)
{
    uint64_t now, due;

    if (player->first_pts == INT64_MIN) {
        player->first_pts = pts_us;
        player->started_ns = SDL_GetTicksNS();

        return;
    }

    due = player->started_ns + (uint64_t)((pts_us - player->first_pts) * 1000);
    now = SDL_GetTicksNS();

    if (due > now) {
        SDL_LockMutex(player->lock);
        SDL_WaitConditionTimeout(player->wake, player->lock, (Sint32)((due - now) / 1000000u));
        SDL_UnlockMutex(player->lock);
    }
}

static int SDLCALL hdrg_gui_run(void *data)
{
    hdrg_gui_player_t *player;
    hdrg_session_output_t out;
    uint64_t start;
    int64_t seek_us;
    bool restart, seek, regrade, view, got;

    player = (hdrg_gui_player_t *)data;

    for (;;) {
        SDL_LockMutex(player->lock);

        while (!player->quit && !player->restart && !player->seeking && !player->regrade &&
               !player->view_changed && (!player->playing || player->ended) && player->shown) {
            SDL_WaitCondition(player->wake, player->lock);
        }

        if (player->quit) {
            SDL_UnlockMutex(player->lock);

            break;
        }

        restart = player->restart || player->seeking || player->regrade || player->session == NULL;
        seek = player->seeking;
        seek_us = player->seek_us;
        regrade = player->regrade;
        view = player->view_changed;
        player->restart = false;
        player->seeking = false;
        player->regrade = false;
        player->view_changed = false;
        SDL_UnlockMutex(player->lock);

        if (view && !restart) {
            hdrg_gui_tables(player);

            if (!player->playing) {
                hdrg_gui_publish(player, player->position_us);

                continue;
            }
        }

        if (restart) {
            hdrg_gui_restart(player, seek || regrade, seek ? seek_us : player->position_us);
            player->shown = false;

            if (player->session == NULL) {
                SDL_LockMutex(player->lock);
                player->shown = true;
                SDL_UnlockMutex(player->lock);

                continue;
            }
        }

        start = SDL_GetTicksNS();
        got = hdrg_gui_next(player, &out);
        player->apply_ms = (float)(SDL_GetTicksNS() - start) / 1e6f;

        if (!got) {
            SDL_LockMutex(player->lock);
            player->ended = true;
            player->playing = false;
            player->shown = true;
            SDL_UnlockMutex(player->lock);

            continue;
        }

        if (player->playing) {
            hdrg_gui_pace(player, out.pts);
        }

        hdrg_gui_publish(player, out.pts);
    }

    return 0;
}

extern bool hdrg_gui_player_open(const char *path, const hdrg_gui_settings_t *settings,
                                 hdrg_gui_player_t **out, char *error, size_t error_size)
{
    hdrg_gui_player_t *player;
    uint32_t i;
    size_t pixels;

    *out = NULL;
    player = (hdrg_gui_player_t *)calloc(1, sizeof(*player));
    if (player == NULL) {
        snprintf(error, error_size, "out of memory");

        return false;
    }

    if (!hdrg_gui_media_open(path, &player->media, &player->info, error, error_size)) {
        free(player);

        return false;
    }

    pixels = (size_t)player->info.width * player->info.height;
    player->copy_count = 2u * HDRG_SESSION_MAX_WINDOW + 1u + HDRG_GUI_SPARE;
    player->copy_live = 1u;
    player->copies = (hdrg_gui_copy_t *)calloc(player->copy_count, sizeof(*player->copies));
    player->pq = (uint16_t *)malloc(pixels * 6u);
    player->lut = (float *)malloc(HDRG_GUI_CODES * sizeof(float));
    player->lock = SDL_CreateMutex();
    player->wake = SDL_CreateCondition();
    player->settings = *settings;
    player->wanted = *settings;
    player->white_level = 1.0f;
    player->latest = 0;
    player->locked = UINT8_MAX;
    player->first_pts = INT64_MIN;

    for (i = 0; i < HDRG_GUI_PICTURES; i++) {
        player->pictures[i] = (uint16_t *)calloc(pixels * 8u, sizeof(uint16_t));
    }

    if (player->copies == NULL || player->pq == NULL || player->lut == NULL ||
        player->lock == NULL || player->wake == NULL || player->pictures[0] == NULL ||
        player->pictures[1] == NULL || player->pictures[2] == NULL) {
        snprintf(error, error_size, "out of memory");
        hdrg_gui_player_close(player);

        return false;
    }

    player->thread = SDL_CreateThread(hdrg_gui_run, "hdrg-gui player", player);
    if (player->thread == NULL) {
        snprintf(error, error_size, "%s", SDL_GetError());
        hdrg_gui_player_close(player);

        return false;
    }

    *out = player;

    return true;
}

extern void hdrg_gui_player_settings(hdrg_gui_player_t *player, const hdrg_gui_settings_t *settings)
{
    SDL_LockMutex(player->lock);
    player->wanted = *settings;
    player->regrade = true;
    SDL_SignalCondition(player->wake);
    SDL_UnlockMutex(player->lock);
}

extern void hdrg_gui_player_view(hdrg_gui_player_t *player, float white_level, uint8_t view)
{
    SDL_LockMutex(player->lock);

    if (player->white_level != white_level || player->view != view) {
        player->white_level = white_level;
        player->view = view;
        player->view_changed = true;
        SDL_SignalCondition(player->wake);
    }

    SDL_UnlockMutex(player->lock);
}

extern void hdrg_gui_player_play(hdrg_gui_player_t *player, bool playing)
{
    SDL_LockMutex(player->lock);

    if (playing && player->ended) {
        player->seeking = true;
        player->seek_us = 0;
    }

    player->playing = playing && player->info.rate_num != 0;
    player->first_pts = INT64_MIN;
    SDL_SignalCondition(player->wake);
    SDL_UnlockMutex(player->lock);
}

extern void hdrg_gui_player_seek(hdrg_gui_player_t *player, int64_t pts_us)
{
    SDL_LockMutex(player->lock);
    player->seeking = true;
    player->seek_us = pts_us;
    player->first_pts = INT64_MIN;
    SDL_SignalCondition(player->wake);
    SDL_UnlockMutex(player->lock);
}

extern bool hdrg_gui_player_lock(hdrg_gui_player_t *player, hdrg_gui_picture_t *out)
{
    bool ok;

    SDL_LockMutex(player->lock);
    ok = player->serial != 0;

    if (ok) {
        player->locked = player->latest;
        out->pixels = player->pictures[player->latest];
        out->serial = player->serials[player->latest];
        out->pts_us = player->pts[player->latest];
        out->width = player->widths[player->latest];
        out->view = player->views[player->latest];
        out->height = player->info.height;
    }

    SDL_UnlockMutex(player->lock);

    return ok;
}

extern void hdrg_gui_player_unlock(hdrg_gui_player_t *player)
{
    SDL_LockMutex(player->lock);
    player->locked = UINT8_MAX;
    SDL_UnlockMutex(player->lock);
}

extern void hdrg_gui_player_status(hdrg_gui_player_t *player, hdrg_gui_status_t *out)
{
    SDL_LockMutex(player->lock);
    memset(out, 0, sizeof(*out));
    out->info = player->info;
    out->position_us = player->position_us;
    out->apply_ms = player->apply_ms;
    out->peak_nits = player->peak_nits;
    out->playing = player->playing;
    out->ended = player->ended;
    memcpy(out->error, player->error, sizeof(out->error));
    SDL_UnlockMutex(player->lock);
}

extern void hdrg_gui_player_close(hdrg_gui_player_t *player)
{
    uint32_t i;

    if (player == NULL) {
        return;
    }

    if (player->thread != NULL) {
        SDL_LockMutex(player->lock);
        player->quit = true;
        SDL_SignalCondition(player->wake);
        SDL_UnlockMutex(player->lock);
        SDL_WaitThread(player->thread, NULL);
    }

    hdrg_session_destroy(player->session);
    hdrg_context_destroy(player->ctx);
    hdrg_gui_media_close(player->media);

    for (i = 0; player->copies != NULL && i < player->copy_count; i++) {
        free(player->copies[i].planes[0]);
        free(player->copies[i].planes[1]);
    }

    for (i = 0; i < HDRG_GUI_PICTURES; i++) {
        free(player->pictures[i]);
    }

    free(player->copies);
    free(player->pq);
    free(player->lut);
    SDL_DestroyCondition(player->wake);
    SDL_DestroyMutex(player->lock);
    free(player);
}
