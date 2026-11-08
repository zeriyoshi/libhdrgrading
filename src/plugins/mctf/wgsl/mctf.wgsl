// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// hdrg.mctf (src/plugins/mctf/mctf.c) on the GPU, the CPU's integers and order,
// the same output. The block motion estimation of src/plugins/shared/blockme.c
// (hdrg.interp.mc's, written again here at phase 0 with the penalty's shift
// a parameter): a frame's half-resolution luma and its pyramid of 2 x 2
// means into its slot (a thread a sample); for each other frame of the
// window its blocks' vectors coarse to fine (a thread a block) into its own
// fields, each level's 3 x 3 median; the finest blocks' weights from their
// match (a thread a block); the output (a thread 2 pixels, every word
// written): each pixel the mean of the job's frame and the others along
// their vectors, weighted. WGSL has no 64-bit integers: the costs are pairs
// of words and the mean's sums are split at 12 bits, put together exactly.

struct Params {
    width: u32,      // the frames'
    height: u32,
    in_stride: u32,  // words a row of every frame of the window ...
    out_stride: u32, // ... of the output
    lv: u32,         // the dispatch's level
    slot_a: u32,     // the pyramid made, or the job's frame's
    slot_b: u32,     // the other frame's pyramid
    ref_index: u32,  // the other frame's fields and weights
    refs: u32,       // the output's other frames (f1 .. f4)
    shift: u32,      // the penalty a quarter pixel off the predictor: 257 << shift
    block_lo: u32,   // a block's mean |difference| (16-bit codes): weight 256 ...
    block_hi: u32,   // ... 0
    w0: u32,         // the other frames' weights by their distance (1/256)
    w1: u32,
    w2: u32,
    w3: u32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> src: array<u32>;
@group(0) @binding(2) var<storage, read_write> pyramid: array<u32>;
@group(0) @binding(3) var<storage, read_write> motion: array<i32>;
@group(0) @binding(4) var<storage, read> table: array<u32>;
@group(0) @binding(5) var<storage, read> f0: array<u32>;
@group(0) @binding(6) var<storage, read> f1: array<u32>;
@group(0) @binding(7) var<storage, read> f2: array<u32>;
@group(0) @binding(8) var<storage, read> f3: array<u32>;
@group(0) @binding(9) var<storage, read> f4: array<u32>;
@group(0) @binding(10) var<storage, read_write> dst: array<u32>;

// the table: TABLE words a level (its width, height, -, -, its blocks across
// and down), each slot's plane of each level in pyramid, each other frame's
// field of each level in motion (vectors as pairs), the scratch's place,
// each other frame's blocks' weights in motion, then tau by the luma's top
// 8 bits and the sample weights' 64 steps
const TABLE: u32 = 8u;
const LEVELS: u32 = 6u;
const PLANES: u32 = 48u;
const FIELDS: u32 = 78u;
const SCRATCH: u32 = 102u;
const WEIGHTS: u32 = 103u;
const TAUS: u32 = 107u;
const STEPS: u32 = 363u;
const BLOCK: u32 = 8u;
const RANGE: i32 = 6;
const QUARTER: i32 = 4;
const KR: u32 = 13933u;
const KG: u32 = 46871u;
const KB: u32 = 4732u;

// 16-bit sample c of pixel (x, y) of the window's frame f (0: the job's)
fn code(f: u32, x: u32, y: u32, c: u32) -> u32 {
    let s = y * params.in_stride * 2u + x * 3u + c;
    var word = 0u;

    switch f {
        case 0u: {
            word = f0[s >> 1u];
        }
        case 1u: {
            word = f1[s >> 1u];
        }
        case 2u: {
            word = f2[s >> 1u];
        }
        case 3u: {
            word = f3[s >> 1u];
        }
        default: {
            word = f4[s >> 1u];
        }
    }

    return (word >> ((s & 1u) * 16u)) & 0xffffu;
}

// 16-bit sample c of pixel (x, y) of the frame a pyramid is made of
fn src_code(x: u32, y: u32, c: u32) -> u32 {
    let s = y * params.in_stride * 2u + x * 3u + c;

    return (src[s >> 1u] >> ((s & 1u) * 16u)) & 0xffffu;
}

fn src_luma(x: u32, y: u32) -> u32 {
    return (KR * src_code(x, y, 0u) + KG * src_code(x, y, 1u) + KB * src_code(x, y, 2u) +
            32768u) >>
           16u;
}

// level lv's plane of slot s at (x, y) in 1/16 pixel, bilinear, the edges repeated: x 256
fn plane_sample(lv: u32, s: u32, xi: i32, yi: i32) -> u32 {
    let w = table[lv * TABLE];
    let h = table[lv * TABLE + 1u];
    let at = table[PLANES + s * LEVELS + lv];
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

// channel c of the window's frame f at (x, y) in 1/16 pixel, bilinear, the edges repeated: x 256
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

// block (bx, by)'s cost at level lv for v (its quarter pixels) about pred, at phase 0 (the job's
// frame's block at q found in the other at q + v): the sum of absolute differences and 257 <<
// shift the distance
fn cost(lv: u32, bx: u32, by: u32, v: vec2i, pred: vec2i) -> vec2u {
    let w = table[lv * TABLE];
    let h = table[lv * TABLE + 1u];
    let vx = v.x * QUARTER;
    let vy = v.y * QUARTER;
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

            let sa = plane_sample(lv, params.slot_a, i32(x) * 16, i32(y) * 16);
            let sb = plane_sample(lv, params.slot_b, i32(x) * 16 + vx, i32(y) * 16 + vy);

            sad = sad + select(sb - sa, sa - sb, sa > sb);
        }
    }

