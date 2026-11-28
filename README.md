# V Simple Editor

**Simple UI, Full-Featured Video Editor**

A professional video editing application built from scratch with C++17, Qt6, and FFmpeg. Designed with a clean, intuitive interface while packing advanced features like After Effects-style compositing, AI-powered editing tools, and GPU-accelerated preview.

---

## Features (90+ functions across 13 phases)

### Core Editing
- Video import & decoding (MP4, MKV, MOV, WebM, FLV)
- Multi-track timeline with drag & drop
- Cut, split, trim at playhead
- Copy / paste / ripple delete / duplicate (Ctrl+D) / lift
- Select all / select forward・backward from playhead / blade all tracks at playhead (Ctrl+Shift+K)
- Frame nudge (Alt+←/→ = 1 frame, Alt+Shift+←/→ = 10 frames) / close all gaps (only gaps common to every unlocked track)
- Track name & color (track header context menu, MCP `set_track_property`)
- Track remove / reorder with full undo, including track-matte, parenting, adjustment-layer, mixer and bus references (track header context menu, Track menu, MCP `add_track` / `remove_track` / `move_track`)
- Marquee (rubber-band) clip selection, zoom to sequence / selection, go to timecode (`Ctrl+Shift+J`)
- Move clip head / tail to playhead, select clips by label, linked-selection toggle
- Sequence list dock (switch / create / rename, MCP `get_sequences` / `set_active_sequence`)
- Clip filmstrip thumbnails on the timeline (off by default) and media-pool thumbnails with hover skimming
- Media pool ratings (favorite / rejected / 1-5 stars), filters (all / favorites / exclude rejected / unused), rename, show in Explorer
- Keyword collections (auto-generated virtual bins from clip tags, media-pool asset context menu "Edit Keywords…") and SmartBin creation/editing with name/tag/name-query/type conditions (media-pool bin-tree context menu "Create Smart Bin…")
- Clip enable/disable toggle (excludes a clip from preview and export, MCP `set_clip_enabled`)
- Clip grouping across tracks (linked select / move / trim / delete, MCP `group_clips` / `ungroup_clips`)
- Linked clips (AV pairs / clip groups) now trim together on edge drag; Alt+drag trims only the grabbed clip (behaviour change in Wave 8; trim-menu operations such as ripple / roll / slip / slide keep applying to linked partners on other tracks, and are rolled back if a partner fails)
- Align (6 directions) and distribute (horizontal / vertical) selected clips (Composition menu)
- Keyframe copy & paste across parameters and clips (Graph Editor, playhead-relative shift)
- Timeline hover skimming (off by default)
- New sequence from a selected clip (media-pool asset context menu "New Sequence from This Clip"; video (+audio) clips are placed at V1 (+A1) head, audio-only clips at A1 head)
- Unused-media removal (media-pool bin-tree context menu "Remove Unused Media…") now also checks LUT / image-overlay / particle-config references, not only the existing provider list; the existing "Unused" filter shares the same check (defect fix: previously narrower)
- CMX3600 EDL / FCPXML import into a new sequence (MCP `import_edl` / `import_fcpxml`)
  - Wave 8: FCPXML connected clips (anchored child asset-clips) are placed into lanes (lane 0 first, then by |lane|, up to 2 video and 2 audio tracks); audio-only assets go to audio tracks
  - Wave 9: FCPXML connected clips inside a `<gap>` are imported (anchored on the gap), J-cut connected clips that start before their parent are imported (the part before 0 s is trimmed with the warning 「接続クリップをタイムライン 0 で切り詰めました: <name> / <ref>」; a clip that lies entirely before 0 s is not imported and is skipped with 「クリップの値が不正です (接続位置): <name> / <ref>」), each descendant of a parent whose time range is invalid (duration 0, or an invalid offset / start) is skipped with its own warning, and a spine with no asset-clips yields an empty sequence plus a warning instead of an error. FCPXML clips keep the source length only when the `<asset>` declares a `duration` and its `start` is 0 (an asset with a non-zero `start` may allow the right edge to extend `start` seconds less than the source end; to be confirmed in Wave 10) (otherwise the used range is the limit, as before), and EDL clips only when the media file is found and the probed length is positive; the right edge of an imported EDL clip can be extended only when the probed length is longer than its used range (otherwise the previous value is kept); for an EDL dissolve of positive length, the outgoing clip can always be extended by the dissolve handle (dissolve length x speed) regardless of the media; growth beyond the handle up to the source length is possible only when the file is found and the probed length is positive and longer than the handle-inclusive length.
  - Wave 13: FCPXML import has no track limit (every lane gets its own track) and reads spine transitions (edge cross-dissolves / fades), compound clips (`ref-clip` as nested sequences), multicam clips (`mc-clip` flattened to the active angle) and `clip` / `sync-clip` containers; connected clips become primary-storyline connections; FCPXML export writes upper video tracks as lanes and connections as connected clips (the transition timing and the `mc-clip` attributes are unconfirmed against a real Final Cut Pro file, see 「FCPXML 読み込みの対応表」)
