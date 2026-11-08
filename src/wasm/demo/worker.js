// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// Runs the conversion off the main thread (the pthreads build blocks while its workers run). One
// context lives as long as the worker, so the thread pool is created once.
// Messages: { type: 'init', build } -> { type: 'ready', version, shader }
//           { type: 'convert', input, preset, settings } -> { type: 'done', output, ms, tier }
//           { type: 'preview-image', rgba, width, height } (the still of the previews)
//           { type: 'preview', id, preset, settings } -> { type: 'preview', id, state, ms }
//           any of them -> { type: 'error', message, request }
// A preview (src/wasm/hdrg_wasm.h) is the state of the apply shader for the page's WebGPU: the
// analysis and the plan here, on a context of its own without a GPU (the state needs none).

const STATE_WORDS = 13;

let m = null;
let ctx = 0;
let previewCtx = 0;
let image = { ptr: 0, width: 0, height: 0 };

function setParam(params, name, value) {
    const key = m.stringToNewUTF8(name);
    const status = m._hdrg_wasm_params_set(params, key, value);

    m._free(key);

    if (status !== 0) {
        throw new Error(`${name} = ${value}: ${m.UTF8ToString(m._hdrg_status_string(status))}`);
    }
}

function createParams(preset, settings) {
    const params = m._hdrg_wasm_params_create(preset);

    if (params === 0) {
        throw new Error('out of memory');
    }

    try {
        for (const [name, value] of Object.entries(settings)) {
            setParam(params, name, value);
        }
    } catch (error) {
        m._hdrg_wasm_params_destroy(params);
        throw error;
    }

    return params;
}

async function convert(input, preset, settings) {
    const params = createParams(preset, settings);
    const ptr = m._malloc(input.byteLength);
    let output = 0;

    try {
        if (ptr === 0) {
            throw new Error('out of memory');
        }

        m.HEAPU8.set(new Uint8Array(input), ptr);

        const start = performance.now();

        output = await m._hdrg_wasm_convert(ctx, ptr, input.byteLength, params);

        const ms = performance.now() - start;
        const status = output === 0 ? 5 : m._hdrg_wasm_output_status(output);

        if (status !== 0) {
            throw new Error(m.UTF8ToString(m._hdrg_status_string(status)));
        }

        const data = m._hdrg_wasm_output_data(output);
        const bytes = m.HEAPU8.slice(data, data + m._hdrg_wasm_output_size(output));

        return { bytes, ms, tier: m.UTF8ToString(m._hdrg_simd_name(m._hdrg_wasm_last_tier(ctx))) };
    } finally {
        m._hdrg_wasm_output_free(output);
        m._free(ptr);
        m._hdrg_wasm_params_destroy(params);
    }
}

function setImage(rgba, width, height) {
    m._free(image.ptr);
    image = { ptr: m._malloc(rgba.byteLength), width, height };

    if (image.ptr === 0) {
        throw new Error('out of memory');
    }

    m.HEAPU8.set(new Uint8Array(rgba), image.ptr);
}

// the state's words as hdrg_wasm.h's HDRG_WASM_PREVIEW_*, its buffers copied out of the module
function preview(preset, settings) {
    if (image.ptr === 0) {
        throw new Error('no image');
    }

    const params = createParams(preset, settings);
    const start = performance.now();
    const handle = m._hdrg_wasm_preview_create(previewCtx, image.ptr, image.width, image.height, params);

    try {
        const status = handle === 0 ? 5 : m._hdrg_wasm_preview_status(handle);

        if (status !== 0) {
            throw new Error(m.UTF8ToString(m._hdrg_status_string(status)));
        }

        const at = m._hdrg_wasm_preview_state(handle);
        const w = new Uint32Array(m.HEAPU8.buffer, at, STATE_WORDS).slice();
        const copy = (ptr, size) => m.HEAPU8.slice(ptr, ptr + size).buffer;
        const state = {
            params: copy(w[0], w[1]),
            luts: copy(w[2], w[3]),
            frame: copy(w[4], w[5]),
            sourcePitch: w[6],
            dstSize: w[7],
            dstPitch: w[8],
            groups: [w[9], w[10]],
            gamut: w[11],
            white: w[12] / 1000,
            width: image.width,
            height: image.height,
        };

        return { state, ms: performance.now() - start };
    } finally {
        m._hdrg_wasm_preview_destroy(handle);
        m._hdrg_wasm_params_destroy(params);
    }
}

self.onmessage = async (event) => {
    const message = event.data;

    try {
        if (message.type === 'init') {
            const { default: createHdrgModule } = await import(`./hdrgrading-${message.build}.mjs`);

            m = await createHdrgModule();
            /* the WebGPU build: the browser's GPU for the built-in GPU filters (HDRG_GPU_AUTO) */
            ctx = m._hdrg_wasm_context_create(0, message.build.endsWith('-webgpu') ? 1 : 0);
            previewCtx = m._hdrg_wasm_context_create(0, 0);

            if (ctx === 0 || previewCtx === 0) {
                throw new Error('the context cannot be created');
            }

            self.postMessage({ type: 'ready', version: m.UTF8ToString(m._hdrg_version()),
                               shader: m.UTF8ToString(m._hdrg_wasm_preview_shader()) });

            return;
        }

        if (message.type === 'preview-image') {
            setImage(message.rgba, message.width, message.height);

            return;
        }

        if (message.type === 'preview') {
            const result = preview(message.preset, message.settings);
            const { params, luts, frame } = result.state;

            self.postMessage({ type: 'preview', id: message.id, state: result.state, ms: result.ms },
                             [params, luts, frame]);

            return;
        }

        const result = await convert(message.input, message.preset, message.settings);

        self.postMessage({ type: 'done', output: result.bytes.buffer, ms: result.ms, tier: result.tier },
                         [result.bytes.buffer]);
    } catch (error) {
        self.postMessage({ type: 'error', message: String(error.message || error), request: message.type });
    }
};
