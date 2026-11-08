/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * Windows: a still by WIC (RGBA, taken as sRGB), a video's first stream by Media Foundation's
 * Source Reader converting to P010 (8-bit sources widened); seeking by its position
 */

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* the source's first video stream */
#define HDRG_GUI_STREAM ((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)

struct hdrg_gui_media {
    IMFSourceReader *reader;
    hdrg_gui_info_t info;
    uint8_t *still;
    uint32_t allocated_height;
    size_t pitch;
    bool still_read, started;
};

/* a sample handed out, its buffer locked */
typedef struct {
    IMFSample *sample;
    IMFMediaBuffer *buffer;
} hdrg_gui_locked_t;

static wchar_t *hdrg_gui_wide(const char *path)
{
    wchar_t *wide;
    int32_t n;

    n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }

    wide = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (wide != NULL) {
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, n);
    }

    return wide;
}

static bool hdrg_gui_open_still(hdrg_gui_media_t *media, const wchar_t *path)
{
    IWICImagingFactory *factory;
    IWICBitmapDecoder *decoder;
    IWICBitmapFrameDecode *frame;
    IWICBitmapSource *converted;
    HRESULT hr;
    UINT width, height;
    bool ok;

    factory = NULL;
    decoder = NULL;
    frame = NULL;
    converted = NULL;
    ok = false;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);

    if (SUCCEEDED(hr)) {
        hr = IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder);
    }

    if (SUCCEEDED(hr)) {
        hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    }

    if (SUCCEEDED(hr)) {
        hr = WICConvertBitmapSource(&GUID_WICPixelFormat32bppRGBA, (IWICBitmapSource *)frame,
                                    &converted);
    }

    if (SUCCEEDED(hr)) {
        hr = IWICBitmapSource_GetSize(converted, &width, &height);
    }

    if (SUCCEEDED(hr) && width > 0 && height > 0) {
        media->still = (uint8_t *)malloc((size_t)width * height * 4u);
        ok = media->still != NULL &&
             SUCCEEDED(IWICBitmapSource_CopyPixels(converted, NULL, width * 4u, width * height * 4u,
                                                   media->still));
        media->info.width = width;
        media->info.height = height;
        media->info.format = HDRG_GUI_RGBA;
        media->info.primaries = HDRG_PRIMARIES_BT709;
        media->info.transfer = HDRG_TRANSFER_SRGB;
    }

    if (converted != NULL) {
        IWICBitmapSource_Release(converted);
    }

    if (frame != NULL) {
        IWICBitmapFrameDecode_Release(frame);
    }

    if (decoder != NULL) {
        IWICBitmapDecoder_Release(decoder);
    }

    if (factory != NULL) {
        IWICImagingFactory_Release(factory);
    }

    return ok;
}

static uint8_t hdrg_gui_matrix_mf(UINT32 matrix)
{
    if (matrix == MFVideoTransferMatrix_BT601) {
        return HDRG_MATRIX_BT601;
    }

    if (matrix == MFVideoTransferMatrix_BT2020_10 || matrix == MFVideoTransferMatrix_BT2020_12) {
        return HDRG_MATRIX_BT2020;
    }

    return HDRG_MATRIX_BT709;
}

