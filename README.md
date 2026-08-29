# V Simple Editor

**Simple UI, Full-Featured Video Editor**

A professional video editing application built from scratch with C++17, Qt6, and FFmpeg. Designed with a clean, intuitive interface while packing advanced features like After Effects-style compositing, AI-powered editing tools, and GPU-accelerated preview.

---

## Features (90+ functions across 13 phases)

### Core Editing
- Video import & decoding (MP4, MKV, MOV, WebM, FLV)
- Multi-track timeline with drag & drop
- Cut, split, trim at playhead
- Copy / paste / ripple delete
- Undo / redo (unlimited history)
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

### Transitions & Effects
- 9 transition types (crossfade, wipe, slide, zoom, etc.)
- Image overlay & Picture-in-Picture
- 9 video effects (blur, sharpen, sepia, negative, etc.)
- Color correction (10 parameters: brightness, contrast, saturation, hue, temperature, tint, gamma, highlights, shadows, exposure)
- LUT import (.cube format + 4 built-in LUTs)
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
- Mask & track mattes
- Particle system (7 presets)
- 3D camera (dolly, pan, orbit, zoom)
- Expression engine (wiggle, noise, ease)
- Shape layers (8 primitives)
- Text animator (15 types)
- Tracker link (motion track to effect)
- Pre-compose / nesting
- Rotoscope (path interpolation)
- Warp & distortion (9 effects)

### Export & Workflow
- 13 export presets (YouTube, Twitter, Instagram, TikTok, 4K, ProRes, etc.)
- Hardware encoding (NVENC / QSV / AMD AMF with available software fallbacks)
- Codec auto-detection
- Project save/load (.veditor JSON format)
- Auto-save & crash recovery
- Proxy editing (low-res preview, full-res export)
- Render queue (batch export)
- Screen recorder (cross-platform)
- Speed ramp (variable speed with easing)
- Timeline markers & YouTube chapter export
- Multi-camera editing (audio sync + camera switch)
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
args = ["--mcp-stdio", "--port", "8765", "--token", "<token>"]
```

このブリッジは stdin/stdout の「1 行 1 JSON-RPC」を localhost の HTTP MCP サーバへ中継します。エディタが起動していない場合はプロセスを落とさず、`-32603` と `editor not running` を返します。ポートを変更した場合は `--port` も合わせて変更してください。

### エディタ内 AI チャット

**表示 > AI チャット** で AI チャット Dock を開きます。Dock は `claude` CLI を headless で `-p <prompt> --output-format stream-json --verbose --mcp-config <一時json> --strict-mcp-config --allowedTools mcp__veditor` のオプション付きで起動し、MCP 設定を一時ファイルで渡して `veditor` のツールだけを許可します。子プロセスの環境から `ANTHROPIC_API_KEY` と `ANTHROPIC_AUTH_TOKEN` を除去するため、ログイン済みの Claude Pro / Max のサブスク枠で動作します。使用する CLI 名は QSettings の `aiChatCommand` で変更できます。

`claude` が PATH に無い場合は、先に次を実行してください。

```sh
npm i -g @anthropic-ai/claude-code
```

### 安全性

MCP の変更系ツールは確認ダイアログを出さず、常に自動承認されます。編集前に Undo 状態を 1 回だけ保存するため、通常は LLM の 1 操作を **Ctrl+Z 1 回**で戻せます。`run_command` は各メニューアクションが自分で Undo を積むため、MCP 側では二重に積みません。`add_caption` は字幕エディタ側の状態を変更するため Ctrl+Z の対象外です。トークンは他人に渡さないでください。サーバは `127.0.0.1` にのみバインドされるため、LAN 上の別の端末からは接続できません。

### MCP ツール

| 名前 | 何をするか | Undo |
|---|---|---|
| `get_project_info` | プロジェクト情報を読み取る | なし |
| `get_timeline` | タイムラインを読み取る | なし |
| `get_captions` | 字幕を読み取る | なし |
| `list_commands` | メニューの全アクション（実測 231 件）を安定 ID（例: `file.11`, `tools.41`）付きで一覧する | なし |
| `run_command` | `list_commands` のアクションを実行する | アクション依存（MCP 側では追加しない） |
| `split_clip` | クリップを分割する | あり |
| `delete_clip` | クリップを削除する | あり |
| `move_clip` | クリップを移動する | あり |
| `set_clip_property` | クリップのプロパティを変更する | あり |
| `add_caption` | 字幕を追加する | なし（Ctrl+Z 対象外） |
| `set_playhead` | 再生ヘッドを移動する | なし |
| `undo` | 直前の編集を元に戻す | なし |
| `redo` | 元に戻した編集をやり直す | なし |

---

## Supported Formats

| Codec | Decode | Encode |
|-------|--------|--------|
| H.264 | Yes | Yes (x264 / NVENC / QSV / AMF) |
| H.265 (HEVC) | Yes | Yes (x265 / NVENC / QSV / AMF) |
| AV1 | Yes | Yes (AOM / SVT-AV1 / NVENC / QSV / AMF) |
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

### AMD Radeon support

On Windows, decoding uses vendor-neutral D3D11VA and preview/effects use
OpenGL. Supported Radeon GPUs can encode H.264, HEVC, and AV1 through AMD AMF
when the installed AMD driver exposes the requested encoder. If AMF cannot be
opened, export tries the matching software candidates in the active FFmpeg
build; it reports an error if none is installed instead of silently changing
to a different codec family. AV1 hardware support depends on the Radeon GPU
generation and driver.

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

`setup.bat` と Windows CI は Modern edition を構成し、AOM の AV1
software fallback を同梱します。`build.bat --classic` は H.264/HEVC の
software 基盤を維持しますが、対応する FFmpeg と GPU driver がある場合は
hardware AV1 も実行時検出されます。`build.bat --modern` は AOM を追加します。

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
`kArgvSelftests[]` table in `src/main.cpp` (SSOT, 54 entries). Four entry
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
3. vcpkg install ffmpeg with the `amf` and `aom` features
4. Qt 6.7.0 install (qtmultimedia / qtsvg / qtimageformats)
5. cmake configure + Release build
6. smoke: `--selftest=list` / `--selftest=tracker-preset` / `--selftest=hdr-routing` / `--selftest=radeon-amf`
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
| `kArgvSelftests` | `src/main.cpp` (anonymous ns) | Selftest dispatch table (54 entries × 5 fields: name / envVar / fn / needsQApp / description) |

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
    ├── main.cpp                 # Entry point + kArgvSelftests[] table + 54 selftest functions
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
- [x] Selftest argv-switch SSOT: `kArgvSelftests[]` 54-entry table, `--selftest=<name>` + `VEDITOR_*_SELFTEST` + `--selftest={list,help,all}` + unknown-name guard
- [x] CI workflow draft (`.github/workflows/selftest.yml`, `workflow_dispatch` only)
- [ ] CI green run + auto trigger on push / PR
- [ ] `src/main.cpp` split refactor (selftest functions → `src/selftests/`, dispatcher → SelftestRegistry module)
- [ ] CRLF → LF wholesale normalization (per repo-root `.gitattributes`)
- [ ] Streaming pipeline real-authentication harness (YouTube / Vimeo / Twitch / X / Instagram currently no-op stub for CI)

---

## License

MIT
