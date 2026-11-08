/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../test_jpeg.h"
#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/math.h"
#include "io/io.h"

/*
 * 病的ケース集 (輝度の評価セット)。合成画像を既定パラメータで変換し、
 * PQ プライマリを復号して実測した APL・領域輝度・最大輝度面積を基準と比べる。
 * 指標表は常に標準出力へ出す。環境変数 HDRG_EVAL_DIR を指定すると、各ケースの
 * 入力 (SDR JPEG) と出力 (HDR JPEG) を書き出す (実機での主観確認用)。
 */
#define PT_W         384u
#define PT_H         256u
#define PT_REGIONS   4u
#define PT_REF_WHITE 203.0f
#define PT_BUDGET    250.0f /* a quarter of the PQ peak (1000 cd/m^2) */

typedef struct {
    uint64_t key;    /* ノイズ・字形の乱数 (カウンタハッシュの鍵) */
    uint32_t draws;  /* 呼び出しごとに変える標本番号 */
    uint8_t *px;     /* sRGB 8-bit RGB */
    uint8_t *region; /* 0 = 対象外, 1..3 = 評価領域 */
} pt_canvas_t;

typedef struct {
    hdrg_stats_t stats;
    uint32_t count[PT_REGIONS];
    double apl, bright_area, mean[PT_REGIONS], sum_sq[PT_REGIONS], code_mean[PT_REGIONS][3];
} pt_metrics_t;

static hdrg_context_t *g_ctx;
static hdrg_allocator_t g_alloc;
static hdrg_encode_params_t g_hybrid; /* the evaluation's output form */

static float pq_eotf_nits(uint8_t code)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f,
                c3 = 18.6875f;
    float p, num;

    p = powf((float)code / 255.0f, 1.0f / m2);
    num = HDRG_TEST_MAX(p - c1, 0.0f);

    return powf(num / (c2 - c3 * p), 1.0f / m1) * 10000.0f;
}

/* SDR (sRGB、参照白 203 nits) での輝度 */
static double sdr_nits(uint8_t r, uint8_t g, uint8_t b)
{
    return 203.0 * (0.2126 * (double)hdrg_math_srgb_eotf((float)r / 255.0f) +
                    0.7152 * (double)hdrg_math_srgb_eotf((float)g / 255.0f) +
                    0.0722 * (double)hdrg_math_srgb_eotf((float)b / 255.0f));
}

