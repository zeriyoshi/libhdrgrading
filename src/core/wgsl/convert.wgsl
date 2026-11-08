// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// Planar Y'CbCr (Cb and Cr in planes of their own or alternating in one:
// pairs) to 16-bit R'G'B' (src/core/yuv.c's hdrg_yuv_to_rgb16) on the GPU
// for a session's input; samples in the high bits of 16
// read through the range. sites: a thread a chroma site,
// the luma filtered to it and its centered chroma. slopes: a thread a site,
// the slopes of Cb and Cr against that luma over its 3 x 3 sites (each
// column's sums down the rows, then across), and under SEGMENTED the bent
// line (knots at the window's darkest, middle and brightest luma, fitted
// through the sites' taps). pixels: a thread 2 pixels, the chroma between
// the nearest sites with the luma's detail (and the bend), every word of
// rows of out_stride words written. The same operations in the same order
// as the CPU's, in f32 (the CPU's normal equations are f64); a GPU may fuse
// a multiply and an add: the output is within L2 of the CPU's.
// A var declared in a loop has an initializer: lavapipe (Tint's SPIR-V) does
// not zero one without it on each iteration.

const FIT_NONE: u32 = 0u;      // 4:4:4: the chroma as it is
const FIT_LINEAR: u32 = 1u;    // HDRG_CHROMA_FIT_LINEAR
const FIT_SEGMENTED: u32 = 2u; // HDRG_CHROMA_FIT_SEGMENTED
const CFL_EPS: f32 = 0.00025;
const CFL_MAX: f32 = 2.0;
const SEG_FLOOR: f32 = 0.000246;
const SEG_SPAN: f32 = 0.047;
const SEG_RIDGE: f32 = 0.05;
const SEG_FULL: f32 = 0.3;
const SEG_NONE: f32 = 0.7;
const SEG_SPREAD: f32 = 0.25;
const SITE: u32 = 0u;   // base's planes of sites: the site luma,
const SLOPE: u32 = 1u;  // the slopes of Cb, Cr,
const CHROMA: u32 = 3u; // Cb, Cr centered,
const BENT: u32 = 5u;   // 1 where the bent line holds (knots, values)

struct Params {
    width: u32,      // the luma's size
    height: u32,
    cw: u32,         // the chroma's
    ch: u32,
    bytes: u32,      // 1 or 2 bytes a sample
    y_stride: u32,   // yuv: words a row of the Y' plane, of a chroma plane
    c_stride: u32,
    cb_at: u32,      // yuv: words where Cb and Cr start
    cr_at: u32,
    dx: u32,         // the sites' offsets: columns, rows (3: not subsampled)
    dy: u32,
    fit: u32,        // FIT_*
    out_stride: u32, // rgb: words a row
    sites: u32,      // cw x ch: a plane of base, knots, values
    pairs: u32,      // 1: Cb, Cr alternating in the plane at cb_at (cr_at unused)
    pad0: u32,
    y_off: f32,      // the range: code = off + value / inv
    inv_y: f32,
    c_off: f32,
    inv_c: f32,
    cr_r: f32,       // R' = Y' + cr_r Cr, B' = Y' + cb_b Cb, G' = Y' - cb_g Cb - cr_g Cr
    cb_b: f32,
    cb_g: f32,
    cr_g: f32,
}

// sites between two subsampled samples: near (weight w) and far
struct Axis {
    near: u32,
    far: u32,
    w: f32,
}

// a site's taps on an axis
struct Taps {
    at: array<u32, 3>,
    w: array<f32, 3>,
    n: u32,
}

