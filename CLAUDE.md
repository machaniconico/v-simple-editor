## 進行中の作業 (2026-07-12 時点 — 完了したらこのセクションを削除すること)

品質監査 **第2波** が発見フェーズ完了・実装未着手の状態で引き継ぎ中。

- 正典: `docs/handoff/audit-wave2/HANDOFF.md`（confirmed 58件 = `verify_results.json`、再開手順・環境前提・selftest レシピ入り）。作業ブランチ: `fix/audit-wave2`
- 「監査の続き」「第2波」等と言われたら **必ず先に HANDOFF.md を読む**。再開点は story pack 生成 → US-2xx PRD 構築 → fullralph 実装ループ
- 第1波（25ストーリー/80バグ）は PR #4 で main へマージ済み。main に CI test ゲート未設置のため PR の auto-merge は不可（手動マージ）

## graphify

This project has a graphify knowledge graph at graphify-out/.

Rules:
- Before answering architecture or codebase questions, read graphify-out/GRAPH_REPORT.md for god nodes and community structure
- If graphify-out/wiki/index.md exists, navigate it instead of reading raw files
- For cross-module "how does X relate to Y" questions, prefer `graphify query "<question>"`, `graphify path "<A>" "<B>"`, or `graphify explain "<concept>"` over grep — these traverse the graph's EXTRACTED + INFERRED edges instead of scanning files
- After modifying code files in this session, run `graphify update .` to keep the graph current (AST-only, no API cost)