static uint8_t clamp_code(int32_t v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void pt_set(pt_canvas_t *c, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p;

    if (x >= PT_W || y >= PT_H) {
        return;
    }

    p = c->px + ((size_t)y * PT_W + x) * 3;
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

static void pt_fill(pt_canvas_t *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t r,
                    uint8_t g, uint8_t b)
{
    uint32_t x, y;

    for (y = y0; y < y1 && y < PT_H; y++) {
        for (x = x0; x < x1 && x < PT_W; x++) {
            pt_set(c, x, y, r, g, b);
        }
    }
}

static void pt_mark(pt_canvas_t *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                    uint8_t region)
{
    uint32_t x, y;

    for (y = y0; y < y1 && y < PT_H; y++) {
        for (x = x0; x < x1 && x < PT_W; x++) {
            c->region[(size_t)y * PT_W + x] = region;
        }
    }
}

/* 一様ノイズ (輝度方向、全チャネル同量): カメラ画像らしさ */
static void pt_noise(pt_canvas_t *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                     uint8_t amp)
{
    uint32_t x, y;
    uint8_t i, *p;
    int32_t d;

    for (y = y0; y < y1 && y < PT_H; y++) {
        for (x = x0; x < x1 && x < PT_W; x++) {
            p = c->px + ((size_t)y * PT_W + x) * 3;
            d = (int32_t)(hdrg_hash_u32(c->key, x, y, c->draws) % (2u * amp + 1u)) - (int32_t)amp;
            for (i = 0; i < 3; i++) {
                p[i] = clamp_code((int32_t)p[i] + d);
            }
        }
    }

    c->draws++;
}

/* 発光体: 飽和したコア + 背景へ溶けるハロー。region はコアだけに付ける */
static void pt_glow(pt_canvas_t *c, uint32_t cx, uint32_t cy, float core, float halo,
                    uint8_t region)
{
    uint32_t x, y, x0, y0, x1, y1;
    uint8_t i, *p;
    float dx, dy, d, w;

    x0 = (uint32_t)HDRG_TEST_MAX((float)cx - core - halo, 0.0f);
    y0 = (uint32_t)HDRG_TEST_MAX((float)cy - core - halo, 0.0f);
    x1 = (uint32_t)((float)cx + core + halo + 1.0f);
    y1 = (uint32_t)((float)cy + core + halo + 1.0f);

    for (y = y0; y < y1 && y < PT_H; y++) {
        for (x = x0; x < x1 && x < PT_W; x++) {
            dx = (float)x - (float)cx;
            dy = (float)y - (float)cy;
            d = sqrtf(dx * dx + dy * dy);
            p = c->px + ((size_t)y * PT_W + x) * 3;

            if (d <= core) {
                p[0] = 255;
                p[1] = 255;
                p[2] = 255;
                c->region[(size_t)y * PT_W + x] = region;

                continue;
            }

            w = 1.0f - (d - core) / halo;
            if (w <= 0.0f) {
                continue;
            }

            w = 0.9f * w * w;
            for (i = 0; i < 3; i++) {
                p[i] = clamp_code((int32_t)lrintf((float)p[i] + (255.0f - (float)p[i]) * w));
            }
        }
    }
}

/*
 * 擬似文字: 5x7 のセグメント字形を行に並べる (縦画の右に 50% の AA 画素)。
 * 文字画素に region を付ける。
 */
static void pt_text(pt_canvas_t *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                    uint32_t line_h, uint8_t level, uint8_t region)
{
    uint32_t bits, gx, gy, x, y, seg;
    uint8_t *p, aa;

    for (gy = y0; gy + 7 <= y1; gy += line_h) {
        for (gx = x0; gx + 5 <= x1; gx += 7) {
            bits = hdrg_hash_u32(c->key, gx, gy, c->draws);
            if ((bits & 7u) == 0) {
                continue;
            }

            for (seg = 0; seg < 7; seg++) {
                if (((bits >> (seg + 3)) & 1u) == 0) {
                    continue;
                }

                for (y = 0; y < 7; y++) {
                    for (x = 0; x < 5; x++) {
                        if ((seg == 0 && y == 0) || (seg == 1 && y == 3) || (seg == 2 && y == 6) ||
                            (seg == 3 && x == 0 && y <= 3) || (seg == 4 && x == 4 && y <= 3) ||
                            (seg == 5 && x == 0 && y >= 3) || (seg == 6 && x == 4 && y >= 3)) {
                            pt_set(c, gx + x, gy + y, level, level, level);
                            c->region[(size_t)(gy + y) * PT_W + gx + x] = region;

                            if (seg >= 3 && gx + x + 1 < PT_W) {
                                p = c->px + ((size_t)(gy + y) * PT_W + gx + x + 1) * 3;
                                aa = (uint8_t)(((uint32_t)p[0] + level) / 2u);

                                if (c->region[(size_t)(gy + y) * PT_W + gx + x + 1] != region) {
                                    p[0] = aa;
                                    p[1] = aa;
                                    p[2] = aa;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    c->draws++;
}

static void pt_ellipse(pt_canvas_t *c, float cx, float cy, float rx, float ry, uint8_t r, uint8_t g,
                       uint8_t b, uint8_t region, float inset)
{
    uint32_t x, y;
    float dx, dy, d;

    for (y = 0; y < PT_H; y++) {
        for (x = 0; x < PT_W; x++) {
            dx = ((float)x - cx) / rx;
            dy = ((float)y - cy) / ry;
            d = sqrtf(dx * dx + dy * dy);
            if (d > 1.0f) {
                continue;
            }

            pt_set(c, x, y, r, g, b);

            /* 描いた画素の評価領域は上書きする (縁から inset 以内は対象外) */
            c->region[(size_t)y * PT_W + x] =
                (d * HDRG_TEST_MAX(rx, ry) <= HDRG_TEST_MAX(rx, ry) - inset) ? region : 0;
        }
    }
}

static void make_document(pt_canvas_t *c)
{
    pt_fill(c, 0, 0, PT_W, PT_H, 250, 250, 250);
    pt_text(c, 16, 16, PT_W - 16, PT_H - 16, 12, 25, 1);
}

static void make_dark_ui(pt_canvas_t *c)
{
    uint32_t i;

    pt_fill(c, 0, 0, PT_W, PT_H, 30, 30, 34);
    pt_fill(c, 0, 0, PT_W, 28, 45, 45, 50);
    pt_fill(c, 200, 40, PT_W - 12, PT_H - 12, 45, 45, 50);
    pt_text(c, 12, 44, 190, PT_H - 12, 13, 255, 1);
    pt_text(c, 210, 52, PT_W - 20, PT_H - 20, 13, 255, 1);

    for (i = 0; i < 8; i++) {
        pt_fill(c, 10 + i * 22, 8, 22 + i * 22, 20, 255, 255, 255);
        pt_mark(c, 10 + i * 22, 8, 22 + i * 22, 20, 2);
    }
}

static void make_blown_sky(pt_canvas_t *c)
{
    uint32_t y, horizon;

    horizon = PT_H * 2 / 5;
    pt_fill(c, 0, 0, PT_W, horizon, 255, 255, 255);
    pt_mark(c, 0, 0, PT_W, horizon - 4, 1);

    for (y = horizon; y < PT_H; y++) {
        pt_fill(c, 0, y, PT_W, y + 1, 70, (uint8_t)(80 + (y - horizon) / 8), 55);
    }

    pt_fill(c, 0, horizon, PT_W, horizon + 3, 190, 190, 185);
    pt_noise(c, 0, horizon, PT_W, PT_H, 20);
}

static void make_snow(pt_canvas_t *c)
{
    uint32_t i, x;

    pt_fill(c, 0, 0, PT_W, PT_H / 4, 205, 215, 235);
    pt_noise(c, 0, 0, PT_W, PT_H / 4, 3);
    pt_fill(c, 0, PT_H / 4, PT_W, PT_H, 238, 240, 245);
    pt_noise(c, 0, PT_H / 4, PT_W, PT_H, 5);
    pt_mark(c, 0, PT_H / 4 + 4, PT_W, PT_H, 1);

    for (i = 0; i < 9; i++) {
        x = 20 + i * 41;
        pt_fill(c, x, PT_H / 6, x + 10, PT_H * 3 / 5, 35, 45, 35);
        pt_mark(c, x, PT_H / 6, x + 10, PT_H * 3 / 5, 0);
        pt_noise(c, x, PT_H / 6, x + 10, PT_H * 3 / 5, 8);
    }
}

static void make_night_lamps(pt_canvas_t *c)
{
    uint32_t i;

    pt_fill(c, 0, 0, PT_W, PT_H, 20, 18, 25);
    pt_noise(c, 0, 0, PT_W, PT_H, 4);

    for (i = 0; i < 6; i++) {
        pt_glow(c, 40 + i * 60, 60 + (i % 3) * 60, 3.0f + (float)(i % 2), 12.0f, 1);
    }
}

static void make_gradient(pt_canvas_t *c)
{
    uint32_t x;
    uint8_t v;

    for (x = 0; x < PT_W; x++) {
        v = (uint8_t)(60 + (190 * x) / (PT_W - 1));
        pt_fill(c, x, 0, x + 1, PT_H, (uint8_t)(v * 6 / 10), (uint8_t)(v * 8 / 10), v);
    }

    pt_mark(c, 0, 0, PT_W, PT_H, 1);
}

static void make_anime(pt_canvas_t *c)
{
    uint32_t i;

    pt_fill(c, 0, 0, PT_W, PT_H, 120, 180, 240);
    pt_fill(c, 0, PT_H * 3 / 4, PT_W, PT_H, 90, 170, 90);
    pt_ellipse(c, 190.0f, 150.0f, 82.0f, 92.0f, 20, 15, 25, 0, 0.0f);
    pt_ellipse(c, 190.0f, 150.0f, 80.0f, 90.0f, 70, 40, 110, 3, 8.0f);
    pt_ellipse(c, 190.0f, 175.0f, 50.0f, 58.0f, 20, 15, 25, 0, 0.0f);
    pt_ellipse(c, 190.0f, 175.0f, 48.0f, 56.0f, 250, 220, 200, 2, 6.0f);

    for (i = 0; i < 3; i++) {
        pt_fill(c, 140 + i * 30, 80 + i * 4, 156 + i * 30, 86 + i * 4, 255, 255, 255);
        pt_mark(c, 140 + i * 30, 80 + i * 4, 156 + i * 30, 86 + i * 4, 1);
    }
}

static void make_hud(pt_canvas_t *c)
{
    pt_fill(c, 0, 0, PT_W, PT_H, 100, 110, 120);
    pt_noise(c, 0, 0, PT_W, PT_H, 25);
    pt_glow(c, 300, 60, 7.0f, 22.0f, 2);
    pt_fill(c, 0, PT_H - 52, PT_W, PT_H, 15, 15, 20);
    pt_fill(c, 16, PT_H - 40, 136, PT_H - 32, 250, 250, 250);
    pt_mark(c, 16, PT_H - 40, 136, PT_H - 32, 1);
    pt_text(c, 160, PT_H - 44, PT_W - 16, PT_H - 8, 12, 250, 1);
}

static void make_face(pt_canvas_t *c)
{
    pt_fill(c, 0, 0, PT_W, PT_H, 90, 110, 130);
    pt_ellipse(c, 150.0f, 128.0f, 70.0f, 90.0f, 215, 165, 135, 1, 6.0f);
    pt_fill(c, 260, 60, 340, 200, 225, 25, 30);
    pt_mark(c, 266, 66, 334, 194, 2);
    pt_noise(c, 0, 0, PT_W, PT_H, 3);
}

static pt_canvas_t pt_new(void)
{
    pt_canvas_t c;

    c.px = (uint8_t *)calloc((size_t)PT_W * PT_H, 3);
    c.region = (uint8_t *)calloc((size_t)PT_W * PT_H, 1);

    TEST_ASSERT_NOT_NULL(c.px);
    TEST_ASSERT_NOT_NULL(c.region);

    c.key = UINT64_C(0x5054484F4C4F4759);
    c.draws = 0;

    return c;
}

static void pt_free(pt_canvas_t *c)
{
    free(c->px);
    free(c->region);
}

static void pt_dump(const char *name, const char *suffix, const uint8_t *data, size_t size)
{
    const char *dir;
    FILE *fp;
    char path[1024];

    dir = getenv("HDRG_EVAL_DIR");
    if (dir == NULL || dir[0] == '\0') {
        return;
    }

    (void)snprintf(path, sizeof(path), "%s/%s%s.jpg", dir, name, suffix);

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return;
    }

    (void)fwrite(data, 1, size, fp);
    (void)fclose(fp);
}

/* 入力そのもの (非圧縮) と評価領域のマスク。解析の調査・旧版との比較用 */
static void pt_dump_ppm(const char *name, const uint8_t *px, const uint8_t *region)
{
    const char *dir;
    FILE *fp;
    char path[1024];

    dir = getenv("HDRG_EVAL_DIR");
    if (dir == NULL || dir[0] == '\0') {
        return;
    }

    (void)snprintf(path, sizeof(path), "%s/%s_sdr.ppm", dir, name);

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return;
    }

    (void)fprintf(fp, "P6 %u %u 255\n", PT_W, PT_H);
    (void)fwrite(px, 1, (size_t)PT_W * PT_H * 3, fp);
    (void)fclose(fp);

    (void)snprintf(path, sizeof(path), "%s/%s_mask.pgm", dir, name);

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return;
    }

    (void)fprintf(fp, "P5 %u %u 255\n", PT_W, PT_H);
    (void)fwrite(region, 1, (size_t)PT_W * PT_H, fp);
    (void)fclose(fp);
}

/*
 * 変換 → PQ プライマリを復号して指標を測る。階調はハイブリッドの PQ プライマリ
 * (ゲインマップのヘッドルーム上限を受けない HDR レンディション) で評価する。
 * params == NULL は既定値
 */
static void pt_measure(const char *name, const pt_canvas_t *c, const hdrg_params_t *params,
                       pt_metrics_t *m)
{
    hdrg_params_t effective;
    hdrg_raw_image_t raw;
    hdrg_decoded_t primary;
    uint32_t x, y;
    uint8_t *out, *sdr, region, ch;
    size_t out_size, sdr_size, idx;
    float nits[3], lum;

    memset(m, 0, sizeof(*m));
    memset(&raw, 0, sizeof(raw));
    raw.pixels = c->px;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = PT_W;
    raw.height = PT_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)PT_W * 3;

    if (params != NULL) {
        effective = *params;
    } else {
        hdrg_params_init(&effective, HDRG_PRESET_PHOTO);
    }

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert_raw(g_ctx, &raw, &effective, NULL, &g_hybrid, &out, &out_size));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &m->stats));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, out, out_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, &primary));
    TEST_ASSERT_FALSE(hdrg_color_within(&primary.color, HDRG_GAMUT_SRGB));

    pt_dump(name, "", out, out_size);

    if (params == NULL) {
        pt_dump_ppm(name, c->px, c->region);
    }

    if (params == NULL && hdrg_test_jpeg_rgb(&g_alloc, c->px, PT_W, PT_H, (size_t)PT_W * 3, 95,
                                             NULL, 0, NULL, 0, &sdr, &sdr_size) == HDRG_OK) {
        pt_dump(name, "_sdr", sdr, sdr_size);
        hdrg_mem_free(&g_alloc, sdr);
    }

    for (y = 0; y < PT_H; y++) {
        for (x = 0; x < PT_W; x++) {
            idx = (size_t)y * PT_W + x;

            for (ch = 0; ch < 3; ch++) {
                nits[ch] = pq_eotf_nits(primary.pixels[(size_t)y * primary.stride + x * 3 + ch]);
            }

            lum = 0.228975f * nits[0] + 0.691739f * nits[1] + 0.079287f * nits[2];
            m->apl += (double)lum;

            if (lum > 2.0f * PT_REF_WHITE) {
                m->bright_area += 1.0;
            }

            region = c->region[idx];
            if (region == 0 || region >= PT_REGIONS) {
                continue;
            }

            m->count[region]++;
            m->mean[region] += (double)lum;
            m->sum_sq[region] += (double)lum * (double)lum;
            for (ch = 0; ch < 3; ch++) {
                m->code_mean[region][ch] +=
                    (double)primary.pixels[(size_t)y * primary.stride + x * 3 + ch];
            }
        }
    }

    m->apl /= (double)PT_W * PT_H;
    m->bright_area /= (double)PT_W * PT_H;
    for (region = 1; region < PT_REGIONS; region++) {
        if (m->count[region] == 0) {
            continue;
        }

        m->mean[region] /= (double)m->count[region];
        m->sum_sq[region] = sqrt(HDRG_TEST_MAX(
            m->sum_sq[region] / (double)m->count[region] - m->mean[region] * m->mean[region], 0.0));
        for (ch = 0; ch < 3; ch++) {
            m->code_mean[region][ch] /= (double)m->count[region];
        }
    }

    printf("%-14s sdr_apl %6.1f  apl %6.1f (est %6.1f, scale %.3f)  diffuse %.2f  light %.4f  "
           "bright %.4f  r1 %6.1f±%-5.1f r2 %6.1f±%-5.1f r3 %6.1f±%-5.1f\n",
           name, (double)m->stats.sdr_apl_nits, m->apl, (double)m->stats.apl_nits,
           (double)m->stats.apl_scale, (double)m->stats.diffuse_stops,
           (double)m->stats.light_area_ratio, m->bright_area, m->mean[1], m->sum_sq[1], m->mean[2],
           m->sum_sq[2], m->mean[3], m->sum_sq[3]);

    hdrg_decoded_free(&g_alloc, &primary);
    hdrg_free(out);
}

/* SDR の時点で予算を超える画像は、コントラストの持ち上げ分 (~1.09x) までに留まる */
static double apl_limit(const pt_metrics_t *m)
{
    return 1.1 * HDRG_TEST_MAX((double)PT_BUDGET, 1.15 * (double)m->stats.sdr_apl_nits);
}

/*
 * 拡散面の水準: SDR の nits に拡散面の倍率を掛けたもの。既定の contrast (1) と clarity (0) は
 * 面の比を変えないので、ディザと色の LUT の丸めの分だけ
 */
static double diffuse_limit(const pt_metrics_t *m, double sdr)
{
    return 1.05 * sdr * pow(2.0, (double)m->stats.diffuse_stops);
}

static void test_white_document(void)
{
    pt_canvas_t c;
    pt_metrics_t m;

    c = pt_new();
    make_document(&c);
    pt_measure("document", &c, NULL, &m);

    TEST_ASSERT_TRUE(m.apl <= apl_limit(&m));
    TEST_ASSERT_TRUE(m.bright_area <= 0.01);

    pt_free(&c);
}

static void test_dark_ui_text_not_boosted(void)
{
    pt_canvas_t c;
    pt_metrics_t m;

    c = pt_new();
    make_dark_ui(&c);
    pt_measure("dark_ui", &c, NULL, &m);

    /* 白文字・アイコンは拡散面 (専用の判定なし: 鋭い縁は光源でない) */
    TEST_ASSERT_TRUE(m.mean[1] <= diffuse_limit(&m, PT_REF_WHITE));
    TEST_ASSERT_TRUE(m.mean[2] <= diffuse_limit(&m, PT_REF_WHITE));
    TEST_ASSERT_TRUE(m.stats.light_area_ratio <= 0.01f);

    pt_free(&c);
}

static void test_blown_sky_limited(void)
{
    pt_canvas_t c;
    pt_metrics_t m;

    c = pt_new();
    make_blown_sky(&c);
    pt_measure("blown_sky", &c, NULL, &m);

    /* 白飛びした空は光る面として少し上がってよいが、ピーク (1000) の 6 割まで */
    TEST_ASSERT_TRUE(m.apl <= apl_limit(&m));
    TEST_ASSERT_TRUE(m.mean[1] <= 600.0);

    pt_free(&c);
}

static void test_snow_limited(void)
{
    pt_canvas_t c;
    pt_metrics_t m;

    c = pt_new();
    make_snow(&c);
    pt_measure("snow", &c, NULL, &m);

    TEST_ASSERT_TRUE(m.apl <= apl_limit(&m));
    TEST_ASSERT_TRUE(m.mean[1] <= diffuse_limit(&m, PT_REF_WHITE));

    pt_free(&c);
}

static void test_night_lamps_keep_headroom(void)
{
    pt_canvas_t c;
    pt_metrics_t m;

    c = pt_new();
    make_night_lamps(&c);
    pt_measure("night_lamps", &c, NULL, &m);

    /* 小さな光源 (ハローつき) はヘッドルームを使う */
    TEST_ASSERT_TRUE(m.mean[1] >= 3.0 * PT_REF_WHITE);
    TEST_ASSERT_TRUE(m.stats.light_area_ratio > 0.0f);
    TEST_ASSERT_TRUE(m.apl <= apl_limit(&m));

    pt_free(&c);
}

static void test_gradient_monotonic(void)
{
    hdrg_params_t params;
    hdrg_raw_image_t raw;
    hdrg_decoded_t primary;
    pt_canvas_t c;
    pt_metrics_t m;
    uint32_t x, y;
    uint8_t *out;
    size_t out_size;
    double col, prev;

    c = pt_new();
    make_gradient(&c);
    pt_measure("gradient", &c, NULL, &m);

    memset(&raw, 0, sizeof(raw));
    raw.pixels = c.px;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = PT_W;
    raw.height = PT_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)PT_W * 3;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(g_ctx, &raw, &params, NULL, &g_hybrid, &out, &out_size));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, out, out_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, &primary));

    /* 列平均の輝度 (G の PQ コード) が単調: 階調の反転・段差がない */
    prev = 0.0;
    for (x = 0; x < PT_W; x++) {
        col = 0.0;
        for (y = 0; y < PT_H; y++) {
            col += (double)primary.pixels[(size_t)y * primary.stride + x * 3 + 1];
        }

        col /= (double)PT_H;
        TEST_ASSERT_TRUE(col >= prev - 0.6);

        prev = col;
    }

    hdrg_decoded_free(&g_alloc, &primary);
    hdrg_free(out);
    pt_free(&c);
}

