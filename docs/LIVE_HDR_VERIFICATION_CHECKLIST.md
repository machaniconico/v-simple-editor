# ライブHDR 実機検証チェックリスト

HDR 多層合成 Stage1〜10 を**実機・実HDR素材で end-to-end 発火させて確認**するための手順。
Stage1〜9b は headless selftest／オラクル検証済だが、実HDR + GL + フラグ ON のライブ経路は未検証。
Stage10（ingest 時 ColorMeta 自動 populate）が「真の有効化」リンク。

> 前提: Windows ネイティブ実機（GL 必須）。WSL は GL/MSVC 制約で実再生検証に不向き。

---

## 0. なぜ Stage10 が必要か（load-bearing）
従来 `Timeline::addClip` は codecpar を probe しても `clip.colorMeta` を設定せず、実HDR素材も
`ColorMeta.isHdr=false`（SDR）として取り込んでいた。そのため Stage1〜9b のフラグを全部 ON にしても
`colorMeta.isHdr` が false → HDR 分岐に一切入らず**永久休眠**だった。
Stage10 で `VEDITOR_HDR_INGEST=1` のとき codecpar→ColorMeta を導出し、初めて HDR 経路が点火する。

---

## 1. 必要素材
- **PQ (HDR10) クリップ**: `color_trc = smpte2084`、`color_primaries = bt2020`、10bit。
  確認: `ffprobe -show_streams <file> | grep -E 'color_transfer|color_primaries|pix_fmt'`
  期待: `color_transfer=smpte2084` `color_primaries=bt2020` `pix_fmt=yuv420p10le` 等。
- **HLG クリップ**: `color_trc = arib-std-b67`。
- 比較用 **SDR クリップ**（Rec709/sRGB 8bit）。
- 2 層合成を見るため HDR クリップを **2 つ**（または HDR + overlay）用意。composite16 系のライブ起動条件は「層>=2 ∧ 全層 RGBA64」。

---

## 2. フラグ集合（段階的に ON）
すべて環境変数。既定は全 OFF（= ビット同一）。**まず診断 → 段階的に点火**。

| フラグ | Stage | 役割 |
|---|---|---|
| `VEDITOR_HDR_TRACE=1` | 10 | 診断: import 毎の ColorMeta と（実装次第で）フレーム分岐を qInfo 印字 |
| `VEDITOR_HDR_INGEST=1` | 10 | **点火の起点**: codecpar→ColorMeta を populate（これが無いと以降全休眠） |
| `VEDITOR_GPU_COMPOSITE=1` | 2-9 | **preview の必須入口**: これが無いと `tryGpuComposeLayers` 自体が呼ばれず composite16/IDT/matte16 は preview で永久に非発火（CPU 8bit 経路のまま）。`VideoPlayer.cpp:3964-3971` で multi-track 時のみ起動。**export には不要**（export は CPU SSOT 経路）。 |
| `VEDITOR_HDR_OVERLAY=1` | 4 | overlay を RGBA64 化（composite16 のライブ起動に必須・要 colorMeta.isHdr。これが無いと allRgba64=false で 8bit 降格） |
| `VEDITOR_HDR_EXPORT16=1` | 2/3 | preview/export の 16bit 合成経路（preview では `preview16Applicable` フラグ、export では `use16` フラグ） |
| `VEDITOR_HDR_IDT=1` | 5 | per-clip 入力色空間→統一空間 IDT |
| `VEDITOR_HDR_ODT=1` | 6 | 出力トーンマップ ODT（作業空間→PQ/HLG/SDR） |
| `VEDITOR_HDR_MATTE16=1` | 7/8 | 16bit トラックマット（マット使用時のみ） |
| `VEDITOR_HDR_IDT_GPU=1` | 9/9b | GPU per-fragment IDT（性能） |
| `VEDITOR_DV_XML=1` | 9-DV | HDR export 時に Dolby Vision XML サイドカー生成 |

### 推奨点火順
0. **GUI 無し事前確認**: `./v-simple-editor.exe --hdr-probe=<file>`（§3b）。素材が HDR 検出されるか即判定。`isHdr=true` を確認してから GUI へ。
1. **診断のみ**: `VEDITOR_HDR_TRACE=1` だけ。HDR クリップ import 時のログを見る（下記 §3）。
2. **ingest 点火**: `VEDITOR_HDR_TRACE=1 VEDITOR_HDR_INGEST=1`。ColorMeta.isHdr=true になるか。
3. **preview 16bit**: 上に `VEDITOR_GPU_COMPOSITE=1 VEDITOR_HDR_OVERLAY=1 VEDITOR_HDR_EXPORT16=1` を追加。
   **preview の発火条件は全て満たす必要がある（`preview16Applicable = flag && !hasMatte && layerCount>=2 && allRgba64`）**:
   - `VEDITOR_GPU_COMPOSITE=1`（入口）+ multi-track（**2 層以上**。単一 HDR クリップは composite16 preview を通らない）
   - 2 層とも HDR（= 全層 RGBA64。混在 SDR+HDR は allRgba64=false で 8bit 降格）
   - **マット無し**（マット使用時は §別途 `VEDITOR_HDR_MATTE16=1` の matte16 経路）
   - 実機 GL 利用可（`m_gpuCompositor->isAvailable()`。不可なら CPU フォールバック）
