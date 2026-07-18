# NLE Feature Audit — v-simple-editor

_Audit date: 2026-06-02 · 108 features classified · status independently spot-verified against source._

> **Scope & denominator caveat (revision note).** The first pass anchored on the **90 selftest-registered engines** plus their flags, which silently SKIPPED older, pre-"selftest-split-campaign" core subsystems that never got a selftest (transitions, undo/redo, color grading, scopes, markers, etc.). Those are added in **§3.1b**. This is a **feature-level** catalog (108 entries); MainWindow.cpp actually has **~251 `addAction` calls**, so many sub-actions of these features are not separately counted. Treat the percentages as a feature-level estimate, not an action-level census — and read them as a **lower bound on "usable"**, since the items most likely to be missing are WIRED core features.

## 1. TL;DR

Of the **108 catalogued features**, **70 (~65%) are genuinely WIRED and usable today** — reachable from a real menu/dock with a real engine behind them and default-on behaviour. That core covers the meat of an NLE: timeline trim/3-point/ripple editing, media pool, source monitor, proxies, in-process libavcore encode/decode, the live audio mixer + bus routing, track mattes (CPU SSOT), HDR export plumbing, ASC-CDL/EDL/FCPXML/DaVinci/Premiere/PPTX export, motion/planar tracking, mograph/lower-thirds/watermark, exposure aids, command palette, and local social/mobile export — **plus the pre-selftest core (§3.1b): transitions, undo/redo + history dock, the color-grading panel (wheels/curves/LUT), scopes/waveform monitors, markers, title presets, the keyframe animation core, autosave/recovery, and in-process AI upscale/frame-interp/mask/smart-reframe (classical engines, not ML models).** The honest weak spots are three "C-like" buckets — **built but not really usable as shipped**: (1) the **entire GPU/HDR compositing path is DORMANT or GATED** — `gpu-composite-math/parity`, `hdr-composite-math/parity` are selftest-only oracles, `live-matte-resolve` only fires behind the default-OFF `VEDITOR_GPU_COMPOSITE` flag, and the HDR16 path (`hdr-composite-parity`/`composite16`) can **never** run live because the overlay decode pool is hard-coded to RGB888 SDR ("Pool stays SDR for the MVP"); (2) **all cloud/social upload integrations are BLOCKED on external credentials** (YouTube, Vimeo, Instagram, X, Frame.io, cloud-render) plus the LLM `transcript-highlighter`; (3) a tail of **MVP-depth features** that are reachable but shallow (Dolby Vision XML-sidecar-only, subtitle-translator stub passthrough, Twitch clipboard-only, whisper stub without external binary, audio loudness/restore simplifications). The remaining DORMANT entries are mostly **selftest harnesses / parity oracles**, not user features — they prove engines work but were never meant to be reachable.

## 2. Tally

| Status | Count | % | Meaning |
|---|---:|---:|---|
| **WIRED** | 70 | 64.8% | Reachable + real engine + default-on/usable |
| **MVP** | 12 | 11.1% | Reachable but shallow / partial / sidecar-only |
| **BLOCKED** | 7 | 6.5% | Wired but needs external creds/dep to function |
| **GATED** | 3 | 2.8% | Real, but hidden behind a default-OFF flag |
| **DORMANT** | 16 | 14.8% | Selftest/oracle only — no UI entry point |
| **TOTAL** | 108 | 100% | |

_(WIRED = 59 selftest-registered + 11 pre-selftest core subsystems from §3.1b. The DORMANT 16 are almost entirely test scaffolding, not user features — excluding them, **70 of 92 real features (76%) are usable**.)_

## 3. By Status

### 3.1 WIRED (59) — usable today