// the chroma at a pixel's column of sites: centered Cb, Cr, the site luma, the slopes
struct Chroma {
    cb: f32,
    cr: f32,
    site: f32,
    s0: f32,
    s1: f32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> yuv: array<u32>;
@group(0) @binding(2) var<storage, read_write> base: array<f32>;
@group(0) @binding(3) var<storage, read_write> knots: array<f32>;  // 3 planes
@group(0) @binding(4) var<storage, read_write> values: array<f32>; // Cb's 3 planes, Cr's
@group(0) @binding(5) var<storage, read_write> rgb: array<u32>;

var<private> pix: array<f32, 49>; // segment: the luma pixels of a window of 3 x 3 sites

fn code_at(plane: u32, x: u32, y: u32) -> f32 {
    var at = params.cr_at;
    var stride = params.c_stride;
    var i = x;

    if (plane == 0u) {
        at = 0u;
        stride = params.y_stride;
    } else if (plane == 1u || params.pairs == 1u) {
        at = params.cb_at;
    }

    if (plane != 0u && params.pairs == 1u) {
        i = 2u * x + plane - 1u;
    }

    let row = at + y * stride;

    if (params.bytes == 1u) {
        return f32((yuv[row + i / 4u] >> (8u * (i % 4u))) & 0xffu);
    }

    return f32((yuv[row + i / 2u] >> (16u * (i % 2u))) & 0xffffu);
}

fn luma_at(x: u32, y: u32) -> f32 {
    return (code_at(0u, x, y) - params.y_off) * params.inv_y;
}

// hdrg_yuv_axis: luma index i between the sites d / 2 samples after the even ones, n of them
fn axis_of(i: u32, d: u32, n: u32) -> Axis {
    var a: Axis;

    if (2u * i < d) {
        a.near = 0u;
        a.far = 0u;
        a.w = 1.0;

        return a;
    }

    let q = 2u * i - d;

    a.near = min(q / 4u, n - 1u);
    a.far = min(a.near + 1u, n - 1u);
    a.w = f32(4u - q % 4u) / 4.0;

    return a;
}

// hdrg_yuv_taps: the luma samples of site k on an axis of n samples
fn taps_of(d: u32, k: u32, n: u32) -> Taps {
    let c = 2u * k + d / 2u;
    var t: Taps;

    if (d == 3u) {
        t.at[0] = k;
        t.w[0] = 1.0;
        t.n = 1u;

        return t;
    }

    if (d == 1u) {
        t.at[0] = 2u * k;
        t.at[1] = min(2u * k + 1u, n - 1u);
        t.w[0] = 0.5;
        t.w[1] = 0.5;
        t.n = 2u;

        return t;
    }

    t.at[0] = select(0u, min(c - 1u, n - 1u), c > 0u);
    t.at[1] = min(c, n - 1u);
    t.at[2] = min(c + 1u, n - 1u);
    t.w[0] = 0.25;
    t.w[1] = 0.5;
    t.w[2] = 0.25;
    t.n = 3u;

    return t;
}

fn site_luma(j: u32, k: u32) -> f32 {
    var tx = taps_of(params.dx, j, params.width);
    var ty = taps_of(params.dy, k, params.height);
    var s = 0.0;

    for (var b = 0u; b < ty.n; b++) {
        var row = 0.0;

        for (var a = 0u; a < tx.n; a++) {
            row += tx.w[a] * luma_at(tx.at[a], ty.at[b]);
        }

        s += ty.w[b] * row;
    }

    return s;
}

// hdrg_yuv_hats: the weights of luma l on the hats of knots k
fn hats_of(k: vec3f, l: f32) -> vec3f {
    if (l <= k.x) {
        return vec3f(1.0, 0.0, 0.0);
    }

    if (l >= k.z) {
        return vec3f(0.0, 0.0, 1.0);
    }

    if (l < k.y) {
        let t = (l - k.x) / (k.y - k.x);

        return vec3f(1.0 - t, t, 0.0);
    }

    let t = (l - k.y) / (k.z - k.y);

    return vec3f(0.0, 1.0 - t, t);
}

// hdrg_yuv_middle: the middle of the 3-means of pix[0..n) in [lo, hi]
fn middle_of(n: u32, lo: f32, hi: f32) -> f32 {
    var center = array<f32, 3>(lo, 0.5 * (lo + hi), hi);

    for (var iter = 0u; iter < 4u; iter++) {
        var sum = array<f32, 3>(0.0, 0.0, 0.0);
        var count = array<f32, 3>(0.0, 0.0, 0.0);

        for (var i = 0u; i < n; i++) {
            var best = 0u;
            var nearest = (pix[i] - center[0]) * (pix[i] - center[0]);

            for (var c = 1u; c < 3u; c++) {
                let d = (pix[i] - center[c]) * (pix[i] - center[c]);

                if (d < nearest) {
                    best = c;
                    nearest = d;
                }
            }

            sum[best] += pix[i];
            count[best] += 1.0;
        }

        for (var c = 0u; c < 3u; c++) {
            if (count[c] > 0.0) {
                center[c] = sum[c] / count[c];
            }
        }
    }

    return center[1];
}

// hdrg_yuv_solve3: a x = b by Cramer's rule (false: singular)
fn solve3(a: array<array<f32, 3>, 3>, b: array<f32, 3>, x: ptr<function, array<f32, 3>>) -> bool {
    let det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
              a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
              a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);

