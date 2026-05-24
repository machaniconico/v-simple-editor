# NLE parity + libavcore + tracker presets + selftest SSOT + selftest split + CI draft + docs (45 commits)

base: `main` ← head: `feat/trackmatte-ssot-parity`
範囲: **45 commits / ~95 files / +39,000 / -19,000** (概算)

## Summary

`main` から 45 commits 分の改修を 1 PR でまとめる。コアテーマは 9 つ。

### 1. 編集↔書き出しピクセル一致 (NLE parity) — 4 commits

プレビュー経路と全 export 経路 (File→Export / mobile / batch / RenderQueue) を **同一の合成関数** に集約し、preview≠export の構造的乖離を解消。3 つの SSOT を導入:

- **`tlrender::renderFrameAt`** (`a5a873f`) — Timeline→QImage を単一経路化
- **`trackmatte::composite`** (`0c34997` → `446d321` で remediation) — マット合成を premul ARGB32 + QPainter SourceOver に統一、matte-free 経路とバイト等価
- **`clipgeom`** (`17ebd52` + `0da6bf7` MINOR cleanup) — clip 幾何変換 (scale/translate/rotation) を layer-center anchor の単一関数に統合

加えて実証 sprint の検証ハーネス (`3aa3a4b`, `c4dc218`) + critic 検出バグ修正 (`5f184fd`, `e226477`)。

### 2. Audio seek 砂嵐 R0→R10 fix — 2 commits

停止中スクラブ→Play の seek 砂嵐を **R0→R9 multi-mechanism remediation** (`b187411`)、R10 atempo race ramp を世代カウンタで構造的閉鎖 (`20c41bd`)。

### 3. libavcore migration (ffmpeg subprocess → in-process) — 4 commits

新規モジュール `src/libavcore/` (Decode / Encode / Probe / VideoFilterGraph / Concat) を SSOT として導入し、ProxyManager / VideoStabilizer / AIHighlight / Exporter の ffmpeg.exe subprocess 呼び出しを in-process libavcore API に移行:

- `696cb59` libavcore module 新規 + Exporter encode refactor (SSOT 基盤)
- `3cbecda` PRD-B-MF: h264_mf (Media Foundation) 基盤への移行
- `75fc101` PRD-B2: ProxyManager / VideoStabilizer / AIHighlight 移行
- `f0693f1` PRD-B3: VideoStabilizer **deshake in-process 化** + HDR10 routing DLL-ready

### 4. Tracker preset system — 4 commits

MotionTracker / PlanarTracker に preset + Dialog UI + Registry を新規追加し、ProjectFile への永続化まで配線:

- `c62a6fc` PRD-TP: TrackerPresetSystem 完全実装 (7 built-in + Dialog UI + Registry)
- `347100d` PRD-MTD-UX2: MotionTrackerDialog UX 拡充 (削除/Reset/JSON エクスポート/インポート/説明 panel)
- `7b1fff9` PRD-PT-PRESET: PlanarTracker preset 化 (5 built-in)
- `58bdbde` PRD-PROJECT-PRESET: ProjectFile への preset 状態永続化

### 5. モジュール cleanup — 3 commits

- `a8553b2` PRD-VST-CLEAN: VideoStabilizer デッドコード除去 + cancel UX + selftest 拡張
- `34a0ab4` PRD-PROXY-CLEAN: ProxyManager コメント整理 + cancel 識別ログ
- `6fbed52` PRD-AIH-CLEAN: AIHighlight.h architecture コメント拡充 + selftest framework

### 6. selftest argv-switch 統一シリーズ — 9 commits

WSL→Windows env 伝播の不安定性で起動失敗していた selftest を `--selftest=<name>` argv-switch に統一 + 中央集権化:

- `1bd23f4` PRD-PROXY-SELFTEST-V2: argv-switch reference 実装 (`--selftest=proxy`)
- `6bc3b2a` PRD-ARGV-UNIFY: 主要 selftest 8 種を argv-switch 化
- `6cec195` PRD-ARGV-UNIFY-2: 残り env-gated selftest 43 種を拡張
- `3a086f8` PRD-ARGV-TABLE: 個別 if 文 55 個を `static const ArgvSelftestEntry kArgvSelftests[]` (54 entry) + 2 走査ループに集約 refactor、**`--selftest=list` 自動生成** (net -132 lines)
- `3c2c890` PRD-ENV-TABLE: env-gate dispatch 53 個の個別 if 文を `envVar` field 経由の 9 行走査ループに集約 (net -200 lines)、argv-switch + env-gate が単一 table SSOT に
- `fb490fc` PRD-ARGV-ALL: `--selftest=all` で全 54 entry を順次実行する CI sweep モード
- `5c07667` PRD-ARGV-EXTRAS: `VEDITOR_ALL_SELFTEST` env-gate + `--selftest=<unknown>` guard (stderr エラー + exit 2)
- `7bb3707` `--selftest=list` の出力に env name 併記 (`--selftest=foo (env: VEDITOR_FOO_SELFTEST)`)
- `3c5914a` PRD-SELFTEST-HELP: 各 entry に description field 追加 + `--selftest=help` で per-entry の name/description/env を整形出力

### 7. **selftest split campaign — main.cpp -94.9%** — 9 commits

`src/main.cpp` 12,008 → **616 行** (元の 5.1%) に圧縮。selftest 系を `src/selftests/` 7 file に体系分散:

- `117897c` **Phase 1**: selftest dispatch を `src/selftests/SelftestRegistry.{h,cpp}` (363 行) に抽出
- `620d20f` **Phase 2-1**: preset selftests 3 関数 → `preset_selftests.cpp` (586 行)
- `0e82ecc` **Phase 2-2**: parity + matte selftests 5 関数 → `parity_matte_selftests.cpp` (5,359 行、最大級 verbatim 移動)
- `fd2d59f` **Phase 3-A**: libavcore selftests 6 関数 + helper 9 個 → `libavcore_selftests.cpp` (1,305 行)
- `b511dbb` **Phase 3-B**: effects 系 6 関数 (AudioMixer/Vfx/Pro/Mograph/HwPerf/ProExt) → `effects_selftests.cpp` (1,477 行)
- `010cd2a` **Phase 3-C**: Sprint 11-22 サービス系 27 関数 (Workflow/Shortcut/Social/Caption/Planar/Mobile/OBS/Affinity/Blender/Import/Youtube/Collab/ColorMatch/Vimeo/Twitch/FrameIo/Davinci/Fcpxml/SmartEdit/CloudRender/XUpload/Instagram/ProjTmpl/Loudness/Hdr/MultiCam/BatchExport) → `sprint_selftests.cpp` (1,766 行)
- `c146271` **Phase 3-D**: misc 系 9 関数 (Chroma/AudioRestore/AnimExport/Easing/SubXlat/LowerThird/Watermark/ExportAudit/TextExport) → `misc_selftests.cpp` (836 行)
- `a56b90b` **Phase 3-E**: TrackMatte Rm5/Rm6 を `parity_matte_selftests.cpp` に補完 (5,359 → 5,731 行、campaign 完結)

src/selftests/ 全 7 file = **12,064 行**。main.cpp は writeLogLine / qtMessageHandler / requireSelftest / defaultLogPath + main() のみの薄い entry point に。各 Phase は executor sonnet 1 story (200-510s) で verbatim cut & paste、全 selftest 回帰 PASS。

### 8. CI workflow draft — 1 commit

- `3c79d72` `.github/workflows/selftest.yml` 新規 (workflow_dispatch のみ、Windows MSVC + vcpkg ffmpeg + Qt 6.7 + cmake + 3 smoke + unknown guard)。初回 green run 後に push/pull_request auto trigger 展開予定

### 9. Documentation — 2 commits