static void test_anime_flat_fills(void)
{
    hdrg_params_t params;
    pt_canvas_t c;
    pt_metrics_t m, flat;

    c = pt_new();
    make_anime(&c);
    pt_measure("anime", &c, NULL, &m);

    /* ハイライトの白い帯は平坦な塗り (拡散面) で、光らせない */
    TEST_ASSERT_TRUE(m.mean[1] <= diffuse_limit(&m, PT_REF_WHITE));

    /*
     * ゲイン場が色面の中で斑にならない (標準偏差が平均の 1.5% 以下)。clarity は
     * 縁の近くを意図的に持ち上げるので、ゲインだけを見るために 0 にする
     */
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.clarity = 0.0f;
    pt_measure("anime_noclar", &c, &params, &flat);

    TEST_ASSERT_TRUE(flat.sum_sq[2] <= HDRG_TEST_MAX(0.015 * flat.mean[2], 0.5));
    TEST_ASSERT_TRUE(flat.sum_sq[3] <= HDRG_TEST_MAX(0.015 * flat.mean[3], 0.5));

    pt_free(&c);
}

static void test_hud_suppressed_scene_kept(void)
{
    pt_canvas_t c;
    pt_metrics_t m;

    c = pt_new();
    make_hud(&c);
    pt_measure("hud", &c, NULL, &m);

    TEST_ASSERT_TRUE(m.mean[1] <= diffuse_limit(&m, sdr_nits(250, 250, 250)));
    TEST_ASSERT_TRUE(m.mean[2] >= 2.5 * PT_REF_WHITE);

    pt_free(&c);
}

