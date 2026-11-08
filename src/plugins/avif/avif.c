/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * The AVIF codec module (org.hdrgrading.avif): a plugin of the codec ABI over the system's
 * libavif and its AV1 codecs, never linked into libhdrgrading. Reads an AVIF's base image (a
 * gain map is left), writes the three forms as 10-bit AV1: full-range Y'CbCr (the library's
 * conversion), the colors signaled by CICP alone, the gain maps as ISO 21496-1 ('tmap'), the PQ
 * bases' light as 'clli'.
 */
#include <string.h>

#include <avif/avif.h>

#include "hdrgrading/filter.h"

#define HDRG_AVIF_LIT(s)   s, sizeof(s) - 1u
#define HDRG_AVIF_ID       "org.hdrgrading.avif"
#define HDRG_AVIF_LICENSE  "Apache-2.0 OR MIT"
#define HDRG_AVIF_COPY     "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_AVIF_MAX_SIDE 65535u
/* H.273 transfer characteristics: SMPTE ST 2084 (PQ) */
#define HDRG_AVIF_TRANSFER_PQ 16u

/* the writer's instance: the image its rows go into */
typedef struct {
    avifImage *image;
    uint32_t desc[HDRG_YUVD_WORDS];
    uint32_t threads; /* the encoder's: the threads option's, else the conversion's */
    uint8_t quality, gainmap_quality, speed;
} hdrg_avif_writer_t;

/* EXIF orientation of irot (angle 0..3, anti-clockwise) and imir (none, axis 0, axis 1) */
static const uint8_t hdrg_avif_orientations[4][3] = {
    {1, 4, 2},
    {8, 5, 7},
    {3, 2, 4},
    {6, 7, 5},
};

/*
 * The matrices the library converts (with the chroma from luma of the JPEG's): BT.709, BT.601
 * (the unspecified one too, libavif's default) and BT.2020's non-constant luminance, subsampled
 * or not. The others (identity, YCgCo, chromaticity-derived) and monochrome go through libavif's
 */
static bool hdrg_avif_matrix(const avifImage *image, uint32_t *out)
{
    switch (image->matrixCoefficients) {
    case AVIF_MATRIX_COEFFICIENTS_BT709:
        *out = HDRG_MATRIX_BT709;

        break;
    case AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED:
    case AVIF_MATRIX_COEFFICIENTS_BT470BG:
    case AVIF_MATRIX_COEFFICIENTS_BT601:
        *out = HDRG_MATRIX_BT601;

        break;
    case AVIF_MATRIX_COEFFICIENTS_BT2020_NCL:
        *out = HDRG_MATRIX_BT2020;

        break;
    default:
        return false;
    }

    return image->yuvFormat == AVIF_PIXEL_FORMAT_YUV420 ||
           image->yuvFormat == AVIF_PIXEL_FORMAT_YUV422 ||
           image->yuvFormat == AVIF_PIXEL_FORMAT_YUV444;
}

/* an ICC profile, else the CICP of an SDR transfer (unspecified: sRGB); PQ / HLG refused */
static hdrg_status_t hdrg_avif_color(hdrg_reader_t *reader, const avifImage *image,
                                     const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;
    uint32_t primaries, transfer;

    if (image->icc.size > 0) {
        return api->read_set_icc(reader, image->icc.data, image->icc.size);
    }

    primaries = (image->colorPrimaries == AVIF_COLOR_PRIMARIES_UNSPECIFIED)
                    ? HDRG_PRIMARIES_BT709
                    : (uint32_t)image->colorPrimaries;
    transfer = (image->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED)
                   ? HDRG_TRANSFER_SRGB
                   : (uint32_t)image->transferCharacteristics;

    if (primaries >= HDRG_PRIMARIES_ADOBE_RGB || transfer >= HDRG_TRANSFER_ADOBE_RGB) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    status = api->read_set_cicp(reader, primaries, transfer);

    return (status == HDRG_ERR_UNSUPPORTED) ? HDRG_ERR_ICC_UNSUPPORTED : status;
}

