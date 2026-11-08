/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * hdrg-inspect — verifies the HDR signaling of a JPEG or an AVIF. Detects a
 * JPEG's output forms: the two-layer gain map (MPF + ISO 21496-1 [+
 * Apple/hdrgm XMP]) and the single-layer PQ (ICC cicp), and dumps the ICC
 * HAGC tag (SMPTE ST 2094-50 gain curves); an AVIF's CICP, CLLI and ISO
 * 21496-1 gain map (libavif); a HEIF's brands, nclx, CLLI, tmap and Apple
 * gain map (its boxes); a JPEG XL's color, intensity target and jhgm gain map
 * (the bundled libjxl). Exit 0 = some HDR signal present.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HDRG_WITH_AVIF
#include <avif/avif.h>
#endif
#if HDRG_BUILTIN_JXL
#include <jxl/decode.h>
#include <jxl/gain_map.h>
#endif

#include "hagc.h"
#include "hdrgrading/hdrgrading.h"

#define ICC_MAX_CHUNKS 255

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ICC 'HAGC' (type 'hagc'): SMPTE ST 2094-50 headroom adaptive gain curves */
static void print_hagc(const uint8_t *tag, uint32_t tag_size)
{
    static const float probes[4] = {0.5f, 1.0f, 2.0f, 4.0f};
    hdrg_hagc_t hagc;
    uint32_t payload;
    uint8_t a, i;

    payload = (tag_size >= 12) ? be32(tag + 8) : 0;
    if (tag_size < 12 || memcmp(tag, "hagc", 4) != 0 || payload > tag_size - 12 ||
        !hdrg_hagc_parse(tag + 12, payload, &hagc)) {
        printf("  HAGC: malformed (%u bytes)\n", tag_size);

        return;
    }

    printf("  HAGC: reference_white=%.1f baseline=%.4f stops%s alternates=%u chromaticities=%u\n",
           (double)hagc.reference_white, (double)hagc.baseline, hagc.rwtmo ? " (RWTMO)" : "",
           hagc.alt_count, hagc.chromaticities);

    for (a = 0; a < hagc.alt_count; a++) {
        printf("    alt %u: headroom=%.4f mix=%u points=%u%s gain@x", a,
               (double)hagc.alt[a].headroom, hagc.alt[a].mix_type, hagc.alt[a].count,
               hagc.alt[a].pchip ? " pchip" : "");

        for (i = 0; i < 4; i++) {
            printf(" %.1f:%+.3f", (double)probes[i],
                   (double)hdrg_hagc_eval(&hagc.alt[a], probes[i]));
        }

        printf("\n");
    }
}

static void print_icc_summary(const uint8_t *icc, size_t size, int *found_pq)
{
    const uint8_t *entry, *tag;
    uint32_t count, offset, tag_size, name_len, name_off, i, j;

    if (size < 132) {
        return;
    }

    count = be32(icc + 128);
    if ((size - 132) / 12 < count) {
        return;
    }

    for (i = 0; i < count; i++) {
        entry = icc + 132 + (size_t)i * 12;
        offset = be32(entry + 4);
        tag_size = be32(entry + 8);
        if (offset > size || size - offset < tag_size) {
            continue;
        }

        tag = icc + offset;

        if (memcmp(entry, "cicp", 4) == 0 && tag_size >= 12) {
            printf("  cicp: primaries=%u transfer=%u matrix=%u full_range=%u\n", tag[8], tag[9],
                   tag[10], tag[11]);

            if (tag[9] == 16) {
                *found_pq = 1;
            }
        } else if (memcmp(entry, "desc", 4) == 0 && tag_size >= 28 && memcmp(tag, "mluc", 4) == 0) {
            name_len = be32(tag + 20);
            name_off = be32(tag + 24);
            if (name_off <= tag_size && name_len <= tag_size - name_off) {
                printf("  desc: ");

                for (j = 1; j < name_len; j += 2) {
                    fputc(tag[name_off + j], stdout);
                }

                printf("\n");
            }
        } else if (memcmp(entry, "rTRC", 4) == 0 && tag_size >= 12) {
            printf("  rTRC: %.4s (%u bytes)\n", (const char *)tag, tag_size);
        } else if (memcmp(entry, "HAGC", 4) == 0) {
            print_hagc(tag, tag_size);
        }
    }
}

