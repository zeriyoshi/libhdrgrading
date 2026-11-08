// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// A port of AMD FidelityFX Super Resolution 1.0.2 (EASU and RCAS,
// third_party/fidelityfx-fsr/ffx-fsr/ffx_fsr1.h):
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// hdrg.scale.fsr1 (src/plugins/fsr/fsr.c, fsr.h) on the GPU, a whole RGB16
// frame: easu upscales (a thread 2 output pixels, every word written) into the
// output, or with RCAS into mid (floats), and rcas sharpens mid into the output.
// fsr.h's float operations in their order; the GPU's division may round
// otherwise, so the output is within the CPU's (L2).

struct Params {
    in_w: u32,
    in_h: u32,
    out_w: u32,
    out_h: u32,
    in_stride: u32,   // words a row of the input
    out_stride: u32,  // ... and of the output
    flags: u32,       // 1: EASU into mid (RCAS follows); 2: the source's codes (the same size)
    pad: u32,
    scale_x: f32,     // output x to input x * scale + offset (FsrEasuCon)
    scale_y: f32,
    offset_x: f32,
    offset_y: f32,
    sharpness: f32,   // RCAS: 2^-stops (FsrRcasCon)
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read_write> mid: array<f32>;
@group(0) @binding(3) var<storage, read_write> dst: array<u32>;

const UNIT: f32 = 1.0 / 65535.0;
const RCAS_LIMIT: f32 = 0.25 - 1.0 / 16.0;

fn rcp_lo(a: f32) -> f32 {
    return bitcast<f32>(0x7ef07ebbu - bitcast<u32>(a));
}

fn rsq_lo(a: f32) -> f32 {
    return bitcast<f32>(0x5f347d74u - (bitcast<u32>(a) >> 1u));
}

// the source pixel (clamped to the frame) as floats
fn texel(x: i32, y: i32) -> vec3f {
    let cx = u32(clamp(x, 0, i32(params.in_w) - 1));
    let cy = u32(clamp(y, 0, i32(params.in_h) - 1));
    var out = vec3f(0.0);

    for (var c = 0u; c < 3u; c++) {
        let s = cx * 3u + c;
        let word = source[cy * params.in_stride + (s >> 1u)];

        out[c] = f32((word >> ((s & 1u) * 16u)) & 0xffffu) * UNIT;
    }

    return out;
}

fn luma(c: vec3f) -> f32 {
    return c.z * 0.5 + (c.x * 0.5 + c.y);
}

// FsrEasuSetF: (dir.x, dir.y, len) gains a corner's
fn easu_set(acc: vec3f, w: f32, la: f32, lb: f32, lc: f32, ld: f32, le: f32) -> vec3f {
    var out = acc;
    let dc = ld - lc;
    let cb = lc - lb;
    var len_x = rcp_lo(max(abs(dc), abs(cb)));
    let dir_x = ld - lb;

    out.x += dir_x * w;
    len_x = clamp(abs(dir_x) * len_x, 0.0, 1.0);
    len_x *= len_x;
    out.z += len_x * w;

    let ec = le - lc;
    let ca = lc - la;
    var len_y = rcp_lo(max(abs(ec), abs(ca)));
    let dir_y = le - la;

    out.y += dir_y * w;
    len_y = clamp(abs(dir_y) * len_y, 0.0, 1.0);
    len_y *= len_y;
    out.z += len_y * w;

    return out;
}

// FsrEasuTapF: a tap's weight (w) and weighted color (xyz)
fn easu_tap(off: vec2f, dir: vec2f, len: vec2f, lob: f32, clp: f32, c: vec3f) -> vec4f {
    var v = vec2f((off.x * dir.x) + (off.y * dir.y), (off.x * -dir.y) + (off.y * dir.x));

    v *= len;

    let d2 = min(v.x * v.x + v.y * v.y, clp);
    var wb = (2.0 / 5.0) * d2 + -1.0;
    var wa = lob * d2 + -1.0;

    wb *= wb;
    wa *= wa;
    wb = (25.0 / 16.0) * wb + -(25.0 / 16.0 - 1.0);

    let w = wb * wa;

    return vec4f(c * w, w);
}

// FsrEasuF: output pixel (x, y)
fn easu_pixel(x: u32, y: u32) -> vec3f {
    var pp = vec2f(f32(x) * params.scale_x + params.offset_x,
                   f32(y) * params.scale_y + params.offset_y);
    let fp = floor(pp);

    pp -= fp;

    let fx = i32(fp.x);
    let fy = i32(fp.y);
    let b = texel(fx, fy - 1);
    let c = texel(fx + 1, fy - 1);
    let e = texel(fx - 1, fy);
    let f = texel(fx, fy);
    let g = texel(fx + 1, fy);
    let h = texel(fx + 2, fy);
    let i = texel(fx - 1, fy + 1);
    let j = texel(fx, fy + 1);
    let k = texel(fx + 1, fy + 1);
    let l = texel(fx + 2, fy + 1);
    let n = texel(fx, fy + 2);
    let o = texel(fx + 1, fy + 2);
    var acc = vec3f(0.0);

    acc = easu_set(acc, (1.0 - pp.x) * (1.0 - pp.y), luma(b), luma(e), luma(f), luma(g), luma(j));
    acc = easu_set(acc, pp.x * (1.0 - pp.y), luma(c), luma(f), luma(g), luma(h), luma(k));
    acc = easu_set(acc, (1.0 - pp.x) * pp.y, luma(f), luma(i), luma(j), luma(k), luma(n));
    acc = easu_set(acc, pp.x * pp.y, luma(g), luma(j), luma(k), luma(l), luma(o));

    var dir = acc.xy;
    var len = acc.z;
    var dir_r = dir.x * dir.x + dir.y * dir.y;
    let zero = dir_r < 1.0 / 32768.0;

    dir_r = select(rsq_lo(dir_r), 1.0, zero);
    dir.x = select(dir.x, 1.0, zero);
    dir *= dir_r;
    len = len * 0.5;
    len *= len;

    let stretch = (dir.x * dir.x + dir.y * dir.y) * rcp_lo(max(abs(dir.x), abs(dir.y)));
    let len2 = vec2f(1.0 + (stretch - 1.0) * len, 1.0 + -0.5 * len);
    let lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    let clp = rcp_lo(lob);
    let lo = min(min(min(f, g), j), k);
    let hi = max(max(max(f, g), j), k);
    var sum = vec4f(0.0);

    sum += easu_tap(vec2f(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    sum += easu_tap(vec2f(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    sum += easu_tap(vec2f(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    sum += easu_tap(vec2f(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    sum += easu_tap(vec2f(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    sum += easu_tap(vec2f(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    sum += easu_tap(vec2f(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    sum += easu_tap(vec2f(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    sum += easu_tap(vec2f(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    sum += easu_tap(vec2f(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    sum += easu_tap(vec2f(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    sum += easu_tap(vec2f(0.0, 2.0) - pp, dir, len2, lob, clp, n);

    if (sum.w == 0.0) {
        return f;
    }

    return min(hi, max(lo, sum.xyz * (1.0 / sum.w)));
}

// the source's pixel at the same size (flags 2: the CPU's copy), else EASU's
fn source_or_easu(x: u32, y: u32) -> vec3f {
    if ((params.flags & 2u) != 0u) {
        return texel(i32(x), i32(y));
    }

    return easu_pixel(x, y);
}

fn code(v: f32) -> u32 {
    return u32(clamp(v, 0.0, 1.0) * 65535.0 + 0.5);
}

// two output pixels' codes into the output's words (an odd width's last: its own two words)
fn put(x: u32, y: u32, p: vec3f, q: vec3f, has_q: bool) {
    let base = y * params.out_stride + (x >> 1u) * 3u;

    dst[base] = code(p.x) | (code(p.y) << 16u);

    if (has_q) {
        dst[base + 1u] = code(p.z) | (code(q.x) << 16u);
        dst[base + 2u] = code(q.y) | (code(q.z) << 16u);
    } else {
        dst[base + 1u] = code(p.z);
    }
}

@compute @workgroup_size(8, 8)
fn easu(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;
    let y = id.y;

    if (x >= params.out_w || y >= params.out_h) {
        return;
    }

    let has_q = x + 1u < params.out_w;
    let p = source_or_easu(x, y);
    var q = p;

    if (has_q) {
        q = source_or_easu(x + 1u, y);
    }

    if ((params.flags & 1u) == 0u) {
        put(x, y, p, q, has_q);

        return;
    }

    let at = (y * params.out_w + x) * 3u;

    for (var c = 0u; c < 3u; c++) {
        mid[at + c] = p[c];

        if (has_q) {
            mid[at + 3u + c] = q[c];
        }
    }
}

fn easu_at(x: i32, y: i32) -> vec3f {
    let cx = u32(clamp(x, 0, i32(params.out_w) - 1));
    let cy = u32(clamp(y, 0, i32(params.out_h) - 1));
    let at = (cy * params.out_w + cx) * 3u;

    return vec3f(mid[at], mid[at + 1u], mid[at + 2u]);
}

// FsrRcasF without its denoise (fsr.h's hdrg_fsr_rcas)
fn rcas_pixel(x: u32, y: u32) -> vec3f {
    let sx = i32(x);
    let sy = i32(y);
    let b = easu_at(sx, sy - 1);
    let d = easu_at(sx - 1, sy);
    let e = easu_at(sx, sy);
    let f = easu_at(sx + 1, sy);
    let h = easu_at(sx, sy + 1);
    let lo = min(min(min(b, d), f), h);
    let hi = max(max(max(b, d), f), h);
    var worst = -RCAS_LIMIT;

    for (var c = 0u; c < 3u; c++) {
        var hit_min = 0.0;
        var hit_max = 0.0;

        if (hi[c] > 0.0) {
            hit_min = min(lo[c], e[c]) * (1.0 / (4.0 * hi[c]));
        }

        if (lo[c] < 1.0) {
            hit_max = (1.0 - max(hi[c], e[c])) * (1.0 / (4.0 * lo[c] + -4.0));
        }

        worst = max(worst, max(-hit_min, hit_max));
    }

    let lobe = max(-RCAS_LIMIT, min(worst, 0.0)) * params.sharpness;
    let rcp = 1.0 / (4.0 * lobe + 1.0);

    return (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcp;
}

@compute @workgroup_size(8, 8)
fn rcas(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;
    let y = id.y;

    if (x >= params.out_w || y >= params.out_h) {
        return;
    }

    let has_q = x + 1u < params.out_w;
    let p = rcas_pixel(x, y);
    var q = p;

    if (has_q) {
        q = rcas_pixel(x + 1u, y);
    }

    put(x, y, p, q, has_q);
}
