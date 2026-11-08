// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.webgpu.lanczos: hdrg.scale.lanczos (src/plugins/polyphase/polyphase.c) on
// the GPU, a whole RGB16 frame resampled in two passes over the CPU's tables
// (hdrg_polyphase_axis: taps, Q14 weights, the samples around each): the
// vertical taps of each output row at the source width into mid (Q2, a
// thread a pixel), then the horizontal taps of mid into the output (a
// thread 2 pixels, every output word written), each with its anti-ringing
// when the axis upscales. The CPU's integers: the vertical sums stay within
// i32, the horizontal ones are split into the high and low bits of mid's
// values (WGSL has no i64) and put together exactly. The output equals the
// CPU's.

struct Params {
    in_w: u32,
    in_h: u32,
    out_w: u32,
    out_h: u32,
    in_stride: u32,   // words a row of the input
    out_stride: u32,  // ... and of the output
    row_taps: u32,
    col_taps: u32,
    rows_index: u32,  // where the rows' tables start in taps ...
    rows_weight: u32,
    rows_near: u32,
    cols_index: u32,  // ... and the columns'
    cols_weight: u32,
    cols_near: u32,
    antiring: i32,    // Q8
    flags: u32,       // 1: the rows upscale with anti-ringing, 2: the columns
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read> taps: array<i32>;
@group(0) @binding(3) var<storage, read_write> mid: array<i32>;
@group(0) @binding(4) var<storage, read_write> dst: array<u32>;

const SHIFT: u32 = 14u;  // the weights' Q
const EXTRA: u32 = 2u;   // mid's extra bits
const AR_SHIFT: u32 = 8u; // the anti-ringing strength's Q
const SPLIT: u32 = 9u;   // mid's values split at this bit for the horizontal sums

fn sample(x: u32, y: u32, c: u32) -> i32 {
    let s = x * 3u + c;
    let word = source[y * params.in_stride + (s >> 1u)];

    return i32((word >> ((s & 1u) * 16u)) & 0xffffu);
}

// v pulled toward [lo, hi] by the anti-ringing strength (hdrg_pp_antiring)
fn antiring(v: i32, lo: i32, hi: i32) -> i32 {
    let clamped = clamp(v, lo, hi);

    return v + (((clamped - v) * params.antiring + (1 << (AR_SHIFT - 1u))) >> AR_SHIFT);
}

// mid's row id.y (an output row) at pixel id.x of the source width
@compute @workgroup_size(8, 8)
fn vertical(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x;
    let y = id.y;

    if (x >= params.in_w || y >= params.out_h) {
        return;
    }

    let first = y * params.row_taps;
    let a = u32(taps[params.rows_near + y * 2u]);
    let b = u32(taps[params.rows_near + y * 2u + 1u]);

    for (var c = 0u; c < 3u; c++) {
        var acc = 0;

        for (var k = 0u; k < params.row_taps; k++) {
            acc += taps[params.rows_weight + first + k] *
                   sample(x, u32(taps[params.rows_index + first + k]), c);
        }

        acc = (acc + (1 << (SHIFT - EXTRA - 1u))) >> (SHIFT - EXTRA);

        if ((params.flags & 1u) != 0u) {
            let sa = sample(x, a, c);
            let sb = sample(x, b, c);

            acc = antiring(acc, min(sa, sb) << EXTRA, max(sa, sb) << EXTRA);
        }

        mid[(y * params.in_w + x) * 3u + c] = acc;
    }
}

// output pixel x of row y from mid
fn horizontal_pixel(x: u32, y: u32) -> vec3u {
    let first = x * params.col_taps;
    let row = y * params.in_w;
    let a = (row + u32(taps[params.cols_near + x * 2u])) * 3u;
    let b = (row + u32(taps[params.cols_near + x * 2u + 1u])) * 3u;
    var out = vec3u(0u);

    for (var c = 0u; c < 3u; c++) {
        var high = 0;
        var low = 0;

        // sum(w v) = sum(w (v >> SPLIT)) 2^SPLIT + sum(w (v & mask)), each within i32
        for (var k = 0u; k < params.col_taps; k++) {
            let w = taps[params.cols_weight + first + k];
            let v = mid[(row + u32(taps[params.cols_index + first + k])) * 3u + c];

            high += w * (v >> SPLIT);
            low += w * (v & ((1 << SPLIT) - 1));
        }

        // floor((high 2^SPLIT + low + 2^(SHIFT - 1)) / 2^SHIFT), high = q 2^(SHIFT - SPLIT) + r
        let q = high >> (SHIFT - SPLIT);
        let r = high - (q << (SHIFT - SPLIT));
        var acc = q + (((r << SPLIT) + low + (1 << (SHIFT - 1u))) >> SHIFT);

        if ((params.flags & 2u) != 0u) {
            let la = mid[a + c];
            let lb = mid[b + c];

            acc = antiring(acc, min(la, lb), max(la, lb));
        }

        acc = (acc + (1 << (EXTRA - 1u))) >> EXTRA;
        out[c] = u32(clamp(acc, 0, 65535));
    }

    return out;
}

// the output pixels id.x * 2 and the next of row id.y, in whole words (an
// odd width's last sample with the two bytes after it)
@compute @workgroup_size(8, 8)
fn horizontal(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;

    if (x >= params.out_w || id.y >= params.out_h) {
        return;
    }

    let p = horizontal_pixel(x, id.y);
    let base = id.y * params.out_stride + id.x * 3u;

    dst[base] = p.x | (p.y << 16u);

    if (x + 1u < params.out_w) {
        let n = horizontal_pixel(x + 1u, id.y);

        dst[base + 1u] = p.z | (n.x << 16u);
        dst[base + 2u] = n.y | (n.z << 16u);
    } else {
        dst[base + 1u] = p.z;
    }
}
