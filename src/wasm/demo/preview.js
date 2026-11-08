// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// The interactive preview: the worker gives the state of the library's apply
// shader for the settings (hdrg_wasm_preview_*: the analysis and the plan in WASM), this page runs
// that WGSL ("apply", HDRG_SHADER_LAYOUT's bindings) on the browser's WebGPU and shows its 16-bit
// PQ output on an HDR canvas (rgba16float, display-p3, toneMapping "extended": the values are
// sRGB-encoded, 1.0 = SDR white). A slider sends a new request when the last one has come back,
// so the latest settings always win.

const DISPLAY_WGSL = /* wgsl */ `
struct View {
    size: vec2u,
    dst_pitch: u32,
    src_pitch: u32,
    split: f32,
    white: f32,
    gamut: u32,
    pad: u32,
}

@group(0) @binding(0) var<uniform> view: View;
@group(0) @binding(1) var<storage, read> dst: array<u32>;
@group(0) @binding(2) var<storage, read> src: array<u32>;

struct Out {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vertex_main(@builtin(vertex_index) i: u32) -> Out {
    var out: Out;
    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));

    out.position = vec4f(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = vec2f(p.x, 1.0 - p.y);

    return out;
}

fn half_at(buffer: u32, byte: u32) -> u32 {
    var word = 0u;

    if (buffer == 0u) {
        word = dst[byte >> 2u];
    } else {
        word = src[byte >> 2u];
    }

    return (word >> ((byte & 2u) * 8u)) & 0xffffu;
}

fn byte_at(byte: u32) -> u32 {
    return (src[byte >> 2u] >> ((byte & 3u) * 8u)) & 0xffu;
}

fn pq_nits(code: f32) -> f32 {
    let e = pow(clamp(code, 0.0, 1.0), 1.0 / 78.84375);
    let n = max(e - 0.8359375, 0.0) / (18.8515625 - 18.6875 * e);

    return 10000.0 * pow(n, 1.0 / 0.1593017578125);
}

fn srgb_linear(v: vec3f) -> vec3f {
    return select(pow((v + 0.055) / 1.055, vec3f(2.4)), v / 12.92, v <= vec3f(0.04045));
}

// the extended sRGB curve of the canvas (sign mirrored)
fn srgb_encode(v: vec3f) -> vec3f {
    let a = abs(v);
    let e = select(1.055 * pow(a, vec3f(1.0 / 2.4)) - 0.055, a * 12.92, a <= vec3f(0.0031308));

    return sign(v) * e;
}

// linear RGB of a gamut (HDRG_GAMUT_*: 0 BT.709, 1 Display P3, 2 BT.2020) in Display P3
fn to_p3(rgb: vec3f, gamut: u32) -> vec3f {
    if (gamut == 0u) {
        return mat3x3f(0.822462, 0.033194, 0.017083, 0.177538, 0.966806, 0.072397,
                       0.0, 0.0, 0.910520) * rgb;
    }

    if (gamut == 2u) {
        return mat3x3f(1.343578, -0.065297, 0.002822, -0.282180, 1.075788, -0.019598,
                       -0.061399, -0.010490, 1.016777) * rgb;
    }

    return rgb;
}

@fragment
fn fragment_main(in: Out) -> @location(0) vec4f {
    let p = min(vec2u(in.uv * vec2f(view.size)), view.size - 1u);
    let x = f32(p.x) / f32(view.size.x);
    var rgb = vec3f(0.0);

    if (abs(x - view.split) * f32(view.size.x) < 1.0 && view.split > 0.0 && view.split < 1.0) {
        return vec4f(1.0, 1.0, 1.0, 1.0);
    }

    if (x < view.split) {
        let at = p.y * view.src_pitch + p.x * 4u;
        let code = vec3f(f32(byte_at(at)), f32(byte_at(at + 1u)), f32(byte_at(at + 2u))) / 255.0;

        rgb = to_p3(srgb_linear(code), 0u);
    } else {
        let at = p.y * view.dst_pitch + p.x * 6u;
        let code = vec3f(f32(half_at(0u, at)), f32(half_at(0u, at + 2u)), f32(half_at(0u, at + 4u)));
        let nits = vec3f(pq_nits(code.r / 65535.0), pq_nits(code.g / 65535.0), pq_nits(code.b / 65535.0));

        rgb = to_p3(nits / view.white, view.gamut);
    }

    return vec4f(srgb_encode(rgb), 1.0);
}
`;

export class Preview {
    constructor(canvas, shader) {
        this.canvas = canvas;
        this.shader = shader;
        this.device = null;
        this.size = null;
        this.rgba = null;
        this.buffers = {};
        this.split = 0.5;
        this.state = null;
    }

    static supported() {
        return 'gpu' in navigator;
    }

