// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// A port of NVIDIA Image Scaling 1.0.3 (third_party/nis/NIS/NIS_Scaler.h):
// The MIT License(MIT)
// Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files(the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// hdrg.scale.nis (src/plugins/nis/nis.c, nis.h) on the GPU, a whole RGB16
// frame: lumas writes the source's luma (a thread a pixel), scale the output
// (a thread 2 output pixels, every word written; flags 1: the source's codes,
// the same size). nis.h's float operations in their order; the GPU's
// divisions may round otherwise, so the output is within the CPU's (L2).

struct Params {
    in_w: u32,
    in_h: u32,
    out_w: u32,
    out_h: u32,
    in_stride: u32,   // words a row of the input
    out_stride: u32,  // ... and of the output
    flags: u32,       // 1: the source's codes (the same size)
    pad: u32,
    scale_x: f32,     // NVScalerUpdateConfig's
    scale_y: f32,
    detect_ratio: f32,
    detect_thres: f32,
    min_contrast_ratio: f32,
    ratio_norm: f32,
    contrast_boost: f32,
    eps: f32,
    sharp_start_y: f32,
    sharp_scale_y: f32,
    sharp_strength_min: f32,
    sharp_strength_scale: f32,
    sharp_limit_min: f32,
    sharp_limit_scale: f32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read> coefs: array<f32>;    // coef_scale, then coef_usm: 64 x 8
@group(0) @binding(3) var<storage, read_write> lumas: array<f32>;
@group(0) @binding(4) var<storage, read_write> dst: array<u32>;

const UNIT: f32 = 1.0 / 65535.0;
const USM: u32 = 512u; // coef_usm's start

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

fn luma_of(c: vec3f) -> f32 {
    return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
}

fn luma_at(x: i32, y: i32) -> f32 {
    let cx = u32(clamp(x, 0, i32(params.in_w) - 1));
    let cy = u32(clamp(y, 0, i32(params.in_h) - 1));

    return lumas[cy * params.in_w + cx];
}

// HLSL's lerp
fn blend(a: f32, b: f32, t: f32) -> f32 {
    return a + t * (b - a);
}

@compute @workgroup_size(8, 8)
fn luma(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.in_w || id.y >= params.in_h) {
        return;
    }

    // in integers (nis.h's hdrg_nis_y_codes): the CPU's exactly
    var codes = vec3u(0u);

    for (var c = 0u; c < 3u; c++) {
        let s = id.x * 3u + c;
        let word = source[id.y * params.in_stride + (s >> 1u)];

        codes[c] = (word >> ((s & 1u) * 16u)) & 0xffffu;
    }

    lumas[id.y * params.in_w + id.x] =
        f32(2126u * codes.x + 7152u * codes.y + 722u * codes.z) * (1.0 / 655350000.0);
}

// GetEdgeMap of the 3 x 3 luma centred at source pixel (x, y): (0, 90, 45, 135)
fn edge_at(x: i32, y: i32) -> vec4f {
    var p: array<array<f32, 3>, 3>;

    for (var k = 0; k < 3; k++) {
        for (var l = 0; l < 3; l++) {
            p[k][l] = luma_at(x + l - 1, y + k - 1);
        }
    }

    let g0 = abs(p[0][0] + p[0][1] + p[0][2] - p[2][0] - p[2][1] - p[2][2]);
    let g45 = abs(p[1][0] + p[0][0] + p[0][1] - p[2][1] - p[2][2] - p[1][2]);
    let g90 = abs(p[0][0] + p[1][0] + p[2][0] - p[0][2] - p[1][2] - p[2][2]);
    let g135 = abs(p[1][0] + p[2][0] + p[2][1] - p[0][1] - p[0][2] - p[1][2]);
    let max_090 = max(g0, g90);
    let min_090 = min(g0, g90);
    let max_45135 = max(g45, g135);
    let min_45135 = min(g45, g135);

    if (max_090 + max_45135 == 0.0) {
        return vec4f(0.0);
    }

    let e_090 = min(max_090 / (max_090 + max_45135), 1.0);
    let e_45135 = 1.0 - e_090;
    let c_090 = (max_090 > (min_090 * params.detect_ratio)) && (max_090 > params.detect_thres) &&
                (max_090 > min_45135);
    let c_45135 = (max_45135 > (min_45135 * params.detect_ratio)) &&
                  (max_45135 > params.detect_thres) && (max_45135 > min_090);
    let is_0 = max_090 == g0;
    let is_45 = max_45135 == g45;
    let f_090 = select(1.0, e_090, c_090 && c_45135);
    let f_45135 = select(1.0, e_45135, c_090 && c_45135);

    return vec4f(select(0.0, f_090, c_090 && is_0), select(0.0, f_090, c_090 && !is_0),
                 select(0.0, f_45135, c_45135 && is_45), select(0.0, f_45135, c_45135 && !is_45));
}