4. **色変換**: `VEDITOR_HDR_IDT=1 VEDITOR_HDR_ODT=1` を追加。preview の見た目が正しくトーンマップされるか。
   （GPU IDT を試すなら `VEDITOR_HDR_IDT_GPU=1` も。Linear transfer 素材は CPU/GPU 両 IDT で一致）
5. **export パリティ**: 同じフラグ集合で書き出し、preview==export を比較。
   **注**: export は CPU SSOT 経路なので `VEDITOR_GPU_COMPOSITE` / `VEDITOR_HDR_OVERLAY` は不要、
   `VEDITOR_HDR_EXPORT16=1`（+ `VEDITOR_HDR_INGEST=1` で colorMeta、+ IDT/ODT 任意）で 16bit 化する。
   preview だけ追加フラグ（GPU_COMPOSITE/OVERLAY）が要る点に注意。

---

## 3. 期待ログ（VEDITOR_HDR_TRACE=1）
import 時（`Timeline::addClip`、`src/Timeline.cpp:3002-3008`）。実コードの marker は **`[hdr-ingest]`**（`[HDR_TRACE]` ではない）:
```
# INGEST OFF（休眠の証拠）:
[hdr-ingest] file= <file.mov> ingest= off videoStreamFound= true derived= "HDR Rec2020 PQ 10bit" clip= "SDR Rec709 sRGB 8bit"
#   ↑ derived は検出結果。clip= が SDR のまま = ingest OFF なので休眠（点火していない）
# INGEST ON（点火）:
[hdr-ingest] file= <file.mov> ingest= on videoStreamFound= true derived= "HDR Rec2020 PQ 10bit" clip= "HDR Rec2020 PQ 10bit"
#   ↑ clip= が HDR = colorMeta に書き込まれ点火
```
> 実コード（`Timeline.cpp:3003-3007`）は `qInfo() << "[hdr-ingest] file=" << ... << "derived=" << describe(derived) << "clip=" << describe(clip)` を出力。
> `describe()` の正確な文字列は `src/color/ClipColor.cpp` 参照。要点は **derived の isHdr/primaries/transfer/bitDepth が実素材と一致**し、ON のとき **clip= も HDR になる**か。
> 一致しなければ Stage10 の `fromCodecParams` マッピングか probe 捕捉のバグ。

### 3b. GUI 無しの事前確認（推奨・最速）— `--hdr-probe`
GUI を起動せず、素材ファイル単体で Stage10 の検出ロジック（`Timeline::addClip` と同一の `captureColorInputs`→`fromCodecParams`）が **HDR を検出するか**を確認できる headless CLI:
```
./v-simple-editor.exe --hdr-probe=<file.mov>
```
codecpar の raw 値（color_primaries / color_trc / pix_fmt / bitDepth / mastering-metadata 有無）と **導出 ColorMeta（isHdr 含む）**、現在の env フラグ状態、preview 点火に不足しているフラグを印字する。
> load-bearing な「この実素材は HDR 検出されるか」を GUI/GL 無しで即判定できる。`isHdr=false` なら §5-1（codecpar の trc が UNSPEC か等を `ffprobe` で確認）へ。`isHdr=true` なら検出は OK、残るは GL/preview の点火条件（§2 推奨点火順 3 以降）のみ。

---

## 4. 確認ポイント
- [ ] §3 で **INGEST OFF だと SDR、ON だと HDR** と出る（= Stage10 が効いている）。
- [ ] 2 層 HDR で preview がクラッシュせず描画される（混在 SDR+HDR は 8bit composite へ安全降格＝クラッシュ無し）。
- [ ] `VEDITOR_HDR_ODT=1` で白飛び/色被りなくトーンマップされる（OFF 比で過抑制/過明にならない）。
- [ ] 同フラグで export → preview と見た目一致（NLE パリティ）。
- [ ] **回帰**: 全フラグ OFF に戻すと従来と完全同一（SDR プロジェクトの見た目・ProjectFile 不変）。

---

## 5. 発火しないときの切り分け
1. §3 で ON でも SDR のまま → `fromCodecParams` か probe 捕捉のバグ（codecpar の color フィールドが UNSPECIFIED の可能性も。`ffprobe` で素材の tag を確認）。trc 未シグナルだが mastering metadata を持つ素材は Stage `d5e18df` で HDR 判定されるはず（`av_packet_side_data_get` で coded_side_data を見る）。
2. ColorMeta は HDR だが preview が SDR のまま → 以下を順にチェック:
   - **`VEDITOR_GPU_COMPOSITE=1` が未設定**（最頻。これが無いと `tryGpuComposeLayers` が呼ばれず composite16 系は preview で一切発火しない）。
   - **単一トラック**（`layerCount>=2` 未達。2 層以上必要）。
   - `VEDITOR_HDR_OVERLAY` 未 ON で overlay が RGB888 のまま → allRgba64=false（write-once: `VideoPlayer.cpp:4505` openTrackDecoder 周辺、消費は :5226）。
   - マット使用中（preview16 は matte-free のみ。マットは `VEDITOR_HDR_MATTE16=1`）。
   - 実機 GL 不可（`m_gpuCompositor->isAvailable()` false → CPU フォールバック）。
3. preview は HDR だが export が SDR → export 経路のフラグ（EXPORT16/ODT）漏れ。export は GPU_COMPOSITE 非依存（CPU SSOT）なので preview と別フラグ集合である点に注意。

---

## 関連
- PRD: `.omc/plans/PRD-STAGE10-INGEST-COLORMETA.md`
- ロードマップ: memory `project-hdr-multilayer-roadmap-2026-06-03`
