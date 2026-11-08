// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// The planes of a session's frames on the app's GPU (video.h's
// hdrg_device_frame_t) into and out of the core's buffers:
// copy_rows, a thread a word, rows words of each row from src (rows of src_pitch
// words from src_at) to dst (rows of dst_pitch words from dst_at). The app's
// offsets and pitches are whole words; a row's last word may carry the
// padding after its samples (within the pitch).

struct Params {
    rows: u32,
    words: u32,     // words a row
    src_at: u32,
    src_pitch: u32,
    dst_at: u32,
    dst_pitch: u32,
    pad0: u32,
    pad1: u32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> src: array<u32>;
@group(0) @binding(2) var<storage, read_write> dst: array<u32>;

@compute @workgroup_size(64, 1, 1)
fn copy_rows(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.words || id.y >= params.rows) {
        return;
    }

    dst[params.dst_at + id.y * params.dst_pitch + id.x] =
        src[params.src_at + id.y * params.src_pitch + id.x];
}