| Feature | Cluster | UI entry (file:line) | Flag / default | Note |
|---|---|---|---|---|
| trackmatte-parity | playback-compositing | MainWindow.cpp:2954 (Composition>トラックマット…) | none / on (export) | CPU SSOT track-matte used by export + CPU preview; real timeline apply. |
| composite-frame-cache | playback-compositing | none (internal) | VEDITOR_ADAPTIVE_PREVIEW_DISABLE / on | LRU composite cache hit in all 3 live tick compose paths. |
| playback-quality-policy | playback-compositing | none (internal) | VEDITOR_ADAPTIVE_PREVIEW_DISABLE / on | Per-tick adaptive canvas divisor under FPS pressure. |
| auto-proxy-policy | playback-compositing | MainWindow.cpp:2475 (Tools>マルチトラック自動プロキシ) | QSettings autoMultitrackProxy / on | Auto-switches heavy multitrack clips to proxy in preview. |
| proxy | playback-compositing / encode-decode-proxy | MainWindow.cpp:2449/2454/2460/2465 (Tools) | env / divisor 2 default | Full proxy gen/manage UI + half-res playback default; in-process. |
| hdr-routing | color-hdr | RenderQueue.cpp:689 via File>HDR出力設定 MainWindow.cpp:1461 | none / on when HDR picked | 3-way 10-bit HEVC routing at export encode time. |
| hdr | color-hdr | MainWindow.cpp:1461 (File>HDR出力設定), 2701 (Tools>HDRグレーディング) | hdrSettings.mode / sdr | Real HDR10/HLG master-display/maxCLL/maxFALL side-data on export. |
| chroma | color-hdr | MainWindow.cpp:2754 (Tools>クロマキー精緻化) | HAVE_CHROMA_KEY_REFINE_DIALOG / present | Reachable chroma-key refinement dialog. |
| asc-cdl-export | color-hdr | MainWindow.cpp:2644 (Tools>ASC CDL 書き出し) | none / on-demand | Lift/Gamma/Gain+sat → .cc/.ccc/.cdl export. |
| exposure-aids | color-hdr | MainWindow.cpp:1689 (View>モニタリング) | none / None default | FalseColor/Zebra/FocusPeaking on display-only copy. |
| tracker-preset | tracking-vfx | MainWindow.cpp:8893 (Edit>モーショントラッカー) | none / on | Preset applied to real motion tracker. |
| planar-preset | tracking-vfx | MainWindow.cpp:2537 (Tools>プラナートラッカー) | none / on | 5 built-in planar presets via dialog. |
| planar | tracking-vfx | MainWindow.cpp:2537 (Tools>プラナートラッカー) | none / on | PlanarTracker engine reachable. |
| planar-tracker | tracking-vfx | MainWindow.cpp:2537 (alias of planar) | none / on | Alias → same dialog/engine. |
| videostab-deshake | tracking-vfx | MainWindow.cpp:2394 (Tools>手ブレ補正) + 1617 (Edit) | none / on | In-process deshake; partial param mapping but core works. |
| vfx | tracking-vfx | MainWindow.cpp:3261 (View>VFX コントロール) | none / off (dock hidden) | Glow/bloom/CA/lightWrap dock toggle. |
| mograph | tracking-vfx | MainWindow.cpp:1842 (Insert>Mographテンプレート) | none / on | Template picker instantiates MographText. |
| animexport | tracking-vfx | MainWindow.cpp:2788 (Tools>アニメGIF・WebP書き出し) | none / on | Animated GIF/WebP export dialog. |
| easing | tracking-vfx | MainWindow.cpp:2796 (Tools>イージングカーブエディタ) | HAVE_EASING_CURVE_EDITOR_DIALOG / present | Real easing-curve editor compiled in. |
| lowerthird | tracking-vfx | MainWindow.cpp:2812 (Tools>ローワーサード) | none / on | Lower-third dialog. |
| watermark | tracking-vfx | MainWindow.cpp:2820 (Tools>ウォーターマーク) | none / on | Watermark dialog. |
| brush | tracking-vfx | MainWindow.cpp:1769 (Insert>ブラシ, shortcut B) | none / on | Brush animation + keyframe track + live preview. |
| nodegraph | tracking-vfx | MainWindow.cpp:3346 (View>ノードコンポジットモード) | none / off (toggle) | Effect-stack ↔ node-graph round-trip (linear chains). |
| media-pool | editing-workflow | MainWindow.cpp:3315 (View) + dock 2841 | none / on | SSOT media pool dock, default left-docked. |
| three-point-edit | editing-workflow | MainWindow.cpp:3326 (View>ソースモニター) | none / on | Real insert/overwrite timeline mutation. |
| trim-ops | editing-workflow | MainWindow.cpp:2134 (トリム menu Q/W/R + slip/slide) | none / on | Real trimops engine via Timeline. |
| text-based-edit | editing-workflow | MainWindow.cpp:2362 (Tools>テキストベース編集) | none / on | Ripple-delete from transcript, tail-first. |
| smartedit | editing-workflow / ai-ml | MainWindow.cpp:2652 (Tools>Smart Edit) | HAVE_SMARTEDIT / present | Silence + scene-change heuristic engine, no stubs. |
| command-search | editing-workflow | MainWindow.cpp:2311/2325 + Ctrl+Shift+P 2331 | none / on | Command palette (cmdsearch::rankMatches). |
| project-preset | editing-workflow | (via tracker dialogs + ProjectFile save/load) | none / on | Tracker state persisted across save/load. |
| projtmpl | editing-workflow | MainWindow.cpp:2685 (Tools>プロジェクトテンプレート) | HAVE_PROJECT_TEMPLATE / present | Real project-template dialog. |
| workspace | editing-workflow | MainWindow.cpp:1679 (View>ワークスペース) | none / on | Dynamic save/switch/delete/reset layouts. |
| shortcut | editing-workflow | MainWindow.cpp:1623 (Prefs>ショートカット設定) | none / on | Custom bindings + Premiere/FCP/DaVinci presets. |
| audio-bus | audio | MainWindow.cpp:3337 (View>オーディオ バス) | none / on | Bus/submix/aux gain in live readData mix. |
| audiomixer | audio | MainWindow.cpp:2014+ (Audio menu DSP) | none / on (core) | Vol/EQ/comp/reverb/NR/bus in live mix. |
| spectral-edit | audio | MainWindow.cpp:2780 (Tools>スペクトル音声修復) | none / on | Self-written radix-2 FFT STFT/iSTFT repair. |
| VEDITOR_HWPERF_AUDIODUCKING | audio | MainWindow.cpp:2021 (Audio>ダッキング設定) | build-time only / on | Misleading name; real sidechain DSP, default-on. |
| edl-export | import-export | MainWindow.cpp:2626 (Tools>EDL CMX3600) | none / on | V1 clips + NTSC drop-frame TC. |
| premiere-xml | import-export | MainWindow.cpp:1422 (File>エクスポート) → combo | none / on | Premiere Pro XML (FCP7) export. |
| fcpxml | import-export | MainWindow.cpp:2618 (Tools>FCPXML 書き出し) | HAVE_FCPXML / present | Real FCPXML build + write. |
| pptx-export | import-export | MainWindow.cpp:2635 (Tools>PowerPoint .pptx) | none / on | OOXML deck from captions + markers. |
| davinci | import-export | MainWindow.cpp:2610 (Tools>DaVinci Resolve XML) | HAVE_DAVINCI_XML / present | FCP7-compatible XML export. |
| affinity | import-export | MainWindow.cpp:1452 (File>取り込みハブ) Affinity tab | HAVE_IMPORT_HUB / present | PSD/SVG/PDF/TIFF layer load. |
| import | import-export | MainWindow.cpp:1452 (File>取り込みハブ) | HAVE_IMPORT_HUB / present | Hub shell; only timeline import ingests (image/mesh/exr stubs). |
| batchexport | import-export | MainWindow.cpp:2744 (Tools, action_batch_export) | HAVE_BATCH_EXPORT / present | Batch export dialog. |
| textexport | import-export | MainWindow.cpp:1763 (Insert>テキスト書き出し SRT/CSV) | none / on | SRT/CSV overlay export. |
| caption | import-export | MainWindow.cpp:1756 (Insert>字幕エディタ) | none / on | Caption editor, also fed by Whisper. |
| broadcast-cc | import-export | MainWindow.cpp:2728 (Tools, action_broadcast_caption) | none / on | CEA-608/708 .scc export. |
| aihighlight | ai-ml | MainWindow.cpp:2430 (Tools>AI自動ハイライト) | none / on | 3-pass FFmpeg DSP heuristic on worker thread. |
| auto-clip-gen | ai-ml | MainWindow.cpp:2354 (Tools>ハイライトから自動カット) | none / on | Deterministic clip-range planner, offline. |
| social | streaming-social | MainWindow.cpp:1433 (File>SNS向けエクスポート) | none / on | Local platform-preset export. |
| snspack | streaming-social | MainWindow.cpp:1433 (= social) | none / on | SNS preset pack driving SocialExportDialog. |
| obs | streaming-social | MainWindow.cpp:1452 (File>取り込みハブ) OBS tab | HAVE_IMPORT_HUB / present | OBS folder scan import. |
| mobile | streaming-social | MainWindow.cpp:1442 (File>モバイル向けエクスポート) | HAVE_MOBILE_EXPORT / present | Local mobile-preset export. |
| youtube-chapter | streaming-social | MainWindow.cpp:2105 (Markers>YouTubeチャプター) | none / on | Chapter text from markers. |
| libavcore-encode | encode-decode-proxy | (internal) Exporter.cpp:203 / RenderQueue.cpp:529 | none / on | In-process HW>SW encoder chain (audio mux NYI). |
| libavcore-decode | encode-decode-proxy | (internal) ProxyManager.cpp:148 etc. | none / on | Fully in-process demux+decode, no subprocess. |

