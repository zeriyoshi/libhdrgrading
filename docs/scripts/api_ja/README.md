# API リファレンスの日本語訳

日本語の API リファレンス (`docs/dev/ja/api/`) は、公開ヘッダ (`include/hdrgrading/*.h`) のコメントを日本語に訳したものを載せ、その下に英語の原文を開けるようにしています。訳はヘッダごとの `<ヘッダ>.json` に置きます。訳の作り直しを避けるためのキャッシュを兼ねるので、コミットします。

## ファイルの形

コメントごとに 1 つの項目を、ヘッダの順に並べます。

```json
{
  "key": "コメントの英語の語 (空白をまとめたもの) の SHA-256 の先頭 16 桁",
  "of": "何の説明か (型・関数・型.メンバ・マクロ、(header) はヘッダ自体)",
  "en": ["ヘッダのコメントの行 (build.py が書く)"],
  "ja": ["訳の行"]
}
```

`key`・`of`・`en` は `docs/scripts/build.py` が書くので、手で直しません。人が書くのは `ja` だけです。

## 変わったときの手順

1. 公開ヘッダのコメントを変えたら `python3 -I docs/scripts/build.py` を走らせます。build.py は次のようにファイルをヘッダに合わせてから失敗し、訳のないコメントを `untranslated: …` で並べます。
   - 英語が変わらないコメントは、`key` が同じなので訳をそのまま残します。並びと `of` だけを直します。折り返しだけを変えたコメントも同じ鍵です (形が変われば下の確認で失敗します)。
   - 新しいコメントと変わったコメントは `"ja": null` で入ります。
   - どのコメントも使わなくなった項目は消します。
2. `"ja": null` の項目だけを訳します。訳のあるものは訳し直しません。
3. `python3 -I docs/scripts/apiref.py docs/scripts/api_ja/<ヘッダ>.json` で形を確かめ、build.py を走らせ直します。
   - 確かめること: 訳がないもの、空行で区切ったまとまりの数、箇条 (`- `) の数、`hdrg_` / `HDRG_` の識別子が原文と同じこと。
   - CI は `build.py --check --strict` で、ファイルが古いとき、訳がないとき、形が原文と合わないときに失敗します。

## 訳し方

- **形:** 原文の空行 (まとまりの区切り) と箇条 (`- ` で始まる行) を同じ数だけ保ちます。1 つの段落と 1 つの箇条は 1 行に書きます。build.py は日本語の行と行の間に空白を入れずにつなぎますが、1 行のほうが読みやすいためです。
- **表:** 字下げした表 (列の間に 2 つ以上の空白) は、行の数と字下げをそのままにします。列の中だけを訳します。
- **変えないもの:** 識別子・関数名・メンバの名前・コード・数値・単位・式・規格の名前 (BT.2020、ST 2094-50 など) は原文のまま書きます。`hdrg_` / `HDRG_` の識別子は、ページで自動でリンクになります。
- **文体:** 文は「です・ます」で書きます。メンバの説明のような名詞句は名詞句のまま訳します (「出力の幅 (0 は入力の幅)」)。
- **書き方:** 括弧は半角の `( )` の前後に空白を置きます。英数字と日本語の間にも空白を置きます (「SDR のファイル」)。句読点は「、」「。」を使います。
- **内容:** 原文にないことを足さず、あることを落としません。分かりにくい原文でも意味を変えずに訳します。原文の誤りに気づいたら、ヘッダのほうを直します。
- **語:** マニュアルの日本語 (`docs/scripts/content/ja/`) と同じ語を使います。主なものを下に挙げます。

| 英語 | 日本語 |
|---|---|
| context / session / frame / still image | コンテキスト / セッション / フレーム / 静止画 |
| plugin / filter / codec / module | プラグイン / フィルタ / コーデック / モジュール |
| reader / writer | 読み手 / 書き手 |
| class / instance / callback / API table | クラス / インスタンス / コールバック / API 表 |
| stage / stage runner / chain | 段 / 段の実行器 / 連鎖 |
| job / band / full frame / row / plane | ジョブ / 帯 / 全画面 / 行 / プレーン (Y'CbCr の面は「面」) |
| stride / pitch | 行の幅 / ピッチ |
| side data / metadata / payload | サイドデータ / メタデータ / ペイロード |
| window (temporal) / clip / shot / cut | ウィンドウ / クリップ / ショット / 切れ目 |
| in flight / submit / land / passthrough | 飛行中 / 投入する / land する / 素通し |
| latency / latency profile / preroll | 遅延 / 遅延のプロファイル / preroll |
| device / host memory | device / host のメモリ |
| analysis / plan / apply / pass A | 解析 / プラン / 適用 / 走査 A |
| cell / grid / bilateral grid | セル / 格子 / bilateral grid |
| rendition / render | 描画 / 描く |
| display caps / headroom / peak | 表示能力 / ヘッドルーム / ピーク |
| diffuse (white) / boost / light | 拡散面 / 上乗せ / 光 |
| gain map / tone map / base image / alternate | ゲインマップ / トーンマップ / 基本の画像 / 代替 |
| gamut / primaries / transfer / matrix | 色域 / 原色 / 伝達特性 / 行列 |
| full range / limited range | フルレンジ / リミテッドレンジ |
| chroma / chroma siting / subsampled | 色差 / 色差のサンプル位置 / 間引いた |
| grain / film grain / resynthesize | 粒 / コーデックの粒 / 再合成する |
| motion / motion vector / confidence / occlusion | 動き / 動きベクトル / 信頼度 / 隠れ |
| interpolation / output rate | 補間 / 出力のレート |
| enlarge / shrink / scaler | 拡大 / 縮小 / scaler |
| deterministic / byte for byte / tolerance | 決定的 / バイト一致 / 許容誤差 |
| status / allocator / thread / worker / pool | 状態 / アロケータ / スレッド / ワーカー / プール |
| option / setting / preset / policy | オプション / 設定値 / プリセット / 方針 |
| version / freeze / reference / mapping | 版 / フリーズ / リファレンス / マッピング |
| struct / member / field | 構造体 / メンバ / 欄 |
| registration / load | 登録 / 読み込み |
| the library / the app / the host | ライブラリ / アプリ / ホスト |
