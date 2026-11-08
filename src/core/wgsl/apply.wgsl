// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// The apply pass (pass C, src/core/apply.c) on the GPU for a session's
// single-layer PQ output: the same stages as the CPU's
// scalar kernels (src/kernels/k_scalar.c) on a whole frame, in the same
// order of operations: code values -> linear light (the source's EOTF
// table), La, the coring toward its 3 x 3 mean, the plan's boost share
// sliced from its bilateral grid and the clarity base, the grading ratio
// (hdrgrading/foundation/fmath.h's polynomials), the color LUT (tetrahedral), the
// highlight desaturation and the shoulder, PQ (the table of
// src/kernels/pq_table.c), the dither (hdrgrading/foundation/hash.h) and the packing
// (apply: a thread 2 pixels of 16-bit output, 4 of 8-bit, every output word
// written). Each row's largest max(R, G, B) and its workgroups' sums go to
// light (MaxCLL / MaxFALL). yuv: the 16-bit output to 4:2:0 Y'CbCr as
// src/core/yuv.c's hdrg_rgb16_to_yuv420 (a thread a word of a chroma plane:
// those samples and the 2 x 2 luma words over them). A GPU may fuse a
// multiply and an add: the output is within L2 of the CPU's.

struct Params {
    width: u32,
    height: u32,
    src_stride: u32,  // bytes a row of the source
    out_stride: u32,  // words a row of dst
    comps: u32,       // the source's samples a pixel (3, or 4 with alpha)
    wide: u32,        // 1: 16-bit source samples
    entries: u32,     // the source EOTF table's entries a channel
    pack: u32,        // the pixels of a thread: 2 (16-bit output) or 4 (8-bit)
    grid_w: u32,
    grid_h: u32,
    flags: u32,       // FLAG_*
    groups_x: u32,    // apply's workgroups a row (light's partial sums)
    lut_at: u32,      // luts: where the color LUT's nodes and the PQ table start
    pq_at: u32,
    rows_at: u32,     // frame: where the rows' cells and weights start (the columns' are first),
    boost_at: u32,    // the boost, its near maxima, the clarity base and the coring table
    near_at: u32,
    base_at: u32,
    coring_at: u32,
    max_code: f32,    // the color LUT's code range
    diffuse: f32,     // the render's gain: diffuse + boost share x span, stops
    span: f32,
    pivot: f32,       // the grade (hdrg_grade_args_t)
    floor: f32,
    contrast: f32,
    clarity: f32,
    src_kr: f32,      // the source's luminance
    src_kg: f32,
    src_kb: f32,
    out_kr: f32,      // the rendition gamut's (hdrg_shoulder_args_t)
    out_kg: f32,
    out_kb: f32,
    desat: f32,
    knee: f32,
    range: f32,
    white: f32,
    fixed_lo: f32,    // a fixed plan's ramp (hdrg_plan_fixed_ramp)
    fixed_hi: f32,
    yuv_bytes: u32,   // yuv: 1 or 2 bytes a sample
    y_stride: u32,    // yuv: words a row of the Y' plane, of a chroma plane
    c_stride: u32,
    cw: u32,          // yuv: the chroma planes' size
    ch: u32,
    cb_at: u32,       // yuv: planes' words where Cb and Cr start
    cr_at: u32,
    kr: f32,          // yuv: the matrix
    kg: f32,
    kb: f32,
    inv_cb: f32,
    inv_cr: f32,
    y_off: f32,       // yuv: the range
    y_range: f32,
    c_off: f32,
    c_range: f32,
    top: f32,
    pairs: u32,       // yuv: 1: Cb, Cr alternating in the plane at cb_at (cr_at unused)
    shift: u32,       // yuv: the codes' shift to the samples' high bits (P010: 6)
    grain_at: u32,    // frame: where the grain starts (FLAG_GRAIN, hdrg_grain_words)
    light_at: u32,    // frame: the light's ratio matrix and SDR white (FLAG_LIGHT)
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
// the source's EOTF tables (3 x entries), the color LUT's nodes (33^3 x 3), the PQ table
@group(0) @binding(2) var<storage, read> luts: array<f32>;
// per frame: the columns' cells, their weights (f32 bits), the rows' first cells, next cells and
// weights, then the boost (cells x BINS), its near maxima, the clarity base, the coring table
@group(0) @binding(3) var<storage, read> frame: array<u32>;
@group(0) @binding(4) var<storage, read_write> dst: array<u32>;
// the rows' largest max(R, G, B) nits (f32 bits, atomicMax), then each row's workgroups' sums,
// then the frame's light (hdrg_lightstat_t: R, G, B's largest, the bins' counts, samples, ratios,
// places)
@group(0) @binding(5) var<storage, read_write> light: array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> planes: array<u32>;

const FLAG_CORED: u32 = 1u;
const FLAG_FIXED: u32 = 2u;
const FLAG_BOOST: u32 = 4u;
const FLAG_BASE: u32 = 8u;
const FLAG_DITHER: u32 = 16u;
const FLAG_LIGHT: u32 = 32u;
const FLAG_GRAIN: u32 = 64u;

const GROUP: u32 = 64u;
const LUT_N: u32 = 33u;
const BINS: u32 = 24u;
const CORING: u32 = 33u;
const MAXRGB_WEIGHT: f32 = 0.7;
const LUMA_FLOOR: f32 = 6.1035e-5;
const LN2: f32 = 0.69314718;
const CLARITY_RANGE: f32 = 1.5;
const BOOST_SPAN_MIN: f32 = 0.05;

const SQRT2: f32 = 0x1.6a09e6p+0f;
const LOG2E: f32 = 0x1.715476p+0f;
const LOG2_C9: f32 = 0x1.484b14p-2f;
const LOG2_C7: f32 = 0x1.a61762p-2f;
const LOG2_C5: f32 = 0x1.2776c6p-1f;
const LOG2_C3: f32 = 0x1.ec709ep-1f;
const LOG2_C1: f32 = 0x1.715476p+1f;
const EXP2_C7: f32 = 0x1.ffcbfcp-17f;
const EXP2_C6: f32 = 0x1.430912p-13f;
const EXP2_C5: f32 = 0x1.5d87fep-10f;
const EXP2_C4: f32 = 0x1.3b2ab6p-7f;
const EXP2_C3: f32 = 0x1.c6b08ep-5f;
const EXP2_C2: f32 = 0x1.ebfbe0p-3f;
const EXP2_C1: f32 = 0x1.62e430p-1f;

const PQ_LOW: f32 = 0x1p-45f;
const PQ_SHIFT: u32 = 17u;
const PQ_BASE: u32 = 5248u;
const PQ_MASK: u32 = 0x1ffffu;
const PQ_FRAC: f32 = 0x1p-17f;
const PQ_INV_PEAK: f32 = 1.0e-4;

const HASH_IV: u32 = 0x6a09e667u;
const HASH_W_KEY0: u32 = 0x9e3779b1u;
const HASH_W_Y: u32 = 0x85ebca77u;
const HASH_W_KEY1: u32 = 0xc2b2ae3du;
const HASH_W_X: u32 = 0x27d4eb2fu;
const HASH_W_S: u32 = 0x165667b1u;
const KEY_LO: u32 = 0x5f585047u; // HDRG_KEY_RENDER
const LIGHT_BINS: u32 = 160u; // hdrg_lightstat_t's (src/core/apply.h)
const LIGHT_LOW: i32 = -6;
const LIGHT_STEP: u32 = 4u;
const LIGHT_SPAN: i32 = 512;
const GRAIN_KEY_HI: u32 = 0x4752414eu; // HDRG_GRAIN_KEY_HI
const GRAIN_TAPS: u32 = 7u;
const GRAIN_RADIUS: u32 = 3u;
const GRAIN_LEVELS: u32 = 256u;
const KEY_HI: u32 = 0x48445247u;

const INV_CODE16: f32 = 1.0 / 65535.0;

var<workgroup> partial: array<f32, GROUP>;

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

fn exp2_of(v: f32) -> f32 {
    let x = clamp(v, -126.0, 127.0);
    let n = round(x);
    let f = x - n;
    var p = EXP2_C7;
    p = p * f + EXP2_C6;
    p = p * f + EXP2_C5;
    p = p * f + EXP2_C4;
    p = p * f + EXP2_C3;
    p = p * f + EXP2_C2;
    p = p * f + EXP2_C1;
    p = p * f + 1.0;

    return p * bitcast<f32>(u32(i32(n) + 127) << 23u);
}

fn exp_of(x: f32) -> f32 {
    return exp2_of(x * LOG2E);
}

fn mix32(v: u32) -> u32 {
    var h = v;
    h = h ^ (h >> 16u);
    h = h * 0x7feb352du;
    h = h ^ (h >> 15u);
    h = h * 0x846ca68bu;
    h = h ^ (h >> 16u);

    return h;
}

fn hash_row(y: u32) -> u32 {
    var h = mix32(HASH_IV ^ (KEY_LO * HASH_W_KEY0));
    h = mix32(h ^ (y * HASH_W_Y));

    return mix32(h ^ (KEY_HI * HASH_W_KEY1));
}

fn hash_tri(a: u32, b: u32) -> f32 {
    return (f32(a >> 8u) - f32(b >> 8u)) * 0x1p-24f;
}

// sample c of pixel (x, y) of the source
fn code_at(x: u32, y: u32, c: u32) -> u32 {
    if (params.wide != 0u) {
        let at = y * params.src_stride + (x * params.comps + c) * 2u;

        return (source[at >> 2u] >> ((at & 2u) * 8u)) & 0xffffu;
    }

    let at = y * params.src_stride + x * params.comps + c;

    return (source[at >> 2u] >> ((at & 3u) * 8u)) & 0xffu;
}

fn codes_at(x: u32, y: u32) -> vec3u {
    return vec3u(code_at(x, y, 0u), code_at(x, y, 1u), code_at(x, y, 2u));
}

fn linear_of(codes: vec3u) -> vec3f {
    let n = params.entries;

    return vec3f(luts[codes.x], luts[n + codes.y], luts[2u * n + codes.z]);
}

fn largest_of(v: vec3f) -> f32 {
    return max(v.x, max(v.y, v.z));
}

// La = max(Y, 0.7 * max(R, G, B))
fn la_of(rgb: vec3f) -> f32 {
    let y = params.src_kr * rgb.x + params.src_kg * rgb.y + params.src_kb * rgb.z;

    return max(y, largest_of(rgb) * MAXRGB_WEIGHT);
}

fn frame_f(i: u32) -> f32 {
    return bitcast<f32>(frame[i]);
}

// hdrg_plan_coring
fn coring_of(la: f32) -> f32 {
    let pos = clamp(la, 0.0, 1.0) * f32(CORING - 1u);
    let i = min(u32(pos), CORING - 2u);
    let t = pos - f32(i);

    return frame_f(params.coring_at + i) * (1.0 - t) + frame_f(params.coring_at + i + 1u) * t;
}

// hdrg_plan_bin: the lower bin of La on the log2 axis and the weight of the next
fn bin_of(la: f32) -> vec2f {
    let v = clamp(la, 0.0009765625, 1.0);
    let bits = bitcast<u32>(v);
    let pos = (f32(i32(bits >> 23u) - 127) + f32(bits & 0x7fffffu) * 0x1p-23f + 10.0) *
              (23.0 / 10.0);
    let i = min(u32(pos), BINS - 2u);

    return vec2f(f32(i), pos - f32(i));
}

struct Pos {
    x0: u32,
    x1: u32,
    y0: u32,
    y1: u32,
    tx: f32,
    ty: f32,
}

fn pos_at(x: u32, y: u32) -> Pos {
    let at = params.rows_at;
    var p: Pos;

    p.x0 = frame[x];
    p.x1 = min(p.x0 + 1u, params.grid_w - 1u);
    p.tx = frame_f(params.width + x);
    p.y0 = frame[at + y];
    p.y1 = frame[at + params.height + y];
    p.ty = frame_f(at + 2u * params.height + y);

    return p;
}

fn cell(cx: u32, cy: u32) -> u32 {
    return cy * params.grid_w + cx;
}

// hdrg_boost_at
fn boost_at(p: Pos, la: f32) -> f32 {
    let bt = bin_of(la);
    let b = params.boost_at + u32(bt.x);
    let c00 = cell(p.x0, p.y0) * BINS + b;
    let c01 = cell(p.x1, p.y0) * BINS + b;
    let c10 = cell(p.x0, p.y1) * BINS + b;
    let c11 = cell(p.x1, p.y1) * BINS + b;
    let lo = (frame_f(c00) * (1.0 - p.tx) + frame_f(c01) * p.tx) * (1.0 - p.ty) +
             (frame_f(c10) * (1.0 - p.tx) + frame_f(c11) * p.tx) * p.ty;
    let hi = (frame_f(c00 + 1u) * (1.0 - p.tx) + frame_f(c01 + 1u) * p.tx) * (1.0 - p.ty) +
             (frame_f(c10 + 1u) * (1.0 - p.tx) + frame_f(c11 + 1u) * p.tx) * p.ty;

    return lo + (hi - lo) * bt.y;
}

// hdrg_grid_lerp of the clarity base
fn base_at(p: Pos) -> f32 {
    let at = params.base_at;

    return (frame_f(at + cell(p.x0, p.y0)) * (1.0 - p.tx) +
            frame_f(at + cell(p.x1, p.y0)) * p.tx) *
               (1.0 - p.ty) +
           (frame_f(at + cell(p.x0, p.y1)) * (1.0 - p.tx) +
            frame_f(at + cell(p.x1, p.y1)) * p.tx) *
               p.ty;
}

// La and max(R, G, B) of pixel (x, y)
fn la_peak(x: u32, y: u32) -> vec2f {
    let rgb = linear_of(codes_at(x, y));

    return vec2f(la_of(rgb), largest_of(rgb));
}

// hdrg_lut_rgb: the color LUT at the codes (tetrahedral), then each channel through the EOTF
fn lut_rgb(codes: vec3u) -> vec3f {
    let n = LUT_N;
    let sr = n * n * 3u;
    let sg = n * 3u;
    let sb = 3u;
    var fr = f32(codes.x * (n - 1u)) / params.max_code;
    var fg = f32(codes.y * (n - 1u)) / params.max_code;
    var fb = f32(codes.z * (n - 1u)) / params.max_code;
    let ir = min(u32(fr), n - 2u);
    let ig = min(u32(fg), n - 2u);
    let ib = min(u32(fb), n - 2u);
    var p1: u32;
    var p2: u32;
    var w0: f32;
    var w1: f32;
    var w2: f32;
    var w3: f32;

    fr = fr - f32(ir);
    fg = fg - f32(ig);
    fb = fb - f32(ib);

    let p0 = params.lut_at + ir * sr + ig * sg + ib * sb;
    let p3 = p0 + sr + sg + sb;

    if (fr >= fg) {
        if (fg >= fb) {
            p1 = p0 + sr;
            p2 = p1 + sg;
            w0 = 1.0 - fr;
            w1 = fr - fg;
            w2 = fg - fb;
            w3 = fb;
        } else if (fr >= fb) {
            p1 = p0 + sr;
            p2 = p1 + sb;
            w0 = 1.0 - fr;
            w1 = fr - fb;
            w2 = fb - fg;
            w3 = fg;
        } else {
            p1 = p0 + sb;
            p2 = p1 + sr;
            w0 = 1.0 - fb;
            w1 = fb - fr;
            w2 = fr - fg;
            w3 = fg;
        }
    } else if (fb >= fg) {
        p1 = p0 + sb;
        p2 = p1 + sg;
        w0 = 1.0 - fb;
        w1 = fb - fg;
        w2 = fg - fr;
        w3 = fr;
    } else if (fb >= fr) {
        p1 = p0 + sg;
        p2 = p1 + sb;
        w0 = 1.0 - fg;
        w1 = fg - fb;
        w2 = fb - fr;
        w3 = fr;
    } else {
        p1 = p0 + sg;
        p2 = p1 + sr;
        w0 = 1.0 - fg;
        w1 = fg - fr;
        w2 = fr - fb;
        w3 = fb;
    }

    var out: vec3f;

    for (var c = 0u; c < 3u; c = c + 1u) {
        var v = w0 * luts[p0 + c] + w1 * luts[p1 + c] + w2 * luts[p2 + c] + w3 * luts[p3 + c];
        let i = min(u32(v), params.entries - 2u);
        let t = c * params.entries;

        v = v - f32(i);
        out[c] = luts[t + i] + v * (luts[t + i + 1u] - luts[t + i]);
    }

    return out;
}

// hdrg_pq_signal
fn pq_signal(nits: f32) -> f32 {
    var y = nits * PQ_INV_PEAK;

    y = select(y, 1.0, y > 1.0);
    y = select(PQ_LOW, y, y >= PQ_LOW);

    let bits = bitcast<u32>(y);
    let k = params.pq_at + (bits >> PQ_SHIFT) - PQ_BASE;
    let t = f32(bits & PQ_MASK) * PQ_FRAC;

    return luts[k] + t * (luts[k + 1u] - luts[k]);
}

// the shoulder's smoothstep desaturation of the highlights
fn desaturate(f: f32, v: vec3f) -> vec3f {
    var t = min(f, 1.0);

    t = t * t * (3.0 - 2.0 * t);

    let scale = 1.0 - params.desat * t;
    let luma = params.out_kr * v.x + params.out_kg * v.y + params.out_kb * v.z;

    return vec3f(max(luma + (v.x - luma) * scale, 0.0), max(luma + (v.y - luma) * scale, 0.0),
                 max(luma + (v.z - luma) * scale, 0.0));
}

// pixel (x, y) through the pass: its output nits
fn render(x: u32, y: u32) -> vec3f {
    let codes = codes_at(x, y);
    let rgb = linear_of(codes);
    let luma = la_of(rgb);
    var la = luma;
    var base = 0.0;
    var guide = 1.0;
    var gain = params.diffuse;

    if ((params.flags & FLAG_FIXED) != 0u) {
        let l = log2_of(max(luma, params.floor));
        let t = (l - params.fixed_lo) / max(params.fixed_hi - params.fixed_lo, BOOST_SPAN_MIN);
        let s = clamp(t, 0.0, 1.0);

        gain = params.diffuse + s * s * (3.0 - 2.0 * s) * params.span;
    } else {
        let p = pos_at(x, y);

        if ((params.flags & FLAG_BASE) != 0u) {
            base = base_at(p);
        }

        // the coring: La toward its 3 x 3 mean (edges repeated), the desaturation's guide
        if ((params.flags & FLAG_CORED) != 0u) {
            let xl = select(x, x - 1u, x > 0u);
            let xr = min(x + 1u, params.width - 1u);
            let ya = select(y, y - 1u, y > 0u);
            let yb = select(y, y + 1u, y + 1u < params.height);
            let l0 = la_peak(xl, ya) + la_peak(xl, y) + la_peak(xl, yb);
            let l1 = la_peak(x, ya) + vec2f(la, largest_of(rgb)) + la_peak(x, yb);
            let l2 = la_peak(xr, ya) + la_peak(xr, y) + la_peak(xr, yb);
            let sums = (l0 + l1 + l2) / 9.0;
            let m = sums.x;
            let d = la - m;
            let s = d * d + coring_of(m);
            let cored = select(la, m + d * (d * d / s), s > 0.0);
            let peak = largest_of(rgb);

            base = base + (la - cored) / (max(cored, LUMA_FLOOR) * LN2);
            la = cored;
            guide = select(1.0, sums.y / peak, peak > 0.0);
        }

        if ((params.flags & FLAG_BOOST) != 0u && frame_f(params.near_at + cell(p.x0, p.y0)) > 0.0) {
            gain = gain + boost_at(p, la) * params.span;
        }
    }

    // the grade (hdrg_scalar_grade): log2 luminance, S-slope, clarity, gain, shadow floor
    let t = log2_of(max(luma, params.floor));
    var graded = params.pivot + (t - params.pivot) * params.contrast + gain;

    if ((params.flags & FLAG_BASE) != 0u) {
        let d = t - base;

        graded = graded + params.clarity * (d / (1.0 + abs(d) / CLARITY_RANGE));
    }

    graded = max(graded, t);

    // the color LUT x the ratio, then the shoulder (hdrg_scalar_shoulder)
    var v = lut_rgb(codes) * exp2_of(graded - t);
    var peak = largest_of(v) * params.white;
    let decided = peak * guide;

    if (params.desat > 0.0 && decided > params.knee) {
        v = desaturate((decided - params.knee) / params.range, v);
        peak = largest_of(v) * params.white;
    }

    if (peak > params.knee) {
        let scale = (params.knee + params.range * (1.0 - exp_of(-(peak - params.knee) / params.range))) /
                    peak;

        v = v * scale;
    }

    return v * params.white;
}

// the codes of output pixel x of row y: 16-bit PQ, or 8-bit PQ with the dither
fn pack_codes(nits: vec3f, x: u32, row_hash: u32) -> vec3u {
    var out: vec3u;

    if (params.pack == 2u) {
        for (var c = 0u; c < 3u; c = c + 1u) {
            out[c] = u32(clamp(i32(round(pq_signal(nits[c]) * 65535.0)), 0, 65535));
        }

        return out;
    }

    let pixel = mix32(row_hash ^ (x * HASH_W_X));

    for (var c = 0u; c < 3u; c = c + 1u) {
        var code = pq_signal(nits[c]) * 255.0;

        if ((params.flags & FLAG_DITHER) != 0u) {
            code = code + hash_tri(mix32(pixel ^ ((c * 2u) * HASH_W_S)),
                                   mix32(pixel ^ ((c * 2u + 1u) * HASH_W_S)));
        }

        out[c] = u32(clamp(i32(round(code)), 0, 255));
    }

    return out;
}

// the grain's (src/core/grain.c, hdrg_grain_words at grain_at): plane p's row hash of row r
fn grain_row(p: u32, r: u32) -> u32 {
    var h = mix32(HASH_IV ^ (frame[params.grain_at + p] * HASH_W_KEY0));
    h = mix32(h ^ (r * HASH_W_Y));

    return mix32(h ^ (GRAIN_KEY_HI * HASH_W_KEY1));
}

// a white sample of mean 0: the sum of the hash's four bytes
fn grain_white(h: u32) -> i32 {
    return i32((h & 0xffu) + ((h >> 8u) & 0xffu) + ((h >> 16u) & 0xffu) + (h >> 24u)) - 510;
}

fn grain_tap(t: u32) -> i32 {
    return bitcast<i32>(frame[params.grain_at + 6u + t]);
}

// plane p's filtered noise at pixel (x, y): across (>> 4), then down (>> 8), by the floor
fn grain_unit(p: u32, x: u32, y: u32) -> i32 {
    var v = 0;

    for (var t = 0u; t < GRAIN_TAPS; t = t + 1u) {
        let row = grain_row(p, y + t - GRAIN_RADIUS);
        var sum = 0;

        for (var i = 0u; i < GRAIN_TAPS; i = i + 1u) {
            sum = sum + grain_tap(i) * grain_white(mix32(row ^ ((x + i - GRAIN_RADIUS) * HASH_W_X)));
        }

        v = v + grain_tap(t) * ((sum + 8) >> 4u);
    }

    return (v + 128) >> 8u;
}

// the codes of pixel (x, y) with the grain at the source's level (16-bit, or 8-bit: pack 4)
fn grain_codes(codes: vec3u, x: u32, y: u32) -> vec3u {
    let source = codes_at(x, y);
    let k = vec3u(frame[params.grain_at + 3u], frame[params.grain_at + 4u],
                  frame[params.grain_at + 5u]);
    let luma = (k.x * source.x + k.y * source.y + k.z * source.z + 16384u) >> 15u;
    let level = min(select(luma, (luma + 128u) / 257u, params.wide != 0u), 255u);
    let scales = params.grain_at + 6u + GRAIN_TAPS + level;
    var g: vec3i;
    var out: vec3u;

    for (var p = 0u; p < 3u; p = p + 1u) {
        let scale = bitcast<i32>(frame[scales + p * GRAIN_LEVELS]);

        g[p] = (grain_unit(p, x, y) * scale + 2048) >> 12u;
    }

    let d = vec3i(g.x + ((6040 * g.z + 2048) >> 12u),
                  g.x - ((674 * g.y + 2340 * g.z + 2048) >> 12u),
                  g.x + ((7706 * g.y + 2048) >> 12u));

    for (var c = 0u; c < 3u; c = c + 1u) {
        if (params.pack == 2u) {
            out[c] = u32(clamp(i32(codes[c]) + d[c], 0, 65535));
        } else {
            out[c] = u32((clamp(i32(codes[c]) * 257 + d[c], 0, 65535) + 128) / 257);
        }
    }

    return out;
}

// hdrg_lightstat_bin: max(R, G, B) cd/m^2 by its float's exponent and top 3 mantissa bits
fn light_bin(m: f32) -> u32 {
    if (!(m >= 0.015625)) {
        return 0u;
    }

    let bits = bitcast<u32>(m);

    return min((((bits >> 23u) & 0xffu) - u32(127 + LIGHT_LOW)) * 8u + ((bits >> 20u) & 7u),
               LIGHT_BINS - 1u);
}

// pixel (x, y)'s light (nits, its max(R, G, B) m) into the frame's: the channels' largest, and on
// the sampled pixels the bin and the log2 of the SDR's max(R, G, B) over m (Q6 + 8 stops)
fn light_add(nits: vec3f, m: f32, x: u32, y: u32) {
    let at = params.height * (1u + params.groups_x);
    let bin = light_bin(m);

    for (var c = 0u; c < 3u; c = c + 1u) {
        atomicMax(&light[at + c], bitcast<u32>(max(nits[c], 0.0)));
    }

    if (y % LIGHT_STEP != 0u || x % LIGHT_STEP != 0u) {
        return;
    }

    atomicAdd(&light[at + 3u + bin], 1u);

    if (!(m > 0.0)) {
        return;
    }

    let lin = linear_of(codes_at(x, y));
    let k = params.light_at;
    var s = 0.0;

    for (var c = 0u; c < 3u; c = c + 1u) {
        s = max(s, bitcast<f32>(frame[k + c * 3u]) * lin.x +
                   bitcast<f32>(frame[k + c * 3u + 1u]) * lin.y +
                   bitcast<f32>(frame[k + c * 3u + 2u]) * lin.z);
    }

    if (!(s > 0.0)) {
        return;
    }

    let q = clamp(i32(round(log2(s * bitcast<f32>(frame[k + 9u]) / m) * 64.0)), -LIGHT_SPAN,
                  LIGHT_SPAN - 1);

    atomicAdd(&light[at + 3u + LIGHT_BINS + bin], 1u);
    atomicAdd(&light[at + 3u + 2u * LIGHT_BINS + bin], u32(q + LIGHT_SPAN));
    atomicAdd(&light[at + 3u + 3u * LIGHT_BINS + bin], select(0u, (bitcast<u32>(m) >> 12u) & 0xffu,
                                                              m >= 0.015625));
}

@compute @workgroup_size(64, 1, 1)
fn apply(@builtin(global_invocation_id) id: vec3u, @builtin(local_invocation_index) lane: u32,
         @builtin(workgroup_id) group: vec3u) {
    let y = id.y;
    let x0 = id.x * params.pack;
    let row_hash = hash_row(y);
    var samples: array<u32, 12>;
    var largest = 0.0;
    var sum = 0.0;

    for (var i = 0u; i < params.pack; i = i + 1u) {
        let x = x0 + i;
        var codes = vec3u(0u);

        if (x < params.width && y < params.height) {
            let nits = render(x, y);
            let m = largest_of(nits);

            if ((params.flags & FLAG_LIGHT) != 0u) {
                light_add(nits, m, x, y);
            }

            codes = pack_codes(nits, x, row_hash);

            if ((params.flags & FLAG_GRAIN) != 0u) {
                codes = grain_codes(codes, x, y);
            }

            largest = max(largest, m);
            sum = sum + m;
        }

        samples[i * 3u] = codes.x;
        samples[i * 3u + 1u] = codes.y;
        samples[i * 3u + 2u] = codes.z;
    }

    // every word of the thread's: 3 of 16-bit pixel pairs, of 8-bit quads
    if (y < params.height && x0 < params.width) {
        let at = y * params.out_stride + id.x * 3u;

        if (params.pack == 2u) {
            dst[at] = samples[0] | (samples[1] << 16u);
            dst[at + 1u] = samples[2] | (samples[3] << 16u);
            dst[at + 2u] = samples[4] | (samples[5] << 16u);
        } else {
            for (var w = 0u; w < 3u; w = w + 1u) {
                dst[at + w] = samples[w * 4u] | (samples[w * 4u + 1u] << 8u) |
                              (samples[w * 4u + 2u] << 16u) | (samples[w * 4u + 3u] << 24u);
            }
        }
    }

    if ((params.flags & FLAG_LIGHT) == 0u) {
        return;
    }

    partial[lane] = sum;
    workgroupBarrier();

    for (var reach = GROUP / 2u; reach > 0u; reach = reach / 2u) {
        if (lane < reach) {
            partial[lane] = partial[lane] + partial[lane + reach];
        }

        workgroupBarrier();
    }

    if (y < params.height) {
        atomicMax(&light[y], bitcast<u32>(largest));

        if (lane == 0u) {
            atomicStore(&light[params.height + y * params.groups_x + group.x], bitcast<u32>(partial[0]));
        }
    }
}

// the codes of pixel x of 16-bit row y of dst
fn rgb16(x: u32, y: u32) -> vec3f {
    var out: vec3f;

    for (var c = 0u; c < 3u; c = c + 1u) {
        let i = x * 3u + c;
        let word = dst[y * params.out_stride + (i >> 1u)];

        out[c] = f32((word >> ((i & 1u) * 16u)) & 0xffffu);
    }

    return out;
}

fn luma16(x: u32, y: u32) -> f32 {
    let v = rgb16(x, y);

    return params.y_off + params.y_range * (params.kr * v.x * INV_CODE16 +
                                            params.kg * v.y * INV_CODE16 +
                                            params.kb * v.z * INV_CODE16);
}

fn cbcr16(x: u32, y: u32) -> vec2f {
    let v = rgb16(x, y) * INV_CODE16;
    let luma = params.kr * v.x + params.kg * v.y + params.kb * v.z;

    return vec2f((v.z - luma) * params.inv_cb, (v.x - luma) * params.inv_cr);
}

fn code_of(v: f32) -> u32 {
    return u32(clamp(v, 0.0, params.top) + 0.5) << params.shift;
}

// the samples of a word: 2 of 16-bit samples, 4 of 8-bit
fn word_of(s: array<u32, 4>) -> u32 {
    if (params.yuv_bytes == 2u) {
        return s[0] | (s[1] << 16u);
    }

    return s[0] | (s[1] << 8u) | (s[2] << 16u) | (s[3] << 24u);
}

@compute @workgroup_size(64, 1, 1)
fn yuv(@builtin(global_invocation_id) id: vec3u) {
    let k = id.y;
    let per = select(4u, 2u, params.yuv_bytes == 2u);
    let j0 = id.x * per;

    if (k >= params.ch || j0 >= params.cw) {
        return;
    }

    let r0 = 2u * k;
    let r1 = select(r0, r0 + 1u, r0 + 1u < params.height);
    var cb: array<u32, 4>;
    var cr: array<u32, 4>;
    var lumas: array<array<u32, 4>, 4>;

    for (var i = 0u; i < 4u; i = i + 1u) {
        cb[i] = 0u;
        cr[i] = 0u;

        for (var q = 0u; q < 4u; q = q + 1u) {
            lumas[i][q] = 0u;
        }
    }

    for (var i = 0u; i < per; i = i + 1u) {
        let j = j0 + i;

        if (j >= params.cw) {
            break;
        }

        let x = 2u * j;
        let l = select(0u, x - 1u, x > 0u);
        let r = min(x + 1u, params.width - 1u);
        var sum = vec2f(0.0);

        for (var n = 0u; n < 2u; n = n + 1u) {
            let row = select(r0, r1, n == 1u);

            sum = sum + (0.25 * cbcr16(l, row) + 0.5 * cbcr16(x, row) + 0.25 * cbcr16(r, row));
        }

        cb[i] = code_of(params.c_off + params.c_range * 0.5 * sum.x);
        cr[i] = code_of(params.c_off + params.c_range * 0.5 * sum.y);

        // the luma of the pixels over the sample (lumas[row][their place in the 2 words])
        for (var n = 0u; n < 2u; n = n + 1u) {
            let row = select(r0, r0 + 1u, n == 1u);

            for (var d = 0u; d < 2u; d = d + 1u) {
                if (x + d < params.width && row < params.height) {
                    let q = 2u * i + d;

                    lumas[n * 2u + q / per][q % per] = code_of(luma16(x + d, row));
                }
            }
        }
    }

    if (params.pairs == 1u) {
        var lo: array<u32, 4>;
        var hi: array<u32, 4>;

        for (var i = 0u; i < 4u; i = i + 1u) {
            lo[i] = select(cb[i / 2u], cr[i / 2u], i % 2u == 1u);
            hi[i] = select(cb[(per + i) / 2u], cr[(per + i) / 2u], i % 2u == 1u);
        }

        let at = params.cb_at + k * params.c_stride + 2u * id.x;

        planes[at] = word_of(lo);
        planes[at + 1u] = word_of(hi);
    } else {
        planes[params.cb_at + k * params.c_stride + id.x] = word_of(cb);
        planes[params.cr_at + k * params.c_stride + id.x] = word_of(cr);
    }

    for (var n = 0u; n < 2u; n = n + 1u) {
        let row = r0 + n;

        if (row >= params.height) {
            break;
        }

        let at = row * params.y_stride + 2u * id.x;

        planes[at] = word_of(lumas[n * 2u]);
        planes[at + 1u] = word_of(lumas[n * 2u + 1u]);
    }
}