/* the picture as 16-bit R'G'B' (rows of stride bytes) */
static hdrg_status_t hdrg_avif_pixels(const avifImage *image, uint32_t fit, uint8_t *pixels,
                                      size_t stride, const hdrg_plugin_api_t *api)
{
    const void *planes[3];
    avifRGBImage rgb;
    uint32_t desc[HDRG_YUVD_WORDS];
    uint8_t c;
    size_t strides[3];

    if (!hdrg_avif_matrix(image, &desc[HDRG_YUVD_MATRIX])) {
        avifRGBImageSetDefaults(&rgb, image);
        rgb.format = AVIF_RGB_FORMAT_RGB;
        rgb.depth = 16;
        rgb.pixels = pixels;
        rgb.rowBytes = (uint32_t)stride;

        return (avifImageYUVToRGB(image, &rgb) == AVIF_RESULT_OK) ? HDRG_OK : HDRG_ERR_DECODE;
    }

    for (c = 0; c < 3u; c++) {
        planes[c] = image->yuvPlanes[c];
        strides[c] = image->yuvRowBytes[c];
    }

    desc[HDRG_YUVD_WIDTH] = image->width;
    desc[HDRG_YUVD_HEIGHT] = image->height;
    desc[HDRG_YUVD_LAYOUT] =
        (image->yuvFormat == AVIF_PIXEL_FORMAT_YUV420)
            ? HDRG_YUV_420
            : ((image->yuvFormat == AVIF_PIXEL_FORMAT_YUV422) ? HDRG_YUV_422 : HDRG_YUV_444);
    desc[HDRG_YUVD_BITS] = image->depth;
    desc[HDRG_YUVD_FLAGS] = (image->yuvRange == AVIF_RANGE_FULL) ? HDRG_YUVF_FULL_RANGE : 0u;

    /* AV1's chroma sample position; unknown: between the luma samples (libavif's upsampling) */
    if (image->yuvChromaSamplePosition == AVIF_CHROMA_SAMPLE_POSITION_VERTICAL) {
        desc[HDRG_YUVD_SITING] = HDRG_CHROMA_LEFT;
    } else if (image->yuvChromaSamplePosition == AVIF_CHROMA_SAMPLE_POSITION_COLOCATED) {
        desc[HDRG_YUVD_SITING] = HDRG_CHROMA_TOPLEFT;
    } else {
        desc[HDRG_YUVD_SITING] = HDRG_CHROMA_CENTER;
    }

    return (api->yuv_to_rgb16(api, desc, planes, strides, fit, (uint16_t *)(void *)pixels,
                              stride) == HDRG_OK)
               ? HDRG_OK
               : HDRG_ERR_DECODE;
}

/* the Exif payload from its TIFF header */
static hdrg_status_t hdrg_avif_exif(hdrg_reader_t *reader, const avifImage *image,
                                    const hdrg_plugin_api_t *api)
{
    size_t offset;

    if (image->exif.size == 0 ||
        avifGetExifTiffHeaderOffset(image->exif.data, image->exif.size, &offset) !=
            AVIF_RESULT_OK ||
        offset >= image->exif.size) {
        return HDRG_OK;
    }

    return api->read_set_exif(reader, image->exif.data + offset, image->exif.size - offset);
}

/* the clean aperture of a valid clap box (else the whole picture) */
static void hdrg_avif_crop(const avifImage *image, avifCropRect *out)
{
    avifDiagnostics diag;

    if ((image->transformFlags & AVIF_TRANSFORM_CLAP) == 0 ||
        !avifCropRectFromCleanApertureBox(out, &image->clap, image->width, image->height, &diag)) {
        out->x = 0;
        out->y = 0;
        out->width = image->width;
        out->height = image->height;
    }
}

