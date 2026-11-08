/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_MCTF_H
#define HDRG_PLUGINS_MCTF_H

/*
 * What hdrg.mctf (src/plugins/mctf/mctf.c) shares with its GPU versions
 * (gk_mctf.c, webgpu_mctf.c): the sample weights, the noise's tau, the
 * options, the output's HDRG_SD_MOTION and the shader's table
 * (src/plugins/mctf/wgsl/mctf.wgsl)
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/plugins/blockme.h"

#define HDRG_MCTF_REACH    2u   /* the window each way at most ... */
#define HDRG_MCTF_REFS     4u   /* ... its frames besides the job's ... */
#define HDRG_MCTF_SLOTS    5u   /* ... the pyramids kept, by frame index */
#define HDRG_MCTF_STEPS    64u  /* the sample weights' steps of d / tau (1/16) ... */
#define HDRG_MCTF_LEVELS   256u /* ... and tau by the luma's top 8 bits */
#define HDRG_MCTF_SHIFT    9u   /* the GPU's penalty: 257 << 9 (a sixteenth of interp's) */
#define HDRG_MCTF_TABLE    8u   /* the shader's table: words a level ... */
#define HDRG_MCTF_PLANES   48u  /* ... each slot's plane of each level ... */
#define HDRG_MCTF_FIELDS   78u  /* ... each other frame's field of each level ... */
#define HDRG_MCTF_SCRATCH  102u /* ... the scratch ... */
#define HDRG_MCTF_WEIGHTS  103u /* ... each other frame's blocks' weights ... */
#define HDRG_MCTF_TAUS     107u /* ... tau by the luma's top 8 bits ... */
#define HDRG_MCTF_STEPS_AT 363u /* ... the sample weights' steps */
#define HDRG_MCTF_WORDS    427u

/* the sample weight of step i of d / tau (1/16) in 1/256 */
extern uint32_t hdrg_mctf_weight(uint32_t i);
/*
 * a frame's noise (HDRG_SD_NOISE) for hdrg.mctf of strength: tau (16-bit
 * codes) by the luma's top 8 bits into taus, the blocks' mean |difference|
 * of all and of no weight; *active whether there is noise to remove
 */
extern hdrg_status_t hdrg_mctf_noise(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                     double strength, uint32_t taus[HDRG_MCTF_LEVELS],
                                     uint32_t *block_lo, uint32_t *block_hi, bool *active);
/* hdrg.mctf's options but threads: strength, past and future */
extern hdrg_status_t hdrg_mctf_options(hdrg_class_t *cls, const hdrg_plugin_api_t *api);
/*
 * The output's HDRG_SD_MOTION (hdrg_bme_motion_size and hdrg_bme_motion_head of count fields:
 * the other frames at offsets in the job's order, phase 0): block i of field k from blockme's
 * vector (quarter pixels at half size), the match's mean |difference| (16-bit codes) and the
 * block's weight (1/256: its confidence)
 */
extern void hdrg_mctf_motion_block(uint32_t blocks_w, uint32_t blocks_h, uint32_t k, size_t i,
                                   int32_t vx, int32_t vy, uint32_t mean, uint32_t weight,
                                   uint8_t *side);
/*
 * The GPU versions' table for bme's levels into words, the pyramid's and the
 * motion's words (false: too large). The motion begins with each other
 * frame's blocks' words (the weight, the mean |difference| above) and their
 * finest fields: the side data's hdrg_mctf_motion_prefix bytes, read back
 * and made the side data's count fields by hdrg_mctf_motion_read
 */
extern bool hdrg_mctf_table(const hdrg_bme_t *bme, uint32_t words[HDRG_MCTF_WORDS],
                            uint64_t *pyramid, uint64_t *motion);
extern size_t hdrg_mctf_motion_prefix(const hdrg_bme_t *bme);
extern void hdrg_mctf_motion_read(const hdrg_bme_t *bme, const uint8_t *prefix, uint32_t count,
                                  const int32_t *offsets, uint8_t *side);

#endif /* HDRG_PLUGINS_MCTF_H */
