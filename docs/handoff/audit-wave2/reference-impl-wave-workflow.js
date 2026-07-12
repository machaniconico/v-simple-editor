export const meta = {
  name: 'fullralph-impl-wave-b1',
  description: 'fullralph Phase 2-3: US-107/112/113/114/115/116 を codex 実装 → 独立レビュー (3並列チャンク×2)',
  phases: [
    { title: 'Implement', detail: 'codex-worker で実装 (3並列)' },
    { title: 'Review', detail: '実装完了次第、独立 Claude レビュー' },
    { title: 'Build', detail: 'チャンク毎に Windows Release ビルド検証' },
  ],
}

const REPO = '/mnt/d/workspace/v-simple-editor'
const FR = '/home/machaniconico/.claude/skills/fullralph/scripts'
const SESSION = 'b38c3e13-c8d0-4988-837a-09dcd47d76e5'

const CHUNKS = [
  [
    { id: 'US-107', title: 'MainWindow の欠陥群を修正', files: ['src/MainWindow.cpp'], bugCount: 3 },
    { id: 'US-112', title: 'AudioEQ/AudioMixer の欠陥群を修正', files: ['src/AudioEQ.cpp', 'src/AudioMixer.cpp'], bugCount: 4 },
    { id: 'US-113', title: 'MotionTracker のスレッド競合・デコード欠陥群を修正', files: ['src/MotionTracker.cpp'], bugCount: 3 },
  ],
  [
    { id: 'US-114', title: 'メディア処理系の単発 high 欠陥群を修正 (ExtrudedMesh/ChromaKey/Particle/SpeedRamp/Stabilizer)', files: ['src/ExtrudedMesh.cpp', 'src/ChromaKeyRefine.cpp', 'src/ParticleSystem.cpp', 'src/SpeedRamp.cpp', 'src/VideoStabilizer.cpp'], bugCount: 5 },
    { id: 'US-115', title: 'インポート/エクスポート系の単発 high 欠陥群を修正 (SceneCut/Fcpxml/Collector/FrameIo)', files: ['src/SceneCutScanner.cpp', 'src/FcpxmlExporter.cpp', 'src/ProjectCollector.cpp', 'src/FrameIoImporter.cpp'], bugCount: 4 },
    { id: 'US-116', title: '外部連携系の単発 high 欠陥群を修正 (Python/Speech/EffectDialogs/YouTube)', files: ['src/PythonScript.cpp', 'src/SpeechRecognizer.cpp', 'src/VideoEffectDialogs.cpp', 'src/YoutubeUploadManager.cpp'], bugCount: 4 },
  ],
]

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    story: { type: 'string' },
    implemented: { type: 'boolean' },
    exitCode: { type: 'number' },
    summary: { type: 'string', description: 'codex が何を修正したかの要約 (日本語)' },
  },
  required: ['story', 'implemented', 'summary'],
}

const VERDICT_SCHEMA = {
  type: 'object',
  properties: {
    story: { type: 'string' },
    passes: { type: 'boolean' },
    reason: { type: 'string', description: '判定理由の要約 (日本語)' },
    defects: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          summary: { type: 'string' },
          file: { type: 'string' },
          line: { type: 'number' },
          genuine: { type: 'boolean' },
        },
        required: ['summary', 'genuine'],
      },
    },
  },
  required: ['story', 'passes', 'reason'],
}

const BUILD_SCHEMA = {
  type: 'object',
  properties: {
    ok: { type: 'boolean' },
    errorCount: { type: 'number' },
    errorSummary: { type: 'string' },
  },
  required: ['ok', 'errorSummary'],
}

function implPrompt(s) {
  return `あなたは fullralph の implement ランナーです。ストーリー ${s.id} (${s.title}) の codex 実装を以下の手順で実行してください。全コマンドは Bash で、リポジトリは ${REPO}。

1. \`node ${FR}/dispatcher.mjs claim-story --story=${s.id} --worker=wf-${s.id} --cwd=${REPO}\` を実行。出力 JSON の ok が false ならロック競合なので {story:"${s.id}", implemented:false, summary:"claim failed"} を返して終了。
2. \`node ${FR}/dispatcher.mjs touch-persistence --session=${SESSION} --cwd=${REPO}\` を実行 (heartbeat)。
3. \`node ${FR}/codex-worker.mjs --story=${s.id} --mode=implement --cwd=${REPO}\` を timeout 600000 で実行 (5〜8分かかる。正常)。
4. exit 0 なら \`node ${FR}/dispatcher.mjs mark-implemented --story=${s.id} --result=.omc/state/fullralph-results/${s.id}.json --cwd=${REPO}\`。exit 非0 (timeout 含む) なら \`node ${FR}/dispatcher.mjs mark-failed --story=${s.id} --feedback="codex exit <code>: <エラー要約>" --cwd=${REPO}\`。
5. どちらの場合も必ず \`node ${FR}/dispatcher.mjs release-story --story=${s.id} --cwd=${REPO}\` を実行。
6. \`git -C ${REPO} status --porcelain\` で untracked の新規ファイルがあれば \`git -C ${REPO} add -N <file>\` する (レビュアーの diff に載せるため)。
7. \`.omc/state/fullralph-results/${s.id}.codex-last.txt\` (${REPO} 配下) を読み、codex が何を修正したかを summary に要約して返す。

story フィールドは "${s.id}"、implemented は手順4で mark-implemented した場合のみ true。`
}

