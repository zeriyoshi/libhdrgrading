/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * Writes the seed corpus of fuzz_convert into a directory (fuzz_seeds DIR):
 * small JPEG, PNG and GIF files over the decoders' branches (color tags, EXIF,
 * chroma subsampling, bit depths, palettes, interlacing, the library's own HDR
 * outputs). Small
 * seeds keep each run short, so the fuzzer gets through many more of them.
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the native jpegli API: JPEGs of 4:2:0 / 4:2:2 chroma (the library writes 4:4:4) */
#include "lib/jpegli/encode.h"

#include "hdrgrading/hdrgrading.h"
#include "io/io.h"
#include "../tests/test_jpeg.h"

#define SEED_W   48u
#define SEED_H   32u
#define SEED_MAX (64u << 10)

typedef struct {
    const char *type;
    const uint8_t *data;
    uint32_t length;
} seed_chunk_t;

/* "Exif\0\0" + little-endian TIFF with orientation = 6 */
static const uint8_t g_exif_orientation6[] = {
    'E',  'x',  'i',  'f',  0x00, 0x00, 'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* "Exif\0\0" + TIFF (II): IFD0 -> Exif IFD (ColorSpace 0xFFFF) -> Interop IFD ("R03") */
static const uint8_t g_exif_r03[80] = {
    'E',  'x',  'i',  'f',  0, 0,                                  /* APP1 header */
    'I',  'I',  42,   0,    8, 0, 0, 0,                            /* TIFF, IFD0 at 8 */
    1,    0,    0x69, 0x87, 4, 0, 1, 0, 0,  0, 26,   0,    0,   0, /* Exif IFD at 26 */
    0,    0,    0,    0,                                           /* no next IFD */
    2,    0,    0x01, 0xA0, 3, 0, 1, 0, 0,  0, 0xFF, 0xFF, 0,   0, /* uncalibrated */
    0x05, 0xA0, 4,    0,    1, 0, 0, 0, 56, 0, 0,    0,            /* Interop IFD at 56 */
    0,    0,    0,    0,                                           /* no next IFD */
    1,    0,    1,    0,    2, 0, 4, 0, 0,  0, 'R',  '0',  '3', 0, /* index "R03" */
    0,    0,    0,    0,                                           /* no next IFD */
};

static uint8_t g_raw[SEED_MAX], g_zlib[SEED_MAX], g_file[SEED_MAX], g_chunk[SEED_MAX];

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* a gradient with a specular dot and a white patch: every analysis branch has something */
static uint8_t seed_value(uint32_t x, uint32_t y, uint8_t c)
{
    if (x >= SEED_W / 2u && y >= SEED_H / 2u) {
        return 250;
    }

    if (x >= 5u && x < 8u && y >= 4u && y < 7u) {
        return 255;
    }

    return (uint8_t)(40u + (150u * y) / SEED_H + 30u * c + (x & 7u));
}

static bool seed_write(const char *dir, const char *name, const uint8_t *data, size_t size)
{
    FILE *fp;
    int32_t written;
    bool ok;
    char path[1024];

    written = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return false;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return false;
    }

    ok = fwrite(data, 1, size, fp) == size;
    ok = (fclose(fp) == 0) && ok;

    return ok;
}