fn lti(p: array<f32, 6>, phase: u32) -> f32 {
    let selector = phase <= 32u;
    var sel = select(p[3], p[0], selector);
    let a_min = min(min(p[1], p[2]), sel);
    let a_max = max(max(p[1], p[2]), sel);

    sel = select(p[5], p[2], selector);

    let b_min = min(min(p[3], p[4]), sel);
    let b_max = max(max(p[3], p[4]), sel);
    let a_cont = a_max - a_min;
    let b_cont = b_max - b_min;
    let ratio = max(a_cont, b_cont) / (min(a_cont, b_cont) + params.eps);

    return (1.0 - clamp((ratio - params.min_contrast_ratio) * params.ratio_norm, 0.0, 1.0)) *
           params.contrast_boost;
}

fn poly6(p: array<f32, 6>, phase: u32) -> f32 {
    var y = 0.0;
    var usm = 0.0;

    for (var i = 0u; i < 6u; i++) {
        y += coefs[phase * 8u + i] * p[i];
    }

    for (var i = 0u; i < 6u; i++) {
        usm += coefs[USM + phase * 8u + i] * p[i];
    }

    let scale = 1.0 - clamp((y - params.sharp_start_y) * params.sharp_scale_y, 0.0, 1.0);
    let strength = scale * params.sharp_strength_scale + params.sharp_strength_min;

    usm *= strength;

    let limit = (scale * params.sharp_limit_scale + params.sharp_limit_min) * y;

    usm = min(limit, max(-limit, usm));
    usm *= lti(p, phase);

    return y + usm;
}

fn normal(p: array<array<f32, 6>, 6>, phase_x: u32, phase_y: u32) -> f32 {
    var h = 0.0;

    for (var j = 0u; j < 6u; j++) {
        var v = 0.0;

        for (var i = 0u; i < 6u; i++) {
            v += p[i][j] * coefs[phase_y * 8u + i];
        }

        h += v * coefs[phase_x * 8u + j];
    }

    return h;
}

// AddDirFilters' 45 / 135 degree lines: t[0..7) shifted by one when plus >= 1
fn diagonal(t: array<f32, 7>, plus: f32, w: f32) -> f32 {
    var line: array<f32, 6>;
    let shift = plus >= 1.0;

    for (var i = 0u; i < 6u; i++) {
        line[i] = select(t[i], t[i + 1u], shift);
    }

    let phase = select(plus, plus - 1.0, shift);

    return poly6(line, u32(phase * 64.0)) * w;
}

