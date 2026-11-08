# SPDX-License-Identifier: Apache-2.0 OR MIT
# Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
"""The manual's diagrams as data: boxes at explicit coordinates and arrows.

Edit a diagram here and run build.py: it writes docs/<lang>/img/<name>.svg
for every language. Coordinates are SVG user units (1 = 1 CSS pixel at 100 %).
A label is a str (every language) or L(en, ja); "\\n" breaks a line.
"""

from __future__ import annotations

from svgdiagram import Diagram


def L(en: str, ja: str) -> dict:
    return {'en': en, 'ja': ja}


def row(x0: float, x1: float, count: int, gap: float = 14.0):
    """x and width of count boxes spread over [x0, x1]."""
    w = (x1 - x0 - gap * (count - 1)) / count

    return [(x0 + i * (w + gap), w) for i in range(count)]


def layers() -> Diagram:
    d = Diagram('layers', 1040, 800,
                L('libhdrgrading: the layers of the library',
                  'libhdrgrading のライブラリの層'),
                L('Callers use the public headers; the entry points run the pipeline, '
                  'which uses the core and the plugin host; a still is written through the container '
                  'and the I/O layer; everything rests on the common layer and the bundled dependencies.',
                  '呼び出す側は公開ヘッダを使い、入口が pipeline を動かし、pipeline がコアとプラグインの'
                  'ホストを使う。静止画は容器と I/O の層を通して書く。すべての下に共通の層と同梱の依存がある。'))
    bands = [
        ('b_callers', 44, L('Callers', '呼び出す側')),
        ('b_api', 132, L('Public API  (include/hdrgrading/)', '公開 API  (include/hdrgrading/)')),
        ('b_entry', 220, L('Entry points  (src/)', '入口  (src/)')),
        ('b_pipe', 308, L('Pipeline, containers, plugin host', 'パイプライン・容器・ホスト')),
        ('b_engine', 396, L('Engines', '処理の本体')),
        ('b_hw', 514, L('CPU and GPU layers', 'CPU と GPU の層')),
        ('b_common', 602, L('Common layer (not public)', '共通の層 (公開しない)')),
        ('b_ext', 690, L('Bundled and system dependencies  (third_party/, system)',
                         '同梱とシステムの依存  (third_party/・システム)')),
    ]
    for id, y, label in bands:
        d.group(id, 20, y, 1000, 104 if id == 'b_engine' else 74, label)

    xs = [(40, 170), (225, 210), (450, 290), (755, 245)]
    d.box('app', *xs[0][:1], 66, xs[0][1], 40, L('Applications', 'アプリ'), 'C API', kind='app')
    d.box('tools', xs[1][0], 66, xs[1][1], 40, L('Tools', 'ツール'), 'tools/*.c · tools/gui',
          kind='app')
    d.box('bindings', xs[2][0], 66, xs[2][1], 40, L('Bindings', 'バインディング'),
          'FFmpeg · GStreamer · VapourSynth', kind='app', mono_sub=False)
    d.box('wasm', xs[3][0], 66, xs[3][1], 40, L('Browser module', 'ブラウザのモジュール'),
          'src/wasm · src/wasm/demo', kind='app')

    apis = [
        ('h_main', 'hdrgrading.h', L('context · stills', 'コンテキスト')),
        ('h_video', 'video.h', L('sessions · 3D LUT', 'セッション')),
        ('h_gpu', 'gpu.h', L('the app\'s GPU', 'アプリの GPU')),
        ('h_meta', 'metadata.h', L('HDR10+ · T.35', 'HDR10+・T.35')),
        ('h_side', 'sidedata.h', L('side data', 'サイドデータ')),
        ('h_plugin', 'plugin.h', L('host API', 'ホスト API')),
        ('h_filter', 'filter.h', L('filter ABI v1', 'フィルタの ABI')),
        ('h_codec', 'codec.h', L('codec ABI', 'コーデックの ABI')),
    ]
    for (x, w), (id, label, sub) in zip(row(40, 1000, len(apis)), apis):
        d.box(id, x, 154, w, 40, label, sub, kind='api', mono_sub=False)

    d.box('context', 40, 242, 190, 40, L('Context', 'コンテキスト'), 'context.c · params.c',
          kind='entry')
    d.box('session', 245, 242, 170, 40, L('Video session', '動画のセッション'), 'session/',
          kind='entry')
    d.box('convert', 430, 242, 170, 40, L('Still conversion', '静止画の変換'), 'convert.c',
          kind='entry')
    d.box('meta', 615, 242, 200, 40, L('3D LUT · T.35 payloads', '3D LUT・T.35'),
          'lut3d.c · dynamic.c', kind='entry')
    d.box('public', 830, 242, 170, 40, L('Plugin host API', 'ホスト API'), 'host/public.c',
          kind='entry')

    d.box('pipeline', 40, 330, 375, 40, L('Pipeline (sequence + frame)', 'パイプライン (系列 + フレーム)'),
          'pipeline/ · pipeline.h', kind='entry')
    d.box('container', 430, 330, 170, 40, L('Still writer', '静止画の書き手'), 'container.c',
          kind='io')
    d.box('host', 615, 326, 385, 50, L('Plugin host and stage runner', 'プラグインのホストと段の実行器'),
          'host/host.c · stage.c · format.c · codec_host.c\nanalyzer.c (ANALYZE) · stats.c (STATS)',
          kind='plugin')

    d.box('core', 40, 418, 375, 66, L('Core: passes A · B · C', 'コア: 走査 A・B・C'),
          'core/analyze · noise · plan · render · apply\ncolor · yuv · grain · predict · motion',
          kind='core')
    d.box('io', 430, 418, 170, 66, 'I/O', 'io/icc (+ HAGC tag)\ngainmeta (ISO 21496-1,\nXMP)',
          kind='io')
    d.box('filters', 615, 418, 385, 66, L('Built-in plugins (projects, same ABI, excludable)',
                                          '内蔵プラグイン (プロジェクト、同じ ABI、外せる)'),
          'plugins/: polyphase · edge · deblock · mctf · interp …\n'
          'predict (ANALYZE) · stats (STATS)\n'
          'jpeg · png · gif · jxl (codecs) · webgpu_*.c · gk_*.c',
          kind='filter')

    d.box('kernels', 40, 536, 250, 40, L('SIMD kernels (kernels/)', 'SIMD カーネル (kernels/)'),
          'SSE4.1 · AVX2 · NEON · SIMD128', kind='core')
    d.box('coregpu', 305, 536, 295, 40, L('Core on the GPU', 'コアの GPU の層'),
          'core/gpu_core · convert · apply · analyze', kind='gpu')
    d.box('gpu', 615, 536, 385, 40, L('GPU layers (hdrgrading-gpu)', 'GPU の層 (hdrgrading-gpu)'),
          'gpu/webgpu.c · metal.m · vulkan.c · gk.h · filter_*.c', kind='gpu')

    d.box('common', 40, 624, 960, 40, L('Foundation (hdrgrading-foundation)',
                                        'foundation (hdrgrading-foundation)'),
          'foundation/: memory · math · fmath · hash · fpenv · thread · pool · clock · gpu_share',
          kind='common')

    ext = [('jpegli', 'jpegli · Highway'), ('png', 'libpng · zlib'), ('gif', 'giflib'),
           ('lcms2', 'lcms2'), ('jxl', 'libjxl · brotli'),
           ('mods', 'libavif · libheif'),
           ('dawn', 'Dawn (WebGPU)'), ('vk', 'Vulkan · Metal')]
    for (x, w), (id, label) in zip(row(40, 1000, len(ext), 6), ext):
        d.box('x_' + id, x, 712, w, 40, label, kind='ext')

    d.edge('session', 'pipeline')
    d.edge('convert', 'pipeline', via=[(450, 301), (395, 301)], sa=0.12, da=0.947)
    d.edge('convert', 'container')
    d.edge('public', 'host')
    d.edge('pipeline', 'host', via=[(405, 389), (640, 389)], sa=0.973, src_side='bottom',
           dst_side='bottom', da=0.065)
    d.edge('pipeline', 'core', sa=0.5, da=0.5)
    d.edge('container', 'io')
    d.edge('host', 'filters')
    d.edge('core', 'kernels', sa=0.333, da=0.5)
    d.edge('session', 'coregpu', src_side='right', dst_side='top', sa=0.5, da=0.397, dashed=True,
           via=[(422, 262), (422, 520)])
    d.edge('filters', 'gpu', dashed=True)
    d.edge('coregpu', 'gpu', dashed=True)
    d.text(1020, 790, L('Upper layers call the ones below; dashed: only with a GPU (HDRG_GPU_AUTO / '
                        'FILTERS)', '上の層が下の層を呼ぶ。破線は GPU を使うときだけ (HDRG_GPU_AUTO / FILTERS)'),
           anchor='end')

    return d