/* the first box of type in [at, end) (no descent): its start and size */
static int find_box(const uint8_t *data, size_t at, size_t end, const char *type, size_t *box,
                    size_t *size)
{
    size_t length;

    while (at + 8u <= end) {
        length = be32(data + at);

        if (length < 8u || length > end - at) {
            return 0;
        }

        if (memcmp(data + at + 4, type, 4) == 0) {
            *box = at;
            *size = length;

            return 1;
        }

        at += length;
    }

    return 0;
}

/* where needle first is in [at, end), else end */
static size_t find_bytes(const uint8_t *data, size_t at, size_t end, const char *needle)
{
    const size_t length = strlen(needle);
    size_t i;

    for (i = at; i + length <= end; i++) {
        if (memcmp(data + i, needle, length) == 0) {
            return i;
        }
    }

    return end;
}

/*
 * a HEIF's signaling from its boxes (no decoder): the brands, the base's nclx and clli, the ISO
 * 21496-1 'tmap' item and its 'altr' group, Apple's auxiliary gain map. Exit 0 for a PQ transfer
 * or a tmap item
 */
/*
 * an ISO-BMFF file by the brands of its leading ftyp box: 'a' an AVIF ('avif' / 'avis', before the
 * generic 'mif1' of HEIF), 'h' a HEIF (an HEVC brand or 'mif1'), else 0
 */
static char bmff_kind(const uint8_t *data, size_t size)
{
    static const char heif_brands[][5] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1"};
    uint32_t box;
    uint8_t b;
    size_t at;
    char kind;

    if (size < 16 || memcmp(data + 4, "ftyp", 4) != 0) {
        return 0;
    }

    box = be32(data);
    kind = 0;

    for (at = 8; at + 4 <= ((size_t)box < size ? (size_t)box : size); at += (at == 8) ? 8u : 4u) {
        if (memcmp(data + at, "avif", 4) == 0 || memcmp(data + at, "avis", 4) == 0) {
            return 'a';
        }

        for (b = 0; b < sizeof(heif_brands) / sizeof(heif_brands[0]); b++) {
            kind = (memcmp(data + at, heif_brands[b], 4) == 0) ? 'h' : kind;
        }
    }

    return kind;
}

static int inspect_heif(const char *path, const uint8_t *data, size_t size)
{
    uint16_t transfer;
    int tmap, altr, apple;
    size_t ftyp, ftyp_size, meta, meta_size, iinf, iinf_size, at, colr, clli;

    if (!find_box(data, 0, size, "ftyp", &ftyp, &ftyp_size) ||
        !find_box(data, 0, size, "meta", &meta, &meta_size)) {
        fprintf(stderr, "not a valid HEIF\n");

        return 1;
    }

    printf("%s: heif, brands", path);

    for (at = ftyp + 8u; at + 4u <= ftyp + ftyp_size; at += (at == ftyp + 8u) ? 8u : 4u) {
        printf(" %.4s", (const char *)(data + at));
    }

    printf("\n");

    iinf = meta_size;
    iinf_size = 0;
    (void)find_box(data, meta + 12u, meta + meta_size, "iinf", &iinf, &iinf_size);
    tmap = iinf_size > 0 && find_bytes(data, iinf, iinf + iinf_size, "tmap") < iinf + iinf_size;
    altr = find_bytes(data, meta, meta + meta_size, "altr") < meta + meta_size;
    apple = find_bytes(data, meta, meta + meta_size, "urn:com:apple:photo:2020:aux:hdrgainmap") <
            meta + meta_size;
    colr = find_bytes(data, meta, meta + meta_size, "nclx");
    clli = find_bytes(data, meta, meta + meta_size, "clli");
    transfer = 0;

    if (colr + 11u <= meta + meta_size) {
        transfer = (uint16_t)be16(data + colr + 6);
        printf("  base nclx %u/%u/%u, %s range\n", be16(data + colr + 4), transfer,
               be16(data + colr + 8), (data[colr + 10] & 0x80u) ? "full" : "limited");
    }

    if (clli + 8u <= meta + meta_size) {
        printf("  clli: MaxCLL %u, MaxFALL %u\n", be16(data + clli + 4), be16(data + clli + 6));
    }

    if (tmap) {
        printf("  hdr: gain map (iso21496 tmap%s%s)\n", altr ? ", altr" : "",
               apple ? " + apple aux" : "");
    } else {
        printf("  hdr (pq nclx): %s\n", (transfer == 16u) ? "yes" : "NO");
    }

    return (tmap || transfer == 16u) ? 0 : 1;
}

