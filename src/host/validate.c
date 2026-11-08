/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "validate.h"

#include <string.h>

#define HDRG_FLOAT_DIGITS  15u /* significant digits a double holds exactly */
#define HDRG_FLOAT_EXP_MAX 22  /* largest power of ten a double holds exactly */
#define HDRG_LICENSE_REF   "LicenseRef-"
#define HDRG_DOCUMENT_REF  "DocumentRef-"

typedef struct {
    const char *s;
    size_t len, pos;
    uint8_t depth;
} hdrg_spdx_t;

static const double hdrg_pow10[HDRG_FLOAT_EXP_MAX + 1] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static inline bool hdrg_is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

static inline bool hdrg_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static inline bool hdrg_is_alnum(char c)
{
    return hdrg_is_lower(c) || hdrg_is_digit(c) || (c >= 'A' && c <= 'Z');
}

static inline bool hdrg_has_prefix(const char *s, size_t len, const char *prefix)
{
    const size_t n = strlen(prefix);

    return len >= n && memcmp(s, prefix, n) == 0;
}

static inline bool hdrg_word_is(const char *w, size_t wl, const char *word)
{
    return wl == strlen(word) && memcmp(w, word, wl) == 0;
}

/* SPDX idstring: [A-Za-z0-9.-]+ */
static inline bool hdrg_spdx_idstring(const char *s, size_t len)
{
    size_t i;

    if (len == 0) {
        return false;
    }

    for (i = 0; i < len; i++) {
        if (!hdrg_is_alnum(s[i]) && s[i] != '.' && s[i] != '-') {
            return false;
        }
    }

    return true;
}

static inline bool hdrg_spdx_operator(const char *w, size_t wl)
{
    return hdrg_word_is(w, wl, "AND") || hdrg_word_is(w, wl, "OR") || hdrg_word_is(w, wl, "WITH");
}

static inline bool hdrg_spdx_word_char(char c)
{
    return hdrg_is_alnum(c) || c == '.' || c == '-' || c == '+' || c == ':';
}

/* code point of a forbidden kind: controls, bidi controls, separators, noncharacters */
static inline bool hdrg_text_allowed(uint32_t cp, uint8_t flags)
{
    if (cp == 0x0Au) {
        return (flags & HDRG_TEXT_NEWLINE) != 0;
    }

    if (cp == 0x09u) {
        return (flags & HDRG_TEXT_TAB) != 0;
    }

    if (cp < 0x20u || cp == 0x7Fu || (cp >= 0x80u && cp <= 0x9Fu)) {
        return false;
    }

    if (cp == 0x061Cu || cp == 0x200Eu || cp == 0x200Fu || (cp >= 0x202Au && cp <= 0x202Eu) ||
        (cp >= 0x2066u && cp <= 0x2069u) || cp == 0x2028u || cp == 0x2029u) {
        return false;
    }

    return !((cp >= 0xFDD0u && cp <= 0xFDEFu) || (cp & 0xFFFEu) == 0xFFFEu);
}

static bool hdrg_spdx_or(hdrg_spdx_t *p);

/* one code point of strict UTF-8 starting at *pos */
static bool hdrg_utf8_next(const uint8_t *s, size_t len, size_t *pos, uint32_t *cp)
{
    const uint8_t b0 = s[*pos];
    uint32_t value;
    uint8_t need, lo, hi, i, b;

    if (b0 < 0x80u) {
        *cp = b0;
        (*pos)++;

        return true;
    }

    lo = 0x80u;
    hi = 0xBFu;

    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        need = 1;
        value = b0 & 0x1Fu;
    } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
        need = 2;
        value = b0 & 0x0Fu;
        lo = (b0 == 0xE0u) ? 0xA0u : 0x80u; /* no overlong forms */
        hi = (b0 == 0xEDu) ? 0x9Fu : 0xBFu; /* no surrogates */
    } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
        need = 3;
        value = b0 & 0x07u;
        lo = (b0 == 0xF0u) ? 0x90u : 0x80u;
        hi = (b0 == 0xF4u) ? 0x8Fu : 0xBFu; /* nothing above U+10FFFF */
    } else {
        return false;
    }

    if (len - *pos - 1 < need) {
        return false;
    }

    for (i = 0; i < need; i++) {
        b = s[*pos + 1 + i];
        if ((i == 0 && (b < lo || b > hi)) || (i > 0 && (b < 0x80u || b > 0xBFu))) {
            return false;
        }

        value = (value << 6) | (b & 0x3Fu);
    }

    *cp = value;
    *pos += 1u + need;

    return true;
}

static void hdrg_spdx_space(hdrg_spdx_t *p)
{
    while (p->pos < p->len && p->s[p->pos] == ' ') {
        p->pos++;
    }
}

