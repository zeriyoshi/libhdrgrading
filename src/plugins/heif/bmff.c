/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * The HEIF (ISO/IEC 23008-12) boxes of a still with an ISO 21496-1 gain
 * map: libheif encodes the HEVC images (its
 * public API writes no 'tmap' item nor 'altr' group), this reads them back
 * out of its file and writes the whole file:
 *   ftyp (libheif's brands, 'tmap')
 *   meta: hdlr pict, pitm 1, iloc v1, iinf, iref (dimg tmap -> base, gain
 *         map; auxl gain map -> base; cdsc XMP -> gain map, Exif -> base),
 *         iprp (ipco: hvcC ispe colr pixi [clli] per image, [auxC], the
 *         tmap's colr and pixi; ipma), idat (tmap, XMP, Exif), grpl (altr:
 *         tmap, base)
 *   mdat: the base, the gain map
 */

#include <string.h>

#include "bmff.h"

#define HDRG_BMFF_FOURCC(a, b, c, d)                                                               \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define HDRG_BMFF_APPLE_AUX "urn:com:apple:photo:2020:aux:hdrgainmap"
#define HDRG_BMFF_XMP_TYPE  "application/rdf+xml"

typedef struct {
    const hdrg_plugin_api_t *api;
    uint8_t *data;
    size_t size, capacity;
    bool failed;
} hdrg_bmff_buf_t;

/* a box at at of [at, end): its type, header and total size */
typedef struct {
    uint32_t type;
    size_t at, header, size;
} hdrg_bmff_box_t;

/* the location of an item (iloc) */
typedef struct {
    uint32_t id;
    uint8_t method;
    uint64_t offset, length;
} hdrg_bmff_loc_t;

static inline uint32_t hdrg_bmff_be(const uint8_t *p, uint8_t bytes)
{
    uint32_t v;
    uint8_t i;

    v = 0;

    for (i = 0; i < bytes; i++) {
        v = (v << 8) | p[i];
    }

    return v;
}

static inline bool hdrg_bmff_room(size_t at, size_t need, size_t end)
{
    return at <= end && need <= end - at;
}

/* the box at at, within [at, end) */
static bool hdrg_bmff_box(const uint8_t *data, size_t at, size_t end, hdrg_bmff_box_t *box)
{
    uint64_t size;

    if (!hdrg_bmff_room(at, 8, end)) {
        return false;
    }

    size = hdrg_bmff_be(data + at, 4);
    box->type = hdrg_bmff_be(data + at + 4, 4);
    box->at = at;
    box->header = 8;

    if (size == 1) {
        if (!hdrg_bmff_room(at, 16, end)) {
            return false;
        }

        size = ((uint64_t)hdrg_bmff_be(data + at + 8, 4) << 32) | hdrg_bmff_be(data + at + 12, 4);
        box->header = 16;
    } else if (size == 0) {
        size = end - at;
    }

    if (size < box->header || size > end - at) {
        return false;
    }

    box->size = (size_t)size;

    return true;
}

/* a variable-size (0, 4 or 8 bytes) field of iloc */
static bool hdrg_bmff_field(const uint8_t *data, size_t *at, size_t end, uint8_t bytes,
                            uint64_t *out)
{
    if (bytes != 0 && bytes != 4 && bytes != 8) {
        return false;
    }

    if (!hdrg_bmff_room(*at, bytes, end)) {
        return false;
    }

    *out = 0;

    if (bytes == 8) {
        *out = ((uint64_t)hdrg_bmff_be(data + *at, 4) << 32) | hdrg_bmff_be(data + *at + 4, 4);
    } else if (bytes == 4) {
        *out = hdrg_bmff_be(data + *at, 4);
    }

    *at += bytes;

    return true;
}

/* the iloc entry of item id (one extent) */
static bool hdrg_bmff_iloc(const uint8_t *data, const hdrg_bmff_box_t *iloc, uint32_t id,
                           hdrg_bmff_loc_t *out)
{
    const size_t end = iloc->at + iloc->size;
    uint64_t base, index, offset, length;
    uint32_t count, i, item, extents, e;
    uint8_t version, offset_size, length_size, base_size, index_size, method;
    size_t at;

    at = iloc->at + iloc->header;

    if (!hdrg_bmff_room(at, 6, end)) {
        return false;
    }

    version = data[at];
    offset_size = (uint8_t)(data[at + 4] >> 4);
    length_size = (uint8_t)(data[at + 4] & 15u);
    base_size = (uint8_t)(data[at + 5] >> 4);
    index_size = (version >= 1) ? (uint8_t)(data[at + 5] & 15u) : 0u;
    at += 6;

    if (version > 2 || !hdrg_bmff_room(at, (version < 2) ? 2u : 4u, end)) {
        return false;
    }

    count = hdrg_bmff_be(data + at, (version < 2) ? 2u : 4u);
    at += (version < 2) ? 2u : 4u;

    for (i = 0; i < count; i++) {
        if (!hdrg_bmff_room(at, ((version < 2) ? 2u : 4u) + ((version >= 1) ? 2u : 0u) + 2u, end)) {
            return false;
        }

        item = hdrg_bmff_be(data + at, (version < 2) ? 2u : 4u);
        at += (version < 2) ? 2u : 4u;
        method = 0;

        if (version >= 1) {
            method = (uint8_t)(hdrg_bmff_be(data + at, 2) & 15u);
            at += 2;
        }

        at += 2; /* data_reference_index */

        if (!hdrg_bmff_field(data, &at, end, base_size, &base) || !hdrg_bmff_room(at, 2, end)) {
            return false;
        }

        extents = hdrg_bmff_be(data + at, 2);
        at += 2;

        for (e = 0; e < extents; e++) {
            if ((index_size != 0 && !hdrg_bmff_field(data, &at, end, index_size, &index)) ||
                !hdrg_bmff_field(data, &at, end, offset_size, &offset) ||
                !hdrg_bmff_field(data, &at, end, length_size, &length)) {
                return false;
            }

            if (item == id && extents == 1) {
                out->id = id;
                out->method = method;
                out->offset = base + offset;
                out->length = length;

                return method <= 1;
            }
        }
    }

    return false;
}

/* the hvcC box associated with item id (ipma over ipco) */
static bool hdrg_bmff_config(const uint8_t *data, const hdrg_bmff_box_t *ipco,
                             const hdrg_bmff_box_t *ipma, uint32_t id, hdrg_bmff_item_t *out)
{
    const size_t end = ipma->at + ipma->size, props_end = ipco->at + ipco->size;
    hdrg_bmff_box_t prop;
    uint32_t count, i, item, index, k;
    uint8_t version, flags, associations, a;
    size_t at, p;

    at = ipma->at + ipma->header;

    if (!hdrg_bmff_room(at, 8, end)) {
        return false;
    }

    version = data[at];
    flags = data[at + 3];
    count = hdrg_bmff_be(data + at + 4, 4);
    at += 8;

    for (i = 0; i < count; i++) {
        if (!hdrg_bmff_room(at, (version < 1) ? 3u : 5u, end)) {
            return false;
        }

        item = hdrg_bmff_be(data + at, (version < 1) ? 2u : 4u);
        at += (version < 1) ? 2u : 4u;
        associations = data[at];
        at++;

        for (a = 0; a < associations; a++) {
            if (!hdrg_bmff_room(at, (flags & 1u) ? 2u : 1u, end)) {
                return false;
            }

            index = (flags & 1u) ? (hdrg_bmff_be(data + at, 2) & 0x7FFFu) : (data[at] & 0x7Fu);
            at += (flags & 1u) ? 2u : 1u;

            if (item != id || index == 0) {
                continue;
            }

            /* the index-th property (1-based) */
            p = ipco->at + ipco->header;

            for (k = 1; k < index && hdrg_bmff_box(data, p, props_end, &prop); k++) {
                p += prop.size;
            }

            if (k == index && hdrg_bmff_box(data, p, props_end, &prop) &&
                prop.type == HDRG_BMFF_FOURCC('h', 'v', 'c', 'C')) {
                out->config = data + p;
                out->config_size = prop.size;

                return true;
            }
        }
    }

    return false;
}

static void hdrg_bmff_put(hdrg_bmff_buf_t *buf, const void *bytes, size_t count)
{
    uint8_t *grown;
    size_t capacity;

    if (buf->failed || count == 0) {
        return;
    }

    if (count > SIZE_MAX / 2u - buf->size) {
        buf->failed = true;

        return;
    }

    if (buf->size + count > buf->capacity) {
        capacity = (buf->capacity == 0) ? 65536u : buf->capacity;

        while (capacity < buf->size + count) {
            capacity *= 2u;
        }

        grown = (uint8_t *)buf->api->mem_realloc(buf->api, buf->data, capacity);
        if (grown == NULL) {
            buf->failed = true;

            return;
        }

        buf->data = grown;
        buf->capacity = capacity;
    }

    memcpy(buf->data + buf->size, bytes, count);
    buf->size += count;
}

static void hdrg_bmff_u8(hdrg_bmff_buf_t *buf, uint8_t v)
{
    hdrg_bmff_put(buf, &v, 1);
}

static void hdrg_bmff_u16(hdrg_bmff_buf_t *buf, uint16_t v)
{
    uint8_t bytes[2];

    bytes[0] = (uint8_t)(v >> 8);
    bytes[1] = (uint8_t)v;

    hdrg_bmff_put(buf, bytes, 2);
}

static void hdrg_bmff_u32(hdrg_bmff_buf_t *buf, uint32_t v)
{
    uint8_t bytes[4];

    bytes[0] = (uint8_t)(v >> 24);
    bytes[1] = (uint8_t)(v >> 16);
    bytes[2] = (uint8_t)(v >> 8);
    bytes[3] = (uint8_t)v;

    hdrg_bmff_put(buf, bytes, 4);
}

static void hdrg_bmff_patch(hdrg_bmff_buf_t *buf, size_t at, uint32_t v)
{
    if (buf->failed || at + 4u > buf->size) {
        return;
    }

    buf->data[at] = (uint8_t)(v >> 24);
    buf->data[at + 1] = (uint8_t)(v >> 16);
    buf->data[at + 2] = (uint8_t)(v >> 8);
    buf->data[at + 3] = (uint8_t)v;
}

/* opens a box (a full box when version is at most 255: its version and flags): where it starts */
static size_t hdrg_bmff_open(hdrg_bmff_buf_t *buf, const char *type, uint16_t version,
                             uint32_t flags)
{
    const size_t at = buf->size;

    hdrg_bmff_u32(buf, 0);
    hdrg_bmff_put(buf, type, 4);

    if (version <= 255u) {
        hdrg_bmff_u32(buf, ((uint32_t)version << 24) | (flags & 0xFFFFFFu));
    }

    return at;
}

static void hdrg_bmff_close(hdrg_bmff_buf_t *buf, size_t at)
{
    hdrg_bmff_patch(buf, at, (uint32_t)(buf->size - at));
}

/* a property into ipco: its index (1-based) */
static uint8_t hdrg_bmff_ispe(hdrg_bmff_buf_t *buf, uint8_t *count, uint32_t width, uint32_t height)
{
    size_t box;

    box = hdrg_bmff_open(buf, "ispe", 0, 0);
    hdrg_bmff_u32(buf, width);
    hdrg_bmff_u32(buf, height);
    hdrg_bmff_close(buf, box);

    return ++*count;
}

static uint8_t hdrg_bmff_colr(hdrg_bmff_buf_t *buf, uint8_t *count, uint8_t primaries,
                              uint8_t transfer, uint8_t matrix)
{
    size_t box;

    box = hdrg_bmff_open(buf, "colr", 256, 0);
    hdrg_bmff_put(buf, "nclx", 4);
    hdrg_bmff_u16(buf, primaries);
    hdrg_bmff_u16(buf, transfer);
    hdrg_bmff_u16(buf, matrix);
    hdrg_bmff_u8(buf, 0x80); /* full range */
    hdrg_bmff_close(buf, box);

    return ++*count;
}

static uint8_t hdrg_bmff_pixi(hdrg_bmff_buf_t *buf, uint8_t *count, uint8_t depth)
{
    uint8_t c;
    size_t box;

    box = hdrg_bmff_open(buf, "pixi", 0, 0);
    hdrg_bmff_u8(buf, 3);

    for (c = 0; c < 3u; c++) {
        hdrg_bmff_u8(buf, depth);
    }

    hdrg_bmff_close(buf, box);

    return ++*count;
}

/* an image's hvcC, ispe, colr, pixi and clli into ipco: their indices into props (essential first)
 */
static uint8_t hdrg_bmff_image_props(hdrg_bmff_buf_t *buf, uint8_t *count,
                                     const hdrg_heif_image_t *image, uint8_t *props)
{
    uint8_t n;
    size_t box;

    hdrg_bmff_put(buf, image->item.config, image->item.config_size);
    props[0] = (uint8_t)(0x80u | ++*count);
    props[1] = hdrg_bmff_ispe(buf, count, image->width, image->height);
    props[2] = hdrg_bmff_colr(buf, count, image->primaries, image->transfer, image->matrix);
    props[3] = hdrg_bmff_pixi(buf, count, image->depth);
    n = 4;

    if (image->max_cll > 0) {
        box = hdrg_bmff_open(buf, "clli", 256, 0);
        hdrg_bmff_u16(buf, image->max_cll);
        hdrg_bmff_u16(buf, image->max_fall);
        hdrg_bmff_close(buf, box);
        props[n++] = ++*count;
    }

    return n;
}

/* an infe (version 2) of item id: hidden in flags; a 'mime' item's content type */
static void hdrg_bmff_infe(hdrg_bmff_buf_t *buf, uint16_t id, const char *type, bool hidden,
                           const char *content_type)
{
    size_t box;

    box = hdrg_bmff_open(buf, "infe", 2, hidden ? 1u : 0u);
    hdrg_bmff_u16(buf, id);
    hdrg_bmff_u16(buf, 0);
    hdrg_bmff_put(buf, type, 4);
    hdrg_bmff_u8(buf, 0); /* item_name */

    if (content_type != NULL) {
        hdrg_bmff_put(buf, content_type, strlen(content_type) + 1u);
    }

    hdrg_bmff_close(buf, box);
}

/* a single item type reference (iref version 0) from item from to count items */
static void hdrg_bmff_ref(hdrg_bmff_buf_t *buf, const char *type, uint16_t from, const uint16_t *to,
                          uint16_t count)
{
    uint16_t i;
    size_t box;

    box = hdrg_bmff_open(buf, type, 256, 0);
    hdrg_bmff_u16(buf, from);
    hdrg_bmff_u16(buf, count);

    for (i = 0; i < count; i++) {
        hdrg_bmff_u16(buf, to[i]);
    }

    hdrg_bmff_close(buf, box);
}

/* an ipma entry (version 0, 7-bit indices) */
static void hdrg_bmff_assoc(hdrg_bmff_buf_t *buf, uint16_t id, const uint8_t *props, uint8_t count)
{
    hdrg_bmff_u16(buf, id);
    hdrg_bmff_u8(buf, count);
    hdrg_bmff_put(buf, props, count);
}

/* an iloc entry (version 1, 4-byte offsets and lengths): where its offset is */
static size_t hdrg_bmff_loc(hdrg_bmff_buf_t *buf, uint16_t id, uint8_t method, uint32_t offset,
                            uint32_t length)
{
    size_t at;

    hdrg_bmff_u16(buf, id);
    hdrg_bmff_u16(buf, method);
    hdrg_bmff_u16(buf, 0); /* data_reference_index */
    hdrg_bmff_u16(buf, 1); /* extent_count */
    at = buf->size;
    hdrg_bmff_u32(buf, offset);
    hdrg_bmff_u32(buf, length);

    return at;
}

extern hdrg_status_t hdrg_bmff_items(const uint8_t *file, size_t size, hdrg_bmff_item_t *items,
                                     uint32_t max, uint32_t *count, uint32_t *primary,
                                     const uint8_t **ftyp, size_t *ftyp_size)
{
    hdrg_bmff_box_t box, meta, child, iinf, iloc, ipco, ipma, idat, infe;
    hdrg_bmff_loc_t loc;
    uint32_t entries, i, id;
    uint8_t version;
    size_t at, end, c;
    bool have_iinf, have_iloc, have_ipco, have_ipma, have_idat;

    *count = 0;
    *primary = 0;
    *ftyp = NULL;
    *ftyp_size = 0;
    have_iinf = false;
    have_iloc = false;
    have_ipco = false;
    have_ipma = false;
    have_idat = false;
    memset(&meta, 0, sizeof(meta));
    memset(&iinf, 0, sizeof(iinf));
    memset(&iloc, 0, sizeof(iloc));
    memset(&ipco, 0, sizeof(ipco));
    memset(&ipma, 0, sizeof(ipma));
    memset(&idat, 0, sizeof(idat));

    for (at = 0; hdrg_bmff_box(file, at, size, &box); at += box.size) {
        if (box.type == HDRG_BMFF_FOURCC('f', 't', 'y', 'p')) {
            *ftyp = file + box.at + box.header;
            *ftyp_size = box.size - box.header;
        } else if (box.type == HDRG_BMFF_FOURCC('m', 'e', 't', 'a')) {
            meta = box;
        }
    }

    if (meta.size < meta.header + 4u || *ftyp == NULL) {
        return HDRG_ERR_ENCODE;
    }

    end = meta.at + meta.size;

    for (c = meta.at + meta.header + 4u; hdrg_bmff_box(file, c, end, &child); c += child.size) {
        if (child.type == HDRG_BMFF_FOURCC('p', 'i', 't', 'm') && child.size >= child.header + 6u &&
            (file[child.at + child.header] == 0 || child.size >= child.header + 8u)) {
            version = file[child.at + child.header];
            *primary = hdrg_bmff_be(file + child.at + child.header + 4u, (version == 0) ? 2u : 4u);
        } else if (child.type == HDRG_BMFF_FOURCC('i', 'i', 'n', 'f')) {
            iinf = child;
            have_iinf = true;
        } else if (child.type == HDRG_BMFF_FOURCC('i', 'l', 'o', 'c')) {
            iloc = child;
            have_iloc = true;
        } else if (child.type == HDRG_BMFF_FOURCC('i', 'd', 'a', 't')) {
            idat = child;
            have_idat = true;
        } else if (child.type == HDRG_BMFF_FOURCC('i', 'p', 'r', 'p')) {
            for (at = child.at + child.header; hdrg_bmff_box(file, at, child.at + child.size, &box);
                 at += box.size) {
                if (box.type == HDRG_BMFF_FOURCC('i', 'p', 'c', 'o')) {
                    ipco = box;
                    have_ipco = true;
                } else if (box.type == HDRG_BMFF_FOURCC('i', 'p', 'm', 'a')) {
                    ipma = box;
                    have_ipma = true;
                }
            }
        }
    }

    if (!have_iinf || !have_iloc || !have_ipco || !have_ipma || iinf.size < iinf.header + 6u ||
        (file[iinf.at + iinf.header] != 0 && iinf.size < iinf.header + 8u)) {
        return HDRG_ERR_ENCODE;
    }

    version = file[iinf.at + iinf.header];
    entries = hdrg_bmff_be(file + iinf.at + iinf.header + 4u, (version == 0) ? 2u : 4u);
    at = iinf.at + iinf.header + 4u + ((version == 0) ? 2u : 4u);
    end = iinf.at + iinf.size;

    for (i = 0; i < entries && hdrg_bmff_box(file, at, end, &infe); i++, at += infe.size) {
        version = (infe.size > infe.header) ? file[infe.at + infe.header] : 0u;

        if (infe.type != HDRG_BMFF_FOURCC('i', 'n', 'f', 'e') || version < 2 ||
            infe.size < infe.header + ((version == 2) ? 12u : 14u)) {
            continue;
        }

        id = hdrg_bmff_be(file + infe.at + infe.header + 4u, (version == 2) ? 2u : 4u);

        if (*count >= max ||
            hdrg_bmff_be(file + infe.at + infe.header + ((version == 2) ? 8u : 10u), 4) !=
                HDRG_BMFF_FOURCC('h', 'v', 'c', '1')) {
            continue;
        }

        memset(&items[*count], 0, sizeof(items[*count]));
        items[*count].id = id;
        items[*count].type = HDRG_BMFF_FOURCC('h', 'v', 'c', '1');

        if (!hdrg_bmff_iloc(file, &iloc, id, &loc) ||
            !hdrg_bmff_config(file, &ipco, &ipma, id, &items[*count])) {
            return HDRG_ERR_ENCODE;
        }

        if (loc.method == 1) {
            if (!have_idat ||
                !hdrg_bmff_room(idat.header + (size_t)loc.offset, (size_t)loc.length, idat.size)) {
                return HDRG_ERR_ENCODE;
            }

            items[*count].data = file + idat.at + idat.header + (size_t)loc.offset;
        } else {
            if (loc.offset > size || loc.length > size - loc.offset) {
                return HDRG_ERR_ENCODE;
            }

            items[*count].data = file + (size_t)loc.offset;
        }

        items[*count].size = (size_t)loc.length;
        (*count)++;
    }

    return (*count > 0) ? HDRG_OK : HDRG_ERR_ENCODE;
}

extern hdrg_status_t hdrg_heif_mux(const hdrg_plugin_api_t *api, hdrg_writer_t *writer,
                                   const hdrg_heif_file_t *file)
{
    const bool gainmap = file->gainmap.item.data != NULL, apple = gainmap && file->xmp != NULL;
    hdrg_bmff_buf_t buf;
    hdrg_status_t status;
    uint32_t brand;
    uint16_t base_id, gm_id, tmap_id, xmp_id, exif_id, group_id, items, to[2];
    uint8_t props_base[8], props_gm[8], props_tmap[4], n_base, n_gm, n_tmap, n_props;
    size_t at, meta, box, sub, base_loc, gm_loc, mdat;
    bool has_tmap_brand;

    if (file->ftyp_size < 8u || (file->ftyp_size % 4u) != 0) {
        return HDRG_ERR_ENCODE;
    }

    memset(&buf, 0, sizeof(buf));
    buf.api = api;
    base_id = 1;
    gm_id = gainmap ? 2u : 0u;
    tmap_id = gainmap ? 3u : 0u;
    items = gainmap ? 4u : 2u;
    xmp_id = apple ? items++ : 0u;
    exif_id = (file->exif != NULL) ? items++ : 0u;
    group_id = items;
    n_props = 0;
    n_gm = 0;
    n_tmap = 0;

    /* ftyp: libheif's brands, and 'tmap' for the gain map's reader */
    box = hdrg_bmff_open(&buf, "ftyp", 256, 0);
    hdrg_bmff_put(&buf, file->ftyp, file->ftyp_size);
    has_tmap_brand = false;

    for (at = 8; at + 4u <= file->ftyp_size; at += 4) {
        brand = hdrg_bmff_be(file->ftyp + at, 4);
        has_tmap_brand = has_tmap_brand || brand == HDRG_BMFF_FOURCC('t', 'm', 'a', 'p');
    }

    if (gainmap && !has_tmap_brand) {
        hdrg_bmff_put(&buf, "tmap", 4);
    }

    hdrg_bmff_close(&buf, box);

    meta = hdrg_bmff_open(&buf, "meta", 0, 0);

    box = hdrg_bmff_open(&buf, "hdlr", 0, 0);
    hdrg_bmff_u32(&buf, 0);
    hdrg_bmff_put(&buf, "pict", 4);
    hdrg_bmff_u32(&buf, 0);
    hdrg_bmff_u32(&buf, 0);
    hdrg_bmff_u32(&buf, 0);
    hdrg_bmff_u8(&buf, 0);
    hdrg_bmff_close(&buf, box);

    box = hdrg_bmff_open(&buf, "pitm", 0, 0);
    hdrg_bmff_u16(&buf, base_id);
    hdrg_bmff_close(&buf, box);

    /* iloc: the images in mdat (patched below), the payloads in idat after one another */
    box = hdrg_bmff_open(&buf, "iloc", 1, 0);
    hdrg_bmff_u8(&buf, 0x44); /* offset_size 4, length_size 4 */
    hdrg_bmff_u8(&buf, 0x00); /* base_offset_size 0, index_size 0 */
    hdrg_bmff_u16(&buf, (uint16_t)(items - 1u));
    base_loc = hdrg_bmff_loc(&buf, base_id, 0, 0, (uint32_t)file->base.item.size);
    gm_loc = 0;
    at = 0;

    if (gainmap) {
        gm_loc = hdrg_bmff_loc(&buf, gm_id, 0, 0, (uint32_t)file->gainmap.item.size);
        (void)hdrg_bmff_loc(&buf, tmap_id, 1, (uint32_t)at, (uint32_t)file->tmap_size);
        at += file->tmap_size;
    }

    if (apple) {
        (void)hdrg_bmff_loc(&buf, xmp_id, 1, (uint32_t)at, (uint32_t)file->xmp_size);
        at += file->xmp_size;
    }

    if (exif_id != 0) {
        (void)hdrg_bmff_loc(&buf, exif_id, 1, (uint32_t)at, (uint32_t)(4u + file->exif_size));
    }

    hdrg_bmff_close(&buf, box);

    box = hdrg_bmff_open(&buf, "iinf", 0, 0);
    hdrg_bmff_u16(&buf, (uint16_t)(items - 1u));
    hdrg_bmff_infe(&buf, base_id, "hvc1", false, NULL);

    if (gainmap) {
        hdrg_bmff_infe(&buf, gm_id, "hvc1", true, NULL);
        hdrg_bmff_infe(&buf, tmap_id, "tmap", false, NULL);
    }

    if (apple) {
        hdrg_bmff_infe(&buf, xmp_id, "mime", false, HDRG_BMFF_XMP_TYPE);
    }

    if (exif_id != 0) {
        hdrg_bmff_infe(&buf, exif_id, "Exif", false, NULL);
    }

    hdrg_bmff_close(&buf, box);

    if (gainmap || exif_id != 0) {
        box = hdrg_bmff_open(&buf, "iref", 0, 0);

        if (gainmap) {
            to[0] = base_id;
            to[1] = gm_id;
            hdrg_bmff_ref(&buf, "dimg", tmap_id, to, 2);
        }

        if (apple) {
            hdrg_bmff_ref(&buf, "auxl", gm_id, &base_id, 1);
            hdrg_bmff_ref(&buf, "cdsc", xmp_id, &gm_id, 1);
        }

        if (exif_id != 0) {
            hdrg_bmff_ref(&buf, "cdsc", exif_id, &base_id, 1);
        }

        hdrg_bmff_close(&buf, box);
    }

    box = hdrg_bmff_open(&buf, "iprp", 256, 0);
    sub = hdrg_bmff_open(&buf, "ipco", 256, 0);
    n_base = hdrg_bmff_image_props(&buf, &n_props, &file->base, props_base);

    if (gainmap) {
        n_gm = hdrg_bmff_image_props(&buf, &n_props, &file->gainmap, props_gm);

        if (apple) {
            at = hdrg_bmff_open(&buf, "auxC", 0, 0);
            hdrg_bmff_put(&buf, HDRG_BMFF_APPLE_AUX, sizeof(HDRG_BMFF_APPLE_AUX));
            hdrg_bmff_close(&buf, at);
            props_gm[n_gm++] = (uint8_t)(0x80u | ++n_props);
        }

        /* the tmap: the base's size, the alternate rendition's colors and depth */
        props_tmap[0] = props_base[1];
        props_tmap[1] = hdrg_bmff_colr(&buf, &n_props, file->alt_primaries, file->alt_transfer, 2);
        props_tmap[2] = hdrg_bmff_pixi(&buf, &n_props, file->alt_depth);
        n_tmap = 3;
    }

    hdrg_bmff_close(&buf, sub);

    sub = hdrg_bmff_open(&buf, "ipma", 0, 0);
    hdrg_bmff_u32(&buf, gainmap ? 3u : 1u);
    hdrg_bmff_assoc(&buf, base_id, props_base, n_base);

    if (gainmap) {
        hdrg_bmff_assoc(&buf, gm_id, props_gm, n_gm);
        hdrg_bmff_assoc(&buf, tmap_id, props_tmap, n_tmap);
    }

    hdrg_bmff_close(&buf, sub);
    hdrg_bmff_close(&buf, box);

    if (gainmap || exif_id != 0) {
        box = hdrg_bmff_open(&buf, "idat", 256, 0);

        if (gainmap) {
            hdrg_bmff_put(&buf, file->tmap, file->tmap_size);
        }

        if (apple) {
            hdrg_bmff_put(&buf, file->xmp, file->xmp_size);
        }

        if (exif_id != 0) {
            hdrg_bmff_u32(&buf, 0); /* exif_tiff_header_offset */
            hdrg_bmff_put(&buf, file->exif, file->exif_size);
        }

        hdrg_bmff_close(&buf, box);
    }

    if (gainmap) {
        box = hdrg_bmff_open(&buf, "grpl", 256, 0);
        sub = hdrg_bmff_open(&buf, "altr", 0, 0);
        hdrg_bmff_u32(&buf, group_id);
        hdrg_bmff_u32(&buf, 2);
        hdrg_bmff_u32(&buf, tmap_id);
        hdrg_bmff_u32(&buf, base_id);
        hdrg_bmff_close(&buf, sub);
        hdrg_bmff_close(&buf, box);
    }

    hdrg_bmff_close(&buf, meta);

    /* mdat: the images, where iloc says */
    mdat = hdrg_bmff_open(&buf, "mdat", 256, 0);
    hdrg_bmff_patch(&buf, base_loc, (uint32_t)buf.size);
    hdrg_bmff_put(&buf, file->base.item.data, file->base.item.size);

    if (gainmap) {
        hdrg_bmff_patch(&buf, gm_loc, (uint32_t)buf.size);
        hdrg_bmff_put(&buf, file->gainmap.item.data, file->gainmap.item.size);
    }

    hdrg_bmff_close(&buf, mdat);

    status = buf.failed ? HDRG_ERR_NOMEM : (buf.size > UINT32_MAX) ? HDRG_ERR_ENCODE : HDRG_OK;

    if (status == HDRG_OK) {
        status = api->write_output(writer, buf.data, buf.size);
    }

    api->mem_free(api, buf.data);

    return status;
}
