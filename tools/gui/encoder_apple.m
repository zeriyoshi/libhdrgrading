/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/* macOS: AVAssetWriter's HEVC Main10 (VideoToolbox) of P010 frames tagged BT.2020 PQ, with the
   mastering display (Display P3 D65 up to the peak) */

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

struct hdrg_gui_encoder {
    AVAssetWriter *writer;
    AVAssetWriterInput *input;
    AVAssetWriterInputPixelBufferAdaptor *adaptor;
    NSURL *url;
    uint32_t width, height;
    bool started;
};

static void hdrg_gui_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void hdrg_gui_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* SMPTE ST 2086 as the SEI's 24 bytes: G, B, R, white in 0.00002, luminances in 0.0001 cd/m2 */
static NSData *hdrg_gui_mastering(float peak_nits)
{
    static const uint32_t xy[8] = {13250, 34500, 7500, 3000, 34000, 16000, 15635, 16450};
    uint8_t bytes[24];
    uint32_t i;

    for (i = 0; i < 8u; i++) {
        hdrg_gui_be16(bytes + i * 2u, xy[i]);
    }

    hdrg_gui_be32(bytes + 16, (uint32_t)(peak_nits * 10000.0f + 0.5f));
    hdrg_gui_be32(bytes + 20, 1);

    return [NSData dataWithBytes:bytes length:sizeof(bytes)];
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
    NSDictionary *settings, *attributes, *compression;
    NSError *failure;
    NSString *type;
    double fps;

    *out = NULL;
    encoder = (hdrg_gui_encoder_t *)calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        snprintf(error, error_size, "out of memory");

        return false;
    }

    @autoreleasepool {
        encoder->url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        [[NSFileManager defaultManager] removeItemAtURL:encoder->url error:nil];
        type = [[@(path) pathExtension] caseInsensitiveCompare:@"mp4"] == NSOrderedSame
                   ? AVFileTypeMPEG4
                   : AVFileTypeQuickTimeMovie;
        failure = nil;
        encoder->writer = [AVAssetWriter assetWriterWithURL:encoder->url fileType:type error:&failure];
        fps = (double)rate_num / (double)rate_den;
        compression = @{
            (__bridge NSString *)kVTCompressionPropertyKey_ProfileLevel :
                (__bridge NSString *)kVTProfileLevel_HEVC_Main10_AutoLevel,
            AVVideoExpectedSourceFrameRateKey : @(fps),
            AVVideoAverageBitRateKey : @((double)width * height * fps * 0.15),
            (__bridge NSString *)kVTCompressionPropertyKey_MasteringDisplayColorVolume :
                hdrg_gui_mastering(peak_nits),
        };
        settings = @{
            AVVideoCodecKey : AVVideoCodecTypeHEVC,
            AVVideoWidthKey : @(width),
            AVVideoHeightKey : @(height),
            AVVideoColorPropertiesKey : @{
                AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_2020,
                AVVideoTransferFunctionKey : AVVideoTransferFunction_SMPTE_ST_2084_PQ,
                AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_2020,
            },
            AVVideoCompressionPropertiesKey : compression,
        };
        attributes = @{
            (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey :
                @(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange),
            (__bridge NSString *)kCVPixelBufferWidthKey : @(width),
            (__bridge NSString *)kCVPixelBufferHeightKey : @(height),
        };

        if (encoder->writer != nil) {
            encoder->input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                outputSettings:settings];
            encoder->input.expectsMediaDataInRealTime = NO;
            encoder->adaptor = [AVAssetWriterInputPixelBufferAdaptor
                assetWriterInputPixelBufferAdaptorWithAssetWriterInput:encoder->input
                                           sourcePixelBufferAttributes:attributes];
        }

        if (encoder->writer == nil || ![encoder->writer canAddInput:encoder->input]) {
            snprintf(error, error_size, "the HEVC 10-bit writer cannot be made: %s",
                     (failure != nil) ? failure.localizedDescription.UTF8String : "no input");
            hdrg_gui_encoder_close(encoder, false, NULL, 0);

            return false;
        }

        [encoder->writer addInput:encoder->input];

        if (![encoder->writer startWriting]) {
            snprintf(error, error_size, "%s", encoder->writer.error.localizedDescription.UTF8String);
            hdrg_gui_encoder_close(encoder, false, NULL, 0);

            return false;
        }
    }

    encoder->width = width;
    encoder->height = height;
    *out = encoder;

    return true;
}