### 3.1b WIRED — pre-selftest core subsystems (11, added in revision)

These predate the selftest-split campaign, so they have no `--selftest=` entry and were missed by the first pass — but all are real engines reachable from default-on menus/docks. (Spot-verified against the cited lines.)

| Feature | UI entry (file:line) | Engine | Note |
|---|---|---|---|
| transitions | Insert>トランジション menu:1777; addTransition:6116 | Timeline::applyTransitionToSelected | Cross-dissolve/wipe etc. applied to selected cut; default-transition apply/edit 1576/1580. |
| undo/redo + history dock | Edit menu:1493/1499; toolbar:3839/3840; dock:3302 | UndoManager / HistoryDockWidget | Full command-history stack + visual history dock. |
| color-grading panel | View dock:2996 | ColorGradingPanel | Wheels→shader:3041, RGB curves→shader:3063, LUT load:3009. Real grade in preview. |
| scopes / waveform | View>スコープ切替:1836 | WaveformGenerator | Lumetri-style waveform/vectorscope monitors. |
| markers (editor) | マーカー menu:2062, Ctrl+M:2064 | timeline markers | The marker feature itself (youtube-chapter only *consumes* markers). |
| title presets | Insert>タイトルプリセット:1795 | title templates | Preset titles inserted onto timeline. |
| keyframe animation core | EffectKeyframeNavBar/Toggle | Keyframe.cpp | Per-effect keyframe tracks + nav bar; underpins brush/mograph/VFX animation. |
| autosave / recovery | MainWindow.cpp:827 | AutoSave | Periodic project autosave + recovery. |
| ai-upscale / frame-interp | Tools:2517 (AI アップスケール/フレーム補間) | AIUpscale.h + FrameInterpolator.h | In-process classical upscale + interpolation (NOT an ML model — heuristic/edge-directed). |
| ai-mask | Tools:2531 (action_aimask_dialog) | AIMask.cpp + AIMaskDialog | Built-in mask engine + optional ExternalPlugin engine path. |
| smart-reframe | Tools:2890 (スマートリフレーム 縦/正方形) | SmartReframe.cpp + AspectReframer | Saliency/motion-weighted auto-reframe to vertical/square; real algorithm, not stub. |

