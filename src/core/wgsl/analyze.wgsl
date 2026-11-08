// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// The analysis's sums on the GPU (hdrg_context_desc_t.gpu_analysis FAST):
// what src/core/analyze.c's pass A and src/core/plan.c's
// pass B add up over a picture of 16-bit R'G'B' rows (a session's frame, or
// its proxy: the pixel at 2^step_shift times the picture's plus offset,
// clamped), the CPU taking the rest (the black bars, the lights, the grid's
// blur). The same operations as the CPU's on each pixel in the same order;
// f32 where the CPU sums in f64, and a GPU may fuse a multiply and an add:
// the sums are within L2, a light near a threshold may judge
// otherwise. edges: a thread a row or a column, its largest max(R, G, B)
// (the bars: table lookups and maxima, exactly the CPU's). cells: a thread a
// cell, its La's sum and maximum, its brightest channel, the chroma of its
// brightest La and its Y's sum. hist: a workgroup ROWS rows of the bars'
// rectangle, La's histogram in workgroup memory then added (integer counts:
// any order). moments: a thread a plan cell, its moments and its boost grid.
// A var declared in a loop has an initializer (lavapipe).

const GROUP: u32 = 64u;
const BINS: u32 = 4096u;    // analyze.c's HDRG_HIST_BINS
const ROWS: u32 = 16u;      // hist: rows a workgroup
const MAXRGB_WEIGHT: f32 = 0.7;
const PLAN_BINS: u32 = 24u;
const PLAN_BIN_LOW: f32 = -10.0;
const PLAN_MOMENTS: u32 = 5u;
const SPAN_MIN: f32 = 0.05;
const MOMENT_STRIDE: u32 = 53u; // a plan cell's: 5 moments, 2 x PLAN_BINS grid values (2 spare)

const SQRT2: f32 = 0x1.6a09e6p+0f;
const LOG2_C9: f32 = 0x1.484b14p-2f;
const LOG2_C7: f32 = 0x1.a61762p-2f;
const LOG2_C5: f32 = 0x1.2776c6p-1f;
const LOG2_C3: f32 = 0x1.ec709ep-1f;
const LOG2_C1: f32 = 0x1.715476p+1f;