def pipeline() -> Diagram:
    d = Diagram('pipeline', 1040, 740,
                L('One frame through the pipeline: analysis, plan, render, apply',
                  '1 枚のフレームがパイプラインを通る流れ: 解析・プラン・描画・適用'),
                L('A frame is read three times from its code values: pass A analyzes it, pass B '
                  'plans the gain on cells, the render fits the plan to the display, pass C applies '
                  'it in bands.',
                  'フレームは符号値から 3 回読まれる。走査 A が解析し、走査 B がセルの上にゲインを計画し、'
                  '描画がプランを表示に合わせ、走査 C が帯ごとに適用する。'))
    x, w = 250, 250
    steps = [
        ('frame', L('Frame (code values)', 'フレーム (符号値)'),
         'core/image: EOTF LUT', 'data'),
        ('noise', L('Noise estimate', 'ノイズの推定'), 'core/noise', 'core'),
        ('prefilter', L('PREFILTER stage (optional)', 'PREFILTER 段 (任意)'),
         'host/stage · plugins/', 'filter'),
        ('passa', L('Pass A: analysis', '走査 A: 解析'), 'core/analyze · ANALYZE stage', 'core'),
        ('passb', L('Pass B: plan', '走査 B: プラン'), 'core/plan', 'core'),
        ('render', L('Render for the display', '表示への描画'), 'core/render', 'core'),
        ('scale', L('SCALE stage (optional)', 'SCALE 段 (任意)'), 'host/stage', 'filter'),
        ('passc', L('Pass C: apply (bands)', '走査 C: 適用 (帯ごと)'),
         'core/apply · kernels/', 'core'),
        ('rows', L('Output rows', '出力の行'), L('container or session', '容器かセッション'), 'io'),
    ]
    notes = [
        L('The caller\'s pixels stay as code values; each pass decodes them on the fly\n'
          'through per-channel EOTF tables (no full-resolution float planes).',
          '呼び出し元の画素は符号値のまま。走査のたびにチャネルごとの EOTF の表で\n'
          'その場で復号する (全解像度の float の面を持たない)。'),
        L('16 × 16 tiles (Immerkær): the Y\' noise by level, Cb / Cr, blockiness, ringing.\n'
          'Given to the filters as HDRG_SD_NOISE; measured again after the chain for pass C.',
          '16 × 16 のタイル (Immerkær 法): 水準ごとの Y\' のノイズ・Cb / Cr・ブロック性・\n'
          'リンギング。フィルタへ HDRG_SD_NOISE で渡す。連鎖の後で測り直して走査 C に使う。'),
        L('Built-in chain deblock → dering → defringe → deband → MCTF (sessions) → NR,\n'
          'or up to 6 filters added; skipped when every filter passes the frame through.',
          '内蔵の連鎖 deblock → dering → defringe → deband → MCTF (セッション) → NR、\n'
          'または追加した 6 つまでのフィルタ。どれも素通しなら段ごと飛ばす。'),
        L('La = max(Y, 0.7 · maxRGB); histogram → p99.5 and key; 4 × 4 cells (2 × 2 under\n'
          'QUALITY). Lights: bright components, rings, an even falloff → boost share;\n'
          'the ANALYZE stage (light_predictor > 0) may lower a doubtful light\'s boost.',
          'La = max(Y, 0.7 × 最大チャネル)。ヒストグラム → p99.5 と key。4 × 4 のセル\n'
          '(QUALITY は 2 × 2)。光: 明るい成分 → 輪 → どの向きにも同じ落ち方 → 上乗せの割合。\n'
          'ANALYZE の段 (light_predictor > 0) が疑わしい光の上乗せを下げることがある。'),
        L('Diffuse level from the key; the boost share on a bilateral grid (8 × 8 cells ×\n'
          '24 log2 La bins); clarity base; APL cell statistics. Display independent.',
          'key から拡散面の倍率。上乗せの割合を bilateral grid (8 × 8 のセル × log2 La の\n'
          '24 区分) に。clarity の基底、APL のセル統計。表示には依存しない。'),
        L('hdrg_display_caps_t → diffuse multiplier, boost span up to the peak, APL budget\n'
          'factor (binary search), shoulder peak, gamut, encoding. Re-rendered per display.',
          'hdrg_display_caps_t → 拡散面の倍率・ピークまでの上乗せの幅・APL の係数 (二分探索)・\n'
          '肩のピーク・色域・符号化。表示を変えるときは描画だけを作り直す。'),
        L('A different output size or a filter on the stage: the filter, else a built-in\n'
          'scaler (Lanczos; the upscaler when no side shrinks). Analysis and plan stay as input.',
          '出力の寸法が違うか段にフィルタがあるとき: そのフィルタ、なければ内蔵の scaler\n'
          '(Lanczos。どの辺も縮まなければ upscaler)。解析とプランは入力の寸法のまま。'),
        L('256-row bands in parallel, 16-pixel blocks: g = diffuse + h · span + contrast +\n'
          'clarity; color LUT (33³) × 2^g; shoulder; PQ, or SDR + gain-map sums; grain.',
          '256 行の帯を並列に、16 画素のブロックで: g = 拡散面 + h × 幅 + contrast + clarity。\n'
          '色の LUT (33³) × 2^g、肩、PQ か SDR + ゲインマップの積算、粒。'),
        L('8 / 16-bit rows to the writer (JPEG, JPEG XL, AVIF, HEIF), or the session\'s PQ\n'
          'R\'G\'B\' / 4:2:0; the light measured on the way gives MaxCLL / FALL, HDR10+ and\n'
          'ST 2094-50.',
          '8 / 16bit の行を書き手 (JPEG・JPEG XL・AVIF・HEIF) へ、またはセッションの PQ R\'G\'B\' /\n'
          '4:2:0 へ。途中で測った光から MaxCLL / FALL、HDR10+、ST 2094-50。'),
    ]
    ys = [40 + i * 76 for i in range(len(steps))]
    for (id, label, sub, kind), y in zip(steps, ys):
        d.box(id, x, y, w, 46, label, sub, kind=kind,
              mono_sub=not isinstance(sub, dict))
    for (a, b) in zip(steps, steps[1:]):
        d.edge(a[0], b[0])
    for note, y in zip(notes, ys):
        d.text(530, y + 19, note)

    d.box('params', 30, ys[3] + 38, 180, 46, 'hdrg_params_t',
          L('content intent', 'コンテンツの意図'), kind='api', mono_sub=False)
    d.box('caps', 30, ys[5], 180, 46, 'hdrg_display_caps_t',
          L('what the display shows', '表示の能力'), kind='api', mono_sub=False)
    d.box('lut', 30, ys[7], 180, 46, L('Color LUT', '色の LUT'),
          L('core/color: Oklab gamut map', 'core/color: Oklab の写像'), kind='core',
          mono_sub=False)
    d.edge('params', 'passa', src_side='right', dst_side='left', sa=0.25, da=0.5,
           via=[(230, ys[3] + 49.5), (230, ys[3] + 23)])
    d.edge('params', 'passb', src_side='right', dst_side='left', sa=0.75, da=0.5,
           via=[(230, ys[3] + 72.5), (230, ys[4] + 23)])
    d.edge('caps', 'render', src_side='right', dst_side='left')
    d.edge('lut', 'passc', src_side='right', dst_side='left')
    d.edge('noise', 'passc', src_side='left', dst_side='left', sa=0.5, da=0.25, dashed=True,
           via=[(236, ys[1] + 23), (236, ys[1] + 40), (222, ys[1] + 40), (222, ys[7] + 11.5)])
    d.text(30, ys[1] + 70, L('dashed: the estimate,\nagain after the chain,\ncores pass C',
                             '破線: 連鎖の後で測り\n直したノイズは走査 C\nのコアリングにも'))

    return d


