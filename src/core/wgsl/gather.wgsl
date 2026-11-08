// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// What the CPU's analysis reads of a session's frame kept on the GPU,
// read back instead of the frame: proxy, the analysis
// image (src/pipeline/pipeline.c's proxy: the center pixel of each 2^shift square,
// clamped at the edges; shift 0: the frame); tiles, the noise estimate's
// windows (src/core/noise.c's grid: tile r's 18 x 18, its pixels and a
// border of 1, at 18 x (r % nx, r / nx) of a mosaic). A thread 2 pixels of
// tight 16-bit R'G'B' rows, every word written; copies only (the CPU's
// values, bit for bit).

struct Params {
    width: u32,         // the frame's size
    height: u32,
    pitch: u32,         // frame: words a row
    shift: u32,         // proxy: the reduction
    proxy_w: u32,
    proxy_h: u32,
    proxy_stride: u32,  // out: words a row of the proxy
    nx: u32,            // tiles: the grid (src/core/noise.h's hdrg_noise_grid_t)
    ny: u32,
    step: u32,
    first_x: u32,
    first_y: u32,
    mosaic_stride: u32, // out: words a row of the mosaic
    tiles_at: u32,      // out: the mosaic's first word
    pad0: u32,
    pad1: u32,
}

const TILE: u32 = 16u;
const WINDOW: u32 = 18u;

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> frame: array<u32>;
@group(0) @binding(2) var<storage, read_write> dst: array<u32>;

// sample c of the frame's pixel (x, y)
fn sample_at(x: u32, y: u32, c: u32) -> u32 {
    let i = y * params.pitch * 2u + x * 3u + c;

    return (frame[i / 2u] >> (16u * (i % 2u))) & 0xffffu;
}

// pixels (x0, y0) and (x1, y1) (a second one when two) into the 3 words at
// dst[at] (the second pixel's half of the middle word 0 when one)
fn put_pair(at: u32, x0: u32, y0: u32, x1: u32, y1: u32, two: bool) {
    dst[at] = sample_at(x0, y0, 0u) | (sample_at(x0, y0, 1u) << 16u);

    if (two) {
        dst[at + 1u] = sample_at(x0, y0, 2u) | (sample_at(x1, y1, 0u) << 16u);
        dst[at + 2u] = sample_at(x1, y1, 1u) | (sample_at(x1, y1, 2u) << 16u);
    } else {
        dst[at + 1u] = sample_at(x0, y0, 2u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn proxy(@builtin(global_invocation_id) id: vec3u) {
    let i = 2u * id.x;
    let j = id.y;

    if (i >= params.proxy_w || j >= params.proxy_h) {
        return;
    }

    let center = (1u << params.shift) / 2u;
    let y = min((j << params.shift) + center, params.height - 1u);
    let x0 = min((i << params.shift) + center, params.width - 1u);
    let x1 = min(((i + 1u) << params.shift) + center, params.width - 1u);

    put_pair(j * params.proxy_stride + 3u * id.x, x0, y, x1, y, i + 1u < params.proxy_w);
}

@compute @workgroup_size(64, 1, 1)
fn tiles(@builtin(global_invocation_id) id: vec3u) {
    let mx = 2u * id.x;
    let my = id.y;

    if (mx >= WINDOW * params.nx || my >= WINDOW * params.ny) {
        return;
    }

    // a window is 18 wide: both pixels in one tile
    let x0 = (params.first_x + (mx / WINDOW) * params.step) * TILE - 1u + mx % WINDOW;
    let y = (params.first_y + (my / WINDOW) * params.step) * TILE - 1u + my % WINDOW;

    put_pair(params.tiles_at + my * params.mosaic_stride + 3u * id.x, x0, y, x0 + 1u, y, true);
}
