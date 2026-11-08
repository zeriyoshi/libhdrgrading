/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include <jxl/color_encoding.h>
#include <jxl/encode.h>
#include <jxl/gain_map.h>

#include "parts.h"

#define HDRG_JXL_ID       "hdrg.jxl"
#define HDRG_JXL_LICENSE  "Apache-2.0 OR MIT"
#define HDRG_JXL_COPY     "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_JXL_MAX_SIDE 65535u /* the frame's sides (level 5 takes 2^18) */
#define HDRG_JXL_CHUNK    65536u /* an output buffer (the gain map's first, doubled) */
#define HDRG_JXL_LOSSLESS 100u   /* a quality of this: lossless */
#define HDRG_JXL_CICP_PQ  16u

/*
 * hdrg.jxl: JPEG XL (libjxl), written in the three forms (HYBRID by default: the PQ base and the
 * gain map back to the SDR rendition): the base's 16-bit R'G'B' as one frame of its CICP color
 * (an HDR base's intensity target the rendition's peak), the gain map an 8-bit grey naked
 * codestream in the jhgm box with its ISO 21496-1 metadata and the alternate's color, the Exif
 * in its box. libjxl's memory the context's allocator, its threads the context's (its bytes the
 * same whatever their count)
 */
typedef struct {
    uint16_t *pixels; /* the base's rows, R'G'B' */
    uint32_t width, height, primaries, transfer;
    uint8_t quality, gainmap_quality, effort;
} hdrg_jxl_writer_t;

/* a parallel run of libjxl's on the context's threads */
typedef struct {
    JxlParallelRunFunction func;
    uint32_t start;
    void *opaque;
} hdrg_jxl_job_t;

static void *hdrg_jxl_alloc(void *opaque, size_t size)
{
    const hdrg_plugin_api_t *api = (const hdrg_plugin_api_t *)opaque;

    return api->mem_alloc(api, size);
}

static void hdrg_jxl_free(void *opaque, void *address)
{
    const hdrg_plugin_api_t *api = (const hdrg_plugin_api_t *)opaque;

    api->mem_free(api, address);
}

static void HDRG_PLUGIN_CALL hdrg_jxl_task(void *user, uint32_t index, uint32_t worker)
{
    const hdrg_jxl_job_t *job = (const hdrg_jxl_job_t *)user;

    job->func(job->opaque, job->start + index, (size_t)worker);
}

static JxlParallelRetCode hdrg_jxl_runner(void *runner_opaque, void *jpegxl_opaque,
                                          JxlParallelRunInit init, JxlParallelRunFunction func,
                                          uint32_t start_range, uint32_t end_range)
{
    const hdrg_plugin_api_t *api = (const hdrg_plugin_api_t *)runner_opaque;
    hdrg_jxl_job_t job;
    JxlParallelRetCode code;

    code = init(jpegxl_opaque, (size_t)api->run_workers(api));
    if (code != 0) {
        return code;
    }

    if (end_range <= start_range) {
        return 0;
    }

    job.func = func;
    job.start = start_range;
    job.opaque = jpegxl_opaque;

    return (api->run_parallel(api, end_range - start_range, hdrg_jxl_task, &job) == HDRG_OK) ? 0
                                                                                             : -1;
}

/* an encoder on the context's allocator and threads; NULL: out of memory */
static JxlEncoder *hdrg_jxl_encoder(const hdrg_plugin_api_t *api)
{
    JxlMemoryManager memory;
    JxlEncoder *enc;

    memory.opaque = (void *)(uintptr_t)api;
    memory.alloc = hdrg_jxl_alloc;
    memory.free = hdrg_jxl_free;
    enc = JxlEncoderCreate(&memory);

    if (enc != NULL && JxlEncoderSetParallelRunner(enc, hdrg_jxl_runner, (void *)(uintptr_t)api) !=
                           JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        enc = NULL;
    }

    return enc;
}