def still() -> Diagram:
    d = Diagram('still', 1060, 540,
                L('Still conversion: hdrg_convert from bytes to an HDR file',
                  '静止画の変換: hdrg_convert がバイト列から HDR のファイルを作る'),
                L('Decode with the color resolved, normalize the orientation, run the pipeline, '
                  'write one of three forms with the context\'s writer (a codec plugin).',
                  '色を解決して復号し、向きを正して pipeline を通し、3 つの形のどれかをコンテキストの書き手 '
                  '(コーデックのプラグイン) で書く。'))
    y = 96
    d.box('in', 20, y, 104, 52, L('Input bytes', '入力のバイト列'), kind='data')
    d.box('detect', 140, y, 120, 52, L('Format', '形式の判別'), L('readers probe', '読み手が試す'),
          kind='io', mono_sub=False)
    d.box('decode', 280, y, 150, 52, L('Reader', '読み手'), 'plugins/<codec>', kind='io')
    d.box('orient', 450, y, 130, 52, L('Orientation', '向き'), 'EXIF 2..8 → 1', kind='entry',
          mono_sub=False)
    d.box('pipe', 600, y, 150, 52, L('Pipeline', 'パイプライン'), 'pipeline.c', kind='entry')
    d.box('cont', 770, y, 140, 52, L('Writer', '書き手'), 'container.c', kind='io')
    d.box('out', 930, y, 122, 52, L('HDR file', 'HDR のファイル'), L('the writer\'s', '書き手の形式'),
          kind='data', mono_sub=False)
    d.box('raw', 20, 16, 250, 44, 'hdrg_convert_raw',
          L('pixels + H.273 primaries / transfer', '画素 + H.273 の原色・伝達特性'), kind='api',
          mono_sub=False)
    for a, b in [('in', 'detect'), ('detect', 'decode'), ('decode', 'orient'), ('orient', 'pipe'),
                 ('pipe', 'cont'), ('cont', 'out')]:
        d.edge(a, b)
    d.edge('raw', 'pipe', via=[(675, 38)], src_side='right', dst_side='top')

    d.group('g_dec', 140, 176, 440, 206, L('Readers and the input color', '読み手と入力の色'))
    decs = [
        ('JPEG', 'jpegli', L('ICC > EXIF R03 > sRGB;\n4:2:0 / 4:2:2: chroma from luma',
                             'ICC > EXIF R03 > sRGB。\n4:2:0 / 4:2:2 は chroma from luma')),
        ('PNG', 'libpng', L('cICP > iCCP > sRGB > cHRM/gAMA', 'cICP > iCCP > sRGB > cHRM/gAMA')),
        ('GIF', 'giflib', L('the first image', '最初の画像だけ')),
        ('AVIF', L('module', 'モジュール'), L('CICP / ICC; PQ, HLG rejected',
                                                    'CICP / ICC。PQ・HLG は拒否')),
    ]
    for i, (name, lib, note) in enumerate(decs):
        bx = 156 + (i % 2) * 212
        by = 202 + (i // 2) * 58
        d.box('d_' + name, bx, by, 200, 50, {lang: f'{name} · {lib[lang] if isinstance(lib, dict) else lib}'
                                             for lang in ('en', 'ja')}, note, kind='io',
              mono_sub=False)
    d.text(368 + 0, 336, L('ICC: matrix + TRC read directly,\nLUT profiles through lcms2',
                           'ICC: 行列 + TRC は直接読み、\nLUT 型は lcms2 で'))
    d.edge('decode', 'g_dec', dst_side='top', da=0.489)

    d.group('g_form', 600, 176, 440, 118, L('Forms (encode.output_mode)', '形 (encode.output_mode)'))
    forms = [('GAINMAP', L('SDR base + gain\nmap', 'SDR の基本 +\nゲインマップ')),
             ('HYBRID', L('PQ base + SDR-\nrecovering map', 'PQ の基本 + SDR を\n戻すゲインマップ')),
             ('PQ', L('single-layer PQ', '単層 PQ'))]
    for (bx, bw), (name, note) in zip(row(614, 1026, 3, 10), forms):
        d.box('f_' + name, bx, 202, bw, 78, name, note, kind='core', mono_sub=False)
    d.edge('cont', 'g_form', dst_side='top', da=0.545)

    d.group('g_cont', 600, 306, 440, 146, L('Writers (hdrg_context_set_writer)',
                                            '書き手 (hdrg_context_set_writer)'))
    conts = [('JPEG', L('jpegli + MPF,\nISO 21496-1,\nXMP, ICC', 'jpegli + MPF・\nISO 21496-1・\nXMP・ICC')),
             ('JXL', L('built in:\nlibjxl, jhgm\ngain map', '内蔵:\nlibjxl・jhgm\nゲインマップ')),
             ('AVIF', L('module: AV1,\nCICP, clli, ISO\ngain map', 'モジュール:\nAV1・CICP・\nclli・ISO')),
             ('HEIF', L('module: HEVC,\ntmap + Apple\n(opt-in)', 'モジュール:\nHEVC・tmap +\nApple (任意)'))]
    for (bx, bw), (name, note) in zip(row(614, 1026, 4, 8), conts):
        d.box('c_' + name, bx, 332, bw, 106, name, note, kind='io', mono_sub=False)

    d.group('g_set', 20, 176, 100, 206, L('Settings', '設定'))
    d.box('s_params', 28, 202, 84, 50, 'params', L('intent', '意図'), kind='api', mono_sub=False)
    d.box('s_caps', 28, 262, 84, 50, 'display', L('caps', '表示'), kind='api', mono_sub=False)
    d.box('s_enc', 28, 322, 84, 50, 'encode', L('form', '形'), kind='api', mono_sub=False)
    d.text(20, 482, L('A still file is graded for the assumed viewing (hdrg_container_caps): 203-nit '
                      'white, the gain map\'s 3-stop cap, Display P3 (JPEG: with lcms2); one call, '
                      'effort QUALITY, grain PRESERVE.',
                      '静止画のファイルは想定の視聴 (hdrg_container_caps: 白 203 nits、ゲインマップは 3 段の上限、'
                      'Display P3 (JPEG は lcms2 があるとき)) へ描く。effort は QUALITY、粒は PRESERVE。'))
    d.text(20, 500, L('output_mode AUTO is the writer\'s default form: GAINMAP for JPEG, AVIF and HEIF, '
                      'HYBRID for JPEG XL; the default writer is hdrg.jpeg.',
                      'output_mode の AUTO は書き手の既定の形 (JPEG・AVIF・HEIF は GAINMAP、JPEG XL は HYBRID)。'
                      '既定の書き手は hdrg.jpeg。'))
    d.text(20, 518, L('Output buffers carry their allocator: hdrg_free works after the context is gone.',
                      '出力のバッファはアロケータを持つので、コンテキストを壊した後も hdrg_free で解放できる。'))

    return d


def session() -> Diagram:
    d = Diagram('session', 1060, 530,
                L('A video session: send_frame and receive_frame',
                  '動画のセッション: send_frame と receive_frame'),
                L('Sent frames are converted, prefiltered, analyzed and queued; received frames take '
                  'their scene\'s statistics, are planned, limited, scaled and applied.',
                  '送ったフレームは変換・前処理・解析されて待ち行列に入り、受け取るフレームはシーンの統計で'
                  'プランされ、制限・拡大・適用される。'))
    d.group('g_send', 20, 30, 1020, 150, 'hdrg_session_send_frame')
    d.group('g_recv', 20, 220, 1020, 170, 'hdrg_session_receive_frame')
    top = [
        ('frame', L('Frame', 'フレーム'), L('host planes or\na GPU frame', 'host の面か\nGPU のフレーム'),
         'data'),
        ('ingest', L('Ingest', '取り込み'), L('Y\'CbCr → RGB16\n(CPU or GPU)', 'Y\'CbCr → RGB16\n(CPU か GPU)'),
         'entry'),
        ('pre', L('PREFILTER chain', 'PREFILTER の連鎖'),
         L('time windows,\nASYNC in flight', '時間のウィンドウ・\n飛行中の ASYNC'), 'filter'),
        ('ana', L('Analysis', '解析'), L('proxy ≤ 960 px\n(REALTIME), noise,\nlights', '縮小画 (REALTIME\nは 960 まで)・\nノイズ・光'),
         'core'),
        ('cut', L('Scene cut test', 'シーンの切れ目'), L('histogram + log\naverage, fades', 'ヒストグラムと\n対数平均・フェード'),
         'core'),
        ('queue', L('Queue', '待ち行列'), L('the queue\'s depth\n+ 1 frames', '待ち行列の\n深さ + 1 枚'), 'data'),
    ]
    for (bx, bw), (id, label, sub, kind) in zip(row(40, 1020, len(top), 22), top):
        d.box(id, bx, 70, bw, 86, label, sub, kind=kind, mono_sub=False)
    for a, b in zip(top, top[1:]):
        d.edge(a[0], b[0])

    bottom = [
        ('out', L('Output', '出力'), L('PQ R\'G\'B\' or 4:2:0,\nHDR10+, ST 2094-50', 'PQ R\'G\'B\' か 4:2:0、\nHDR10+・ST 2094-50'),
         'data'),
        ('apply', L('Apply (pass C)', '適用 (走査 C)'), L('CPU kernels\nor the GPU', 'CPU のカーネル\nか GPU'), 'core'),
        ('scale', L('SCALE / INTERP', 'SCALE / INTERP'), L('output size,\nout_rate', '出力の寸法・\n出力のレート'),
         'filter'),
        ('flash', L('Flash limiter', 'フラッシュリミッタ'), 'ITU-R BT.1702-3', 'core'),
        ('plan', L('Plan + render', 'プラン + 描画'), L('pass B, display\ncaps', '走査 B・\n表示能力'), 'core'),
        ('stats', L('STATS stage', 'STATS の段'),
         L('statistics across\nframes, fades,\nsteady lights', 'フレームをまたぐ\n統計・フェード・\n光の安定化'),
         'filter'),
    ]
    for (bx, bw), (id, label, sub, kind) in zip(row(40, 1020, len(bottom), 22), bottom):
        d.box(id, bx, 262, bw, 100, label, sub, kind=kind, mono_sub=False)
    for a, b in zip(bottom[1:], bottom):
        d.edge(a[0], b[0])
    d.edge('queue', 'stats', src_side='bottom', dst_side='top')

    d.text(40, 420, L('Latency (hdrg_session_latency): the statistics window\'s future, the filters\' '
                      'time windows\' future and their ASYNC frames in flight; LOW waits for each job.',
                      '遅延 (hdrg_session_latency) = 統計のウィンドウの未来 + フィルタの時間のウィンドウの未来 + 飛行中の ASYNC。'
                      'LOW はジョブを 1 つずつ待つ。'))
    d.text(40, 440, L('Seeking: start = n − p, preroll = p (hdrg_session_preroll) renders frame n '
                      'byte for byte as a session from the first frame (WINDOWED, TWO_PASS, STATIC).',
                      'シーク: start = n − p、preroll = p (hdrg_session_preroll) で、フレーム n からが頭から'
                      '通したセッションとバイト一致 (WINDOWED・TWO_PASS・STATIC)。'))
    d.text(40, 460, L('Frames on the GPU (HDRG_GPU_AUTO, Y\'CbCr input, the library\'s GPU filters alone, no SCALE): '
                      'ingest to apply stay on the device; the analysis reads back a proxy and noise windows.',
                      'GPU に置く経路 (HDRG_GPU_AUTO・Y\'CbCr の入力・ライブラリの GPU のフィルタだけ・SCALE なし): '
                      '取り込みから適用まで device に置き、解析は縮小画とノイズのウィンドウだけを読み戻す。'))
    d.text(40, 480, L('Each receive also reports late (the rate is not kept), shed, synthetic / phase '
                      '(out_rate) and the motion fields; hdrg_session_latency_report says what to change.',
                      '受け取りごとに late (レートに遅れた)・shed・synthetic / phase (出力のレート)・動きの場も返す。'
                      'hdrg_session_latency_report が直すところを勧める。'))
    d.text(40, 510, L('A session is used by one thread at a time: send until HDRG_ERR_AGAIN, receive '
                      'until HDRG_ERR_AGAIN, …; send NULL at the end, then receive until HDRG_EOF.',
                      'セッションは 1 度に 1 つのスレッドから: HDRG_ERR_AGAIN まで送り、HDRG_ERR_AGAIN まで受け取り、…。'
                      '終わりに NULL を送り、HDRG_EOF まで受け取る。'))

    return d


def window() -> Diagram:
    d = Diagram('window', 1040, 250,
                L('A session\'s windows: past, future, latency and preroll',
                  'セッションのウィンドウ: 過去・未来・遅延・preroll'),
                L('Frame t is rendered from the frames of its scene in [t − past, t + future]; it '
                  'comes out once frame t + future was sent.',
                  'フレーム t はシーンの [t − past, t + future] のフレームから描かれ、t + future を送った後に出てくる。'))
    n, x0, w, gap = 21, 40, 40, 6
    t = 10
    for i in range(n):
        k = i - t
        if k == 0:
            kind, label = 'entry', 't'
        else:
            kind = 'core' if -8 <= k <= 4 else 'data'
            label = f'{k:+d}'
        d.box(f'f{i}', x0 + i * (w + gap), 90, w, 40, label, kind=kind)
    xl = x0 + (t - 8) * (w + gap)
    xt = x0 + t * (w + gap)
    xr = x0 + (t + 4) * (w + gap) + w
    d.line([(xl, 80), (xl, 72), (xt - 3, 72), (xt - 3, 80)])
    d.line([(xt + w + 3, 80), (xt + w + 3, 72), (xr, 72), (xr, 80)])
    d.text((xl + xt) / 2, 64, L('past (8 by default with the STATS filter): averaged',
                                'past (STATS のフィルタがあれば既定 8): 平均する'),
           anchor='middle')
    d.text((xt + w + xr) / 2, 64, L('future: latency', 'future: 遅延'), anchor='middle')
    d.line([(xt + w / 2, 140), (xt + w / 2, 168), (xr - w / 2, 168), (xr - w / 2, 140)],
           arrow=False, dashed=True)
    d.text((xt + xr) / 2, 186, L('receive t after sending t + future', 't + future を送ってから t を受け取る'),
           anchor='middle')
    xp = x0
    d.line([(xp + w / 2, 140), (xp + w / 2, 214), (xt + w / 2, 214), (xt + w / 2, 142)], arrow=True)
    d.text(xp + 4, 234, L('a seek to t: start = t − preroll, the preroll frames build the state and are '
                          'not received (preroll ≥ hdrg_session_preroll)',
                          't へのシーク: start = t − preroll。preroll のフレームは状態を作るだけで受け取らない '
                          '(preroll ≥ hdrg_session_preroll)'))
    d.text(x0, 24, L('Statistics follow the frames of the scene only: a cut starts a new window.',
                     '統計は同じシーンのフレームだけを追う: 切れ目でウィンドウは新しく始まる。'))

    return d


def gpu() -> Diagram:
    d = Diagram('gpu', 1060, 640,
                L('The GPU layers: shaders, backends, filters and the core',
                  'GPU の層: シェーダ・下位層・フィルタ・コア'),
                L('WGSL is the source of every shader; Tint generates MSL and SPIR-V. Filters and the '
                  'core are written once over WebGPU or the common layer of Metal and Vulkan.',
                  'シェーダのリファレンスは WGSL で、Tint が MSL と SPIR-V を作る。フィルタとコアは WebGPU か、'
                  'Metal と Vulkan の共通の層の上で 1 回だけ書く。'))
    d.group('g_sh', 20, 20, 1020, 112, L('Shaders (build time)', 'シェーダ (ビルド時)'))
    d.box('wgsl', 40, 50, 250, 64, L('WGSL (source of truth)', 'WGSL (リファレンス)'),
          'plugins/*/wgsl · core/wgsl', kind='gpu')
    d.box('tint', 330, 50, 230, 64, L('scripts/gen_shaders.sh', 'scripts/gen_shaders.sh'),
          L('Tint CLI (build_dawn.sh --tint)', 'Tint の CLI (build_dawn.sh --tint)'),
          kind='common', mono_sub=False)
    d.box('gen', 600, 50, 420, 64, L('Generated, committed', '生成物 (コミットする)'),
          'msl/*.metal · spirv/*.spv · core/hlsl/*.hlsl\n(CMake embeds every one as arrays)',
          kind='data')
    d.edge('wgsl', 'tint')
    d.edge('tint', 'gen')

    d.group('g_use', 20, 150, 1020, 160, L('Users of the GPU', 'GPU を使うもの'))
    d.box('fw', 40, 182, 300, 110, L('WebGPU filters', 'WebGPU のフィルタ'),
          'plugins/*/webgpu_*.c\nhdrg.webgpu.deblock · dering ·\ndefringe · deband · denoise ·\nmctf · lanczos · edge · fsr1 ·\nnis · interp.mc',
          kind='filter')
    d.box('fg', 360, 182, 330, 110, L('Metal and Vulkan filters', 'Metal と Vulkan のフィルタ'),
          'plugins/*/gk_*.c (written once)\nhdrg.metal.* · hdrg.vulkan.*\n+ plugins/metalfx (MetalFX)',
          kind='filter')
    d.box('cg', 710, 182, 310, 110, L('Core on the GPU', 'コアの GPU の層'),
          'core/gpu_core.c: 40 buffer slots\ngpu_convert: Y\'CbCr → RGB16\ngpu_apply: pass C\ngpu_analyze · gpu_gather',
          kind='gpu')

    d.group('g_be', 20, 328, 1020, 150, L('Backends (src/gpu/, not public)', '下位層 (src/gpu/、公開しない)'))
    d.box('webgpu', 40, 360, 300, 100, 'gpu/webgpu.{h,c}',
          L('Dawn; one device thread runs\nthe work (browser: inline + JSPI)',
            'Dawn。device のスレッドが作業を\n順に実行 (ブラウザはその場 + JSPI)'),
          kind='gpu', mono_sub=False)
    d.box('gk', 360, 360, 330, 40, 'gpu/gk.h',
          L('buffers · kernels · passes · frames', 'バッファ・カーネル・パス・フレーム'),
          kind='gpu', mono_sub=False)
    d.box('metal', 360, 420, 160, 40, 'gpu/metal.m', 'gk_metal.c', kind='gpu')
    d.box('vulkan', 530, 420, 160, 40, 'gpu/vulkan.c', 'gk_vulkan.c', kind='gpu')
    d.box('apple', 710, 360, 310, 100, 'gpu/apple.c',
          L('CVPixelBuffer / IOSurface:\npixel formats, planes', 'CVPixelBuffer / IOSurface:\n画素の形式と面'),
          kind='gpu', mono_sub=False)
    d.edge('gk', 'metal', sa=0.24, da=0.5)
    d.edge('gk', 'vulkan', sa=0.76, da=0.5)
    d.edge('fw', 'webgpu')
    d.edge('fg', 'gk')
    d.edge('cg', 'gk', src_side='bottom', dst_side='right', sa=0.2, da=0.5,
           via=[(772, 380)])
    d.edge('cg', 'webgpu', src_side='bottom', dst_side='top', sa=0.06, da=0.85,
           via=[(729, 318), (295, 318)], dashed=True)

    d.group('g_dev', 20, 496, 1020, 124, L('Devices', 'device'))
    d.box('dev_app', 40, 528, 300, 76, L('The app\'s device', 'アプリの device'),
          'hdrg_context_set_device\nHDRG_DEVICE_METAL / VULKAN / WEBGPU', kind='api')
    d.box('dev_own', 360, 528, 330, 76, L('The library\'s own device', 'ライブラリ自身の device'),
          L('made on the first use; one layer per\nkind shared by the core and the plugins',
            '最初に使うときに作る。種類ごとに 1 つの\n層をコアとプラグインで共有'),
          kind='gpu', mono_sub=False)
    d.box('order', 710, 528, 310, 76, L('Selection order', '選ぶ順'),
          L('config.device\'s kind first, then\nMetal → Vulkan → WebGPU → CPU',
            'config.device の種類を先に、\n次に Metal → Vulkan → WebGPU → CPU'),
          kind='common', mono_sub=False)

    return d


def plugins() -> Diagram:
    d = Diagram('plugins', 1100, 620,
                L('Plugins: registration, stages, jobs and codecs',
                  'プラグイン: 登録・段・ジョブ・コーデック'),
                L('Plugins register through an API table, the host validates and freezes them, the '
                  'stage runner calls their filters per frame and the codec host their readers and '
                  'writers.',
                  'プラグインは API 表を通して登録し、ホストが検証してフリーズし、段の実行器がフレームごとに'
                  'フィルタを、コーデックのホストが読み手と書き手を呼ぶ。'))
    d.box('app', 20, 40, 230, 134, L('Application', 'アプリ'),
          'hdrg_context_register_plugin\nhdrg_context_load_plugin\nhdrg_context_set_plugin_policy\n'
          'hdrg_context_add_filter\nhdrg_context_set_writer\nhdrg_context_set_device',
          kind='app')
    d.box('module', 20, 210, 230, 104, L('Plugin module', 'プラグインのモジュール'),
          'HDRG_PLUGIN_DEFINE(init)\nhdrg_plugin_abi_version()\nhdrg_plugin_init(plugin, api)\n(.so / .dylib / .dll)',
          kind='plugin')
    d.box('builtin', 20, 364, 230, 104, L('Built-in plugins', '内蔵のプラグイン'),
          L('hdrg.deblock · hdrg.mctf ·\nhdrg.scale.* · hdrg.stats.* ·\nhdrg.jpeg · hdrg.png · hdrg.jxl\n… (at context creation)',
            'hdrg.deblock · hdrg.mctf ·\nhdrg.scale.* · hdrg.stats.* ·\nhdrg.jpeg · hdrg.png · hdrg.jxl\n… (コンテキストの作成時)'),
          kind='filter')

    d.group('g_host', 290, 30, 380, 450, L('Host (src/host/)', 'ホスト (src/host/)'))
    d.box('loader', 310, 60, 340, 46, 'loader.c', L('dlopen / LoadLibrary; ABI version check',
                                                    'dlopen / LoadLibrary、ABI の版の照合'),
          kind='plugin', mono_sub=False)
    d.box('reg', 310, 126, 340, 66, 'host.c',
          L('entry point → report (info, license,\ncomponents) → policy → freeze',
            '入口 → 報告 (情報・ライセンス・\n部品) → 方針 → フリーズ'),
          kind='plugin', mono_sub=False)
    d.box('validate', 310, 212, 165, 46, 'validate.c',
          L('UTF-8, ids, SemVer, SPDX', 'UTF-8・ID・SemVer・SPDX'), kind='plugin',
          mono_sub=False)
    d.box('codec', 485, 212, 165, 46, 'codec_host.c',
          L('readers in turn, the writer', '読み手を順に・書き手'), kind='plugin',
          mono_sub=False)
    d.box('stages', 310, 278, 340, 66, L('Stages (analyzer.c · stats.c)', '段 (analyzer.c・stats.c)'),
          L('PREFILTER ≤ 6 (a chain) · INTERP 1 · SCALE 1\nANALYZE 1 (lights) · STATS 1 (sessions)',
            'PREFILTER ≤ 6 (連鎖)・INTERP 1・SCALE 1\nANALYZE 1 (光)・STATS 1 (セッション)'),
          kind='plugin', mono_sub=False)
    d.box('stage', 310, 364, 340, 96, 'stage.c · format.c',
          L('bands or whole frames, RGB16 ↔ job formats,\ntime windows and clips, ASYNC in flight,\ndevice frames between neighbors',
            '帯か全画面、RGB16 ↔ ジョブの形式、\n時間のウィンドウとクリップ、飛行中の ASYNC、\n隣り合うフィルタの device のフレーム'),
          kind='plugin', mono_sub=False)
    d.edge('loader', 'reg')
    d.edge('reg', 'validate', both=False, sa=0.24)
    d.edge('reg', 'codec', sa=0.76)
    d.edge('validate', 'stages', da=0.243)
    d.edge('stages', 'stage')
    d.edge('app', 'loader', src_side='right', dst_side='left', sa=0.2)
    d.edge('app', 'stages', src_side='right', dst_side='left', sa=0.68, da=0.5,
           via=[(272, 131.1), (272, 311)])
    d.edge('module', 'loader', src_side='right', dst_side='left', sa=0.3, da=0.7,
           via=[(262, 241), (262, 92)], dashed=True)
    d.edge('builtin', 'reg', src_side='right', dst_side='left', sa=0.2, da=0.7,
           via=[(282, 385), (282, 172)])

    d.group('g_cb', 700, 30, 340, 450, L('A filter instance\'s life', 'フィルタのインスタンスの一生'))
    cbs = [
        ('cb_create', 'create', L('once per conversion / session:\nwindow, clip, max_inflight, formats',
                                  '変換・セッションで 1 回: ウィンドウ・クリップ・\nmax_inflight・形式を申告')),
        ('cb_conf', 'configure', L('before each frame: input, side data,\noutput size and format, passthrough',
                                   'フレームごと: 入力・サイドデータ・\n出力の寸法と形式・素通し')),
        ('cb_job', 'process / submit', L('one job per band or frame;\nASYNC ends with job_done',
                                         '帯かフレームごとのジョブ。\nASYNC は job_done で終わる')),
        ('cb_destroy', 'destroy', L('after cancel and the jobs in flight', 'cancel と飛行中のジョブの後')),
    ]
    for i, (id, label, sub) in enumerate(cbs):
        d.box(id, 720, 62 + i * 102, 300, 78, label, sub, kind='plugin', mono_sub=False)
    for a, b in zip(cbs, cbs[1:]):
        d.edge(a[0], b[0])
    d.edge('cb_job', 'cb_conf', src_side='right', dst_side='right', via=[(1050, 305), (1050, 203)],
           label=L('next\nframe', '次の\nフレーム'), label_at=1, label_dx=26)
    d.edge('stage', 'cb_conf', src_side='right', dst_side='left', sa=0.3, da=0.7, dashed=True)
    d.text(20, 510, L('Strings cross as (pointer, length) and are validated and copied; no enums cross '
                      'the boundary, the host\'s objects are opaque; callbacks receive the API table last.',
                      '文字列は (ポインタ, 長さ) で渡り、検証して複製する。境界を enum は渡らず、ホストのものは中の見えないハンドル。'
                      'コールバックは最後の引数に API 表を受け取る。'))
    d.text(20, 530, L('The ABI stays at version 1 until it is frozen: build plugins against the same '
                      'header.',
                      'ABI はフリーズまで版 1 のまま。プラグインは同じ版のヘッダで組み直す。'))
    d.text(20, 550, L('Side data: HDRG_SD_NOISE (measured), HDRG_SD_MOTION (app, MCTF, INTERP), '
                      'HDRG_SD_FILM_GRAIN (the codec\'s grain).',
                      'サイドデータ: HDRG_SD_NOISE (測った値)・HDRG_SD_MOTION (アプリ・MCTF・INTERP)・'
                      'HDRG_SD_FILM_GRAIN (コーデックの粒)。'))
    d.text(20, 570, L('Codecs (codec.h): a reader probes a file\'s head and decodes it into the picture the '
                      'host allocates; a writer takes the rows band by band, then the gain map, metadata and light.',
                      'コーデック (codec.h): 読み手はファイルの頭を試し、ホストが確保した絵へ復号する。書き手は行を'
                      '帯ごとに受け取り、終わりにゲインマップ・メタデータ・光を受け取る。'))

    return d


def bindings() -> Diagram:
    d = Diagram('bindings', 1100, 470,
                L('Bindings: media frameworks on top of the public API',
                  'バインディング: 公開 API の上のメディアの枠組み'),
                L('Each framework binding maps its frames, colors and metadata to a session; the browser '
                  'module converts stills.',
                  '枠組みのバインディングは、そのフレーム・色・メタデータをセッションへ対応づける。ブラウザの'
                  'モジュールは静止画を変換する。'))
    hosts = [
        ('ff', 'FFmpeg', L('libavfilter, hdrg-ffmpeg', 'libavfilter・hdrg-ffmpeg')),
        ('gst', 'GStreamer', L('1.20 or later', '1.20 以降')),
        ('vs', 'VapourSynth', 'R81 (VapourSynth4.h)'),
        ('web', L('Browser', 'ブラウザ'), L('Emscripten, WebGPU', 'Emscripten・WebGPU')),
    ]
    cols = row(30, 1070, 4, 20)
    for (x, w), (id, label, sub) in zip(cols, hosts):
        d.box('h_' + id, x, 40, w, 50, label, sub, kind='app', mono_sub=False)
    layers = [
        ('ff', 'src/bindings/ffmpeg', L('patches/: vf_libhdrgrading.c\n(tools/ffmpeg: AVFrame ↔ session,\nzero copy, film grain, VideoToolbox)',
                                    'patches/: vf_libhdrgrading.c\n(tools/ffmpeg: AVFrame ↔ セッション、\nゼロコピー・粒・VideoToolbox)')),
        ('gst', 'src/bindings/gstreamer', L('element hdrgrade: caps colorimetry,\nlatency = future window,\nHdrgDynamicHdrMeta',
                                        '要素 hdrgrade: caps の colorimetry、\n遅延 = 未来のウィンドウ、\nHdrgDynamicHdrMeta')),
        ('vs', 'src/bindings/vapoursynth', L('hdrg.Grade: any order, parallel;\nseeks with the preroll;\nframe props HDR10Plus',
                                         'hdrg.Grade: 任意の順・並列、\npreroll でシーク、\nフレームの属性 HDR10Plus')),
        ('web', 'src/wasm', L('hdrg_wasm_*: settings by name,\nconvert (JSPI Promise),\ninteractive preview state',
                              'hdrg_wasm_*: 名前で設定、\nconvert (JSPI の Promise)、\n対話プレビューの状態')),
    ]
    for (x, w), (id, label, sub) in zip(cols, layers):
        d.box('b_' + id, x, 130, w, 96, label, sub, kind='entry', mono_sub=False)
        d.edge('h_' + id, 'b_' + id)
    d.box('api', 30, 270, 1040, 64, L('Public API', '公開 API'),
          'video.h: hdrg_session_*    metadata.h: hdrg_hdr10plus_t35 · hdrg_st2094_50_t35    '
          'hdrgrading.h: hdrg_setting_* · hdrg_convert', kind='api')
    for (x, w), (id, _, _) in zip(cols, layers):
        d.edge('b_' + id, 'api', da=(x + w / 2 - 30) / 1040)
    d.text(30, 366, L('Settings take the hdrg-cli option names (hdrg_setting_at / set / get; VapourSynth: _ for -) '
                      'and numbers; the own preset / temporal / latency options take value names.',
                      '設定は hdrg-cli のオプション名 (hdrg_setting_at / set / get。VapourSynth は - を _ に) と数で渡す。'
                      '自分の preset・temporal・latency の選択肢は値の名前を受け取る。'))
    d.text(30, 388, L('The video bindings give single-layer PQ (BT.2020; VapourSynth\'s rgb48 Display P3 unless '
                      'bt2020=1)\nwith the mastering display (hdrg_session_peak) and per-frame HDR10+ and '
                      'ST 2094-50 as T.35 payloads.',
                      '動画のバインディングの出力は単層 PQ (BT.2020。VapourSynth の rgb48 は bt2020=1 でなければ '
                      'Display P3) で、\nマスタリングディスプレイ (hdrg_session_peak) と、フレームごとの HDR10+・'
                      'ST 2094-50 の T.35 のペイロードを付ける。'))
    d.text(30, 425, L('The framework bindings use the public headers alone and link libhdrgrading (the FFmpeg '
                      'filter the shared library\nthrough pkg-config); the browser module also reads '
                      'foundation\'s memory.h.',
                      '枠組みのバインディングは公開ヘッダだけを使い libhdrgrading をリンクする (FFmpeg のフィルタは '
                      'pkg-config で共有ライブラリを)。\nブラウザのモジュールは foundation の memory.h も読む。'))

    return d


def tools() -> Diagram:
    d = Diagram('tools', 1100, 540,
                L('Tools: what each one runs and which library it links',
                  'ツール: それぞれが何を動かし、どのライブラリをリンクするか'),
                L('The tools use the public API; the two developer tools read internal headers and link '
                  'the internal static library.',
                  'ツールは公開 API を使う。開発者向けの 2 つは内部のヘッダを読み、内部の静的ライブラリをリンクする。'))
    public = [
        ('cli', 'hdrg-cli', L('stills: hdrg_convert,\nevery setting', '静止画: hdrg_convert、\n全部の設定')),
        ('video', 'hdrg-video', L('y4m / raw pipes: sessions,\n3D LUT, HDR10+ JSON', 'y4m / raw のパイプ: セッション、\n3D LUT・HDR10+ の JSON')),
        ('ffmpeg', 'hdrg-ffmpeg', L('files through FFmpeg\nand tools/ffmpeg', 'FFmpeg と tools/ffmpeg\nでファイルを')),
        ('inspect', 'hdrg-inspect', L('HDR signaling check', 'HDR の信号の検査')),
        ('gui', 'hdrg-gui', L('SDL3 + Dear ImGui,\nthe OS\'s decoders and\nencoders (not installed)',
                              'SDL3 + Dear ImGui、\nOS のデコーダ・エンコーダ\n(install しない)')),
    ]
    internal = [
        ('bench', 'hdrg-bench', L('tests/tools/: time of each stage', 'tests/tools/: 段ごとの時間')),
        ('pdata', 'hdrg-predictor-data', L('tests/tools/: the light predictor\'s rows', 'tests/tools/: 光の予測器の材料')),
    ]
    for (x, w), (id, label, sub) in zip(row(30, 1070, len(public), 16), public):
        d.box(id, x, 40, w, 72, label, sub, kind='app', mono_sub=False)
        d.edge(id, 'pub', da=(x + w / 2 - 30) / 1040)
    d.box('pub', 30, 160, 1040, 52, 'libhdrgrading',
          L('the public API alone (static or shared; a shared one exports the HDRG_API functions)',
            '公開 API だけ (静的か共有。共有なら HDRG_API の関数だけを公開)'),
          kind='api', mono_sub=False)
    for (x, w), (id, label, sub) in zip(row(30, 1070, len(internal), 16), internal):
        d.box(id, x, 262, w, 64, label, sub, kind='common', mono_sub=False)
        d.edge(id, 'int', da=(x + w / 2 - 30) / 1040)
    d.box('int', 30, 374, 1040, 52, '${HDRG_INTERNAL}',
          L('the same objects with the internal headers (src/): the static library, or '
            'hdrgrading_internal beside a shared one',
            '同じオブジェクトと内部のヘッダ (src/): 静的なライブラリ、共有のときは並べて作る hdrgrading_internal'),
          kind='common', mono_sub=False)
    d.text(30, 464, L('hdrg-ffmpeg is built when libavcodec / libavformat / libswscale are found; hdrg-gui '
                      'with -DHDRG_BUILD_GUI=ON (FFmpeg on Linux: HDRG_GUI_MEDIA=ffmpeg).',
                      'hdrg-ffmpeg は libavcodec / libavformat / libswscale があるときに組む。hdrg-gui は '
                      '-DHDRG_BUILD_GUI=ON で (Linux の FFmpeg 版は HDRG_GUI_MEDIA=ffmpeg)。'))
    d.text(30, 484, L('hdrg-inspect includes the public headers alone; with the jxl family built it links '
                      '${HDRG_INTERNAL} for the bundled libjxl\'s objects.',
                      'hdrg-inspect は公開ヘッダだけを読む。系統 jxl を組むときは、同梱の libjxl のオブジェクトのために '
                      '${HDRG_INTERNAL} をリンクする。'))
    d.text(30, 504, L('The tests and fuzz_seeds link ${HDRG_INTERNAL} too; the plugins and the bindings '
                      'link the public library.',
                      'テストと fuzz_seeds も ${HDRG_INTERNAL} をリンクし、プラグインとバインディングは公開のライブラリをリンクする。'))

    return d


ALL = [layers, pipeline, still, session, window, gpu, plugins, bindings, tools]