    if (!(det > 1e-12 || det < -1e-12)) {
        return false;
    }

    (*x)[0] = (b[0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
               a[0][1] * (b[1] * a[2][2] - a[1][2] * b[2]) +
               a[0][2] * (b[1] * a[2][1] - a[1][1] * b[2])) / det;
    (*x)[1] = (a[0][0] * (b[1] * a[2][2] - a[1][2] * b[2]) -
               b[0] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * b[2] - b[1] * a[2][0])) / det;
    (*x)[2] = (a[0][0] * (a[1][1] * b[2] - b[1] * a[2][1]) -
               a[0][1] * (a[1][0] * b[2] - b[1] * a[2][0]) +
               b[0] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) / det;

    return true;
}

// hdrg_yuv_segment: site (j, k)'s bent line, from the line through mean, means with its slopes
fn segment(j: u32, k: u32, mean: f32, means_in: array<f32, 2>, slope_in: array<f32, 2>) {
    let n_sites = params.sites;
    let j0 = select(0u, j - 1u, j > 0u);
    let j1 = min(j + 1u, params.cw - 1u);
    let k0 = select(0u, k - 1u, k > 0u);
    let k1 = min(k + 1u, params.ch - 1u);
    let at = k * params.cw + j;
    var means = means_in;
    var slope = slope_in;
    var chroma: array<array<f32, 2>, 9>;
    var hat: array<array<f32, 3>, 9>;
    var normal: array<array<f32, 3>, 3>;
    var line: array<array<f32, 3>, 2>;
    var fitted: array<array<f32, 3>, 2>;
    var residual: array<f32, 2>;
    var n = 0u;

    base[BENT * n_sites + at] = 0.0;

    for (var kk = k0; kk <= k1; kk++) {
        for (var jj = j0; jj <= j1; jj++) {
            let l = base[kk * params.cw + jj];

            for (var p = 0u; p < 2u; p++) {
                chroma[n][p] = base[(CHROMA + p) * n_sites + kk * params.cw + jj];

                let e = chroma[n][p] - (means[p] + slope[p] * (l - mean));

                residual[p] += e * e;
            }

            n++;
        }
    }

    if (residual[0] <= f32(n) * SEG_FLOOR && residual[1] <= f32(n) * SEG_FLOOR) {
        return;
    }

    // the luma pixels the window's taps read
    var t = taps_of(params.dx, j0, params.width);
    let x0 = t.at[0];

    t = taps_of(params.dy, k0, params.height);

    let y0 = t.at[0];

    t = taps_of(params.dx, j1, params.width);

    let wide = t.at[t.n - 1u] - x0 + 1u;

    t = taps_of(params.dy, k1, params.height);

    let tall = t.at[t.n - 1u] - y0 + 1u;
    var lo = 1.0;
    var hi = 0.0;

    for (var y = 0u; y < tall; y++) {
        for (var x = 0u; x < wide; x++) {
            let l = luma_at(x0 + x, y0 + y);

            pix[y * wide + x] = l;
            lo = min(lo, l);
            hi = max(hi, l);
        }
    }

    if (hi - lo < SEG_SPAN) {
        return;
    }

    let knot = vec3f(lo, clamp(middle_of(wide * tall, lo, hi), lo + 0.1 * (hi - lo),
                               hi - 0.1 * (hi - lo)), hi);

    // each sample's hats through its taps, the normal equations
    for (var r = 0u; r < 3u; r++) {
        normal[r][r] = SEG_RIDGE;
    }

    var s = 0u;

    for (var kk = k0; kk <= k1; kk++) {
        var ty = taps_of(params.dy, kk, params.height);

        for (var jj = j0; jj <= j1; jj++) {
            var tx = taps_of(params.dx, jj, params.width);

            for (var b = 0u; b < ty.n; b++) {
                for (var a = 0u; a < tx.n; a++) {
                    let h = hats_of(knot, pix[(ty.at[b] - y0) * wide + tx.at[a] - x0]);

                    hat[s][0] += ty.w[b] * tx.w[a] * h.x;
                    hat[s][1] += ty.w[b] * tx.w[a] * h.y;
                    hat[s][2] += ty.w[b] * tx.w[a] * h.z;
                }
            }

            for (var r = 0u; r < 3u; r++) {
                for (var c = 0u; c < 3u; c++) {
                    normal[r][c] += hat[s][r] * hat[s][c];
                }
            }

            s++;
        }
    }

    var beta = 0.0;
    var knot_at = array<f32, 3>(knot.x, knot.y, knot.z);

    for (var p = 0u; p < 2u; p++) {
        var rhs = array<f32, 3>(0.0, 0.0, 0.0);
        var solved = array<f32, 3>(0.0, 0.0, 0.0);

        for (var r = 0u; r < 3u; r++) {
            line[p][r] = means[p] + slope[p] * (knot_at[r] - mean);
            fitted[p][r] = line[p][r];
            rhs[r] = SEG_RIDGE * line[p][r];

            for (var q = 0u; q < n; q++) {
                rhs[r] += hat[q][r] * chroma[q][p];
            }
        }

        if (!solve3(normal, rhs, &solved)) {
            continue;
        }

        var res = 0.0;
        var bent = 0.0;

        for (var q = 0u; q < n; q++) {
            var e = chroma[q][p] -
                    (hat[q][0] * line[p][0] + hat[q][1] * line[p][1] + hat[q][2] * line[p][2]);

            res += e * e;
            e = chroma[q][p] - (hat[q][0] * solved[0] + hat[q][1] * solved[1] + hat[q][2] * solved[2]);
            bent += e * e;
        }

        if (res <= f32(n) * SEG_FLOOR) {
            continue;
        }

        let beta_p = clamp((SEG_NONE - bent / res) / (SEG_NONE - SEG_FULL), 0.0, 1.0);
        var c_lo = chroma[0][p];
        var c_hi = chroma[0][p];

        for (var q = 1u; q < n; q++) {
            c_lo = min(c_lo, chroma[q][p]);
            c_hi = max(c_hi, chroma[q][p]);
        }

        let spread = SEG_SPREAD * (c_hi - c_lo);

        for (var r = 0u; r < 3u; r++) {
            fitted[p][r] = clamp(line[p][r] + beta_p * (solved[r] - line[p][r]), c_lo - spread,
                                 c_hi + spread);
        }

        beta = max(beta, beta_p);
    }

    if (beta <= 0.0) {
        return;
    }

    // less the bent line's own mean over the site's taps
    var tx = taps_of(params.dx, j, params.width);
    var ty = taps_of(params.dy, k, params.height);
    var own: array<f32, 3>;

    for (var b = 0u; b < ty.n; b++) {
        for (var a = 0u; a < tx.n; a++) {
            let h = hats_of(knot, pix[(ty.at[b] - y0) * wide + tx.at[a] - x0]);

            own[0] += ty.w[b] * tx.w[a] * h.x;
            own[1] += ty.w[b] * tx.w[a] * h.y;
            own[2] += ty.w[b] * tx.w[a] * h.z;
        }
    }

    for (var p = 0u; p < 2u; p++) {
        let site = own[0] * fitted[p][0] + own[1] * fitted[p][1] + own[2] * fitted[p][2];

        for (var r = 0u; r < 3u; r++) {
            values[(p * 3u + r) * n_sites + at] = fitted[p][r] - site;
        }
    }

    knots[at] = knot.x;
    knots[n_sites + at] = knot.y;
    knots[2u * n_sites + at] = knot.z;
    base[BENT * n_sites + at] = 1.0;
}

