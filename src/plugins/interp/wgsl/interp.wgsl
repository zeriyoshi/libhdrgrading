// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.interp.mc (src/plugins/interp/interp_mc.c) on the GPU, the CPU's integers and
// order, the same output: the half-resolution luma of both frames and the
// pyramid of 2 x 2 means (a thread a sample); each level's 8 x 8 blocks'
// vectors coarse to fine (a thread a block: the coarsest level searched
// +-6 pixels, the finer ones from the parent's 5 predictors refined by 1,
// 1/2 and at the finest 1/4 of their pixels, the first best kept as the CPU
// keeps it), each into the scratch and its 3 x 3 median into the level's
// field; the output (a thread 2 pixels, every word written); the finest
// field's records of the output's HDRG_SD_MOTION (a thread a block). WGSL has no
// 64-bit integers: the costs are pairs of words, the products split into
// high and low bits and put together exactly.

struct Params {
    width: u32,      // the frames'
    height: u32,
    a_stride: u32,   // words a row of the first frame ...
    b_stride: u32,   // ... of the second
    out_stride: u32, // ... of the output
    phase: u32,      // the output's place between the frames, 1/65536
    lv: u32,         // the dispatch's level
    levels: u32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> frame_a: array<u32>;
@group(0) @binding(2) var<storage, read> frame_b: array<u32>;
@group(0) @binding(3) var<storage, read_write> pyramid: array<u32>;
@group(0) @binding(4) var<storage, read_write> fields: array<i32>;
@group(0) @binding(5) var<storage, read> table: array<u32>;
@group(0) @binding(6) var<storage, read_write> dst: array<u32>;
@group(0) @binding(7) var<storage, read_write> records: array<u32>;

// the table: TABLE words a level (its width, height, the first frame's
// plane and the second's in pyramid, its blocks across and down, its field
// in fields: vectors as pairs), then the scratch's place
const TABLE: u32 = 8u;
const SCRATCH: u32 = 48u;
const BLOCK: u32 = 8u;
const RANGE: i32 = 6;
const QUARTER: i32 = 4;
const KR: u32 = 13933u;
const KG: u32 = 46871u;
const KB: u32 = 4732u;
const NEAR_LO: u32 = 526336u;  // 8 x 257 x 256
const NEAR_HI: u32 = 2105344u; // 32 x 257 x 256
const NEAR_STEP: u32 = 6168u;  // (NEAR_HI - NEAR_LO) / 256
const SURE: u32 = 2056u;       // a block's mean |difference| 8 x 257: confidence 255 ...
const UNSURE: u32 = 8224u;     // ... 32 x 257: none

// 16-bit sample c of pixel (x, y) of the first frame (f 0) or the second
fn code(f: u32, x: u32, y: u32, c: u32) -> u32 {
    let stride = select(params.b_stride, params.a_stride, f == 0u);
    let s = y * stride * 2u + x * 3u + c;
    var word = 0u;

    if (f == 0u) {
        word = frame_a[s >> 1u];
    } else {
        word = frame_b[s >> 1u];
    }

    return (word >> ((s & 1u) * 16u)) & 0xffffu;
}

fn luma(f: u32, x: u32, y: u32) -> u32 {
    return (KR * code(f, x, y, 0u) + KG * code(f, x, y, 1u) + KB * code(f, x, y, 2u) + 32768u) >>
           16u;
}

// floor((phase v + 32768) / 65536) of phase up to 65536, exactly
fn mul_round(phase: u32, v: i32) -> i32 {
    let high = v >> 16u;
    let low = u32(v) & 0xffffu;

    return i32(phase) * high + i32((phase * low + 32768u) >> 16u);
}

// level lv's plane of frame f at (x, y) in 1/16 pixel, bilinear, the edges repeated: x 256
fn plane_sample(lv: u32, f: u32, xi: i32, yi: i32) -> u32 {
    let base = lv * TABLE;
    let w = table[base];
    let h = table[base + 1u];
    let at = table[base + 2u + f];
    let x = clamp(xi, 0, i32(w - 1u) * 16);
    let y = clamp(yi, 0, i32(h - 1u) * 16);
    let x0 = u32(x) >> 4u;
    let y0 = u32(y) >> 4u;
    let fx = u32(x) & 15u;
    let fy = u32(y) & 15u;
    let x1 = select(x0, x0 + 1u, x0 + 1u < w);
    let y1 = select(y0, y0 + 1u, y0 + 1u < h);
    let r0 = at + y0 * w;
    let r1 = at + y1 * w;

    return (16u - fx) * (16u - fy) * pyramid[r0 + x0] + fx * (16u - fy) * pyramid[r0 + x1] +
           (16u - fx) * fy * pyramid[r1 + x0] + fx * fy * pyramid[r1 + x1];
}

// channel c of frame f at (x, y) in 1/16 pixel, bilinear, the edges repeated: x 256
fn rgb_sample(f: u32, xi: i32, yi: i32, c: u32) -> u32 {
    let x = clamp(xi, 0, i32(params.width - 1u) * 16);
    let y = clamp(yi, 0, i32(params.height - 1u) * 16);
    let x0 = u32(x) >> 4u;
    let y0 = u32(y) >> 4u;
    let fx = u32(x) & 15u;
    let fy = u32(y) & 15u;
    let x1 = select(x0, x0 + 1u, x0 + 1u < params.width);
    let y1 = select(y0, y0 + 1u, y0 + 1u < params.height);

    return (16u - fx) * (16u - fy) * code(f, x0, y0, c) + fx * (16u - fy) * code(f, x1, y0, c) +
           (16u - fx) * fy * code(f, x0, y1, c) + fx * fy * code(f, x1, y1, c);
}

// a < b of costs (low word, high word)
fn less(a: vec2u, b: vec2u) -> bool {
    return a.y < b.y || (a.y == b.y && a.x < b.x);
}

// block (bx, by)'s cost at level lv for v (its quarter pixels) about pred:
// the sum of absolute differences and 2105344 (257 x 2^13) the distance
fn cost(lv: u32, bx: u32, by: u32, v: vec2i, pred: vec2i) -> vec2u {
    let w = table[lv * TABLE];
    let h = table[lv * TABLE + 1u];
    let vx = v.x * QUARTER;
    let vy = v.y * QUARTER;
    let ax = mul_round(params.phase, vx);
    let ay = mul_round(params.phase, vy);
    let bx16 = vx - ax;
    let by16 = vy - ay;
    let n = min(u32(abs(v.x - pred.x)) + u32(abs(v.y - pred.y)), 0xffffffu);
    let m = n * 257u;
    var sad = 0u;

    for (var j = 0u; j < BLOCK; j = j + 1u) {
        let y = by * BLOCK + j;

        if (y >= h) {
            break;
        }

        for (var i = 0u; i < BLOCK; i = i + 1u) {
            let x = bx * BLOCK + i;

            if (x >= w) {
                break;
            }

            let sa = plane_sample(lv, 0u, i32(x) * 16 - ax, i32(y) * 16 - ay);
            let sb = plane_sample(lv, 1u, i32(x) * 16 + bx16, i32(y) * 16 + by16);

            sad = sad + select(sb - sa, sa - sb, sa > sb);
        }
    }

    let low = m << 13u;
    let total = low + sad;

    return vec2u(total, (m >> 19u) + select(0u, 1u, total < low));
}

fn store(at: u32, v: vec2i) {
    fields[at] = v.x;
    fields[at + 1u] = v.y;
}

fn load(at: u32) -> vec2i {
    return vec2i(fields[at], fields[at + 1u]);
}

// pixel (x, y) of level 0's planes from the frames: 2 x 2 means of the luma
@compute @workgroup_size(8, 8)
fn lumas(@builtin(global_invocation_id) id: vec3u) {
    let w = table[0];
    let h = table[1];

    if (id.x >= w || id.y >= h) {
        return;
    }

    let y0 = 2u * id.y;
    let y1 = select(y0, y0 + 1u, y0 + 1u < params.height);
    let x0 = 2u * id.x;
    let x1 = select(x0, x0 + 1u, x0 + 1u < params.width);

    for (var f = 0u; f < 2u; f = f + 1u) {
        pyramid[table[2u + f] + id.y * w + id.x] =
            (luma(f, x0, y0) + luma(f, x1, y0) + luma(f, x0, y1) + luma(f, x1, y1) + 2u) >> 2u;
    }
}

// pixel (x, y) of level lv's planes from the level below: 2 x 2 means
@compute @workgroup_size(8, 8)
fn down(@builtin(global_invocation_id) id: vec3u) {
    let base = params.lv * TABLE;
    let fine = base - TABLE;
    let w = table[base];
    let h = table[base + 1u];
    let fw = table[fine];
    let fh = table[fine + 1u];

    if (id.x >= w || id.y >= h) {
        return;
    }

    let y0 = 2u * id.y;
    let y1 = select(y0, y0 + 1u, y0 + 1u < fh);
    let x0 = 2u * id.x;
    let x1 = select(x0, x0 + 1u, x0 + 1u < fw);

    for (var f = 0u; f < 2u; f = f + 1u) {
        let r0 = table[fine + 2u + f] + y0 * fw;
        let r1 = table[fine + 2u + f] + y1 * fw;

        pyramid[table[base + 2u + f] + id.y * w + id.x] =
            (pyramid[r0 + x0] + pyramid[r0 + x1] + pyramid[r1 + x0] + pyramid[r1 + x1] + 2u) >> 2u;
    }
}

// block (bx, by) of the coarsest level searched +-RANGE pixels into the scratch
@compute @workgroup_size(8, 8)
fn coarse(@builtin(global_invocation_id) id: vec3u) {
    let base = params.lv * TABLE;
    let bw = table[base + 4u];

    if (id.x >= bw || id.y >= table[base + 5u]) {
        return;
    }

    let zero = vec2i(0, 0);
    var best = zero;
    var best_cost = cost(params.lv, id.x, id.y, zero, zero);

    for (var dy = -RANGE; dy <= RANGE; dy = dy + 1) {
        for (var dx = -RANGE; dx <= RANGE; dx = dx + 1) {
            let v = vec2i(dx * QUARTER, dy * QUARTER);
            let c = cost(params.lv, id.x, id.y, v, zero);

            if (less(c, best_cost)) {
                best = v;
                best_cost = c;
            }
        }
    }

    // the finest level searched alone (a small frame): refined to 1/2 and 1/4 pixel
    for (var r = 1u; params.lv == 0u && r < 3u; r = r + 1u) {
        let step = QUARTER >> r;
        let center = best;

        for (var dy = -step; dy <= step; dy = dy + step) {
            for (var dx = -step; dx <= step; dx = dx + step) {
                if (dx == 0 && dy == 0) {
                    continue;
                }

                let v = center + vec2i(dx, dy);
                let c = cost(params.lv, id.x, id.y, v, zero);

                if (less(c, best_cost)) {
                    best = v;
                    best_cost = c;
                }
            }
        }
    }

    store(table[SCRATCH] + 2u * (id.y * bw + id.x), best);
}

// block (bx, by) of a finer level: the parent's vector and its neighbors' refined, into the scratch
@compute @workgroup_size(8, 8)
fn refine(@builtin(global_invocation_id) id: vec3u) {
    let base = params.lv * TABLE;
    let up = base + TABLE;
    let bw = table[base + 4u];
    let pw = table[up + 4u];
    let ph = table[up + 5u];
    let parent = table[up + 6u];

    if (id.x >= bw || id.y >= table[base + 5u]) {
        return;
    }

    let px = min(id.x / 2u, pw - 1u);
    let py = min(id.y / 2u, ph - 1u);
    var preds = array<vec2i, 5>(
        load(parent + 2u * (py * pw + px)),
        load(parent + 2u * (py * pw + select(px, px - 1u, px > 0u))),
        load(parent + 2u * (py * pw + select(px, px + 1u, px + 1u < pw))),
        load(parent + 2u * (select(py, py - 1u, py > 0u) * pw + px)),
        load(parent + 2u * (select(py, py + 1u, py + 1u < ph) * pw + px)));

    for (var k = 0u; k < 5u; k = k + 1u) {
        preds[k] = preds[k] * 2;
    }

    let pred = preds[0];
    var best = pred;
    var best_cost = cost(params.lv, id.x, id.y, pred, pred);

    for (var k = 1u; k < 5u; k = k + 1u) {
        let c = cost(params.lv, id.x, id.y, preds[k], pred);

        if (less(c, best_cost)) {
            best = preds[k];
            best_cost = c;
        }
    }

    // the steps of hdrg_mc_refine: 1 and 1/2 of the level's pixels, at the finest 1/4 too
    let rounds = select(2u, 3u, params.lv == 0u);

    for (var r = 0u; r < rounds; r = r + 1u) {
        let step = QUARTER >> r;
        let center = best;

        for (var dy = -step; dy <= step; dy = dy + step) {
            for (var dx = -step; dx <= step; dx = dx + step) {
                if (dx == 0 && dy == 0) {
                    continue;
                }

                let v = center + vec2i(dx, dy);
                let c = cost(params.lv, id.x, id.y, v, pred);

                if (less(c, best_cost)) {
                    best = v;
                    best_cost = c;
                }
            }
        }
    }

    store(table[SCRATCH] + 2u * (id.y * bw + id.x), best);
}

// the middle of 9 values (an insertion sort, hdrg_mc_middle)
fn middle(values: array<i32, 9>) -> i32 {
    var v = values;

    for (var i = 1u; i < 9u; i = i + 1u) {
        let t = v[i];
        var j = i;

        while (j > 0u && v[j - 1u] > t) {
            v[j] = v[j - 1u];
            j = j - 1u;
        }

        v[j] = t;
    }

    return v[4];
}

// block (bx, by) of level lv's field: the 3 x 3 median of the scratch on each axis
@compute @workgroup_size(8, 8)
fn median(@builtin(global_invocation_id) id: vec3u) {
    let base = params.lv * TABLE;
    let bw = table[base + 4u];
    let bh = table[base + 5u];
    let scratch = table[SCRATCH];
    var xs = array<i32, 9>(0, 0, 0, 0, 0, 0, 0, 0, 0);
    var ys = array<i32, 9>(0, 0, 0, 0, 0, 0, 0, 0, 0);
    var n = 0u;

    if (id.x >= bw || id.y >= bh) {
        return;
    }

    for (var dy = -1; dy <= 1; dy = dy + 1) {
        for (var dx = -1; dx <= 1; dx = dx + 1) {
            let x = u32(clamp(i32(id.x) + dx, 0, i32(bw) - 1));
            let y = u32(clamp(i32(id.y) + dy, 0, i32(bh) - 1));
            let v = load(scratch + 2u * (y * bw + x));

            xs[n] = v.x;
            ys[n] = v.y;
            n = n + 1u;
        }
    }

    store(table[base + 6u] + 2u * (id.y * bw + id.x), vec2i(middle(xs), middle(ys)));
}

// the finest field's vector at full-resolution pixel (x, y): 1/16 pixel (hdrg_mc_vector)
fn vector_at(x: u32, y: u32) -> vec2i {
    let bw = table[4];
    let bh = table[5];
    let field = table[6];
    let px = max(i32(x) * 16 - 120, 0);
    let py = max(i32(y) * 16 - 120, 0);
    var bx0 = u32(px) >> 8u;
    var by0 = u32(py) >> 8u;
    var wx = u32(px) & 255u;
    var wy = u32(py) & 255u;

    if (bx0 + 1u >= bw) {
        bx0 = bw - 1u;
        wx = 0u;
    }

    if (by0 + 1u >= bh) {
        by0 = bh - 1u;
        wy = 0u;
    }

    let bx1 = select(bx0, bx0 + 1u, bx0 + 1u < bw);
    let by1 = select(by0, by0 + 1u, by0 + 1u < bh);
    let v00 = load(field + 2u * (by0 * bw + bx0)) * 8;
    let v10 = load(field + 2u * (by0 * bw + bx1)) * 8;
    let v01 = load(field + 2u * (by1 * bw + bx0)) * 8;
    let v11 = load(field + 2u * (by1 * bw + bx1)) * 8;
    let w00 = (256u - wx) * (256u - wy);
    let w10 = wx * (256u - wy);
    let w01 = (256u - wx) * wy;
    let w11 = wx * wy;

    // the weights' sum is 65536: the low halves' weighted sum stays within a word
    let high = i32(w00) * (v00 >> vec2u(16u)) + i32(w10) * (v10 >> vec2u(16u)) +
               i32(w01) * (v01 >> vec2u(16u)) + i32(w11) * (v11 >> vec2u(16u));
    let low = w00 * (vec2u(v00) & vec2u(0xffffu)) + w10 * (vec2u(v10) & vec2u(0xffffu)) +
              w01 * (vec2u(v01) & vec2u(0xffffu)) + w11 * (vec2u(v11) & vec2u(0xffffu));

    return high + vec2i((low + vec2u(32768u)) >> vec2u(16u));
}

// output pixel (x, y): both frames along its vector, mixed (hdrg_mc_out_rows)
fn pixel(x: u32, y: u32) -> vec3u {
    let v = vector_at(x, y);
    let ax = mul_round(params.phase, v.x);
    let ay = mul_round(params.phase, v.y);
    var sa = vec3u(0u);
    var sb = vec3u(0u);
    var d = 0u;
    var t = 0u;
    var out = vec3u(0u);

    for (var c = 0u; c < 3u; c = c + 1u) {
        sa[c] = rgb_sample(0u, i32(x) * 16 - ax, i32(y) * 16 - ay, c);
        sb[c] = rgb_sample(1u, i32(x) * 16 + (v.x - ax), i32(y) * 16 + (v.y - ay), c);
        d = max(d, select(sb[c] - sa[c], sa[c] - sb[c], sa[c] > sb[c]));
    }

    // where the samples disagree, toward the nearer frame: ((d - lo) << 16) / (hi - lo)
    if (d >= NEAR_HI) {
        t = 65536u;
    } else if (d > NEAR_LO) {
        t = ((d - NEAR_LO) << 8u) / NEAR_STEP;
    }

    var wb = select(((params.phase * (65536u - t) + 32768u) >> 16u), 65536u - t,
                    params.phase == 65536u);

    if (params.phase >= 32768u) {
        wb = wb + t;
    }

    // (sa (65536 - wb) + sb wb + 2^23) >> 24 from 12-bit halves of the samples
    for (var c = 0u; c < 3u; c = c + 1u) {
        let high = (sa[c] >> 12u) * (65536u - wb) + (sb[c] >> 12u) * wb;
        let low = (sa[c] & 4095u) * (65536u - wb) + (sb[c] & 4095u) * wb + 8388608u;

        out[c] = min((high >> 12u) + ((((high & 4095u) << 12u) + low) >> 24u), 65535u);
    }

    return out;
}

// output pixels 2 x and 2 x + 1 of row y: three words
@compute @workgroup_size(8, 8)
fn compose(@builtin(global_invocation_id) id: vec3u) {
    let x0 = 2u * id.x;

    if (x0 >= params.width || id.y >= params.height) {
        return;
    }

    let p = pixel(x0, id.y);
    var q = vec3u(0u);

    if (x0 + 1u < params.width) {
        q = pixel(x0 + 1u, id.y);
    }

    let at = id.y * params.out_stride + id.x * 3u;

    dst[at] = p.x | (p.y << 16u);
    dst[at + 1u] = p.z | (q.x << 16u);
    dst[at + 2u] = q.y | (q.z << 16u);
}

// block (bx, by) of the finest field: its record (hdrg_mc_record_rows), two words: the vector in
// the frame's quarter pixels (x low, y high), then the mean |difference| and the confidence
@compute @workgroup_size(8, 8)
fn confidence(@builtin(global_invocation_id) id: vec3u) {
    let w = table[0];
    let h = table[1];
    let bw = table[4];

    if (id.x >= bw || id.y >= table[5]) {
        return;
    }

    let v = load(table[6] + 2u * (id.y * bw + id.x));
    let sad = cost(0u, id.x, id.y, v, v).x;
    let n = min(BLOCK, w - id.x * BLOCK) * min(BLOCK, h - id.y * BLOCK);
    let mean = sad / (n * 256u);
    let mv = clamp(2 * v, vec2i(-32768), vec2i(32767));
    var sure = 0u;

    if (mean <= SURE) {
        sure = 255u;
    } else if (mean < UNSURE) {
        sure = (255u * (UNSURE - mean) + (UNSURE - SURE) / 2u) / (UNSURE - SURE);
    }

    let at = 2u * (id.y * bw + id.x);

    records[at] = (u32(mv.x) & 0xffffu) | (u32(mv.y) << 16u);
    records[at + 1u] = min(mean, 0xfffeu) | (sure << 16u);
}