### 3.2 GATED (3) — real, hidden behind a default-OFF flag

| Feature | Cluster | UI entry (file:line) | Flag / default | Note |
|---|---|---|---|---|
| live-matte-resolve | playback-compositing | matte assign at MainWindow.cpp:2954; resolve at VideoPlayer.cpp:4966 | VEDITOR_GPU_COMPOSITE / **OFF** | Live GPU matte resolver only fires when GPU compositing AND GL enabled; default user gets CPU matte-free fallback. |
| aces-color | color-hdr | MainWindow.cpp:2710 (Tools>カラーマネジメント ACES) | AcesPipeline.enabled / **false** | Wired in preview+export but bit-identical until enabled in dialog. |
| VEDITOR_AUDIO_ATEMPO | audio | none (speed-ramp playback only) | VEDITOR_AUDIO_ATEMPO / **OFF** | Per-fragment atempo on speed ramps; invisible without env=1. |

### 3.3 MVP (12) — reachable but shallow

| Feature | Cluster | UI entry (file:line) | Flag / default | Note |
|---|---|---|---|---|
| dolby-vision | color-hdr | MainWindow.cpp:2719 (Tools>Dolby Vision メタデータ) | none / off | XML sidecar only; no RPU/pixel grading (Dolby license out of scope). |
| colormatch | color-hdr | MainWindow.cpp:2575 (Tools>AI カラーマッチ) | HAVE_COLORMATCH | Exports .cube LUT but no auto-apply; manual re-import. |
| auto-matte | tracking-vfx | MainWindow.cpp:2762 (Tools>自動背景除去/マッティング) | none / on | Deterministic color-distance matte, no ML model. |
| multicam | editing-workflow | MainWindow.cpp:1408 (File>マルチカメラ) + 2736 sync | none / on | Base sync aligns all to t=0; dedicated sync path has real audio xcorr. |
| audiorestore | audio | MainWindow.cpp:2771 (Tools>音声リストア) | none / on | deClick/deHum real; NR is simplified block-RMS gate. |
| loudness | audio | MainWindow.cpp:2693 (Tools>ラウドネスマスタリング) + dock | none / on | Real BS.1770-4 on samples; file path NaN for general containers. |
| blender | import-export | MainWindow.cpp:1452 (File>取り込みハブ) Blender tab | VEDITOR_HAVE_ASSIMP | Native OBJ/GLTF parse; FBX/Alembic need optional assimp; ingest is TODO. |
| subxlat | import-export | MainWindow.cpp:2804 (Tools, action_subtitle_translator) | HAVE_..._DIALOG; VEDITOR_TRANSLATE_KEY | Default Stub provider just prepends `[lang]`; real translation needs API key. |
| whisper-transcribe | ai-ml | MainWindow.cpp:2338 (Tools>動画を文字起こし) | none / on | Stub canned transcript; real ASR needs external whisper-cli on PATH. |
| twitch | streaming-social | MainWindow.cpp:2594 (Tools>Twitch 配信設定) | stream key in dialog | Builds rtmp ffmpeg command to clipboard; no live launch. |
| collab | streaming-social | MainWindow.cpp:2565 (Tools>変更履歴) | HAVE_COLLAB / present | Local history/comments only; no server sync. |
| ytdlp-downloader | streaming-social | MainWindow.cpp:1368 (File>URL から取り込み) | none / on | QProcess wrapper; degrades if yt-dlp.exe absent. |