    let low = m << params.shift;
    let total = low + sad;

    return vec2u(total, (m >> (32u - params.shift)) + select(0u, 1u, total < low));
}

fn store(at: u32, v: vec2i) {
    motion[at] = v.x;
    motion[at + 1u] = v.y;
}

fn load(at: u32) -> vec2i {
    return vec2i(motion[at], motion[at + 1u]);
}

// the other frame's field of level lv
fn field(lv: u32) -> u32 {
    return table[FIELDS + params.ref_index * LEVELS + lv];
}

// pixel (x, y) of level 0's plane of slot_a from the frame src: 2 x 2 means of the luma
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

    pyramid[table[PLANES + params.slot_a * LEVELS] + id.y * w + id.x] =
        (src_luma(x0, y0) + src_luma(x1, y0) + src_luma(x0, y1) + src_luma(x1, y1) + 2u) >> 2u;
}

// pixel (x, y) of level lv's plane of slot_a from the level below: 2 x 2 means
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
    let below = table[PLANES + params.slot_a * LEVELS + params.lv - 1u];
    let r0 = below + y0 * fw;
    let r1 = below + y1 * fw;

    pyramid[table[PLANES + params.slot_a * LEVELS + params.lv] + id.y * w + id.x] =
        (pyramid[r0 + x0] + pyramid[r0 + x1] + pyramid[r1 + x0] + pyramid[r1 + x1] + 2u) >> 2u;
}

