// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
//
// The WASM demo: picks one of the builds ({SIMD128, scalar} x {pthreads,
// single thread}, or SIMD128 with WebGPU where the browser has it and JSPI), converts in a worker
// and shows the result as <img> twice, once limited to the standard dynamic range and once not.
// Where the browser has WebGPU, the sliders also drive an interactive preview (preview.js).

import { Preview } from './preview.js';

const MAX_PIXELS = 50e6; // wasm32 has 4 GB; the conversion holds a few hundred bytes per pixel at most
const PREVIEW_SIDE = 2048; // the preview's still is scaled down to this longest side
const MODES = { gainmap: 0, hybrid: 1, pq: 2 };
const PRESETS = { photo: 0, anime: 1, game: 2 };
// i32.const 0; i8x16.splat; i8x16.popcnt: validates only where the engine has SIMD128
const SIMD_PROBE = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0,
                                   65, 0, 253, 15, 253, 98, 11]);

const $ = (id) => document.getElementById(id);
const sliders = ['diffuse-max', 'strength', 'saturation', 'expand', 'peak-nits', 'sdr-white'];
const overrides = new Map();
let worker = null;
let input = null;
let inputName = 'image';
let outputUrl = null;
let busy = false;
let preview = null;
let previewImage = null;
let previewId = 0;
let previewWaiting = false;
let previewAgain = false;

function pickBuild() {
    const simd = WebAssembly.validate(SIMD_PROBE);
    const threads = self.crossOriginIsolated === true && typeof SharedArrayBuffer !== 'undefined';
    const webgpu = 'gpu' in navigator && typeof WebAssembly.Suspending === 'function';
    const asked = new URLSearchParams(location.search).get('build');

    if (asked !== null && /^(simd128|scalar)-(st|mt)$|^simd128-webgpu$/.test(asked)) {
        return asked;
    }

    if (simd && webgpu) {
        return 'simd128-webgpu';
    }

    return `${simd ? 'simd128' : 'scalar'}-${threads ? 'mt' : 'st'}`;
}

// a reload under the service worker that adds COOP / COEP (once per session)
async function isolate() {
    if (self.crossOriginIsolated || !('serviceWorker' in navigator)) {
        return false;
    }

    try {
        await navigator.serviceWorker.register('coi-sw.js');

        if (navigator.serviceWorker.controller === null && sessionStorage.getItem('coi-reloaded') === null) {
            sessionStorage.setItem('coi-reloaded', '1');
            location.reload();

            return true;
        }
    } catch {
        // the single-thread build runs
    }

    return false;
}

function status(text, error = false) {
    $('status').textContent = text;
    $('status').classList.toggle('error', error);
}

function settings() {
    const out = { mode: MODES[$('mode').value] };

    for (const [name, value] of overrides) {
        out[name] = value;
    }

    return out;
}

function showSlider(name) {
    $(`${name}-value`).textContent = overrides.has(name) ? Number($(name).value).toFixed(2) : '既定';
}

// the still for the preview: 8-bit sRGB RGBA, at most PREVIEW_SIDE on its longest side
async function decodePreview(blob) {
    const bitmap = await createImageBitmap(blob);
    const scale = Math.min(1, PREVIEW_SIDE / Math.max(bitmap.width, bitmap.height));
    const width = Math.max(1, Math.round(bitmap.width * scale));
    const height = Math.max(1, Math.round(bitmap.height * scale));
    const canvas = new OffscreenCanvas(width, height);
    const g = canvas.getContext('2d', { colorSpace: 'srgb' });

    g.drawImage(bitmap, 0, 0, width, height);
    bitmap.close();

    return { rgba: new Uint8Array(g.getImageData(0, 0, width, height).data.buffer), width, height };
}

// a preview of the current settings: one request in flight, the latest settings sent next
function requestPreview() {
    if (preview === null || previewImage === null) {
        return;
    }

    if (previewWaiting) {
        previewAgain = true;

        return;
    }

    previewWaiting = true;
    previewAgain = false;
    worker.postMessage({ type: 'preview', id: ++previewId, preset: PRESETS[$('preset').value],
                         settings: settings() });
}

async function setPreviewImage(blob) {
    if (preview === null) {
        return;
    }

    try {
        const still = await decodePreview(blob);

        previewImage = still;
        preview.setImage(still.rgba, still.width, still.height);
        worker.postMessage({ type: 'preview-image', rgba: still.rgba.slice().buffer, width: still.width,
                             height: still.height });
        requestPreview();
    } catch (error) {
        $('preview-status').textContent = `プレビューの画像を読めません: ${error.message || error}`;
    }
}