// column j of a plane of base between the chroma rows of ay
fn plane_vertical(plane: u32, j: u32, ay: Axis) -> f32 {
    let at = plane * params.sites + j;

    if (ay.near == ay.far) {
        return base[at + ay.near * params.cw];
    }

    return ay.w * base[at + ay.near * params.cw] + (1.0 - ay.w) * base[at + ay.far * params.cw];
}

fn chroma_at(j: u32, ay: Axis) -> Chroma {
    var c: Chroma;

    c.cb = plane_vertical(CHROMA, j, ay);
    c.cr = plane_vertical(CHROMA + 1u, j, ay);
    c.site = plane_vertical(SITE, j, ay);
    c.s0 = plane_vertical(SLOPE, j, ay);
    c.s1 = plane_vertical(SLOPE + 1u, j, ay);

    return c;
}

// hdrg_yuv_bend: what the bent lines of the sites around pixel (x, y) add, centered chroma
fn bend_of(x: u32, y: u32, ax: Axis, ay: Axis) -> vec2f {
    let n_sites = params.sites;
    let nc = select(1u, 2u, ax.far != ax.near);
    let nr = select(1u, 2u, ay.far != ay.near);
    var cols = array<u32, 2>(ax.near, ax.far);
    var rows = array<u32, 2>(ay.near, ay.far);
    var col_w = array<f32, 2>(select(1.0, ax.w, nc == 2u), 1.0 - ax.w);
    var row_w = array<f32, 2>(select(1.0, ay.w, nr == 2u), 1.0 - ay.w);
    var out = vec2f(0.0, 0.0);
    var chord = false;
    var l = 0.0;
    var lo = 0.0;
    var hi = 0.0;
    var t = 0.0;

    for (var b = 0u; b < nr; b++) {
        for (var a = 0u; a < nc; a++) {
            let at = rows[b] * params.cw + cols[a];

            if (base[BENT * n_sites + at] == 0.0) {
                continue;
            }

            if (!chord) {
                l = code_at(0u, x, y);
                lo = l;
                hi = l;

                for (var yy = select(0u, y - 1u, y > 0u); yy <= min(y + 1u, params.height - 1u);
                     yy++) {
                    for (var xx = select(0u, x - 1u, x > 0u); xx <= min(x + 1u, params.width - 1u);
                         xx++) {
                        let f = code_at(0u, xx, yy);

                        lo = min(lo, f);
                        hi = max(hi, f);
                    }
                }

                l = (l - params.y_off) * params.inv_y;
                lo = (lo - params.y_off) * params.inv_y;
                hi = (hi - params.y_off) * params.inv_y;
                t = select(0.0, (l - lo) / (hi - lo), hi > lo);
                chord = true;
            }

            let knot = vec3f(knots[at], knots[n_sites + at], knots[2u * n_sites + at]);
            let h = hats_of(knot, l);
            let lo_h = hats_of(knot, lo);
            let hi_h = hats_of(knot, hi);
            let weight = row_w[b] * col_w[a];
            var at_l = vec2f(0.0, 0.0);
            var on_chord = vec2f(0.0, 0.0);

            for (var p = 0u; p < 2u; p++) {
                let v0 = values[(p * 3u) * n_sites + at];
                let v1 = values[(p * 3u + 1u) * n_sites + at];
                let v2 = values[(p * 3u + 2u) * n_sites + at];
                let line = base[(SLOPE + p) * n_sites + at] * (l - base[at]);
                let f_lo = lo_h.x * v0 + lo_h.y * v1 + lo_h.z * v2;
                let f_hi = hi_h.x * v0 + hi_h.y * v1 + hi_h.z * v2;

                at_l[p] = h.x * v0 + h.y * v1 + h.z * v2 - line;
                on_chord[p] = f_lo + t * (f_hi - f_lo) - line;
            }

            if (on_chord.x * on_chord.x + on_chord.y * on_chord.y <
                at_l.x * at_l.x + at_l.y * at_l.y) {
                at_l = on_chord;
            }

            out.x += weight * at_l.x;
            out.y += weight * at_l.y;
        }
    }

    return out;
}