// block (bx, by) of the coarsest level searched +-RANGE pixels into the scratch
@compute @workgroup_size(8, 8)
fn coarse(@builtin(global_invocation_id) id: vec3u) {
    let bw = table[params.lv * TABLE + 4u];

    if (id.x >= bw || id.y >= table[params.lv * TABLE + 5u]) {
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
    let parent = field(params.lv + 1u);

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

    // the steps of hdrg_bme_refine: 1 and 1/2 of the level's pixels, at the finest 1/4 too
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

// the middle of 9 values (an insertion sort, hdrg_bme_middle)
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
    let bw = table[params.lv * TABLE + 4u];
    let bh = table[params.lv * TABLE + 5u];
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

    store(field(params.lv) + 2u * (id.y * bw + id.x), vec2i(middle(xs), middle(ys)));
}

// block (bx, by) of the finest field: its weight from its match along its vector against the noise
// (the low half of its word; the high half the match's mean |difference|, the side data's cost)
@compute @workgroup_size(8, 8)
fn blocks(@builtin(global_invocation_id) id: vec3u) {
    let w = table[0];
    let h = table[1];
    let bw = table[4];

    if (id.x >= bw || id.y >= table[5]) {
        return;
    }

    let v = load(field(0u) + 2u * (id.y * bw + id.x));
    let sad = cost(0u, id.x, id.y, v, v).x;
    let n = min(BLOCK, w - id.x * BLOCK) * min(BLOCK, h - id.y * BLOCK);
    let mean = sad / (n * 256u);
    var weight = 0u;

    if (mean <= params.block_lo) {
        weight = 256u;
    } else if (mean < params.block_hi) {
        weight = (256u * (params.block_hi - mean) + (params.block_hi - params.block_lo) / 2u) /
                 (params.block_hi - params.block_lo);
    }

    motion[table[WEIGHTS + params.ref_index] + id.y * bw + id.x] =
        bitcast<i32>(weight | (min(mean, 0xfffeu) << 16u));
}

// the bilinear between the block centers at full-resolution pixel (x, y): the blocks' corner and
// its weights (hdrg_bme_vector's)
fn corner(x: u32, y: u32) -> vec4u {
    let bw = table[4];
    let bh = table[5];
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

    return vec4u(bx0, by0, wx, wy);
}

// the finest field of other frame k at the corner: 1/16 pixel (hdrg_bme_vector)
fn vector_at(k: u32, at: vec4u) -> vec2i {
    let bw = table[4];
    let bh = table[5];
    let field = table[FIELDS + k * LEVELS];
    let bx1 = select(at.x, at.x + 1u, at.x + 1u < bw);
    let by1 = select(at.y, at.y + 1u, at.y + 1u < bh);
    let v00 = load(field + 2u * (at.y * bw + at.x)) * 8;
    let v10 = load(field + 2u * (at.y * bw + bx1)) * 8;
    let v01 = load(field + 2u * (by1 * bw + at.x)) * 8;
    let v11 = load(field + 2u * (by1 * bw + bx1)) * 8;
    let w00 = (256u - at.z) * (256u - at.w);
    let w10 = at.z * (256u - at.w);
    let w01 = (256u - at.z) * at.w;
    let w11 = at.z * at.w;

    // the weights' sum is 65536: the low halves' weighted sum stays within a word
    let high = i32(w00) * (v00 >> vec2u(16u)) + i32(w10) * (v10 >> vec2u(16u)) +
               i32(w01) * (v01 >> vec2u(16u)) + i32(w11) * (v11 >> vec2u(16u));
    let low = w00 * (vec2u(v00) & vec2u(0xffffu)) + w10 * (vec2u(v10) & vec2u(0xffffu)) +
              w01 * (vec2u(v01) & vec2u(0xffffu)) + w11 * (vec2u(v11) & vec2u(0xffffu));

    return high + vec2i((low + vec2u(32768u)) >> vec2u(16u));
}

// a block's weight of its word (blocks's)
fn block_weight(at: u32) -> u32 {
    return bitcast<u32>(motion[at]) & 0xffffu;
}

// the blocks' weights of other frame k at the corner (hdrg_mt_weight_at's)
fn weight_at(k: u32, at: vec4u) -> u32 {
    let bw = table[4];
    let bh = table[5];
    let weights = table[WEIGHTS + k];
    let bx1 = select(at.x, at.x + 1u, at.x + 1u < bw);
    let by1 = select(at.y, at.y + 1u, at.y + 1u < bh);

    return ((256u - at.z) * (256u - at.w) * block_weight(weights + at.y * bw + at.x) +
            at.z * (256u - at.w) * block_weight(weights + at.y * bw + bx1) +
            (256u - at.z) * at.w * block_weight(weights + by1 * bw + at.x) +
            at.z * at.w * block_weight(weights + by1 * bw + bx1) + 32768u) >>
           16u;
}

// output pixel (x, y): the job's frame and the others along their vectors, weighted
// (hdrg_mt_out_rows): sum (w s) over sum (w 256) of samples x 256, split at 12 bits
fn pixel(x: u32, y: u32) -> vec3u {
    let own = vec3u(code(0u, x, y, 0u), code(0u, x, y, 1u), code(0u, x, y, 2u));
    let luma = (KR * own.x + KG * own.y + KB * own.z + 32768u) >> 16u;
    let tau = table[TAUS + (luma >> 8u)];
    let at = corner(x, y);
    var high = ((own << vec3u(8u)) >> vec3u(12u)) << vec3u(8u);
    var low = ((own << vec3u(8u)) & vec3u(4095u)) << vec3u(8u);
    var den = 65536u;
    var out = vec3u(0u);

    for (var k = 0u; k < params.refs; k = k + 1u) {
        let v = vector_at(k, at);
        var s = vec3u(0u);
        var d = 0u;

        for (var c = 0u; c < 3u; c = c + 1u) {
            s[c] = rgb_sample(k + 1u, i32(x) * 16 + v.x, i32(y) * 16 + v.y, c);
            d = d + select((own[c] << 8u) - s[c], s[c] - (own[c] << 8u), s[c] > (own[c] << 8u));
        }

        d = d / 3u;

        let i = min(((d >> 8u) << 4u) / tau, 63u);
        let distance = select(select(select(params.w3, params.w2, k == 2u), params.w1, k == 1u),
                              params.w0, k == 0u);
        let w = (distance * table[STEPS + i] * weight_at(k, at) + 32768u) >> 16u;

        high = high + w * (s >> vec3u(12u));
        low = low + w * (s & vec3u(4095u));
        den = den + (w << 8u);
    }

    // (high 4096 + low + den / 2) / den, exactly: high = qa den + ra
    for (var c = 0u; c < 3u; c = c + 1u) {
        let qa = high[c] / den;
        let ra = high[c] - qa * den;

        out[c] = min(qa * 4096u + (ra * 4096u + low[c] + den / 2u) / den, 65535u);
    }

    return out;
}

// output pixels 2 x and 2 x + 1 of row y: three words
@compute @workgroup_size(8, 8)
fn average(@builtin(global_invocation_id) id: vec3u) {
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