fn directional(p: array<array<f32, 6>, 6>, fx: f32, fy: f32, phase_x: u32, phase_y: u32,
               w: vec4f) -> f32 {
    var f = 0.0;
    var line: array<f32, 6>;
    var t: array<f32, 7>;

    if (w.x > 0.0) {
        for (var i = 0u; i < 6u; i++) {
            line[i] = blend(p[i][2], p[i][3], fx);
        }

        f += poly6(line, phase_y) * w.x;
    }

    if (w.y > 0.0) {
        for (var i = 0u; i < 6u; i++) {
            line[i] = blend(p[2][i], p[3][i], fy);
        }

        f += poly6(line, phase_x) * w.y;
    }

    if (w.z > 0.0) {
        var b = 0.5 + 0.5 * (fx - fy);

        t[1] = blend(p[2][1], p[1][2], b);
        t[3] = blend(p[3][2], p[2][3], b);
        t[5] = blend(p[4][3], p[3][4], b);
        b = b - 0.5;

        let ab = abs(b);
        let up = b >= 0.0;

        t[0] = blend(p[1][1], select(p[2][0], p[0][2], up), ab);
        t[2] = blend(p[2][2], select(p[3][1], p[1][3], up), ab);
        t[4] = blend(p[3][3], select(p[4][2], p[2][4], up), ab);
        t[6] = blend(p[4][4], select(p[5][3], p[3][5], up), ab);
        f += diagonal(t, fx + fy, w.z);
    }

    if (w.w > 0.0) {
        var b = 0.5 * (fx + fy);

        t[1] = blend(p[3][1], p[4][2], b);
        t[3] = blend(p[2][2], p[3][3], b);
        t[5] = blend(p[1][3], p[2][4], b);
        b = b - 0.5;

        let ab = abs(b);
        let up = b >= 0.0;

        t[0] = blend(p[4][1], select(p[3][0], p[5][2], up), ab);
        t[2] = blend(p[3][2], select(p[2][1], p[4][3], up), ab);
        t[4] = blend(p[2][3], select(p[1][2], p[3][4], up), ab);
        t[6] = blend(p[1][4], select(p[0][3], p[2][5], up), ab);
        f += diagonal(t, 1.0 + (fx - fy), w.w);
    }

    return f;
}

// output pixel (x, y) (hdrg_nis_pixel)
fn pixel(x: u32, y: u32) -> vec3f {
    if ((params.flags & 1u) != 0u) {
        return texel(i32(x), i32(y));
    }

    let src_x = (0.5 + f32(x)) * params.scale_x - 0.5;
    let src_y = (0.5 + f32(y)) * params.scale_y - 0.5;
    let ix = i32(floor(src_x));
    let iy = i32(floor(src_y));
    let fx = src_x - floor(src_x);
    let fy = src_y - floor(src_y);
    let phase_x = u32(fx * 64.0);
    let phase_y = u32(fy * 64.0);
    let e00 = edge_at(ix, iy);
    let e01 = edge_at(ix + 1, iy);
    let e10 = edge_at(ix, iy + 1);
    let e11 = edge_at(ix + 1, iy + 1);
    var w = vec4f(0.0);
    var p: array<array<f32, 6>, 6>;

    for (var k = 0u; k < 4u; k++) {
        w[k] = blend(blend(e00[k], e01[k], fx), blend(e10[k], e11[k], fx), fy);
    }

    for (var i = 0; i < 6; i++) {
        for (var j = 0; j < 6; j++) {
            p[i][j] = luma_at(ix + j - 2, iy + i - 2);
        }
    }

    let base = 1.0 - w.x - w.y - w.z - w.w;
    var op_y = 0.0;

    op_y += normal(p, phase_x, phase_y) * base;
    op_y += directional(p, fx, fy, phase_x, phase_y, w);

    let s0 = texel(ix, iy);
    let s1 = texel(ix + 1, iy);
    let s2 = texel(ix, iy + 1);
    let s3 = texel(ix + 1, iy + 1);
    var rgb = vec3f(0.0);

    for (var c = 0u; c < 3u; c++) {
        rgb[c] = blend(blend(s0[c], s1[c], fx), blend(s2[c], s3[c], fx), fy);
    }

    return rgb + vec3f(op_y - luma_of(rgb));
}

fn code(v: f32) -> u32 {
    return u32(clamp(v, 0.0, 1.0) * 65535.0 + 0.5);
}

@compute @workgroup_size(8, 8)
fn scale(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;
    let y = id.y;

    if (x >= params.out_w || y >= params.out_h) {
        return;
    }

    let p = pixel(x, y);
    let base = y * params.out_stride + id.x * 3u;

    dst[base] = code(p.x) | (code(p.y) << 16u);

    if (x + 1u < params.out_w) {
        let q = pixel(x + 1u, y);

        dst[base + 1u] = code(p.z) | (code(q.x) << 16u);
        dst[base + 2u] = code(q.y) | (code(q.z) << 16u);
    } else {
        dst[base + 1u] = code(p.z);
    }
}
