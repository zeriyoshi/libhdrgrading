/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/* macOS: a still by ImageIO (oriented, drawn in sRGB), a video's first track by AVFoundation
   (AVAssetReader: NV12, or P010 for a source above 8 bits), seeking by a new reader */

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

struct hdrg_gui_media {
    AVAsset *asset;
    AVAssetTrack *track;
    AVAssetReader *reader;
    AVAssetReaderTrackOutput *output;
    uint8_t *still;
    hdrg_gui_info_t info;
    OSType pixel_format;
    bool still_read;
};

/* the keys loaded, waited for */
static bool hdrg_gui_load(id<AVAsynchronousKeyValueLoading> object, NSArray<NSString *> *keys)
{
    dispatch_semaphore_t done;
    NSString *key;
    NSError *error;

    done = dispatch_semaphore_create(0);
    [object loadValuesAsynchronouslyForKeys:keys
                          completionHandler:^{
                            dispatch_semaphore_signal(done);
                          }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

    for (key in keys) {
        error = nil;

        if ([object statusOfValueForKey:key error:&error] != AVKeyValueStatusLoaded) {
            return false;
        }
    }

    return true;
}

static bool hdrg_gui_open_still(hdrg_gui_media_t *media, NSURL *url)
{
    CGImageSourceRef source;
    CGImageRef image;
    CGColorSpaceRef space;
    CGContextRef context;
    NSDictionary *options;
    size_t width, height;

    source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    if (source == NULL || CGImageSourceGetCount(source) == 0) {
        if (source != NULL) {
            CFRelease(source);
        }

        return false;
    }

    options = @{
        (__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
        (__bridge NSString *)kCGImageSourceCreateThumbnailWithTransform : @YES,
        (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize : @(1 << 16),
    };
    image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)options);
    CFRelease(source);
    if (image == NULL) {
        return false;
    }

    width = CGImageGetWidth(image);
    height = CGImageGetHeight(image);
    media->still = (uint8_t *)calloc(width * height, 4u);
    space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    context = (media->still != NULL)
                  ? CGBitmapContextCreate(media->still, width, height, 8, width * 4u, space,
                                          (CGBitmapInfo)kCGImageAlphaNoneSkipLast |
                                              kCGBitmapByteOrder32Big)
                  : NULL;
    CGColorSpaceRelease(space);

    if (context == NULL) {
        CGImageRelease(image);
        free(media->still);
        media->still = NULL;

        return false;
    }

    CGContextDrawImage(context, CGRectMake(0, 0, (CGFloat)width, (CGFloat)height), image);
    CGContextRelease(context);
    CGImageRelease(image);
    media->info.width = (uint32_t)width;
    media->info.height = (uint32_t)height;
    media->info.format = HDRG_GUI_RGBA;
    media->info.primaries = HDRG_PRIMARIES_BT709;
    media->info.transfer = HDRG_TRANSFER_SRGB;

    return true;
}

/* a reader of the track from start_us on */
static bool hdrg_gui_reader(hdrg_gui_media_t *media, int64_t start_us)
{
    NSDictionary *settings;
    NSError *error;
    CMTime start;

    [media->reader cancelReading];
    media->reader = nil;
    media->output = nil;
    error = nil;
    media->reader = [AVAssetReader assetReaderWithAsset:media->asset error:&error];
    if (media->reader == nil) {
        return false;
    }

    start = CMTimeMake(start_us, 1000000);
    media->reader.timeRange = CMTimeRangeMake(start, kCMTimePositiveInfinity);
    settings = @{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey : @(media->pixel_format)};
    media->output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:media->track
                                                               outputSettings:settings];
    media->output.alwaysCopiesSampleData = NO;

    if (![media->reader canAddOutput:media->output]) {
        return false;
    }

    [media->reader addOutput:media->output];

    return [media->reader startReading];
}

