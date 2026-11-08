// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.webgpu.deblock: hdrg.deblock (src/plugins/deblock/deblock.c) on the GPU, on a
// whole RGB16 frame in two passes: the vertical boundaries of the 8-pixel
// grid into the output (verticals: a thread the 8 pixels a boundary reads,
// from 4 before it to 4 after, every output word written), then the
// horizontal ones over that output in place (horizontals: a thread 2 pixels
// of the 8 rows a boundary reads). No boundary reads samples another one
// moves, and the integers are the CPU's: the output equals the CPU's.

struct Params {
    width: u32,
    height: u32,
    in_stride: u32,  // words a row of the input
    out_stride: u32, // ... and of the output
    alpha: i32,      // the Y' step across a boundary below which its line is filtered
    beta: i32,       // ... and the steps next to it
    tc: i32,         // the largest move of a sample
    pad0: u32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> source: array<u32>;
@group(0) @binding(2) var<storage, read_write> dst: array<u32>;

const GRID: u32 = 8u;  // the blocks' grid, from the frame's origin
const REACH: u32 = 4u; // the samples a boundary reads on each side
const KR: u32 = 13933u; // BT.709 luma weights in 1/65536 for the decisions
const KG: u32 = 46871u;
const KB: u32 = 4732u;

fn code(v: i32) -> i32 {
    return clamp(v, 0, 65535);
}

// a line across a boundary, p3 p2 p1 p0 | q0 q1 q2 q3: sample c of its i-th
// pixel at i * 3 + c, filtered as hdrg_db_line does
fn deblock(v: array<i32, 24>) -> array<i32, 24> {
    let alpha = params.alpha;
    let beta = params.beta;
    let tc = params.tc;
    var o = v;
    var y: array<i32, 8>;

    for (var i = 0u; i < 8u; i++) {
        y[i] = i32((u32(v[i * 3u]) * KR + u32(v[i * 3u + 1u]) * KG + u32(v[i * 3u + 2u]) * KB +
                    32768u) >> 16u);
    }

    let d = y[4] - y[3];

    if (abs(d) >= alpha || abs(y[2] - y[3]) >= beta || abs(y[5] - y[4]) >= beta) {
        return o;
    }

    let ap = abs(y[1] - y[3]);
    let aq = abs(y[6] - y[4]);
    let strong = ap < beta && aq < beta && abs(d) < alpha / 4 + tc;

    for (var c = 0u; c < 3u; c++) {
        let p3 = v[c];
        let p2 = v[3u + c];
        let p1 = v[6u + c];
        let p0 = v[9u + c];
        let q0 = v[12u + c];
        let q1 = v[15u + c];
        let q2 = v[18u + c];
        let q3 = v[21u + c];

        if (strong) {
            o[3u + c] = code(clamp((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3u, p2 - 2 * tc,
                                   p2 + 2 * tc));
            o[6u + c] = code(clamp((p2 + p1 + p0 + q0 + 2) >> 2u, p1 - 2 * tc, p1 + 2 * tc));
            o[9u + c] = code(clamp((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3u, p0 - 2 * tc,
                                   p0 + 2 * tc));
            o[12u + c] = code(clamp((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3u, q0 - 2 * tc,
                                    q0 + 2 * tc));
            o[15u + c] = code(clamp((p0 + q0 + q1 + q2 + 2) >> 2u, q1 - 2 * tc, q1 + 2 * tc));
            o[18u + c] = code(clamp((p0 + q0 + q1 + 3 * q2 + 2 * q3 + 4) >> 3u, q2 - 2 * tc,
                                    q2 + 2 * tc));

            continue;
        }

        let delta = clamp(((q0 - p0) * 4 + (p1 - q1) + 4) >> 3u, -tc, tc);
        let mid = (p0 + q0 + 1) >> 1u;
        var a = p1;
        var b = q1;

        if (ap < beta) {
            a += clamp((p2 + mid - 2 * p1) >> 1u, -tc / 2, tc / 2);
        }

        if (aq < beta) {
            b += clamp((q2 + mid - 2 * q1) >> 1u, -tc / 2, tc / 2);
        }

        o[6u + c] = code(a);
        o[9u + c] = code(p0 + delta);
        o[12u + c] = code(q0 - delta);
        o[15u + c] = code(b);
    }

    return o;
}

fn sample(x: u32, y: u32, c: u32) -> i32 {
    let s = x * 3u + c;
    let word = source[y * params.in_stride + (s >> 1u)];

    return i32((word >> ((s & 1u) * 16u)) & 0xffffu);
}

// the pixels [first, last) of a row around the boundary id.x * GRID (none
// for id.x 0: the row's first pixels), filtered when the boundary is one
@compute @workgroup_size(8, 8)
fn verticals(@builtin(global_invocation_id) id: vec3u) {
    let w = params.width;
    let x = id.x * GRID;

    if (id.y >= params.height || x >= w + REACH) {
        return;
    }

    let first = select(x - REACH, 0u, id.x == 0u);
    let last = min(x + REACH, w);
    let n = (last - first) * 3u;
    let base = id.y * params.out_stride + first * 3u / 2u;
    var v: array<i32, 24>;

    for (var i = 0u; i < n; i++) {
        v[i] = sample(first + i / 3u, id.y, i % 3u);
    }

    if (id.x > 0u && x + REACH <= w) {
        v = deblock(v);
    }

    // whole words: an odd width's last sample with the two bytes after it
    for (var j = 0u; j * 2u < n; j++) {
        var high = 0u;

        if (j * 2u + 1u < n) {
            high = u32(v[j * 2u + 1u]) << 16u;
        }

        dst[base + j] = u32(v[j * 2u]) | high;
    }
}

// the pixels id.x * 2 and the next of the rows around the boundary
// id.y * GRID, filtered in place when it is one
@compute @workgroup_size(8, 8)
fn horizontals(@builtin(global_invocation_id) id: vec3u) {
    let x = id.x * 2u;
    let top = id.y * GRID;

    if (x >= params.width || id.y == 0u || top + REACH > params.height) {
        return;
    }

    let pair = x + 1u < params.width;
    var a: array<i32, 24>;
    var b: array<i32, 24>;

    for (var i = 0u; i < 8u; i++) {
        let base = (top - REACH + i) * params.out_stride + id.x * 3u;
        let w0 = dst[base];
        let w1 = dst[base + 1u];
        var w2 = 0u;

        if (pair) {
            w2 = dst[base + 2u];
        }

        a[i * 3u] = i32(w0 & 0xffffu);
        a[i * 3u + 1u] = i32(w0 >> 16u);
        a[i * 3u + 2u] = i32(w1 & 0xffffu);
        b[i * 3u] = i32(w1 >> 16u);
        b[i * 3u + 1u] = i32(w2 & 0xffffu);
        b[i * 3u + 2u] = i32(w2 >> 16u);
    }

    a = deblock(a);

    if (pair) {
        b = deblock(b);
    }

    // the rows a boundary moves: p2 to q2
    for (var i = 1u; i < 7u; i++) {
        let base = (top - REACH + i) * params.out_stride + id.x * 3u;

        dst[base] = u32(a[i * 3u]) | (u32(a[i * 3u + 1u]) << 16u);

        if (pair) {
            dst[base + 1u] = u32(a[i * 3u + 2u]) | (u32(b[i * 3u]) << 16u);
            dst[base + 2u] = u32(b[i * 3u + 1u]) | (u32(b[i * 3u + 2u]) << 16u);
        } else {
            dst[base + 1u] = u32(a[i * 3u + 2u]) | (dst[base + 1u] & 0xffff0000u);
        }
    }
}
