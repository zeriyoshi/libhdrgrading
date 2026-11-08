// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.scale.edge (src/plugins/edge/edge.c, edge.h) on the GPU, a whole
// RGB16 frame: luma writes the source's luma in integers (a thread a pixel),
// scale the output (a thread 2 output pixels, every word written; flags 1:
// the source's codes, the same size). edge.h's float operations in their
// order; the GPU's divisions may round otherwise, so the output is within
// the CPU's (L2).

struct Params {
    in_w: u32,
    in_h: u32,
    out_w: u32,
    out_h: u32,
    in_stride: u32,   // words a row of the input
    out_stride: u32,  // ... and of the output
    flags: u32,       // 1: the source's codes (the same size)
    pad: u32,
    scale_x: f32,     // output pixel centre to source: (x + 0.5) scale - 0.5
    scale_y: f32,
    stretch: f32,
    sharpen: f32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read_write> lumas: array<f32>;
@group(0) @binding(3) var<storage, read_write> dst: array<u32>;

const UNIT: f32 = 1.0 / 65535.0;
const FLAT: f32 = 1.0 / 4096.0;
const NORMAL: f32 = 1e-12;
const GATE: f32 = 2.0;

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

fn luma_at(x: i32, y: i32) -> f32 {
    let cx = u32(clamp(x, 0, i32(params.in_w) - 1));
    let cy = u32(clamp(y, 0, i32(params.in_h) - 1));

    return lumas[cy * params.in_w + cx];
}

@compute @workgroup_size(8, 8)
fn luma(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.in_w || id.y >= params.in_h) {
        return;
    }

    // in integers (edge.h's hdrg_edge_luma): the CPU's exactly
    var codes = vec3u(0u);

    for (var c = 0u; c < 3u; c++) {
        let s = id.x * 3u + c;
        let word = source[id.y * params.in_stride + (s >> 1u)];

        codes[c] = (word >> ((s & 1u) * 16u)) & 0xffffu;
    }

    lumas[id.y * params.in_w + id.x] =
        f32(2126u * codes.x + 7152u * codes.y + 722u * codes.z) * (1.0 / 655350000.0);
}

// the 3-lobe kernel at x^2 (hdrg_edge_kernel)
fn kernel3(t: f32) -> f32 {
    if (t >= 9.0) {
        return 0.0;
    }

    let g = 0.99495652 +
            t * (-0.448686746 + t * (0.0832075298 + t * (-0.00742809153 + t * 0.000262519738)));

    return (1.0 - t) * (1.0 - 0.25 * t) * (1.0 - t / 9.0) * g;
}

// hdrg_edge_sqrt: Newton's iterations from the exponent's half
fn root_of(v: f32) -> f32 {
    if (!(v > 0.0)) {
        return 0.0;
    }

    var r = bitcast<f32>((bitcast<u32>(v) >> 1u) + 0x1fbd1df5u);

    for (var i = 0u; i < 4u; i++) {
        r = 0.5 * (r + v / r);
    }

    return r;
}

// the output pixel (x, y) (hdrg_edge_out, hdrg_edge_pixel)
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
    var l: array<array<f32, 6>, 6>;

    for (var i = 0; i < 6; i++) {
        for (var j = 0; j < 6; j++) {
            l[i][j] = luma_at(ix + j - 2, iy + i - 2);
        }
    }

    // the edge frame (hdrg_edge_frame)
    var jxx = 0.0;
    var jxy = 0.0;
    var jyy = 0.0;

    for (var i = 1u; i < 5u; i++) {
        for (var j = 1u; j < 5u; j++) {
            let ox = f32(j) - 2.0 - fx;
            let oy = f32(i) - 2.0 - fy;
            let d2 = ox * ox + oy * oy;

            if (!(d2 < 4.0)) {
                continue;
            }

            var w = 1.0 - 0.25 * d2;

            w *= w;

            let dx = (l[i - 1u][j + 1u] + 2.0 * l[i][j + 1u] + l[i + 1u][j + 1u]) -
                     (l[i - 1u][j - 1u] + 2.0 * l[i][j - 1u] + l[i + 1u][j - 1u]);
            let dy = (l[i + 1u][j - 1u] + 2.0 * l[i + 1u][j] + l[i + 1u][j + 1u]) -
                     (l[i - 1u][j - 1u] + 2.0 * l[i - 1u][j] + l[i - 1u][j + 1u]);

            jxx += w * (dx * dx);
            jxy += w * (dx * dy);
            jyy += w * (dy * dy);
        }
    }

    let trace = jxx + jyy;
    let diff = jxx - jyy;
    let root = root_of(diff * diff + 4.0 * jxy * jxy);
    var gx = 1.0;
    var gy = 0.0;
    var coherence = 0.0;

    if (trace > FLAT * FLAT) {
        var nx = diff + root;
        var ny = 2.0 * jxy;
        var len = nx * nx + ny * ny;

        if (!(len > NORMAL)) {
            nx = -ny;
            ny = diff - root;
            len = nx * nx + ny * ny;
        }

        if (len > NORMAL) {
            len = root_of(len);
            gx = nx / len;
            gy = ny / len;

            let c = min(root / trace, 1.0);

            coherence = c * c * (trace / (trace + GATE));
        }
    }

    // the kernel in the edge's frame over the 6 x 6 taps
    let along = 1.0 / (1.0 + params.stretch * coherence);
    let across = 1.0 + params.sharpen * coherence;
    var sum = vec3f(0.0);
    var weight = 0.0;

    for (var i = 0; i < 6; i++) {
        for (var j = 0; j < 6; j++) {
            let ox = f32(j) - 2.0 - fx;
            let oy = f32(i) - 2.0 - fy;
            let a = (ox * -gy + oy * gx) * along;
            let b = (ox * gx + oy * gy) * across;
            let w = kernel3(a * a) * kernel3(b * b);

            sum += texel(ix + j - 2, iy + i - 2) * w;
            weight += w;
        }
    }

    let p00 = texel(ix, iy);
    let p01 = texel(ix + 1, iy);
    let p10 = texel(ix, iy + 1);
    let p11 = texel(ix + 1, iy + 1);
    let lo = min(min(p00, p01), min(p10, p11));
    let hi = max(max(p00, p01), max(p10, p11));

    if (!(weight > 0.0)) {
        return p00;
    }

    return min(hi, max(lo, sum / weight));
}

fn code(v: f32) -> u32 {
    return u32(v * 65535.0 + 0.5);
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
