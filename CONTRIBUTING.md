# Contributing

## 開発環境

推奨は DevContainer (`.devcontainer/`、Debian 13 + LLVM 23 + GCC 16 + Valgrind + emsdk)。
VS Code などで開けば、ツールチェインと依存の取得 (`scripts/fetch_deps.sh`) が揃う。
エディタを使わずに同じ環境で実行するには `scripts/docker_dev.sh` を使う。

```sh
scripts/docker_dev.sh                                   # 対話シェル
scripts/docker_dev.sh cmake --workflow --preset dev     # ビルド + テスト
scripts/docker_dev.sh --build true                      # イメージの再ビルド
```

コンテナの中のコマンドは `scripts/jobs.sh` の下で走り、ビルドとテストの並列の数がコアと空きメモリ
(コンテナとホストの小さいほう) に合わせて決まる。

コンテナ内のビルドディレクトリは `build/linux-<preset>` (環境変数 `HDRG_BUILD_PREFIX=linux-`)
で、ホストのビルド (`build/<preset>`) と衝突しない。コンテナの `~/.cache` (Emscripten のシステムライブラリと
port、ccache) は実行をまたいで残る Docker のボリューム `hdrg-dev-<arch>-cache` で、`HDRG_DEV_CACHE=<ディレクトリ>`
を指定するとホストのディレクトリをそこにマウントする (CI はその中身を Actions のキャッシュに置く)。

## ビルド

依存はすべて `third_party/` の submodule のソースから CMake がビルドする (事前ビルドなし)。

```sh
scripts/fetch_deps.sh                  # submodule + jpegli の入れ子依存 (highway, libjpeg-turbo)
cmake --workflow --preset dev          # configure + build + test (Debug)
scripts/jobs.sh cmake --workflow --preset dev    # 同じことを空きメモリに合わせた並列で
```

`scripts/jobs.sh` は並列の数を min(コア数, (空きメモリ − 総量の 1/8 (最低 2 GiB)) / 1 ジョブのメモリ)
にして `CMAKE_BUILD_PARALLEL_LEVEL` と `CTEST_PARALLEL_LEVEL` に入れる (すでに設定されていればそのまま)。
1 ジョブのメモリは `HDRG_JOB_MIB` (既定 1024 MiB。Valgrind を含め、測った最大は 700 MiB 未満)。引数なしなら数だけを出す。

| preset | 内容 |
|---|---|
| `dev` / `release` | Debug (静的ライブラリ) / Release (共有ライブラリ: 公開する記号の一覧 (test_exports) と install した物を使う側 (test_install) も確かめる) |
| `temporal` | Debug で、既定 OFF の時間方向の機能 (`HDRG_WITH_TEMPORAL`: 統計の時間方向のフィルタ・動き補償の補間・多フレームの拡大) を ON にしたもの |
| `nolcms2` | lcms2 なし: JPEG はゲインマップの形だけ、JPEG XL なし (PQ が要るスイートは skip) |
| `minimal` | Debug で、外せる内蔵プラグインを全部外したもの (`HDRG_PLUGINS_EXCLUDE`: Lanczos と JPEG だけ。外したもののテストは ignore か組まない) |
| `heif` | HEIF のモジュール (`HDRG_WITH_HEIF`: システムの libheif、なければビルド時に libde265・x265・libheif を取得する) |
| `avif` | AVIF のモジュール (`HDRG_WITH_AVIF`: システムの libavif。ほかのプリセットには入らない) |
| `asan-ubsan` / `tsan` | AddressSanitizer + UBSan / ThreadSanitizer |
| `msan-core` | MemorySanitizer (Linux + Clang、コーデックの系統を外したもの: `HDRG_PLUGINS_EXCLUDE=codecs`。src/io は組む) |
| `valgrind` | memcheck (`CMAKE_TEST_LAUNCHER`、Debug のまま: 最適化したコードは誤検知・見逃しが出る) |
| `coverage` / `coverage-msvc` | Clang source-based coverage (時間方向の機能も ON。Windows は clang-cl の `coverage-msvc`)。レポートは `scripts/coverage.sh` |
| `fuzz` | libFuzzer + ASan/UBSan (`tests/fuzz` の `fuzz_convert`、シードは `fuzz_seeds`)。Clang とサニタイザが必須で、third_party を含む全体を計装する |
| `wasm` / `wasm-mt` / `wasm-scalar` / `wasm-scalar-mt` / `wasm-webgpu` / `wasm-debug` | Emscripten ({SIMD128, scalar} × {単一スレッド, pthreads}、SIMD128 + ブラウザの WebGPU (emdawnwebgpu、JSPI)、Debug)。開発イメージで `source $HDRG_EMSDK_DIR/emsdk_env.sh` してから使う。テストは node で走る |