    async init() {
        const adapter = await navigator.gpu.requestAdapter();

        if (adapter === null) {
            throw new Error('WebGPU のアダプタがありません');
        }

        this.device = await adapter.requestDevice({
            requiredLimits: { maxStorageBufferBindingSize: adapter.limits.maxStorageBufferBindingSize,
                              maxBufferSize: adapter.limits.maxBufferSize },
        });
        this.context = this.canvas.getContext('webgpu');
        this.context.configure({ device: this.device, format: 'rgba16float', colorSpace: 'display-p3',
                                 toneMapping: { mode: 'extended' }, alphaMode: 'opaque' });

        const storage = (type) => ({ type });
        const entry = (binding, visibility, buffer) => ({ binding, visibility, buffer });

        this.applyLayout = this.device.createBindGroupLayout({
            entries: [
                entry(0, GPUShaderStage.COMPUTE, storage('uniform')),
                entry(1, GPUShaderStage.COMPUTE, storage('read-only-storage')),
                entry(2, GPUShaderStage.COMPUTE, storage('read-only-storage')),
                entry(3, GPUShaderStage.COMPUTE, storage('read-only-storage')),
                entry(4, GPUShaderStage.COMPUTE, storage('storage')),
                entry(5, GPUShaderStage.COMPUTE, storage('storage')),
                entry(6, GPUShaderStage.COMPUTE, storage('storage')),
            ],
        });
        this.apply = this.device.createComputePipeline({
            layout: this.device.createPipelineLayout({ bindGroupLayouts: [this.applyLayout] }),
            compute: { module: this.device.createShaderModule({ code: this.shader }), entryPoint: 'apply' },
        });

        const display = this.device.createShaderModule({ code: DISPLAY_WGSL });

        this.displayLayout = this.device.createBindGroupLayout({
            entries: [
                entry(0, GPUShaderStage.FRAGMENT, storage('uniform')),
                entry(1, GPUShaderStage.FRAGMENT, storage('read-only-storage')),
                entry(2, GPUShaderStage.FRAGMENT, storage('read-only-storage')),
            ],
        });
        this.display = this.device.createRenderPipeline({
            layout: this.device.createPipelineLayout({ bindGroupLayouts: [this.displayLayout] }),
            vertex: { module: display, entryPoint: 'vertex_main' },
            fragment: { module: display, entryPoint: 'fragment_main', targets: [{ format: 'rgba16float' }] },
            primitive: { topology: 'triangle-list' },
        });
        this.view = this.buffer('view', 32, GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST);
        this.light = this.buffer('light', 4, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
        this.planes = this.buffer('planes', 4, GPUBufferUsage.STORAGE);
    }

    // a buffer of at least size bytes under name, made again when it is too small
    buffer(name, size, usage) {
        const bytes = Math.max(4, Math.ceil(size / 4) * 4);
        const old = this.buffers[name];

        if (old !== undefined && old.size >= bytes && old.usage === usage) {
            return old;
        }

        old?.destroy();
        this.buffers[name] = this.device.createBuffer({ size: bytes, usage });

        return this.buffers[name];
    }

    // the still's 8-bit RGBA (rows of width x 4 bytes)
    setImage(rgba, width, height) {
        this.rgba = rgba;
        this.size = { width, height };
        this.canvas.width = width;
        this.canvas.height = height;
        this.sourcePitch = 0;
        this.state = null;
    }

    // data (an ArrayBuffer or a typed array) as bytes: writeBuffer counts a typed array's elements
    upload(buffer, data) {
        const bytes = data instanceof ArrayBuffer ? new Uint8Array(data)
            : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);

        this.device.queue.writeBuffer(buffer, 0, bytes, 0, Math.floor(bytes.byteLength / 4) * 4);
    }

    // one state of the worker: its apply on the GPU, then the canvas
    run(state) {
        const { width, height } = this.size;
        const usage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;

        if (state.sourcePitch !== this.sourcePitch) {
            const rows = new Uint8Array(state.sourcePitch * height);

            for (let y = 0; y < height; y++) {
                rows.set(this.rgba.subarray(y * width * 4, (y + 1) * width * 4), y * state.sourcePitch);
            }

            this.upload(this.buffer('source', rows.byteLength, usage), rows);
            this.sourcePitch = state.sourcePitch;
        }

        const params = this.buffer('params', state.params.byteLength,
                                   GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST);
        const luts = this.buffer('luts', state.luts.byteLength, usage);
        const frame = this.buffer('frame', state.frame.byteLength, usage);
        const dst = this.buffer('dst', state.dstSize, GPUBufferUsage.STORAGE);

        this.upload(params, state.params);
        this.upload(luts, state.luts);
        this.upload(frame, state.frame);
        this.upload(this.light, new Uint32Array(1));
        this.state = state;

        const encoder = this.device.createCommandEncoder();
        const pass = encoder.beginComputePass();

        pass.setPipeline(this.apply);
        pass.setBindGroup(0, this.device.createBindGroup({
            layout: this.applyLayout,
            entries: [params, this.buffers.source, luts, frame, dst, this.light, this.planes]
                .map((buffer, binding) => ({ binding, resource: { buffer } })),
        }));
        pass.dispatchWorkgroups(state.groups[0], state.groups[1]);
        pass.end();
        this.device.queue.submit([encoder.finish()]);
        this.draw();
    }

    // the canvas: the source left of the split, the output right of it
    draw() {
        if (this.state === null) {
            return;
        }

        const { width, height } = this.size;
        const view = new ArrayBuffer(32);
        const u32 = new Uint32Array(view);
        const f32 = new Float32Array(view);

        u32[0] = width;
        u32[1] = height;
        u32[2] = this.state.dstPitch;
        u32[3] = this.sourcePitch;
        f32[4] = this.split;
        f32[5] = this.state.white;
        u32[6] = this.state.gamut;
        this.device.queue.writeBuffer(this.view, 0, view);

        const encoder = this.device.createCommandEncoder();
        const pass = encoder.beginRenderPass({
            colorAttachments: [{ view: this.context.getCurrentTexture().createView(), loadOp: 'clear',
                                 storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }],
        });

        pass.setPipeline(this.display);
        pass.setBindGroup(0, this.device.createBindGroup({
            layout: this.displayLayout,
            entries: [this.view, this.buffers.dst, this.buffers.source]
                .map((buffer, binding) => ({ binding, resource: { buffer } })),
        }));
        pass.draw(3);
        pass.end();
        this.device.queue.submit([encoder.finish()]);
    }

    setSplit(split) {
        this.split = split;
        this.draw();
    }
}