/* the H.273 code points of a color as libjxl's; false: none of its own */
static bool hdrg_jxl_color(uint32_t primaries, uint32_t transfer, bool grey, JxlColorEncoding *out)
{
    /* H.273 primaries 1, 9, 12 and transfers 1, 6, 8, 13 to 16, 18 (0: none of libjxl's) */
    static const JxlPrimaries gamuts[13] = {
        0, JXL_PRIMARIES_SRGB, 0, 0, 0, 0, 0, 0, 0, JXL_PRIMARIES_2100, 0, 0, JXL_PRIMARIES_P3};
    static const JxlTransferFunction curves[19] = {0,
                                                   JXL_TRANSFER_FUNCTION_709,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   JXL_TRANSFER_FUNCTION_709,
                                                   0,
                                                   JXL_TRANSFER_FUNCTION_LINEAR,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   JXL_TRANSFER_FUNCTION_SRGB,
                                                   JXL_TRANSFER_FUNCTION_709,
                                                   JXL_TRANSFER_FUNCTION_709,
                                                   JXL_TRANSFER_FUNCTION_PQ,
                                                   0,
                                                   JXL_TRANSFER_FUNCTION_HLG};

    memset(out, 0, sizeof(*out));

    if (primaries >= 13u || transfer >= 19u || gamuts[primaries] == 0 || curves[transfer] == 0) {
        return false;
    }

    out->color_space = grey ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
    out->white_point = JXL_WHITE_POINT_D65;
    out->primaries = gamuts[primaries];
    out->transfer_function = curves[transfer];
    out->rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;

    return true;
}

/*
 * the frame of bytes of pixels in format, its settings lossless at quality 100, else the distance
 * of the quality, at effort
 */
static hdrg_status_t hdrg_jxl_frame(JxlEncoder *enc, uint8_t quality, uint8_t effort,
                                    const JxlPixelFormat *format, const void *pixels, size_t bytes)
{
    JxlEncoderFrameSettings *settings;
    JxlEncoderStatus status;

    settings = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (settings == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT, effort);
    if (status == JXL_ENC_SUCCESS) {
        status = (quality >= HDRG_JXL_LOSSLESS)
                     ? JxlEncoderSetFrameLossless(settings, JXL_TRUE)
                     : JxlEncoderSetFrameDistance(settings,
                                                  JxlEncoderDistanceFromQuality((float)quality));
    }

    if (status == JXL_ENC_SUCCESS) {
        status = JxlEncoderAddImageFrame(settings, format, pixels, bytes);
    }

    return (status == JXL_ENC_SUCCESS) ? HDRG_OK : HDRG_ERR_ENCODE;
}

/* the encoder's whole output (input closed) into *out (mem_free), *size bytes */
static hdrg_status_t hdrg_jxl_drain(JxlEncoder *enc, const hdrg_plugin_api_t *api, uint8_t **out,
                                    size_t *size)
{
    JxlEncoderStatus status;
    uint8_t *data, *grown, *next;
    size_t cap, used, avail;

    *out = NULL;
    *size = 0;
    cap = HDRG_JXL_CHUNK;
    used = 0;

    data = (uint8_t *)api->mem_alloc(api, cap);
    if (data == NULL) {
        return HDRG_ERR_NOMEM;
    }

    do {
        next = data + used;
        avail = cap - used;
        status = JxlEncoderProcessOutput(enc, &next, &avail);
        used = (size_t)(next - data);

        if (status == JXL_ENC_NEED_MORE_OUTPUT) {
            grown = (uint8_t *)api->mem_realloc(api, data, cap * 2u);
            if (grown == NULL) {
                api->mem_free(api, data);

                return HDRG_ERR_NOMEM;
            }

            data = grown;
            cap *= 2u;
        }
    } while (status == JXL_ENC_NEED_MORE_OUTPUT);

    if (status != JXL_ENC_SUCCESS) {
        api->mem_free(api, data);

        return HDRG_ERR_ENCODE;
    }

    *out = data;
    *size = used;

    return HDRG_OK;
}

/* the file's bytes out through write_output a buffer at a time (none held whole) */
static hdrg_status_t hdrg_jxl_stream(JxlEncoder *enc, hdrg_writer_t *writer,
                                     const hdrg_plugin_api_t *api)
{
    JxlEncoderStatus status;
    hdrg_status_t written;
    uint8_t *data, *next;
    size_t avail;

    data = (uint8_t *)api->mem_alloc(api, HDRG_JXL_CHUNK);
    if (data == NULL) {
        return HDRG_ERR_NOMEM;
    }

    do {
        next = data;
        avail = HDRG_JXL_CHUNK;
        status = JxlEncoderProcessOutput(enc, &next, &avail);
        written = (status == JXL_ENC_ERROR)
                      ? HDRG_ERR_ENCODE
                      : api->write_output(writer, data, (size_t)(next - data));
    } while (status == JXL_ENC_NEED_MORE_OUTPUT && written == HDRG_OK);

    api->mem_free(api, data);

    return (written == HDRG_OK && status != JXL_ENC_SUCCESS) ? HDRG_ERR_ENCODE : written;
}