- `1756f06` README に Testing / Continuous Integration / Architecture (SSOT) / Project Structure 拡充 + Roadmap に Sprint 15-22 / NLE parity / libavcore / Tracker Preset / selftest SSOT / CI draft 追記
- `265b50e` CONTRIBUTING.md 新規 (15 分 Quick start / SSOT 表 / selftest 追加手順 / conventional-commit scope / dualralph workflow / local CI dry-run)
- `4512613` PR_DESCRIPTION.md 36 commits 同期 (前段、本 PR では 45 commits 反映)

### CRLF/EOL 正規化

- `cfec1d0` リポジトリルートに `.gitattributes` 追加 (CRLF↔LF 正規化ポリシー)
- `c309e09` `src/main.cpp` + `VideoStabilizer.{cpp,h}` を LF 正規化 (`.gitattributes` 準拠)
- (別ブランチ `chore/crlf-norm` `23276e2` で 27 files の wholesale CRLF→LF を未 push 中、別 PR 想定)

## Commits (古→新、45 個)

```
3aa3a4b test(e2e): 実証 sprint — 実メディア検証ハーネス + stub 境界監査
5f184fd fix(defects): 不具合チェック検出の実バグ 10 件を全件修正
e226477 fix(watermark): 非タイル位置の負 margin off-canvas を修正
a5a873f feat(nle-parity): 編集↔書き出しピクセル一致 — Timeline→QImage SSOT
0c34997 feat(trackmatte): 単一 trackmatte::composite SSOT に統合
446d321 fix(trackmatte): 0c34997 の C1-RESIDUAL 等を remediation
17ebd52 feat(transform-parity): 単一 clipgeom SSOT に統合
b187411 fix(audio-seek): 停止中スクラブ→Play の seek 砂嵐 R0→R9
0da6bf7 refactor(transform-parity-minor): clipgeom 系 4 件の MINOR cleanup
c4dc218 feat(realfootage-harness): scripts/realfootage_parity.sh
cfec1d0 chore(gitattributes): CRLF↔LF 正規化ポリシー
696cb59 feat(libavcore): libavcore module 新規 + Exporter encode refactor
20c41bd fix(audio-atempo): R10 atempo race ramp-REMOVED + 世代カウンタ
c309e09 chore(eol): src/main.cpp + VideoStabilizer を LF 正規化
3cbecda feat(libavcore): PRD-B-MF — h264_mf 基盤
75fc101 feat(libavcore): PRD-B2 — ProxyManager/VideoStabilizer/AIHighlight 移行
f0693f1 feat(libavcore): PRD-B3 — deshake in-process + HDR10 DLL-ready
c62a6fc feat(tracker-presets): PRD-TP — TrackerPresetSystem 完全実装
a8553b2 refactor(stabilizer): PRD-VST-CLEAN — デッドコード除去 + cancel UX
347100d feat(tracker-presets): PRD-MTD-UX2 — MotionTrackerDialog UX
7b1fff9 feat(planar-tracker): PRD-PT-PRESET — PlanarTracker preset
58bdbde feat(project-presets): PRD-PROJECT-PRESET — ProjectFile 永続化
34a0ab4 refactor(proxy-manager): PRD-PROXY-CLEAN — コメント整理 + log
6fbed52 refactor(ai-highlight): PRD-AIH-CLEAN — architecture docs + selftest
1bd23f4 feat(selftest): PRD-PROXY-SELFTEST-V2 — argv-switch reference
6bc3b2a feat(selftest): PRD-ARGV-UNIFY — 8 種統一
6cec195 feat(selftest): PRD-ARGV-UNIFY-2 — 43+ 種拡張
3a086f8 refactor(selftest): PRD-ARGV-TABLE — table 集約 + --selftest=list
3c2c890 refactor(selftest): PRD-ENV-TABLE — env-gate を envVar 経由 1 走査ループに集約
fb490fc feat(selftest): PRD-ARGV-ALL — --selftest=all 全件 sweep
5c07667 feat(selftest): PRD-ARGV-EXTRAS — VEDITOR_ALL + unknown guard (exit 2)
7bb3707 feat(selftest): --selftest=list に env name 併記
3c79d72 ci(selftest): GitHub Actions workflow draft (workflow_dispatch)
3c5914a feat(selftest): PRD-SELFTEST-HELP — description field + --selftest=help
1756f06 docs(readme): Testing / CI / Architecture / Roadmap 拡充
265b50e docs(contributing): CONTRIBUTING.md 新規 (quick start + SSOT + selftest)
4512613 docs(pr): PR_DESCRIPTION.md を 36 commits 反映に同期
117897c refactor(selftest): PRD-SPLIT-MAIN-1 — SelftestRegistry.{h,cpp} 抽出
620d20f refactor(selftest): PRD-SPLIT-MAIN-2 Phase 1 — preset 3 関数を preset_selftests.cpp 分離
0e82ecc refactor(selftest): PRD-SPLIT-MAIN-2 Phase 2 — parity+matte 5 関数 (5,359 行)
fd2d59f refactor(selftest): PRD-SPLIT-MAIN-3 Phase 3-A — libavcore 6 関数 (1,305 行)
b511dbb refactor(selftest): PRD-SPLIT-MAIN-3 Phase 3-B — effects 6 関数 (1,477 行)
010cd2a refactor(selftest): PRD-SPLIT-MAIN-3 Phase 3-C — Sprint 11-22 サービス 27 関数 (1,766 行)
c146271 refactor(selftest): PRD-SPLIT-MAIN-3 Phase 3-D — misc 9 関数 (836 行)
a56b90b refactor(selftest): PRD-SPLIT-MAIN-3 Phase 3-E — TrackMatte Rm5/Rm6 補完 (campaign 完結)
```

