/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * Two-layer gain-map JPEG serializer.
 *
 * Layout: [primary JPEG][gain map JPEG], linked by an MPF APP2 in the
 * primary. Segment order follows measured iPhone 17 Pro files:
 *   primary:  SOI APP0 [APP1 Exif] APP2(MPF) APP2(ISO version) [APP1 XMP
 *             GContainer] [APP2 ICC] <encoder tables/scan>
 *   gain map: SOI APP0 [APP1 XMP (Apple + hdrgm)] APP2(ISO metadata)
 *             <encoder tables/scan>
 * MP entry offsets are relative to the MP endian field and patched last.
 */
#include "jpeg_mux.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"

#define HDRG_ISO_URN     "urn:iso:std:iso:ts:21496:-1"
#define HDRG_ISO_URN_LEN 28u /* incl. NUL */
#define HDRG_XMP_SIG     "http://ns.adobe.com/xap/1.0/"
#define HDRG_XMP_SIG_LEN 29u    /* incl. NUL */
#define HDRG_MPF_PAYLOAD 86u    /* "MPF\0" + TIFF index for 2 images */
#define HDRG_SEGMENT_MAX 65533u /* an APPn's payload at most */

typedef struct {
    const hdrg_plugin_api_t *api;
    uint8_t *data;
    size_t size, capacity;
    bool failed;
} hdrg_buf_t;

/* value to 6 decimals whatever the locale (printf's %f follows LC_NUMERIC) */
static inline void hdrg_xmp_fixed(double value, char out[32])
{
    uint64_t scaled;
    bool negative;

    negative = value < 0.0;
    scaled = (uint64_t)((negative ? -value : value) * 1000000.0 + 0.5);
    snprintf(out, 32, "%s%" PRIu64 ".%06" PRIu64, negative ? "-" : "", scaled / 1000000u,
             scaled % 1000000u);
}