`temporal`・`heif`・`avif` とカバレッジのプリセットが ON にするオプション (`HDRG_WITH_TEMPORAL`・`HDRG_WITH_HEIF`・`HDRG_WITH_AVIF`) は、
第三者の権利との関係で利用が安全だと言い切れないため既定で OFF にしているもので、これらのプリセットはこのプロジェクトの検証のためのもの。
配るバイナリやほかのプリセットには入れない。有効にする場合は自己責任で、自分でビルドする (README_ja.md の「既定で OFF のオプションについて」、英語の README.md は "About the options off by default")。

fuzz はシードを書き出してから回す (シードは 48 × 32 の JPEG / PNG / GIF とライブラリ自身の
HDR 出力。assets の kodim23 のような大きな画像は 1 回の実行が長く、探索が進まない):

```sh
scripts/docker_dev.sh bash -c 'cmake --preset fuzz && cmake --build --preset fuzz && \
  mkdir -p build/fuzz-corpus && build/linux-fuzz/fuzz_seeds build/fuzz-corpus && \
  build/linux-fuzz/fuzz_convert -max_total_time=180 -rss_limit_mb=4096 build/fuzz-corpus'
```

並列に回すときは `-jobs=N -workers=N` (同じコーパスを共有する独立したプロセス) を使う。`-fork=N` は、
開発イメージの LLVM 23 の libFuzzer では、1 つのジョブの中で同じ入力が 2 回縮む (REDUCE) と親のプロセスが落ちる
(途中の大きさのコーパスのファイルが特徴量のファイルなしで残り、親のマージがそれを読む)。

コンパイラは `CC` / `CXX` で選ぶ (例: `CC=gcc-16 CXX=g++-16 cmake --workflow --preset dev`)。
`HDRG_WITH_SYSTEM_DEPS=ON` で zlib / libpng / giflib / lcms2 をシステムのものに切り替えられる
(jpegli は常に同梱版)。

## CI と GitHub Pages

- `.github/workflows/ci.yml`: 規約・書式・生成物 (シェーダとドキュメント) の検査、Linux (amd64 / arm64 × GCC 16 / Clang × Debug / Release、
  開発イメージ内。lcms2 なしと時間方向の機能ありも)、ASan+UBSan / TSan / MSan、fuzz (3 分)、FFmpeg のフィルタ
  (共有の本体を入れ、パッチを当てた FFmpeg を組んで test_filter)、WASM の 5 種 (node)、10 のプラットフォームのカバレッジ
  (`scripts/coverage.sh --keep-going`) とその合算 (`coverage-merge`)、macOS (arm64 / Intel)、Windows (clang-cl / cl、現在は非ブロッキング)、
  GUI (hdrg-gui を Linux の GStreamer 版と FFmpeg 版・macOS・Windows x64 / arm64 で組んで test_gui_*。Windows は非ブロッキング)。
  実行ごとに変わらないものは Actions のキャッシュに置き、プルリクエストの外でだけ書く: 開発イメージのレイヤ
  (buildx の gha)、FFmpeg のタグの clone (`scripts/build_ffmpeg.sh` の `HDRG_FFMPEG_CACHE`) とそのコンパイルの ccache、
  WASM のビルドごと (と Pages のデモ) の Emscripten のキャッシュ、fuzz のコーパス (実行をまたいで育てる。
  クラッシュの入力は artifact の `fuzz-crashes`)。キーは中身を決めるもの (FFmpeg のタグ、emsdk の版と
  `cmake/HdrgWasm.cmake`) で、ccache とコーパスは実行ごとに新しく保存して最新を戻す
- `.github/workflows/nightly.yml`: 毎日 (02:23 JST) と手動で、Valgrind (amd64 / arm64)。
  push ごとには走らせない (1 回に 1 時間を超える。push と PR は sanitizer で見る)
- `.github/workflows/pages.yml`: main への CI が終わったら、ドキュメント (`docs/scripts/build.py --site`、
  サイトのトップ)、WASM デモ (`scripts/wasm_demo.sh`) とカバレッジ HTML を 1 つの artifact にまとめて
  GitHub Pages へデプロイする。デモ付きのサイトは手元でも `scripts/docker_dev.sh scripts/wasm_demo.sh` で
  `build/web` に組み立て、静的サーバーで配信すれば動く

## 依存関係 (third_party)