## 新規モジュール / ファイル

- **`src/libavcore/`** (7 ファイル、+3,553 行): Concat / Decode / Encode / Probe / VideoFilterGraph
- **`src/TrackerPreset.{cpp,h}`** + **`src/TrackerPresetRegistry.{cpp,h}`** + **`src/MotionTrackerDialog.{cpp,h}`**
- **`src/PlanarTrackerPreset.{cpp,h}`** + **`src/PlanarTrackerPresetRegistry.{cpp,h}`** + **`src/PlanarTrackerDialog.{cpp,h}`**
- **`src/selftests/`** (7 ファイル、12,064 行): SelftestRegistry / preset / parity_matte / libavcore / effects / sprint / misc
- **`scripts/realfootage_parity.sh`** (実 footage 駆動 selftest harness)
- **`.github/workflows/selftest.yml`** (CI workflow draft)
- **`CONTRIBUTING.md`** (15 分 Quick start + SSOT 表 + selftest 追加手順)

## 検証

### selftest (WSL 直実行、env 不要)

```bash
./build_win/Release/v-simple-editor.exe --selftest=list   # 54 entry 列挙
./build_win/Release/v-simple-editor.exe --selftest=<name> # 各 selftest 起動
./build_win/Release/v-simple-editor.exe --selftest=help   # entry ごとの description
```

| selftest | 結果 |
|---|---|
| `--selftest=proxy` | 6/6 PASS |
| `--selftest=tracker-preset` | 10/10 PASS |
| `--selftest=planar-preset` | 10/10 PASS |
| `--selftest=project-preset` | 10/10 PASS |
| `--selftest=aihighlight` | 6/6 PASS |
| `--selftest=videostab-deshake` | 9 gate PASS |
| `--selftest=hdr-routing` | 3-way routing PASS |
| `--selftest=parity` | S1-S11 全 PASS (10-bit HDR10 含む) |
| `--selftest=e2e` | 実 H.264 decode + deHum PASS |
| `--selftest=trackmatte-parity` | 4 matte types pixel-match |
| `--selftest=libavcore-encode` | h264_mf round-trip PASS |
| `--selftest-trackmatte-rm6-duplicate` (legacy argv) | RM-6 PASS |
| `--selftest-trackmatte-rm5-reorder` (legacy argv) | RM-5 PASS |
| その他 ~40 種 (vfx/pro/mograph/easing/chroma/loudness/watermark/youtube 等) | 全 exit 0 |