fn code16_of(v: f32) -> u32 {
    return u32(clamp(v, 0.0, 1.0) * 65535.0 + 0.5);
}

// hdrg_yuv_put: R'G'B' of luma and centered chroma u, v
fn put_codes(luma: f32, u: f32, v: f32) -> vec3u {
    return vec3u(code16_of(luma + params.cr_r * v),
                 code16_of(luma - params.cb_g * u - params.cr_g * v),
                 code16_of(luma + params.cb_b * u));
}

fn pixel_of(x: u32, y: u32) -> vec3u {
    let luma = luma_at(x, y);
    var ay = Axis(y, y, 1.0);

    // 4:4:4: the chroma as it is
    if (params.fit == FIT_NONE) {
        return put_codes(luma, (code_at(1u, x, y) - params.c_off) * params.inv_c,
                         (code_at(2u, x, y) - params.c_off) * params.inv_c);
    }

    if (params.dy != 3u) {
        ay = axis_of(y, params.dy, params.ch);
    }

    let ax = axis_of(x, params.dx, params.cw);

    // hdrg_yuv_cfl_row: each column vertically, then between them (one column too)
    if (params.fit == FIT_LINEAR) {
        let w = ax.w;
        let d = luma - (w * plane_vertical(SITE, ax.near, ay) +
                        (1.0 - w) * plane_vertical(SITE, ax.far, ay));

        return put_codes(luma,
                         w * plane_vertical(CHROMA, ax.near, ay) +
                             (1.0 - w) * plane_vertical(CHROMA, ax.far, ay) +
                             (w * plane_vertical(SLOPE, ax.near, ay) +
                              (1.0 - w) * plane_vertical(SLOPE, ax.far, ay)) * d,
                         w * plane_vertical(CHROMA + 1u, ax.near, ay) +
                             (1.0 - w) * plane_vertical(CHROMA + 1u, ax.far, ay) +
                             (w * plane_vertical(SLOPE + 1u, ax.near, ay) +
                              (1.0 - w) * plane_vertical(SLOPE + 1u, ax.far, ay)) * d);
    }

    // hdrg_yuv_rgb_rows: the columns mixed only when they are two, then the bend
    let lo = chroma_at(ax.near, ay);
    var mid = lo;

    if (ax.far != ax.near) {
        let hi = chroma_at(ax.far, ay);

        mid.cb = ax.w * lo.cb + (1.0 - ax.w) * hi.cb;
        mid.cr = ax.w * lo.cr + (1.0 - ax.w) * hi.cr;
        mid.site = ax.w * lo.site + (1.0 - ax.w) * hi.site;
        mid.s0 = ax.w * lo.s0 + (1.0 - ax.w) * hi.s0;
        mid.s1 = ax.w * lo.s1 + (1.0 - ax.w) * hi.s1;
    }

    let bend = bend_of(x, y, ax, ay);

    mid.cb += bend.x;
    mid.cr += bend.y;

    let d = luma - mid.site;

    return put_codes(luma, mid.cb + mid.s0 * d, mid.cr + mid.s1 * d);
}