static uint32_t HDRG_PLUGIN_CALL hdrg_avif_probe(const uint8_t *data, size_t size,
                                                 const hdrg_plugin_api_t *api)
{
    uint32_t box;
    size_t at, end;

    (void)api;

    if (size < 16 || memcmp(data + 4, "ftyp", 4) != 0) {
        return 0;
    }

    /* the major brand and the compatible ones of the leading ftyp box */
    box = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) |
          (uint32_t)data[3];
    end = (box < size) ? box : size;

    for (at = 8; at + 4 <= end; at += (at == 8) ? 8u : 4u) {
        if (memcmp(data + at, "avif", 4) == 0 || memcmp(data + at, "avis", 4) == 0) {
            return 1;
        }
    }

    return 0;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_avif_read(hdrg_reader_t *reader, const uint8_t *data,
                                                     size_t size, const hdrg_plugin_api_t *api)
{
    const avifImage *image;
    avifDecoder *decoder;
    avifCropRect rect;
    hdrg_status_t status;
    uint64_t max_pixels;
    uint32_t fit, y;
    uint8_t mirror, *full, *pixels;
    size_t full_stride, stride;

    (void)api->read_limits(reader, &max_pixels, &fit);

    decoder = avifDecoderCreate();
    if (decoder == NULL) {
        return HDRG_ERR_NOMEM;
    }

    decoder->maxThreads = (int)api->run_workers(api);
    decoder->strictFlags = AVIF_STRICT_DISABLED;
    decoder->ignoreXMP = AVIF_TRUE;
    full = NULL;
    status = HDRG_ERR_DECODE;

    if (avifDecoderSetIOMemory(decoder, data, size) != AVIF_RESULT_OK ||
        avifDecoderParse(decoder) != AVIF_RESULT_OK) {
        goto cleanup;
    }

    image = decoder->image;

    if ((uint64_t)image->width * image->height > max_pixels) {
        status = HDRG_ERR_LIMIT;

        goto cleanup;
    }

    /*
     * the codec's frames (the alpha, the tiles) held to the host's limit too, not libavif's: the
     * codecs made again with it
     */
    if (max_pixels > 0 && max_pixels < (uint64_t)decoder->imageSizeLimit) {
        decoder->imageSizeLimit = (uint32_t)max_pixels;

        if (avifDecoderReset(decoder) != AVIF_RESULT_OK) {
            goto cleanup;
        }

        image = decoder->image;
    }

    if (avifDecoderNextImage(decoder) != AVIF_RESULT_OK) {
        goto cleanup;
    }

    /* (the decoded picture's size, which the item's may not have told) */
    if ((uint64_t)image->width * image->height > max_pixels) {
        status = HDRG_ERR_LIMIT;

        goto cleanup;
    }

    status = hdrg_avif_color(reader, image, api);
    if (status == HDRG_OK) {
        status = hdrg_avif_exif(reader, image, api);
    }

    if (status != HDRG_OK) {
        goto cleanup;
    }

    hdrg_avif_crop(image, &rect);

    status = api->read_image(reader, rect.width, rect.height, 16, &pixels, &stride);
    if (status != HDRG_OK) {
        goto cleanup;
    }

    /* the whole picture straight into the host's, or (a clean aperture) its part of it */
    if (rect.x == 0 && rect.y == 0 && rect.width == image->width && rect.height == image->height) {
        status = hdrg_avif_pixels(image, fit, pixels, stride, api);
    } else {
        full_stride = (size_t)image->width * 6u;
        full = (uint8_t *)api->mem_alloc(api, full_stride * image->height);
        status =
            (full != NULL) ? hdrg_avif_pixels(image, fit, full, full_stride, api) : HDRG_ERR_NOMEM;

        for (y = 0; status == HDRG_OK && y < rect.height; y++) {
            memcpy(pixels + (size_t)y * stride,
                   full + (size_t)(rect.y + y) * full_stride + (size_t)rect.x * 6u,
                   (size_t)rect.width * 6u);
        }
    }

    if (status != HDRG_OK) {
        goto cleanup;
    }

    mirror =
        ((image->transformFlags & AVIF_TRANSFORM_IMIR) != 0) ? 1u + (image->imir.axis & 1u) : 0u;
    status = api->read_set_orientation(
        reader, hdrg_avif_orientations[((image->transformFlags & AVIF_TRANSFORM_IROT) != 0)
                                           ? image->irot.angle & 3u
                                           : 0u][mirror]);

cleanup:
    api->mem_free(api, full);
    avifDecoderDestroy(decoder);

    return status;
}

static bool hdrg_avif_signed(double v, avifSignedFraction out[3])
{
    uint8_t c;

    for (c = 0; c < 3u; c++) {
        if (!avifDoubleToSignedFraction(v, &out[c])) {
            return false;
        }
    }

    return true;
}

