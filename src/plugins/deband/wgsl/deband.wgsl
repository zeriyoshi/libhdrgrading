// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.webgpu.deband: hdrg.deband (src/plugins/deband/deband.c) on the GPU, on a
// whole RGB16 frame in three passes: Y' (lumas), the 3 x 3 Y' ranges over
// the edge threshold (edges), the fits (fit, two pixels a thread so the
// output is written in whole words). The integers are the CPU's (Y', the
// edges, the window sums of the samples less the center's and their
// moments); the fit's residual is summed directly from its plane in f32
// where the CPU subtracts the plane from the window's sums in f64, so the
// results agree within the tolerance a GPU is given (L2).

struct Params {
    width: u32,
    height: u32,
    in_stride: u32,  // words a row of the input
    out_stride: u32, // ... and of the output
    strength: f32,
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read_write> luma: array<i32>;
@group(0) @binding(3) var<storage, read_write> edge: array<u32>;
@group(0) @binding(4) var<storage, read_write> dst: array<u32>;

const EDGE: i32 = 3084;               // 12 x 257: a 3 x 3 Y' range from which a window has an edge
const LO: f32 = 205.6;                // 0.8 x 257: the residual RMS of a full weight, x strength
const HI: f32 = 411.2;                // 1.6 x 257: and of none
const QUANT: f32 = 102.8;             // 0.4 x 257: what quantization leaves, x strength ...
const GRAIN: f32 = 171.3419;          // 0.6667 x 257: ... from which only steps are smoothed
const COH_LO: f32 = 0.2;
const COH_HI: f32 = 0.5;
const K: vec3f = vec3f(0.212600708, 0.715194702, 0.072204590); // 13933, 46871, 4732 / 65536

fn sample(x: u32, y: u32, c: u32) -> i32 {
    let s = x * 3u + c;
    let word = source[y * params.in_stride + (s >> 1u)];

    return i32((word >> ((s & 1u) * 16u)) & 0xffffu);
}

fn pixel(x: u32, y: u32) -> vec3i {
    return vec3i(sample(x, y, 0u), sample(x, y, 1u), sample(x, y, 2u));
}

fn lum(x: u32, y: u32) -> i32 {
    return luma[y * params.width + x];
}

@compute @workgroup_size(8, 8)
fn lumas(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.width || id.y >= params.height) {
        return;
    }

    let v = vec3u(pixel(id.x, id.y));

    luma[id.y * params.width + id.x] = i32((v.x * 13933u + v.y * 46871u + v.z * 4732u + 32768u) >> 16u);
}

@compute @workgroup_size(8, 8)
fn edges(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.width || id.y >= params.height) {
        return;
    }

    let x0 = select(id.x - 1u, 0u, id.x == 0u);
    let x1 = min(id.x + 1u, params.width - 1u);
    let y0 = select(id.y - 1u, 0u, id.y == 0u);
    let y1 = min(id.y + 1u, params.height - 1u);
    var lo = lum(id.x, id.y);
    var hi = lo;

    for (var y = y0; y <= y1; y++) {
        for (var x = x0; x <= x1; x++) {
            lo = min(lo, lum(x, y));
            hi = max(hi, lum(x, y));
        }
    }

    edge[id.y * params.width + id.x] = select(0u, 1u, hi - lo >= EDGE);
}

