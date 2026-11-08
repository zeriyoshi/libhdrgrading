// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.webgpu.dering: hdrg.dering (src/plugins/dering/dering.c) on the GPU, on a
// whole RGB16 frame in two passes: the Y' range of each 8 x 8 block of the
// grid (contrasts: a thread a block, 0 below an edge's), then every sample of
// the blocks with an edge the mean of the samples of its window within its
// range of it (dering: a thread 2 pixels, every output word written). The
// integers are the CPU's and each output sample comes from the input alone:
// the output equals the CPU's.

struct Params {
    width: u32,
    height: u32,
    in_stride: u32,  // words a row of the input
    out_stride: u32, // ... and of the output
    blocks: u32,     // the block columns
    reach: u32,      // the window's radius
    floor: i32,      // the range's floor (the measured ringing)
    pad0: u32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read_write> contrast: array<i32>;
@group(0) @binding(3) var<storage, read_write> dst: array<u32>;

const GRID: u32 = 8u;         // the blocks' grid, from the frame's origin
const EDGE: i32 = 32 * 257;   // a block's Y' range from which it has an edge
const SHARE: i32 = 4;         // the range's ceiling: the block's Y' range over this
const SLOPE: i32 = 2;         // and its cap: this times the local slope
const STEEP: i32 = 5;         // a slope over the block's Y' range over this: on the edge itself
const KR: u32 = 13933u;       // BT.709 luma weights in 1/65536 for the decisions
const KG: u32 = 46871u;
const KB: u32 = 4732u;

// the samples of pixel x of row y
fn pixel(x: u32, y: u32) -> vec3i {
    let s = x * 3u;
    let at = y * params.in_stride + (s >> 1u);
    let w0 = source[at];
    let w1 = source[at + 1u];

    if ((s & 1u) == 0u) {
        return vec3i(i32(w0 & 0xffffu), i32(w0 >> 16u), i32(w1 & 0xffffu));
    }

    return vec3i(i32(w0 >> 16u), i32(w1 & 0xffffu), i32(w1 >> 16u));
}

fn luma(p: vec3i) -> i32 {
    return i32((u32(p.x) * KR + u32(p.y) * KG + u32(p.z) * KB + 32768u) >> 16u);
}

// the largest minmod of the differences to the previous and the next sample
// over the channels (a missing neighbor: the other side's difference)
fn slope_of(s: vec3i, prev: vec3i, next: vec3i, has_prev: bool, has_next: bool) -> i32 {
    var back = s - prev;
    var ahead = next - s;

    if (!has_prev) {
        back = ahead;
    }

    if (!has_next) {
        ahead = back;
    }

    let zero = vec3i(0);
    let rising = (back > zero) & (ahead > zero);
    let falling = (back < zero) & (ahead < zero);
    let g = select(select(zero, -max(back, ahead), falling), min(back, ahead), rising);

    return max(g.x, max(g.y, g.z));
}

// output pixel x of row y, as hdrg_dr_row has it
fn dering_pixel(x: u32, y: u32) -> vec3i {
    let w = params.width;
    let h = params.height;
    let s = pixel(x, y);
    let c = contrast[(y / GRID) * params.blocks + x / GRID];

    if (c == 0) {
        return s;
    }

    var left = s;
    var right = s;
    var up = s;
    var down = s;

    if (x > 0u) {
        left = pixel(x - 1u, y);
    }

    if (x + 1u < w) {
        right = pixel(x + 1u, y);
    }

    if (y > 0u) {
        up = pixel(x, y - 1u);
    }

    if (y + 1u < h) {
        down = pixel(x, y + 1u);
    }

    let slope = max(slope_of(s, left, right, x > 0u, x + 1u < w),
                    slope_of(s, up, down, y > 0u, y + 1u < h));
    var range = min(SLOPE * slope, c / SHARE);

    if (!(range > params.floor && slope * STEEP <= c)) {
        range = params.floor;
    }

    let span = vec3u(u32(2 * range));
    let reach = params.reach;
    let x0 = select(0u, x - reach, x > reach);
    let x1 = min(x + reach, w - 1u);
    let y0 = select(0u, y - reach, y > reach);
    let y1 = min(y + reach, h - 1u);
    var sum = vec3i(0);
    var count = 0u;

    for (var ny = y0; ny <= y1; ny++) {
        for (var nx = x0; nx <= x1; nx++) {
            let n = pixel(nx, ny);

            if (all(bitcast<vec3u>(n - s + vec3i(range)) <= span)) {
                sum += n;
                count++;
            }
        }
    }

    return (sum + vec3i(i32(count / 2u))) / vec3i(i32(count));
}

// the Y' range of the block id (0: below an edge's)
@compute @workgroup_size(8, 8)
fn contrasts(@builtin(global_invocation_id) id: vec3u) {
    let x0 = id.x * GRID;
    let y0 = id.y * GRID;

    if (x0 >= params.width || y0 >= params.height) {
        return;
    }

    let x1 = min(x0 + GRID, params.width);
    let y1 = min(y0 + GRID, params.height);
    var lo = 65536;
    var hi = -1;

    for (var y = y0; y < y1; y++) {
        for (var x = x0; x < x1; x++) {
            let v = luma(pixel(x, y));

            lo = min(lo, v);
            hi = max(hi, v);
        }
    }

    contrast[id.y * params.blocks + id.x] = select(hi - lo, 0, hi - lo < EDGE);
}

// the pixels id.x * 2 and the next of row id.y, in whole words (an odd
// width's last sample with the two bytes after it)
@compute @workgroup_size(8, 8)
fn dering(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;

    if (x >= params.width || id.y >= params.height) {
        return;
    }

    let a = dering_pixel(x, id.y);
    let base = id.y * params.out_stride + id.x * 3u;

    dst[base] = u32(a.x) | (u32(a.y) << 16u);

    if (x + 1u < params.width) {
        let b = dering_pixel(x + 1u, id.y);

        dst[base + 1u] = u32(a.z) | (u32(b.x) << 16u);
        dst[base + 2u] = u32(b.y) | (u32(b.z) << 16u);
    } else {
        dst[base + 1u] = u32(a.z);
    }
}