/* the 8-bit monochrome gain map with its metadata, the image's */
static hdrg_status_t hdrg_avif_gainmap(avifImage *image, hdrg_writer_t *writer,
                                       const hdrg_plugin_api_t *api)
{
    const uint8_t *pixels;
    avifGainMap *map;
    hdrg_status_t status;
    uint32_t width, height, primaries, transfer, bits, y;
    uint8_t c;
    size_t stride;
    double values[HDRG_GAINMAP_VALUES];

    status = api->write_gainmap(writer, &pixels, &stride, &width, &height, values);
    if (status == HDRG_ERR_NOT_FOUND) {
        return HDRG_OK;
    }

    if (status == HDRG_OK) {
        status = api->write_alternate(writer, &primaries, &transfer, &bits);
    }

    if (status != HDRG_OK) {
        return status;
    }

    map = avifGainMapCreate();
    if (map == NULL) {
        return HDRG_ERR_NOMEM;
    }

    image->gainMap = map;
    map->image = avifImageCreate(width, height, 8, AVIF_PIXEL_FORMAT_YUV400);
    if (map->image == NULL) {
        return HDRG_ERR_NOMEM;
    }

    map->image->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    map->image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    map->image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
    map->image->yuvRange = AVIF_RANGE_FULL;

    if (avifImageAllocatePlanes(map->image, AVIF_PLANES_YUV) != AVIF_RESULT_OK) {
        return HDRG_ERR_NOMEM;
    }

    for (y = 0; y < height; y++) {
        memcpy(map->image->yuvPlanes[AVIF_CHAN_Y] +
                   (size_t)y * map->image->yuvRowBytes[AVIF_CHAN_Y],
               pixels + (size_t)y * stride, width);
    }

    for (c = 0; c < 3u; c++) {
        if (!avifDoubleToUnsignedFraction(values[HDRG_GAINMAP_GAMMA], &map->gainMapGamma[c])) {
            return HDRG_ERR_ENCODE;
        }
    }

    map->useBaseColorSpace = AVIF_TRUE;
    map->altColorPrimaries = (avifColorPrimaries)primaries;
    map->altTransferCharacteristics = (avifTransferCharacteristics)transfer;
    map->altMatrixCoefficients = AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED;
    map->altYUVRange = AVIF_RANGE_FULL;
    map->altDepth = bits;
    map->altPlaneCount = 3;

    return (hdrg_avif_signed(values[HDRG_GAINMAP_MIN], map->gainMapMin) &&
            hdrg_avif_signed(values[HDRG_GAINMAP_MAX], map->gainMapMax) &&
            hdrg_avif_signed(values[HDRG_GAINMAP_BASE_OFFSET], map->baseOffset) &&
            hdrg_avif_signed(values[HDRG_GAINMAP_ALT_OFFSET], map->alternateOffset) &&
            avifDoubleToUnsignedFraction(values[HDRG_GAINMAP_BASE_HEADROOM],
                                         &map->baseHdrHeadroom) &&
            avifDoubleToUnsignedFraction(values[HDRG_GAINMAP_ALT_HEADROOM],
                                         &map->alternateHdrHeadroom))
               ? HDRG_OK
               : HDRG_ERR_ENCODE;
}

/* an option of the writer's (declared, so present) */
static int64_t hdrg_avif_option(const hdrg_plugin_api_t *api, const hdrg_options_t *options,
                                const char *key)
{
    int64_t value;

    value = 0;
    (void)api->option_get_int(options, key, strlen(key), &value);

    return value;
}