外部依存は `third_party/` の git submodule としてリリースタグの指すコミットに pin している。
`.gitmodules` の `branch = <tag>` は Renovate (`.github/renovate.json`) のタグ追従用であり、
git 標準の運用 (`git submodule update --remote`) からは外れることに注意。

### 手動更新 (原則 Renovate PR に任せる)

```sh
cd third_party/<dep>
git fetch --depth 1 origin tag <new-tag>
git checkout <new-tag>
cd ../..
git config -f .gitmodules submodule.third_party/<dep>.branch <new-tag>
cmake --workflow --preset dev --fresh
```

- **GitHub Actions**: Dependabot (`.github/dependabot.yml`) が action を上げ、同じ PR で
  `.github/workflows/actions.lock` (公式の Lockfile) も書き直す。ワークフローの `uses:` を
  足す・変えるときは `gh extension install github/gh-actions-lock --pin v0.4.1` を入れて
  `gh actions-lock` を実行し、Lockfile をコミットする (手で編集しない。同じリポジトリの composite
  action は `$/.github/actions/...` で参照する)。`--migrate-local-actions` は submodule
  (third_party/libjxl など) の action.yml まで書き換えるので使わない。CI の checks が
  `gh actions-lock --verify` で検査する
- **emsdk**: `.gitmodules` の branch と `.devcontainer/Dockerfile` の `EMSDK_VERSION` を一致
  させる (CI が検査する。Renovate は両方を 1 つの PR にまとめる)
- **Valgrind**: Renovate が `VALGRIND_VERSION` を上げたら、同じ PR で `VALGRIND_SHA256` も更新する
- **jpegli**: google/jpegli には**リリースタグがない**ため commit pin (Renovate 対象外)。
  上流 main の履歴を確認してセキュリティ修正を含む commit を選び、`third_party/jpegli` を
  checkout して全テストで検証する。upstream のヘッダは extern "C"
  API なのに C++ 専用 include を含むため、C からは `src/plugins/jpeg/jpegli_compat/` 経由で使う
- **Dawn** (WebGPU): submodule ではなく、`scripts/build_dawn.sh` がタグでピンしてソースから組む。タグは手で上げ、
  `.devcontainer/Dockerfile` の `DAWN_TAG` と一致させる (CI が検査する)
- **libheif / libde265 / x265** (HEIF、`HDRG_WITH_HEIF`、オプトイン): submodule ではなく、システムに
  なければビルド時に取得する。版は `cmake/HdrgHeif.cmake` の `HDRG_HEIF_*_TAG` で手で上げ、
  `cmake --workflow --preset heif --fresh` を Mac と Linux で通す (test_heif の ImageIO の確認は Mac で)
- **libjxl / brotli** (JPEG XL、内蔵プラグインの系統 jxl): タグの submodule (Renovate が上げる)。libjxl は
  自分の入れ子の依存を使わず、jpegli の Highway・同梱の brotli・lcms2 で組む (ソースの一覧は libjxl の
  `lib/jxl_lists.cmake` から。ゲインマップの箱の `extras/gain_map.cc` だけを足す: `cmake/HdrgDeps.cmake`)。
  上げる前に libjxl と brotli のセキュリティ情報を確かめ、test_jxl と全構成 (MSan・WASM を含む) で確かめる
- **libavif** (AVIF、`HDRG_WITH_AVIF`、オプトイン): 同梱しない。システムの libavif (pkg-config、1.2 以降) を AVIF の
  モジュールが使い、プリセット `avif` だけが組む。macOS は Homebrew、開発イメージは Debian の `libavif-dev`。
  既定は変換のスレッドで符号化する (書き手のオプション `threads`、0 = 変換の数)。システムの libaom の出力は版で、版によっては
  スレッド数でも変わるので、AVIF のバイト列はライブラリの決定性の保証の外。テストは `threads=1` で走らせる

## コーディング規約

CLAUDE.md の規約に従う (C11、固定幅整数、`_t` サフィックス、関数先頭での変数宣言・型ごとに 1 文・
決まった順序、`{}` 省略禁止、ロジックごとの空行、SPDX ヘッダ)。書式は `.clang-format`、静的解析は
`.clang-tidy` (どちらも LLVM 23。zendriver の `common/` と同じ設定)。

```sh
python3 -I scripts/check_style.py                       # 規約の検査 (CI の checks ジョブと同じ)
scripts/docker_dev.sh bash -c 'find src include tools tests -name "*.[ch]" -print0 | xargs -0 clang-format -i --style=file'
```

Claude Code では `.claude/settings.json` のフックが、C ファイルを編集するたびに `check_style.py` を実行して違反を返す。