- Undo / redo (unlimited history)
- Wave 11: a cross-track clip drop, including the linked partner and a track created for it, is one undo step ("クリップをトラック間移動"); roto, parent links and track mattes follow clips through cross-track drag, link drops and range ripple (transcript edit)
- Wave 12: dropping a clip onto the new-track band (a permanent 10 px row after the last video track / the last audio track) creates the track and moves the clip there as one undo step ("クリップを新しいトラックへ移動"; the linked partner follows onto the other kind's track with the same number, created if it does not exist); a clip of the other kind and media-bin drops are ignored (US-1117, `roto` selftest, 44 gates)
- Wave 12: every clip carries a stable `uid` (minted at insert, load, paste and split; a colliding uid is re-minted, also after decompose) that project diffs ignore, and roto, parent links and track mattes are keyed on it first, so pasted clones keep their own entries and a reorder across a preceding clip no longer drops them (US-1118 / US-1119, `clip-uid` selftest 5 gates, `matte-ssot` 13 gates)
- Wave 12 (undo): matte settings and the bus panel's bus assignment / gain / mute / solo / name edits push no undo point of their own; they are absorbed into the current undo point (an absorbed edit discards redo), so undoing the next edit no longer loses a matte; assigning a bus from the mixer strip combo is its own undo point ("トラックのバスを変更", 1 Ctrl+Z) (US-1107)
- Wave 13: a clip can be connected to a V1 clip and follows it (Edit menu / context menu 「主ストーリーラインに接続」 / 「接続を解除」, MCP `connect_clip` / `disconnect_clip`); the opt-in 「主ストーリーライン (マグネティック)」 mode (`timeline.storyline_mode`, default OFF) ripples V1 only and keeps it gap-free
- Snap to grid & playhead
- Timeline zoom
- J/K/L shuttle playback
- In/Out mark points
- Clip speed adjustment

### Text & Overlays
- Multiple text overlays with rich text editing
- 12 animation types (fade, slide, typewriter, bounce, etc.)
- Drop shadow & double outline
- Drag move / resize / rotate on canvas
- SRT/VTT subtitle import
- Ruby / furigana support
- Template save & load
- AI-powered subtitle generation (Whisper)
- Caption editor split / merge: split a caption at a chosen time ("分割" with "分割位置 (ms)") and merge two adjacent captions of the same speaker ("結合"), backed by the pure helpers `caption::splitClip` / `caption::mergeClips` and the `caption-edit` selftest (6 gates)

### Transitions & Effects
- 45 transition types (crossfade, wipe, slide, zoom, light leak, morph cut, etc.), rendered identically in preview and export
- Image overlay & Picture-in-Picture
- 9 video effects (blur, sharpen, sepia, negative, etc.)
- Color correction (10 parameters: brightness, contrast, saturation, hue, temperature, tint, gamma, highlights, shadows, exposure)
- LUT import (.cube format + 4 built-in LUTs), applied from the Effects menu to the selected clip and honoured by export (defect fix: menu LUT used to affect preview only)
- Log-range color wheels (Shadow / Midtone / Highlight) and still store with wipe compare
- Color warper (12 x 3 hue / saturation mesh, CPU and GPU parity)
- Camera Log → Rec.709 (S-Log3 / LogC3 / V-Log / Cineon, exposure offset, gamma 2.4 / linear / Rec.709 OETF output)
- Luma key / color key (CPU-only, alpha survives through export; place last in the stack)
- Broadcast safe (Rec.709 legal / full-range limiter) and Leave Color (keep one hue, desaturate the rest), both CPU-only so preview matches export
- Offset color wheel (printer-light style, CPU/GPU parity, MCP `set_color_correction` `offsetR`/`offsetG`/`offsetB`)
- Per-effect mask (Bezier path with 3+ points and feather; brush strokes are not supported and are discarded)
- Flip / Flop, solid color layer insertion, and 5 parametric warps (wave / ripple / spherize / fisheye / pinch)
- Effect stacking with keyframe animation (5 interpolation modes)
- Plugin system (5 built-in: Glow, Emboss, Posterize, Edge Detect, Color Shift)
- Effect preset library (8 built-in + custom save)
- GPU shader effects (17 effects: chromatic aberration, halftone, duotone, gaussian blur, radial blur, tilt shift, barrel distortion, water ripple, pixelate, glitch, film grain, vignette, CRT retro, sketch, oil paint, gradient map, directional blur)

### Audio
- Per-clip volume control
- BGM / audio file import
- Mute / solo per track
- Audio waveform display
- 5-band equalizer with presets
- 7 audio effects
- Audio noise reduction (afftdn)
- VST3 / AudioUnit plugin support
- Explicit audio transitions (constant-power crossfade / fade in / fade out) with true clip overlap (Center / Start / End at cut)
- Dialogue leveler (automatic speech loudness leveling to a target LUFS)
- Music remix (beat-aware BGM re-timing to a target duration)
- Per-track 4-band EQ / compressor / reverb / noise reduction as a shared DSP module, applied to export as well as playback (defect fix: projects using track FX now export the processed sound)
- Master bus EQ (post-mix, off by default) and correct panel-to-engine track mapping (defect fix: A1's FX used to land on another track)
- Track FX and master EQ are saved in the project and cleared when switching projects (defect fix: settings used to leak between projects)
- De-esser (dynamic sibilance suppression) added to the track FX chain, off by default
- Channel-strip audio mixer dock (faders + meters, hidden by default; defect fix: fader/normalize track gain now reaches export even without track FX)
- Master-bus spectrum analyzer dock ("スペクトラムアナライザ", View menu, hidden by default): 2048-point FFT of the 48 kHz master mix, display only; the source combo picks the master mix ("マスター") or one track after its FX and before its fader (US-1116); the PCM tap is written by one epilogue in `readData` (decoder stalls and normal blocks merge before it, nothing is written while paused) and copies nothing while no dock is subscribed; with no signal the display decays to the floor at 12 dB/s (`spectrum` selftest, 8 gates)
- Legacy 3-band EQ migration (Audio > Track EQ > A<n> > "4 バンド EQ へ移行"): a representable legacy EQ moves into the 4-band track EQ as one undo step ("EQ を 4 バンドへ移行"); explicit action only, projects are never converted on load, and the result is approximate (PCM differs by at most 2 LSB in the fixture)
- De-esser listen follows its own track through track delete / add / move and their undo / redo (Wave 11)
- Wave 12: per-clip 4-band EQ (right-click an audio clip > "クリップ EQ…" opens the audio clip editor's EQ panel; one widget operation = one undo step, "クリップ EQ を変更"; off by default, nested clips follow child-first-else-parent), applied in playback (float one-pass per entry) and in the export mix (US-1110 / US-1111 / US-1126, `clip-eq` selftest 6 gates, `audio-fixture` variant h)
- Wave 12: per-track pan and fader gain are saved in the project as `audioMixer.trackMix` (written only when some track has a gain other than 1.0 or a pan other than 0; the array then lists every track) and applied to playback and export; the mixer strips gain a pan slider and a bus-assignment combo ("マスター" + bus names; undo "トラックのパンを変更" / "トラックのバスを変更"); a clip pan other than 0 without track FX now exports through the DSP path so the pan is applied (export exception; a project saved with a fader gain other than 1.0 now keeps that gain after reload, where it used to return to 1) (US-1114 / US-1115, `mixer-dock` selftest 13 gates, `audio-fixture` variants i / j)
- Wave 12: AUX sends - the audio bus panel's "センド" table sets a level (dB, -60 dB or lower removes it) and pre/post-fader per track and bus (undo "AUX センドを変更"); playback mixes the sends in, and the export mix now applies the bus assignment, bus gain / mute / solo and the sends (export exception; the send is a parallel add without the bus FX; track solo was not applied to export until Wave 13 (US-1209 applies it; track mute already was)) (US-1122 / US-1123, `aux-send` selftest 3 gates, `audio-fixture` variants k / l / m)
- Wave 12 (export audio): an export whose V1 is empty but which has video on an upper track gets an audio track on every entry point (normal / mobile / MCP `export_video` / batch with auto loudness ON or OFF): silent when there is no audio clip, mixed when there is; the one exception is a batch task with auto loudness ON and no audio clip, which is still rejected as silent (the existing limit below; only batch OFF writes the silent track) (export exception; projects with V1 clips and projects without any video are unchanged) (US-1109, `mcp` G188-emptyV1 re-pinned)
- Wave 12 (auto loudness): the audio mix and the loudness measurement run off the GUI thread for normal / mobile exports (progress dialog "音声を準備しています…" with cancel) and for MCP `export_video` with `autoLoudness=true` (`get_export_status` answers while preparing, G189); the mix is split into a GUI-side plan and a timeline-free executor in `AudioExportMix` (US-1104 / US-1105 / US-1127, `loudness-auto` selftest 15 gates). Batch export stays synchronous
- Manual loudness-normalization gain and normalizer amount are now saved in the project (defect fix: previously reset on reopen)
- Automatic loudness normalization on export (off by default; -14 LUFS streaming / -23 LUFS broadcast presets or custom target; also applied in the batch export queue through saved user presets (Wave 8). Applied to MCP `export_video` since Wave 10 through the optional `autoLoudness` / `targetLufs` arguments; when they are omitted the specified user preset's setting is followed, so an existing MCP call that uses an auto-loudness-ON user preset via `preset=` now produces normalized audio without adding any argument (Wave 10 behavior change; pass `autoLoudness=false` explicitly to keep the old output). Not applied to RenderInPlace or manual additions in the render-queue dialog). The saved manual gain above is now actually applied at export time (defect fix: a Windows file-handle issue in the post-process step used to prevent the replacement from taking effect). Wave 11 (behavior change, GUI normal / mobile export only): with auto loudness ON an empty audio sequence now exports silence instead of the V1 source audio, and a failed PCM mix (including a failed silent mix) rejects the job instead of exporting unnormalized
- Wave 9: the batch export queue now exports the timeline audio mix (multi-track, track FX, gain, fades) for tasks of the current project when auto loudness is off and the timeline has audible audio (the source-file audio is kept when the timeline length is 0 or it has no clip with audio; if every track is muted the mix is silent); the export dialog's audio bitrate is honored on the AAC re-encode path; image-sequence export disables the auto-loudness checkbox; the EQ / compressor / reverb / noise-reduction docks follow project load and track add / remove like the de-esser dock

### AI & Automation
- Silence detection
- Auto jump cut
- Scene change detection
- Motion tracking (NCC template matching)
- Video stabilization (vidstab 2-pass)
- AI auto-highlight reel generation
- Whisper subtitle generation (SRT/VTT/overlay)

### Compositing (After Effects-style)
- Layer compositor with 13 blend modes
- Transform animations (7 presets)
- Mask & track mattes (the matte data is owned by `Timeline` like roto; since Wave 11 mattes follow cross-track drag, link drops and range ripple (transcript edit), while setting a matte is still not an undo step)
- Particle system (7 presets)
- 3D camera (dolly, pan, orbit, zoom) with optional true perspective projection (look-at, roll, layer rotation)
- 3D camera solve from planar tracks (homography decomposition, no external solver)
- Two-view relative pose (normalized 8-point + RANSAC + essential decomposition) and linear triangulation (numeric core)
- Multi-frame feature tracking (Planar Tracker dialog's "Feature Tracking (3D)" section): KLT tracking → two-view pose chaining → bundle adjustment solves camera position/look-at keyframes directly from the UI (Levenberg-Marquardt, Schur-complement solver by default, up to 60 frames x 800 points). Frame range is specified in 0-based source frames, and the default start/end auto-fill from the clip's trimmed in/out (clamped to the 60-frame window); applied keyframes correctly convert to timeline time honoring the clip's trim/speed/reverse; not applicable to speed-ramped or time-remapped clips
- Mesh warp per clip with on-preview grid handles (Composition menu, off by default, CPU path so preview matches export)
- Puppet pin: interactive mesh deformation via draggable pins on the preview (Composition menu, off by default, CPU path so preview matches export)
- Expression engine (wiggle, noise, ease, `audioLevel()` from linked audio); since Wave 16 wiggle and expressions are stored in the clip and applied in exports
- Convert audio to keyframes (RMS envelope → opacity / scale / rotation / position)
- Shape layers (8 primitives) as timeline clips, with Repeater / Trim Paths / Round Corners / ZigZag / Wiggle Path / Pucker & Bloat / Twist / Offset Path modifiers (Wiggle Path varies over clip time when its wiggles-per-second setting is above 0)
- Auto-orient along motion path; orient towards the project camera for 3D layers (context menu "カメラへ自動的に向ける", off by default, true-perspective camera only)
- Text animator (15 types)
- Tracker link (motion track to effect)
- Parent layers (a child follows its parent's transform): since Wave 11 the preview adopts a parent with the same `parentLayerAt` rule as export, so a disabled parent or the incoming clip of a transition overlap is no longer followed by the preview
- Pre-compose / nesting
- Rotoscope (path interpolation; since Wave 10 roto is reflected in every export path and in RenderInPlace, follows clip edits and is part of undo; Wave 11: roto alpha survives the grade stage and non-geometric FX in the CPU preview and export, and RenderInPlace of a reversed clip with time remap / speed ramp, or with handles that extend past the source range, matches the timeline frame (in-source handles: Wave 12 below)); Wave 12: RenderInPlace keeps the original roto entry as `renderInPlaceOriginalRoto` so "元のクリップに戻す" (decompose) restores it (US-1102, `roto` selftest, 44 gates), a reversed bake with in-bounds handles or at a bake fps above the stream fps now aligns the tail frame and roto index with the timeline and holds the last frame past the media end (US-1103, export exception for those bakes only), and the GPU preview (GLPreview) shows Echo / rolling-shutter special composites through one baked-mode entry so roto alpha and the grade are not applied twice (preview only, US-1106, `special-composite` selftest 7 gates)
- Warp & distortion (9 effects)

### Export & Workflow
- 13 export presets (YouTube, Twitter, Instagram, TikTok, 4K, ProRes, etc.)
- Hardware encoding (NVENC / QSV / AMF auto-detect)
- Codec auto-detection
- Project save/load (.veditor JSON format)
- Auto-save & crash recovery
- Proxy editing (low-res preview, full-res export)
- Render queue (batch export); cancel the selected job (a pending job is removed from the list) or stop the whole queue
- Export-complete notification ("環境設定" > "書き出し完了を通知する", off by default): when the render queue finishes, the status bar shows the job and failure counts, the window flashes and a beep sounds; the batch export queue and RenderInPlace are not covered (`render-notify` selftest, 7 gates)
- Wave 12 (render queue): export progress is filtered by job uuid and cancelling in the progress dialog cancels only that job (the whole shared queue used to stop), each job snapshots the ACES pipeline when it is added (US-1108); the render-queue dialog now shares the main queue instead of owning one, so its jobs count in the export-complete notification and carry an explicit ACES pipeline and `loudnessGainDb` (export exception: only a dialog export with project ACES enabled changes) (US-1125, `render-notify` selftest 7 gates)
- Rate control: bitrate or CRF quality mode (H.264 / H.265 / VP9 0..51, AV1 0..63 with software AV1 fallback)
- Audio-only export (m4a / wav / mp3, PCM intermediate mix)
- User export presets (save / delete current settings, resolved by name from MCP `export_video`; since Wave 10 a preset with auto loudness ON also normalizes the audio of `export_video` when `autoLoudness` is omitted, behavior change)
- Alpha export on ProRes 4444 / 4444 XQ or VP9 (yuva420p) (`keepAlpha`, off by default; transparent areas stay transparent in the output)
- Image-sequence export (PNG / TIFF, GUI export dialog only); Wave 13: EXR (display-referred or scene-linear) when the bundled avcodec has the exr encoder
- Export metadata embedding (title / author / copyright / comment, MCP `export_video` `metadata`)
- Subtitle export mode: none / burn-in / mov_text embedded stream / both (MP4 / MOV only, GUI export dialog only)
- Screen recorder (cross-platform)
- Speed ramp (variable speed with easing)
- Timeline markers & YouTube chapter export
- Multi-camera editing (audio-correlation sync + per-angle offset trim + camera switch)
- Offline media detection & batch relink (project collect follows the same path list); the File menu "メディアを再リンク…" opens the relink dialog at any time, and offline video / audio clips on the timeline carry a badge (clips 40 px wide or narrower show only the (オフライン) suffix on the name) (overlay images, particles and nested contents count in the relink list but get no badge)
- ImportHub-generated media is stored in <project name>_assets/importhub/ next to the project (moved there on the first save)
- 4 themes (Dark / Light / Midnight / Ocean)

### Scripting & Integration
- Python scripting engine (embedded or process mode)
- Script console with syntax highlighting
- Remotion export (React video framework)
- Network rendering (distributed, TCP-based)
- Free resource guide (7 categories, 40+ sites)

---

## MCP 連携 (LLM からタイムラインを操作する)

V Simple Editor には、LLM からローカルのエディタを操作するための MCP サーバが内蔵されています。会話で「このクリップを分割して、不要な方を削除して」のように指示すると、Claude Code や Codex CLI が MCP ツールを呼び出し、エディタのタイムラインが編集されます。

### なぜサブスク枠で動くのか

API キー方式でモデルを呼び出すと、通常は API の従量課金になります。一方、Claude Code などの CLI はログイン済みのサブスクリプション枠で利用できます。そこで、エディタ自身が localhost の MCP サーバになり、考えて指示を出す側をログイン済みの Claude Code / Codex CLI が担う構造にしています。エディタが API キーを使ってモデルを直接呼び出す構成ではありません。

### MCP サーバの起動

1. エディタで **ツール > MCP サーバ > MCP サーバを有効にする** を選びます。
2. サーバは `127.0.0.1` のみにバインドし、既定ポート `8765`（使用中なら `+1` し、最大 20 回試行）で起動します。
3. 認証は Bearer トークンです。`Authorization: Bearer <token>` ヘッダー、または URL の `?token=<token>` を使います。
4. MCP は protocolVersion `2025-06-18` の Streamable HTTP で、POST 1 回に対して JSON レスポンスを返します。

トークンは QSettings の `VSimpleEditor/Preferences/mcpToken` に保存され、次回起動後も同じ値が使われます。`VEDITOR_MCP_TOKEN` と `VEDITOR_MCP_PORT` を設定すると、それぞれトークンとポートを上書きできます。起動オプション `--mcp-serve` を使うと、その起動中だけサーバを有効にでき、保存設定は変更しません。

### Claude Code から使う

1. 上記のメニューでサーバを起動し、**接続情報...** から mcp.json の内容をコピーします。
2. それを `veditor-mcp.json` として保存します（`<token>` は実際のトークンに置き換えます）。
3. 次のように起動します。

   ```sh
   claude --mcp-config veditor-mcp.json --strict-mcp-config
   ```

実際に動作する設定例:

```json
{"mcpServers":{"veditor":{"type":"http","url":"http://127.0.0.1:8765/mcp","headers":{"Authorization":"Bearer <token>"}}}}
```

### Codex CLI から使う

エディタを起動した状態で、Codex CLI の `config.toml`（通常は `~/.codex/config.toml`）に stdio ブリッジを登録します。`<token>` と必要なら実行ファイルのパスを置き換えてください。

```toml
[mcp_servers.veditor]
command = "v-simple-editor.exe"
args = ["--mcp-stdio", "--port", "8765"]
env = { VEDITOR_MCP_TOKEN = "<token>" }
# 非対話 (codex exec) でツール呼び出しが自動拒否されないようにする
default_tools_approval_mode = "approve"
```

このブリッジは stdin/stdout の「1 行 1 JSON-RPC」を localhost の HTTP MCP サーバへ中継します。HTTP/ネットワークのエラーはプロセスを落とさず、`-32603` と次のメッセージを返します: `invalid token`（HTTP 401、トークン不一致）、`editor did not respond in time`（リクエストタイムアウト）、`editor not running`（接続拒否）、`unexpected editor response`（その他の HTTP/ネットワークエラー）。ポートを変更した場合は `--port` も合わせて変更してください。

`--token <token>` を args に渡す形も受け付けますが、プロセス一覧からトークンが見えるためブリッジは stderr に警告を出します。`VEDITOR_MCP_TOKEN` が設定されている場合はそちらが優先されます。ブリッジの HTTP リクエストのタイムアウトは `VEDITOR_MCP_TIMEOUT_MS`（1000 以上のミリ秒）で変更できます。

### エディタ内 AI チャット

一番簡単な入口は画面右下（ステータスバー右端）の **「LLM に指示を出す」** ボタンです。押すと MCP サーバの起動と AI チャット Dock の表示をまとめて行い、入力欄にフォーカスが移ります（不要なら **表示 > 「LLM に指示を出す」ボタンを表示** のチェックを外すと隠せます。設定は保存されます）。**表示 > AI チャット** で AI チャット Dock を直接開くこともできます。Dock の下部 (送信ボタンの下) には MCP サーバの状態（待受ポート）、最後に接続してきたクライアント（`initialize` の clientInfo: Claude Code / Codex など）、選択中の CLI の検出状況とモデル名が常に表示され、隣の **「接続 / 切断」** ボタンでサーバを起動・停止できます。CLI の選択で **Claude Code** と **Codex CLI** を切り替えられます。Codex は `codex exec --json` を stdio ブリッジ（このエディタ自身の `--mcp-stdio`）付きで起動し、会話継続は `codex exec resume <thread_id>`、モデル名は `~/.codex/config.toml` の `model` を表示します（コマンド名は QSettings の `aiChatCodexCommand`、既定 `codex`）。Codex は MCP ツール呼び出しごとに承認を求め、非対話実行では自動拒否（`user cancelled MCP tool call`）されるため、このサーバに限り `-c mcp_servers.veditor.default_tools_approval_mode='approve'` を渡して自動承認にしています（シェルコマンドの sandbox は `read-only` のまま）。`~/.codex/config.toml` に手で登録する場合も同じキーを付けてください。Dock は `claude` CLI を headless で `-p --output-format stream-json --verbose --mcp-config <一時json> --strict-mcp-config [--resume <sessionId>] --allowedTools mcp__veditor` のオプション付きで起動し、MCP 設定を一時ファイルで渡して `veditor` のツールだけを許可します。プロンプトは argv ではなく stdin で渡します。会話継続時は前回の `session_id` を `--resume <sessionId>` で指定します（`--allowedTools` の直前）。子プロセスの環境から `ANTHROPIC_API_KEY` と `ANTHROPIC_AUTH_TOKEN` を除去するため、ログイン済みの Claude Pro / Max のサブスク枠で動作します。使用する CLI 名は QSettings の `aiChatCommand` で変更できます。

`claude` が PATH に無い場合は、先に次を実行してください。

```sh
npm i -g @anthropic-ai/claude-code
```

### 安全性

MCP の変更系ツールは確認ダイアログを出さず、原則として自動承認されます。ただし `run_command` は `list_commands` が返す危険度（safe / blocking / quit）で分類され、blocking（モーダルダイアログ）のコマンドは `allowBlocking:true` を渡さない限り拒否され、quit のコマンドは MCP から常に実行できません。編集前に Undo 状態を 1 回だけ保存するため、通常は LLM の 1 操作を **Ctrl+Z 1 回**で戻せます。`run_command` は各メニューアクションが自分で Undo を積むため、MCP 側では二重に積みません。`add_caption` は字幕エディタ側の状態を変更するため Ctrl+Z の対象外です。タイムラインへ反映するには `apply_captions` を呼びます（こちらは Ctrl+Z で戻せますが、戻るのはタイムライン側だけです）。トークンは他人に渡さないでください。サーバは `127.0.0.1` にのみバインドされるため、LAN 上の別の端末からは接続できません。

### MCP ツール

| 名前 | 何をするか | Undo |
|---|---|---|
| `get_project_info` | プロジェクト情報を読み取る | なし |
| `get_timeline` | タイムラインを読み取る。各クリップの `leadIn` / `trailOut` は `{type, durationSec, alignment, easing, softness, borderWidth, borderColor}`、`overlap` は隣接クリップとのトランジションで実際に重なる秒数 `{leadInSec, trailOutSec}` を返す。各クリップに `colorCorrection`（28 項目の色補正値）と `lut`（`{filePath, intensity}`）、各トラックに `name` / `color`（`#rrggbb`、未設定は空文字）も返る（いずれも既存フィールドと `required` は不変）。Wave 13 から、接続中のクリップには `anchorUid` / `anchorOffsetSec` も返る (接続の無いクリップは従来どおり)。Wave 17 から、各クリップに任意の `autoOrientToCamera` も返る。Wave 18 から、3D レイヤーのクリップに任意の `worldX` / `worldY` (0 のときは省略) も返る | なし |
| `get_frame` | 指定時刻の合成フレームを PNG で返す（既定 640px 以下・1MB 以内） | なし |
| `get_captions` | 字幕を読み取る | なし |
| `get_export_status` | `export_video` ジョブの状態・進捗を返す | なし |
| `compare_project` | `filePath` の保存済み `.veditor` と現在のタイムラインを構造比較し、`{ok, changes:[{type, path, before, after}], summary:{added, removed, moved, trimmed, changed}}` を返す（読み取り専用）。`type` は `Added` / `Removed` / `Moved` / `Trimmed` / `PropertyChanged` / `EffectsChanged` / `TransitionChanged` / `TrackFlagChanged`。数値の微小差（eps 以内）は同一とみなす | なし |
| `list_commands` | メニュー直下のお気に入り登録可能なアクションを一覧する（サブメニュー内の項目は含まない）。ID は `<メニューキー>.<メニュー内の通し番号>`（例: `file.11`, `tools.41`）で、表示文言の変更では変わらないが、メニューの途中にアクションが追加されると後続の番号がずれる（位置依存）ため、実行前に `list_commands` で id を確認する。`query` 省略時は全件（約 230 件・JSON で約 60KB）を返すので通常は `query` で id / 表示名 / メニュー名を部分一致フィルタする。各項目に `risk`（safe / blocking / quit）と `enabled` を返す | なし |
| `run_command` | `list_commands` のアクションを ID で実行する（`allowBlocking` は既定 false。応答の `undoRecorded` で Ctrl+Z / `undo` の対象になったか確認できる） | アクション依存（MCP 側では追加しない） |
| `export_video` | タイムラインを動画へ非同期で書き出し、`jobId` を返す。音声はトリム・分割・並べ替え・音量・ミュートを反映したタイムラインのミックスを ffmpeg で作ってから多重化する（`audioCodec` / `audioBitrate` 省略時は aac / 192 kbps）。`rateControl`（`bitrate` / `crf`、既定 bitrate）と `crf`（-1 = 未指定、通常 0..51、AV1 は 0..63）で品質ベースのレート制御を選べる（CRF 指定時にハードウェア AV1 が使えない環境ではソフトウェア AV1 へフォールバック）。`audioOnly: true` は音声だけを `outputPath` の拡張子（m4a / wav / mp3）に合わせたコーデックで同期書き出しし、`jobId` の代わりに `{ok, outputPath, warnings}` を返す（映像系の引数は無視して `warnings` に列挙）。Wave 10 から `autoLoudness`（boolean）/ `targetLufs`（number、-70..-1）で自動ラウドネス正規化を指定でき、省略時は `preset` の設定に従う（下の説明を参照。挙動変更） | なし |
| `import_media` | ダイアログなしで素材を指定トラックへ取り込む（`kind` 既定 `auto`: 映像があれば V/A の組、無いファイル（BGM・ナレーション）は音声トラックだけ。`video` / `audio` で片側だけ。開けないファイルはエラー）。ロック中のトラックを指定するとエラー「ロック中のトラックには配置できません」、自動配置はロック中のトラックを飛ばす (Wave 14。US-1306) | あり |
| `save_project` | プロジェクトを指定パスへ保存する（ダイアログなし。拡張子は GUI と同じ `.veditor` を推奨） | なし |
| `open_project` | 指定パスのプロジェクトを読み込む（未保存確認なし） | なし |
| `select_clip` | 指定クリップを選択する（`kind` / `trackIndex` 省略時は video / 0） | なし |
| `clear_selection` | 選択をすべて解除する | なし |
| `split_clip` | クリップを分割する。接続先のクリップを分割すると、接続中の各クリップは位置を変えずに左右どちらかの片に付け替えられる (Wave 13) | あり |
| `delete_clip` | クリップを削除する。主ストーリーラインの接続先 (V1) を削除すると、接続クリップも連動して削除される (Wave 13。ロック中のトラックの接続クリップと、削除される区間の外にはみ出した接続クリップは削除せず接続を解除して警告) | あり |
| `connect_clip` | 指定クリップ (と linkGroup が同じクリップ) を V1 のクリップ (`anchorClipIndex`。`anchorTrackIndex` は 0 のみ) に接続する。接続したクリップは接続先の移動・トリム・速度変更に追従する。`get_timeline` の各クリップに接続中は `anchorUid` / `anchorOffsetSec` が付く。応答は `{ok, anchorUid, anchorOffsetSec, warnings}` | あり |
| `disconnect_clip` | 指定クリップ (と linkGroup が同じクリップ) の V1 への接続を解除する (位置は変わらない)。接続されていないクリップはエラー。応答は `{ok, warnings}` | あり |
| `move_clip` | クリップを移動する（`newTrackIndex` で別トラックへ。既定プロジェクトは V1/A1 の 1 段なので、先に `run_command` の「ビデオトラックを追加」を実行する。存在しないトラックを指定するとエラー文でそのコマンド id を案内する。V1 のクリップに接続クリップがあるときは、接続クリップも一緒に動く (Wave 13)） | あり |
| `set_clip_property` | クリップのプロパティ（volume / opacity / speed / pan / videoScale / reversed / autoOrient / autoOrientToCamera / is3DLayer / worldX / worldY）を変更する。`speed` と `reversed` はリンクした音声クリップにも同時に適用される。`autoOrient`（boolean）はモーションパスの進行方向へクリップを自動回転させる（`get_timeline` の各クリップに `autoOrient` が返る）。`autoOrientToCamera`（boolean、映像クリップのみ）は 3D レイヤーをプロジェクトのカメラへ向ける（`is3DLayer` が有効で、カメラが trueProjection のときだけ効く。`autoOrient` のパス進行方向の回転とは別機能で、`get_timeline` の各クリップに任意項目 `autoOrientToCamera` が返る）。`is3DLayer`（boolean、映像クリップのみ）は 3D レイヤーの ON / OFF で、false にすると positionZ / rotationX / rotationY / rotationZ は 0 に消える（Ctrl+Z / `undo` で戻る）。応答には変更後の `is3DLayer` も含まれる。`worldX` / `worldY` (number、±10000、映像クリップのみ) は 3D レイヤーの射影前のワールド X / Y (キャンバス px) で、プロジェクトカメラ (trueProjection) のときだけ効き、`is3DLayer` が false のクリップは拒否する (Wave 18。`get_timeline` の各クリップに 0 以外のとき `worldX` / `worldY` が返る。1 回の set は undo 1 段)。音声クリップとロック中のトラックはエラー | あり |
| `set_clip_wiggle` | `trackIndex` / `clipIndex` の映像クリップへ wiggle（揺れ）を設定する。`wiggle` は `{positionFrequency, positionAmplitude:{x,y}, rotationFrequency, rotationAmplitudeDeg, scaleFrequency, scaleAmplitude, seed, octaves, enabled}` で、省略したキーは既定値に戻る（全置換）。未知のキーはエラー、`octaves` は 1..6 に丸める。応答は `{ok, wiggle}`。音声クリップとロック中のトラックはエラー | あり |
| `set_clip_expression` | 映像クリップの `propPath`（`transform.position.x` など式を付けられる正規パス）に式 `code` を設定する。`propPath` が未知ならエラー、`code` が空文字・空白だけならそのプロパティの式を解除する。応答は `{ok, propPath, cleared, expressions}`。音声クリップとロック中のトラックはエラー | あり |
| `get_clip_dynamics` | 映像クリップの `{is3DLayer, autoOrientToCamera, wiggle, expressions}` を返す（読み取り専用。`expressions` は propPath から式コードへの対応）。音声クリップはエラー | なし |
| `trim_clip` | edge=in は開始位置を保ったまま timeSec 時点の内容を新しい先頭にし、以降が (timeSec−開始) だけ左へ詰まる（RippleIn）。edge=out は末尾を timeSec にし後続が詰まる（RippleOut）。kind は video のみだが、同じ linkGroup の音声クリップも同じ量だけトリムされる（ripple 既定 true） | あり |
| `set_transition` | `kind` 既定 `video`: V1 のクリップにトランジションを設定する（FadeIn は先頭、その他は末尾、None で解除）。`type` には `MorphCut`（オプティカルフローで A/B を変形しながらつなぐモーフカット）も指定できる。ビデオトランジションはプレビューだけでなく書き出しにも反映される。`kind:"audio"` では指定音声トラック（`trackIndex`）のクリップにコンスタントパワーの音声トランジションを設定する。音声で使える `type` は `CrossDissolve`（隣接クリップとのクロスフェード）/ `FadeIn` / `FadeOut` のみで、None を含む他の type はエラー。`durationSec` は 0.1..5.0（既定 0.5）。音声側の変更は映像へミラーしない。`alignment`（`Center` / `Start` / `End`、既定 Center）と `easing`（`Linear` / `EaseIn` / `EaseOut` / `EaseInOut`、既定 Linear）は映像・音声どちらでも指定できる。`softness`（0..1）/ `borderWidth`（0..50 px）/ `borderColor`（`#RRGGBB`）はワイプ・バーンドア・アイリス・クロックワイプ系だけに効き、他の type では無視して `warning` を返す | あり |
| `add_text_overlay` | V1 にテキスト／テロップを追加する（時刻は秒、位置は 0..1。区間と重なる全クリップに付くのでクリップ境界をまたいでも表示される） | あり |
| `add_caption` | 字幕エディタの一覧に 1 件追加する（タイムラインへは `apply_captions` で反映） | なし（Ctrl+Z 対象外） |
| `apply_captions` | 字幕エディタの字幕を V1 の 1 語字幕オーバーレイとしてタイムラインへ適用する（既存の生成済み 1 語字幕は置き換え） | あり（タイムライン側のみ。字幕エディタの一覧は戻らない） |
| `remove_caption` | 字幕エディタの一覧から `index`（`get_captions` の `captions[].index`）の字幕を 1 件削除する | なし（Ctrl+Z 対象外） |
| `clear_captions` | 字幕エディタの一覧を空にする | なし（Ctrl+Z 対象外） |
| `set_playhead` | 再生ヘッドを移動する | なし |
| `undo` | 直前の編集を元に戻す | なし |
| `redo` | 元に戻した編集をやり直す | なし |
| `set_track_locked` | 指定トラックの編集ロックを設定する。ロック中は `split_clip` / `delete_clip` / `move_clip` などのクリップ編集を拒否する | なし（ロック変更自体は Ctrl+Z 対象外） |
| `set_track_property` | 指定トラック（`kind` / `trackIndex` 必須）の `name`（空文字で既定に戻す）/ `color`（`#RRGGBB`、空文字で解除）/ `muted` / `solo` / `hidden` を設定する。応答は `{ok, track:{index, name, color, locked, muted, solo, hidden}, undoRecorded}` | 名前・色はまとめて 1 回（`undoRecorded`）。muted / solo / hidden は Ctrl+Z 対象外 |
| `set_clip_label` | クリップのラベルカラーを設定する（`none` / `red` / `orange` / `yellow` / `green` / `cyan` / `blue` / `purple` / `pink`）。`kind` / `trackIndex` 省略時は video / 0 | あり |
| `set_project_option` | プロジェクト全体の設定を変更する。現在は `option=timecodeBurnIn` のみ受け付け、プレビューと以後の書き出しへ即時反映する | なし（Ctrl+Z 対象外） |
| `dynamic_zoom` | 指定動画クリップへダイナミックズームを適用し、位置とスケールに開始・終了キーフレームを生成する。`preset`（zoomIn / zoomOut / panLeft / panRight / panUp / panDown）か `start` / `end`（cx / cy / w の正規化座標）のどちらか一方を指定する（両方指定はエラー）。枠は常にキャンバスのアスペクト比に固定され、`h` を指定しても無視して warning を返す | あり |
| `match_frame` | 再生ヘッド位置（または `timeSec`）の動画クリップを、speed・逆再生・リマップを反映したソース時刻でソースモニターに開く | なし |
| `replace_clip` | 指定クリップの素材を `filePath` のメディアへ置き換える。位置・inPoint・長さを可能な限り維持し、同じ linkGroup の音声も新素材に音声があれば置き換える。新素材が短い場合は warning を返す | あり |
| `render_in_place` | 指定した映像クリップ (`trackIndex` / `clipIndex`、`kind` は `video` のみ)の効果・変形を新規メディアへ焼き込み、そのクリップを差し替える (`codec` は `h264` / `prores`、`handlesSec` 0..5 で前後の余白秒)。同じ linkGroup の音声クリップも同時に焼き込む。応答は `{ok, outputPath, replaced, linkedAudioReplaced}`。重ね合わせトランジション付き・クロマキー付きのクリップは拒否する。Wave 10: roto を持つクリップは `codec` にかかわらずアルファ付き ProRes 4444 (.mov) で焼き込み、元クリップの roto は外れる (undo で戻る。Wave 12 からは `decompose_render_in_place` でも戻る)。Wave 18: ブラシを持つクリップも `codec` にかかわらずアルファ付き ProRes 4444 (.mov) で焼き込み、差し替えクリップにブラシは残らない (`decompose_render_in_place` / undo で戻る。RenderInPlace.cpp:462-465) | あり (映像・音声まとめて 1 回) |
| `decompose_render_in_place` | `render_in_place` で差し替えたクリップ (`trackIndex` / `clipIndex`)を効果・変形つきの元のクリップに戻す (リンク音声も同時)。Wave 10: render_in_place で外れた roto は undo で戻る。Wave 12 からは分解でも元の roto が復元される (1 undo) | あり |
| `set_color_correction` | 映像クリップ（`clipIndex` 必須、`kind` / `trackIndex` 省略時は video / 0）の色補正を `values` で部分更新する（brightness / contrast / saturation / temperature / tint / highlights / shadows は -100..100、hue は -180..180、gamma は 0.1..3、exposure は -3..3、liftR/G/B と gainR/G/B は -2..2、gammaR/G/B は log2 で -6.64..4、logShadow/Mid/High の R/G/B は -1..1）。`reset: true` は既定値へ戻してから `values` を適用する。不正な項目があれば何も変えずにエラー。`hueSatWarp` は無視して `warning` を返す。応答は `{ok, colorCorrection, warning?}` | あり（1 回） |
| `apply_lut` | 映像クリップに `.cube` などの LUT ファイル（`filePath`、存在しないとエラー）を `intensity`（0..1、既定 1）で適用する。`filePath` を空文字にすると解除。応答は `{ok, lut:{filePath, intensity}}` | あり（適用・解除それぞれ 1 回。同じ値の再設定は積まない） |
| `relink_media` | 見つからないメディア / LUT のパスを `mapping`（`[{from, to}]`、1 件以上）で一括再リンクする。`to` が全て実在するファイルのときだけ変更し、リンクした映像・音声、パーティクル設定やオーバーレイの参照、ネストしたシーケンスまで 1 回の Undo で更新する。存在しないファイルを指定すると「ファイルが見つかりません」のエラーで何も変更しない。応答は `{"ok":true,"relinked":N}` | あり |
| `music_remix` | 音声クリップ（`kind` は `audio` のみ）をビート境界のセグメントで再構成し、`targetSec`（0 より大きく 86400 以下）の尺へ自動調整する。継ぎ目にはコンスタントパワーのクロスフェードが付く。ビートが 2 個未満の素材は変更せずエラー。応答に `resultDuration` と `segmentCount` を返す | あり |
| `dialogue_level` | 音声クリップ（`kind` は `audio` のみ）の短時間ラウドネスを解析し、会話音量を `targetLufs`（既定 -18）へ平準化する音量エンベロープを生成する。応答に `pointCount` / `measuredLufsMin` / `measuredLufsMax` を返す | あり |
| `get_sequences` | プロジェクト内の全シーケンスを列挙する（`{ok, sequences:[{id, name, active}]}`）。引数なし | なし |
| `set_active_sequence` | `id` のシーケンスへ切り替える。存在しない `id` はエラー | なし（Ctrl+Z 対象外） |
| `add_track` | `kind`（`video` / `audio`）のトラックを末尾に追加する | あり（1 回） |
| `remove_track` | `kind` / `trackIndex`（0 始まり）のトラックを削除する。載っているクリップも消え、トラックマット / 親子付け / 調整レイヤー / ミキサー / バス送りの参照も付け替わる。最後の 1 本とロック中は拒否 | あり（1 回で本数も中身も復元） |
| `move_track` | `kind` / `trackIndex` のトラックを `newTrackIndex` へ移動する。範囲外はエラー | あり（1 回） |
| `set_clip_enabled` | 映像・音声クリップの有効状態を `enabled` で切り替える。Null Object は対象外 | あり |
| `group_clips` | 別々のトラックのクリップをグループ化する。`clips`（`{kind, trackIndex, clipIndex}` の配列、2 件以上）を指定する。応答は `{ok, linkGroup, undoRecorded}` | あり |
| `ungroup_clips` | 指定クリップのグループを解除する。`clips`（配列、1 件以上） | あり |
| `import_edl` | `filePath` の CMX3600 EDL をプロジェクトの FPS で新規シーケンスへ読み込む。未解決素材はオフラインになる。応答は `{ok, activeId, resolvedFiles, unresolvedFiles, warnings}` | あり（1 回） |
| `import_fcpxml` | `filePath` の FCPXML の spine 直下 asset-clip と、アンカーの子要素の接続クリップを lane に取り込み、新規シーケンスへ読み込む。lane は 0 を先頭に番号の絶対値が小さい順に並べ、飛び番を詰めて 1 lane 1 トラックで取り込む (トラック数の上限なし。Wave 13)。音声専用 asset-clip は音声トラックに置く。gap の子の接続クリップと親より前に始まる接続クリップ（J カット。0 秒より前に全体が出るものは「クリップの値が不正です (接続位置): …」で読み飛ばし、一部だけ出るものは「接続クリップをタイムライン 0 で切り詰めました: …」の警告で切り詰める）も取り込み、接続クリップを持つ親の時刻範囲が不正（duration が 0、または offset / start の計算が不正）な場合は、その親とその接続クリップを警告付きで読み飛ばす。asset-clip が 0 本の spine は空のシーケンス + 警告。FCPXML では <asset> に duration があれば素材の全長を保持する（asset の start が 0 でなくても同じ。無ければ使用範囲まで）。Wave 13 から transition は前後のクリップの両縁のクロスディゾルブ (先頭・末尾はフェード)、複合クリップ (ref-clip) はネストシーケンス、マルチカム (mc-clip) は有効アングルに平坦化、clip / sync-clip は子を平坦化して取り込み、接続クリップは主ストーリーラインへの接続 (anchorUid / anchorOffsetSec) として取り込む (transition の時刻配置と mc-clip の属性は実機未確認)。それ以外の要素は読み飛ばして警告に積む。未解決素材はオフラインになる。応答は `{ok, activeId, resolvedFiles, unresolvedFiles, sequences, warnings}` (`sequences` は取り込んだ入れ子シーケンスの ID) | あり（1 回） |

`export_video` には書き出しプリセット名を渡す `preset`（組み込み → ユーザープリセットの順に解決。他の引数を併記するとそちらが優先）と、ProRes 4444 / 4444 XQ または VP9（yuva420p）でアルファを保持する `keepAlpha` / `proresProfile` があります（`keepAlpha` は `videoCodec` が `prores` かつ `proresProfile` が 4 または 5、または `videoCodec` が VP9 系のときだけ受理）。`metadata`（`title` / `author` / `copyright` / `comment` の各文字列、いずれも省略可）を指定すると、非空の項目だけが書き出しファイルへメタデータとして埋め込まれます。連番静止画書き出し・字幕の mov_text 埋め込みは書き出しダイアログの機能で、`export_video` の引数には含まれず、MCP 経由の書き出しには適用されません。

Wave 10 から `export_video` は自動ラウドネス正規化の任意引数 `autoLoudness`(boolean)と `targetLufs`(number、-70..-1、LUFS)を受け付けます (ツールの追加はなく、`outputSchema` / `required` も変わりません)。解決順は「明示した引数 > `preset` で指定したプリセットの値 > OFF(`targetLufs` は -14)」です。**挙動変更: `autoLoudness` を省略したときは指定プリセットの `autoLoudness` / `targetLufs` に従います。自動ラウドネスが ON のユーザープリセットを `preset=` で使っている既存の MCP 呼び出しは、引数を足さなくても書き出しの音量が正規化されるようになり、従来と出力音量が変わります。従来どおりにするには `autoLoudness=false` を明示してください。** 引数を省略し、プリセットも無い (または OFF のプリセット)ときは従来と同じです。`targetLufs` だけを指定して解決後の `autoLoudness` が OFF のときは、値の検証だけを行い正規化はしません。`autoLoudness=true` の明示と `audioOnly=true` の併用はエラーです。`audioOnly=true` は、ON のプリセットを指定して引数を省略した場合も自動ラウドネスを行いません (Wave 10 の挙動変更。従来はプリセットの ON に従って測定していました。GUI の音声のみ書き出しは変わりません)。測定は Wave 12 から GUI スレッドの外で実行され、`autoLoudness=true` の準備中も `get_export_status` は応答します (status は `queued`。`autoLoudness=false` の経路は従来どおり同期です)。測定に失敗した (無音・音声なしなど)場合は正規化をスキップして 0 dB のゲインで書き出しを続け、ジョブは完了します。ジョブが `failed` になるのは、音声ミックスを作れない・変換できないときだけです。`audioBitrate`(kbps、既定 192)は、192 以外のとき書き出し前に作る音声ミックス (AAC)のビットレートにも反映されます。なお RenderInPlace とレンダーキューダイアログからの手動追加には、自動ラウドネスは適用しません (設計)。

MCP サーバの自己テストは `--selftest=mcp` または `VEDITOR_MCP_SELFTEST=1` で実行できます (実装: `src/selftests/mcp_selftest.cpp`、ゲート G1..G200。ツール数は 57)。Wave 18 は MCP ツールを足さず (57 ツール)、`set_clip_property` に `worldX` / `worldY` (±10000、3D レイヤーのみ、プロジェクトカメラ (trueProjection) でのみ効く) を足し、`get_timeline` の各クリップに任意の `worldX` / `worldY` (0 のときは返さない) を返します (既存ツールの `required` は不変。selftest の拡張は G200 です。US-1815)。Wave 17 は MCP ツールを 3 本追加しました (`set_clip_wiggle` / `set_clip_expression` / `get_clip_dynamics`、ツール数 57)。`set_clip_property` に `autoOrientToCamera` / `is3DLayer` を足し、`get_timeline` の各クリップに任意の `autoOrientToCamera` を返します (既存ツールの `required` は不変)。selftest の拡張は G195..G199 です。Wave 13 は MCP ツールを 2 本追加しました (`connect_clip` / `disconnect_clip`、ツール数 54)。selftest の拡張は G190..G192 です。Wave 14 は MCP ツールを追加せず (ツール数 54)、`import_media` のロックの G193 と、V1 にクリップが無い書き出しの G194 を足しました。Wave 12 も MCP ツールの追加はなく (ツール数 52)、`export_video` の入力スキーマ・説明文・引数 (`autoLoudness` / `targetLufs`) と省略時の規則は変わりません。selftest の拡張は G189 (autoLoudness=true の準備中でも get_export_status が応答する) と G188-emptyV1 の期待値の反転 (V1 が空で上位に映像があるとき無音または混ざった音声トラックが付く) だけです。Wave 8 では MCP ツールの追加はありません (ツール数 52 / ゲート G1..G177 のまま)。Wave 10 でも MCP ツールの追加はなく (ツール数 52)、`export_video` の任意引数 `autoLoudness` / `targetLufs` の追加と、selftest の拡張 (G178..G187) だけです。Wave 11 の変更に MCP ツールの追加はなく (ツール数 52)、既存ツールの入力スキーマ・説明文も変わりません。selftest の拡張は G188 (自動ラウドネス ON で音声ミックスの作成に失敗したとき export_video のジョブを拒否する) と、G182 への音声シーケンスが空のプロジェクトのサブケース (無音ミックス 1 回) の追加です。

ゲートを持つ selftest（`GateTally` を使うもの）は、最後に `N PASS, M FAIL, K SKIP` の 3 語書式のサマリ行を標準エラー出力へ出します。`SKIP` は環境依存で検証できなかったゲート（エンコーダ無し、Debug ビルドなど）で、未検証を意味します（終了コードは FAIL の数なので、SKIP だけなら 0 になります）。`VEDITOR_SELFTEST_TIMEOUT_SEC`（秒、既定 900、0 で無効）を超えると `summary: TIMEOUT after <N>s (selftest=<name>)` を出して終了コード 124 で打ち切られます。`--selftest=all` の対象は `VEDITOR_ALL_SELFTEST_FILTER`（カンマ区切り）で絞れ、未知または空の項目は終了コード 2 になります。MCP サーバの待受ポートは `VEDITOR_MCP_PORT` で指定でき、使い切ると警告が出ます。

---

## Supported Formats

| Codec | Decode | Encode |
|-------|--------|--------|
| H.264 (x264) | Yes | Yes |
| H.265 (HEVC) | Yes | Yes |
| AV1 | Yes | Yes (SVT-AV1) |
| ProRes | Yes | Yes |
| VP9 | Yes | Yes |

| Container | Support |
|-----------|---------|
| MP4 | Yes |
| MKV | Yes |
| MOV | Yes |
| WebM | Yes |
| FLV | Yes |

---

## Platforms

| Platform | Status |
|----------|--------|
| **Windows** | Primary |
| **macOS** | Supported |
| **Linux** | Supported |

---

## Build

### Prerequisites

- CMake 3.20+
- C++17 compiler (MSVC 2019+, GCC 11+, Clang 14+)
- Qt6 (Widgets, Gui, Multimedia, MultimediaWidgets, OpenGL, OpenGLWidgets, Network)
- FFmpeg 5+ (libavformat, libavcodec, libavutil, libswscale, libswresample, libavfilter)
- pkg-config
- Python 3.8+ (optional, for scripting extension)

### Windows (One-Click Setup)

`setup.bat` が必要なツールを **すべて自動でインストール** します。手動での事前準備は不要です。

```bash
# GitHub から取得（git clone または ZIP ダウンロード）
git clone https://github.com/machaniconico/v-simple-editor.git
cd v-simple-editor

# 右クリック → 「管理者として実行」で setup.bat を起動
setup.bat
```

> **Note:** 自動インストールには **管理者権限** が必要です。`setup.bat` を右クリック →「管理者として実行」してください。winget (Windows 10 1809+ / 11 に標準搭載) を使って不足ツールを自動導入します。

`setup.bat` が行うこと：
- **Git** の自動インストール（未インストールの場合）
- **CMake** の自動インストール（未インストールの場合）
- **Visual Studio 2022 Build Tools** + C++ ワークロードの自動インストール（未インストールの場合）
- **vcpkg** の自動インストール（C:\vcpkg）
- **Qt6, FFmpeg, pkgconf** のインストール（初回は30-60分）
- CMake configure & Release ビルド
- DLL 配置 & FFmpeg CLI ツールのコピー
- **Python** のインストール（任意 — スクリプト拡張機能用）
- **Whisper** のインストール（任意 — AI字幕生成用）
- 完了後にアプリを起動するか確認

手動でビルドする場合：
```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### macOS (Homebrew)

```bash
brew install cmake qt@6 ffmpeg pkg-config python@3

cmake -B build -S .
cmake --build build
```

### Linux (apt)

```bash
sudo apt install cmake g++ pkg-config \
    qt6-base-dev qt6-multimedia-dev libqt6opengl6-dev \
    libavformat-dev libavcodec-dev libavutil-dev \
    libswscale-dev libswresample-dev libavfilter-dev \
    python3-dev

cmake -B build -S .
cmake --build build
```

---

## Keyboard Shortcuts

| Action | Shortcut |
|--------|----------|
| New Project | `Ctrl+N` |
| Open File | `Ctrl+O` |
| Save Project | `Ctrl+S` |
| Export | `Ctrl+E` |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Shift+Z` |
| Copy / Paste | `Ctrl+C` / `Ctrl+V` |
| Duplicate / Select All | `Ctrl+D` / `Ctrl+A` |
| Blade All Tracks at Playhead | `Ctrl+Shift+K` |
| Go to Timecode | `Ctrl+Shift+J` |
| Nudge 1 / 10 frames | `Alt+←/→` / `Alt+Shift+←/→` |
| Split at Playhead | `S` |
| Delete / Ripple Delete | `Del` / `Shift+Del` |
| Toggle Snap | `N` |
| Zoom In / Out | `Ctrl+=` / `Ctrl+-` |
| Play / Pause | `K` |
| Reverse / Forward | `J` / `L` |
| Mark In / Out | `I` / `O` |
| Add Text | `T` |
| Toggle Mute | `M` |
| Add Marker | `Ctrl+M` |
| Color Correction | `Ctrl+G` |
| Keyframes | `Ctrl+K` |
| Render Queue | `Ctrl+Shift+R` |
| Resource Guide | `F1` |

All shortcuts are customizable via Edit > Keyboard Shortcuts.

---

## Testing

Selftests are wired into the executable itself, dispatched via a single
`kArgvSelftests[]` table in `src/selftests/SelftestRegistry.cpp` (SSOT, 279 entries; `--selftest=help` prints 280 because it also lists `credential-store`, which is handled in `src/main.cpp`). Four entry
points cover the routing matrix:

```bash
# 1. enumerate (compact, one line each + env name)
./build_win/Release/v-simple-editor.exe --selftest=list

# 2. detailed help (per-entry description + env name + usage header)
./build_win/Release/v-simple-editor.exe --selftest=help

# 3. run a single selftest
./build_win/Release/v-simple-editor.exe --selftest=tracker-preset
./build_win/Release/v-simple-editor.exe --selftest=parity
./build_win/Release/v-simple-editor.exe --selftest=trackmatte-parity

# 4. CI-friendly full sweep (runs every entry; exit code = failed count)
./build_win/Release/v-simple-editor.exe --selftest=all
```

Each `--selftest=<name>` also has a parallel env-gate (`VEDITOR_<NAME>_SELFTEST=1`)
that runs the same function — useful for legacy CI / dev setups that already
script env vars:

```bash
VEDITOR_TRACKER_PRESET_SELFTEST=1 ./build_win/Release/v-simple-editor.exe
VEDITOR_ALL_SELFTEST=1 ./build_win/Release/v-simple-editor.exe   # full sweep
```

Typos / stale selftest names exit 2 with a friendly stderr message instead
of silently launching the GUI:

```bash
$ ./build_win/Release/v-simple-editor.exe --selftest=unknown-foo
[ERROR] unknown selftest: --selftest=unknown-foo
Run with --selftest=list to see available selftests.
$ echo $?
2
```

Since Wave 9 (US-800) every selftest that uses `GateTally` is cross-checked against the
`(N gates)` figure in its registry description: a mismatch prints
`FAIL gate-count: declared N, ran M (selftest=<name>)` and exits non-zero, and the
`--selftest=all` sweep ends with a `gate-count summary: verified V, mismatch W, unverified U,
no-declaration D` line. New in Wave 9: `--selftest=parent-gate` (7 gates; the shared
`tlrender::parentLayerAt` rule) and `gate-count-probe` (internal helper for the gate-tally
selftest, 2 gates, not meant to be run on its own). Gate counts for other selftests are in
the registry descriptions (`grep -oE '\([0-9]+ gates\)' src/selftests/SelftestRegistry.cpp`).

Adding a new selftest = one line in `kArgvSelftests[]`: argv-switch name +
`VEDITOR_<NAME>_SELFTEST` env var + function pointer + QApplication-required
flag + 1-line description. Both argv-switch and env-gate routing are wired
automatically.

---

## Continuous Integration

`.github/workflows/selftest.yml` — a Windows MSVC smoke workflow on GitHub
Actions. Currently `workflow_dispatch` (manual trigger only); auto trigger
on push / pull_request gets enabled once the first green run pins the
vcpkg + Qt provisioning costs.

Steps (build + light smoke, ~15-30 min on a fresh runner):

1. checkout
2. MSVC x64 dev env
3. vcpkg install ffmpeg:x64-windows
4. Qt 6.7.0 install (qtmultimedia / qtsvg / qtimageformats)
5. cmake configure + Release build
6. smoke: `--selftest=list` / `--selftest=tracker-preset` / `--selftest=hdr-routing`
7. guard: `--selftest=unknown-foo` exits 2

Heavy entries (`--selftest=all` sweep with parity / e2e / libavcore-encode)
need real footage assets and stronger DLL surface than the runner ships;
a separate workflow will follow.

---

## Architecture — SSOT modules

A few critical SSOTs (single source of truth) keep preview and export
byte-equivalent and the selftest dispatch coherent:

| SSOT | Location | Purpose |
|---|---|---|
| `tlrender::renderFrameAt` | `src/tlrender/` | Timeline → QImage rendering, every export path goes through here |
| `trackmatte::composite` | `src/trackmatte/` | Track matte (4 types) compositing, premul ARGB32 + QPainter SourceOver, byte-equivalent to matte-free path |
| `clipgeom` | `src/clipgeom.h/cpp` | Clip geometry (scale/translate/rotation) anchored at layer center |
| `libavcore` | `src/libavcore/` | In-process FFmpeg integration (Decode / Encode / Probe / VideoFilterGraph / Concat) replacing per-call ffmpeg.exe subprocess |
| `kArgvSelftests` | `src/selftests/SelftestRegistry.cpp` | Selftest dispatch table (279 entries × 5 fields: name / envVar / fn / needsQApp / description) |

Architecture rationale: preview ≠ export was historically the #1 NLE
parity bug source. The three rendering SSOTs (`tlrender` / `trackmatte` /
`clipgeom`) collapse preview and every export path into the same call
graph; the `PARITY` selftest (`--selftest=parity` S1-S11) enforces it.

---

## Project Structure

```
v-simple-editor/
├── CMakeLists.txt
├── setup.bat                    # Windows env setup
├── .github/workflows/
│   └── selftest.yml             # CI smoke (draft, workflow_dispatch)
├── resources/
│   ├── resources.qrc            # Qt resource file
│   └── icons/                   # App icons
├── scripts/
│   └── realfootage_parity.sh    # Real-footage selftest harness
└── src/
    ├── main.cpp                 # Entry point
    ├── selftests/               # SelftestRegistry.{h,cpp} dispatcher + per-feature *Selftest.cpp files
    ├── libavcore/               # In-process FFmpeg (Decode/Encode/Probe/VideoFilterGraph/Concat)
    ├── MainWindow.h/cpp         # Main application window
    ├── VideoPlayer.h/cpp        # Video decode & preview
    ├── Timeline.h/cpp           # Multi-track timeline
    ├── ProjectSettings.h/cpp    # Project configuration
    ├── ProjectFile.h/cpp        # .veditor save/load + tracker preset persistence
    ├── ExportDialog.h/cpp       # Export settings UI
    ├── Exporter.h/cpp           # FFmpeg encoding (in-process via libavcore)
    ├── CodecDetector.h/cpp      # HW codec detection
    ├── UndoManager.h/cpp        # Undo/redo stack
    ├── GLPreview.h/cpp          # OpenGL 3.3 GPU preview
    ├── TrackerPreset.h/cpp           # Motion tracker preset (7 built-in)
    ├── TrackerPresetRegistry.h/cpp   # Motion preset registry (QSettings)
    ├── MotionTrackerDialog.h/cpp     # Motion tracker dialog + preset UI
    ├── PlanarTrackerPreset.h/cpp     # Planar tracker preset (5 built-in)
    ├── PlanarTrackerPresetRegistry.h/cpp
    ├── PlanarTrackerDialog.h/cpp     # Planar tracker dialog + preset UI
    ├── ThemeManager.h/cpp       # UI themes
    ├── Overlay.h/cpp            # Video overlays
    ├── TextManager.h/cpp        # Text/telop system
    ├── TextInteractive.h/cpp    # On-canvas text editing
    ├── TextAnimator.h/cpp       # 15 text animations
    ├── VideoEffect.h/cpp        # Effect processing
    ├── VideoEffectDialogs.h/cpp # Effect parameter UI
    ├── Keyframe.h/cpp           # Keyframe animation
    ├── EffectPlugin.h/cpp       # Plugin architecture
    ├── EffectPreset.h/cpp       # Preset management
    ├── ShaderEffect.h/cpp       # GPU shader effects
    ├── AutoEdit.h/cpp           # AI auto-editing
    ├── AutoSave.h/cpp           # Auto-save & recovery
    ├── WaveformGenerator.h/cpp  # Audio waveform
    ├── MultiCam.h/cpp           # Multi-camera editing
    ├── MotionTracker.h/cpp      # Motion tracking
    ├── TrackerLink.h/cpp        # Tracker to effect link
    ├── NoiseReduction.h/cpp     # Audio/video denoise
    ├── SubtitleGenerator.h/cpp  # Whisper subtitles
    ├── LutImporter.h/cpp        # LUT import
    ├── VideoStabilizer.h/cpp    # Stabilization
    ├── SpeedRamp.h/cpp          # Variable speed
    ├── AudioEQ.h/cpp            # EQ & audio effects
    ├── TimelineMarker.h/cpp     # Markers & chapters
    ├── ProxyManager.h/cpp       # Proxy workflow
    ├── RenderQueue.h/cpp        # Batch rendering
    ├── ScreenRecorder.h/cpp     # Screen capture
    ├── AIHighlight.h/cpp        # AI highlight reel
    ├── LayerCompositor.h/cpp    # Layer compositing
    ├── TransformAnimator.h/cpp  # Transform animations
    ├── MaskSystem.h/cpp         # Masks & mattes
    ├── ParticleSystem.h/cpp     # Particle effects
    ├── Camera3D.h/cpp           # 3D camera
    ├── Expression.h/cpp         # Expression engine
    ├── ShapeLayer.h/cpp         # Shape primitives
    ├── Precompose.h/cpp         # Pre-compose/nesting
    ├── Rotoscope.h/cpp          # Rotoscoping
    ├── WarpDistortion.h/cpp     # Warp effects
    ├── VSTHost.h/cpp            # VST3/AU plugins
    ├── PythonScript.h/cpp       # Python scripting
    ├── ShortcutEditor.h/cpp     # Shortcut customization
    ├── RecentFiles.h/cpp        # Recent file history
    ├── NetworkRender.h/cpp      # Distributed rendering
    └── RemotionExport.h/cpp     # Remotion integration
```

---

## Roadmap

- [x] Phase 1: Video load, preview, timeline, cut, export (13 presets)
- [x] Phase 2: Multi-track, transitions, text overlay, PiP, audio (17 features)
- [x] Phase 3: Color grading, effects, keyframes, plugins (14 features)
- [x] Phase 4: Project save/load, OpenGL GPU preview (4 features)
- [x] Phase 5: Waveform, AI auto-edit, themes, multi-camera (4 features)
- [x] Phase 6: Advanced text/telop system (9 features)
- [x] Phase 7: Motion tracking, noise reduction, Whisper subtitles, presets (5 features)
- [x] Phase 8: Auto-save, proxy editing, render queue (3 features)
- [x] Phase 9: LUT, stabilizer, speed ramp, screen recorder (4 features)
- [x] Phase 10: Audio EQ, markers, AI highlight (3 features)
- [x] Phase 11: AE compositing - layers, transforms, masks, particles (4 features)
- [x] Phase 12: Motion graphics - 3D camera, expressions, shapes, text animator (4 features)
- [x] Phase 13: Advanced compositing - tracker link, pre-compose, rotoscope, warp (4 features)
- [x] Phase 14: Polish & extensibility - shortcuts, recent files, icon, GPU shaders, VST/AU, Python scripting, network render, Remotion export
- [x] Sprint 15-22: External tool import (OBS / Affinity / Blender), mobile export (iOS / Android), platform pipelines (YouTube OAuth, Vimeo, Twitch, Frame.io, DaVinci, FCPXML), AI smart-edit, cloud render, X / Instagram upload, batch export, loudness (BS.1770), HDR routing, multi-cam, chroma key, audio restoration, animated GIF / WebP, easing, subtitle translation, lower-third, watermark
- [x] NLE Parity SSOT: `tlrender::renderFrameAt` (every export path) + `trackmatte::composite` (4 matte types) + `clipgeom` (transform parity)
- [x] PRD-B series: `libavcore` in-process FFmpeg migration (h264_mf 8-bit / HDR10 subprocess fallback)
- [x] Tracker Preset system: Motion / Planar with Registry + Dialog UX + ProjectFile persistence
- [x] Selftest argv-switch SSOT: `kArgvSelftests[]` 279-entry table in `src/selftests/SelftestRegistry.cpp`, `--selftest=<name>` + `VEDITOR_*_SELFTEST` + `--selftest={list,help,all}` + unknown-name guard
- [x] CI workflow draft (`.github/workflows/selftest.yml`, `workflow_dispatch` only)
- [x] Wave 6 (17 stories): menu LUT export fix, marquee / zoom / timecode jump, clip-edge & label selection, track FX mapping fix + master EQ, track FX persistence fix, export user presets, ProRes 4444 alpha export, media-pool thumbnails, clip filmstrip, sequence dock, broadcast safe / leave color, mesh warp, track remove / reorder, bundle adjustment
- [x] Wave 7 (24 stories, US-600..623): clip enable/disable, loudness-gain project persistence, keyword bins, sequence-from-clip, unused-media removal scope extension, clip grouping, align/distribute, keyframe copy/paste, timeline hover skim, per-effect mask, offset wheel, puppet pin, VP9 alpha export, image-sequence export, export metadata, mov_text subtitle embedding, EDL/FCPXML import, de-esser, audio mixer dock (track-gain export fix), automatic loudness normalization on export, multi-frame camera tracking with keyframe application, bundle adjustment Schur-complement solver
- [x] Wave 8 (US-700..718): per-entry selftest watchdog and PASS / FAIL / SKIP gate tally, shared export job mapping, audio export byte-compare fixture, linked edge-drag trim (Alt = one side), align/distribute through the rendered transform, surface / mesh / puppet pin tools through the clip transform, special-composite preview parity, legacy keyframe dialog `grade.*` migration and hue / gamma / highlights / shadows keyframes, FCPXML connected clips and audio lanes, batch export queue auto loudness, user preset round-trip, camera-tracking backend tag and UI polish, de-esser dock (live-apply). No MCP tools added (52 tools, G1..G177)
- [x] Wave 9 (US-800..810): selftest gate-count auto cross-check, auto loudness prepared inside the shared export job builder, batch export queue exports the timeline audio mix, export audio bitrate honored on AAC intermediates and temp-file cleanup unified in `RenderQueue`, shared parent-layer rule (`parentLayerAt`) for the renderer / align-distribute / surface tools, EQ / compressor / reverb / noise-reduction docks follow project changes, FCPXML gap children / J-cuts / empty spines, imported clips keep the source length only under the conditions above (FCPXML: `<asset>` has a `duration`; EDL: media found and probed length longer than the used range), saved queues keep the project path instead of a temporary audio file. No MCP tools added
- [x] Wave 10 (US-900..915): selftest hygiene (mcp moved to the gate tally, `objectName` lookups, light3d latch, non-flat audio EQ fixture, watchdog minidump + text stack), rotoscope data moved into `Timeline` / `ProjectFile` / undo and reflected in every export path and in RenderInPlace (existing roto projects export differently), roto follows clip edits and the main-clip cross-track drag, MCP `export_video` `autoLoudness` / `targetLufs` (omitted = follow the preset; behavior change for auto-loudness-ON user presets), VideoPlayer parent admission observed against `parentLayerAt` (`video-player-parent` selftest, divergences kept as limitations), audio dock gestures are undoable, the selftest hang root cause (self-deadlock in the Qt message handlers) fixed. No MCP tools added (52 tools, G1..G187)
- [x] Wave 11 (US-1000..1020): selftest hygiene (gate-tally operand details, light3d, watchdog dump cleanup, mcp compound gate values), GUI auto loudness caught in `prepareJob` (empty audio sequence = silence, mix failure = rejected job), RenderInPlace of reversed clips matches the timeline frame, roto alpha survives the grade stage and non-geometric FX, VideoPlayer parent adoption decided by `parentLayerAt`, cross-track drop is undoable and link partners follow, track-matte data moved into `Timeline` (follows cross-track drag, link drops and range ripple), de-esser listen follows its track, caption split / merge, export-complete notification (off by default), master-bus spectrum analyzer dock, legacy EQ to 4-band migration. No MCP tools added (52 tools, G1..G188)
- [x] Wave 12 (US-1100..1127; US-1112 / US-1113 EXR image-sequence export were sent to Wave 13 because the bundled avcodec had no exr encoder, and shipped there): selftest hygiene (stale `(N gates)` corrections, matte-export E2E selftest), empty-V1 exports get an audio track, matte and bus-assignment edits absorbed into undo, GPU special composite through one baked-mode entry, export audio mix split into plan / executor with an asynchronous auto-loudness preparation (GUI, mobile, MCP) and cancel, job-uuid queue isolation and a shared render-queue dialog, reversed-bake tail alignment, RenderInPlace roto restored on decompose, per-clip uid keys for roto / parent / matte, per-clip 4-band EQ, new-track drop band, per-track pan and gain (`trackMix`), strip pan / bus controls, AUX sends with bus gain / mute / solo in export, tap-source spectrum analyzer. No MCP tools added (52 tools, G1..G189)
- [x] Wave 13 (US-1200..1221): selftest hygiene (type-only `findChild` lookups named, the autosave recovery dialog suppressed under selftests), connected clips (a clip follows a V1 anchor; Edit menu / context menu / MCP `connect_clip` and `disconnect_clip`) and an opt-in magnetic primary-storyline mode (`timeline.storyline_mode`, default OFF), FCPXML import without a track limit with transition / compound clip / multicam / clip / sync-clip support and FCPXML export of lanes and connected clips, track solo in the export mix, mobile and render-queue-dialog exports through the timeline audio mix, keyer alpha carried through later effects on opaque sources, EXR image-sequence export (display-referred or scene-linear, only when the exr encoder is available). Two MCP tools added (54 tools, G1..G192)
- [x] Wave 14 (US-1300..1321): selftest hygiene (the real multicam-apply and precompose dialogs are driven by closing the modal prompts), alpha carried through the geometric effects DisplacementMap / LensDistortion (LensDistortion edge transparent only for alpha media), storyline mode V1 media drop as insert and ripple edge trims / nudges (RippleOut double shift fixed), import refused on locked tracks with partner tracks created only after validation, FCPXML asset-clips with audio as linked A/V pairs and multicam audio sources / connected mc-clips / container durations, link groups of three or more clips moved together on cross-track drops, one clip-key remap per mutation that rekeys the five position-keyed sidecars (undo / redo included), render-queue audio trimmed to the job start, export of timelines whose visible V1 holds no clip.
- [x] Wave 15 (US-1400..1421): three-point insert / overwrite rekey the roto, matte, parent and sidecar carriers and are refused on a locked V1, time-remap / 3D-text / expression / wiggle dialog commits are undo points carried by value, saved-project render-queue and batch exports apply time-remap and freeze-frame (export exception), clip uids duplicated across video tracks are repaired on load, reversed (uniform speed) clips keep the source frames of both halves when split, cross-track drops onto a same-link-group member's position, media-bin / file drops on the new-track band in storyline mode, a File-menu relink entry with offline badges, the ImportHub preview PNG deleted when the import is refused, Round Corners / ZigZag / static Wiggle Path shape modifiers. No MCP tools added (54 tools, G1..G194)
- [x] Wave 16 (US-1500..1521): gate tally for mainwindow-lifecycle / media-relink, splitting a speed-ramped / time-remapped clip keeps the source span and duration of both halves (ramp halves outside 0.1x..10x become a time-remap curve; export exception), wiggle and expressions carried in the clip itself and applied by every export path (export exception), Wiggle Path varying over clip time, Offset Path / Pucker & Bloat / Twist shape modifiers, ImportHub-generated media stored next to the project and moved there on the first save, a render-queue "cancel selected job" button (the stop button renamed to stop the whole queue), batch tasks with an unresolved preset name fail, orient-towards-camera for 3D layers under the project camera, an FCPXML duplicate-uid repair gate. No MCP tools added (54 tools, G1..G194)
- [x] Wave 17 (US-1700..1722; US-1702 was deferred to Wave 18, and the camera-solve point cloud / 3D null candidate was moved to Wave 18 with only the US-1716 measurement shipped): gate tally for camera3d / shape-clip / render-in-place and the remaining suites (gate-count summary), 3D text and brush animations carried in the clip itself (copy / duplicate / paste keep them) and drawn by every export path (export exception), one shared clip layer source function for V1 / overlays / transition neighbours, file-based render jobs get the project camera and the real `audioLevel()` envelope (export exceptions; the first decode is started ahead for `audioLevel()` expressions), V1-only clips keep their effective opacity in the single-layer export shortcut (export exception), Light3D rotation order Rz*Ry*Rx (export exception), the preview camera time resolved through the owning video player, brush progress following the playhead in the preview, MCP `set_clip_wiggle` / `set_clip_expression` / `get_clip_dynamics` and `autoOrientToCamera` / `is3DLayer` on `set_clip_property`, a camera-solve geometry probe (measurement only). Three MCP tools added (57 tools, G1..G199)
- [x] Wave 18 (US-1800..1821): gate-count hygiene (log-to-rec709 declares 8, camtrack-ui scratch tally kept out of the suite tally, smart-render G100 renumbered to G28), smart render no longer stream-copies clips with brush strokes (bytes only, pixels identical; export exception), render-in-place bakes the brush into the material before the grade (export exception), the render-in-place temporary timeline uses the project fps, the special-composite preview stacks V1 frontmost when no matte is active and draws every clip's brush at the source stage while paused, a split keeps the 3D text and brush on both halves and shifts the right half's keyframes / effect windows / volume envelope / 3D text time by the cut, the reference-centred scaled camera-solve mapping with the analysed focal length written to the Fov track and a scale field in the tracker dialog (export exception for new feature solves), pre-projection world x/y for true-projection 3D layers (UI, timeline, graph editor, MCP), the solved point cloud carried in the camera JSON and shown as a preview-only overlay, a hidden 3D null created at a clicked solve point, a zero-byte-parent characterisation pin. No MCP tools added (57 tools, G1..G200); new suite solve-points (8 gates)
- [ ] CI green run + auto trigger on push / PR
- [x] `src/main.cpp` split refactor (selftest functions → `src/selftests/`, dispatcher → SelftestRegistry module)
- [ ] CRLF → LF wholesale normalization (per repo-root `.gitattributes`)
- [ ] Streaming pipeline real-authentication harness (YouTube / Vimeo / Twitch / X / Instagram currently no-op stub for CI)

### Wave 10 で据え置いた制限事項

Wave 18 の変更 (既存プロジェクトにも効くもの。書き出しの結果が変わるものは「書き出し例外」と明記):

- 書き出し例外 (バイトのみ、画素は同一): ブラシ (brushData) を持つクリップをスマートレンダーがストリームコピーせず、再エンコードします (SmartRender.cpp:450 / :478、拒否理由 "clip has brush strokes")。ブラシは brush_progress が 0 より大きいときだけ描かれ (TimelineFrameRenderer.cpp:1955-1962 の applyClipBrushSource)、brush_progress のキーフレームを持つクリップは従来からキーフレーム有りとしてストリームコピーされません。従って、従来ストリームコピーされていたのは進行度 0 (= 描かれない) のブラシだけで、この変更で絵が変わるクリップはなく、出力のバイト列だけが変わります (smart-render G29。US-1801)。
- 書き出し例外: ブラシを持つクリップの RenderInPlace (焼き込み) は、ブラシを roto の後、grade / vfx / fx の前の素材段階で焼き込み画像に描き、差し替えクリップにはブラシを残しません (brushData / brush_progress は焼く側のクリップが持ち、ハンドル分だけ brush_progress のキーをずらします。RenderInPlace.cpp:27-29)。従来は差し替えクリップにブラシを残し、grade 済みの絵の上にブラシが描かれていました。ブラシを持つクリップの焼き込みは、ダイアログや MCP で要求したコーデックによらず、roto と同じ ProRes 4444 (alpha 保持) の .mov になります (RenderInPlace.cpp:462-506。ブラシも roto も無い焼き込みは従来どおり要求したコーデック)。分解 (decompose) で元に戻ります。3D テキストは従来どおり差し替えクリップに残します (RenderInPlace.cpp:44。clip-sidecar G8 / G10 / G11。US-1803)。
- 書き出し例外: Wave 18 以降に「特徴点を追跡」→「カメラを解析」で適用したカメラは、新しい写像で書かれます。基準姿勢 (最初の有効な姿勢) に中心化し、position = S*c + f*(R3 - e3)、fov = f で、点群は w(p) = S*p' - f*e3 で写します (CameraSolver.cpp:302-373)。f は解析の焦点距離をキャンバス px に換算した値で、Fov トラックは定数 f に置き換えます (解析サイズとキャンバスが同じなら解析時のダイアログの fov と同じ値。判断 C2)。S (スケール、px/単位) の既定は f / median(z) を丸めない値で、点群が無ければ f です。トラッカーダイアログの「スケール (px/単位)」欄 (0.001〜1000000、小数 3 桁) で編集でき、編集した値は次の解析に使われます。ただし追跡元の変更や「特徴点を追跡」のやり直しで編集は破棄され、次の解析は既定値に戻ります (「カメラを解析」だけのやり直しでは保たれます)。編集していない既定値が欄の範囲外のときは、欄の上限 / 下限にクランプした値が使われます。基準フレームでは Z = 0 のレイヤーが従来の既定カメラと同じ位置に写り、他のフレームは解析カメラに従うため、そのプロジェクトの既存の 3D レイヤー / 3D テキストの書き出しも変わります。保存済みの解析 (camera JSON に `solveMapping` が無いもの) とプラナートラッカーの「3D カメラを解析」は旧写像のままで、再解析するまで変わりません (判断 C5 / C6)。写像は Camera3D の JSON の `solveMapping` (無いときはキーを書かない) に、点群は中心化後の solver 単位で `solvePoints` (空なら書かない、最大 sfm::kMaxBundlePoints 点) に保存され、プラナー解析と再解析で置き換わります (camtrack-ui G11 / G12、camsolve-probe G2-G5、solve-points G1-G3。US-1810 / US-1811 / US-1816)。
- カメラ解析の注意 (F21): 既定のカメラの fov (焦点距離) 60 px は 1920 幅では約 172 度の画角で、solver の前提としては極端です。本 Wave は幾何を設定された f に対して正しくするだけで、fov の妥当性を確認する UI はありません (Wave 19 候補)。カメラモーションダイアログの fov スピンは [10, 170]、位置 / 注視点のスピンは ±9999 にクランプされるため (CameraMotionDialog.cpp:206-212)、範囲外の fov のカメラや、大きな S で位置が ±9999 を超えるカメラをダイアログで開いて書き戻すとクランプされます。焦点距離が 0 以下、または S が非有限か 0 以下の解析は適用されず、カメラと undo は変わりません (ステータスバーに「3D カメラ解析を適用できませんでした（焦点距離またはスケールが不正です）」。MainWindow.cpp:16289-16297)。有効な姿勢が無い解析も適用されず、カメラと undo は変わりませんが、この場合はステータスバーに何も表示されません (MainWindow.cpp:16274-16277。通常は解析ダイアログ側で止まり、適用に進みません)。
- Wave 18 で書き出しの結果が変わるのは、この 4 項目 (ブラシ付きクリップのスマートレンダー (バイトのみ) / ブラシの焼き込み / 新しい解析写像とそのプロジェクトの既存 3D レイヤー / 音量エンベロープの 1 ms 以内の点 (US-1807 の欠陥修正)) だけです。書き出しは従来、1 ms 以内に並ぶ音量エンベロープの点を最小ゲインの 1 点にまとめていましたが、プレビューが読む点をすべて残すようになり (同一時刻 (1e-6 秒以内) に重なる点だけは先頭と末尾を残します)、速いランプや段差は、点の近くで分割した後も含めてプレビューと同じゲインで書き出されます (AudioExportMix.cpp:135 の sortedDedupedEnvelope。従来のまとめ方は自動ダッキングの結合 duckingMergedEnvelope (AudioExportMix.cpp:40) にだけ残ります)。render-fixture 15 ファイルと audio-fixture 23 ファイルは基点 f924c62 と IDENTICAL です (US-1820)。fixture には 1 ms 以内に並ぶエンベロープの点がありません (render-fixture の audio.json は volumeEnvelope が空、audio-fixture はエンベロープを置きません)。
- プレビューの特殊合成経路 (buildSpecialClipComposite) は、そのフレームにトラックマットが無いときは V1 を手前に並べ替え、書き出し (renderFrameAt) と同じ重なり順になります (MainWindow.cpp:8954-8966)。マットがあるフレームは従来どおり上位トラックが手前で、並べ替えはフレーム単位で決まります (マット元が終わったフレームでは V1 が手前)。Normal 以外のブレンドと調整レイヤーはこの経路の対象外のままです (special-composite G14-G16。プレビューだけの変更で、書き出しは変わりません。US-1804)。
- 一時停止中のプレビューで、ブラシを持つタイムラインは特殊合成経路を通り、全クリップのブラシを書き出しと同じ素材段階 (roto の後、grade / fx の前) で描き、選択中のクリップのブラシを重ねて描きません (TimelineFrameRenderer.cpp:1984 の timelineBrushWantsSpecialPath、MainWindow.cpp:8597 / :8997)。通る条件は、ブラシを持つクリップがあり、かつタイムラインに Normal 以外のブレンド / 調整クリップ / clip mask / 親子付け (有効時) / シェイプクリップ / シーケンス参照 / 無効クリップ / brush_progress 以外のキーフレーム / fitContain・fitCover / レイヤースタイル / トランジション (leadIn / trailOut) のどれも無いことです。判定はタイムライン全体のクリップで行い、再生ヘッドの時刻に無いクリップも数えます。再生中は従来どおり選択クリップのブラシだけです (判断 B6。書き出しは常に全クリップのブラシ。brush-export G11-G14。プレビューだけの変更で、書き出しは変わりません。US-1805)。
- 制限: 親子付け・シェイプ・シーケンス参照・無効クリップ・ブラシ以外のキーフレーム・fitContain / fitCover・レイヤースタイル・トランジションを含む Timeline は、一時停止プレビューで選択クリップのブラシのみ (書き出しは全ブラシ) です。Normal 以外のブレンド / 調整クリップ / clip mask を含む Timeline も同じです。再生中は常に選択クリップのブラシのみです。判定を再生ヘッド時刻のクリップに限る (または特殊合成を renderFrameAt と同等にする) のは Wave 19 候補です (US-1805)。
- 分割の新規則: クリップの分割で右片にも 3D テキストの設定とブラシのデータが残り (従来は左片だけ)、右片のクリップ局所時刻を持つ 4 種 (キーフレーム / string track、エフェクトの時間窓 startSec / endSec、音量エンベロープ volumeEnvelope、3D テキストのアニメ時刻 timeOffsetSec) を分割位置 L (タイムライン秒) だけずらすため、分割後も分割前と同じ見た目・同じ音で再生されます (Timeline.cpp:2080 の TimelineTrack::splitClipAt の 1 か所)。キーフレームは負の時刻の key を残し、エフェクトは endSec - L < 0 なら外し (== 0 は残す)、音量エンベロープは分割位置より前の点を捨て、分割位置から 1e-9 秒以内の点があればそれを時刻 0 の先頭点にし、無ければ分割位置の補間値 (プレビューと同じ audio::evaluateVolumeEnvelope) を先頭点に挿入し (Timeline.cpp:2171-2195、kEnvelopeCutEps = 1e-9)、3D テキストは text3DConfig の timeOffsetSec に L を足します (Text3DLayer.cpp:367 で描画時刻に足す。JSON の非有限値は 0)。timeOffsetSec は 3D テキストのダイアログで編集しても保たれ、ダイアログ内のプレビューは 0 から再生されます (Text3DExtrusionDialog.cpp:315 / :438-440、プレビューは :456 の buildLayer が timeOffsetSec を設定しない)。同じ規則が分割の全経路 (クリップ分割 / 全トラック分割 / 再生ヘッドで分割 / 三点編集の上書き / 範囲リップル削除 / フリーズフレーム) に効きます。フリーズフレームの右片では、ずらしたキーフレームが止まった映像の上で動き続けます。テキスト overlay はタイムライン秒で評価されるので元からずれず、本 Wave では触りません。対象外: wiggle / 式 (右片で位相と time が 0 から始まります。Wave 19 候補)。影響なし: スタビライザーと mask tracking (ソース秒)。音声トラックのタイムリマップは分割で捨てられ (一様経路は Timeline.cpp:2121-2123、消費ソース経路は :1964 で消します)、映像のタイムリマップは右片の曲線を切った位置から作り直します (Wave 16 から。Timeline.cpp:1925 の splitClipByConsumedSource、曲線の切り出しは :1973)。音声の fade は leadIn / trailOut のトランジションとして持つため、分割がそれをリセットします (従来どおり。Timeline.cpp:2131)。保存済みのプロジェクトは変わらず、新規の分割操作の結果 (絵と音) だけが変わります (clip-sidecar G5。US-1807)。
- 制限: 音量エンベロープの gain が 2 を超える点は、プレビュー (上限 4。AudioMixer.cpp:932) と書き出し (上限 2。AudioExportMix.cpp:51 / :147) で従来から違うため、分割の前後で書き出しの gain がずれることがあります (Wave 19 候補。US-1807)。
- 3D レイヤーに射影前のワールド X / Y (キャンバス px、±10000) を足しました (モーションセクションの「ワールド X」「ワールド Y」、タイムラインのクリップモーション、グラフエディタの「モーション: ワールド X / Y」、エフェクトコントロール)。プロジェクトカメラ (真の透視投影) のときだけ効き (ツールチップ「プロジェクトカメラ (trueProjection) でのみ効く」)、従来の位置 X / Y (videoDx / videoDy) は射影後の 2D のままです。0 のときは JSON に書かず、既存プロジェクトの出力は同一です。真の透視投影 OFF (legacy) の 3D では無視されます (Camera3D.cpp:325 の短絡。判断 C9。Wave 19 候補)。3D レイヤーでないクリップのワールド X / Y は 0 で、エフェクトの貼り付けでは 0 に戻り、属性の貼り付けでは引き継がれます。ワールド X / Y のキーフレームは位置 Z / 回転と同じく UI (グラフエディタ) だけで、描画では評価されません (判断 C8。Wave 19 候補) (camera3d G12-G15、light3d G20、keyframe-anim-parity G8。US-1813 / US-1814)。
- MCP: `set_clip_property` に `worldX` / `worldY` (±10000、3D レイヤーのみ、プロジェクトカメラ (trueProjection) でのみ効く。is3DLayer=false のクリップは拒否) を足し、`get_timeline` の各クリップに任意の `worldX` / `worldY` (0 のときは返さない) を返します。ツール数 57 は不変、既存ツールの required は不変、set は undo 1 段です (mcp G200。US-1815)。
- 解析点群の表示 (表示メニュー「解析点群を表示」、既定 OFF): 特徴点のカメラ解析で保存した点群をプロジェクトカメラの再生ヘッド時刻の姿勢で射影し、プレビューに十字で重ねます (プレビュー専用。GLPreview.cpp:2875。アップロードされるフレームと書き出しには乗りません)。ピンホールの後ろの点は描きません。シーク / スクラブ / 再生 / カメラ同期で追従します。プラナー解析と旧 solve には点群が無いので何も出ません (solve-points G4-G6。US-1817)。
- 3D ヌルの作成: 点群を表示中にプレビューの十字 (8 px 以内、最も近い点) を左クリックすると、その解析点のワールド X / Y と位置 Z を持つ非表示の 3D ヌルオブジェクト (opacity 0、visible false) を新しい映像トラックに、解析の時間範囲 (`solveMapping` の startSec / sampleCount / fps。古い解析ではキーフレームから推定) で作ります (MainWindow.cpp:15394 の createNullAtSolvePoint、undo 1 段「解析点にヌルオブジェクトを作成」)。座標が ±10000 を超える点は拒否してステータスバーに表示します。ヌルに親子付けした子はヌルの 3D 射影位置には追従しません (親子付けは 2D の当たり判定で整列するため。Wave 19 候補) (solve-points G7-G8。US-1818)。
- 修正: RenderInPlace の焼き込みの一時タイムラインがプロジェクトの fps を使うようになり (RenderInPlace.cpp:442)、素材の fps を読めない roto クリップのマスクのフレーム番号が 30 fps 固定ではなくなりました (render-in-place G12。US-1802)。実素材では probe が fps を返すため、出力は変わりません。
- 修正: 0 バイトの親素材を持つ親付きクリップの挙動を実測して固定しました (video-player-parent G11。US-1812)。「整列は親ありとして計算し、描画は親を省く」という従来の記述は誤りで、正しくは下の制限事項の親付きクリップの項を参照してください。
- 修正: 親素材が 0 バイトでも parentLayerAt は hit / decodable を返しますが、VideoPlayer は親を採用しません (parentHit = 0、parentHitDecodable = 0、parentSourceIndex = -1)。この食い違いを実デコード結果に合わせるのは Wave 19 候補です。
- プロジェクトカメラの焼き込み (trueProjection) のプレビューは、renderFrameAt から全クリップのブラシを既に受け取っていることを固定しました (brush-export G10。挙動の変更はありません。US-1806)。
- 候補 1 (c) (3D テキストの書き出しキャッシュの最適化) は、Wave 17 の計測が 1 フレーム 12 ms (< 100 ms) だったので対応せず閉じました。
- selftest: log-to-rec709 は 8 gate を申告し (従来 7)、camtrack-ui は検査用の scratch tally を suite の集計に報告せず (従来 ran 11 / declared 11、US-1800 で 10)、smart-render の caption 検査は G100 から G28 になりました (US-1800)。新 suite solve-points (8 gate)。gate 数が増えた suite: smart-render 28 → 29、camtrack-ui 10 → 12、render-in-place 11 → 12、clip-sidecar 9 → 11、special-composite 13 → 16、brush-export 9 → 14、camsolve-probe 1 → 5、video-player-parent 10 → 11、camera3d 11 → 15、light3d 19 → 20、keyframe-anim-parity 7 → 8、mcp 199 → 200。Registry は 279 本です。累積レビュー (US-1820) は自動ダッキングのエンベロープ結合を共有関数 mergeDuckingEnvelope (AudioExportMix.cpp:161) に移しました (出力同一。auto-ducking G16)。
- Wave 19 へ送ったもの: Echo の過去フレームへの 3D テキスト / ブラシ (候補 1 (g)、判断 B4)、MCP の 3D テキスト / ブラシのツール (候補 1 (d)、判断 B5)、render-in-place の nativeVideoSize 失敗時の outputSize フォールバック (US-1702 案 (a))、再生中のブラシの全クリップ描画 (判断 B6)、特殊合成経路のブレンド / 調整レイヤー、プラナー経路の幾何 (旧写像のまま)、ヌルの子の 3D 追従 (判断 C4)、3D keyframe (位置 Z / 回転 / ワールド X / Y) の描画評価 (判断 C8)、legacy (真の透視投影 OFF) でのワールド X / Y (判断 C9)、legacy 3D のプレビューと書き出しの不一致 (書き出しは legacy 3D を射影しない)、render-in-place がタイムライン 0 以外で始まるクリップのテキスト overlay を落とす件、ダイアログの fov 確認 UI、wiggle / 式のクリップ単位の局所時刻オフセット、gain > 2 のエンベロープのクランプ統一、ブラシの特殊合成の適格判定の局所化、0 バイト親の parentLayerAt の decodable 判定、auto-ducking の GateTally。

Wave 17 の変更 (既存プロジェクトにも効くもの。書き出しの結果が変わるものは「書き出し例外」と明記):

- 書き出し例外: 3D テキストを持つクリップの書き出しに 3D テキストが乗ります (ライブ Timeline の書き出し (MCP の export_video を含む)、レンダーキューのファイル指定、バッチ書き出しの File)。従来は 3D テキストはプレビュー (特殊合成 / GLPreview) だけに乗り、書き出しには乗りませんでした。3D テキストを設定していた既存プロジェクトの書き出し結果が変わります。重なりトランジションの相手側のクリップでも描かれます。スマートレンダーは 3D テキストを持つクリップをストリームコピーしません (US-1707 / US-1720)。ブラシだけを持つクリップはスマートレンダーの判定で弾かれず、ストリームコピーされていました (Wave 18 で変更: SmartRender.cpp:450 / :478 がブラシを持つクリップも弾きます。US-1801)。 (Wave 18 で訂正) ブラシは brush_progress が 0 より大きいときだけ描かれ、brush_progress のキーフレームを持つクリップは元からキーフレーム有りとしてストリームコピーされないため、ストリームコピーで失われるブラシはありませんでした (画素は同一。Wave 18 の変更を参照)。
- 3D テキストの書き出しでの合成点は、clip mask を掛けた後、変換 (位置 / スケール / 回転 / 不透明度) の前です (利用者判断 M)。プレビューの特殊合成経路は clip mask を一切掛けないため、mask を設定したクリップでは書き出しだけが素材を mask で切ります (3D テキストはどちらでも mask で切られません)。プレビューの 3D テキストが proxy で描かれるのは、押し出し (extrusion) を有効にした 3D テキストだけです (Text3DLayer.cpp:390)。proxy になるのはフレームの長辺がしきい値を超えたときで (Text3DLayer.cpp:394)、しきい値は QSettings の preview/text3dProxyThreshold (既定 1280。MainWindow.cpp:8576) です。proxy は縦横比を保ったまま min(1/2, 640 / 長辺) の倍率で縮小されるため、長辺は 640 以下になり (Text3DLayer.cpp:382)、16:9 で長辺が 1280 を超えるフレーム (例: 1920 x 1080 や 3840 x 2160) では 640 x 360 になります。書き出しはフル解像度で描くため、プレビューと書き出しが画素まで一致するのは、押し出しを無効にした 3D テキストか、長辺がしきい値以下のフレームだけです。Echo の過去フレームには 3D テキストもブラシも入りません (US-1707 / US-1708)。プレビューの特殊合成経路 (buildSpecialClipComposite) は上位のトラックを手前に重ねます (zOrder = トラック番号。MainWindow.cpp:8875、MainWindow.cpp:8918 で昇順に並べる)。書き出しは V1 を手前にする V1-wins の重なり順です (TimelineFrameRenderer.cpp:3078)。このため、複数のトラックで不透明なレイヤーが重なる所ではプレビューと書き出しが異なります (3D テキストの有無によらず。US-1707 の run2 で maxDiff 117 を計測)。G13 の一致検証が確かめるのは、重ならない配置だけです。 (Wave 18 で変更) マットが無いフレームでは V1 を手前に並べ替え、書き出しと同じ重なり順になります (MainWindow.cpp:8954-8966。zOrder = トラック番号は MainWindow.cpp:8875 / :8918。US-1804。Wave 18 の変更を参照)。
- 書き出し例外: ブラシ (ブラシアニメーション) を持つクリップの書き出しにブラシが乗ります (全クリップ分。経路は 3D テキストと同じ)。従来はプレビューだけに乗り、書き出しには乗りませんでした。ブラシを設定していた既存プロジェクトの書き出し結果が変わります。ブラシは roto の後、grade / vfx / fx の前の素材段階で描かれるため、grade / fx を持つクリップではブラシにも grade / fx が掛かります。書き出しのブラシの進行度 (brush_progress) はクリップ時間のキーフレームで決まり、プレビューの進行度も再生位置に追従するようになりました (従来は時刻 0 の値のまま。プレビューの挙動変更で、この部分は書き出しの結果を変えません。US-1708)。
- プレビューのブラシは、通常のプレビューでは選択中のクリップ 1 本だけで、合成済みキャンバス (特殊合成) では全トラックの上に描かれます (HEAD の制限。書き出しは全クリップのブラシを素材段階で描くため異なります)。 (Wave 18 で変更) 一時停止中は、条件を満たすタイムラインで特殊合成経路が全クリップのブラシを素材段階で描きます (条件と制限は Wave 18 の変更を参照。再生中は従来どおり選択クリップのみ。US-1805)。プロジェクトカメラの焼き込み (trueProjection) では、プレビューも書き出しと同じレンダラー (tlrender::renderFrameAt、GLPreview.cpp:2153) で全クリップのブラシを素材段階で描き、選択中のクリップのブラシを重ねて描くことはしません (GLPreview.cpp:1236。US-1708)。シェーダの grade と CPU の grade の画素一致は、平坦な入力での水準の確認 (GradeKeyframeSelftest / offset_wheel_selftest) だけで、画素ごとの一致は確認していません。
- RenderInPlace は 3D テキストを焼き込み画像に含めず、差し替えクリップに残してレンダラーが描きます (text3DConfig は差し替えクリップへコピー。RenderInPlace.cpp:44。US-1720)。Wave 17 まではブラシも同様に差し替えクリップに残し (brush_progress のキーフレームも差し替え側)、grade / fx を持つクリップでは焼き込み後に grade / fx が焼き込まれた絵の上にブラシが描かれていました。 (Wave 18 で変更) ブラシは焼き込み素材に grade / fx の前で焼かれ、差し替えクリップには残りません (RenderInPlace.cpp:27-29。ブラシ付きの焼き込みは ProRes 4444 の .mov。US-1803。Wave 18 の変更を参照)。
- 3D テキストの設定とブラシのデータはクリップ自身が持つようになり (Wave 16 の wiggle / 式と同じ形)、コピー・複製・貼り付けで複製先に引き継がれます (従来は MainWindow が位置で持つサイドカーでした)。ブレードなど分割の右片には付きません (左片に残ります)。 (Wave 18 で変更) 分割の右片にも付き、右片のアニメ時刻は分割位置ぶんずらされます (US-1807。Wave 18 の変更を参照)。設定の確定は undo 点 1 つです。旧形式のプロジェクト節 (text3DClipEntries / brushAnimations) は新ビルドが読み込み時にクリップへ移し、保存では書きません。非アクティブなシーケンスの旧節は移行されず、読み込みで失われます (wiggle の旧節と同じ。US-1706)。
- 旧ビルドで Wave 17 のプロジェクトを開いて再保存すると、クリップの text3D / brush のキーが失われます (旧ビルドでも開けますが、新ビルドで保存した 3D テキストの設定とブラシのアニメーションは読み込まれず、再保存すると消えます)。
- 書き出し例外 (欠陥修正): ファイルから書き出すジョブ (レンダーキューのファイル指定 / バッチの File) がプロジェクトのカメラ (真の透視投影、カメラへ自動的に向ける設定、カメラ解析のアニメーション) を使うようになり、ライブの書き出しと同じ絵になります。従来はカメラが渡らず、カメラを使うプロジェクトの絵がライブの書き出しと異なりました (US-1715)。
- 書き出し例外: ファイルを指定したジョブ (レンダーキューのファイル指定 / バッチの File) の式の audioLevel() が、0 ではなくリンク音声の実際のレベルで評価されます (Wave 16 の利用者判断 D の制限の解消。US-1709)。入れ子シーケンスの中のクリップの式の audioLevel() は従来どおり 0 です。audioLevel() を含む式を持つクリップは、リンクグループの最初の音声だけを、音声エンベロープの公開時に背景で先にデコードします (式を使わないプロジェクトでは新たなデコードは起きません。US-1710)。この先行デコードが走査するのはトップレベル (入れ子でない) の全映像トラック (V1, V2, ...) のクリップだけで、入れ子シーケンスの中のクリップは走査しません (Timeline.cpp:7958 は m_videoTracks を回します)。また先にデコードする音声ファイルは最大 16 本で (Timeline.cpp:7978、warmPaths.size() < 16)、それを超えるリンク音声は初回の使用時にデコードされます。
- 書き出し例外 (欠陥修正): V1 だけにあるクリップ (上位トラックが空) の実効不透明度 (静的 / キーフレーム / 式) が 1 未満のフレームの書き出しが、そのクリップを V2 に置いた場合と同じ半透明になります (従来は単一レイヤーの近道で不透明度が捨てられ、不透明でした。US-1703)。
- 書き出し例外 (欠陥修正): ライトを有効にした 3D レイヤーで、実効回転 (カメラへ自動的に向けた後を含む) が 回転 Z ≠ 0 かつ (回転 X ≠ 0 または 回転 Y ≠ 0) のとき、陰影の回転順が射影 (真の透視投影) と同じ Rz*Ry*Rx になります (プレビューと書き出し。US-1705)。既定の経路 (真の透視投影 OFF) の射影は従来どおり Z → X → Y の順なので、既定の経路では陰影と射影の回転順が食い違います (従来は真の透視投影の経路が食い違っていました。HEAD の制限。利用者判断 I)。
- Wave 17 で書き出しの結果が変わるのは、この 6 項目 (3D テキスト / ブラシ / ファイルジョブのカメラ / ファイルジョブの audioLevel() / V1 単独の不透明度 / ライトの回転順) だけです。
- プレビューのカメラ時刻は、GLPreview の直接の親ウィジェットではなく、所有する VideoPlayer の再生位置から決まります。実際のウィジェット階層では GLPreview の親は表示切り替え用のスタックなので、従来はカメラのアニメーションが時刻 0 の値のまま固定され、真の透視投影のカメラ描画の分岐もライブプレビューでは実行されませんでした (挙動変更で、書き出しの結果は変わりません。US-1704)。
- MCP に set_clip_wiggle / set_clip_expression / get_clip_dynamics の 3 ツールを足し (57 ツール)、set_clip_property に autoOrientToCamera / is3DLayer を足しました。get_timeline の各クリップには任意の autoOrientToCamera が返ります (既存ツールの required は変わりません。set は undo 1 段。mcp selftest は G1..G199。US-1711)。
- プロジェクト比較 (project-diff) には 3D テキストとブラシの差分は出ません (Wave 18 でも未対応)。
- トラックの mute / solo は書き出しに乗ります (W13-EXPORT-TRACK-MUTE は調査の結果、対応不要として終了)。書き出しのミックスは AudioExportMix.cpp:299 の entry.audioMuted と :301 の trackAudible (solo の規則は :261-267) で決まり、ミキサーストリップの mute / solo ボタンは Timeline のトラックを直接動かす (AudioMixerDock.cpp:198-203) ので UI からの操作も乗ります (audio-fixture G19 / G20)。乗らないのは AudioMixer のトラック状態の muted で、呼び手は VideoPlayer::setMuted (プレイヤー全体のミュート) だけです。これは書き出しに乗らないのが正しく、audio-fixture G18 が固定しています。
- Wave 18 へ送ったもの: RenderInPlace の焼き込みで素材の fps を読めない roto クリップのマスクのフレーム番号 (30 fps のまま。US-1702 は見送り)、カメラ解析の点群の表示と 3D ヌルの配置 (未実装。US-1716 はカメラ解析のカメラとプロジェクトカメラの幾何を合成シーンで計測しただけで、機能の追加はありません)、親素材が存在するがデコードできない場合の親の採否判定 (W11-PARENT-DECODABLE-GATE)。 (Wave 18 の結果) fps は US-1802 で修正、点群の表示と 3D ヌルは US-1817 / US-1818 で実装、0 バイト親の採否は US-1812 が実測を固定し、制限事項の親付きクリップの項を訂正しました (Wave 18 の変更を参照)。

Wave 16 の変更 (既存プロジェクトにも効くもの。書き出しの結果が変わるものは「書き出し例外」と明記):

- 書き出し例外 (欠陥修正): 速度ランプまたはタイムリマップを持つクリップを分割したあとの書き出しが変わります。分割した各片は実際に消費する素材区間を in / out に持ち、速度を片ごとに計算し直すため、両片の素材と長さが元のクリップと連続します (従来の分割は片が素材フレームを保ちませんでした)。書き出しは速度ランプの形を読まないため、ランプを保つ片は片の平均速度で書き出されます (プレビューはランプに従います)。速度が 0.1x〜10x の範囲に収まらない ramp 片は速度曲線に変換され、曲線で書き出されます。ランプが素材の端でクランプされる静止区間の片は、その位置のフレームを保持します (US-1501)。
- 書き出し例外 (音声): 音声トラックの片は速度曲線を持ちません。順方向の音声片は 1x (クリップの speed のまま) で再生されます。ramp の末尾の静止区間や、ランプが素材の端に達する時刻 (t*) 付近で分割した片は、映像の平均速度には合いません (従来からの制限)。reversed の音声片は片の平均速度で再生され、0.1x 未満はプレビューでクランプされます。atempo OFF で分割した片の ramp は原点が揃いません (従来と同じ)。
- 書き出し例外: wiggle と式を持つクリップの書き出しに、wiggle と式が乗ります (ライブ Timeline の書き出し (MCP の export_video を含む)、レンダーキューのファイル指定、バッチ書き出しの File)。従来はプレビュー (特殊合成 / GLPreview) だけに乗り、書き出しには乗りませんでした。wiggle または式を設定していた既存プロジェクトの書き出し結果が変わります。ファイルを指定したジョブ (レンダーキューのファイル指定 / バッチの File) の式の audioLevel() は Wave 16 では音声レベル 0 で評価されました (利用者判断 D。US-1504 / US-1505)。Wave 17 から実際の音声レベルで評価されます (US-1709)。
- 書き出し例外 (欠陥修正): ファイルから書き出すレンダーキューのジョブで、素材の fps を読めない roto クリップ (シェイプ / 生成クリップなどで、タイムリマップの sourceFps も無いもの) のマスクのフレーム番号が、30 fps ではなくプロジェクトの fps で決まります (プレビューと同じ。プロジェクトが 30 fps のとき、素材の fps が読めるクリップは変わりません。US-1520)。Wave 16 で書き出しの結果が変わるのは、この 4 項目だけです。
- 速度ランプまたはタイムリマップを持つクリップの分割は、両片が素材区間と長さを保ちます (US-1501)。ブレード / 全トラックブレード / 範囲上書き / 3 点上書き / 範囲削除 / 接続の分割など、分割を伴う操作はすべて対象で、分割は常に成功します。ランプを保てない片 (速度が 0.1x〜10x の範囲外) は速度曲線に変換されるため、その片は速度ランプのダイアログで編集できません (曲線になっています)。
- 分割後の音声の書き出しは、分割前と連続しません (右片は in + R(L) から始まります。R は速度ランプを通した素材位置、L は分割位置。従来は一様速度の規則で in + L × speed でした)。t* 付近で分割すると、音声片はプレビューでも 1x になります。環境変数 VEDITOR_AUDIO_ATEMPO=1 (開発用) を設定すると atempo OFF のクリップでも音声がランプを読むため、この規則の例外になります。
- wiggle と式はクリップ自身が持つようになり、コピー・複製・分割で複製先と分割後の両片に乗ります (従来は MainWindow が位置で持つサイドカーで、ブレードの右片には付きませんでした。US-1504 / US-1505)。サーフェスツールは wiggle で揺れません (整列と親子付けの書き戻しも、揺れを焼き込まずキーフレームだけを見ます)。入れ子シーケンスの中のクリップの式の audioLevel() は 0 です。式の audioLevel() は、リンクした音声の編集 (差し替え / ずらし / 速度変更 / リンク解除) のあと、音声クリップ列の再構築 1 回分遅れて追従します。書き出し中のプレビューでも、別ファイルの音声キャッシュはデコード中のファイルを待たされません (デコードはロックの外で行います)。
- ImportHub で生成した素材は、未保存のうちは一時フォルダに置かれ、最初の保存でプロジェクトの隣の <プロジェクト名>_assets/importhub/ へ移されてクリップのパスが書き換わります (保存のステータス表示に「取り込み素材 N 件をプロジェクトの隣へ移動しました」が添えられ、保存で undo が 1 段増えます。保存に失敗したときはその 1 段を戻します)。保存済みのプロジェクトで生成した素材は最初からその場所に置かれるため、素材の置き場は保存の前後と保存先ごとに混在しえます (US-1506)。
- レンダーキューダイアログに「選択ジョブを中止」を足し、既存の「停止」ボタンを「キュー全体を停止」に改名しました。待機中のジョブを中止すると一覧から削除され、実行中のジョブは中止 (Cancelled) になります。「キュー全体を停止」は従来どおり共有キュー全体を止め、待機中のジョブは一覧に残ります (US-1507)。
- バッチ書き出しで、保存済みプリセットとして解決できないプリセット名のタスクは、実行時に失敗 (Failed、「プリセット '名前' が見つかりません」) になり、出力は作られず、後続のタスクは続行します。組み込みのプリセット名と存在するユーザープリセットは従来どおりです。キューに積んだ時点で解決できなかった名前 (と取り込み値を持たずに復元したタスク) は実行時にもう一度引き直し、それでも見つからなければ失敗します。積んだ時点で解決できたタスクは、あとでユーザープリセットを削除しても取り込んだ値のまま書き出されます (US-1510)。
- シェイプモディファイアに Offset Path (パスのオフセット) / Pucker & Bloat (膨張・収縮) / Twist (ひねり) を足し、Wiggle Path に「毎秒のウィグル数」を足しました。いずれも既定は無効 (毎秒のウィグル数は 0) で、既存のプロジェクトの JSON と描画は変わりません (有効のときだけ offsetPath / puckerBloat / twist のキーを保存し、wigglesPerSecond は 0 以外のときだけ保存します)。適用順は Round Corners → ZigZag → Wiggle Path → Pucker & Bloat → Twist → Offset Path → Trim → Repeater です。Offset Path は Qt のパス演算で自己交差を消すため、負のオフセットで細い部分は消えます。Wiggle Path は毎秒のウィグル数が 0 のあいだ静的で、0 より大きいとクリップの時間で変化し、時間変化中の振幅は最大で 2 倍になります (US-1502 / US-1503)。
- 3D レイヤーをプロジェクトカメラへ向ける設定を足しました (クリップを右クリック → 「カメラへ自動的に向ける」。3D レイヤーでだけ有効で、既定は OFF)。効くのはカメラの「真の透視投影」が ON のときだけで、回転 Z は保たれ、ライティングの法線も向いた後の向きになります。プレビューと書き出しは同じ向きで描かれ、切り替えは undo 点 1 つです (US-1509)。
- 累積レビューの修正 (US-1520): 分割・凍結したタイムリマップのクリップの曲線は、undo と位置 key の張り替えをまたいでも、そのクリップ自身の sidecar の項目に保たれます。レンダーキューのジョブはプロジェクトの fps をクリップの式に渡します。

Wave 15 の変更 (既存プロジェクトにも効くもの。書き出しの結果が変わるものは「書き出し例外」と明記):

- 書き出し例外 (欠陥修正): 保存済みプロジェクトから書き出すレンダーキュー (ファイルを指定したジョブ) とバッチ書き出しの File が、保存されたタイムリマップとフリーズフレームを反映します。従来は保存済みプロジェクトを読み込んだ書き出しからどちらも消え、素材が元の速度とフレームのまま書き出されていました。タイムリマップまたはフリーズフレームを持つ既存プロジェクトを保存済みファイル経由で書き出した結果が変わります (利用者判断 F。US-1402)。Wave 15 で書き出しの結果が変わるのはこの項目だけです。
- ソースモニターの 3 点 insert / overwrite は、roto / マット / 親子付けと位置 key のサイドカーを 1 回の張り替えで追従させます (undo / redo の各段でも追従)。V1 がロック中なら拒否してステータスバーに「ロック中のトラックには配置できません」を出し、成功の表示は出しません (挙動変更。US-1400)。
- タイムリマップ / 3D テキスト / 式 / wiggle のダイアログ確定は 1 回が undo 点 1 つ (Ctrl+Z 1 回) になり、4 種は値ごと undo / redo に乗ります。内容が変わらない確定は undo 点を積みません。ブラシのアニメーションは従来どおりです (US-1401)。
- 読み込み時に、映像トラックをまたいで重複するクリップの uid を無言で振り直します (通知は出ません。手編集した、または旧形式のプロジェクトファイルだけで起きていた状態の修復です。US-1405)。
- reversed (一様速度) のクリップの分割は、両片が素材フレームを保ちます。ブレード / 全トラックブレード / 範囲上書き / 3 点上書き / 範囲削除 / 接続の分割など、reversed クリップの分割を伴う操作はすべて対象です。reversed クリップのフリーズフレームは再生ヘッド位置の素材を保持します (従来は元の末尾。挙動変更。US-1406)。速度ランプまたはタイムリマップを持つクリップの分割は、Wave 16 から両片が素材区間と長さを保ちます (Wave 16 の変更を参照。US-1501)。
- リンクグループが 3 本以上のクリップを、同じグループのメンバーが居る位置へ別トラックから drop したとき、そのメンバーも一緒に動かして受け付けます (carrier は uid で 1 回張り替えます)。いずれかのメンバーの行き先が drop 先のトラックと同じになる、または行き先がロック中のトラックのときは、従来どおり拒否します。drop 先の候補表示も受理される位置と一致します (US-1403 / US-1420)。
- ストーリーライン ON では、新トラックの帯がメディアビン / ファイルの drop も受け、トラックを作ってそこへ取り込みます (V の帯は映像ファイル、A の帯は音声を持つファイルを音声のみとして)。OFF では従来どおりです (US-1407)。
- ファイルメニューの「メディアを再リンク…」から、プロジェクトを開いたあとでも再リンクのダイアログを開けます (ショートカットなし。オフラインのメディアが無いときはステータスバーに「オフラインのメディアはありません」を出します)。タイムライン上のオフラインの映像 / 音声クリップには左上にバッジが付き (幅 40px 以下のクリップはバッジを省き、名前末尾の (オフライン) だけ)、名前の末尾に (オフライン) と付きます。overlay 画像 / パーティクル / 入れ子シーケンスの中身は再リンクの件数には入りますが、バッジは付きません。オフラインの再判定は、読み込み / 再リンク / 取り込み / メニュー / undo・redo / シーケンス切替の 6 つの契機で行い、周期タイマーは使いません (US-1408)。
- ImportHub の画像 / メッシュ / EXR 連番の取り込みが拒否されたとき (ロック中のトラックなど)、プレビュー用に書き出した PNG ファイルを削除してステータスバーに理由を出します (US-1404)。
- シェイプモディファイアに Round Corners / ZigZag / Wiggle Path を足しました。Wiggle Path は Wave 15 では静的でした (Wave 16 で時間変化に対応。US-1502)。3 つとも既定は無効で、既存のプロジェクトの JSON と描画は変わりません (有効のときだけ roundCorners / zigZag / wigglePath のキーを保存します)。適用順は Round Corners → ZigZag → Wiggle Path → Trim → Repeater です (US-1409)。

Wave 14 の変更 (既存プロジェクトにも効くもの。書き出しの結果が変わるものは「書き出し例外」と明記):

- 書き出し例外 (欠陥修正): DisplacementMap / LensDistortion を含む幾何 FX が alpha を運びます。変位の前に alpha が 255 未満の場所 (alpha 付き素材、roto の外側、有効なキーヤー (クロマ / ルマ / カラー) のあと) があれば、その alpha が絵と一緒に動きます (従来は RGB だけを返し、alpha が 255 に戻っていました)。LensDistortion の画面外の縁は、クリップのメディアが alpha を持つとき (ProRes 4444 / QTRLE / PNG など。フレームの内容ではなくクリップ単位のメタデータで判定) だけ透明になり、不透明な素材は従来の黒です (利用者判断 14 / 18)。メッシュワープとパペットも同じ経路 (applyClipMeshWarp) で alpha を運びます (従来から。Wave 14 で gate に固定)。alpha 付き素材を使う既存プロジェクトの書き出し結果が変わります。キーヤーも roto も前に無い不透明な素材の書き出しは変わりません (US-1301)。
- 書き出し例外 (欠陥修正): V1 が表示中でクリップが 1 本も無く、上位トラックにクリップがあるタイムラインは、透明な下地の上に上位トラックを合成して書き出せます (従来は GUI が事前に拒否するか、描画時に失敗していました)。タイムライン全体にクリップが 1 本も無い (全フレームが空の) 場合は従来どおり書き出しません。V1 にクリップがあって隙間のある区間の絵は変わりません (US-1312。利用者判断 3)。
- 書き出し例外 (欠陥修正): レンダーキューのジョブの範囲が 0 秒より後から始まり、マーク範囲が無いとき、音声もジョブの開始位置から切り出します (従来は音声がタイムラインの 0 秒から始まっていました) (US-1311)。
- 素材の取り込み (GUI の取り込み / drop、MCP の `import_media`) はロック中のトラックに置きません。トラックを明示してロック中なら「ロック中のトラックには配置できません」で拒否し (GUI はステータスバーに理由を出します)、自動配置はロック中のトラックを飛ばして次の空いた未ロックのトラック (無ければ新規) に置きます。配置設定が `AppendToFirstTrack` で V1 / A1 がロック中のときは拒否してステータスバーに理由を出します。検証に通ってからトラックを作るため、拒否したときに空のトラックや undo 点は残りません。リンクグループの番号は使用中の番号を飛ばして採番します (US-1306)。
- 主ストーリーライン: ON のとき、V1 へのメディアの drop は drop 位置への insert になり、V1 の端トリムと nudge も ripple します。V1 と他のトラックにまたがる nudge は拒否されます (選択した V1 クリップのリンク相手は対象外)。MCP の編集ツールは ON でも ripple しません。OFF のときの W と MCP の `trim_clip` は、下流のクリップがトリム量 d だけ動きます (従来は 2d 動いていました)。詳細は下の「主ストーリーラインと接続クリップ」 (US-1304 / US-1305)。
- リンクグループが 3 本以上のクリップのクロストラック drop で、全メンバーが一緒に移ります (2 クリップの AV リンクの動作は変わりません)。roto / マット / 親子付けは 1 回の張り替えで追従します (US-1313)。
- クリップの key が変わる編集のあと、位置 key のサイドカー 5 種とブラシのアニメーションが、roto と同じ張り替え (編集 1 回につき 1 回) に乗り、undo / redo の各段でも追従します。ブレードの右片に 3D テキスト / ブラシは付きません (Wave 17 から 3D テキストの設定とブラシのデータはクリップ自身が持ち、位置の張り替えの対象ではなく、コピー・複製・貼り付けで引き継がれます。US-1706。Wave 18 から分割の右片にも付き、右片のキーフレーム / エフェクト窓 / 音量エンベロープ / 3D テキストの時刻は分割位置ぶんずらされます。US-1807。タイムリマップは Wave 16 からクリップ自身のカーブが切った位置で 2 つに分けられ、両片で再生されます。US-1501 / US-1520。式と wiggle は Wave 16 からクリップ自身が持ち、分割後の両片に乗ります。US-1504 / US-1505)。パーティクルは filePath key です (US-1302 / US-1303)。
- FCPXML の読み込み: 音声付きの asset-clip は映像と音声のリンク対になります (US-1308)。マルチカムの音声 source / `audioStart` / 接続された mc-clip を取り込み、アングルが範囲より短いときは警告を出します (US-1309)。コンテナの `duration` を守り、0 秒より前に出る接続コンテナを 0 秒で切り詰めます (US-1310)。詳細は下の「FCPXML 読み込みの対応表」。実際の Final Cut Pro が書き出した .fcpxml での確認はどの行もまだ行っていません。
- 累積レビューの修正: 1 回の取り込みが clip key の張り替えを 2 回出さないよう 1 回に束ね、張り替え後にライブブラシのプレビューを選択中のクリップへ結び直します。クリップが 1 本も無いタイムラインではフレームを作らず、全フレームが空の書き出しは従来どおり拒否します (US-1320)。

Wave 13 の変更 (既存プロジェクトにも効くもの。書き出しの結果が変わるものは「書き出し例外」と明記):

- 書き出し例外: トラックの solo が書き出しのミックスに反映されます。solo のあるプロジェクトでは solo でないトラックが無音になります (トラックの mute とバスの gain / mute / solo は Wave 12 までと同じ)。solo のないプロジェクトの書き出しは変わりません (US-1209)。
- 書き出し例外: モバイル書き出し (V1 にクリップがあり、自動ラウドネスが OFF) とレンダーキューダイアログの手動追加が、タイムラインの音声ミックス (クリップ EQ、トラックの pan / gain、複数トラック、バス、AUX) を通ります。音声が「クリップ 1 本・編集無し」と同じ扱い (素通し) になるプロジェクトは、従来どおり V1 素材の音声を使うため変わりません (US-1210)。
- 書き出し例外 (欠陥修正): 不透明な素材でも、スタックに有効なキーヤー (クロマ / ルマ / カラー) があれば、後段のぼかしなどの FX のあとでも alpha が保たれます。キーヤーのない不透明な素材の書き出しは変わりません (US-1211)。
- FCPXML の読み込みは、トラック数の上限なしで全 lane を取り込み、transition・複合クリップ・マルチカム・clip / sync-clip を読めるようになりました (従来は読み飛ばして警告)。詳細は下の「FCPXML 読み込みの対応表」 (US-1204 〜 US-1207)。
- FCPXML の書き出しは、V2 以上の映像クリップを lane の子として書き、主ストーリーラインへの接続を接続クリップとして書きます。負のオフセット (J カット) の接続も書き出して読み戻せます (US-1208)。
- 連番静止画の書き出しの形式に EXR が加わりました (書き出しダイアログの種類は「連番静止画 (PNG / TIFF / EXR)」)。同梱の avcodec に exr エンコーダがあるときだけ選べ、無い環境では EXR は選択肢に出ません。表示参照 (既定) のほか、チェック「EXR をシーンリニアで書き出す」 (既定 OFF) でシーンリニアで書けます。この設定はユーザープリセットにも保存されます。PNG / TIFF の書き出しは変わりません (US-1212 / US-1213)。
- 接続クリップの分割・無音カット・貼り付け・bake・precompose・トラック操作の各経路は、接続の規則 (下の「主ストーリーラインと接続クリップ」) に従って接続を引き継ぎます。接続のないプロジェクトの編集結果は変わりません (US-1214)。
- 累積レビューの修正: ダイアログの手動追加で、保存済みプロジェクトのジョブに入れ子シーケンスの音声が入ります。主ストーリーラインの ON で、V1 以外のクリップを削除したときに V1 が詰まらなくなりました。主ストーリーラインの ON で貼り付けるとき、使える (ロックされていない) 音声トラックが無ければ何も貼り付けず、ロック中の音声トラックにクリップを書き込まなくなりました (US-1220)。

主ストーリーラインと接続クリップ (Wave 13。接続は明示操作、主ストーリーラインのモードは既定 OFF):

- V2 以上の映像クリップと音声クリップを、V1 のクリップに「接続」できます。編集メニューまたはクリップの右クリックの「主ストーリーラインに接続」で接続し、「接続を解除」で外します (新規ショートカットはありません。どちらも 1 操作 = Ctrl+Z 1 回)。接続したクリップは接続先 (アンカー) の移動・トリム・速度変更に追従し、タイムラインに接続線とバッジが描かれます。MCP の `connect_clip` / `disconnect_clip` でも同じ操作ができます (US-1201 / US-1202)。
- 編集メニューの「主ストーリーライン (マグネティック)」 (チェック式。コマンド ID は `timeline.storyline_mode`) は**既定 OFF** で、プロジェクトに保存されます (undo の対象外)。ON の間は V1 だけが insert / ripple 削除 (リンク相手も一緒に消える) / ripple トリム (Q / W) / 隙間詰めの対象になり、V1 に隙間が残りません (Wave 14 で V1 への media drop は drop 位置への insert になり、マウスの端トリムと nudge も ripple します。例外として + 方向の nudge は、開けた前の空きが後続の storyline insert や移動で詰まるまで残ります。US-1304 / US-1305)。V1 のリンクした音声は V1 に追従します。V2 以上や音声トラックへ素材・クリップを drop すると、開始位置が重なる V1 のクリップにその場で接続されます (重なる V1 クリップが無ければ接続せずに置きます)。OFF のときの動作は Wave 12 までと同じです (US-1203)。
- 接続はリンクグループ単位です (対になる音声も同じ接続先に付きます)。接続先は V1 のクリップだけで、V1 のクリップ自身は接続できません。
- 接続先の開始・イン / アウト点・速度・反転・ランプ・リマップが変わったとき、線形な接続先 (速度一定・非反転・ランプとリマップ無し) では、接続クリップは同じ素材フレームに付いたまま動きます。非線形な接続先では、接続先の開始からの位置を保ちます (FCP との既知の差)。接続先の前後にはみ出してもかまいません。追従先に置き場所が無いときは、元の位置 (空いていなければ自トラックの末尾) に置いて接続を解除し、ステータスバーに「「<名前>」の接続を解除しました (退避先のトラックがありません)」を出します。
- 接続先を消すのは明示的な削除 (Delete / Shift+Delete / MCP の `delete_clip` など Timeline の削除操作) だけで、そのとき接続クリップも連動して削除されます。MultiCam 適用・新規シーケンス化・読込など、それ以外の理由で接続先が消えたときは、接続クリップを残して接続を解除し、ステータスバーに「「<名前>」の接続を解除しました (<理由>)」の警告を出します。ロック中のトラックの接続クリップと、削除される区間の外にはみ出した接続クリップも、削除せず解除 + 警告です。
- 接続がある間は、V1 トラックの削除と、V1 を起点または終点にするトラックの移動が拒否されます。ロック中のトラックの接続は動かさず、解除 + 警告です。
- 接続先の分割では、接続クリップが位置を変えずに左右の片へ振り分けられます (接続クリップ自身を分割したときは右片も同じ接続先に付きます)。cut → paste、複製、新規シーケンス化は接続を複製しません。bake (映像・音声) は接続を保ちます。無音カットで除去した区間にある接続クリップは、削除せず解除して残ります。MultiCam を適用すると接続は解除されます。
- FCPXML から取り込んだ接続クリップは、モードが OFF でも接続として取り込まれ、同じ規則で追従します。
- 主ストーリーラインの ON では、V1 へのメディアファイルの drop が drop 位置への insert になり、V1 の端トリム (リンクした音声も開始位置固定) と nudge も ripple します (Wave 14。US-1304 / US-1305)。nudge で + 方向に開けた前の空きは、後続の storyline insert や移動で詰まるまで V1 に残ります。V1 と他のトラックにまたがる選択の nudge は拒否されます (選択した V1 クリップのリンク相手は対象外)。ブレードと MCP の編集ツールは ON でも ripple しません (従来どおり)。OFF のときの W と MCP の trim_clip は、下流のクリップがトリム量 d だけ動きます (従来は 2d 動く欠陥の修正)。

FCPXML 読み込みの対応表 (Wave 13 / Wave 14。実際の Final Cut Pro が書き出した .fcpxml での確認は、どの行もまだ行っていません):

| FCPXML の要素 | 取り込み方 | 実機確認 |
|---|---|---|
| lane (接続クリップ) | トラック数の上限なし。lane は 0 を先頭に番号の絶対値が小さい順に並べ、飛び番を詰めて 1 lane 1 トラック。音声専用の素材は音声トラック (US-1204) | 自作 fixture で確認 |
| clip / sync-clip | コンテナを平坦化して、子の映像・音声を取り込む (US-1204) | 自作 fixture で確認 |
| 音声付き asset-clip | `<asset>` が `hasAudio="1"` で音声専用でなく、`srcEnable` が video 以外の asset-clip は、映像と同じ offset / start / duration の音声クリップを同じ番号の A lane に作り、V + リンク A の対 (リンクグループ) として取り込む。接続クリップなら映像側と同じ接続先に付く。`<clip>` に明示の音声の子があるときは複製しない (US-1308) | 自作 fixture で確認 |
| 接続クリップ | 主ストーリーラインへの接続 (`anchorUid` / `anchorOffsetSec`、負のオフセット = J カット) として取り込む。音声のみの spine クリップに付いた子は接続を解除し、警告「主ストーリーラインが音声クリップのため接続を解除しました: <名前>」 (US-1204) | 自作 fixture で確認 |
| gap に接続された子 | gap の位置を基準に取り込むが、接続にはしない (FCP では接続になる。既知の差) | 自作 fixture で確認 |
| 負の lane の映像 | V に置き、警告「lane N の映像は FCP では主ストーリーラインの下に合成されます。V<n> に置きました。」 (合成順が FCP と逆になる。既知の差) | 自作 fixture で確認 |
| transition | 前後のクリップの両縁に CrossDissolve (Center)。前後が重なる形は重なりの中点で詰める。ハンドル不足は長さを縮めて警告。先頭・末尾は FadeIn / FadeOut。名前が Cross Dissolve 以外のものは CrossDissolve に置き換えて警告 (US-1205)。前後のクリップが取り込み後に同じトラックで接していない場合 (別トラックに置かれた、間が空いている、または重なりを詰めた結果が別トラックに分かれた場合) は transition を付けず、「未対応の要素を読み飛ばしました: transition (行 N)」の警告を出す。 | 実機未確認 (transition の時刻配置は自作 fixture のみ) |
| 複合クリップ (media/sequence + ref-clip) | ネストシーケンス (`sequenceRefId`) として取り込む。入れ子は深さ 8 まで、循環参照は警告して読み飛ばす。MCP `import_fcpxml` の応答 `sequences` に入れ子の ID が入る。取り込み直後の undo 1 回で取り込み前の状態 (入れ子シーケンスを含む) に戻る (US-1206) | 自作 fixture で確認 |
| マルチカム (media/multicam + mc-clip) | 有効アングル (`mc-source` の `srcEnable` が video / all。無ければ最初のアングルを使い警告) の spine を、mc-clip の区間で切り出して平坦化する。有効な音声アングルが別なら音声トラックの接続クリップ。使わなかったアングルは警告に列挙 (US-1207)。`srcEnable="audio"` の source は音声アングルとして取り込み、mc-clip の `audioStart` / `audioDuration` を音声クリップの開始と長さに使う。lane が 0 以外の mc-clip (接続されたマルチカム) は読み飛ばさず、mc-clip を囲む spine の親クリップへの接続クリップとして取り込む (別アングルの音声は映像の切り出しに接続)。アングルが mc-clip の範囲より短いときは末尾を空け、警告「mc-clip <名前> のアングル <アングル> が範囲より <秒> 秒短いため末尾を空けました」を出す (US-1309)。mc-angle 内の transition は「mc-angle 内の transition を読み飛ばしました (N 件)」の警告で読み飛ばす。 | 実機未確認 (mc-clip の属性は自作 fixture のみ。`audioStart` / `audioDuration` は DTD の記憶に基づく) |
| コンテナ (clip / sync-clip / mc-clip) の範囲 | コンテナの `duration` を枠の終端に使い、超える子は切り詰めて警告「コンテナの長さを超えるクリップを切り詰めました: <名前> / <ref>」、枠の外に完全に出た子は警告「コンテナの範囲外のクリップを読み飛ばしました: <名前> / <ref>」で読み飛ばす。接続されたコンテナが 0 秒より前に出る分は 0 秒で切り詰め (警告「接続クリップをタイムライン 0 で切り詰めました」)、子は切り詰め後の枠で配置する (US-1310) | 自作 fixture で確認 |
| 上記以外の要素 | 従来どおり、警告「未対応の要素を読み飛ばしました: <要素名> (行 N)」で読み飛ばす | - |

Wave 13 の保存形式 (.veditor。`PROJECT_FORMAT_VERSION` は 2 のまま):

- 任意キーが 3 つ加わりました: クリップの `anchorUid` と `anchorOffsetSec` (接続されているときだけ)、プロジェクトの `storylineMode` (ON のときだけ)。旧ファイルはそのまま読め、旧ビルドでも読めますが、旧ビルドで再保存すると接続とモードは失われます。読込時に接続先の uid が見つからない接続は、解除 + 警告です。プロジェクトの比較 (compare) は `anchorUid` / `anchorOffsetSec` を無視します。

Wave 12 の変更 (既存プロジェクトにも効くもの。書き出しの結果が変わるものは「書き出し例外」と明記):

- V1 が空で上位のトラックに映像がある書き出しは、通常 / モバイル / MCP の `export_video` / バッチ (自動ラウドネス ON / OFF) のすべての入口で音声トラックが付きます (書き出し例外。音声クリップが無ければ無音、あれば従来は落ちていた音声が混ざります。ただし自動ラウドネス ON のバッチで音声クリップが無いタスクは、従来どおり無音として拒否されます (下の制限事項と同じ。無音のトラックを書くのはバッチ OFF だけです))。V1 にクリップがあるプロジェクトと、映像が 1 つも無いプロジェクトは変わりません (US-1109)。
- 逆再生クリップを、ハンドル付き (素材の範囲内に収まるものを含む) で RenderInPlace に焼き込むと、クリップ末尾まで映像と roto マスクが元のタイムラインと同じソースフレームになります。焼き込みの fps がストリームの fps より高く素材末尾に達する焼き込みは、末尾で最後のフレームを保持します (書き出し例外。通常の書き出しと、素材末尾に達しない順方向の焼き込みは変わりません) (US-1103)。
- 書き出しの進捗は job の uuid で絞られ、進捗ダイアログのキャンセルはそのジョブだけを止めます (従来は共有キュー全体が止まりました)。ジョブは追加時に ACES の設定を写して持つため、あとで ACES 設定を変えても追加済みのジョブは変わりません。書き出し結果は変わりません (US-1108)。
- レンダーキューダイアログは、専用のキューではなくメインの共有キューを使います。ダイアログから追加したジョブも書き出し完了の通知に数えられ、プロジェクトの ACES が有効なときは ACES 付きで書き出され (書き出し例外)、書き出しダイアログの音量ゲイン (`loudnessGainDb`) も明示して持ちます (US-1125)。
- マットの設定や、バスパネルのバス割当・gain / mute / solo / 名前のように自前の undo 点を積まない編集は、直前の undo 点に吸収されます (吸収した編集は redo を捨てます)。マット設定のあとに別の編集を undo してもマットは消えません。ミキサーのストリップのコンボでバスを割り当てる操作は自前の undo 点 (「トラックのバスを変更」、Ctrl+Z 1 回) になり、タイムライン状態と一緒に undo / redo されます。書き出し結果は変わりません (US-1107)。
- GPU プレビュー (GLPreview) の特殊合成 (Echo / ローリングシャッター補正) は、焼き込み済みフレームを 1 つの入口から表示します。roto の alpha とグレードが二重に掛からなくなります (プレビューだけが変わり、書き出しは変わりません。Wave 11 から持ち越した「GPU で roto の alpha が落ちる」不具合は再現せず、実際の原因は二重のグレードでした) (US-1106)。
- RenderInPlace は焼き込み時の元の roto を `renderInPlaceOriginalRoto` に保存し、「元のクリップに戻す」 (分解) で元の roto が復元されます (1 undo。書き出し結果は変わりません) (US-1102)。
- クリップを V トラック末尾 / A トラック末尾の「新トラックの帯」 (常時表示の 10 px の行) に drop すると、トラックが新設されクリップがそこへ移ります (1 操作 = Ctrl+Z 1 回、undo の文言は「クリップを新しいトラックへ移動」。リンク相手は同じ番号の相方の種類のトラック (無ければ新設) へ付いて動きます)。書き出し結果は変わりません (US-1117)。
- roto / 親子付け / マットのキーは、クリップの `uid` を優先して見分けます。リンク番号 0 で同一の貼り付けクローンも、それぞれ自分のエントリを持ち続け、手前のクリップをまたぐ並べ替えでもエントリは外れません。すべてのクリップは `uid` を持ち (挿入 / 読込 / 貼り付け / 分割で採番。分解で衝突したときも新しい uid)、プロジェクトの比較 (compare) では uid は無視されます。書き出し結果は変わりません (US-1118 / US-1119)。
- トラックのフェーダー gain と pan は `trackMix` として保存され、再生と書き出しに反映されます (書き出し例外。gain 1.0 以外で保存したプロジェクトは、従来は読込で gain が 1 に戻っていたため、再読込後の再生と書き出しが変わります。すべてのトラックが既定値 (gain 1.0 / pan 0) のときは書きません)。クリップ pan が 0 以外でトラック FX の無い書き出しは FX 経路に移り、pan が掛かります (書き出し例外) (US-1114)。
- バスの割当・gain・mute・solo と AUX センドを持つプロジェクトの書き出しに、それらが反映されます (書き出し例外。AUX センドは再生にも接続されます)。AUX はバス FX を通さない並列加算です (トラックの solo の書き出し反映は Wave 13 で入りました (US-1209)) (US-1122)。
- 音声の PCM タップは `readData` の 1 か所の epilogue で書き込みます (デコーダ停止中の無音と通常のブロックが合流したあとで、一時停止中は書きません)。スペクトラムアナライザはマスターまたはトラック (FX 後・フェーダー前) を選べ、無信号のときは 12 dB/s で床まで減衰します。書き出し結果は変わりません (US-1116)。
- 自動ラウドネスの音声ミックスと測定は GUI スレッドの外で実行されます。通常 / モバイル書き出しは進捗ダイアログ「音声を準備しています…」 (キャンセル可) を出し、MCP の `export_video` (`autoLoudness=true`) は準備中も `get_export_status` が応答します。ミックスは GUI 側の計画 (`buildMixPlan`) と Timeline に依存しない実行 (`executeMixPlan`) に分かれました。書き出し結果は変わりません (US-1104 / US-1105 / US-1127)。

Wave 12 の追加機能 (すべて既定 OFF または非表示、または明示操作):

- 音声クリップの右クリック > 「クリップ EQ…」で、音声クリップエディタの EQ パネル (「クリップ EQ」チェックと 4 バンドの周波数 / ゲイン / Q) が開きます。1 操作 = Ctrl+Z 1 回 (「クリップ EQ を変更」)。再生と書き出しのミックスに掛かり、入れ子のクリップは子を優先、無ければ親 (直列にはしません) です。
- 音声 Dock のチャンネルストリップに、パンのスライダ (中央スナップ) とバス割当のコンボ (「マスター」とバス名) が加わりました (undo の文言は「トラックのパンを変更」「トラックのバスを変更」)。
- 音声バスパネルの「センド」表で、バスごとにトラックのセンド量 (レベル (dB)。-60 dB 以下でセンド無し) とプリフェーダーを設定できます (undo の文言は「AUX センドを変更」)。
- スペクトラムアナライザのタップ元の選択 (マスター / A<n>)。

Wave 12 の保存形式 (.veditor。`PROJECT_FORMAT_VERSION` は 2 のまま):

- 任意キーが 4 つ加わりました: クリップの `uid` (空でないときだけ)、クリップの `clipEq` (クリップ EQ が有効なときだけ)、RenderInPlace のクリップの `renderInPlaceOriginalRoto` (元の roto があるときだけ)、`audioMixer.trackMix` (トラックごとの gain / pan。いずれかのトラックが既定値でないときだけ書き、その場合は全トラックを並べます)。旧ファイルはそのまま読め、旧ビルドでも読めますが、旧ビルドで再保存するとこれらのキーは失われます。書き出し設定 (ExportConfig) に加わった保存項目はありません (プロジェクトには保存しません)。

Wave 11 の変更 (既存プロジェクトにも効くもの):

- 自動ラウドネスが ON の GUI 書き出し (通常 / モバイル) で、音声の再生シーケンスが空のプロジェクト (音声クリップが無い) は、出力の音声が V1 素材の音声ではなく無音になります (挙動変更)。バッチの出力は変わらず、MCP の `export_video` は Wave 10 と同じ結果です。自動ラウドネスが OFF の書き出しと、音声がある ON の書き出しも変わりません。
- 自動ラウドネスが ON の GUI 書き出し (通常 / モバイル) で、音声ミックスの作成に失敗したとき (無音ミックスの失敗を含む) は、正規化なしで書き出さずにジョブを拒否します (挙動変更)。GUI では通常書き出しは「Export Failed」、モバイル書き出しは「モバイル書き出し失敗」ダイアログに空でないエラーが表示され、ジョブはキューに入りません (MCP の `export_video` では get_export_status の `failed` と空でない `error` で観測できます)。
- 逆再生クリップを、タイムリマップ / 速度ランプ付き、またはハンドルが素材の範囲 (イン点より前 / 素材末尾より後ろ) にはみ出す状態で RenderInPlace に焼き込むと、焼き込み結果の映像と roto マスクが元のタイムラインと同じソースフレームになります (欠陥修正。素材の範囲内に収まるハンドルは Wave 12 の変更を参照)。順方向のクリップと通常の書き出しは変わりません。
- roto を持つクリップ、または alpha 付き素材では、色補正 / HSL セカンダリ / カーブ / LUT のあと、および FX パックの幾何でない FX のあとでも alpha が保たれます (CPU プレビューと書き出しの両方。入力に alpha が無いプロジェクトの書き出しは変わりません)。幾何 FX / メッシュワープ / パペットのあとは、据え置いた制限事項を参照してください。
- 親付きクリップのプレビューは、書き出しと同じ判定 (`parentLayerAt`) で親を採用します。一時停止中の CPU のみのプレビューで無効化した親に追従しなくなり、トランジションの重なり区間で incoming 側のクリップを親にした子は親なしになります。親のフレームが取れないときも親を採用しません (書き出しと同じ)。プレビューだけが変わり、書き出しは変わりません。
- クロストラック drop は undo の対象になりました (「クリップをトラック間移動」)。リンク相手の移動とトラックの新設を含めて 1 操作 = Ctrl+Z 1 回です。
- リンクした音声トラックへのクロストラック drop で、リンク相手の映像クリップの roto / 親子付けも、移動後のクリップに付いたまま動きます (欠陥修正)。
- トラックマットは、主クリップのクロストラック drag、リンクした音声トラックへの drop、範囲リップル (文字起こし編集) でも、元のクリップに付いたまま動きます (欠陥修正)。マットのデータも roto と同じく Timeline が持ちます。マットの設定操作そのものは従来どおり undo の対象外です。
- ディエッサーの試聴 (listen) は、そのトラック自身に付いて動きます。トラックの削除 / 追加 / 移動とその undo / redo の前後でも、試聴が別のトラックに移りません (欠陥修正)。

Wave 11 の追加機能 (すべて既定 OFF または非表示):

- 字幕エディタ (挿入 > 字幕エディタ…) に「分割」と「結合」が加わりました。1 行を選び、「分割位置 (ms)」を指定して「分割」を押すと、その時刻で 2 つの字幕に分かれます (単語の時刻がある字幕は単語ごとの時刻で振り分け、無ければ文字数の比で分けます。話者は両方に複製されます)。隣り合う同じ話者の 2 行を選んで「結合」を押すと 1 つになります (テキストは半角スペース 1 つで連結)。範囲外の位置、隣り合わない行、話者の違う行では押せません。
- 環境設定 > 「書き出し完了を通知する」 (既定 OFF)。ON にすると、通常 / モバイル / MCP の書き出しがすべて終わったとき、ステータスバーに「書き出しが完了しました (N 件、失敗 M 件)」を 10 秒表示し、ウィンドウを点滅させて音を鳴らします。
- 表示 > 「スペクトラムアナライザ」 (既定は非表示の Dock)。master バスの音声を 48 kHz の 2048 点 FFT で表示します (表示だけで音声は変わりません)。Dock を出している間だけ PCM タップが有効になります。
- オーディオ > Track EQ > A<n> > 「4 バンド EQ へ移行」。旧 3 バンドの EQ を現行の 4 バンド トラック EQ へ移します (1 操作 = Ctrl+Z 1 回、undo の文言は「EQ を 4 バンドへ移行」)。プロジェクトの読込では変換しません。

Wave 10 で変わったこと (既存プロジェクトにも効くもの):

- ロトスコープ (roto) が書き出しに反映されます (Wave 10)。**roto を設定した既存プロジェクトの書き出しは、Wave 9 までと変わります** (従来はプレビューだけで、書き出しには出ませんでした)。通常 / モバイル / バッチ (別プロジェクトのタスクや保存済みキューのようにファイル経由のジョブでも、ブラシマスク (brushMask) を含めて) / MCP `export_video` の書き出し、静止画書き出し、RenderInPlace に反映され、トランジションの incoming 側のクリップにも掛かります。入れ子シーケンスの中のクリップは対象外で、Echo のサンプルフレームには掛かりません。roto を持つタイムラインは Smart Render の素通し (再エンコードなしのコピー) の対象外になり、通常の再エンコードで書き出されます。プロキシを使っているときは、書き出しは元素材の解像度で roto を適用するため、プレビューと解像度が違って見えることがあります。roto の無いプロジェクトの書き出しは変わりません。
- RenderInPlace (「効果を焼き込んで差し替え」) でも roto が焼き込まれます。roto を持つクリップを焼き込むときは、選んだコーデックにかかわらずアルファ付きの ProRes 4444 (MOV) で書き出されます (roto は範囲外のアルファを 0 にするため)。焼き込み後は元のクリップの roto が外れ、新しいクリップには roto が残りません (undo で元に戻ります。分解 (decompose) で元の roto が復元されるのは Wave 12 からです)。
- roto はクリップ編集に追従し、undo の対象です。削除 / 分割 / 移動 / リップル / 範囲リップル (文字起こし編集) / ペースト / ドラッグ並べ替え / トラック増減 / nudge / 複製 / ギャップ詰め / 全トラック分割 / リフト、および主クリップのクロストラック drag で、roto は元のクリップに付いたまま動きます (クロストラック drop の undo は上の Wave 11 の変更を参照)。roto の編集 (ロトツール) は 1 操作 = 1 undo です (クリップ削除の undo で roto も戻ります)。従来は編集後に roto のキーがずれることがありました (欠陥修正)。
- MCP `export_video` は `autoLoudness` / `targetLufs` を省略するとプリセットの設定に従います。自動ラウドネスが ON のユーザープリセットを `preset=` で使う既存の MCP 呼び出しは、引数を足さなくても音量が正規化されるようになります (挙動変更。従来どおりにするには `autoLoudness=false` を明示)。詳細は MCP の節の `export_video` の説明を参照。
- 音声 Dock の操作が undo の対象になりました (ジェスチャ単位: スライダーやダイヤルのドラッグ 1 回 = 1 undo。マスターは EQ / ディエッサーが対象。wheel は 1 ノッチ 1 回、ディエッサーはスピンボックスの確定で 1 回)。ディエッサーの試聴 (listen) とレガシーのマスターコンプは対象外です。
- selftest が稀にハングする原因は、アプリ自身の Qt メッセージハンドラが Qt のシグナル / スロットのロックを保持したまま自己デッドロックすることでした (トラックの削除、トラック追加 / 削除の undo で起こり得ました)。ログとクラッシュのハンドラを修正し、mixer-dock 0/300、mcp 0/100、track-ops 0/100 でハングは出なくなりました (到達)。selftest のタイムアウト時は `.omc/artifacts/hang-dumps/` に (Windows のみ) minidump (.dmp、必須) とテキストスタック (.txt、ベストエフォート) が書かれます。

据え置いた制限事項:

- roto の alpha は、Echo / ローリングシャッター補正が有効なクリップでは、Echo の軌跡が roto で切り抜かれず、roto で透過にした外側に軌跡が見えます。幾何 FX・メッシュワープ・パペットピンのあとの alpha は Wave 14 で保たれるようになりました (書き出しと CPU プレビュー。GPU プレビューは従来のまま)。グレード段と幾何でない FX のあとは、プレビュー (CPU と GPU) と書き出しで保たれます。
- 自動ラウドネスが ON のバッチ書き出しは、測定できない (NaN) 音声や -70 LUFS 以下の無音を拒否します (無音は書き出さない方針)。GUI (通常 / モバイル) と MCP は同じ素材を 0 dB で書き出すため、方針が異なります。
- 自動ラウドネスが OFF のバッチ書き出しで音声の再生シーケンスが空のタスクは、V1 素材の音声へフォールバックします (V1 が空で上位トラックに映像があるときは無音のミックスになります (US-1109))。GUI の ON では無音になります。
- 書き出し完了の通知は、バッチ書き出しの専用キューと RenderInPlace のローカルキューの対象外です。レンダーキューダイアログの「キュー全体を停止」ボタンは、共有キュー全体 (MCP や通常書き出しのジョブを含む) を止めます。待機中のジョブは一覧に残ります。「選択ジョブを中止」は選んだジョブだけを止め、待機中のジョブは一覧から削除されます (Wave 16。US-1507)。
- 旧 3 バンド EQ の 4 バンド移行は、表現できる設定だけが対象です (プリアンプ 0 dB、スロット 0〜2 のうち平坦でない帯が 2 つ以下、周波数 / ゲイン / Q が 4 バンド EQ の範囲内、そのトラックのディエッサー / コンプレッサー (比率 1 超) / リバーブ (ミックス 0 超) が無効 (有効だと処理順が変わるため))。表現できないときはメッセージを出して何も変えません。プロジェクトの読込では変換せず、メニューの明示操作だけで移行します。移行後の PCM は元の EQ と近似で、フィクスチャでの差は最大 2 LSB です。
- ディエッサーの試聴 (listen) の復元は「トラック自身に付く」方式です。undo で戻したトラックの試聴を、後から追加したトラックが引き継ぐことはありません。
- 親付きクリップ: 親素材のファイルは存在するが 0 バイトでデコードできない場合、プレビュー (VideoPlayer の CPU / トラック経路) と書き出し (renderFrameAt) のどちらも親無しと同じに描き、子は親の変換に従いません (parentLayerAt は hit / decodable を返しますが、VideoPlayer は親を採用しません: parentHit = 0、parentHitDecodable = 0、parentSourceIndex = -1。Wave 18 の US-1812 G11 で実測して固定。Wave 17 までの「整列は親ありとして計算し、描画は親を省く」は誤りでした。parentLayerAt の decodable 判定を実デコード結果に合わせるのは Wave 19 候補)。V1 上のキーフレーム付きクリップがトランジションの借用区間にあるときは、整列 / サーフェスツール (メッシュワープ / パペットピンの共通の編集ツール) はクランプした時刻で評価します。
- 自動ラウドネスは RenderInPlace / レンダーキューダイアログの手動追加では適用しません (設計)。
- Echo / ローリングシャッターの特殊合成は、ソースフレームの取得関数がプレビュー (decodeClipFrameAtSourceTime) と書き出し (renderClipSourceFrame) で別実装です (現状の結果は一致)。
- 自動ラウドネスの準備 (ミックスと測定) は、バッチ書き出しでは同期実行です (GUI の通常 / モバイル書き出しと MCP の `export_video` (`autoLoudness=true`) は非同期)。非同期の準備のキャンセルは次の ffmpeg 起動前に効き、すでに走っている ffmpeg は終わるまで続きます。音声シーケンスが空のときの無音トラックの計画 (Timeline の読み取り) は GUI スレッドで行い、作成は準備と同じく別スレッドで行います (キャンセルも効きます)。
- 自動ラウドネスを使わないレンダーキューダイアログの手動追加 / RenderInPlace の経路では、音声ミックスの準備が GUI スレッドで同期実行され、キャンセルもタイムアウトもできません (Wave 12 から。US-1210 の持ち越し)。
- AUX センドは、バス FX を通さない並列加算です (センド先のバスのエフェクトは掛かりません)。
- スペクトラムアナライザは、再生を一時停止している間は新しい音を取り込まず、表示が床まで減衰するだけです。表示できるのはマスターまたはトラック (FX 後・フェーダー前) で、バスは選べません。
- 旧ビルドで Wave 12 のプロジェクトを開いて再保存すると、`uid` / `clipEq` / `renderInPlaceOriginalRoto` / `trackMix` のキーが失われます (旧ビルドでも開けますが、クリップ EQ、トラックの gain / pan、焼き込み前の roto は保存されません)。
- 旧ビルドで Wave 16 のプロジェクトを開いて再保存すると、wiggle / expressions / autoOrientToCamera / wigglesPerSecond のキーが失われます (旧ビルドでも開けますが、クリップの wiggle と式、カメラへ自動的に向ける設定、Wiggle Path の毎秒のウィグル数は保存されません。新ビルドで保存したファイルは、旧ビルドで開いても wiggle と式が効きません。旧形式の wiggleClipEntries / expressionBindingsEntries は新ビルドが読み込み時にクリップへ移し、保存では書きません)。
- ストーリーライン ON ではメディアビン / ファイルの drop でも新トラックを作ります (V の帯は映像ファイル、A の帯は音声を持つファイルを音声のみとして取り込みます。Wave 15。US-1407)。OFF では従来どおり、帯へ drop できるのはタイムライン上のクリップだけで、メディアビンからの drop では新しいトラックは作られず、映像クリップを A の帯へ、音声クリップを V の帯へ drop した場合は無視されます。
- EXR 連番書き出しは、同梱の avcodec に exr エンコーダがある環境でだけ選べます (Wave 13 で出荷。エンコーダが無いと種類のコンボに EXR は出ません)。
- 幾何 FX の alpha (A-05) は Wave 14 で直りました (US-1301)。DisplacementMap / LensDistortion を含む幾何 FX は alpha を運び、LensDistortion の画面外の縁は、クリップのメディアが alpha を持つときだけ透明、不透明な素材は従来の黒です (利用者判断 14 / 18。書き出し結果の変更点は Wave 14 の変更を参照)。メッシュワープ / パペットも同じ経路 (applyClipMeshWarp) で alpha を運びます。これは書き出しと CPU プレビュー (tlrender の描画経路) の変更で、GPU プレビューは従来のままです。残る制限: Echo / ローリングシャッター補正が有効なクリップでは、レンズのあとに alpha を落とす FX (ぼかしなど) を置くと縁が不透明に戻ります (レンズのあとが alpha を保つ FX だけなら透明のままです)。Echo と roto を併用すると、Echo の軌跡は roto で切り抜かれず、roto で透過にした外側に軌跡が見えます。
- キーヤー後の alpha の運搬 (US-1211) は tlrender の描画経路 (書き出しと CPU プレビュー) の変更です。Exporter::doExport が applyEffectStack を直接呼ぶ旧書き出し経路との一致は、Wave 11 の roto の alpha (US-1004) と同じ扱いで、揃えていません。VideoPlayer がプレビューで applyEffectStack を直接呼ぶ経路 (調整レイヤーの適用 VideoPlayer.cpp:706 と、プレビューの FX スタック VideoPlayer.cpp:4219) もキーヤー後の alpha の運搬は行わず、US-1004 と同じ扱いで揃えていません。
- MainWindow が位置で持つサイドカーのうち 3 種 (3D テキストの設定、タイムリマップ、ライブブラシのアニメーション) とブラシのアニメーションの項目は、roto と同じ張り替え (編集 1 回につき 1 回) に乗り、undo / redo の各段でも追従します (Wave 14。US-1302 / US-1303)。ブレードの右片に 3D テキスト / ブラシは付きません (Wave 17 から 3D テキストの設定とブラシのデータはクリップ自身が持ち、位置の張り替えの対象ではなく、コピー・複製・貼り付けで引き継がれます。US-1706。Wave 18 から分割の右片にも付き、右片の局所時刻は分割位置ぶんずらされます。US-1807。タイムリマップは Wave 16 からクリップ自身のカーブが切った位置で 2 つに分けられ、両片で再生されます。US-1501 / US-1520)。パーティクルは filePath key で持つため張り替えの対象外です。式のバインドと wiggle のパラメータは Wave 16 からクリップ自身が持ち、サイドカーではありません (US-1504 / US-1505)。Wave 15 から、タイムリマップ / 3D テキスト / 式 / wiggle はダイアログの確定 1 回が undo 点 1 つ (Ctrl+Z 1 回) になり、値ごと undo / redo に乗ります (US-1401)。ブラシは従来どおりです。 (Wave 17 で変更) ブラシの設定の確定も undo 点 1 つ (Ctrl+Z 1 回) です (US-1706。Timeline.cpp:7909 の saveUndoState("ブラシを変更"))。uid の並びが同一のトラックの入れ替えと連続削除の制限は解消しました (読み込み時に映像トラックをまたぐ重複 uid を修復します。US-1405)。
- reversed (一様速度) のクリップの分割は、両片が素材フレームを保ちます (Wave 15。US-1406)。ブレード / 全トラックブレード / 範囲上書き / 3 点上書き / 範囲削除 / 接続の分割など、reversed クリップの分割を伴う操作はすべて両片の素材を保ちます。reversed クリップのフリーズフレームは再生ヘッド位置の素材を保持します (従来は元の末尾)。速度ランプまたはタイムリマップを持つクリップの分割は、Wave 16 から両片が素材区間と長さを保ちます (書き出し結果の変更点は Wave 16 の変更を参照。US-1501)。
- FCPXML の書き出しは、他のクリップと重なる接続のない V2 以上のクリップを、重なる spine クリップの lane の子として書きます (FCPXML は接続なしの重なりを表せないため)。書き出した FCPXML を読み戻すと、そのクリップは接続クリップになります。
- 主ストーリーラインの ON では、V1 へのメディアファイルの drop が drop 位置への insert になり、V1 の端トリム (リンクした音声も開始位置固定) と nudge も ripple します (Wave 14。US-1304 / US-1305)。nudge で + 方向に開けた前の空きは、後続の storyline insert や移動で詰まるまで V1 に残ります。V1 と他のトラックにまたがる選択の nudge は拒否されます (選択した V1 クリップのリンク相手は対象外)。ブレードと MCP の編集ツールは ON でも ripple しません (従来どおり)。OFF のときの W と MCP の trim_clip は、下流のクリップがトリム量 d だけ動きます (従来は 2d 動く欠陥の修正)。
- タイムリマップとフリーズフレームは、保存済みプロジェクト経由の書き出し (レンダーキューのファイル指定とバッチ書き出しの File) にも乗ります (Wave 15。US-1402。書き出し結果の変更点は Wave 15 の変更を参照)。式と wiggle は Wave 16 から書き出しにも乗ります (US-1504 / US-1505。ファイル指定ジョブの式の audioLevel() は Wave 16 では 0、Wave 17 から実際の音声レベル。書き出し結果の変更点は Wave 16 / Wave 17 の変更を参照)。3D テキスト / ブラシアニメーションは Wave 17 から書き出しにも乗ります (US-1707 / US-1708。書き出し結果の変更点は Wave 17 の変更を参照)。
- FCPXML のマルチカムは平坦化して取り込みます。MultiCamSession としての取り込みは未対応です。FCPXML の書き出しと読み戻しの往復で接続のない重なりが接続クリップになる点は、FCPXML の表現の限界として据え置きです。
- Wiggle Path (シェイプモディファイア) は、毎秒のウィグル数を 0 より大きくするとクリップの時間で変化します (Wave 16。US-1502)。時間変化中は点の変位の振幅が最大で 2 倍になります。0 (既定) のあいだは従来どおり静的です。
- 音声トラックの片は速度曲線を持たず、順方向の音声片は 1x (クリップの speed のまま) で再生されます。ramp の末尾の静止区間や t* 付近で分割した片は、映像の平均速度には合わず、プレビューでも 1x になります (従来からの制限)。reversed の音声片は片の平均速度で再生されます (0.1x 未満はプレビューでクランプ)。atempo OFF で分割した片の ramp は原点が揃いません (従来と同じ)。分割後の音声の書き出しは分割前と連続しません (Wave 16。US-1501)。
- RenderInPlace の焼き込みで、素材の fps を読めない roto クリップのマスクのフレーム番号は、従来どおり 30 fps で決まります (レンダーキューのファイル指定はプロジェクトの fps に変わりました。RenderInPlace 側の修正は Wave 18 候補で、US-1702 は見送りました。US-1520)。 (Wave 18 で変更) RenderInPlace の一時タイムラインもプロジェクトの fps を使います (RenderInPlace.cpp:442。US-1802)。
- バッチ書き出しで、保存済みプリセットとして解決できないプリセット名のタスクは、Wave 16 から失敗 (Failed) になり、出力は作られません (Wave 16。US-1510)。失敗するのは、キューに積んだ時点で解決できなかった名前 (実行時にもう一度引き直します) だけで、MCP の export_video には波及しません。
- バッチ書き出しの音声: 現在のプロジェクト以外のファイルを指すタスク (task.timeline が null) は、自動ラウドネスが OFF のときだけ従来どおり元ファイルの音声で書き出されます。自動ラウドネスが ON のときは測定できないため、Queue::processNext がエラー「自動ラウドネス: 現在のプロジェクト以外のタスクでは測定できません」を記録し、RenderQueue がジョブを拒否します (書き出されません)。バッチ設定ダイアログには、組み込み名 (1080p / 720p / 4K) と同名のユーザープリセットは出ません (組み込みが優先)。ミックスはタスク実行時のタイムラインから作られるため、キュー投入後の編集が反映されます (映像と同じ)。タイムラインの長さが 0 より大きく音声の再生シーケンスが空でないときだけミックスを作ります (V1 が空で上位トラックに映像があるタスクは例外で、音声が無くても無音のミックスを作ります。長さ 0、または音声トラックが無い / 音声を持つクリップが無いときは従来どおり元ファイルの音声で、警告は出ません。全トラックがミュートでも音声を持つクリップがあれば、ミュート分を除いた無音のミックスで書き出され、警告は出ません。通常書き出しと同じ)。ミックスの作成に失敗したときは元ファイル音声で書き出し、書き出しが成功した場合だけタスクのメッセージが「Export complete (タイムライン音声ミックスの作成に失敗したため元ファイル音声で書き出します: <原因>)」になります (書き出しが失敗した場合はエラーメッセージが優先され、警告は残りません)。
- 書き出し設定の音声ビットレートは音声を再エンコードする経路にだけ効きます。素材の音声をそのままコピーする経路では元のビットレートのままです。
- FCPXML / EDL: 接続クリップを持つ親クリップの時刻範囲が不正 (duration が 0、または offset / start の計算が不正) な場合、その親は「lane N の時刻範囲が不正なクリップを読み飛ばしました: <名前> / <ref>」で、その接続クリップは「親クリップが不正なため接続クリップを読み飛ばしました: <名前> / <ref>」で、警告付きで読み飛ばされます (従来は取り込まれることがありました)。子を持たない spine の gap は警告を出さなくなりました。J カットが 0 秒より前に一部だけ出る分は切り詰めて警告「接続クリップをタイムライン 0 で切り詰めました: <名前> / <ref>」を出し、全体が 0 秒より前に出るクリップは取り込まれず「クリップの値が不正です (接続位置): <名前> / <ref>」で読み飛ばされます。EDL の dissolve は、長さが正のものなら素材の有無に関係なく、少なくともディゾルブ長 × 速度ぶんのハンドルが確保されて trail が常に正になるため (0 フレームのディゾルブはハンドルが 0 になり得て、trail は正になりません)、取り込み直後のトランジション区間の描画が変わります (送り側クリップは、そのハンドル分だけは常に右端を伸ばせます。ハンドルを超えて素材長まで伸びるのは、素材が見つかり probe が正の値でハンドル込みの長さより長いときだけです)。EDL はソース開始タイムコードを差し引きません (既存の制限)。
- FCPXML の `<asset>` の `start` が 0 でない素材でも、`<asset>` に `duration` があれば、右端を伸ばせる範囲は素材末尾までです (Wave 9 の「`start` が 0 の場合だけ」という記載は誤りでした)。取り込み時の combine が `asset.start` を引いたうえでファイル内の時刻で比較するためです (`FcpxmlImport.cpp` の取り込み処理)。

---

## License

MIT