// the debanded pixel (x, y) before rounding
fn deband(x: u32, y: u32) -> vec3f {
    let w = params.width;
    let h = params.height;
    let center = pixel(x, y);
    let c = vec3f(center);
    var weight = 0.0;
    var grain = false;
    var plane = c;

    for (var i = 0u; i < 2u && weight <= 0.0 && !grain; i++) {
        let r = select(4u, 7u, i == 0u);

        if (x < r || y < r || x + r >= w || y + r >= h) {
            continue;
        }

        var edged = false;

        for (var yy = y - r; yy <= y + r && !edged; yy++) {
            for (var xx = x - r; xx <= x + r; xx++) {
                if (edge[yy * w + xx] != 0u) {
                    edged = true;
                    break;
                }
            }
        }

        if (edged) {
            continue;
        }

        // the plane: the window's mean and slopes of the samples less the center's (exact sums)
        var s = vec3i(0);
        var sx = vec3i(0);
        var sy = vec3i(0);

        for (var yy = y - r; yy <= y + r; yy++) {
            for (var xx = x - r; xx <= x + r; xx++) {
                let d = pixel(xx, yy) - center;

                s += d;
                sx += d * (i32(xx) - i32(x));
                sy += d * (i32(yy) - i32(y));
            }
        }

        let side = f32(2u * r + 1u);
        let n = side * side;
        let sxx = f32(r * (r + 1u) * (2u * r + 1u)) / 3.0 * side;
        let m = vec3f(s) / n;
        let bx = vec3f(sx) / sxx;
        let by = vec3f(sy) / sxx;
        var e = vec3f(0.0);

        for (var yy = y - r; yy <= y + r; yy++) {
            for (var xx = x - r; xx <= x + r; xx++) {
                let d = vec3f(pixel(xx, yy) - center) - m - bx * f32(i32(xx) - i32(x)) -
                        by * f32(i32(yy) - i32(y));

                e += d * d;
            }
        }

        let worst = sqrt(max(max(e.x, e.y), e.z) / n);
        let lx = 2.0 * dot(K, bx);
        let ly = 2.0 * dot(K, by);

        plane = c + m;
        weight = clamp((HI - worst) / (HI - LO), 0.0, 1.0);

        // above what quantization leaves, only steps: the residual's Y' differences coherent
        if (weight > 0.0 && worst > QUANT * params.strength) {
            var gxx = 0.0;
            var gyy = 0.0;
            var gxy = 0.0;

            for (var yy = y - r; yy <= y + r; yy++) {
                for (var xx = x - r; xx <= x + r; xx++) {
                    let dx = f32(lum(min(xx + 1u, w - 1u), yy) - lum(select(xx - 1u, 0u, xx == 0u), yy)) - lx;
                    let dy = f32(lum(xx, min(yy + 1u, h - 1u)) - lum(xx, select(yy - 1u, 0u, yy == 0u))) - ly;

                    gxx += dx * dx;
                    gyy += dy * dy;
                    gxy += dx * dy;
                }
            }

            var coh = 1.0;

            if (gxx + gyy > 0.0) {
                coh = sqrt((gxx - gyy) * (gxx - gyy) + 4.0 * gxy * gxy) / (gxx + gyy);
            }

            let steps = max((coh - COH_LO) / (COH_HI - COH_LO),
                            (GRAIN * params.strength - worst) / ((GRAIN - QUANT) * params.strength));

            weight *= clamp(steps, 0.0, 1.0);
            grain = weight <= 0.0;
        }
    }

    if (weight <= 0.0) {
        return c;
    }

    return c + weight * (plane - c);
}

fn code(v: vec3f) -> vec3u {
    return vec3u(clamp(v + 0.5, vec3f(0.0), vec3f(65535.0)));
}

@compute @workgroup_size(8, 8)
fn fit(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;

    if (x >= params.width || id.y >= params.height) {
        return;
    }

    let a = code(deband(x, id.y));
    let base = id.y * params.out_stride + id.x * 3u;

    // an odd width's last pixel: its words alone (a row's stride may end two bytes past it)
    if (x + 1u >= params.width) {
        dst[base] = a.x | (a.y << 16u);
        dst[base + 1u] = a.z;

        return;
    }

    let b = code(deband(x + 1u, id.y));

    dst[base] = a.x | (a.y << 16u);
    dst[base + 1u] = a.z | (b.x << 16u);
    dst[base + 2u] = b.y | (b.z << 16u);
}