function reviewPrompt(s) {
  return `あなたは fullralph パイプラインの独立レビュアー (approval lane) です。ストーリー ${s.id} の実装を厳密にレビューし、判定を構造化して返してください。

リポジトリ: ${REPO} (branch: fix/audit-wave1)
ストーリー: ${s.id} — ${s.title}
touchedFiles: ${s.files.join(', ')}

手順:
1. Bash で \`node ${FR}/reviewer-router.mjs --story=${s.id} --mode=claude --cwd=${REPO}\` を実行し、出力されたレビュープロンプト (ストーリー詳細 + diff + worker出力末尾) を読む。
2. \`.omc/state/audit/story_packs/${s.id}.json\` を読む。これが修正対象バグの正典 (各件 file/line/summary/failure_scenario)。
3. \`git -C ${REPO} diff -- ${s.files.join(' ')}\` でこのストーリー自身の diff を精査し、story pack の全 ${s.bugCount} 件それぞれについて「failure_scenario がコード上成立しなくなったか」を該当コードと呼び出し元を読んで検証する。
4. 修正が新たなバグ (UAF/リーク/ロジック反転/後方互換破壊) を持ち込んでいないかも確認する。

判定規律 (重要):
- 作業ツリーには他ストーリーの未コミット変更が同居している。touchedFiles 外の変更を理由に FAIL しないこと。
- あなたのサンドボックスでは MSVC/Qt ビルドは実行できない。「ビルド未検証」を理由に FAIL しないこと (ビルドは別レーンで実行済み)。
- FAIL にするのは「story pack のバグ N が依然として成立する」または「修正が具体的な新規ロジック欠陥を持ち込んだ」と、コードを読んで具体的に指摘できる場合のみ。その場合 defects に file/line/summary を必ず入れ genuine:true とする。
- 全件修正済みで新規欠陥なしなら passes:true。

story フィールドには "${s.id}" を入れて返すこと。`
}

const out = []
for (let ci = 0; ci < CHUNKS.length; ci++) {
  const chunk = CHUNKS[ci]
  log(`チャンク ${ci + 1}/${CHUNKS.length}: ${chunk.map(s => s.id).join(', ')} の実装開始`)

  const results = await pipeline(
    chunk,
    s => agent(implPrompt(s), { label: `impl:${s.id}`, phase: 'Implement', schema: IMPL_SCHEMA, effort: 'low' }),
    (impl, s) => {
      if (!impl || !impl.implemented) {
        return { story: s.id, passes: false, reason: 'implement 失敗: ' + (impl ? impl.summary : 'runner agent died'), implementFailed: true }
      }
      return agent(reviewPrompt(s), { label: `review:${s.id}`, phase: 'Review', schema: VERDICT_SCHEMA, agentType: 'oh-my-claudecode:code-reviewer' })
        .then(v => v ? Object.assign({ implSummary: impl.summary }, v) : { story: s.id, passes: false, reason: 'review agent died', reviewDied: true })
    }
  )

  const build = await agent(`Bash で Windows Release ビルドを実行し結果を返してください。
コマンド: \`cmd.exe /c "cd /d D:\\\\workspace\\\\v-simple-editor && cmake --build build --config Release --parallel"\` (timeout 600000)
出力から "error C" / "LNK" / "fatal error" を集計。エラー0件なら ok:true。エラーがあれば errorSummary に代表的なエラー行 (ファイル名・エラーコード) を入れる。`, { label: `build:chunk${ci + 1}`, phase: 'Build', schema: BUILD_SCHEMA, effort: 'low' })

  out.push({ chunk: ci + 1, stories: chunk.map(s => s.id), results: results.filter(Boolean), build })
  log(`チャンク ${ci + 1} 完了: build=${build && build.ok ? 'GREEN' : 'RED/不明'}`)
}

return out
