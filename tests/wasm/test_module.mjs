// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// The browser module (src/wasm/hdrg_wasm.h) under node: the default conversion gives the same
// bytes as hdrg-cli of the same build, a rerun and one thread give the same bytes, every output
// form is a JPEG of its kind, the JPEG XL writer chosen by its id writes its form, bad parameters
// and writers are refused, and the interactive preview gives its state (the same twice) and the
// apply WGSL.
// usage: node test_module.mjs <hdrgrading-*.mjs> <input image> <hdrg-cli output of the input>

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const HDRG_OK = 0;
const HDRG_ERR_INVALID_ARG = 1;
const HDRG_ERR_NOT_FOUND = 10;

const [modulePath, inputPath, referencePath] = process.argv.slice(2);
const { default: createHdrgModule } = await import(pathToFileURL(modulePath).href);
const m = await createHdrgModule();
let failures = 0;

function check(condition, message) {
    if (!condition) {
        console.error(`FAIL: ${message}`);
        failures++;
    }
}

function setParam(params, name, value) {
    const key = m.stringToNewUTF8(name);
    const status = m._hdrg_wasm_params_set(params, key, value);

    m._free(key);

    return status;
}

function setFormat(params, id, options) {
    const name = m.stringToNewUTF8(id);
    let status = m._hdrg_wasm_params_format(params, name);

    m._free(name);

    for (const [key, value] of Object.entries(options)) {
        const k = m.stringToNewUTF8(key), v = m.stringToNewUTF8(String(value));

        status = (status === HDRG_OK) ? m._hdrg_wasm_params_format_option(params, k, v) : status;
        m._free(k);
        m._free(v);
    }

    return status;
}

async function convert(ctx, input, settings, format = null) {
    const params = m._hdrg_wasm_params_create(0);
    const ptr = m._malloc(input.length);
    let status, bytes = null;

    for (const [name, value] of Object.entries(settings)) {
        status = setParam(params, name, value);
        check(status === HDRG_OK, `param ${name} = ${value}: status ${status}`);
    }

    if (format !== null) {
        status = setFormat(params, format.id, format.options);
        check(status === HDRG_OK, `format ${format.id}: status ${status}`);
    }

    m.HEAPU8.set(input, ptr);

    const output = await m._hdrg_wasm_convert(ctx, ptr, input.length, params);

    status = m._hdrg_wasm_output_status(output);

    if (status === HDRG_OK) {
        const data = m._hdrg_wasm_output_data(output);

        bytes = m.HEAPU8.slice(data, data + m._hdrg_wasm_output_size(output));
    }

    m._hdrg_wasm_output_free(output);
    m._free(ptr);
    m._hdrg_wasm_params_destroy(params);

    return { status, bytes };
}

function contains(bytes, text) {
    return Buffer.from(bytes).includes(Buffer.from(text, 'latin1'));
}

const input = new Uint8Array(readFileSync(inputPath));
const reference = new Uint8Array(readFileSync(referencePath));
const ctx = m._hdrg_wasm_context_create(0, 0);

check(ctx !== 0, 'context');

const first = await convert(ctx, input, {});

check(first.status === HDRG_OK, `default conversion: status ${first.status}`);
check(first.bytes !== null && Buffer.compare(Buffer.from(first.bytes), Buffer.from(reference)) === 0,
      'the default conversion equals hdrg-cli of the same build');
check(m._hdrg_wasm_last_tier(ctx) !== 0, 'a SIMD tier is reported');
console.log(`hdrgrading ${m.UTF8ToString(m._hdrg_version())}, tier ` +
            m.UTF8ToString(m._hdrg_simd_name(m._hdrg_wasm_last_tier(ctx))));

const again = await convert(ctx, input, {});
const one = m._hdrg_wasm_context_create(1, 0);
const single = await convert(one, input, {});
const gpu = m._hdrg_wasm_context_create(0, 1);
const auto = await convert(gpu, input, {});

m._hdrg_context_destroy(one);
m._hdrg_context_destroy(gpu);

check(again.bytes !== null && Buffer.compare(Buffer.from(again.bytes), Buffer.from(first.bytes)) === 0,
      'a rerun gives the same bytes');
check(single.bytes !== null && Buffer.compare(Buffer.from(single.bytes), Buffer.from(first.bytes)) === 0,
      'one thread gives the same bytes');
check(auto.bytes !== null && Buffer.compare(Buffer.from(auto.bytes), Buffer.from(first.bytes)) === 0,
      'HDRG_GPU_AUTO without a GPU (node) gives the same bytes');

for (const [mode, marker] of [[0, 'urn:iso:std:iso:ts:21496:-1'], [1, 'urn:iso:std:iso:ts:21496:-1'], [2, 'ICC_PROFILE']]) {
    const out = await convert(ctx, input, { mode, 'diffuse-max': 1.5, 'output-width': 300 });

    check(out.status === HDRG_OK, `mode ${mode}: status ${out.status}`);
    check(out.bytes !== null && out.bytes[0] === 0xff && out.bytes[1] === 0xd8, `mode ${mode}: a JPEG`);
    check(out.bytes !== null && contains(out.bytes, marker), `mode ${mode}: has ${marker}`);
}