static void test_skin_not_expanded(void)
{
    hdrg_params_t colorimetric;
    pt_canvas_t c;
    pt_metrics_t m, ref;
    uint8_t ch;
    double moved;

    c = pt_new();
    make_face(&c);
    pt_measure("face", &c, NULL, &m);

    hdrg_params_init(&colorimetric, HDRG_PRESET_PHOTO);
    colorimetric.expand_strength = 0.0f;
    pt_measure("face_colorim", &c, &colorimetric, &ref);

    /* 肌 (中間調) は拡散面の水準のまま、ハイライトとして伸ばさない */
    TEST_ASSERT_TRUE(m.mean[1] <= diffuse_limit(&m, sdr_nits(215, 165, 135)));

    /* 肌は測色的変換のまま、境界近くの彩色 (赤) だけが P3 へ広がる */
    moved = 0.0;
    for (ch = 0; ch < 3; ch++) {
        TEST_ASSERT_TRUE(fabs(m.code_mean[1][ch] - ref.code_mean[1][ch]) <= 1.0);

        moved = HDRG_TEST_MAX(moved, fabs(m.code_mean[2][ch] - ref.code_mean[2][ch]));
    }

    TEST_ASSERT_TRUE(moved >= 2.0);

    pt_free(&c);
}

static void test_apl_budget_adjustable(void)
{
    hdrg_params_t params;
    pt_canvas_t c;
    pt_metrics_t def, off, raised;

    c = pt_new();
    make_snow(&c);
    pt_measure("snow", &c, NULL, &def);

    /* 上限を外すと共通係数は 1 のまま、明るくなる */
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.apl_budget_nits = 0.0f;
    params.apl_peak_fraction = 0.0f;
    pt_measure("snow_apl_off", &c, &params, &off);

    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, off.stats.apl_scale);
    TEST_ASSERT_TRUE(off.apl >= 1.2 * def.apl);

    /* 上限を上げると、その上限に収まる範囲で明るくなる */
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.apl_budget_nits = 320.0f;
    params.apl_peak_fraction = 0.5f;
    pt_measure("snow_apl_320", &c, &params, &raised);

    TEST_ASSERT_TRUE(raised.apl > def.apl);
    TEST_ASSERT_TRUE(raised.apl <= 1.1 * 320.0);

    pt_free(&c);
}