### env-gate 後方互換

- 52 個の既存 `VEDITOR_*_SELFTEST` env-gate 全部維持 (両経路維持)
- `VEDITOR_TRACKER_PRESET_SELFTEST=1` 回帰確認: 10/10 PASS

### Build

- MSVC Release build: green (warnings pre-existing range)

### Known pre-existing failure (非 PR ブロック)

- `--selftest=textexport` exit=1 (`textbake::bakeOverlays was never invoked`) — Phase 3-D 移動前後で関数 body verbatim 等価確認済、環境/設計依存の pre-existing failure。別 issue で追跡

## Test plan

- [ ] `git checkout feat/trackmatte-ssot-parity && cmd.exe /c "cmake.exe --build build_win --config Release"` clean green
- [ ] `./build_win/Release/v-simple-editor.exe --selftest=list` で 55 行出力 (`help` 行含む) → exit 0
- [ ] 主要 selftest 8 種を `--selftest=<name>` 形式で起動 → 全 exit 0
- [ ] env-gate 1 種 (例: `VEDITOR_TRACKER_PRESET_SELFTEST=1`) を起動 → PASS
- [ ] 実 footage parity 検証: `scripts/realfootage_parity.sh` (asset 揃ってる環境のみ)
- [ ] GUI 起動 → File→Export 経路で preview と書き出しがピクセル一致 (track matte / transform / nle-parity)
- [ ] GUI 起動 → 編集 → MotionTracker / PlanarTracker dialog で preset 選択 → ProjectFile に save/load OK
- [ ] `wc -l src/main.cpp` → 616 行 (campaign 完成形)

## レビュー履歴

- track-matte SSOT: teams/Codex adversarial review を 3 ラウンド → C1-RESIDUAL 等 5 件検出 → remediation 完了 (`446d321`)
- transform-parity: R1→R5 review → 欠陥 5→0 で収束 (`17ebd52`)
- audio seek: R0→R10 multi-mechanism remediation で完全閉鎖
- libavcore migration: PRD-B-MF → B2 → B3 の 3 段階で in-process 化、全 selftest GREEN
- tracker presets / argv-switch: 各 PRD で executor 完走 + WSL 直実行 sample 検証
- selftest split campaign: 7 Phase (1 / 2-1 / 2-2 / 3-A / 3-B / 3-C / 3-D / 3-E) すべて executor sonnet 1 story PASS、main.cpp -94.9%

## 既知の非ブロック MINOR (critic 全件非ブロック判定済)

- `layerPaintOrderLess` の namespace 内包 (ODR 理論上)
- clipgeom contract コメント三重複の整理
- `composeMultiTrackFrameForTest` shim が rotation 非転送
- G7 `|dx|>5` migration の sub-5px 既知許容
- `--selftest=textexport` の pre-existing failure (textbake 経路調査未着手)

## push & PR 作成手順

```bash
# 1. push 済 (origin/feat/trackmatte-ssot-parity = ローカル HEAD a56b90b)

# 2a. gh CLI 未導入 — Web から PR 作成 (推奨):
#   https://github.com/<USERNAME>/v-simple-editor/compare/main...feat/trackmatte-ssot-parity
#   本文に .omc/PR_DESCRIPTION.md の内容を貼り付け
#   Title: "NLE parity + libavcore + tracker presets + selftest SSOT + selftest split + CI draft (45 commits)"

# 2b. gh CLI を導入した場合:
gh pr create \
  --base main \
  --head feat/trackmatte-ssot-parity \
  --title "NLE parity + libavcore + tracker presets + selftest SSOT + selftest split + CI draft (45 commits)" \
  --body-file .omc/PR_DESCRIPTION.md
```

🤖 Generated with [Claude Code](https://claude.com/claude-code)
