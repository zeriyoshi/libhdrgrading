/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_VALIDATE_H
#define HDRG_HOST_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Validation of what crosses the plugin boundary.
 * Every function reads exactly len bytes and never relies on a terminator.
 */
#define HDRG_TEXT_NEWLINE 0x01u /* allow line feeds */
#define HDRG_TEXT_TAB     0x02u /* allow tabs */

#define HDRG_SPDX_MAX_DEPTH 8u

/*
 * Strict UTF-8 (no overlong forms, surrogates or code points above
 * U+10FFFF) without NUL, C0 / C1 controls, DEL, bidirectional controls,
 * line / paragraph separators or noncharacters; flags allow LF / TAB.
 */
extern bool hdrg_valid_text(const char *s, size_t len, uint8_t flags);
/* reverse-domain id: [a-z0-9._-], starts with a letter, dot-separated non-empty labels */
extern bool hdrg_valid_id(const char *s, size_t len);
/* SemVer characters: [0-9A-Za-z.+-], non-empty */
extern bool hdrg_valid_version(const char *s, size_t len);
/* option key: [a-z0-9_], starts with a letter */
extern bool hdrg_valid_key(const char *s, size_t len);
/* SPDX license expression syntax (AND / OR / WITH, parentheses up to 8 deep) */
extern bool hdrg_valid_spdx(const char *s, size_t len);
/* LicenseRef-<idstring> (as a license text's id) */
extern bool hdrg_valid_license_ref(const char *s, size_t len);
/*
 * Iterates the document-local LicenseRef- identifiers an SPDX expression
 * uses: *pos starts at 0; false when there are no more
 */
extern bool hdrg_spdx_next_ref(const char *s, size_t len, size_t *pos, const char **ref,
                               size_t *ref_len);

/* [+-]digits, no overflow */
extern bool hdrg_parse_int(const char *s, size_t len, int64_t *out);
/*
 * [+-]digits[.digits][(e|E)[+-]digits], locale-independent and identical on
 * every platform (correctly rounded up to 15 significant digits, |exponent| <= 22)
 */
extern bool hdrg_parse_float(const char *s, size_t len, double *out);

#endif /* HDRG_HOST_VALIDATE_H */