static void test_natural_photo_guard(void)
{
    hdrg_stats_t stats;
    FILE *fp;
    uint8_t *data, *out;
    long size;
    size_t out_size;

    HDRG_TEST_REQUIRE_BUILTIN(PNG);

    fp = fopen(HDRG_TEST_ASSETS_DIR "/kodim23.png", "rb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_END));

    size = ftell(fp);
    TEST_ASSERT_TRUE(size > 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_SET));

    data = (uint8_t *)malloc((size_t)size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_size_t((size_t)size, fread(data, 1, (size_t)size, fp));

    (void)fclose(fp);

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert(g_ctx, data, (size_t)size, NULL, NULL, NULL, &out, &out_size));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_get_last_stats(g_ctx, &stats));

    printf("%-14s sdr_apl %6.1f  apl est %6.1f  scale %.3f  diffuse %.2f  max_gain %.3f\n",
           "kodim23", (double)stats.sdr_apl_nits, (double)stats.apl_nits, (double)stats.apl_scale,
           (double)stats.diffuse_stops, (double)stats.applied_max_gain_stops);

    /* 自然画像: 拡散面がそろって上がり、全体が明るくなる */
    TEST_ASSERT_TRUE(stats.apl_nits >= 1.8f * stats.sdr_apl_nits);

    pt_dump("kodim23", "", out, out_size);

    hdrg_free(out);
    free(data);
}

void setUp(void)
{
#ifndef HDRG_WITH_LCMS2
    /* the evaluation measures the PQ primary (HYBRID), which needs lcms2 */
    TEST_IGNORE_MESSAGE("lcms2 disabled");
#endif
}

void tearDown(void)
{
}

int main(void)
{
    g_alloc = hdrg_test_allocator();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&g_ctx, NULL, 0));
    hdrg_encode_params_init(&g_hybrid);
    g_hybrid.output_mode = HDRG_OUTPUT_HYBRID;

    UNITY_BEGIN();
    RUN_TEST(test_white_document);
    RUN_TEST(test_dark_ui_text_not_boosted);
    RUN_TEST(test_blown_sky_limited);
    RUN_TEST(test_snow_limited);
    RUN_TEST(test_night_lamps_keep_headroom);
    RUN_TEST(test_gradient_monotonic);
    RUN_TEST(test_anime_flat_fills);
    RUN_TEST(test_hud_suppressed_scene_kept);
    RUN_TEST(test_skin_not_expanded);
    RUN_TEST(test_apl_budget_adjustable);
    RUN_TEST(test_natural_photo_guard);

    hdrg_context_destroy(g_ctx);

    return UNITY_END();
}