#ifdef HDRG_WITH_AVIF
static double fraction(int64_t n, uint32_t d)
{
    return (d == 0) ? 0.0 : (double)n / (double)d;
}

/* an AVIF's signaling: exit 0 for a PQ transfer or a gain map */
static int inspect_avif(const char *path, const uint8_t *data, size_t size)
{
    const avifImage *image;
    const avifGainMap *map;
    avifDecoder *decoder;
    int hdr;

    decoder = avifDecoderCreate();
    if (decoder == NULL) {
        return 1;
    }

    decoder->imageContentToDecode = AVIF_IMAGE_CONTENT_ALL;

    if (avifDecoderSetIOMemory(decoder, data, size) != AVIF_RESULT_OK ||
        avifDecoderParse(decoder) != AVIF_RESULT_OK) {
        fprintf(stderr, "not a valid AVIF\n");
        avifDecoderDestroy(decoder);

        return 1;
    }

    image = decoder->image;
    map = image->gainMap;
    printf("%s: %ux%u avif, %u-bit %s, %s range, cicp %d/%d/%d, icc=%zu, exif=%d\n", path,
           image->width, image->height, image->depth, avifPixelFormatToString(image->yuvFormat),
           (image->yuvRange == AVIF_RANGE_FULL) ? "full" : "limited", (int)image->colorPrimaries,
           (int)image->transferCharacteristics, (int)image->matrixCoefficients, image->icc.size,
           image->exif.size > 0);

    if (image->clli.maxCLL > 0) {
        printf("  clli: MaxCLL %u, MaxFALL %u\n", image->clli.maxCLL, image->clli.maxPALL);
    }

    if (map != NULL) {
        printf("  hdr: gain map (iso21496) %ux%u, headroom base %.3f / alternate %.3f, log2 gain "
               "[%.3f, %.3f], alternate cicp %d/%d\n",
               (map->image != NULL) ? map->image->width : 0u,
               (map->image != NULL) ? map->image->height : 0u,
               fraction(map->baseHdrHeadroom.n, map->baseHdrHeadroom.d),
               fraction(map->alternateHdrHeadroom.n, map->alternateHdrHeadroom.d),
               fraction(map->gainMapMin[0].n, map->gainMapMin[0].d),
               fraction(map->gainMapMax[0].n, map->gainMapMax[0].d), (int)map->altColorPrimaries,
               (int)map->altTransferCharacteristics);
    }

    hdr = map != NULL || image->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_PQ;

    if (map == NULL) {
        printf("  hdr (pq cicp): %s\n", hdr ? "yes" : "NO");
    }

    avifDecoderDestroy(decoder);

    return hdr ? 0 : 1;
}
#endif

#if HDRG_BUILTIN_JXL
/* a JPEG XL codestream's (or container's) header and color; false: not one */
static bool jxl_header(const uint8_t *data, size_t size, JxlBasicInfo *info,
                       JxlColorEncoding *color)
{
    JxlDecoderStatus status;
    JxlDecoder *dec;
    bool header, colored;

    dec = JxlDecoderCreate(NULL);
    if (dec == NULL ||
        JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING) !=
            JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, data, size) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);

        return false;
    }

    JxlDecoderCloseInput(dec);
    header = false;
    colored = false;

    do {
        status = JxlDecoderProcessInput(dec);

        if (status == JXL_DEC_BASIC_INFO) {
            header = JxlDecoderGetBasicInfo(dec, info) == JXL_DEC_SUCCESS;
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            colored = JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                                         color) == JXL_DEC_SUCCESS;
        }
    } while (status == JXL_DEC_BASIC_INFO || (status == JXL_DEC_COLOR_ENCODING && !colored));

    if (!colored) {
        memset(color, 0, sizeof(*color));
    }

    JxlDecoderDestroy(dec);

    return header;
}

