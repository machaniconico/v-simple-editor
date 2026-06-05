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
| `VEDITOR_HDR_OVERLAY=1` | 4 | overlay を RGBA64 化（composite16 のライブ起動に必須・要 colorMeta.isHdr） |
| `VEDITOR_HDR_EXPORT16=1` | 2/3 | preview/export の 16bit 合成経路 |
| `VEDITOR_HDR_IDT=1` | 5 | per-clip 入力色空間→統一空間 IDT |
| `VEDITOR_HDR_ODT=1` | 6 | 出力トーンマップ ODT（作業空間→PQ/HLG/SDR） |
| `VEDITOR_HDR_MATTE16=1` | 7/8 | 16bit トラックマット（マット使用時のみ） |
| `VEDITOR_HDR_IDT_GPU=1` | 9/9b | GPU per-fragment IDT（性能） |
| `VEDITOR_DV_XML=1` | 9-DV | HDR export 時に Dolby Vision XML サイドカー生成 |

### 推奨点火順
1. **診断のみ**: `VEDITOR_HDR_TRACE=1` だけ。HDR クリップ import 時のログを見る（下記 §3）。
2. **ingest 点火**: `VEDITOR_HDR_TRACE=1 VEDITOR_HDR_INGEST=1`。ColorMeta.isHdr=true になるか。
3. **preview 16bit**: 上に `VEDITOR_HDR_OVERLAY=1 VEDITOR_HDR_EXPORT16=1` を追加。2 層 HDR で composite16 分岐に入るか。
4. **色変換**: `VEDITOR_HDR_IDT=1 VEDITOR_HDR_ODT=1` を追加。preview の見た目が正しくトーンマップされるか。
5. **export パリティ**: 同じフラグ集合で書き出し、preview==export を比較。

---

## 3. 期待ログ（VEDITOR_HDR_TRACE=1）
import 時（`Timeline::addClip`）:
```
# INGEST OFF（休眠の証拠）:
[HDR_TRACE] addClip <file.mov> colorMeta= SDR Rec709 sRGB 8bit   ← 実HDRなのに SDR = 休眠原因
# INGEST ON（点火）:
[HDR_TRACE] addClip <file.mov> colorMeta= HDR Rec2020 PQ 10bit   ← isHdr=true で点火
```
> ログ文言は実装に合わせて読み替え。要点は **isHdr と primaries/transfer/bitDepth が実素材と一致**しているか。
> 一致しなければ Stage10 の `fromCodecParams` マッピングか probe 捕捉のバグ。

---

## 4. 確認ポイント
- [ ] §3 で **INGEST OFF だと SDR、ON だと HDR** と出る（= Stage10 が効いている）。
- [ ] 2 層 HDR で preview がクラッシュせず描画される（混在 SDR+HDR は 8bit composite へ安全降格＝クラッシュ無し）。
- [ ] `VEDITOR_HDR_ODT=1` で白飛び/色被りなくトーンマップされる（OFF 比で過抑制/過明にならない）。
- [ ] 同フラグで export → preview と見た目一致（NLE パリティ）。
- [ ] **回帰**: 全フラグ OFF に戻すと従来と完全同一（SDR プロジェクトの見た目・ProjectFile 不変）。

---

## 5. 発火しないときの切り分け
1. §3 で ON でも SDR のまま → `fromCodecParams` か probe 捕捉のバグ（codecpar の color フィールドが UNSPECIFIED の可能性も。`ffprobe` で素材の tag を確認）。
2. ColorMeta は HDR だが composite16 に入らない → 「層>=2 ∧ 全層 RGBA64」未達。`VEDITOR_HDR_OVERLAY` 未 ON、または overlay デコードが RGB888 のまま（decodePoolFrame の write-once: VideoPlayer.cpp openTrackDecoder 周辺）。
3. preview は HDR だが export がSDR → export 経路のフラグ（EXPORT16/ODT）漏れ。

---

## 関連
- PRD: `.omc/plans/PRD-STAGE10-INGEST-COLORMETA.md`
- ロードマップ: memory `project-hdr-multilayer-roadmap-2026-06-03`
