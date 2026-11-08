/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_MOTION_H
#define HDRG_CORE_MOTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hdrgrading/filter.h>

#include "plan.h"

/*
 * HDRG_SD_MOTION (include/hdrgrading/filter.h items 5 and
 * 8): the side data's layout checked, its fields read, and the gain cap of
 * an interpolated frame from it
 */
#define HDRG_MOTION_GRID_MAX  (1u << 24) /* a field's blocks at most */
#define HDRG_MOTION_BLOCK_MAX 4096u      /* a block's side at most */
#define HDRG_MOTION_CAP_LO    64u        /* a block's confidence at or below: the whole cap */
#define HDRG_MOTION_CAP_HI    192u       /* ... at or above: none */

/* whether data (size bytes) is HDRG_SD_MOTION version 1 as filter.h lays it out */
extern bool hdrg_motion_check(const void *data, size_t size);
/*
 * field i of checked side data: its header and the bytes of its blocks
 * (hdrg_sd_motion_block_t in rows, possibly unaligned); false: no such field
 */
extern bool hdrg_motion_field(const void *data, size_t size, uint32_t i,
                              hdrg_sd_motion_field_t *field, const uint8_t **blocks);
/*
 * The boost grid of an interpolated frame of width x height: head's (its plan's, the nearer input
 * frame's) into capped, each cell whose center lies in a block of low confidence of motion's first
 * field (ref_offset 1: the motion from frame a to frame b at the frame's
 * phase) taking at most, bin by bin, the smaller of a's grid at the cell
 * center's place in a (q - phase v) and b's in b (q + (1 - phase) v): all of
 * the cap at HDRG_MOTION_CAP_LO and below, none at HDRG_MOTION_CAP_HI and
 * above, in between in proportion. a and b: grids of head's geometry (NULL:
 * a plan without boost, all 0). Returns
 * whether any cell is capped (capped is head's otherwise)
 */
extern bool hdrg_motion_cap(const hdrg_plan_t *head, const float *a, const float *b,
                            const void *motion, size_t size, uint32_t width, uint32_t height,
                            float *capped);

#endif /* HDRG_CORE_MOTION_H */