/* a JPEG XL's signaling: exit 0 for a PQ / HLG base or a jhgm gain map */
static int inspect_jxl(const char *path, const uint8_t *data, size_t size)
{
    JxlGainMapBundle bundle;
    JxlColorEncoding color, map_color;
    JxlBasicInfo info, map_info;
    int hdr;
    size_t at, box, read;
    bool gainmap, exif;

    if (!jxl_header(data, size, &info, &color)) {
        fprintf(stderr, "not a valid JPEG XL\n");

        return 1;
    }

    gainmap = false;
    exif = false;
    memset(&bundle, 0, sizeof(bundle));

    /* the container's boxes (a naked codestream has none) */
    for (at = 0; size >= 12 && memcmp(data + 4, "JXL ", 4) == 0 && at + 8u <= size; at += box) {
        box = be32(data + at);
        box = (box == 0) ? size - at : box;
        if (box < 8u || box > size - at) {
            break;
        }

        exif = exif || memcmp(data + at + 4, "Exif", 4) == 0;

        if (memcmp(data + at + 4, "jhgm", 4) == 0) {
            gainmap = JxlGainMapReadBundle(&bundle, data + at + 8u, box - 8u, &read) == JXL_TRUE;
        }
    }

    printf("%s: %ux%u jxl, %u-bit, %s, primaries %d, transfer %d, intensity target %.0f, exif=%d\n",
           path, info.xsize, info.ysize, info.bits_per_sample,
           info.uses_original_profile ? "original profile" : "xyb", (int)color.primaries,
           (int)color.transfer_function, (double)info.intensity_target, exif);

    if (gainmap && jxl_header(bundle.gain_map, bundle.gain_map_size, &map_info, &map_color)) {
        printf("  hdr: gain map (jhgm, iso21496 %u bytes) %ux%u, alternate transfer %d\n",
               (unsigned int)bundle.gain_map_metadata_size, map_info.xsize, map_info.ysize,
               bundle.has_color_encoding ? (int)bundle.color_encoding.transfer_function : -1);
    }

    hdr = gainmap || color.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
          color.transfer_function == JXL_TRANSFER_FUNCTION_HLG;

    if (!gainmap) {
        printf("  hdr (pq / hlg): %s\n", hdr ? "yes" : "NO");
    }

    return hdr ? 0 : 1;
}
#endif

