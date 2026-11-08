/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include <gif_lib.h>

#include "parts.h"

#define HDRG_GIF_ID      "hdrg.gif"
#define HDRG_GIF_LICENSE "Apache-2.0 OR MIT"
#define HDRG_GIF_COPY    "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"

typedef struct {
    const uint8_t *data;
    size_t size, offset;
} hdrg_gif_source_t;

static int hdrg_gif_read_fn(GifFileType *gif, GifByteType *out, int length)
{
    hdrg_gif_source_t *source = (hdrg_gif_source_t *)gif->UserData;
    size_t take;

    if (length <= 0) {
        return 0;
    }

    take = source->size - source->offset;
    take = ((size_t)length < take) ? (size_t)length : take;
    memcpy(out, source->data + source->offset, take);
    source->offset += take;

    return (int)take;
}

/* Interlaced GIFs store rows in four passes. */
static uint32_t hdrg_gif_row_map(uint32_t stored_row, uint32_t height, bool interlaced)
{
    static const uint32_t start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
    uint32_t pass, rows_in_pass, remaining;

    if (!interlaced) {
        return stored_row;
    }

    remaining = stored_row;
    for (pass = 0; pass < 4; pass++) {
        rows_in_pass =
            (height > start[pass]) ? (height - start[pass] + step[pass] - 1) / step[pass] : 0;
        if (remaining < rows_in_pass) {
            return start[pass] + remaining * step[pass];
        }

        remaining -= rows_in_pass;
    }

    return stored_row;
}

static uint32_t HDRG_PLUGIN_CALL hdrg_gif_probe(const uint8_t *data, size_t size,
                                                const hdrg_plugin_api_t *api)
{
    (void)api;

    return (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) ? 1u
                                                                                             : 0u;
}

/*
 * The first image, read record by record: its descriptor comes before its
 * pixels, so its size is checked before anything is allocated for it (and
 * the frames after it are never decoded). sRGB (the host's default)
 */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_gif_read(hdrg_reader_t *reader, const uint8_t *data,
                                                    size_t size, const hdrg_plugin_api_t *api)
{
    const ColorMapObject *colormap;
    const GifColorType *color;
    hdrg_gif_source_t source;
    hdrg_status_t status;
    GifFileType *gif;
    GifRecordType type;
    GifByteType *extension;
    GifPixelType *line;
    uint64_t max_pixels;
    uint32_t x, y, dst_row, width, height;
    uint8_t *dst, *pixels;
    int gif_error, code, color_index;
    size_t stride;
    bool interlaced;

    source.data = data;
    source.size = size;
    source.offset = 0;
    gif_error = 0;
    line = NULL;
    status = HDRG_ERR_DECODE;

    (void)api->read_limits(reader, &max_pixels, NULL);

    gif = DGifOpen(&source, hdrg_gif_read_fn, &gif_error);
    if (gif == NULL) {
        return HDRG_ERR_DECODE;
    }

    do {
        if (DGifGetRecordType(gif, &type) != GIF_OK) {
            goto done;
        }

        if (type != EXTENSION_RECORD_TYPE) {
            continue;
        }

        if (DGifGetExtension(gif, &code, &extension) != GIF_OK) {
            goto done;
        }

        while (extension != NULL) {
            if (DGifGetExtensionNext(gif, &extension) != GIF_OK) {
                goto done;
            }
        }
    } while (type != IMAGE_DESC_RECORD_TYPE && type != TERMINATE_RECORD_TYPE);

    if (type != IMAGE_DESC_RECORD_TYPE || DGifGetImageDesc(gif) != GIF_OK) {
        goto done;
    }

    colormap = (gif->Image.ColorMap != NULL) ? gif->Image.ColorMap : gif->SColorMap;
    if (colormap == NULL || gif->Image.Width <= 0 || gif->Image.Height <= 0) {
        goto done;
    }

    width = (uint32_t)gif->Image.Width;
    height = (uint32_t)gif->Image.Height;
    interlaced = gif->Image.Interlace;

    if ((uint64_t)width * height > max_pixels) {
        status = HDRG_ERR_LIMIT;

        goto done;
    }

    status = api->read_image(reader, width, height, 8, &pixels, &stride);
    if (status != HDRG_OK) {
        goto done;
    }

    status = HDRG_ERR_NOMEM;

    line = (GifPixelType *)api->mem_alloc(api, width);
    if (line == NULL) {
        goto done;
    }

    status = HDRG_ERR_DECODE;

    for (y = 0; y < height; y++) {
        if (DGifGetLine(gif, line, (int)width) != GIF_OK) {
            goto done;
        }

        dst_row = hdrg_gif_row_map(y, height, interlaced);
        dst = pixels + (size_t)dst_row * stride;
        for (x = 0; x < width; x++) {
            color_index = line[x];
            if (color_index >= colormap->ColorCount) {
                color_index = 0;
            }

            color = &colormap->Colors[color_index];
            dst[x * 3 + 0] = color->Red;
            dst[x * 3 + 1] = color->Green;
            dst[x * 3 + 2] = color->Blue;
        }
    }

    status = HDRG_OK;

done:
    api->mem_free(api, line);
    (void)DGifCloseFile(gif, &gif_error);

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_codec_gif_init(hdrg_plugin_t *plugin,
                                                          const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_CODEC_LIT(HDRG_GIF_ID), HDRG_CODEC_LIT("GIF"),
                                  HDRG_CODEC_LIT("1.0.0"), HDRG_CODEC_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_CODEC_LIT(HDRG_GIF_LICENSE),
                                         HDRG_CODEC_LIT(HDRG_GIF_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_CODEC_LIT(HDRG_GIF_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_extensions(cls, HDRG_CODEC_LIT("gif"));
    if (status == HDRG_OK) {
        status = api->class_set_reader(cls, hdrg_gif_probe, hdrg_gif_read);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