function previewDone(message) {
    previewWaiting = false;

    if (message.type === 'preview') {
        preview.run(message.state);
        $('preview-status').textContent = `解析とプラン ${message.ms.toFixed(0)} ms (WASM)、適用は GPU ` +
            `(${message.state.width} × ${message.state.height})`;
    } else {
        $('preview-status').textContent = `エラー: ${message.message}`;
    }

    if (previewAgain) {
        requestPreview();
    }
}

async function startPreview(shader) {
    if (!Preview.supported()) {
        return;
    }

    try {
        preview = new Preview($('preview'), shader);
        await preview.init();
        $('preview-card').hidden = false;
        $('split').addEventListener('input', () => preview.setSplit(Number($('split').value)));
    } catch (error) {
        preview = null;
        $('preview-card').hidden = false;
        $('preview-status').textContent = `WebGPU のプレビューを始められません: ${error.message || error}`;
    }
}

function setInput(buffer, name, url) {
    const probe = new Image();

    probe.onload = () => {
        if (probe.naturalWidth * probe.naturalHeight > MAX_PIXELS) {
            status(`画素数が上限 (${MAX_PIXELS / 1e6} MP) を超えています`, true);

            return;
        }

        input = buffer;
        inputName = name.replace(/\.[^.]+$/, '');
        $('original').src = url;
        $('size').textContent = `${probe.naturalWidth} × ${probe.naturalHeight}`;
        $('convert').disabled = worker === null;
        status('「変換」を押してください');
        setPreviewImage(new Blob([buffer]));
    };
    probe.onerror = () => status('画像を読み込めませんでした', true);
    probe.src = url;
}

async function loadFile(file) {
    setInput(await file.arrayBuffer(), file.name, URL.createObjectURL(file));
}

async function loadSample() {
    const response = await fetch('sample.png');

    setInput(await response.arrayBuffer(), 'kodim23.png', 'sample.png');
}

function convert() {
    if (input === null || busy) {
        return;
    }

    busy = true;
    $('convert').disabled = true;
    status('変換中…');
    worker.postMessage({ type: 'convert', input: input.slice(0), preset: PRESETS[$('preset').value],
                         settings: settings() });
}

function done(message) {
    const blob = new Blob([message.output], { type: 'image/jpeg' });

    if (outputUrl !== null) {
        URL.revokeObjectURL(outputUrl);
    }

    outputUrl = URL.createObjectURL(blob);
    $('limited').src = outputUrl;
    $('unlimited').src = outputUrl;
    $('download').href = outputUrl;
    $('download').download = `${inputName}-${$('mode').value}.jpg`;
    $('download').hidden = false;
    status(`${message.ms.toFixed(0)} ms、${(blob.size / 1024).toFixed(0)} KB (カーネル: ${message.tier})`);
}

async function main() {
    if (await isolate()) {
        return;
    }

    const build = pickBuild();

    $('build').textContent = build;
    $('display').textContent = matchMedia('(dynamic-range: high)').matches ? 'HDR 対応' : 'SDR (HDR の表示には対応ディスプレイが必要)';
    $('css').hidden = CSS.supports('dynamic-range-limit', 'no-limit');

    for (const name of sliders) {
        $(name).addEventListener('input', () => {
            overrides.set(name, Number($(name).value));
            showSlider(name);
            requestPreview();
        });
        showSlider(name);
    }

    $('preset').addEventListener('change', requestPreview);
    $('reset').addEventListener('click', () => {
        overrides.clear();
        sliders.forEach(showSlider);
        requestPreview();
    });
    $('file').addEventListener('change', (event) => {
        if (event.target.files.length > 0) {
            loadFile(event.target.files[0]);
        }
    });
    $('sample').addEventListener('click', loadSample);
    $('convert').addEventListener('click', convert);

    worker = new Worker('worker.js', { type: 'module' });
    worker.onmessage = (event) => {
        const message = event.data;

        if (message.type === 'ready') {
            $('version').textContent = message.version;
            $('convert').disabled = input === null;
            status(input === null ? '画像を選んでください' : '「変換」を押してください');
            startPreview(message.shader);

            return;
        }

        if (message.type === 'preview' || (message.type === 'error' && message.request === 'preview')) {
            previewDone(message);

            return;
        }

        busy = false;
        $('convert').disabled = false;

        if (message.type === 'done') {
            done(message);
        } else {
            status(`エラー: ${message.message}`, true);
        }
    };
    worker.postMessage({ type: 'init', build });
    status('WASM を読み込み中…');
}

main();
