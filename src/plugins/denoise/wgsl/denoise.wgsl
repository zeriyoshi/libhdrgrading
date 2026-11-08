// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.webgpu.denoise: hdrg.denoise.spatial (src/plugins/denoise/denoise.c)
// on the GPU, on a whole RGB16 frame in two passes: each pixel's self-guided
// filter coefficients a = var / (var + eps) and b = mean (1 - a) of the
// code values' Y', Cb and Cr over its window (coefs: a thread a pixel), then
// q = mean(a) I + mean(b) over the window, back to R'G'B' (guide: a thread
// 2 pixels, every output word written). Windows of (2 radius + 1)^2 cut at
// the frame's edges, as the CPU's. In f32 (the CPU sums in double): the
// variance is the mean squared difference from the window's mean, not
// mean(I^2) - mean(I)^2, which f32 cannot hold for 16-bit codes.

struct Params {
    width: u32,
    height: u32,
    in_stride: u32,  // words a row of the input
    out_stride: u32, // ... and of the output
    radius: u32,
    pad0: u32,
    pad1: u32,
    pad2: u32,
    sigma: vec4f,          // the Y' noise without levels, Cb's and Cr's eps sigma, the strength
    level: array<vec4f, 2>, // the Y' noise by level in 16-bit codes (< 0: no data)
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read_write> coef_a: array<f32>;
@group(0) @binding(3) var<storage, read_write> coef_b: array<f32>;
@group(0) @binding(4) var<storage, read_write> dst: array<u32>;

const BINS: i32 = 8;
const TOP: f32 = 65535.0;
const KR: f32 = 0.2126;  // the decorrelating Y'CbCr of the code values (BT.709)
const KB: f32 = 0.0722;
const KG: f32 = 1.0 - KR - KB;
const EPS_MIN: f32 = 1e-6;

// the samples of pixel x of row y
fn pixel(x: u32, y: u32) -> vec3f {
    let s = x * 3u;
    let at = y * params.in_stride + (s >> 1u);
    let w0 = source[at];
    let w1 = source[at + 1u];

    if ((s & 1u) == 0u) {
        return vec3f(f32(w0 & 0xffffu), f32(w0 >> 16u), f32(w1 & 0xffffu));
    }

    return vec3f(f32(w0 >> 16u), f32(w1 & 0xffffu), f32(w1 >> 16u));
}

// Y', Cb, Cr of R'G'B' code values
fn components(c: vec3f) -> vec3f {
    let y = KR * c.x + KG * c.y + KB * c.z;

    return vec3f(y, (c.z - y) / (2.0 * (1.0 - KB)), (c.x - y) / (2.0 * (1.0 - KR)));
}

fn binned(i: i32) -> f32 {
    return params.level[i / 4][i % 4];
}

// the Y' noise at a level in [0, 1]: the bins with data interpolated, held past the ends
fn noise_at(y: f32) -> f32 {
    let pos = y * f32(BINS) - 0.5;
    var lo = -1;
    var hi = -1;

    for (var i = 0; i < BINS; i++) {
        if (binned(i) < 0.0) {
            continue;
        }

        if (f32(i) <= pos) {
            lo = i;
        }

        if (f32(i) >= pos && hi < 0) {
            hi = i;
        }
    }

    if (lo < 0 && hi < 0) {
        return params.sigma.x;
    }

    if (lo < 0 || hi < 0 || lo == hi) {
        return binned(select(lo, hi, lo < 0));
    }

    let t = (pos - f32(lo)) / f32(hi - lo);

    return binned(lo) * (1.0 - t) + binned(hi) * t;
}

// the window of (x, y), cut at the frame's edges: x0, y0, x1, y1 (inclusive)
fn window(x: u32, y: u32) -> vec4u {
    let r = params.radius;

    return vec4u(select(0u, x - r, x > r), select(0u, y - r, y > r), min(x + r, params.width - 1u),
                 min(y + r, params.height - 1u));
}

// the coefficients of pixel id
@compute @workgroup_size(8, 8)
fn coefs(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.width || id.y >= params.height) {
        return;
    }

    let w = window(id.x, id.y);
    let n = f32((w.z - w.x + 1u) * (w.w - w.y + 1u));
    var sum = vec3f(0.0);

    for (var y = w.y; y <= w.w; y++) {
        for (var x = w.x; x <= w.z; x++) {
            sum += components(pixel(x, y));
        }
    }

    let mean = sum / n;
    var squares = vec3f(0.0);

    for (var y = w.y; y <= w.w; y++) {
        for (var x = w.x; x <= w.z; x++) {
            let d = components(pixel(x, y)) - mean;

            squares += d * d;
        }
    }

    let variance = squares / n;
    let sigma = vec3f(noise_at(mean.x / TOP) * params.sigma.w, params.sigma.y, params.sigma.z);
    let eps = max(sigma * sigma, vec3f(EPS_MIN));
    let a = variance / (variance + eps);
    let b = mean * (1.0 - a);
    let i = (id.y * params.width + id.x) * 3u;

    coef_a[i] = a.x;
    coef_a[i + 1u] = a.y;
    coef_a[i + 2u] = a.z;
    coef_b[i] = b.x;
    coef_b[i + 1u] = b.y;
    coef_b[i + 2u] = b.z;
}

// output pixel x of row y: mean(a) I + mean(b), back to R'G'B' codes
fn guided_pixel(x: u32, y: u32) -> vec3u {
    let w = window(x, y);
    let n = f32((w.z - w.x + 1u) * (w.w - w.y + 1u));
    var sum_a = vec3f(0.0);
    var sum_b = vec3f(0.0);

    for (var ny = w.y; ny <= w.w; ny++) {
        for (var nx = w.x; nx <= w.z; nx++) {
            let i = (ny * params.width + nx) * 3u;

            sum_a += vec3f(coef_a[i], coef_a[i + 1u], coef_a[i + 2u]);
            sum_b += vec3f(coef_b[i], coef_b[i + 1u], coef_b[i + 2u]);
        }
    }

    let q = sum_a / n * components(pixel(x, y)) + sum_b / n;
    let r = q.x + 2.0 * (1.0 - KR) * q.z;
    let b = q.x + 2.0 * (1.0 - KB) * q.y;
    let g = (q.x - KR * r - KB * b) / KG;

    return vec3u(clamp(floor(vec3f(r, g, b) + 0.5), vec3f(0.0), vec3f(TOP)));
}

// the pixels id.x * 2 and the next of row id.y, in whole words (an odd
// width's last sample with the two bytes after it)
@compute @workgroup_size(8, 8)
fn guide(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;

    if (x >= params.width || id.y >= params.height) {
        return;
    }

    let a = guided_pixel(x, id.y);
    let base = id.y * params.out_stride + id.x * 3u;

    dst[base] = a.x | (a.y << 16u);

    if (x + 1u < params.width) {
        let b = guided_pixel(x + 1u, id.y);

        dst[base + 1u] = a.z | (b.x << 16u);
        dst[base + 2u] = b.y | (b.z << 16u);
    } else {
        dst[base + 1u] = a.z;
    }
}
