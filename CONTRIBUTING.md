# Contributing to V Simple Editor

Thanks for the interest! This document explains how to set up a dev env,
run the selftest suite, and submit changes.

## Quick start (Windows, 15 minutes)

```bash
# 1. clone
git clone https://github.com/machaniconico/v-simple-editor.git
cd v-simple-editor

# 2. one-shot setup (installs Git / CMake / VS 2022 Build Tools / vcpkg / Qt6 / FFmpeg / Python)
# right-click setup.bat -> "Run as administrator"
setup.bat

# 3. enumerate the selftest entry points
./build_win/Release/v-simple-editor.exe --selftest=list

# 4. run one selftest to confirm the build is healthy
./build_win/Release/v-simple-editor.exe --selftest=tracker-preset
```

See the main [README.md](README.md#build) for macOS / Linux instructions.

## Repository layout

```
src/
├── main.cpp                # Entry point + 54 selftest functions + kArgvSelftests[]
├── libavcore/              # In-process FFmpeg integration (SSOT for decode/encode/probe)
├── tlrender/               # Timeline → QImage rendering SSOT (preview ≡ export)
├── trackmatte/             # Track matte compositing SSOT (4 matte types)
├── clipgeom.h/cpp          # Clip geometry SSOT (transform parity)
├── TrackerPreset.{h,cpp}   # Motion tracker preset system
├── PlanarTrackerPreset.*   # Planar tracker preset system
├── MotionTrackerDialog.*   # Motion tracker dialog + preset UX
├── PlanarTrackerDialog.*   # Planar tracker dialog + preset UX
└── (90+ feature modules — see README.md "Project Structure")

.github/workflows/
└── selftest.yml            # CI smoke (Windows MSVC, workflow_dispatch only for now)

scripts/
└── realfootage_parity.sh   # Real-footage selftest harness (optional)

.omc/
└── PR_DESCRIPTION.md       # Latest PR body draft
```

## Architecture invariants

A handful of SSOTs (single source of truth) are load-bearing — don't
duplicate their logic, route into them:

| SSOT | Module | Owns |
|---|---|---|
| `tlrender::renderFrameAt` | `src/tlrender/` | Timeline → QImage. Every export path goes here. |
| `trackmatte::composite` | `src/trackmatte/` | Track matte compositing (premul ARGB32 + QPainter SourceOver). Byte-equivalent to matte-free path. |
| `clipgeom` | `src/clipgeom.{h,cpp}` | Clip geometry (scale / translate / rotation), layer-center anchored. |
| `libavcore` | `src/libavcore/` | In-process FFmpeg helpers (Decode / Encode / Probe / VideoFilterGraph / Concat). Replaces per-call `ffmpeg.exe` subprocess. |
| `kArgvSelftests` | `src/main.cpp` (anon ns) | Selftest dispatch table (54 entries × 5 fields). |

**Why these matter**: preview ≠ export was historically the #1 NLE
parity bug source. The rendering SSOTs collapse preview and every export
path into the same call graph; selftests enforce it.

## Adding a new selftest

1. Implement `int runFooSelftest()` in `src/main.cpp` (or wherever the
   feature module lives) — return 0 on pass, non-zero on fail.
2. Add one line to `kArgvSelftests[]` in `src/main.cpp`:

   ```cpp
   { "foo",  "VEDITOR_FOO_SELFTEST",  runFooSelftest,  /*needsQApp=*/true,
     "One-line description shown by --selftest=help" },
   ```

3. That's it — argv-switch (`--selftest=foo`), env-gate
   (`VEDITOR_FOO_SELFTEST=1`), full sweep (`--selftest=all`), list, help,
   and unknown-name guard all activate automatically.

### When `needsQApplication` matters

- `false` — selftest is pure in-memory or only touches libavcore /
  QSettings (no QWidget, no Qt event loop). Dispatched **before** QApplication
  is constructed, so it never hangs on missing GUI infrastructure (this
  matters in WSL → Windows env-propagation edge cases).
- `true` — selftest needs QApplication (QWidget, MainWindow seed, Qt
  signal/slot, libav with Qt singletons). Dispatched **after** `QApplication
  app(argc, argv);` runs.

If unsure, set `true` — it's the safer default. The selftest will still
get the live Qt context.

## Submitting changes

1. Branch from `main`: `git checkout -b feat/<topic>` or `fix/<topic>`.
2. Write the change. Keep commits atomic and focused (one logical change
   per commit). Use conventional-commit style:

   ```
   feat(scope): one-line summary

   Longer body explaining *why* and any non-obvious decisions.
   ```

   Scopes used in the repo: `selftest`, `libavcore`, `tracker-presets`,
   `trackmatte`, `transform-parity`, `nle-parity`, `audio-seek`, `proxy-manager`,
   `ai-highlight`, `stabilizer`, `docs`, `ci`, `chore`, `refactor`, `fix`.

3. Run the selftests covering your change:

   ```bash
   # everything (long)
   ./build_win/Release/v-simple-editor.exe --selftest=all

   # or just the entries you touched
   ./build_win/Release/v-simple-editor.exe --selftest=<name>
   ```

4. Open a PR. The body should include:
   - Summary (what changed, in 1-3 sentences)
   - Why (the bug / feature / refactor motive)
   - Test plan (which selftests pass, manual UI steps if any)

   See [`.omc/PR_DESCRIPTION.md`](.omc/PR_DESCRIPTION.md) for a worked
   example covering 28+ commits.

## Local CI dry-run (optional)

The Windows MSVC smoke workflow at `.github/workflows/selftest.yml` is
currently `workflow_dispatch` only. To approximate it locally:

```bash
# from a vcvarsall-loaded shell
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64
cmake --build build_win --config Release
./build_win/Release/v-simple-editor.exe --selftest=list
./build_win/Release/v-simple-editor.exe --selftest=tracker-preset
./build_win/Release/v-simple-editor.exe --selftest=hdr-routing
```

## Multi-engine PRD-driven workflow (advanced)

For larger refactors we use the [dualralph](https://github.com/anthropics/claude-code)
skill: design a PRD with 1+ stories, dispatch implementation to a Claude
executor (or OpenCode / Codex when available), and verify via the
selftest suite. PRDs are stored under `.omc/prd-*-DONE.json` for audit.

This is not required for normal contributions — just useful when you're
splitting a big change across multiple stories.

## CRLF / EOL

The repo root has a `.gitattributes` that normalizes most text files to
LF. If git shows `warning: CRLF will be replaced by LF` on your edits,
that's expected — `.gitattributes` is doing its job.

## License

MIT (same as the main project).