int main(int argc, char **argv)
{
    const uint8_t *chunk_ptrs[ICC_MAX_CHUNKS];
    static const char iso_urn[] = "urn:iso:std:iso:ts:21496:-1";
    FILE *fp;
    uint32_t chunk_count, soi_count, i;
    uint16_t sof_w, sof_h;
    uint8_t *data, *icc, *seg_data, marker, seq;
    long file_size;
    int found_pq, has_exif, has_iso, has_mpf2, has_apple, has_hdrgm;
    size_t size, seg_len, icc_size, pos, j, chunk_sizes[ICC_MAX_CHUNKS];

    if (argc != 2) {
        fprintf(stderr, "usage: hdrg-inspect <file.{jpg,avif,heic,jxl}>\n");

        return 2;
    }

    fp = fopen(argv[1], "rb");
    if (fp == NULL || fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "failed to open %s\n", argv[1]);

        if (fp != NULL) {
            fclose(fp);
        }

        return 1;
    }

    size = (size_t)file_size;
    data = (uint8_t *)malloc(size);
    if (data == NULL || fread(data, 1, size, fp) != size) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        fclose(fp);
        free(data);

        return 1;
    }

    fclose(fp);

    /* a JPEG XL: its container's signature box, or a naked codestream's FF 0A */
    if ((size >= 12 && memcmp(data, "\0\0\0\x0cJXL \r\n\x87\n", 12) == 0) ||
        (size >= 2 && data[0] == 0xFF && data[1] == 0x0A)) {
#if HDRG_BUILTIN_JXL
        found_pq = inspect_jxl(argv[1], data, size);
#else
        fprintf(stderr, "a JPEG XL (this build has no libjxl: jxl left out)\n");
        found_pq = 1;
#endif
        free(data);

        return found_pq;
    }

    if (bmff_kind(data, size) == 'h') {
        found_pq = inspect_heif(argv[1], data, size);
        free(data);

        return found_pq;
    }

    if (bmff_kind(data, size) == 'a') {
#ifdef HDRG_WITH_AVIF
        found_pq = inspect_avif(argv[1], data, size);
#else
        fprintf(stderr, "an AVIF (this build has no libavif: HDRG_WITH_AVIF)\n");
        found_pq = 1;
#endif
        free(data);

        return found_pq;
    }

    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        fprintf(stderr, "not a JPEG\n");
        free(data);

        return 1;
    }

    memset(chunk_ptrs, 0, sizeof(chunk_ptrs));
    memset(chunk_sizes, 0, sizeof(chunk_sizes));
    chunk_count = 0;
    found_pq = 0;
    has_exif = 0;
    sof_w = 0;
    sof_h = 0;

    pos = 2;
    while (pos + 4 <= size && data[pos] == 0xFF) {
        marker = data[pos + 1];
        if (marker == 0xDA || marker == 0xD9) {
            break;
        }

        seg_len = be16(data + pos + 2);
        if (seg_len < 2 || pos + 2 + seg_len > size) {
            break;
        }

        seg_data = data + pos + 4;

        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC && seg_len >= 7) {
            sof_h = be16(seg_data + 1);
            sof_w = be16(seg_data + 3);
        } else if (marker == 0xE1 && seg_len >= 2 + 6 && memcmp(seg_data, "Exif\0\0", 6) == 0) {
            has_exif = 1;
        } else if (marker == 0xE2 && seg_len >= 2 + 14 &&
                   memcmp(seg_data, "ICC_PROFILE\0", 12) == 0) {
            seq = seg_data[12];
            if (seq >= 1 && chunk_ptrs[seq - 1] == NULL) {
                chunk_ptrs[seq - 1] = seg_data + 14;
                chunk_sizes[seq - 1] = seg_len - 2 - 14;
            }
        }

        pos += 2 + seg_len;
    }

    /* the profile's chunks in order from the first: a missing one ends it */
    while (chunk_count < ICC_MAX_CHUNKS && chunk_ptrs[chunk_count] != NULL) {
        chunk_count++;
    }

    /* ゲインマップ検出: ISO urn / MPF / Apple / hdrgm / 2 本目の SOI */
    has_iso = 0;
    has_mpf2 = 0;
    has_apple = 0;
    has_hdrgm = 0;
    soi_count = 0;
    for (j = 0; j + 28 <= size; j++) {
        if (!has_iso && memcmp(data + j, iso_urn, 27) == 0) {
            has_iso = 1;
        }

        if (!has_mpf2 && memcmp(data + j, "MPF\0", 4) == 0) {
            has_mpf2 = 1;
        }

        if (!has_apple && memcmp(data + j, "HDRGainMap:HDRGainMapVersion", 28) == 0) {
            has_apple = 1;
        }

        if (!has_hdrgm && memcmp(data + j, "hdrgm:Version", 13) == 0) {
            has_hdrgm = 1;
        }

        if (j + 1 < size && data[j] == 0xFF && data[j + 1] == 0xD8) {
            soi_count++;
        }
    }

    if (has_iso && has_mpf2 && soi_count >= 2) {
        printf("%s: %ux%u, exif=%d\n", argv[1], sof_w, sof_h, has_exif);
        printf("  hdr: gain map (iso21496%s%s), streams=%u\n", has_apple ? " + apple" : "",
               has_hdrgm ? " + hdrgm" : "", soi_count);
        free(data);

        return 0;
    }

    printf("%s: %ux%u, exif=%d, icc_chunks=%u\n", argv[1], sof_w, sof_h, has_exif, chunk_count);

    if (chunk_count == 0) {
        printf("  ** no HDR signaling — SDR-only rendition **\n");
        free(data);

        return 1;
    }

    icc_size = 0;
    for (i = 0; i < chunk_count; i++) {
        icc_size += chunk_sizes[i];
    }

    icc = (uint8_t *)malloc(icc_size);
    if (icc == NULL) {
        free(data);

        return 1;
    }

    icc_size = 0;
    for (i = 0; i < chunk_count; i++) {
        memcpy(icc + icc_size, chunk_ptrs[i], chunk_sizes[i]);
        icc_size += chunk_sizes[i];
    }

    print_icc_summary(icc, icc_size, &found_pq);
    printf("  hdr (pq cicp): %s\n", found_pq ? "yes" : "NO");

    free(icc);
    free(data);

    return found_pq ? 0 : 1;
}