static bool hdrg_spdx_word(hdrg_spdx_t *p, const char **w, size_t *wl)
{
    size_t start;

    hdrg_spdx_space(p);

    start = p->pos;

    while (p->pos < p->len && hdrg_spdx_word_char(p->s[p->pos])) {
        p->pos++;
    }

    *w = p->s + start;
    *wl = p->pos - start;

    return *wl > 0;
}

/* license id with an optional "+", LicenseRef-x or DocumentRef-x:LicenseRef-y */
static bool hdrg_spdx_simple(const char *w, size_t wl)
{
    const char *colon;
    size_t head;

    if (hdrg_has_prefix(w, wl, HDRG_DOCUMENT_REF)) {
        colon = (const char *)memchr(w, ':', wl);
        if (colon == NULL) {
            return false;
        }

        head = (size_t)(colon - w);

        return hdrg_spdx_idstring(w + strlen(HDRG_DOCUMENT_REF),
                                  head - strlen(HDRG_DOCUMENT_REF)) &&
               hdrg_valid_license_ref(colon + 1, wl - head - 1);
    }

    if (hdrg_has_prefix(w, wl, HDRG_LICENSE_REF)) {
        return hdrg_valid_license_ref(w, wl);
    }

    if (hdrg_spdx_operator(w, wl)) {
        return false;
    }

    return (w[wl - 1] == '+') ? hdrg_spdx_idstring(w, wl - 1) : hdrg_spdx_idstring(w, wl);
}

static bool hdrg_spdx_atom(hdrg_spdx_t *p)
{
    const char *w;
    size_t wl, save;

    hdrg_spdx_space(p);

    if (p->pos < p->len && p->s[p->pos] == '(') {
        if (p->depth >= HDRG_SPDX_MAX_DEPTH) {
            return false;
        }

        p->depth++;
        p->pos++;
        if (!hdrg_spdx_or(p)) {
            return false;
        }

        hdrg_spdx_space(p);

        if (p->pos >= p->len || p->s[p->pos] != ')') {
            return false;
        }

        p->pos++;
        p->depth--;

        return true;
    }

    if (!hdrg_spdx_word(p, &w, &wl) || !hdrg_spdx_simple(w, wl)) {
        return false;
    }

    save = p->pos;

    if (!hdrg_spdx_word(p, &w, &wl) || !hdrg_word_is(w, wl, "WITH")) {
        p->pos = save;

        return true;
    }

    return hdrg_spdx_word(p, &w, &wl) && hdrg_spdx_idstring(w, wl) && !hdrg_spdx_operator(w, wl);
}

static bool hdrg_spdx_and(hdrg_spdx_t *p)
{
    const char *w;
    size_t wl, save;

    if (!hdrg_spdx_atom(p)) {
        return false;
    }

    for (;;) {
        save = p->pos;

        if (!hdrg_spdx_word(p, &w, &wl) || !hdrg_word_is(w, wl, "AND")) {
            p->pos = save;

            return true;
        }

        if (!hdrg_spdx_atom(p)) {
            return false;
        }
    }
}

static bool hdrg_spdx_or(hdrg_spdx_t *p)
{
    const char *w;
    size_t wl, save;

    if (!hdrg_spdx_and(p)) {
        return false;
    }

    for (;;) {
        save = p->pos;

        if (!hdrg_spdx_word(p, &w, &wl) || !hdrg_word_is(w, wl, "OR")) {
            p->pos = save;

            return true;
        }

        if (!hdrg_spdx_and(p)) {
            return false;
        }
    }
}

extern bool hdrg_valid_text(const char *s, size_t len, uint8_t flags)
{
    const uint8_t *u = (const uint8_t *)s;
    uint32_t cp;
    size_t pos;

    if (s == NULL) {
        return len == 0;
    }

    pos = 0;
    while (pos < len) {
        if (!hdrg_utf8_next(u, len, &pos, &cp) || !hdrg_text_allowed(cp, flags)) {
            return false;
        }
    }

    return true;
}

extern bool hdrg_valid_id(const char *s, size_t len)
{
    size_t i;
    bool dot, label_start;
    char c;

    if (s == NULL || len == 0 || !hdrg_is_lower(s[0])) {
        return false;
    }

    dot = false;
    label_start = true;
    for (i = 0; i < len; i++) {
        c = s[i];
        if (c == '.') {
            if (label_start) {
                return false;
            }

            label_start = true;
            dot = true;

            continue;
        }

        if (!hdrg_is_lower(c) && !hdrg_is_digit(c) && c != '-' && c != '_') {
            return false;
        }

        if (label_start && !hdrg_is_lower(c) && !hdrg_is_digit(c)) {
            return false;
        }

        label_start = false;
    }

    return dot && !label_start;
}

extern bool hdrg_valid_version(const char *s, size_t len)
{
    size_t i;

    if (s == NULL || len == 0) {
        return false;
    }

    for (i = 0; i < len; i++) {
        if (!hdrg_is_alnum(s[i]) && s[i] != '.' && s[i] != '+' && s[i] != '-') {
            return false;
        }
    }

    return true;
}

