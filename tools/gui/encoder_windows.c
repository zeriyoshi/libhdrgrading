/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * Windows: Media Foundation's Sink Writer, HEVC Main10 (the system's encoder, a hardware one
 * where there is) into MP4, of P010 frames tagged BT.2020 PQ with the mastering luminance
 */

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* codecapi.h's eAVEncH265VProfile_Main_420_10: that header declares its GUIDs in C++'s form only */
#define HDRG_GUI_HEVC_MAIN_420_10 2u

struct hdrg_gui_encoder {
    IMFSinkWriter *writer;
    wchar_t *path;
    LONGLONG duration;
    uint32_t width, height;
    DWORD stream;
    bool started;
};

static wchar_t *hdrg_gui_wide_path(const char *path)
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

/* the frame's size, rate and progressive scan on a type of the subtype */
static HRESULT hdrg_gui_video_type(const GUID *subtype, uint32_t width, uint32_t height,
                                   uint32_t rate_num, uint32_t rate_den, IMFMediaType **out)
{
    HRESULT hr;

    hr = MFCreateMediaType(out);
    if (FAILED(hr)) {
        return hr;
    }

    IMFMediaType_SetGUID(*out, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(*out, &MF_MT_SUBTYPE, subtype);
    IMFMediaType_SetUINT32(*out, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT64(*out, &MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
    IMFMediaType_SetUINT64(*out, &MF_MT_FRAME_RATE, ((UINT64)rate_num << 32) | rate_den);
    IMFMediaType_SetUINT64(*out, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1u);
    IMFMediaType_SetUINT32(*out, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
    IMFMediaType_SetUINT32(*out, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084);
    IMFMediaType_SetUINT32(*out, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_10);
    IMFMediaType_SetUINT32(*out, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

    return S_OK;
}

extern bool hdrg_gui_encoder_available(void)
{
    return true;
}

extern bool hdrg_gui_encoder_open(const char *path, uint32_t width, uint32_t height,
                                  uint32_t rate_num, uint32_t rate_den, float peak_nits,
                                  hdrg_gui_encoder_t **out, char *error, size_t error_size)
{
    hdrg_gui_encoder_t *encoder;
    IMFAttributes *attributes;
    IMFMediaType *output, *input;
    HRESULT hr;
    size_t length;

    *out = NULL;
    length = strlen(path);

    if (length < 4u || _stricmp(path + length - 4u, ".mp4") != 0) {
        snprintf(error, error_size, "Windows writes MP4 (.mp4) only");

        return false;
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    encoder = (hdrg_gui_encoder_t *)calloc(1, sizeof(*encoder));
    if (encoder == NULL || (encoder->path = hdrg_gui_wide_path(path)) == NULL) {
        snprintf(error, error_size, "out of memory");
        free(encoder);

        return false;
    }

    attributes = NULL;
    output = NULL;
    input = NULL;
    hr = MFCreateAttributes(&attributes, 1);

    if (SUCCEEDED(hr)) {
        IMFAttributes_SetUINT32(attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        hr = MFCreateSinkWriterFromURL(encoder->path, NULL, attributes, &encoder->writer);
    }

    if (SUCCEEDED(hr)) {
        hr = hdrg_gui_video_type(&MFVideoFormat_HEVC, width, height, rate_num, rate_den, &output);
    }

    if (SUCCEEDED(hr)) {
        IMFMediaType_SetUINT32(output, &MF_MT_AVG_BITRATE,
                               (UINT32)((double)width * height * rate_num / rate_den * 0.15));
        IMFMediaType_SetUINT32(output, &MF_MT_VIDEO_PROFILE, HDRG_GUI_HEVC_MAIN_420_10);
        IMFMediaType_SetUINT32(output, &MF_MT_MAX_MASTERING_LUMINANCE, (UINT32)(peak_nits + 0.5f));
        IMFMediaType_SetUINT32(output, &MF_MT_MIN_MASTERING_LUMINANCE, 1u);
        hr = IMFSinkWriter_AddStream(encoder->writer, output, &encoder->stream);
    }

    if (SUCCEEDED(hr)) {
        hr = hdrg_gui_video_type(&MFVideoFormat_P010, width, height, rate_num, rate_den, &input);
    }

    if (SUCCEEDED(hr)) {
        hr = IMFSinkWriter_SetInputMediaType(encoder->writer, encoder->stream, input, NULL);
    }

    if (SUCCEEDED(hr)) {
        hr = IMFSinkWriter_BeginWriting(encoder->writer);
    }

    if (input != NULL) {
        IMFMediaType_Release(input);
    }

    if (output != NULL) {
        IMFMediaType_Release(output);
    }

    if (attributes != NULL) {
        IMFAttributes_Release(attributes);
    }

    if (FAILED(hr)) {
        snprintf(
            error, error_size,
            "no HEVC 10-bit encoder (HRESULT 0x%08lx; the HEVC Video Extensions may be needed)",
            (unsigned long)hr);
        hdrg_gui_encoder_close(encoder, false, NULL, 0);

        return false;
    }

    encoder->width = width;
    encoder->height = height;
    encoder->duration = (LONGLONG)(10000000.0 * rate_den / rate_num);
    *out = encoder;

    return true;
}

extern bool hdrg_gui_encoder_write(hdrg_gui_encoder_t *encoder, const uint8_t *luma,
                                   const uint8_t *chroma, int64_t pts_us)
{
    const size_t luma_bytes = (size_t)encoder->width * 2u * encoder->height,
                 chroma_bytes =
                     (size_t)((encoder->width + 1u) / 2u) * 4u * ((encoder->height + 1u) / 2u);
    IMFMediaBuffer *buffer;
    IMFSample *sample;
    HRESULT hr;
    BYTE *data;

    buffer = NULL;
    sample = NULL;
    hr = MFCreateMemoryBuffer((DWORD)(luma_bytes + chroma_bytes), &buffer);

    if (SUCCEEDED(hr)) {
        hr = IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
    }

    if (SUCCEEDED(hr)) {
        memcpy(data, luma, luma_bytes);
        memcpy(data + luma_bytes, chroma, chroma_bytes);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_SetCurrentLength(buffer, (DWORD)(luma_bytes + chroma_bytes));
        hr = MFCreateSample(&sample);
    }

    if (SUCCEEDED(hr)) {
        IMFSample_AddBuffer(sample, buffer);
        IMFSample_SetSampleTime(sample, (LONGLONG)pts_us * 10);
        IMFSample_SetSampleDuration(sample, encoder->duration);
        hr = IMFSinkWriter_WriteSample(encoder->writer, encoder->stream, sample);
        encoder->started = true;
    }

    if (sample != NULL) {
        IMFSample_Release(sample);
    }

    if (buffer != NULL) {
        IMFMediaBuffer_Release(buffer);
    }

    return SUCCEEDED(hr);
}

extern bool hdrg_gui_encoder_close(hdrg_gui_encoder_t *encoder, bool keep, char *error,
                                   size_t error_size)
{
    HRESULT hr;
    bool ok;

    ok = true;

    if (encoder->writer != NULL) {
        if (keep && encoder->started) {
            hr = IMFSinkWriter_Finalize(encoder->writer);
            ok = SUCCEEDED(hr);

            if (!ok && error != NULL) {
                snprintf(error, error_size, "finalizing failed (HRESULT 0x%08lx)",
                         (unsigned long)hr);
            }
        }

        IMFSinkWriter_Release(encoder->writer);
    }

    if ((!keep || !ok) && encoder->path != NULL) {
        DeleteFileW(encoder->path);
    }

    free(encoder->path);
    free(encoder);

    return ok;
}
