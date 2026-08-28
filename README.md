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
- Hardware encoding (NVENC / QSV / AMF auto-detect)
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
args = ["--mcp-stdio", "--port", "8765"]
env = { VEDITOR_MCP_TOKEN = "<token>" }
```

このブリッジは stdin/stdout の「1 行 1 JSON-RPC」を localhost の HTTP MCP サーバへ中継します。HTTP/ネットワークのエラーはプロセスを落とさず、`-32603` と次のメッセージを返します: `invalid token`（HTTP 401、トークン不一致）、`editor did not respond in time`（リクエストタイムアウト）、`editor not running`（接続拒否）、`unexpected editor response`（その他の HTTP/ネットワークエラー）。ポートを変更した場合は `--port` も合わせて変更してください。

`--token <token>` を args に渡す形も受け付けますが、プロセス一覧からトークンが見えるためブリッジは stderr に警告を出します。`VEDITOR_MCP_TOKEN` が設定されている場合はそちらが優先されます。ブリッジの HTTP リクエストのタイムアウトは `VEDITOR_MCP_TIMEOUT_MS`（1000 以上のミリ秒）で変更できます。

### エディタ内 AI チャット

**表示 > AI チャット** で AI チャット Dock を開きます。Dock は `claude` CLI を headless で `-p --output-format stream-json --verbose --mcp-config <一時json> --strict-mcp-config [--resume <sessionId>] --allowedTools mcp__veditor` のオプション付きで起動し、MCP 設定を一時ファイルで渡して `veditor` のツールだけを許可します。プロンプトは argv ではなく stdin で渡します。会話継続時は前回の `session_id` を `--resume <sessionId>` で指定します（`--allowedTools` の直前）。子プロセスの環境から `ANTHROPIC_API_KEY` と `ANTHROPIC_AUTH_TOKEN` を除去するため、ログイン済みの Claude Pro / Max のサブスク枠で動作します。使用する CLI 名は QSettings の `aiChatCommand` で変更できます。

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
| `get_timeline` | タイムラインを読み取る | なし |
| `get_frame` | 指定時刻の合成フレームを PNG で返す（既定 640px 以下・1MB 以内） | なし |
| `get_captions` | 字幕を読み取る | なし |
| `get_export_status` | `export_video` ジョブの状態・進捗を返す | なし |
| `list_commands` | メニュー直下のお気に入り登録可能なアクションを一覧する（サブメニュー内の項目は含まない）。ID は `<メニューキー>.<メニュー内の通し番号>`（例: `file.11`, `tools.41`）で、表示文言の変更では変わらないが、メニューの途中にアクションが追加されると後続の番号がずれる（位置依存）ため、実行前に `list_commands` で id を確認する。`query` 省略時は全件（約 230 件・JSON で約 60KB）を返すので通常は `query` で id / 表示名 / メニュー名を部分一致フィルタする。各項目に `risk`（safe / blocking / quit）と `enabled` を返す | なし |
| `run_command` | `list_commands` のアクションを ID で実行する（`allowBlocking` は既定 false。応答の `undoRecorded` で Ctrl+Z / `undo` の対象になったか確認できる） | アクション依存（MCP 側では追加しない） |
| `export_video` | タイムラインを動画へ非同期で書き出し、`jobId` を返す。音声はトリム・分割・並べ替え・音量・ミュートを反映したタイムラインのミックスを ffmpeg で作ってから多重化する（`audioCodec` / `audioBitrate` 省略時は aac / 192 kbps） | なし |
| `import_media` | ダイアログなしで素材を指定トラックへ取り込む（`kind` 既定 `auto`: 映像があれば V/A の組、無いファイル（BGM・ナレーション）は音声トラックだけ。`video` / `audio` で片側だけ。開けないファイルはエラー） | あり |
| `save_project` | プロジェクトを指定パスへ保存する（ダイアログなし。拡張子は GUI と同じ `.veditor` を推奨） | なし |
| `open_project` | 指定パスのプロジェクトを読み込む（未保存確認なし） | なし |
| `select_clip` | 指定クリップを選択する（`kind` / `trackIndex` 省略時は video / 0） | なし |
| `clear_selection` | 選択をすべて解除する | なし |
| `split_clip` | クリップを分割する | あり |
| `delete_clip` | クリップを削除する | あり |
| `move_clip` | クリップを移動する（`newTrackIndex` で別トラックへ。既定プロジェクトは V1/A1 の 1 段なので、先に `run_command` の「ビデオトラックを追加」を実行する。存在しないトラックを指定するとエラー文でそのコマンド id を案内する） | あり |
| `set_clip_property` | クリップのプロパティ（volume / opacity / speed / pan / videoScale）を変更する。`speed` はリンクした音声クリップにも同時に適用される | あり |
| `trim_clip` | edge=in は開始位置を保ったまま timeSec 時点の内容を新しい先頭にし、以降が (timeSec−開始) だけ左へ詰まる（RippleIn）。edge=out は末尾を timeSec にし後続が詰まる（RippleOut）。kind は video のみだが、同じ linkGroup の音声クリップも同じ量だけトリムされる（ripple 既定 true） | あり |
| `set_transition` | V1 のクリップにトランジションを設定する（FadeIn は先頭、その他は末尾、None で解除） | あり |
| `add_text_overlay` | V1 にテキスト／テロップを追加する（時刻は秒、位置は 0..1。区間と重なる全クリップに付くのでクリップ境界をまたいでも表示される） | あり |
| `add_caption` | 字幕エディタの一覧に 1 件追加する（タイムラインへは `apply_captions` で反映） | なし（Ctrl+Z 対象外） |
| `apply_captions` | 字幕エディタの字幕を V1 の 1 語字幕オーバーレイとしてタイムラインへ適用する（既存の生成済み 1 語字幕は置き換え） | あり（タイムライン側のみ。字幕エディタの一覧は戻らない） |
| `remove_caption` | 字幕エディタの一覧から `index`（`get_captions` の `captions[].index`）の字幕を 1 件削除する | なし（Ctrl+Z 対象外） |
| `clear_captions` | 字幕エディタの一覧を空にする | なし（Ctrl+Z 対象外） |
| `set_playhead` | 再生ヘッドを移動する | なし |
| `undo` | 直前の編集を元に戻す | なし |
| `redo` | 元に戻した編集をやり直す | なし |

MCP サーバの自己テストは `--selftest=mcp` または `VEDITOR_MCP_SELFTEST=1` で実行できます（実装: `src/selftests/mcp_selftest.cpp`、ゲート G1..G108）。

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
`kArgvSelftests[]` table in `src/selftests/SelftestRegistry.cpp` (SSOT, 182 entries). Four entry
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
| `kArgvSelftests` | `src/selftests/SelftestRegistry.cpp` | Selftest dispatch table (182 entries × 5 fields: name / envVar / fn / needsQApp / description) |

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
- [x] Selftest argv-switch SSOT: `kArgvSelftests[]` 182-entry table in `src/selftests/SelftestRegistry.cpp`, `--selftest=<name>` + `VEDITOR_*_SELFTEST` + `--selftest={list,help,all}` + unknown-name guard
- [x] CI workflow draft (`.github/workflows/selftest.yml`, `workflow_dispatch` only)
- [ ] CI green run + auto trigger on push / PR
- [x] `src/main.cpp` split refactor (selftest functions → `src/selftests/`, dispatcher → SelftestRegistry module)
- [ ] CRLF → LF wholesale normalization (per repo-root `.gitattributes`)
- [ ] Streaming pipeline real-authentication harness (YouTube / Vimeo / Twitch / X / Instagram currently no-op stub for CI)

---

## License

MIT