static uint8_t hdrg_gui_matrix(CFTypeRef matrix)
{
    if (matrix != NULL && CFEqual(matrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4)) {
        return HDRG_MATRIX_BT601;
    }

    if (matrix != NULL && CFEqual(matrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_2020)) {
        return HDRG_MATRIX_BT2020;
    }

    return HDRG_MATRIX_BT709;
}

static uint8_t hdrg_gui_chroma(CFTypeRef location)
{
    if (location != NULL && CFEqual(location, kCMFormatDescriptionChromaLocation_Center)) {
        return HDRG_CHROMA_CENTER;
    }

    if (location != NULL && CFEqual(location, kCMFormatDescriptionChromaLocation_TopLeft)) {
        return HDRG_CHROMA_TOPLEFT;
    }

    return HDRG_CHROMA_LEFT;
}

static bool hdrg_gui_open_video(hdrg_gui_media_t *media, NSURL *url, char *error,
                                size_t error_size)
{
    CMFormatDescriptionRef description;
    CFTypeRef transfer, bits, full;
    NSArray *tracks, *descriptions;
    CGSize size;
    CMTime duration;
    float rate;

    media->asset = [AVURLAsset URLAssetWithURL:url options:nil];

    if (!hdrg_gui_load(media->asset, @[ @"tracks", @"duration" ])) {
        snprintf(error, error_size, "not a still or a video this system decodes");

        return false;
    }

    tracks = [media->asset valueForKey:@"tracks"];
    media->track = nil;

    for (AVAssetTrack *track in tracks) {
        if ([track.mediaType isEqualToString:AVMediaTypeVideo]) {
            media->track = track;

            break;
        }
    }

    if (media->track == nil ||
        !hdrg_gui_load(media->track, @[ @"naturalSize", @"nominalFrameRate", @"formatDescriptions" ])) {
        snprintf(error, error_size, "no video track");

        return false;
    }

    size = [[media->track valueForKey:@"naturalSize"] sizeValue];
    rate = [[media->track valueForKey:@"nominalFrameRate"] floatValue];
    duration = [[media->asset valueForKey:@"duration"] CMTimeValue];
    descriptions = [media->track valueForKey:@"formatDescriptions"];
    description = (descriptions.count > 0)
                      ? (__bridge CMFormatDescriptionRef)descriptions[0]
                      : NULL;
    transfer = (description != NULL) ? CMFormatDescriptionGetExtension(
                                           description, kCMFormatDescriptionExtension_TransferFunction)
                                     : NULL;

    if (transfer != NULL && (CFEqual(transfer, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ) ||
                             CFEqual(transfer, kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG))) {
        snprintf(error, error_size, "the video is already HDR (PQ / HLG)");

        return false;
    }

    bits = (description != NULL) ? CMFormatDescriptionGetExtension(
                                       description, kCMFormatDescriptionExtension_BitsPerComponent)
                                 : NULL;
    full = (description != NULL) ? CMFormatDescriptionGetExtension(
                                       description, kCMFormatDescriptionExtension_FullRangeVideo)
                                 : NULL;
    media->info.width = (uint32_t)size.width;
    media->info.height = (uint32_t)size.height;
    media->info.rate_num = (uint32_t)(rate * 1000.0f + 0.5f);
    media->info.rate_den = 1000u;
    media->info.duration_us = (int64_t)(CMTimeGetSeconds(duration) * 1e6);
    media->info.full_range = full != NULL && CFBooleanGetValue((CFBooleanRef)full);
    media->info.format = (bits != NULL && [(__bridge NSNumber *)bits intValue] > 8)
                             ? HDRG_GUI_P010
                             : HDRG_GUI_NV12;
    media->info.matrix = hdrg_gui_matrix(
        (description != NULL)
            ? CMFormatDescriptionGetExtension(description, kCMFormatDescriptionExtension_YCbCrMatrix)
            : NULL);
    media->info.chroma_loc = hdrg_gui_chroma(
        (description != NULL) ? CMFormatDescriptionGetExtension(
                                    description, kCMFormatDescriptionExtension_ChromaLocationTopField)
                              : NULL);
    media->info.primaries = HDRG_PRIMARIES_BT709;
    media->info.transfer = HDRG_TRANSFER_BT709;

    if (media->info.format == HDRG_GUI_P010) {
        media->pixel_format = media->info.full_range ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                                                     : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    } else {
        media->pixel_format = media->info.full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                                     : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    }

    if (media->info.width == 0 || media->info.height == 0 || media->info.rate_num == 0 ||
        !hdrg_gui_reader(media, 0)) {
        snprintf(error, error_size, "the video cannot be read");

        return false;
    }

    return true;
}