struct Params {
    width: u32,      // the picture's size
    height: u32,
    src_stride: u32, // words a row of the source
    step_shift: u32, // log2 of the source pixels a picture pixel stands for
    offset: u32,     // ... the one it is, within those
    src_width: u32,  // the source's size (the clamps)
    src_height: u32,
    entries: u32,    // the EOTF table's entries a channel
    cell_shift: u32, // cells: log2 of a cell's side (picture pixels)
    qw: u32,
    qh: u32,
    x0: u32,         // hist: the bars' rectangle
    x1: u32,
    y0: u32,
    y1: u32,
    cell: u32,       // moments: a plan cell's side, the plan's grid
    grid_w: u32,
    grid_h: u32,
    light_shift: u32, // log2 of the picture pixels an analysis cell spans
    light_w: u32,
    light_h: u32,
    has_light: u32,
    kr: f32,         // the source's luminance
    kg: f32,
    kb: f32,
    floor: f32,      // log2's floor (HDRG_LUMA_FLOOR)
    pad0: u32,
    pad1: u32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read> luts: array<f32>;
// cells: per cell the sum of La, its maximum, the brightest channel, the sum of Y, the chroma
// (planes of qw x qh in that order)
@group(0) @binding(3) var<storage, read_write> cells: array<f32>;
// the histogram's bins, the clip count, then the rows' and the columns' largest max(R, G, B)
@group(0) @binding(4) var<storage, read_write> counts: array<atomic<u32>>;
// the analysis's lights per cell: boost, lo, hi (planes of light_w x light_h)
@group(0) @binding(5) var<storage, read> lights: array<f32>;
@group(0) @binding(6) var<storage, read_write> moments: array<f32>;

var<workgroup> local: array<atomic<u32>, BINS>;

// hdrgrading/foundation/fmath.h's log2 (the CPU's boost kernel)
fn log2_of(x: f32) -> f32 {
    let bits = bitcast<u32>(x);
    var e = i32((bits >> 23u) & 0xffu) - 127;
    var m = bitcast<f32>((bits & 0x007fffffu) | 0x3f800000u);

    if (m > SQRT2) {
        m = m * 0.5;
        e = e + 1;
    }

    let s = (m - 1.0) / (m + 1.0);
    let s2 = s * s;
    var p = LOG2_C9;
    p = p * s2 + LOG2_C7;
    p = p * s2 + LOG2_C5;
    p = p * s2 + LOG2_C3;
    p = p * s2 + LOG2_C1;

    return f32(e) + s * p;
}

// linear R, G, B of picture pixel (x, y): its source pixel's codes through the EOTF tables
fn linear_at(x: u32, y: u32) -> vec3f {
    let sx = min((x << params.step_shift) + params.offset, params.src_width - 1u);
    let sy = min((y << params.step_shift) + params.offset, params.src_height - 1u);
    let i = sx * 3u;
    let row = sy * params.src_stride;
    var codes = vec3u(0u);

    for (var c = 0u; c < 3u; c = c + 1u) {
        let at = i + c;
        codes[c] = (source[row + at / 2u] >> (16u * (at % 2u))) & 0xffffu;
    }

    return vec3f(luts[codes.x], luts[params.entries + codes.y], luts[2u * params.entries + codes.z]);
}

fn luma_of(rgb: vec3f) -> f32 {
    return params.kr * rgb.x + params.kg * rgb.y + params.kb * rgb.z;
}

fn la_of(y: f32, rgb: vec3f) -> f32 {
    return max(y, max(rgb.x, max(rgb.y, rgb.z)) * MAXRGB_WEIGHT);
}

@compute @workgroup_size(64, 1, 1)
fn edges(@builtin(global_invocation_id) id: vec3u) {
    let n = id.x;
    var top = 0.0;

    if (n >= params.height + params.width) {
        return;
    }

    if (n < params.height) {
        for (var x = 0u; x < params.width; x = x + 1u) {
            let rgb = linear_at(x, n);

            top = max(top, max(rgb.x, max(rgb.y, rgb.z)));
        }
    } else {
        for (var y = 0u; y < params.height; y = y + 1u) {
            let rgb = linear_at(n - params.height, y);

            top = max(top, max(rgb.x, max(rgb.y, rgb.z)));
        }
    }

    atomicStore(&counts[BINS + n], bitcast<u32>(top));
}

@compute @workgroup_size(64, 1, 1)
fn cells_of(@builtin(global_invocation_id) id: vec3u) {
    let qx = id.x;
    let qy = id.y;
    let side = 1u << params.cell_shift;
    let total = params.qw * params.qh;

    if (qx >= params.qw || qy >= params.qh) {
        return;
    }

    var sum_la = 0.0;
    var top_la = 0.0;
    var top_peak = 0.0;
    var sum_y = 0.0;
    var chroma = 0.0;
    let x0 = qx * side;
    let y0 = qy * side;
    let x1 = min(x0 + side, params.width);
    let y1 = min(y0 + side, params.height);

    for (var y = y0; y < y1; y = y + 1u) {
        for (var x = x0; x < x1; x = x + 1u) {
            let rgb = linear_at(x, y);
            let lum = luma_of(rgb);
            let la = la_of(lum, rgb);
            let peak = max(rgb.x, max(rgb.y, rgb.z));

            sum_la = sum_la + la;

            if (la > top_la) {
                chroma = peak - min(rgb.x, min(rgb.y, rgb.z));
            }

            top_la = max(top_la, la);
            top_peak = max(top_peak, peak);
            sum_y = sum_y + lum;
        }
    }

    let n = f32((y1 - y0) * (x1 - x0));
    let at = qy * params.qw + qx;

    cells[at] = sum_la / n;
    cells[total + at] = top_la;
    cells[2u * total + at] = top_peak;
    cells[3u * total + at] = sum_y / n;
    cells[4u * total + at] = chroma;
}

@compute @workgroup_size(64, 1, 1)
fn hist(@builtin(local_invocation_id) lid: vec3u, @builtin(workgroup_id) wid: vec3u) {
    for (var b = lid.x; b < BINS; b = b + GROUP) {
        atomicStore(&local[b], 0u);
    }

    workgroupBarrier();

    let y0 = params.y0 + wid.y * ROWS;
    let y1 = min(y0 + ROWS, params.y1);

    for (var y = y0; y < y1; y = y + 1u) {
        for (var x = params.x0 + lid.x; x < params.x1; x = x + GROUP) {
            let rgb = linear_at(x, y);
            let la = la_of(luma_of(rgb), rgb);
            var v = 0.0;

            // NaN (a broken curve) counts as black: never an index out of the histogram
            if (la > 0.0) {
                v = min(la, 1.0);
            }

            atomicAdd(&local[u32(v * f32(BINS - 1u))], 1u);
        }
    }

    workgroupBarrier();

    for (var b = lid.x; b < BINS; b = b + GROUP) {
        let n = atomicLoad(&local[b]);

        if (n != 0u) {
            atomicAdd(&counts[b], n);
        }
    }
}

// plan.h's hdrg_plan_bin: the bin of La's log2 (its bits) below the place and the place past it
fn plan_bin(la: f32) -> vec2f {
    let v = clamp(la, 0.0009765625, 1.0);
    let bits = bitcast<u32>(v);
    let pos = (f32(i32(bits >> 23u) - 127) + f32(bits & 0x7fffffu) * 0x1p-23f - PLAN_BIN_LOW) *
              (f32(PLAN_BINS - 1u) / -PLAN_BIN_LOW);
    let i = min(u32(pos), PLAN_BINS - 2u);

    return vec2f(f32(i), pos - f32(i));
}

@compute @workgroup_size(64, 1, 1)
fn moments_of(@builtin(global_invocation_id) id: vec3u) {
    let cx = id.x;
    let cy = id.y;

    if (cx >= params.grid_w || cy >= params.grid_h) {
        return;
    }

    let light_cells = params.light_w * params.light_h;
    var m: array<f32, 5>;
    var g: array<f32, 48>;

    for (var k = 0u; k < 5u; k = k + 1u) {
        m[k] = 0.0;
    }

    for (var k = 0u; k < 48u; k = k + 1u) {
        g[k] = 0.0;
    }

    let x0 = cx * params.cell;
    let y0 = cy * params.cell;
    let x1 = min(x0 + params.cell, params.width);
    let y1 = min(y0 + params.cell, params.height);

    for (var y = y0; y < y1; y = y + 1u) {
        for (var x = x0; x < x1; x = x + 1u) {
            let rgb = linear_at(x, y);
            let lum = luma_of(rgb);
            let la = la_of(lum, rgb);
            var boost = 0.0;
            var lo = -1.0;
            var hi = 0.0;

            if (params.has_light == 1u) {
                let li = min(y >> params.light_shift, params.light_h - 1u) * params.light_w +
                         min(x >> params.light_shift, params.light_w - 1u);

                boost = lights[li];
                lo = lights[light_cells + li];
                hi = lights[2u * light_cells + li];
            }

            let l = log2_of(max(la, params.floor));
            let t = clamp((l - lo) / max(hi - lo, SPAN_MIN), 0.0, 1.0);
            let h = boost * (t * t * (3.0 - 2.0 * t));
            let bin = plan_bin(la);
            let b = u32(bin.x);

            m[0] = m[0] + 1.0;
            m[1] = m[1] + la;
            m[2] = m[2] + lum;
            m[3] = m[3] + lum * h;
            m[4] = m[4] + l;
            g[b * 2u] = g[b * 2u] + (1.0 - bin.y);
            g[b * 2u + 1u] = g[b * 2u + 1u] + (1.0 - bin.y) * h;
            g[b * 2u + 2u] = g[b * 2u + 2u] + bin.y;
            g[b * 2u + 3u] = g[b * 2u + 3u] + bin.y * h;
        }
    }

    let at = (cy * params.grid_w + cx) * MOMENT_STRIDE;

    for (var k = 0u; k < 5u; k = k + 1u) {
        moments[at + k] = m[k];
    }

    for (var k = 0u; k < 48u; k = k + 1u) {
        moments[at + PLAN_MOMENTS + k] = g[k];
    }
}