static uint32_t png_crc(const uint8_t *data, size_t size)
{
    uint32_t crc, k;
    uint8_t b;
    size_t i;

    crc = 0xFFFFFFFFu;

    for (i = 0; i < size; i++) {
        crc ^= data[i];

        for (b = 0; b < 8u; b++) {
            k = ((crc & 1u) != 0) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ k;
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

/* a chunk at p (length, type, data, the CRC of the type and data); returns its size */
static size_t png_chunk(uint8_t *p, const char *type, const uint8_t *data, uint32_t length)
{
    put_be32(p, length);
    memcpy(p + 4, type, 4);

    if (length > 0) {
        memcpy(p + 8, data, length);
    }

    put_be32(p + 8 + length, png_crc(p + 4, length + 4u));

    return 12u + length;
}

/* data as a zlib stream of stored deflate blocks; returns its size */
static size_t zlib_stored(uint8_t *out, const uint8_t *data, size_t size)
{
    uint32_t a, b, block;
    size_t i, n, pos;

    out[0] = 0x78;
    out[1] = 0x01;
    n = 2;
    pos = 0;

    do {
        block = (size - pos > 65535u) ? 65535u : (uint32_t)(size - pos);
        out[n] = (uint8_t)((pos + block == size) ? 1u : 0u);
        put_le16(out + n + 1, block);
        put_le16(out + n + 3, ~block & 0xFFFFu);
        memcpy(out + n + 5, data + pos, block);
        n += 5u + block;
        pos += block;
    } while (pos < size);

    a = 1;
    b = 0;
    for (i = 0; i < size; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }

    put_be32(out + n, (b << 16) | a);

    return n + 4u;
}

/* the sample of channel ch at (x, y): a palette index, a gray level, a color or an alpha */
static uint16_t png_sample(uint8_t color_type, uint8_t depth, uint32_t x, uint32_t y, uint8_t ch)
{
    uint16_t v;

    if (color_type == 3) {
        return (uint16_t)((x / 6u + y / 4u) & 15u);
    }

    if (ch == 3 || (color_type == 4 && ch == 1)) {
        v = (uint16_t)((x * 5u + y * 2u) & 0xFFu);
    } else if (color_type == 0 || color_type == 4) {
        v = (uint16_t)((2u * seed_value(x, y, 0) + 5u * seed_value(x, y, 1) + seed_value(x, y, 2)) /
                       8u);
    } else {
        v = seed_value(x, y, ch);
    }

    if (depth == 16) {
        return (uint16_t)((v * 257u) ^ ((x * y) & 0xFFu));
    }

    return v;
}

/* the pixels x0, x0 + dx, ... of row y after its filter byte (none); returns the bytes written */
static size_t png_row(uint8_t *out, uint8_t color_type, uint8_t depth, uint32_t y, uint32_t x0,
                      uint32_t dx)
{
    const uint8_t channels[7] = {1, 0, 3, 1, 2, 0, 4};
    uint32_t x, acc, bits;
    uint16_t v;
    uint8_t ch;
    size_t n;

    out[0] = 0;
    n = 1;
    acc = 0;
    bits = 0;
    for (x = x0; x < SEED_W; x += dx) {
        for (ch = 0; ch < channels[color_type]; ch++) {
            v = png_sample(color_type, depth, x, y, ch);

            if (depth == 16) {
                out[n++] = (uint8_t)(v >> 8);
                out[n++] = (uint8_t)(v & 0xFFu);
            } else if (depth == 8) {
                out[n++] = (uint8_t)v;
            } else {
                acc = (acc << depth) | v;
                bits += depth;
                if (bits == 8) {
                    out[n++] = (uint8_t)acc;
                    acc = 0;
                    bits = 0;
                }
            }
        }
    }

    if (bits > 0) {
        out[n++] = (uint8_t)(acc << (8u - bits));
    }

    return n;
}

/* a SEED_W x SEED_H PNG of the color type and bit depth, with the chunks before its IDAT */
static bool png_seed(const char *dir, const char *name, uint8_t color_type, uint8_t depth,
                     bool adam7, const seed_chunk_t *chunks, uint8_t chunk_count)
{
    /* x0, y0, dx, dy: the whole image, then the seven Adam7 passes */
    const uint32_t pass[8][4] = {{0, 0, 1, 1}, {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8},
                                 {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    uint32_t y;
    uint8_t ihdr[13], p, first, last, i;
    size_t raw, n;

    raw = 0;
    first = adam7 ? 1u : 0u;
    last = adam7 ? 8u : 1u;
    for (p = first; p < last; p++) {
        for (y = pass[p][1]; y < SEED_H; y += pass[p][3]) {
            raw += png_row(g_raw + raw, color_type, depth, y, pass[p][0], pass[p][2]);
        }
    }

    memset(ihdr, 0, sizeof(ihdr));
    put_be32(ihdr, SEED_W);
    put_be32(ihdr + 4, SEED_H);
    ihdr[8] = depth;
    ihdr[9] = color_type;
    ihdr[12] = adam7 ? 1u : 0u;

    memcpy(g_file, "\x89PNG\r\n\x1a\n", 8);
    n = 8;
    n += png_chunk(g_file + n, "IHDR", ihdr, sizeof(ihdr));

    for (i = 0; i < chunk_count; i++) {
        n += png_chunk(g_file + n, chunks[i].type, chunks[i].data, chunks[i].length);
    }

    n += png_chunk(g_file + n, "IDAT", g_zlib, (uint32_t)zlib_stored(g_zlib, g_raw, raw));
    n += png_chunk(g_file + n, "IEND", NULL, 0);

    return seed_write(dir, name, g_file, n);
}

/* a 4-bit LZW code into the packed bytes (least significant bit first) */
static void gif_code(uint8_t *lzw, size_t *length, uint32_t *acc, uint32_t *bits, uint32_t code)
{
    *acc |= code << *bits;
    *bits += 4;

    while (*bits >= 8) {
        lzw[(*length)++] = (uint8_t)(*acc & 0xFFu);
        *acc >>= 8;
        *bits -= 8;
    }
}

static uint32_t gif_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t shift)
{
    if (x >= w / 2u && y >= h / 2u) {
        return 7;
    }

    return (x / 6u + y / 4u + shift) % 7u;
}

/*
 * The image data of a w x h frame of 8 colors: 4-bit LZW codes with a clear
 * code before every four pixels (the table never outgrows them), in sub-blocks;
 * returns the bytes written
 */
static size_t gif_frame(uint8_t *out, uint32_t w, uint32_t h, uint32_t shift)
{
    uint32_t x, y, acc, bits;
    size_t length, n, i, block;

    acc = 0;
    bits = 0;
    length = 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (((y * w + x) % 4u) == 0) {
                gif_code(g_raw, &length, &acc, &bits, 8);
            }

            gif_code(g_raw, &length, &acc, &bits, gif_index(x, y, w, h, shift));
        }
    }

    gif_code(g_raw, &length, &acc, &bits, 9);

    if (bits > 0) {
        g_raw[length++] = (uint8_t)acc;
    }

    out[0] = 3;
    n = 1;
    for (i = 0; i < length; i += block) {
        block = (length - i > 255u) ? 255u : length - i;
        out[n] = (uint8_t)block;
        memcpy(out + n + 1, g_raw + i, block);
        n += 1u + block;
    }

    out[n] = 0;

    return n + 1u;
}

/*
 * A GIF of 8 colors; animated adds the loop and comment extensions, a
 * transparent index and a second, interlaced frame with its own colors
 */
static bool gif_seed(const char *dir, const char *name, bool animated)
{
    const uint8_t loop[19] = {0x21, 0xFF, 11,  'N', 'E', 'T', 'S', 'C', 'A', 'P',
                              'E',  '2',  '.', '0', 3,   1,   0,   0,   0},
                  comment[8] = {0x21, 0xFE, 4, 's', 'e', 'e', 'd', 0},
                  control[8] = {0x21, 0xF9, 4, 0x05, 10, 0, 0, 0};
    uint8_t i;
    size_t n;

    memcpy(g_file, "GIF89a", 6);
    put_le16(g_file + 6, SEED_W);
    put_le16(g_file + 8, SEED_H);
    g_file[10] = 0xF2;
    g_file[11] = 0;
    g_file[12] = 0;
    n = 13;
    for (i = 0; i < 8u; i++) {
        g_file[n++] = (uint8_t)((i < 7u) ? i * 30u : 250u);
        g_file[n++] = (uint8_t)((i < 7u) ? i * 30u + 20u : 250u);
        g_file[n++] = (uint8_t)((i < 7u) ? i * 30u + 40u : 250u);
    }

    if (animated) {
        memcpy(g_file + n, loop, sizeof(loop));
        n += sizeof(loop);
        memcpy(g_file + n, comment, sizeof(comment));
        n += sizeof(comment);
        memcpy(g_file + n, control, sizeof(control));
        n += sizeof(control);
    }

    g_file[n] = 0x2C;
    put_le16(g_file + n + 1, 0);
    put_le16(g_file + n + 3, 0);
    put_le16(g_file + n + 5, SEED_W);
    put_le16(g_file + n + 7, SEED_H);
    g_file[n + 9] = 0;
    n += 10;
    n += gif_frame(g_file + n, SEED_W, SEED_H, 0);

    if (animated) {
        memcpy(g_file + n, control, sizeof(control));
        n += sizeof(control);

        g_file[n] = 0x2C;
        put_le16(g_file + n + 1, 8);
        put_le16(g_file + n + 3, 4);
        put_le16(g_file + n + 5, SEED_W / 2u);
        put_le16(g_file + n + 7, SEED_H / 2u);
        g_file[n + 9] = 0xC2;
        n += 10;
        for (i = 0; i < 8u; i++) {
            g_file[n++] = (uint8_t)(250u - i * 30u);
            g_file[n++] = (uint8_t)(200u - i * 25u);
            g_file[n++] = (uint8_t)(i * 20u);
        }

        n += gif_frame(g_file + n, SEED_W / 2u, SEED_H / 2u, 3);
    }

    g_file[n++] = 0x3B;

    return seed_write(dir, name, g_file, n);
}

static bool png_seeds(const char *dir, const uint8_t *icc, size_t icc_size)
{
    /* Display P3: white, red, green, blue (x, y) x 100000; then three equal primaries (unusable) */
    const uint32_t chrm[8] = {31270, 32900, 68000, 32000, 26500, 69000, 15000, 6000},
                   flat[8] = {31270, 32900, 30000, 30000, 30000, 30000, 30000, 30000};
    const uint8_t srgb[1] = {0}, cicp_p3[4] = {12, 13, 0, 1}, cicp_bt2020[4] = {9, 1, 0, 1};
    seed_chunk_t chunks[2];
    uint8_t gama[4], chrm_bytes[32], flat_bytes[32], plte[48], trns[4], i;
    size_t n;
    bool ok;

    put_be32(gama, 45455);

    for (i = 0; i < 8u; i++) {
        put_be32(chrm_bytes + i * 4u, chrm[i]);
        put_be32(flat_bytes + i * 4u, flat[i]);
    }

    for (i = 0; i < 16u; i++) {
        plte[i * 3u] = (uint8_t)(i * 16u);
        plte[i * 3u + 1u] = (uint8_t)(255u - i * 12u);
        plte[i * 3u + 2u] = (uint8_t)(i * 9u + 60u);
    }

    memset(trns, 0x40, sizeof(trns));

    ok = true;

    chunks[0] = (seed_chunk_t){"sRGB", srgb, sizeof(srgb)};
    ok = png_seed(dir, "rgb8_srgb.png", 2, 8, false, chunks, 1) && ok;
    chunks[0] = (seed_chunk_t){"gAMA", gama, sizeof(gama)};
    chunks[1] = (seed_chunk_t){"cHRM", chrm_bytes, sizeof(chrm_bytes)};
    ok = png_seed(dir, "rgb16_gama_chrm.png", 2, 16, false, chunks, 2) && ok;
    chunks[0] = (seed_chunk_t){"cHRM", flat_bytes, sizeof(flat_bytes)};
    ok = png_seed(dir, "rgb8_chrm_flat.png", 2, 8, false, chunks, 1) && ok;
    ok = png_seed(dir, "gray8.png", 0, 8, false, NULL, 0) && ok;
    ok = png_seed(dir, "gray_alpha8.png", 4, 8, false, NULL, 0) && ok;
    chunks[0] = (seed_chunk_t){"cICP", cicp_p3, sizeof(cicp_p3)};
    ok = png_seed(dir, "rgba8_cicp_p3.png", 6, 8, false, chunks, 1) && ok;
    chunks[0] = (seed_chunk_t){"cICP", cicp_bt2020, sizeof(cicp_bt2020)};
    ok = png_seed(dir, "rgb16_cicp_bt2020.png", 2, 16, false, chunks, 1) && ok;
    chunks[0] = (seed_chunk_t){"PLTE", plte, sizeof(plte)};
    chunks[1] = (seed_chunk_t){"tRNS", trns, sizeof(trns)};
    ok = png_seed(dir, "palette4_trns.png", 3, 4, false, chunks, 2) && ok;
    ok = png_seed(dir, "palette8.png", 3, 8, false, chunks, 1) && ok;
    ok = png_seed(dir, "rgb8_adam7.png", 2, 8, true, NULL, 0) && ok;

    if (icc != NULL) {
        memcpy(g_chunk, "P3\0\0", 4);
        n = 4u + zlib_stored(g_chunk + 4, icc, icc_size);
        chunks[0] = (seed_chunk_t){"iCCP", g_chunk, (uint32_t)n};
        ok = png_seed(dir, "rgb8_iccp_p3.png", 2, 8, false, chunks, 1) && ok;
    }

    return ok;
}

/* a JPEG of the seed image with the ICC profile and EXIF given; keep receives it (or NULL) */
static bool jpeg_seed(const hdrg_allocator_t *alloc, const char *dir, const char *name,
                      const uint8_t *icc, size_t icc_size, const uint8_t *exif, size_t exif_size,
                      uint8_t **keep, size_t *keep_size)
{
    uint32_t x, y;
    uint8_t c, *jpeg;
    size_t jpeg_size;
    bool ok;

    for (y = 0; y < SEED_H; y++) {
        for (x = 0; x < SEED_W; x++) {
            for (c = 0; c < 3u; c++) {
                g_raw[(y * SEED_W + x) * 3u + c] = seed_value(x, y, c);
            }
        }
    }

    if (hdrg_test_jpeg_rgb(alloc, g_raw, SEED_W, SEED_H, SEED_W * 3u, 85, icc, icc_size, exif,
                           exif_size, &jpeg, &jpeg_size) != HDRG_OK) {
        return false;
    }

    ok = seed_write(dir, name, jpeg, jpeg_size);

    if (keep != NULL) {
        *keep = jpeg;
        *keep_size = jpeg_size;
    } else {
        hdrg_mem_free(alloc, jpeg);
    }

    return ok;
}

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
} seed_jpeg_error_t;

static void seed_jpeg_error_exit(j_common_ptr cinfo)
{
    longjmp(((seed_jpeg_error_t *)(void *)cinfo->err)->jump, 1);
}

/* a JPEG of the seed image with the luma sampled h x v (the chroma subsampled), maybe progressive
 */
static bool subsampled_seed(const char *dir, const char *name, int h, int v, bool progressive)
{
    struct jpeg_compress_struct cinfo;
    seed_jpeg_error_t error;
    JSAMPROW row;
    unsigned long size;
    uint32_t x, y;
    uint8_t c, *out;
    bool ok;

    for (y = 0; y < SEED_H; y++) {
        for (x = 0; x < SEED_W; x++) {
            for (c = 0; c < 3u; c++) {
                g_raw[(y * SEED_W + x) * 3u + c] = seed_value(x, y, c);
            }
        }
    }

    out = NULL;
    size = 0;
    cinfo.err = jpegli_std_error(&error.mgr);
    error.mgr.error_exit = seed_jpeg_error_exit;

    /* out is not read after a longjmp (C11 7.13.2.1): a failed seed leaks it */
    if (setjmp(error.jump)) {
        jpegli_destroy_compress(&cinfo);

        return false;
    }

    jpegli_create_compress(&cinfo);
    jpegli_mem_dest(&cinfo, &out, &size);
    cinfo.image_width = SEED_W;
    cinfo.image_height = SEED_H;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpegli_set_defaults(&cinfo);
    jpegli_set_quality(&cinfo, 75, TRUE);
    cinfo.comp_info[0].h_samp_factor = h;
    cinfo.comp_info[0].v_samp_factor = v;

    if (progressive) {
        jpegli_simple_progression(&cinfo);
    }

    jpegli_start_compress(&cinfo, TRUE);

    for (y = 0; y < SEED_H; y++) {
        row = g_raw + (size_t)y * SEED_W * 3u;
        (void)jpegli_write_scanlines(&cinfo, &row, 1);
    }

    jpegli_finish_compress(&cinfo);
    jpegli_destroy_compress(&cinfo);
    ok = seed_write(dir, name, out, size);

    free(out);

    return ok;
}

/* the library's own output for the input in one output mode (JPEG): HDR files as inputs */
static bool hdr_seed(hdrg_context_t *ctx, const char *dir, const char *name, const uint8_t *input,
                     size_t input_size, uint8_t mode)
{
    hdrg_encode_params_t encode;
    uint8_t *output;
    size_t output_size;
    bool ok;

    hdrg_encode_params_init(&encode);
    encode.output_mode = mode;

    if (hdrg_convert(ctx, input, input_size, NULL, NULL, &encode, &output, &output_size) !=
        HDRG_OK) {
        return false;
    }

    ok = seed_write(dir, name, output, output_size);

    hdrg_free(output);

    return ok;
}

static void *seed_malloc(size_t size, void *user)
{
    (void)user;

    return malloc(size);
}

static void *seed_realloc(void *ptr, size_t size, void *user)
{
    (void)user;

    return realloc(ptr, size);
}

static void seed_free(void *ptr, void *user)
{
    (void)user;
    free(ptr);
}

int main(int argc, char **argv)
{
    const hdrg_allocator_t alloc = {seed_malloc, NULL, seed_realloc, seed_free, NULL};
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    uint8_t *icc, *pq_icc, *base;
    size_t icc_size, pq_icc_size, base_size;
    bool ok;

    if (argc != 2) {
        fprintf(stderr, "usage: %s DIR\n", argv[0]);

        return 2;
    }

    icc = NULL;
    pq_icc = NULL;
    icc_size = 0;
    pq_icc_size = 0;
    base = NULL;
    base_size = 0;

#ifdef HDRG_WITH_LCMS2
    if (hdrg_icc_create_p3(&alloc, &icc, &icc_size) != HDRG_OK ||
        hdrg_icc_create_p3_pq(&alloc, 0.0f, NULL, 0, &pq_icc, &pq_icc_size) != HDRG_OK) {
        return 1;
    }
#endif

    hdrg_context_desc_init(&desc);
    desc.thread_count = 1;

    if (hdrg_context_create(&desc, &ctx) != HDRG_OK) {
        return 1;
    }

    ok = png_seeds(argv[1], icc, icc_size);
    ok = gif_seed(argv[1], "single.gif", false) && ok;
    ok = gif_seed(argv[1], "animated.gif", true) && ok;
    ok = jpeg_seed(&alloc, argv[1], "srgb.jpg", NULL, 0, NULL, 0, &base, &base_size) && ok;
    ok = subsampled_seed(argv[1], "yuv420.jpg", 2, 2, false) && ok;
    ok = subsampled_seed(argv[1], "yuv422_progressive.jpg", 2, 1, true) && ok;
    ok = jpeg_seed(&alloc, argv[1], "exif_orientation6.jpg", NULL, 0, g_exif_orientation6,
                   sizeof(g_exif_orientation6), NULL, NULL) &&
         ok;
    ok = jpeg_seed(&alloc, argv[1], "exif_r03.jpg", NULL, 0, g_exif_r03, sizeof(g_exif_r03), NULL,
                   NULL) &&
         ok;

    if (icc != NULL) {
        ok = jpeg_seed(&alloc, argv[1], "icc_p3.jpg", icc, icc_size, NULL, 0, NULL, NULL) && ok;
        ok =
            jpeg_seed(&alloc, argv[1], "icc_p3_pq.jpg", pq_icc, pq_icc_size, NULL, 0, NULL, NULL) &&
            ok;
    }

    if (base != NULL) {
        ok = hdr_seed(ctx, argv[1], "hdr_gainmap.jpg", base, base_size, HDRG_OUTPUT_GAINMAP) && ok;
#ifdef HDRG_WITH_LCMS2
        ok = hdr_seed(ctx, argv[1], "hdr_hybrid.jpg", base, base_size, HDRG_OUTPUT_HYBRID) && ok;
        ok = hdr_seed(ctx, argv[1], "hdr_pq.jpg", base, base_size, HDRG_OUTPUT_PQ) && ok;
#endif
    }

    hdrg_mem_free(&alloc, base);
    hdrg_mem_free(&alloc, icc);
    hdrg_mem_free(&alloc, pq_icc);
    hdrg_context_destroy(ctx);

    return ok ? 0 : 1;
}
