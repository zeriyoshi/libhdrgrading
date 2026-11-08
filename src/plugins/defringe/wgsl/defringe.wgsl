// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.webgpu.defringe: hdrg.defringe (src/plugins/defringe/defringe.c) on the GPU, on
// a whole RGB16 frame in four passes: Y', B' - Y' and R' - Y' (planes), the
// 3 x 3 Y' ranges over the edge threshold (edges), those dilated across
// reach - 1 columns (wides), the means (fringe, two pixels a thread so the
// output is written in whole words). The same integers as the CPU's, each
// output sample from the input alone: the output equals the CPU's.

struct Params {
    width: u32,
    height: u32,
    in_stride: u32,  // words a row of the input
    out_stride: u32, // ... and of the output
    reach: u32,      // the window's radius, 1 + 2 x strength
    pad0: u32,
    pad1: u32,
    pad2: u32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read_write> luma: array<i32>;
@group(0) @binding(3) var<storage, read_write> db: array<i32>;
@group(0) @binding(4) var<storage, read_write> dr: array<i32>;
@group(0) @binding(5) var<storage, read_write> edge: array<u32>;
@group(0) @binding(6) var<storage, read_write> wide: array<u32>;
@group(0) @binding(7) var<storage, read_write> dst: array<u32>;

const EDGE: i32 = 6168;  // 24 x 257: a 3 x 3 Y' range from which a sample is on an edge
const LUMA: i32 = 1542;  // 6 x 257: the neighbors' largest Y' difference (on an edge: half)
const CB: i32 = 15260;   // and Cb difference, 32 codes, as B' - Y' (x 1.8556)
const CR: i32 = 12951;   // and Cr difference, 32 codes, as R' - Y' (x 1.5748)
const KR: i32 = 13933;   // BT.709 luma weights in 1/65536
const KG: i32 = 46871;
const KB: i32 = 4732;

fn sample(x: u32, y: u32, c: u32) -> u32 {
    let s = x * 3u + c;
    let word = source[y * params.in_stride + (s >> 1u)];

    return (word >> ((s & 1u) * 16u)) & 0xffffu;
}

// sum / count rounded half away from zero
fn mean(sum: i32, count: i32) -> i32 {
    if (sum >= 0) {
        return (sum + count / 2) / count;
    }

    return -((-sum + count / 2) / count);
}

fn code(v: i32) -> u32 {
    return u32(clamp(v, 0, 65535));
}

@compute @workgroup_size(8, 8)
fn planes(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.width || id.y >= params.height) {
        return;
    }

    let i = id.y * params.width + id.x;
    let r = sample(id.x, id.y, 0u);
    let g = sample(id.x, id.y, 1u);
    let b = sample(id.x, id.y, 2u);
    let y = i32((r * 13933u + g * 46871u + b * 4732u + 32768u) >> 16u);

    luma[i] = y;
    db[i] = i32(b) - y;
    dr[i] = i32(r) - y;
}

@compute @workgroup_size(8, 8)
fn edges(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.width || id.y >= params.height) {
        return;
    }

    let w = params.width;
    let x0 = select(id.x - 1u, 0u, id.x == 0u);
    let x1 = min(id.x + 1u, w - 1u);
    let y0 = select(id.y - 1u, 0u, id.y == 0u);
    let y1 = min(id.y + 1u, params.height - 1u);
    var lo = luma[id.y * w + id.x];
    var hi = lo;

    for (var y = y0; y <= y1; y++) {
        for (var x = x0; x <= x1; x++) {
            lo = min(lo, luma[y * w + x]);
            hi = max(hi, luma[y * w + x]);
        }
    }

    edge[id.y * w + id.x] = select(0u, 1u, hi - lo >= EDGE);
}

@compute @workgroup_size(8, 8)
fn wides(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.width || id.y >= params.height) {
        return;
    }

    let w = params.width;
    let side = params.reach - 1u;
    let x0 = select(id.x - side, 0u, id.x < side);
    let x1 = min(id.x + side, w - 1u);
    var found = 0u;

    for (var x = x0; x <= x1; x++) {
        found |= edge[id.y * w + x];
    }

    wide[id.y * w + id.x] = found;
}

// the defringed pixel (x, y): R', G', B'
fn defringe(x: u32, y: u32) -> vec3u {
    let w = params.width;
    let h = params.height;
    let reach = params.reach;
    let side = reach - 1u;
    let source_pixel = vec3u(sample(x, y, 0u), sample(x, y, 1u), sample(x, y, 2u));
    var near = false;

    for (var b = select(y - side, 0u, y < side); b <= min(y + side, h - 1u); b++) {
        if (wide[b * w + x] != 0u) {
            near = true;
            break;
        }
    }

    if (!near) {
        return source_pixel;
    }

    let i = y * w + x;
    let l = luma[i];
    let cb = db[i];
    let cr = dr[i];
    let range = select(LUMA, LUMA / 2, edge[i] != 0u);
    var sum_b = 0;
    var sum_r = 0;
    var count = 0;

    for (var b = select(y - reach, 0u, y < reach); b <= min(y + reach, h - 1u); b++) {
        for (var a = select(x - reach, 0u, x < reach); a <= min(x + reach, w - 1u); a++) {
            let j = b * w + a;

            if (abs(luma[j] - l) > range || abs(db[j] - cb) > CB || abs(dr[j] - cr) > CR) {
                continue;
            }

            sum_b += db[j];
            sum_r += dr[j];
            count++;
        }
    }

    let mb = mean(sum_b, count);
    let mr = mean(sum_r, count);

    // an unchanged chroma keeps its bytes (G' from a rounded Y' could move by one)
    if (mb == cb && mr == cr) {
        return source_pixel;
    }

    return vec3u(code(l + mr), code(l - mean(KR * mr + KB * mb, KG)), code(l + mb));
}

@compute @workgroup_size(8, 8)
fn fringe(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;

    if (x >= params.width || id.y >= params.height) {
        return;
    }

    let a = defringe(x, id.y);
    let base = id.y * params.out_stride + id.x * 3u;

    // an odd width's last pixel: its words alone (a row's stride may end two bytes past it)
    if (x + 1u >= params.width) {
        dst[base] = a.x | (a.y << 16u);
        dst[base + 1u] = a.z;

        return;
    }

    let b = defringe(x + 1u, id.y);

    dst[base] = a.x | (a.y << 16u);
    dst[base + 1u] = a.z | (b.x << 16u);
    dst[base + 2u] = b.y | (b.z << 16u);
}