extern bool hdrg_gui_media_open(const char *path, hdrg_gui_media_t **out, hdrg_gui_info_t *info,
                                char *error, size_t error_size)
{
    hdrg_gui_media_t *media;
    NSURL *url;
    bool ok;

    *out = NULL;
    media = (hdrg_gui_media_t *)calloc(1, sizeof(*media));
    if (media == NULL) {
        snprintf(error, error_size, "out of memory");

        return false;
    }

    @autoreleasepool {
        url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        ok = hdrg_gui_open_still(media, url) || hdrg_gui_open_video(media, url, error, error_size);
    }

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
    CMSampleBufferRef sample;
    CVPixelBufferRef pixels;
    size_t p;

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

    @autoreleasepool {
        sample = [media->output copyNextSampleBuffer];
    }

    if (sample == NULL) {
        return (media->reader.status == AVAssetReaderStatusCompleted) ? HDRG_GUI_READ_END
                                                                      : HDRG_GUI_READ_ERROR;
    }

    pixels = CMSampleBufferGetImageBuffer(sample);
    if (pixels == NULL || CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != 0) {
        CFRelease(sample);

        return HDRG_GUI_READ_ERROR;
    }

    /* the session reads the size it was made with: a smaller picture is refused */
    if (CVPixelBufferGetPlaneCount(pixels) < 2u ||
        CVPixelBufferGetWidthOfPlane(pixels, 0) < media->info.width ||
        CVPixelBufferGetHeightOfPlane(pixels, 0) < media->info.height ||
        CVPixelBufferGetHeightOfPlane(pixels, 1) < (media->info.height + 1u) / 2u) {
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        CFRelease(sample);

        return HDRG_GUI_READ_ERROR;
    }

    for (p = 0; p < 2u; p++) {
        frame->planes[p] = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixels, p);
        frame->strides[p] = CVPixelBufferGetBytesPerRowOfPlane(pixels, p);
    }

    frame->pts_us = (int64_t)(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)) * 1e6 +
                              0.5);
    frame->handle = (void *)sample;

    return HDRG_GUI_READ_FRAME;
}

extern void hdrg_gui_media_release(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame)
{
    CMSampleBufferRef sample;

    (void)media;
    sample = (CMSampleBufferRef)frame->handle;

    if (sample != NULL) {
        CVPixelBufferUnlockBaseAddress(CMSampleBufferGetImageBuffer(sample),
                                       kCVPixelBufferLock_ReadOnly);
        CFRelease(sample);
        frame->handle = NULL;
    }
}

extern bool hdrg_gui_media_seek(hdrg_gui_media_t *media, int64_t pts_us)
{
    bool ok;

    if (media->still != NULL) {
        media->still_read = false;

        return true;
    }

    @autoreleasepool {
        ok = hdrg_gui_reader(media, (pts_us > 0) ? pts_us : 0);
    }

    return ok;
}

extern void hdrg_gui_media_close(hdrg_gui_media_t *media)
{
    if (media == NULL) {
        return;
    }

    @autoreleasepool {
        [media->reader cancelReading];
        media->reader = nil;
        media->output = nil;
        media->track = nil;
        media->asset = nil;
    }

    free(media->still);
    free(media);
}