static bool hdrg_gui_open_video(hdrg_gui_media_t *media, const wchar_t *path, char *error,
                                size_t error_size)
{
    IMFAttributes *attributes;
    IMFMediaType *native, *wanted, *current;
    PROPVARIANT duration;
    MFVideoArea area;
    HRESULT hr;
    UINT64 size, rate;
    UINT32 transfer, matrix, range, stride;

    attributes = NULL;
    native = NULL;
    wanted = NULL;
    current = NULL;
    hr = MFCreateAttributes(&attributes, 1);

    if (SUCCEEDED(hr)) {
        IMFAttributes_SetUINT32(attributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        hr = MFCreateSourceReaderFromURL(path, attributes, &media->reader);
    }

    if (SUCCEEDED(hr)) {
        hr = IMFSourceReader_GetNativeMediaType(media->reader, HDRG_GUI_STREAM, 0, &native);
    }

    if (FAILED(hr)) {
        snprintf(error, error_size, "not a still or a video this system decodes");

        if (attributes != NULL) {
            IMFAttributes_Release(attributes);
        }

        return false;
    }

    transfer = MFVideoTransFunc_709;
    matrix = MFVideoTransferMatrix_BT709;
    range = MFNominalRange_16_235;
    IMFMediaType_GetUINT32(native, &MF_MT_TRANSFER_FUNCTION, &transfer);
    IMFMediaType_GetUINT32(native, &MF_MT_YUV_MATRIX, &matrix);
    IMFMediaType_GetUINT32(native, &MF_MT_VIDEO_NOMINAL_RANGE, &range);
    IMFMediaType_Release(native);
    IMFAttributes_Release(attributes);

    if (transfer == MFVideoTransFunc_2084 || transfer == MFVideoTransFunc_HLG) {
        snprintf(error, error_size, "the video is already HDR (PQ / HLG)");

        return false;
    }

    hr = MFCreateMediaType(&wanted);

    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(wanted, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(wanted, &MF_MT_SUBTYPE, &MFVideoFormat_P010);
        hr = IMFSourceReader_SetCurrentMediaType(media->reader, HDRG_GUI_STREAM, NULL, wanted);
        IMFMediaType_Release(wanted);
    }

    if (SUCCEEDED(hr)) {
        hr = IMFSourceReader_GetCurrentMediaType(media->reader, HDRG_GUI_STREAM, &current);
    }

    if (FAILED(hr)) {
        snprintf(error, error_size, "the video cannot be decoded to P010");

        return false;
    }

    size = 0;
    rate = 0;
    IMFMediaType_GetUINT64(current, &MF_MT_FRAME_SIZE, &size);
    IMFMediaType_GetUINT64(current, &MF_MT_FRAME_RATE, &rate);
    media->allocated_height = (uint32_t)size;
    media->info.width = (uint32_t)(size >> 32);
    media->info.height = (uint32_t)size;
    stride = media->info.width * 2u;
    IMFMediaType_GetUINT32(current, &MF_MT_DEFAULT_STRIDE, &stride);
    media->pitch = (size_t)stride;

    if (SUCCEEDED(IMFMediaType_GetBlob(current, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area,
                                       (UINT32)sizeof(area), NULL))) {
        media->info.width = (uint32_t)area.Area.cx;
        media->info.height = (uint32_t)area.Area.cy;
    }

    IMFMediaType_Release(current);
    media->info.rate_num = (uint32_t)(rate >> 32);
    media->info.rate_den = (uint32_t)rate;
    media->info.format = HDRG_GUI_P010;
    media->info.matrix = hdrg_gui_matrix_mf(matrix);
    media->info.full_range = range == MFNominalRange_0_255;
    media->info.chroma_loc = HDRG_CHROMA_LEFT;
    media->info.primaries = HDRG_PRIMARIES_BT709;
    media->info.transfer = HDRG_TRANSFER_BT709;
    PropVariantInit(&duration);

    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(
            media->reader, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &duration))) {
        media->info.duration_us = (int64_t)(duration.uhVal.QuadPart / 10u);
    }

    PropVariantClear(&duration);

    if (media->info.width == 0 || media->info.height == 0 || media->info.rate_num == 0 ||
        media->info.rate_den == 0) {
        snprintf(error, error_size, "the video has no size or rate");

        return false;
    }

    return true;
}

extern bool hdrg_gui_media_open(const char *path, hdrg_gui_media_t **out, hdrg_gui_info_t *info,
                                char *error, size_t error_size)
{
    hdrg_gui_media_t *media;
    wchar_t *wide;
    bool ok;

    *out = NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    media = (hdrg_gui_media_t *)calloc(1, sizeof(*media));
    wide = hdrg_gui_wide(path);

    if (media == NULL || wide == NULL) {
        snprintf(error, error_size, "out of memory");
        free(media);
        free(wide);

        return false;
    }

    ok = hdrg_gui_open_still(media, wide) || hdrg_gui_open_video(media, wide, error, error_size);
    free(wide);

    if (!ok) {
        hdrg_gui_media_close(media);

        return false;
    }

    *info = media->info;
    *out = media;

    return true;
}