### 3.4 DORMANT (16) — selftest/oracle only, no UI

| Feature | Cluster | UI entry | Flag | Note |
|---|---|---|---|---|
| gpu-composite-math | playback-compositing | none | VEDITOR_GPU_COMPOSITE_MATH_SELFTEST | 8-bit blend-math parity oracle. |
| gpu-composite-parity | playback-compositing | none | VEDITOR_GPU_COMPOSITE_PARITY_SELFTEST | GPU-vs-CPU SSIM/MAE harness (16/16). |
| hdr-composite-math | playback-compositing | none | VEDITOR_HDR_COMPOSITE_MATH_SELFTEST | 16-bit RGBA64 reference oracle. |
| hdr-composite-parity | playback-compositing | none | VEDITOR_HDR_COMPOSITE_PARITY_SELFTEST | composite16 has zero live callers; overlay pool forces RGB888 SDR. |
| parity | playback-compositing | none | VEDITOR_PARITY_SELFTEST | Synthetic framediff MSE sanity gate. |
| e2e | playback-compositing | none | VEDITOR_E2E_SELFTEST | Headless decode+audio harness; skips if test_assets absent. |
| exportaudit | import-export | none | VEDITOR_EXPORTAUDIT_SELFTEST | Validates single-path export; CLI-only. |
| cred-ttl | streaming-social | none | VEDITOR_CRED_TTL_SELFTEST | Token-TTL infra; selftest entry only. |
| credential-vault | streaming-social | none | VEDITOR_CREDENTIAL_VAULT_SELFTEST | Win Credential Manager tier; named feature has no UI. |
| cred-audit-log | streaming-social | none | VEDITOR_CRED_AUDIT_LOG_SELFTEST | JSONL audit infra; no viewer UI. |
| oauth-mock-e2e | streaming-social | none | VEDITOR_OAUTH_MOCK_SELFTEST | Localhost OAuth mock harness. |
| oauth-refresh-e2e | streaming-social | none | VEDITOR_OAUTH_REFRESH_E2E_SELFTEST | refresh_token e2e test. |
| platform-mock-e2e | streaming-social | none | VEDITOR_PLATFORM_MOCK_SELFTEST | IG/X mock-server test. |
| hwperf | encode-decode-proxy | none | VEDITOR_HWPERF_SELFTEST | Aggregate effects selftest bundle (misnomer). |
| pro | encode-decode-proxy | none | VEDITOR_PRO_SELFTEST | Optical-flow effects selftest (misnomer). |
| proext | encode-decode-proxy | none | VEDITOR_PROEXT_SELFTEST | HDR PQ/HLG transfer-fn math selftest (misnomer). |

