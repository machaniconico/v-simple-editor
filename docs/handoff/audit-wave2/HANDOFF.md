# 引き継ぎ: 品質監査 第2波 (audit-wave2)

作成: 2026-07-12 / 前セッション: fullralph + ultracode による監査パイプライン

## 現在地

| フェーズ | 状態 |
|---|---|
| 第1波 (US-001〜126, 25ストーリー/80バグ) | ✅ 完了。PR #4 で main (`8669994`) へ squash マージ済み |
| 第2波 発見スイープ (12 finder → 反証2票検証) | ✅ 完了。**confirmed 58件** (high 12 / medium 46)、反証棄却 2、判定割れ 0 |
| 第2波 PRD 構築 (story pack → US-2xx) | ⬜ **ここから再開** |
| 第2波 実装ループ (codex 実装 → 独立レビュー → ビルド) | ⬜ 未着手 |

## このディレクトリの成果物

- `verify_results.json` — **正典**。confirmed 58件 (file/line/summary/failure_scenario/severity/lens + 反証2票 v1/v2 の結論)
- `known_findings.json` — 第1波の既知99候補 (次回スイープの除外リスト)
- `finder_groups.json` — スイープ時の 10 エリア分割 (参考)
- `story-pack-format-sample_US-105.json` — story pack の形式サンプル (第1波 US-105)
- `reference-impl-wave-workflow.js` — 実装波 Workflow スクリプトの実績版 (第1波 B1 波で使用: codex 実装 3並列チャンク → opus 独立レビュー → チャンク毎ビルド)

## 再開手順 (fullralph WORKFLOW MODE)

1. **準備**: このブランチ `fix/audit-wave2` 上で作業する (main 直 push 禁止)。成果物を `.omc/state/audit2/` へコピー: `mkdir -p .omc/state/audit2 && cp docs/handoff/audit-wave2/*.json .omc/state/audit2/`
2. **story pack 生成**: `verify_results.json` の confirmed 58件をファイル/サブシステム単位でグループ化し、`.omc/state/audit/story_packs/US-2xx.json` を生成 (形式はサンプル準拠: 各要素に file/line/summary/failure_scenario と v1/v2)。
   - **注意**: MainWindow.cpp に 10件集中 → 2〜3 ストーリーに分割。他ストーリーと touchedFiles を交差させない (交差するとファイルロックで直列化する)
   - 目安: 12〜15 ストーリー、1ストーリー 3〜6 バグ
3. **PRD**: `.omc/fullralph-prd.json` に US-201〜 を追加 (第1波の25ストーリーは passed のまま残してよい)。description には「story pack を必ず最初に読む」「touchedFiles 外変更禁止」「selftest を緩めない」「ビルド検証: cmd.exe /c "cmake --build build --config Release --parallel"」を第1波と同様に記載
4. **arm-persistence**: `node ~/.claude/skills/fullralph/scripts/dispatcher.mjs arm-persistence --session="$CLAUDE_CODE_SESSION_ID" --cwd=<repo> --max-iter=200 --prompt="fullralph: 監査第2波実装"`
5. **実装波**: `reference-impl-wave-workflow.js` の CHUNKS を差し替えて Workflow 起動 (3ストーリー並列 × チャンク数)。フロー: claim → codex-worker --mode=implement → mark-implemented → release → opus code-reviewer 独立レビュー → チャンク毎ビルド。**verdict の mark-passed/failed は HUB (メインセッション) が adjudicate してから**
6. **チャンク毎に**: HUB が diff を自分でも確認 → mark-passed → ストーリー単位 commit → push
7. **Phase 6**: 全 PASS 後、Release ビルド + runtime selftest 39 スイート → ship.sh で PR 作成 (main の CI test ゲートは未設置なので auto-merge は fail-closed で見送りになる。マージは手動)

## 環境前提 (新 PC で要確認)

- `codex` CLI ログイン済み (第1波実績: 1ストーリー 3〜7分で実装)
- `~/.claude/skills/fullralph/` スキル一式 + oh-my-claudecode プラグイン
- Windows ビルド: リポジトリを `D:\workspace\v-simple-editor` 相当に配置し `build/` を cmake 構成済みにする (パスが違う場合はプロンプト内の `D:\workspace\v-simple-editor` を読み替え)
- **runtime selftest の実行レシピ** (WSL から。qwindows.dll が exe 隣に無いため plugin path 必須):
  ```bash
  env "VEDITOR_VFX_SELFTEST=1" \
    QT_QPA_PLATFORM_PLUGIN_PATH='C:\vcpkg\installed\x64-windows\Qt6\plugins\platforms' \
    WSLENV="VEDITOR_VFX_SELFTEST:QT_QPA_PLATFORM_PLUGIN_PATH" \
    cmd.exe /c 'D:\workspace\v-simple-editor\build\Release\v-simple-editor.exe'
  # exit 0 + 末尾 "XXX selftest OK" が PASS。全39スイート名は main.cpp の VEDITOR_*_SELFTEST を grep
  ```
  ※ `cmd.exe /c "set VAR=1&& \"path\""` 形式はクォート崩れで全滅するので使わない

## 第2波 confirmed の要点 (詳細は verify_results.json)

- **high 12件**: エクスポート/トラッキング完了シグナルの接続蓄積 UAF (MainWindow ×2)、closeEvent が未保存確認なしで終了+リカバリ抑止、悪意 PSD の null 書込みクラッシュ (AffinityPsd ×2)、StringKeyframeTrack シリアライズ欠落 (ProjectFile)、1D LUT を 3D として読む OOB (GLPreview)、Timeline setClip* 系の共通欠陥、EffectPreset の type 名往復不一致、SpeedRamp ワーカー UAF、AIHighlight の int キャスト、NetworkRender ほか
- **medium 46件**: SRT インポート/Whisper 字幕適用が no-op (videoClips() デタッチコピー問題 ×2)、BatchExportQueue がタイマー芝居で実処理ゼロ、GUI スレッド同期ブロック群 (SmartEdit/MultiCam/Caption/Remotion npm/ScreenRecorder/ProxyManager/MotionStabilizer)、Remotion 系 4件 (leadInSec 無視/パス無エスケープ/同名アセット統合/npm フリーズ)、ダッキング threshold 誤用、SceneCut 座標系ずれ、AutoSave 書込み戻り値無視 ほか