extern uint8_t hdrg_gui_media_read(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame)
{
    hdrg_gui_locked_t *locked;
    IMFSample *sample;
    HRESULT hr;
    LONGLONG time;
    DWORD index, flags, length;
    BYTE *data;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    memset(frame, 0, sizeof(*frame));

    if (media->still != NULL) {
        if (media->still_read) {
            return HDRG_GUI_READ_END;
        }

        media->still_read = true;
        frame->planes[0] = media->still;
        frame->strides[0] = (size_t)media->info.width * 4u;

        return HDRG_GUI_READ_FRAME;
    }

    for (;;) {
        sample = NULL;
        flags = 0;
        hr = IMFSourceReader_ReadSample(media->reader, HDRG_GUI_STREAM, 0, &index, &flags, &time,
                                        &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            return HDRG_GUI_READ_ERROR;
        }

        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            if (sample != NULL) {
                IMFSample_Release(sample);
            }

            return HDRG_GUI_READ_END;
        }

        if (sample != NULL) {
            break;
        }
    }

    locked = (hdrg_gui_locked_t *)calloc(1, sizeof(*locked));
    if (locked == NULL) {
        IMFSample_Release(sample);

        return HDRG_GUI_READ_ERROR;
    }

    locked->sample = sample;
    hr = IMFSample_ConvertToContiguousBuffer(sample, &locked->buffer);

    if (SUCCEEDED(hr)) {
        hr = IMFMediaBuffer_Lock(locked->buffer, &data, NULL, &length);
    }

    /* the planes the session reads at the size it was made with: a shorter buffer is refused */
    if (SUCCEEDED(hr) && (size_t)length < media->pitch * media->allocated_height +
                                              media->pitch * ((media->info.height + 1u) / 2u)) {
        IMFMediaBuffer_Unlock(locked->buffer);
        hr = E_FAIL;
    }

    if (FAILED(hr)) {
        if (locked->buffer != NULL) {
            IMFMediaBuffer_Release(locked->buffer);
        }

        IMFSample_Release(sample);
        free(locked);

        return HDRG_GUI_READ_ERROR;
    }

    frame->planes[0] = data;
    frame->planes[1] = data + media->pitch * media->allocated_height;
    frame->strides[0] = media->pitch;
    frame->strides[1] = media->pitch;
    frame->pts_us = (int64_t)(time / 10);
    frame->handle = locked;

    return HDRG_GUI_READ_FRAME;
}

extern void hdrg_gui_media_release(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame)
{
    hdrg_gui_locked_t *locked;

    (void)media;
    locked = (hdrg_gui_locked_t *)frame->handle;

    if (locked != NULL) {
        IMFMediaBuffer_Unlock(locked->buffer);
        IMFMediaBuffer_Release(locked->buffer);
        IMFSample_Release(locked->sample);
        free(locked);
        frame->handle = NULL;
    }
}

extern bool hdrg_gui_media_seek(hdrg_gui_media_t *media, int64_t pts_us)
{
    PROPVARIANT position;
    HRESULT hr;

    if (media->still != NULL) {
        media->still_read = false;

        return true;
    }

    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = ((pts_us > 0) ? pts_us : 0) * 10;
    hr = IMFSourceReader_SetCurrentPosition(media->reader, &GUID_NULL, &position);
    PropVariantClear(&position);

    return SUCCEEDED(hr);
}

extern void hdrg_gui_media_close(hdrg_gui_media_t *media)
{
    if (media == NULL) {
        return;
    }

    if (media->reader != NULL) {
        IMFSourceReader_Release(media->reader);
    }

    free(media->still);
    free(media);
}