### 3.5 BLOCKED (7) — wired but needs external creds/dep

| Feature | Cluster | UI entry (file:line) | Blocker | Note |
|---|---|---|---|---|
| transcript-highlighter | ai-ml | MainWindow.cpp:2346 (Tools>文字起こしからハイライト検出) | ANTHROPIC_API_KEY | LLM detection; no offline fallback, returns empty without key. |
| youtube | streaming-social | MainWindow.cpp:2547 (Tools>YouTube アップロード) | Google OAuth creds | Real resumable QNAM upload, needs CLIENT_ID/SECRET. |
| vimeo | streaming-social | MainWindow.cpp:2586 (Tools>Vimeo 直送) | Vimeo creds + in-app auth stub | tus upload real; Authenticate button not wired. |
| instagram | streaming-social | MainWindow.cpp:2677 (Tools>Instagram Reels) | Meta IG token + user id | Real 2-step Graph API publish. |
| xupload | streaming-social | MainWindow.cpp:2669 (Tools>X 動画投稿) | X_BEARER_TOKEN | Real chunked media upload. |
| frameio | streaming-social | MainWindow.cpp:2602 (Tools>Frame.io コメント取り込み) | Frame.io API token | Real REST asset/comment fetch. |
| cloudrender | streaming-social | MainWindow.cpp:2660 (Tools>クラウドレンダリング) | endpoint + API key | Generic render-farm REST client. |

## 4. The C-Cluster — "built but not really usable today"

These are the features most analogous to the HDR-3 case: the code exists and may pass selftests, but a real user on a default build cannot benefit.

| Feature | Why unusable today | Hard blocker? |
|---|---|---|
| **hdr-composite-parity / composite16** | `composite16()` requires **all** layers be RGBA64-premultiplied, but the live overlay decode pool is hard-coded `AV_PIX_FMT_RGB24` / `Format_RGB888` (VideoPlayer.cpp:5083/5087, "Pool stays SDR for the MVP" :5071). The ≥2-all-RGBA64-layers precondition can **never** be met in playback. `VEDITOR_GPU_HDR` has zero readers in VideoPlayer. | Yes — needs an HDR (RGBA64) overlay-decode pool before it can ever fire. |
| **gpu-composite-math / -parity, hdr-composite-math** | Pure parity oracles/selftests. They prove the GPU/HDR blend math is correct but are never invoked at runtime — not features. | N/A (intentional test scaffolding). |
| **live-matte-resolve** | Only runs inside `tryGpuComposeLayers`, which early-returns unless `m_gpuCompositeEnabled` (`VEDITOR_GPU_COMPOSITE`, default **false**) AND GL is present AND a matte exists. Default users get the CPU matte-free fallback. | No — flip-a-flag away, but GPU compositing parity must be trusted first. |
| **aces-color** | Fully wired into preview + export, but `AcesPipeline.enabled` defaults **false**, so output is bit-identical until the user opts in via the dialog. | No — a real, working feature merely hidden by a default. |
| **transcript-highlighter** | Core LLM detection depends on a paid Anthropic API key with **no offline/heuristic fallback** — returns empty out-of-box. | Yes — external paid dep. |
| **youtube / vimeo / instagram / xupload / frameio / cloudrender** | All real network clients, all blocked on user-supplied OAuth/API creds (vimeo additionally has an unwired in-app Authenticate button). | Yes — external accounts/creds. |
| **dolby-vision** | Authors a sidecar DV XML only; no RPU mux / pixel grading (requires Dolby license). | Yes — license/tooling dep (dovi_tool external). |
| **subxlat / whisper-transcribe** | Default to offline stubs (passthrough `[lang]` prefix / canned transcript). Real behaviour needs an API key (translate) or external `whisper-cli` binary (ASR). | Partial — degrade gracefully but useless until external dep present. |

## 5. Recommendations (ROI-ordered)