extern bool hdrg_gui_encoder_write(hdrg_gui_encoder_t *encoder, const uint8_t *luma,
                                   const uint8_t *chroma, int64_t pts_us)
{
    const uint32_t rows[2] = {encoder->height, (encoder->height + 1u) / 2u};
    const size_t bytes[2] = {(size_t)encoder->width * 2u,
                             (size_t)((encoder->width + 1u) / 2u) * 4u};
    const uint8_t *planes[2] = {luma, chroma};
    CVPixelBufferRef buffer;
    CMTime time;
    uint32_t p, y;
    uint8_t *base;
    size_t pitch;
    bool ok;

    @autoreleasepool {
        time = CMTimeMake(pts_us, 1000000);

        if (!encoder->started) {
            [encoder->writer startSessionAtSourceTime:time];
            encoder->started = true;
        }

        while (!encoder->input.readyForMoreMediaData) {
            if (encoder->writer.status != AVAssetWriterStatusWriting) {
                return false;
            }

            [NSThread sleepForTimeInterval:0.002];
        }

        buffer = NULL;

        if (CVPixelBufferPoolCreatePixelBuffer(NULL, encoder->adaptor.pixelBufferPool, &buffer) !=
                kCVReturnSuccess ||
            buffer == NULL) {
            return false;
        }

        CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey,
                              kCVImageBufferColorPrimaries_ITU_R_2020, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey,
                              kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey,
                              kCVImageBufferYCbCrMatrix_ITU_R_2020, kCVAttachmentMode_ShouldPropagate);
        CVPixelBufferLockBaseAddress(buffer, 0);

        for (p = 0; p < 2u; p++) {
            base = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buffer, p);
            pitch = CVPixelBufferGetBytesPerRowOfPlane(buffer, p);

            for (y = 0; y < rows[p]; y++) {
                memcpy(base + (size_t)y * pitch, planes[p] + (size_t)y * bytes[p],
                       (pitch < bytes[p]) ? pitch : bytes[p]);
            }
        }

        CVPixelBufferUnlockBaseAddress(buffer, 0);
        ok = [encoder->adaptor appendPixelBuffer:buffer withPresentationTime:time];
        CVPixelBufferRelease(buffer);
    }

    return ok;
}

extern bool hdrg_gui_encoder_close(hdrg_gui_encoder_t *encoder, bool keep, char *error,
                                   size_t error_size)
{
    dispatch_semaphore_t done;
    bool ok;

    ok = true;

    @autoreleasepool {
        if (keep && encoder->writer != nil && encoder->started) {
            done = dispatch_semaphore_create(0);
            [encoder->input markAsFinished];
            [encoder->writer finishWritingWithCompletionHandler:^{
              dispatch_semaphore_signal(done);
            }];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            ok = encoder->writer.status == AVAssetWriterStatusCompleted;

            if (!ok && error != NULL) {
                snprintf(error, error_size, "%s",
                         encoder->writer.error.localizedDescription.UTF8String);
            }
        } else if (encoder->writer != nil) {
            [encoder->writer cancelWriting];
            [[NSFileManager defaultManager] removeItemAtURL:encoder->url error:nil];
        }

        encoder->adaptor = nil;
        encoder->input = nil;
        encoder->writer = nil;
        encoder->url = nil;
    }

    free(encoder);

    return ok;
}
