/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_MODULES_HEIF_BMFF_H
#define HDRG_MODULES_HEIF_BMFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"

/*
 * bmff.c: the HEIF of the HEVC images libheif encoded. hdrg_bmff_items reads the coded items of a
 * file of libheif's (each image item's data, of one extent, and its hvcC box); hdrg_heif_mux
 * writes the file through the writer's write_output: the base the primary item; a gain map
 * hidden, the input of a 'tmap' item (ISO 21496-1, its payload the version byte and the metadata
 * body) that an 'altr' group puts before the base, and Apple's auxiliary HDR gain map too where
 * xmp is given (auxC, Apple's XMP item); the Exif item. The brands are libheif's and 'tmap'
 */
typedef struct {
    const uint8_t *data, *config; /* the coded image, its hvcC box (header included) */
    uint32_t id, type;
    size_t size, config_size;
} hdrg_bmff_item_t;

typedef struct {
    hdrg_bmff_item_t item;
    uint32_t width, height;
    uint16_t max_cll, max_fall;          /* 0: no clli */
    uint8_t primaries, transfer, matrix; /* CICP (H.273) */
    uint8_t depth;
} hdrg_heif_image_t;

typedef struct {
    hdrg_heif_image_t base, gainmap;     /* gainmap.item.data NULL: none */
    const uint8_t *ftyp;                 /* libheif's ftyp payload (brands) */
    const uint8_t *tmap;                 /* the tmap item's payload */
    const uint8_t *xmp, *exif;           /* NULL: none (exif: TIFF, no prefix) */
    uint8_t alt_primaries, alt_transfer; /* the tmap item's colr ... */
    uint8_t alt_depth;                   /* ... and pixi */
    size_t ftyp_size, tmap_size, xmp_size, exif_size;
} hdrg_heif_file_t;

/* up to max items; primary: pitm's item; ftyp: its payload in file */
extern hdrg_status_t hdrg_bmff_items(const uint8_t *file, size_t size, hdrg_bmff_item_t *items,
                                     uint32_t max, uint32_t *count, uint32_t *primary,
                                     const uint8_t **ftyp, size_t *ftyp_size);
extern hdrg_status_t hdrg_heif_mux(const hdrg_plugin_api_t *api, hdrg_writer_t *writer,
                                   const hdrg_heif_file_t *file);

#endif /* HDRG_MODULES_HEIF_BMFF_H */