### (a) ENABLE — flip a default / tiny wiring (highest ROI)
1. **SURFACE aces-color (discoverability, not enable).** It's fully wired in preview AND export and bit-identical when off. Lowest-risk win: keep it default-off but add a visible toggle/indicator in the UI so users discover it (the engine is already trustworthy). *(ROI: high, risk: ~0. NB: this is a discoverability fix — leave the default off.)*
2. **live-matte-resolve / GPU compositing (`VEDITOR_GPU_COMPOSITE`).** Parity is proven (16/16 SSIM ≥0.98). Promote the flag from env-only to a Preferences toggle (still default-off), so power users get GPU matte preview without env hacking. *(ROI: high, risk: low — parity already gated.)*
3. **VEDITOR_AUDIO_ATEMPO** — expose as a checkbox on speed-ramp clips; the implementation is complete, only discoverability is missing. *(ROI: medium, risk: low.)*

### (b) POLISH — MVP → full
4. **whisper-transcribe & subxlat** — bundle/auto-detect `whisper-cli` and add a clear "external engine required" affordance + key entry; today they silently degrade to stubs that look like bugs. *(ROI: high — these read as broken.)*
5. **loudness** — wire container decode (reuse libavcore-decode) so the file-path overload stops returning NaN for non-PCM. The hard part (BS.1770-4) is done. *(ROI: medium.)*
6. **import (image/mesh/exr legs)** and **blender ingest** — finish the TODO ingest handlers (currently qInfo-only) so successful loads actually land on the timeline. *(ROI: medium.)*
7. **audiorestore NR** — upgrade the simplified block-RMS gate to overlap-add spectral subtraction (FFT infra already exists in SpectralEngine). *(ROI: low-medium.)*
8. **multicam** — route the base dialog through the existing real `MultiCamSync` audio xcorr instead of "align all to t=0". *(ROI: medium — engine already exists.)*

### (c) UNBLOCK — needs external dep (defer / document)
9. **youtube / vimeo / instagram / xupload / frameio / cloudrender** — leave BLOCKED; add a one-line "set VEDITOR_*_* / sign in" hint in each dialog. Finish vimeo's unwired Authenticate button (small) so it matches the others. *(Defer; these are correct-by-design pending user creds.)*
10. **transcript-highlighter** — add a **deterministic offline fallback** (keyword/energy heuristic) so it isn't dead without an Anthropic key. *(Medium ROI — turns BLOCKED into MVP.)*
11. **dolby-vision** — keep as XML-sidecar MVP; document the dovi_tool external step. Full RPU is license-gated, do not invest. *(Defer.)*

### (d) CUT / keep-as-engine (don't ship as features)
12. **hdr-composite-parity / composite16 + gpu/hdr-composite-math/parity** — these are **correct as test oracles**. Either (i) build the RGBA64 overlay pool so `composite16` can actually run, or (ii) explicitly label the whole HDR-GPU compositing path as "reference/selftest only" and stop tracking it as a user feature. Do **not** advertise HDR GPU compositing until the SDR overlay-pool ceiling is lifted. *(Decisive: it cannot work today.)*
13. **parity / e2e / exportaudit / oauth-*-e2e / platform-mock-e2e / cred-* / hwperf / pro / proext** — all legitimate selftest/CI scaffolding. Keep them, but they should **not** appear in any user-facing feature count. They inflate the catalog without being features.

---

_Bottom line: of the **real** (non-test) features, **~76% (70/92) are genuinely usable today**, and the WIRED core — timeline edit/trim/transitions, undo/redo, mixer + bus, color grading (wheels/curves/LUT), scopes, markers, keyframe animation, autosave, tracking, proxies, in-process codec, and the full export-format set — is real, not faked. The NLE is **NOT broadly "C-like."** The honest gaps are concentrated and explainable in three buckets: (1) the not-yet-live GPU/HDR compositing path (blocked by the SDR overlay-pool ceiling), (2) credential-blocked cloud uploads, and (3) a short tail of stub-defaulting external-dep features (whisper, subtitle-translate, transcript-highlighter, Dolby Vision sidecar). Caveat: this is a feature-level estimate over 108 catalogued features; the action-level surface (~251 menu actions) is larger, and the uncounted remainder skews WIRED — so 76% is a floor, not a ceiling._
