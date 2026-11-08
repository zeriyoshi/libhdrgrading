/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "motion.h"

/* the grid's cell of a place (source pixels of cell side): clamped into the grid */
static uint32_t hdrg_motion_cell(double place, double side, uint32_t cells)
{
    const double at = place / side;

    if (at <= 0.0) {
        return 0;
    }

    return (at >= (double)(cells - 1u)) ? cells - 1u : (uint32_t)at;
}

extern bool hdrg_motion_check(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    hdrg_sd_motion_t head;
    hdrg_sd_motion_field_t field;
    hdrg_sd_motion_block_t block;
    uint64_t blocks, k;
    uint32_t i;
    size_t at;

    if (data == NULL || size < sizeof(head)) {
        return false;
    }

    memcpy(&head, bytes, sizeof(head));

    if (head.fields == 0 || head.fields > HDRG_SD_MOTION_FIELDS || head.reserved != 0) {
        return false;
    }

    at = sizeof(head);

    for (i = 0; i < head.fields; i++) {
        if (size - at < sizeof(field)) {
            return false;
        }

        memcpy(&field, bytes + at, sizeof(field));
        at += sizeof(field);

        if (field.source < HDRG_MOTION_INTERNAL || field.source > HDRG_MOTION_HOST ||
            field.ref_offset == 0 || field.block_w == 0 || field.block_h == 0 ||
            field.block_w > HDRG_MOTION_BLOCK_MAX || field.block_h > HDRG_MOTION_BLOCK_MAX ||
            field.grid_w == 0 || field.grid_h == 0 ||
            (uint64_t)field.grid_w * field.grid_h > HDRG_MOTION_GRID_MAX || field.phase > 65536u ||
            field.signal < HDRG_MOTION_SIGNAL_LUMA || field.signal > HDRG_MOTION_SIGNAL_NONE ||
            field.shift > 8u || field.reserved != 0) {
            return false;
        }

        blocks = (uint64_t)field.grid_w * field.grid_h;

        if ((uint64_t)(size - at) < blocks * sizeof(block)) {
            return false;
        }

        for (k = 0; k < blocks; k++) {
            memcpy(&block, bytes + at + k * sizeof(block), sizeof(block));

            if (block.occlusion > HDRG_MOTION_UNCOVERED) {
                return false;
            }
        }

        at += (size_t)(blocks * sizeof(block));
    }

    return at == size;
}

extern bool hdrg_motion_field(const void *data, size_t size, uint32_t i,
                              hdrg_sd_motion_field_t *field, const uint8_t **blocks)
{
    const uint8_t *bytes = (const uint8_t *)data;
    hdrg_sd_motion_t head;
    uint32_t k;
    size_t at;

    if (data == NULL || size < sizeof(head)) {
        return false;
    }

    memcpy(&head, bytes, sizeof(head));

    if (i >= head.fields) {
        return false;
    }

    at = sizeof(head);

    for (k = 0; k <= i; k++) {
        memcpy(field, bytes + at, sizeof(*field));
        at += sizeof(*field);
        *blocks = bytes + at;
        at += (size_t)field->grid_w * field->grid_h * sizeof(hdrg_sd_motion_block_t);
    }

    return at <= size;
}

extern bool hdrg_motion_cap(const hdrg_plan_t *head, const float *a, const float *b,
                            const void *motion, size_t size, uint32_t width, uint32_t height,
                            float *capped)
{
    const uint8_t *blocks;
    const size_t cells = (size_t)head->grid_w * head->grid_h;
    hdrg_sd_motion_field_t field;
    hdrg_sd_motion_block_t block;
    uint32_t cx, cy, bx, by, at_a, at_b, bin;
    size_t cell;
    bool any;
    double side_x, side_y, x, y, phase, vx, vy;
    float weight, d, m;

    memcpy(capped, head->boost, cells * HDRG_PLAN_BINS * sizeof(float));

    if (!hdrg_motion_field(motion, size, 0, &field, &blocks) || field.ref_offset != 1 ||
        head->width == 0 || head->height == 0) {
        return false;
    }

    /* a cell's side in the frame's pixels (a proxy plan's cells are the frame's too) */
    side_x = (double)head->cell * (double)width / (double)head->width;
    side_y = (double)head->cell * (double)height / (double)head->height;
    phase = (double)field.phase / 65536.0;
    any = false;

    for (cy = 0; cy < head->grid_h; cy++) {
        for (cx = 0; cx < head->grid_w; cx++) {
            x = ((double)cx + 0.5) * side_x - (double)field.origin_x;
            y = ((double)cy + 0.5) * side_y - (double)field.origin_y;

            if (x < 0.0 || y < 0.0) {
                continue;
            }

            bx = (uint32_t)(x / (double)field.block_w);
            by = (uint32_t)(y / (double)field.block_h);

            if (bx >= field.grid_w || by >= field.grid_h) {
                continue;
            }

            memcpy(&block, blocks + ((size_t)by * field.grid_w + bx) * sizeof(block),
                   sizeof(block));

            if (block.confidence >= HDRG_MOTION_CAP_HI) {
                continue;
            }

            weight = (block.confidence <= HDRG_MOTION_CAP_LO)
                         ? 1.0f
                         : (float)(HDRG_MOTION_CAP_HI - block.confidence) /
                               (float)(HDRG_MOTION_CAP_HI - HDRG_MOTION_CAP_LO);
            x += (double)field.origin_x;
            y += (double)field.origin_y;
            vx = (double)block.mv_x / 4.0;
            vy = (double)block.mv_y / 4.0;
            at_a = hdrg_motion_cell(y - phase * vy, side_y, head->grid_h) * head->grid_w +
                   hdrg_motion_cell(x - phase * vx, side_x, head->grid_w);
            at_b = hdrg_motion_cell(y + (1.0 - phase) * vy, side_y, head->grid_h) * head->grid_w +
                   hdrg_motion_cell(x + (1.0 - phase) * vx, side_x, head->grid_w);
            cell = (size_t)cy * head->grid_w + cx;

            for (bin = 0; bin < HDRG_PLAN_BINS; bin++) {
                d = capped[cell * HDRG_PLAN_BINS + bin];
                m = (a == NULL || b == NULL) ? 0.0f
                                             : HDRG_MIN(a[(size_t)at_a * HDRG_PLAN_BINS + bin],
                                                        b[(size_t)at_b * HDRG_PLAN_BINS + bin]);

                if (d > m) {
                    capped[cell * HDRG_PLAN_BINS + bin] = d - weight * (d - m);
                    any = true;
                }
            }
        }
    }

    return any;
}
