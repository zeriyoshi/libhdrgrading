/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_TOOLS_COMMON_H
#define HDRG_TOOLS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/plugin.h"
#include "hdrgrading/video.h"

/*
 * What the command-line tools (hdrg-cli, hdrg-video, hdrg-ffmpeg) parse alike: the filters to
 * add (--filter <stage>=<id> and the --filter-opt key=value after it), the settings by name
 * (--set name=value: a value's name or a number, hdrg_setting_parse) and the enumerated
 * options (their values' names from the library's table)
 */
#define TOOLS_MAX_FILTERS 8u
#define TOOLS_MAX_OPTS    64u /* --filter-opt a filter */
#define TOOLS_MAX_KEY     64u

/* a --filter <stage>=<id> and the --filter-opt key=value that follow it */
typedef struct {
    const char *spec;
    const char *opts[TOOLS_MAX_OPTS];
    uint32_t opt_count;
} tools_filter_t;

/* --filter spec after the count there are (TOOLS_MAX_FILTERS at most); false: printed why */
extern bool tools_filter(tools_filter_t *filters, uint32_t *count, const char *spec);
/* --filter-opt opt of the latest --filter; false: printed why */
extern bool tools_filter_opt(tools_filter_t *filters, uint32_t count, const char *opt);
/*
 * the filters onto ctx in order, each at the stage its spec names (scale=, prefilter=,
 * interp=; among stages: HDRG_STAGE_* bits) with its options; false: printed why
 */
extern bool tools_add_filters(hdrg_context_t *ctx, const tools_filter_t *filters, uint32_t count,
                              uint32_t stages);
/*
 * --set name=value into the struct of the setting's part (NULL: the tool has none of it);
 * false: printed why
 */
extern bool tools_set(const char *arg, hdrg_params_t *params, hdrg_display_caps_t *display,
                      hdrg_encode_params_t *encode, hdrg_session_config_t *session);
/*
 * option's text as the value of setting name: one of its values' names, else a whole number of
 * 0..255; false: printed the names it expects
 */
extern bool tools_named(const char *option, const char *name, const char *text, uint8_t *out);

#endif /* HDRG_TOOLS_COMMON_H */
