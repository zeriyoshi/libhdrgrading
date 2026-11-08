/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_WASM_H
#define HDRG_WASM_H

#include <stdint.h>

#include "hdrgrading/hdrgrading.h"

/* hdrg_wasm_preview_state's words (HDRG_SHADER_LAYOUT's bindings 0 to 4, hdrg_apply_state_t) */
#define HDRG_WASM_PREVIEW_PARAMS      0u  /* binding 0, the uniform: pointer, then its size */
#define HDRG_WASM_PREVIEW_LUTS        2u  /* binding 2: pointer, size */
#define HDRG_WASM_PREVIEW_FRAME       4u  /* binding 3: pointer, size */
#define HDRG_WASM_PREVIEW_SOURCE      6u  /* binding 1, the frame's rows: pitch in bytes */
#define HDRG_WASM_PREVIEW_DST         7u  /* binding 4, the PQ R'G'B' output: size, pitch */
#define HDRG_WASM_PREVIEW_GROUPS      9u  /* "apply"'s workgroups: x, y */
#define HDRG_WASM_PREVIEW_GAMUT       11u /* HDRG_GAMUT_* of the output's primaries */
#define HDRG_WASM_PREVIEW_WHITE_MILLI 12u /* the SDR white in millinits */
#define HDRG_WASM_PREVIEW_WORDS       13u

/*
 * The JS-facing layer of the WASM module: plain C calls on
 * pointers and numbers, so JS never depends on a struct layout. Besides
 * these, the module exports malloc / free, hdrg_context_destroy,
 * hdrg_version, hdrg_status_string and hdrg_simd_name. The module is not
 * reentrant: one JS thread (a Worker) calls it.
 */

/* the result of hdrg_wasm_convert */
typedef struct hdrg_wasm_output hdrg_wasm_output_t;
/* the settings of a conversion: the content intent, the display and the file's form */
#define HDRG_WASM_FORMAT  64u /* a writer's id at most, its NUL included */
#define HDRG_WASM_OPTIONS 8u  /* a writer's options at most ... */
#define HDRG_WASM_OPTION  96u /* ... each "key=value" with its NUL */

typedef struct hdrg_wasm_params hdrg_wasm_params_t;
/* the interactive preview of a still: the apply shader's state for the browser's GPU */
typedef struct hdrg_wasm_preview hdrg_wasm_preview_t;

/*
 * threads: the context's thread count (0 = one per core); gpu: HDRG_GPU_* (the
 * WebGPU build: the browser's GPU, else the CPU); NULL when it cannot be created
 */
extern hdrg_context_t *hdrg_wasm_context_create(uint32_t threads, uint32_t gpu);

/* the defaults, the content intent's of preset (HDRG_PRESET_*); NULL on allocation failure */
extern hdrg_wasm_params_t *hdrg_wasm_params_create(uint8_t preset);
/*
 * One parameter by the name of its hdrg-cli option without the dashes
 * ("diffuse-max", "apl-peak-fraction", "mode", "dither" = 0 / 1, ...): hdrg_setting_set.
 * Unknown names give HDRG_ERR_NOT_FOUND, values outside the field's type
 * HDRG_ERR_INVALID_ARG; the ranges are checked by hdrg_wasm_convert.
 */
extern hdrg_status_t hdrg_wasm_params_set(hdrg_wasm_params_t *params, const char *name,
                                          double value);
/*
 * The conversion's writer, a codec's id (hdrg.jxl; NULL or "": hdrg.jpeg, the default), its options
 * dropped; HDRG_ERR_INVALID_ARG for an id too long (the id itself is checked by hdrg_wasm_convert)
 */
extern hdrg_status_t hdrg_wasm_params_format(hdrg_wasm_params_t *params, const char *id);
/*
 * an option of the writer ("quality" = "90" ...), parsed and checked by hdrg_wasm_convert;
 * HDRG_ERR_INVALID_ARG past HDRG_WASM_OPTIONS of them or one too long
 */
extern hdrg_status_t hdrg_wasm_params_format_option(hdrg_wasm_params_t *params, const char *key,
                                                    const char *value);
extern void hdrg_wasm_params_destroy(hdrg_wasm_params_t *params);

/*
 * hdrg_convert of input into the params' writer (NULL params: the defaults, hdrg.jpeg); NULL only
 * when the result itself cannot be allocated
 */
extern hdrg_wasm_output_t *hdrg_wasm_convert(hdrg_context_t *ctx, const uint8_t *input,
                                             uint32_t input_size, const hdrg_wasm_params_t *params);
extern hdrg_status_t hdrg_wasm_output_status(const hdrg_wasm_output_t *output);
/* the output file (NULL unless the status is HDRG_OK) and its size */
extern const uint8_t *hdrg_wasm_output_data(const hdrg_wasm_output_t *output);
extern uint32_t hdrg_wasm_output_size(const hdrg_wasm_output_t *output);
extern void hdrg_wasm_output_free(hdrg_wasm_output_t *output);

/* HDRG_SIMD_* of the kernels of the last conversion (0 = none yet) */
extern uint32_t hdrg_wasm_last_tier(const hdrg_context_t *ctx);

/*
 * The interactive preview of a still (the demo's WebGPU 2):
 * rgba (8-bit sRGB, width x height, rows of width x 4 bytes, alpha ignored)
 * through a session of one frame (hdrg_session_receive_state: the frame's
 * analysis and plan here, the apply on the browser's GPU with the shader of
 * hdrg_wasm_preview_shader). The params' PREFILTER filters are left out (a
 * state's session has none) and the grain kept (PRESERVE), as a still's; the
 * output is 16-bit PQ R'G'B' in the display's gamut. rgba is read until
 * hdrg_wasm_preview_destroy. NULL only when the preview itself cannot be
 * allocated
 */
extern hdrg_wasm_preview_t *hdrg_wasm_preview_create(hdrg_context_t *ctx, const uint8_t *rgba,
                                                     uint32_t width, uint32_t height,
                                                     const hdrg_wasm_params_t *params);
extern hdrg_status_t hdrg_wasm_preview_status(const hdrg_wasm_preview_t *preview);
/*
 * The state's HDRG_WASM_PREVIEW_WORDS words (HDRG_OK only): pointers into the
 * module's memory and sizes in bytes, valid until hdrg_wasm_preview_destroy
 */
extern const uint32_t *hdrg_wasm_preview_state(const hdrg_wasm_preview_t *preview);
extern void hdrg_wasm_preview_destroy(hdrg_wasm_preview_t *preview);
/* the apply module's WGSL (entry point "apply"), NUL-terminated */
extern const char *hdrg_wasm_preview_shader(void);

#endif /* HDRG_WASM_H */