/* the gain map (8-bit grey, rows of stride bytes) as a naked codestream into *out (mem_free) */
static hdrg_status_t hdrg_jxl_gainmap_stream(const hdrg_jxl_writer_t *state,
                                             const hdrg_plugin_api_t *api, const uint8_t *pixels,
                                             size_t stride, uint32_t width, uint32_t height,
                                             uint8_t **out, size_t *size)
{
    const JxlPixelFormat format = {1, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlColorEncoding color;
    JxlBasicInfo info;
    JxlEncoder *enc;
    hdrg_status_t status;
    uint32_t y;
    uint8_t *packed;

    *out = NULL;
    *size = 0;

    packed = (uint8_t *)api->mem_alloc(api, (size_t)width * height);
    if (packed == NULL) {
        return HDRG_ERR_NOMEM;
    }

    for (y = 0; y < height; y++) {
        memcpy(packed + (size_t)y * width, pixels + (size_t)y * stride, width);
    }

    enc = hdrg_jxl_encoder(api);
    status = (enc != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;

    if (status == HDRG_OK) {
        JxlEncoderInitBasicInfo(&info);
        info.xsize = width;
        info.ysize = height;
        info.bits_per_sample = 8;
        info.num_color_channels = 1;
        info.uses_original_profile = JXL_TRUE;
        JxlColorEncodingSetToSRGB(&color, JXL_TRUE);
        status = (JxlEncoderUseContainer(enc, JXL_FALSE) == JXL_ENC_SUCCESS &&
                  JxlEncoderSetBasicInfo(enc, &info) == JXL_ENC_SUCCESS &&
                  JxlEncoderSetColorEncoding(enc, &color) == JXL_ENC_SUCCESS)
                     ? HDRG_OK
                     : HDRG_ERR_ENCODE;
    }

    if (status == HDRG_OK) {
        status = hdrg_jxl_frame(enc, state->gainmap_quality, state->effort, &format, packed,
                                (size_t)width * height);
    }

    if (status == HDRG_OK) {
        JxlEncoderCloseInput(enc);
        status = hdrg_jxl_drain(enc, api, out, size);
    }

    JxlEncoderDestroy(enc);
    api->mem_free(api, packed);

    return status;
}

/*
 * GAINMAP and HYBRID: the gain map's jhgm box (its codestream, the ISO 21496-1 metadata, the
 * alternate's color); none for PQ
 */
static hdrg_status_t hdrg_jxl_gainmap_box(JxlEncoder *enc, const hdrg_jxl_writer_t *state,
                                          hdrg_writer_t *writer, const hdrg_plugin_api_t *api)
{
    static const uint8_t no_icc[1] = {0};
    const uint8_t *pixels;
    JxlGainMapBundle bundle;
    hdrg_status_t status;
    uint32_t width, height, primaries, transfer, bits;
    uint8_t *stream, *metadata, *box;
    size_t stride, stream_size, metadata_size, metadata_cap, box_size, written;
    double values[HDRG_GAINMAP_VALUES];

    status = api->write_gainmap(writer, &pixels, &stride, &width, &height, values);
    if (status == HDRG_ERR_NOT_FOUND) {
        return HDRG_OK;
    }

    if (status == HDRG_OK) {
        status = api->write_alternate(writer, &primaries, &transfer, &bits);
    }

    metadata_size = 0;

    if (status == HDRG_OK) {
        (void)api->write_iso21496(writer, NULL, 0, &metadata_size);
        status = (metadata_size > 0 && metadata_size <= UINT16_MAX) ? HDRG_OK : HDRG_ERR_ENCODE;
    }

    if (status != HDRG_OK) {
        return status;
    }

    metadata = (uint8_t *)api->mem_alloc(api, metadata_size);
    if (metadata == NULL) {
        return HDRG_ERR_NOMEM;
    }

    metadata_cap = metadata_size;
    status = api->write_iso21496(writer, metadata, metadata_cap, &metadata_size);
    if (status == HDRG_OK && metadata_size > metadata_cap) {
        status = HDRG_ERR_INTERNAL;
    }

    if (status == HDRG_OK) {
        status = hdrg_jxl_gainmap_stream(state, api, pixels, stride, width, height, &stream,
                                         &stream_size);
    }

    if (status != HDRG_OK) {
        api->mem_free(api, metadata);

        return status;
    }

    memset(&bundle, 0, sizeof(bundle));
    bundle.gain_map_metadata_size = (uint16_t)metadata_size;
    bundle.gain_map_metadata = metadata;
    bundle.has_color_encoding =
        hdrg_jxl_color(primaries, transfer, false, &bundle.color_encoding) ? JXL_TRUE : JXL_FALSE;
    bundle.gain_map_size = (uint32_t)stream_size;
    bundle.gain_map = stream;
    /* no alternate's ICC (its color the encoding's): libjxl copies its 0 bytes from a pointer */
    bundle.alt_icc = no_icc;
    bundle.alt_icc_size = 0;
    box = NULL;
    status = (stream_size <= UINT32_MAX && JxlGainMapGetBundleSize(&bundle, &box_size))
                 ? HDRG_OK
                 : HDRG_ERR_ENCODE;

    if (status == HDRG_OK) {
        box = (uint8_t *)api->mem_alloc(api, box_size);
        status = (box != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK &&
        (!JxlGainMapWriteBundle(&bundle, box, box_size, &written) ||
         JxlEncoderAddBox(enc, "jhgm", box, written, JXL_FALSE) != JXL_ENC_SUCCESS)) {
        status = HDRG_ERR_ENCODE;
    }

    api->mem_free(api, box);
    api->mem_free(api, stream);
    api->mem_free(api, metadata);

    return status;
}

/* the Exif's box: its TIFF after a zero offset */
static hdrg_status_t hdrg_jxl_exif_box(JxlEncoder *enc, hdrg_writer_t *writer,
                                       const hdrg_plugin_api_t *api)
{
    const uint8_t *tiff;
    hdrg_status_t status;
    uint8_t *box;
    size_t size;

    if (api->write_exif(writer, &tiff, &size) != HDRG_OK || size == 0) {
        return HDRG_OK;
    }

    box = (uint8_t *)api->mem_calloc(api, 1, size + 4u);
    if (box == NULL) {
        return HDRG_ERR_NOMEM;
    }

    memcpy(box + 4, tiff, size);
    status = (JxlEncoderAddBox(enc, "Exif", box, size + 4u, JXL_FALSE) == JXL_ENC_SUCCESS)
                 ? HDRG_OK
                 : HDRG_ERR_ENCODE;
    api->mem_free(api, box);

    return status;
}

/* an option of the writer's (declared, so present) */
static int64_t hdrg_jxl_option(const hdrg_plugin_api_t *api, const hdrg_options_t *options,
                               const char *key)
{
    int64_t value;

    value = 0;
    (void)api->option_get_int(options, key, strlen(key), &value);

    return value;
}

static void HDRG_PLUGIN_CALL hdrg_jxl_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_jxl_writer_t *state = (hdrg_jxl_writer_t *)inst;

    if (state == NULL) {
        return;
    }

    api->mem_free(api, state->pixels);
    api->mem_free(api, state);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_jxl_create(hdrg_writer_t *writer,
                                                      const hdrg_options_t *options, void **inst,
                                                      const hdrg_plugin_api_t *api)
{
    hdrg_jxl_writer_t *state;
    JxlColorEncoding color;
    uint32_t threads;

    *inst = NULL;

    state = (hdrg_jxl_writer_t *)api->mem_calloc(api, 1, sizeof(*state));
    if (state == NULL) {
        return HDRG_ERR_NOMEM;
    }

    state->quality = (uint8_t)hdrg_jxl_option(api, options, "quality");
    state->gainmap_quality = (uint8_t)hdrg_jxl_option(api, options, "gainmap_quality");
    state->effort = (uint8_t)hdrg_jxl_option(api, options, "effort");
    (void)api->write_image(writer, &state->width, &state->height, NULL, &state->primaries,
                           &state->transfer, &threads);

    /* a color of libjxl's own (the library's renditions are) */
    if (!hdrg_jxl_color(state->primaries, state->transfer, false, &color)) {
        hdrg_jxl_destroy(state, api);

        return HDRG_ERR_UNSUPPORTED;
    }

    state->pixels = (uint16_t *)api->mem_alloc(api, (size_t)state->width * state->height * 3u *
                                                        sizeof(uint16_t));
    if (state->pixels == NULL) {
        hdrg_jxl_destroy(state, api);

        return HDRG_ERR_NOMEM;
    }

    *inst = state;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_jxl_rows(void *inst, uint32_t y0, uint32_t rows,
                                                    const uint8_t *data, size_t stride,
                                                    const hdrg_plugin_api_t *api)
{
    const hdrg_jxl_writer_t *state = (const hdrg_jxl_writer_t *)inst;
    const size_t row = (size_t)state->width * 3u * sizeof(uint16_t);
    uint32_t y;

    (void)api;

    if (y0 > state->height || rows > state->height - y0) {
        return HDRG_ERR_INVALID_ARG;
    }

    for (y = 0; y < rows; y++) {
        memcpy((uint8_t *)state->pixels + (size_t)(y0 + y) * row, data + (size_t)y * stride, row);
    }

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_jxl_finish(void *inst, hdrg_writer_t *writer,
                                                      const hdrg_plugin_api_t *api)
{
    const JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    hdrg_jxl_writer_t *state = (hdrg_jxl_writer_t *)inst;
    JxlColorEncoding color;
    JxlBasicInfo info;
    JxlEncoder *enc;
    hdrg_status_t status;
    uint32_t max_cll, max_fall, peak;

    peak = 0;
    status = api->write_light(writer, &max_cll, &max_fall, &peak);
    if (status != HDRG_OK) {
        return status;
    }

    enc = hdrg_jxl_encoder(api);
    if (enc == NULL) {
        return HDRG_ERR_NOMEM;
    }

    /* an HDR base's intensity target the rendition's ceiling (a decoder's tone map from it) */
    JxlEncoderInitBasicInfo(&info);
    info.xsize = state->width;
    info.ysize = state->height;
    info.bits_per_sample = 16;
    info.uses_original_profile = (state->quality >= HDRG_JXL_LOSSLESS) ? JXL_TRUE : JXL_FALSE;

    if (state->transfer == HDRG_JXL_CICP_PQ && peak > 0) {
        info.intensity_target = (float)peak;
    }

    (void)hdrg_jxl_color(state->primaries, state->transfer, false, &color);
    status = (JxlEncoderUseBoxes(enc) == JXL_ENC_SUCCESS &&
              JxlEncoderSetBasicInfo(enc, &info) == JXL_ENC_SUCCESS &&
              JxlEncoderSetColorEncoding(enc, &color) == JXL_ENC_SUCCESS)
                 ? HDRG_OK
                 : HDRG_ERR_ENCODE;

    if (status == HDRG_OK) {
        status = hdrg_jxl_exif_box(enc, writer, api);
    }

    if (status == HDRG_OK) {
        status = hdrg_jxl_gainmap_box(enc, state, writer, api);
    }

    if (status == HDRG_OK) {
        JxlEncoderCloseBoxes(enc);
        status = hdrg_jxl_frame(enc, state->quality, state->effort, &format, state->pixels,
                                (size_t)state->width * state->height * 3u * sizeof(uint16_t));
    }

    /* libjxl holds its copy of the rows: the encoding runs without the writer's */
    api->mem_free(api, state->pixels);
    state->pixels = NULL;

    if (status == HDRG_OK) {
        JxlEncoderCloseInput(enc);
        status = hdrg_jxl_stream(enc, writer, api);
    }

    JxlEncoderDestroy(enc);

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_codec_jxl_init(hdrg_plugin_t *plugin,
                                                          const hdrg_plugin_api_t *api)
{
    const uint32_t forms = HDRG_CODEC_FORM(HDRG_OUTPUT_GAINMAP) |
                           HDRG_CODEC_FORM(HDRG_OUTPUT_HYBRID) | HDRG_CODEC_FORM(HDRG_OUTPUT_PQ);
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_CODEC_LIT(HDRG_JXL_ID), HDRG_CODEC_LIT("JPEG XL"),
                                  HDRG_CODEC_LIT("1.0.0"), HDRG_CODEC_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_CODEC_LIT(HDRG_JXL_LICENSE),
                                         HDRG_CODEC_LIT(HDRG_JXL_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_CODEC_LIT(HDRG_JXL_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_extensions(cls, HDRG_CODEC_LIT("jxl"));
    if (status == HDRG_OK) {
        status = api->class_set_writer(cls, forms, HDRG_OUTPUT_HYBRID, HDRG_CODEC_ROWS_RGB16,
                                       HDRG_CODEC_COLOR_CICP, HDRG_JXL_MAX_SIDE, hdrg_jxl_create,
                                       hdrg_jxl_rows, hdrg_jxl_finish, hdrg_jxl_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(
            cls, HDRG_CODEC_LIT("quality"), 90, 0, 100,
            HDRG_CODEC_LIT("the base's quality (libjxl's distance of it; 100: lossless)"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(cls, HDRG_CODEC_LIT("gainmap_quality"), 90, 0, 100,
                                           HDRG_CODEC_LIT("... of the gain map"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(cls, HDRG_CODEC_LIT("effort"), 7, 1, 10,
                                           HDRG_CODEC_LIT("libjxl's effort (1: fastest)"));
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