// JPEG XL, the built-in writer by its id: HYBRID by default (its jhgm gain map), its options
// checked, an unknown writer refused
const jxl = await convert(ctx, input, { 'output-width': 300 },
                          { id: 'hdrg.jxl', options: { quality: 80, effort: 3 } });
const signature = Buffer.from([0, 0, 0, 12, 0x4a, 0x58, 0x4c, 0x20, 0x0d, 0x0a, 0x87, 0x0a]);

check(jxl.status === HDRG_OK, `JPEG XL: status ${jxl.status}`);
check(jxl.bytes !== null && Buffer.from(jxl.bytes.slice(0, 12)).equals(signature), 'JPEG XL: its signature');
check(jxl.bytes !== null && contains(jxl.bytes, 'jhgm'), 'JPEG XL: the gain map box');
check((await convert(ctx, input, {}, { id: 'hdrg.jxl', options: { quality: 300 } })).status ===
      HDRG_ERR_INVALID_ARG, 'JPEG XL: a quality past its range refused');
check((await convert(ctx, input, {}, { id: 'org.hdrgrading.nonesuch', options: {} })).status ===
      HDRG_ERR_NOT_FOUND, 'an unknown writer refused');

const params = m._hdrg_wasm_params_create(0);

check(setParam(params, 'no-such-option', 1) === HDRG_ERR_NOT_FOUND, 'unknown name refused');
check(setParam(params, 'scale-shift', 300) === HDRG_ERR_INVALID_ARG, 'out-of-type value refused');
check(setParam(params, 'scale-shift', 2.5) === HDRG_ERR_INVALID_ARG, 'fraction for an integer refused');
check(setParam(params, 'dither', 2) === HDRG_ERR_INVALID_ARG, 'switch other than 0 / 1 refused');
check(setParam(params, 'diffuse-max', NaN) === HDRG_ERR_INVALID_ARG, 'NaN refused');
check(setFormat(params, '', { 'quality=1': 2 }) === HDRG_ERR_INVALID_ARG, 'a writer option key with = refused');
m._hdrg_wasm_params_destroy(params);

// the default writer's options (no id): a lower quality makes a smaller JPEG
const low = await convert(ctx, input, {}, { id: '', options: { quality: 30 } });

check(low.status === HDRG_OK && low.bytes !== null && low.bytes.length < first.bytes.length,
      'the default writer takes its options without an id');

const bad = await convert(ctx, input, { strength: -1 });

check(bad.status === HDRG_ERR_INVALID_ARG, `an invalid parameter fails the conversion: status ${bad.status}`);

// the interactive preview's state (hdrg_wasm.h HDRG_WASM_PREVIEW_*) of a gray ramp: its words, the
// same bytes twice, the apply WGSL, and a still without pixels refused
const PW = 64;
const PH = 32;
const ramp = new Uint8Array(PW * PH * 4);

for (let i = 0; i < PW * PH; i++) {
    const v = (i % PW) * 4;

    ramp.set([v, v, v, 255], i * 4);
}

const rampPtr = m._malloc(ramp.length);
const previewParams = m._hdrg_wasm_params_create(0);

m.HEAPU8.set(ramp, rampPtr);
check(setParam(previewParams, 'sdr-white', 250) === HDRG_OK, 'preview: sdr-white');

const states = [];

for (let k = 0; k < 2; k++) {
    const handle = m._hdrg_wasm_preview_create(ctx, rampPtr, PW, PH, previewParams);
    const status = handle === 0 ? -1 : m._hdrg_wasm_preview_status(handle);

    check(status === HDRG_OK, `preview: status ${status}`);

    if (status === HDRG_OK) {
        const w = new Uint32Array(m.HEAPU8.buffer, m._hdrg_wasm_preview_state(handle), 13).slice();

        check(w[1] > 0 && w[3] > 0 && w[5] > 0, 'preview: the uniform, LUTs and frame have bytes');
        check(w[6] >= PW * 4, `preview: the source pitch ${w[6]}`);
        check(w[8] >= PW * 6 && w[7] >= w[8] * (PH - 1) + PW * 6, `preview: the output ${w[7]} / ${w[8]}`);
        check(w[9] > 0 && w[10] > 0, 'preview: workgroups');
        check(w[11] === 1, `preview: Display P3 output (${w[11]})`);
        check(w[12] === 250000, `preview: the SDR white ${w[12]}`);
        states.push(Buffer.concat([0, 2, 4].map((i) => Buffer.from(m.HEAPU8.slice(w[i], w[i] + w[i + 1])))));
    }

    m._hdrg_wasm_preview_destroy(handle);
}

check(states.length === 2 && Buffer.compare(states[0], states[1]) === 0, 'preview: the same state twice');
check(m.UTF8ToString(m._hdrg_wasm_preview_shader()).includes('fn apply('), 'preview: the apply WGSL');

const empty = m._hdrg_wasm_preview_create(ctx, rampPtr, 0, PH, previewParams);

check(empty !== 0 && m._hdrg_wasm_preview_status(empty) === HDRG_ERR_INVALID_ARG,
      'preview: a still of width 0 refused');
m._hdrg_wasm_preview_destroy(empty);
m._hdrg_wasm_params_destroy(previewParams);
m._free(rampPtr);
m._hdrg_context_destroy(ctx);

if (failures > 0) {
    console.error(`${failures} failure(s)`);
    process.exit(1);
}

console.log('OK');