static void HDRG_PLUGIN_CALL hdrg_avif_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_avif_writer_t *state = (hdrg_avif_writer_t *)inst;

    if (state == NULL) {
        return;
    }

    avifImageDestroy(state->image);
    api->mem_free(api, state);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_avif_create(hdrg_writer_t *writer,
                                                       const hdrg_options_t *options, void **inst,
                                                       const hdrg_plugin_api_t *api)
{
    const uint8_t *tiff;
    hdrg_avif_writer_t *state;
    avifImage *image;
    uint32_t width, height, primaries, transfer, threads;
    size_t tiff_size;
    bool subsampled;
    char chroma[8];

    *inst = NULL;

    if (api->option_get_string(options, HDRG_AVIF_LIT("chroma"), chroma, sizeof(chroma),
                               &tiff_size) != HDRG_OK ||
        (strcmp(chroma, "444") != 0 && strcmp(chroma, "420") != 0)) {
        return HDRG_ERR_INVALID_ARG;
    }

    subsampled = strcmp(chroma, "420") == 0;

    state = (hdrg_avif_writer_t *)api->mem_calloc(api, 1, sizeof(*state));
    if (state == NULL) {
        return HDRG_ERR_NOMEM;
    }

    state->quality = (uint8_t)hdrg_avif_option(api, options, "quality");
    state->gainmap_quality = (uint8_t)hdrg_avif_option(api, options, "gainmap_quality");
    state->speed = (uint8_t)hdrg_avif_option(api, options, "speed");
    (void)api->write_image(writer, &width, &height, NULL, &primaries, &transfer, &state->threads);
    threads = (uint32_t)hdrg_avif_option(api, options, "threads");
    state->threads = (threads != 0u) ? threads : state->threads;

    image = avifImageCreate(width, height, 10,
                            subsampled ? AVIF_PIXEL_FORMAT_YUV420 : AVIF_PIXEL_FORMAT_YUV444);
    state->image = image;
    if (image == NULL) {
        hdrg_avif_destroy(state, api);

        return HDRG_ERR_NOMEM;
    }

    image->colorPrimaries = (avifColorPrimaries)primaries;
    image->transferCharacteristics = (avifTransferCharacteristics)transfer;
    image->matrixCoefficients = (primaries == HDRG_PRIMARIES_BT2020)
                                    ? AVIF_MATRIX_COEFFICIENTS_BT2020_NCL
                                    : AVIF_MATRIX_COEFFICIENTS_BT601;
    image->yuvRange = AVIF_RANGE_FULL;
    /* rgb16_to_yuv's chroma: on the even columns, between the rows (H.273 0) */
    image->yuvChromaSamplePosition =
        subsampled ? AVIF_CHROMA_SAMPLE_POSITION_VERTICAL : AVIF_CHROMA_SAMPLE_POSITION_UNKNOWN;

    if (api->write_exif(writer, &tiff, &tiff_size) != HDRG_OK) {
        tiff_size = 0;
    }

    if (avifImageAllocatePlanes(image, AVIF_PLANES_YUV) != AVIF_RESULT_OK ||
        (tiff_size > 0 && avifImageSetMetadataExif(image, tiff, tiff_size) != AVIF_RESULT_OK)) {
        hdrg_avif_destroy(state, api);

        return HDRG_ERR_NOMEM;
    }

    /* the rows are upright (the source's orientation is applied): no irot / imir of the Exif's */
    image->transformFlags = AVIF_TRANSFORM_NONE;
    state->desc[HDRG_YUVD_WIDTH] = width;
    state->desc[HDRG_YUVD_HEIGHT] = height;
    state->desc[HDRG_YUVD_LAYOUT] = subsampled ? HDRG_YUV_420 : HDRG_YUV_444;
    state->desc[HDRG_YUVD_MATRIX] =
        (primaries == HDRG_PRIMARIES_BT2020) ? HDRG_MATRIX_BT2020 : HDRG_MATRIX_BT601;
    state->desc[HDRG_YUVD_BITS] = 10;
    state->desc[HDRG_YUVD_SITING] = HDRG_CHROMA_LEFT;
    state->desc[HDRG_YUVD_FLAGS] = HDRG_YUVF_FULL_RANGE;
    *inst = state;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_avif_rows(void *inst, uint32_t y0, uint32_t rows,
                                                     const uint8_t *data, size_t stride,
                                                     const hdrg_plugin_api_t *api)
{
    const hdrg_avif_writer_t *state = (const hdrg_avif_writer_t *)inst;
    uint8_t c;
    size_t strides[3];
    void *planes[3];

    (void)stride;

    for (c = 0; c < 3u; c++) {
        planes[c] = state->image->yuvPlanes[c];
        strides[c] = state->image->yuvRowBytes[c];
    }

    return api->rgb16_to_yuv(api, state->desc, planes, strides,
                             (const uint16_t *)(const void *)data, y0, rows);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_avif_finish(void *inst, hdrg_writer_t *writer,
                                                       const hdrg_plugin_api_t *api)
{
    const hdrg_avif_writer_t *state = (const hdrg_avif_writer_t *)inst;
    avifEncoder *encoder;
    avifRWData data = AVIF_DATA_EMPTY;
    hdrg_status_t status;
    uint32_t max_cll, max_fall;

    status = api->write_light(writer, &max_cll, &max_fall, NULL);
    if (status == HDRG_OK) {
        state->image->clli.maxCLL = (uint16_t)((max_cll < 65535u) ? max_cll : 65535u);
        state->image->clli.maxPALL = (uint16_t)((max_fall < 65535u) ? max_fall : 65535u);

        status = hdrg_avif_gainmap(state->image, writer, api);
    }

    if (status != HDRG_OK) {
        return status;
    }

    encoder = avifEncoderCreate();
    if (encoder == NULL) {
        return HDRG_ERR_NOMEM;
    }

    /*
     * libaom where libavif has it (its lossless quantizer), on the writer's threads: the
     * system's encoder decides its bytes, which change with the threads in some versions
     * (libaom 3.12) and with its version, outside the library's determinism (threads 1 for a
     * reproducible run, as the tests); the tiles by the image's size alone (for the decoders'
     * threads)
     */
    if (avifCodecName(AVIF_CODEC_CHOICE_AOM, AVIF_CODEC_FLAG_CAN_ENCODE) != NULL) {
        encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
    }

    encoder->autoTiling = AVIF_TRUE;
    encoder->maxThreads = (int)((state->threads > 0u) ? state->threads : 1u);
    encoder->quality = state->quality;
    encoder->qualityAlpha = state->quality;
    encoder->qualityGainMap = state->gainmap_quality;
    encoder->speed = state->speed;

    status = (avifEncoderWrite(encoder, state->image, &data) == AVIF_RESULT_OK) ? HDRG_OK
                                                                                : HDRG_ERR_ENCODE;
    avifEncoderDestroy(encoder);

    if (status == HDRG_OK) {
        status = api->write_output(writer, data.data, data.size);
    }

    avifRWDataFree(&data);

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_avif_init(hdrg_plugin_t *plugin,
                                                     const hdrg_plugin_api_t *api)
{
    const uint32_t forms = HDRG_CODEC_FORM(HDRG_OUTPUT_GAINMAP) |
                           HDRG_CODEC_FORM(HDRG_OUTPUT_HYBRID) | HDRG_CODEC_FORM(HDRG_OUTPUT_PQ);
    hdrg_class_t *cls;
    hdrg_status_t status;
    bool encodes;

    status = api->plugin_set_info(plugin, HDRG_AVIF_LIT(HDRG_AVIF_ID), HDRG_AVIF_LIT("AVIF"),
                                  HDRG_AVIF_LIT("1.0.0"), HDRG_AVIF_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_AVIF_LIT(HDRG_AVIF_LICENSE),
                                         HDRG_AVIF_LIT(HDRG_AVIF_COPY));
    }

    /* the system's libavif may have no AV1 encoder: then it reads alone */
    encodes = avifCodecName(AVIF_CODEC_CHOICE_AUTO, AVIF_CODEC_FLAG_CAN_ENCODE) != NULL;

    if (status == HDRG_OK) {
        status =
            encodes
                ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                : api->plugin_set_status(plugin, HDRG_PLUGIN_DEGRADED, HDRG_REASON_MISSING_RESOURCE,
                                         HDRG_AVIF_LIT("libavif has no AV1 encoder"));
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_AVIF_LIT(HDRG_AVIF_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_extensions(cls, HDRG_AVIF_LIT("avif"));
    if (status == HDRG_OK) {
        status = api->class_set_reader(cls, hdrg_avif_probe, hdrg_avif_read);
    }

    if (status == HDRG_OK && encodes) {
        status = api->class_set_writer(cls, forms, HDRG_OUTPUT_GAINMAP, HDRG_CODEC_ROWS_RGB16,
                                       HDRG_CODEC_COLOR_CICP, HDRG_AVIF_MAX_SIDE, hdrg_avif_create,
                                       hdrg_avif_rows, hdrg_avif_finish, hdrg_avif_destroy);
    }

    if (status == HDRG_OK && encodes) {
        status = api->class_add_option_int(
            cls, HDRG_AVIF_LIT("quality"), 90, 0, 100,
            HDRG_AVIF_LIT("libavif's quality of the base (100: the lossless quantizer)"));
    }

    if (status == HDRG_OK && encodes) {
        status = api->class_add_option_int(cls, HDRG_AVIF_LIT("gainmap_quality"), 90, 0, 100,
                                           HDRG_AVIF_LIT("... of the gain map"));
    }

    if (status == HDRG_OK && encodes) {
        status = api->class_add_option_int(cls, HDRG_AVIF_LIT("speed"), 6, 0, 10,
                                           HDRG_AVIF_LIT("the encoder's speed (0: slowest)"));
    }

    if (status == HDRG_OK && encodes) {
        status = api->class_add_option_int(
            cls, HDRG_AVIF_LIT("threads"), 0, 0, 64,
            HDRG_AVIF_LIT("the encoder's threads (0: the conversion's; the bytes may change with "
                          "them and libaom's version, 1 reproduces)"));
    }

    if (status == HDRG_OK && encodes) {
        status = api->class_add_option_string(
            cls, HDRG_AVIF_LIT("chroma"), HDRG_AVIF_LIT("444"),
            HDRG_AVIF_LIT("444 (AV1 High profile) or 420 (Main profile)"));
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