static void hdrg_buf_put(hdrg_buf_t *buf, const void *bytes, size_t count)
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
        capacity = (buf->capacity == 0) ? 65536 : buf->capacity;
        while (capacity < buf->size + count) {
            capacity *= 2;
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

static void hdrg_buf_u8(hdrg_buf_t *buf, uint8_t v)
{
    hdrg_buf_put(buf, &v, 1);
}

static void hdrg_buf_u16be(hdrg_buf_t *buf, uint16_t v)
{
    uint8_t bytes[2];

    bytes[0] = (uint8_t)(v >> 8);
    bytes[1] = (uint8_t)(v & 0xFF);

    hdrg_buf_put(buf, bytes, 2);
}

static void hdrg_buf_u32be(hdrg_buf_t *buf, uint32_t v)
{
    uint8_t bytes[4];

    bytes[0] = (uint8_t)(v >> 24);
    bytes[1] = (uint8_t)(v >> 16);
    bytes[2] = (uint8_t)(v >> 8);
    bytes[3] = (uint8_t)(v & 0xFF);

    hdrg_buf_put(buf, bytes, 4);
}

/* APPn segment: marker + length + payload */
static void hdrg_buf_segment(hdrg_buf_t *buf, uint8_t marker, const void *payload,
                             size_t payload_size)
{
    hdrg_buf_u8(buf, 0xFF);
    hdrg_buf_u8(buf, marker);
    hdrg_buf_u16be(buf, (uint16_t)(payload_size + 2));
    hdrg_buf_put(buf, payload, payload_size);
}

/* the Exif APP1: its "Exif\0\0" header and the TIFF (none past a segment's size) */
static void hdrg_buf_exif(hdrg_buf_t *buf, const uint8_t *tiff, size_t size)
{
    if (tiff == NULL || size == 0 || size + 6u > HDRG_SEGMENT_MAX) {
        return;
    }

    hdrg_buf_u8(buf, 0xFF);
    hdrg_buf_u8(buf, 0xE1);
    hdrg_buf_u16be(buf, (uint16_t)(size + 8u));
    hdrg_buf_put(buf, "Exif\0\0", 6);
    hdrg_buf_put(buf, tiff, size);
}

/* the ICC APP2 of a profile in one chunk (none past a segment's size) */
static void hdrg_buf_icc(hdrg_buf_t *buf, const uint8_t *icc, size_t size)
{
    if (icc == NULL || size == 0 || size + 14u > HDRG_SEGMENT_MAX) {
        return;
    }

    hdrg_buf_u8(buf, 0xFF);
    hdrg_buf_u8(buf, 0xE2);
    hdrg_buf_u16be(buf, (uint16_t)(size + 16u));
    hdrg_buf_put(buf, "ICC_PROFILE\0", 12);
    hdrg_buf_u8(buf, 1);
    hdrg_buf_u8(buf, 1);
    hdrg_buf_put(buf, icc, size);
}

/*
 * Splits a plain encoder-produced JPEG into: header segments we keep
 * (everything after SOI [+ APP0 if present]) and reports the APP0 payload.
 * Returns the offset where the retained remainder starts, and the APP0
 * segment bounds (0 length when absent).
 */
static bool hdrg_jpeg_head(const uint8_t *jpeg, size_t size, size_t *app0_off, size_t *app0_len,
                           size_t *rest_off)
{
    size_t seg_len;

    if (size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false;
    }

    *app0_off = 0;
    *app0_len = 0;
    *rest_off = 2;

    if (size >= 8 && jpeg[2] == 0xFF && jpeg[3] == 0xE0) {
        seg_len = ((size_t)jpeg[4] << 8) | jpeg[5];
        if (2 + 2 + seg_len <= size) {
            *app0_off = 2;
            *app0_len = 2 + seg_len;
            *rest_off = 2 + 2 + seg_len;
        }
    }

    return true;
}

/* XMP APP1 payload for the gain map stream: Apple HDRGainMap and/or hdrgm */
static void hdrg_buf_gainmap_xmp(hdrg_buf_t *payload, const hdrg_jpeg_mux_t *input)
{
    int n;
    char xml[2048], apple_block[640], hdrgm_block[768], values[4][32];

    apple_block[0] = '\0';
    hdrgm_block[0] = '\0';

    /* the version 2 block: the headroom as a ratio */
    if (input->apple_xmp) {
        hdrg_xmp_fixed((double)hdrg_math_exp2f((float)input->values[HDRG_GAINMAP_ALT_HEADROOM]),
                       values[0]);
        snprintf(apple_block, sizeof(apple_block),
                 "      <rdf:Description rdf:about=\"\"\n"
                 "            xmlns:apdi=\"http://ns.apple.com/pixeldatainfo/1.0/\"\n"
                 "            xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">\n"
                 "         <apdi:NativeFormat>1278226488</apdi:NativeFormat>\n"
                 "         <apdi:AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap"
                 "</apdi:AuxiliaryImageType>\n"
                 "         <apdi:StoredFormat>1278226488</apdi:StoredFormat>\n"
                 "         <HDRGainMap:HDRGainMapVersion>131072"
                 "</HDRGainMap:HDRGainMapVersion>\n"
                 "         <HDRGainMap:HDRGainMapHeadroom>%s"
                 "</HDRGainMap:HDRGainMapHeadroom>\n"
                 "      </rdf:Description>\n",
                 values[0]);
    }

    if (input->hdrgm_xmp) {
        hdrg_xmp_fixed((double)(float)input->values[HDRG_GAINMAP_MIN], values[0]);
        hdrg_xmp_fixed((double)(float)input->values[HDRG_GAINMAP_MAX], values[1]);
        hdrg_xmp_fixed((double)(float)input->values[HDRG_GAINMAP_GAMMA], values[2]);
        hdrg_xmp_fixed((double)(float)input->values[HDRG_GAINMAP_ALT_HEADROOM], values[3]);
        snprintf(hdrgm_block, sizeof(hdrgm_block),
                 "      <rdf:Description rdf:about=\"\"\n"
                 "            xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\"\n"
                 "         hdrgm:Version=\"1.0\"\n"
                 "         hdrgm:GainMapMin=\"%s\"\n"
                 "         hdrgm:GainMapMax=\"%s\"\n"
                 "         hdrgm:Gamma=\"%s\"\n"
                 "         hdrgm:OffsetSDR=\"0.00001\"\n"
                 "         hdrgm:OffsetHDR=\"0.00001\"\n"
                 "         hdrgm:HDRCapacityMin=\"0\"\n"
                 "         hdrgm:HDRCapacityMax=\"%s\"\n"
                 "         hdrgm:BaseRenditionIsHDR=\"False\"/>\n",
                 values[0], values[1], values[2], values[3]);
    }

    n = snprintf(xml, sizeof(xml),
                 "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"libhdrgrading\">\n"
                 "   <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                 "%s%s"
                 "   </rdf:RDF>\n"
                 "</x:xmpmeta>",
                 apple_block, hdrgm_block);

    hdrg_buf_put(payload, HDRG_XMP_SIG, HDRG_XMP_SIG_LEN);
    hdrg_buf_put(payload, xml, (size_t)n);
}

/* GContainer XMP for the primary (UltraHDR readers need the directory) */
static void hdrg_buf_primary_xmp(hdrg_buf_t *payload, size_t gainmap_size)
{
    int n;
    char xml[1024];

    n = snprintf(xml, sizeof(xml),
                 "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"libhdrgrading\">\n"
                 "   <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                 "      <rdf:Description rdf:about=\"\"\n"
                 "            xmlns:Container=\"http://ns.google.com/photos/1.0/container/\"\n"
                 "            xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\"\n"
                 "            xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\"\n"
                 "         hdrgm:Version=\"1.0\">\n"
                 "         <Container:Directory>\n"
                 "            <rdf:Seq>\n"
                 "               <rdf:li rdf:parseType=\"Resource\">\n"
                 "                  <Container:Item Item:Semantic=\"Primary\""
                 " Item:Mime=\"image/jpeg\"/>\n"
                 "               </rdf:li>\n"
                 "               <rdf:li rdf:parseType=\"Resource\">\n"
                 "                  <Container:Item Item:Semantic=\"GainMap\""
                 " Item:Mime=\"image/jpeg\" Item:Length=\"%zu\"/>\n"
                 "               </rdf:li>\n"
                 "            </rdf:Seq>\n"
                 "         </Container:Directory>\n"
                 "      </rdf:Description>\n"
                 "   </rdf:RDF>\n"
                 "</x:xmpmeta>",
                 gainmap_size);

    hdrg_buf_put(payload, HDRG_XMP_SIG, HDRG_XMP_SIG_LEN);
    hdrg_buf_put(payload, xml, (size_t)n);
}

/*
 * MPF APP2 (identifier "MPF\0", big-endian TIFF MP Index IFD, 2 images).
 * Entry sizes/offsets are patched after both streams are final; this writes
 * the fixed-size skeleton so the segment length never changes.
 */
static void hdrg_buf_mpf(hdrg_buf_t *buf)
{
    hdrg_buf_u8(buf, 0xFF);
    hdrg_buf_u8(buf, 0xE2);
    hdrg_buf_u16be(buf, HDRG_MPF_PAYLOAD + 2);
    hdrg_buf_put(buf, "MPF\0", 4);

    /* TIFF header (big endian), IFD at offset 8 */
    hdrg_buf_put(buf, "MM", 2);
    hdrg_buf_u16be(buf, 0x002A);
    hdrg_buf_u32be(buf, 8);

    /* MP Index IFD: 3 entries */
    hdrg_buf_u16be(buf, 3);
    hdrg_buf_u16be(buf, 0xB000); /* MPFVersion */
    hdrg_buf_u16be(buf, 7);      /* UNDEFINED */
    hdrg_buf_u32be(buf, 4);
    hdrg_buf_put(buf, "0100", 4);
    hdrg_buf_u16be(buf, 0xB001); /* NumberOfImages */
    hdrg_buf_u16be(buf, 4);      /* LONG */
    hdrg_buf_u32be(buf, 1);
    hdrg_buf_u32be(buf, 2);
    hdrg_buf_u16be(buf, 0xB002); /* MPEntry */
    hdrg_buf_u16be(buf, 7);      /* UNDEFINED */
    hdrg_buf_u32be(buf, 32);
    hdrg_buf_u32be(buf, 50); /* value offset (from endian field) */
    hdrg_buf_u32be(buf, 0);  /* next IFD */

    /* 2 x 16-byte MP entries, patched later */
    hdrg_buf_u32be(buf, 0x00030000); /* primary: baseline + representative */
    hdrg_buf_u32be(buf, 0);          /* size (patch) */
    hdrg_buf_u32be(buf, 0);          /* offset = 0 fixed */
    hdrg_buf_u16be(buf, 0);
    hdrg_buf_u16be(buf, 0);
    hdrg_buf_u32be(buf, 0x00000000); /* gain map */
    hdrg_buf_u32be(buf, 0);          /* size (patch) */
    hdrg_buf_u32be(buf, 0);          /* offset (patch) */
    hdrg_buf_u16be(buf, 0);
    hdrg_buf_u16be(buf, 0);
}

static void hdrg_patch_u32be(uint8_t *at, uint32_t v)
{
    at[0] = (uint8_t)(v >> 24);
    at[1] = (uint8_t)(v >> 16);
    at[2] = (uint8_t)(v >> 8);
    at[3] = (uint8_t)(v & 0xFF);
}

extern hdrg_status_t hdrg_jpeg_mux_gainmap(const hdrg_plugin_api_t *api, hdrg_writer_t *writer,
                                           const hdrg_jpeg_mux_t *input)
{
    hdrg_buf_t gm, primary, seg;
    hdrg_status_t status;
    uint8_t *entries;
    size_t app0_off, app0_len, rest_off, mpf_seg_off, mpf_base, primary_len;

    memset(&gm, 0, sizeof(gm));
    memset(&primary, 0, sizeof(primary));
    memset(&seg, 0, sizeof(seg));
    gm.api = api;
    primary.api = api;
    seg.api = api;

    /* ---- gain map stream: SOI APP0 [XMP] ISO <rest> ---- */
    if (!hdrg_jpeg_head(input->gainmap, input->gainmap_size, &app0_off, &app0_len, &rest_off)) {
        return HDRG_ERR_INTERNAL;
    }

    hdrg_buf_put(&gm, "\xFF\xD8", 2);
    hdrg_buf_put(&gm, input->gainmap + app0_off, app0_len);

    if (input->apple_xmp || input->hdrgm_xmp) {
        seg.size = 0;
        hdrg_buf_gainmap_xmp(&seg, input);
        if (!seg.failed) {
            hdrg_buf_segment(&gm, 0xE1, seg.data, seg.size);
        }
    }

    seg.size = 0;
    hdrg_buf_put(&seg, HDRG_ISO_URN, HDRG_ISO_URN_LEN);
    hdrg_buf_put(&seg, input->iso, input->iso_size);
    if (!seg.failed) {
        hdrg_buf_segment(&gm, 0xE2, seg.data, seg.size);
    }

    hdrg_buf_put(&gm, input->gainmap + rest_off, input->gainmap_size - rest_off);

    /* ---- primary stream: SOI APP0 [Exif] MPF ISOv [XMP] [ICC] <rest> ---- */
    if (!hdrg_jpeg_head(input->base, input->base_size, &app0_off, &app0_len, &rest_off)) {
        api->mem_free(api, gm.data);
        api->mem_free(api, seg.data);

        return HDRG_ERR_INTERNAL;
    }

    hdrg_buf_put(&primary, "\xFF\xD8", 2);
    hdrg_buf_put(&primary, input->base + app0_off, app0_len);
    hdrg_buf_exif(&primary, input->exif, input->exif_size);

    mpf_seg_off = primary.size;
    hdrg_buf_mpf(&primary);

    /* primary-side ISO segment: version signalling only */
    seg.size = 0;
    hdrg_buf_put(&seg, HDRG_ISO_URN, HDRG_ISO_URN_LEN);
    hdrg_buf_u16be(&seg, 0);
    hdrg_buf_u16be(&seg, 0);
    if (!seg.failed) {
        hdrg_buf_segment(&primary, 0xE2, seg.data, seg.size);
    }

    if (input->hdrgm_xmp) {
        seg.size = 0;
        hdrg_buf_primary_xmp(&seg, gm.size);
        if (!seg.failed) {
            hdrg_buf_segment(&primary, 0xE1, seg.data, seg.size);
        }
    }

    hdrg_buf_icc(&primary, input->icc, input->icc_size);
    hdrg_buf_put(&primary, input->base + rest_off, input->base_size - rest_off);

    status = (gm.failed || primary.failed || seg.failed) ? HDRG_ERR_NOMEM : HDRG_OK;

    if (status == HDRG_OK) {
        /* ---- patch MP entries (offsets relative to the MP endian field) ---- */
        primary_len = primary.size;
        mpf_base = mpf_seg_off + 2 + 2 + 4; /* marker + length + "MPF\0" */
        entries = primary.data + mpf_base + 50;

        hdrg_patch_u32be(entries + 4, (uint32_t)primary_len);
        hdrg_patch_u32be(entries + 16 + 4, (uint32_t)gm.size);
        hdrg_patch_u32be(entries + 16 + 8, (uint32_t)(primary_len - mpf_base));

        status = api->write_output(writer, primary.data, primary_len);
    }

    if (status == HDRG_OK) {
        status = api->write_output(writer, gm.data, gm.size);
    }

    api->mem_free(api, gm.data);
    api->mem_free(api, primary.data);
    api->mem_free(api, seg.data);

    return status;
}

extern hdrg_status_t hdrg_jpeg_mux_single(const hdrg_plugin_api_t *api, hdrg_writer_t *writer,
                                          const uint8_t *jpeg, size_t size, const uint8_t *icc,
                                          size_t icc_size, const uint8_t *exif, size_t exif_size)
{
    hdrg_buf_t buf;
    hdrg_status_t status;
    size_t app0_off, app0_len, rest_off;

    if (!hdrg_jpeg_head(jpeg, size, &app0_off, &app0_len, &rest_off)) {
        return HDRG_ERR_INTERNAL;
    }

    memset(&buf, 0, sizeof(buf));
    buf.api = api;

    /* SOI APP0 [ICC] [Exif] <rest>: the writer's own order */
    hdrg_buf_put(&buf, "\xFF\xD8", 2);
    hdrg_buf_put(&buf, jpeg + app0_off, app0_len);
    hdrg_buf_icc(&buf, icc, icc_size);
    hdrg_buf_exif(&buf, exif, exif_size);
    hdrg_buf_put(&buf, jpeg + rest_off, size - rest_off);

    status = buf.failed ? HDRG_ERR_NOMEM : api->write_output(writer, buf.data, buf.size);
    api->mem_free(api, buf.data);

    return status;
}