extern bool hdrg_valid_key(const char *s, size_t len)
{
    size_t i;

    if (s == NULL || len == 0 || !hdrg_is_lower(s[0])) {
        return false;
    }

    for (i = 1; i < len; i++) {
        if (!hdrg_is_lower(s[i]) && !hdrg_is_digit(s[i]) && s[i] != '_') {
            return false;
        }
    }

    return true;
}

extern bool hdrg_valid_spdx(const char *s, size_t len)
{
    hdrg_spdx_t p;

    if (s == NULL || len == 0) {
        return false;
    }

    p.s = s;
    p.len = len;
    p.pos = 0;
    p.depth = 0;

    if (!hdrg_spdx_or(&p)) {
        return false;
    }

    hdrg_spdx_space(&p);

    return p.pos == len;
}

extern bool hdrg_valid_license_ref(const char *s, size_t len)
{
    return s != NULL && hdrg_has_prefix(s, len, HDRG_LICENSE_REF) &&
           hdrg_spdx_idstring(s + strlen(HDRG_LICENSE_REF), len - strlen(HDRG_LICENSE_REF));
}

extern bool hdrg_spdx_next_ref(const char *s, size_t len, size_t *pos, const char **ref,
                               size_t *ref_len)
{
    size_t start;

    while (*pos < len) {
        if (!hdrg_spdx_word_char(s[*pos])) {
            (*pos)++;

            continue;
        }

        start = *pos;

        while (*pos < len && hdrg_spdx_word_char(s[*pos])) {
            (*pos)++;
        }

        if (hdrg_has_prefix(s + start, *pos - start, HDRG_LICENSE_REF)) {
            *ref = s + start;
            *ref_len = *pos - start;

            return true;
        }
    }

    return false;
}

extern bool hdrg_parse_int(const char *s, size_t len, int64_t *out)
{
    const uint64_t limit_pos = (uint64_t)INT64_MAX;
    uint64_t value, limit;
    size_t i;
    bool negative;

    if (s == NULL || len == 0) {
        return false;
    }

    i = 0;
    negative = s[0] == '-';

    if (s[0] == '-' || s[0] == '+') {
        i = 1;
    }

    if (i == len) {
        return false;
    }

    limit = negative ? limit_pos + 1u : limit_pos;
    value = 0;
    for (; i < len; i++) {
        if (!hdrg_is_digit(s[i]) || value > (limit - (uint64_t)(s[i] - '0')) / 10u) {
            return false;
        }

        value = value * 10u + (uint64_t)(s[i] - '0');
    }

    if (negative) {
        *out = (value == limit_pos + 1u) ? INT64_MIN : -(int64_t)value;
    } else {
        *out = (int64_t)value;
    }

    return true;
}

extern bool hdrg_parse_float(const char *s, size_t len, double *out)
{
    uint64_t mant;
    uint32_t digits, zeros, seen;
    int64_t exp, e;
    size_t i, start;
    bool negative, fraction;
    double value;

    if (s == NULL || len == 0) {
        return false;
    }

    i = 0;
    negative = s[0] == '-';

    if (s[0] == '-' || s[0] == '+') {
        i = 1;
    }

    mant = 0;
    digits = 0;
    zeros = 0;
    seen = 0;
    exp = 0;
    fraction = false;

    /* trailing zeros are deferred, so they never count as significant digits */
    for (; i < len; i++) {
        if (s[i] == '.' && !fraction) {
            fraction = true;

            continue;
        }

        if (!hdrg_is_digit(s[i])) {
            break;
        }

        seen++;

        if (fraction) {
            exp--;
        }

        if (s[i] == '0') {
            if (mant != 0) {
                zeros++;
            }

            continue;
        }

        if (digits + zeros + 1u > HDRG_FLOAT_DIGITS) {
            return false;
        }

        /* the deferred zeros become significant: counted before they are multiplied in */
        digits += zeros + 1u;

        for (; zeros > 0; zeros--) {
            mant *= 10u;
        }

        mant = mant * 10u + (uint64_t)(s[i] - '0');
    }

    if (seen == 0) {
        return false;
    }

    exp += zeros;

    if (i < len) {
        if (s[i] != 'e' && s[i] != 'E') {
            return false;
        }

        start = i + 1;
        if (len - start > 6 || !hdrg_parse_int(s + start, len - start, &e)) {
            return false;
        }

        exp += e;
    }

    if (mant == 0) {
        *out = negative ? -0.0 : 0.0;

        return true;
    }

    if (exp > HDRG_FLOAT_EXP_MAX || exp < -HDRG_FLOAT_EXP_MAX) {
        return false;
    }

    /* both operands are exact, so the one operation rounds correctly */
    value = (exp >= 0) ? (double)mant * hdrg_pow10[exp] : (double)mant / hdrg_pow10[-exp];
    *out = negative ? -value : value;

    return true;
}