@compute @workgroup_size(64, 1, 1)
fn sites(@builtin(global_invocation_id) id: vec3u) {
    let j = id.x;
    let k = id.y;

    if (j >= params.cw || k >= params.ch) {
        return;
    }

    let at = k * params.cw + j;

    base[SITE * params.sites + at] = site_luma(j, k);
    base[CHROMA * params.sites + at] = (code_at(1u, j, k) - params.c_off) * params.inv_c;
    base[(CHROMA + 1u) * params.sites + at] = (code_at(2u, j, k) - params.c_off) * params.inv_c;
}

@compute @workgroup_size(64, 1, 1)
fn slopes(@builtin(global_invocation_id) id: vec3u) {
    let j = id.x;
    let k = id.y;

    if (j >= params.cw || k >= params.ch) {
        return;
    }

    let n_sites = params.sites;
    let k0 = select(0u, k - 1u, k > 0u);
    let k1 = min(k + 1u, params.ch - 1u);
    let lo = select(0u, j - 1u, j > 0u);
    let hi = min(j + 1u, params.cw - 1u);
    let rows = f32(k1 - k0 + 1u);
    let n = rows * f32(hi - lo + 1u);
    let at = k * params.cw + j;
    var s: array<f32, 8>;
    var means: array<f32, 2>;
    var slope: array<f32, 2>;

    for (var jj = lo; jj <= hi; jj++) {
        var col = array<f32, 8>(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

        for (var kk = k0; kk <= k1; kk++) {
            let a = kk * params.cw + jj;
            let l = base[a];

            col[0] += l;
            col[1] += l * l;

            for (var p = 0u; p < 2u; p++) {
                let c = base[(CHROMA + p) * n_sites + a];

                col[2u + p] += c;
                col[4u + p] += l * c;
                col[6u + p] += c * c;
            }
        }

        for (var i = 0u; i < 8u; i++) {
            s[i] += col[i];
        }
    }

    let mean = s[0] / n;
    let variance = max(s[1] / n - mean * mean, 0.0);

    for (var p = 0u; p < 2u; p++) {
        let cov = s[4u + p] / n - mean * (s[2u + p] / n);
        var m = cov / (variance + CFL_EPS);

        // the line: times the correlation's square (noise and texture do not follow)
        if (params.fit == FIT_LINEAR) {
            let vc = max(s[6u + p] / n - (s[2u + p] / n) * (s[2u + p] / n), 0.0);

            m *= select(0.0, cov * cov / (variance * vc), variance > 0.0 && vc > 0.0);
        }

        slope[p] = clamp(m, -CFL_MAX, CFL_MAX);
        base[(SLOPE + p) * n_sites + at] = slope[p];
        means[p] = s[2u + p] / n;
    }

    if (params.fit == FIT_SEGMENTED) {
        segment(j, k, mean, means, slope);
    }
}

@compute @workgroup_size(64, 1, 1)
fn pixels(@builtin(global_invocation_id) id: vec3u) {
    let x = 2u * id.x;
    let y = id.y;

    if (x >= params.width || y >= params.height) {
        return;
    }

    let a = pixel_of(x, y);
    let row = y * params.out_stride + 3u * id.x;

    rgb[row] = a.x | (a.y << 16u);

    if (x + 1u < params.width) {
        let b = pixel_of(x + 1u, y);

        rgb[row + 1u] = a.z | (b.x << 16u);
        rgb[row + 2u] = b.y | (b.z << 16u);
    } else {
        rgb[row + 1u] = a.z;
    }
}
