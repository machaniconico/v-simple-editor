# Real-Footage Verification Procedure

## Purpose

Automated parity testing to date has been exercised against a single synthetic
640x360 H.264 asset in a headless environment.  This document closes that gap by
providing:

1. A **headless multi-codec automated pass** via `scripts/realfootage_parity.sh`
   that runs both parity selftests across an arbitrary library of real clips.
2. A **manual GUI-vs-export visual verification** procedure for track-matte
   (alpha and luma cases).
3. A **codec/resolution coverage matrix** to be filled in as clips are tested.

The procedures target the track-matte SSOT implemented in TM-1/TM-3/TM-4 and
the general edit↔export parity guarantee established in Sprint 22 / NLE-parity.

All comparisons follow the project's **independent-comparator principle**: parity
is proven against an expectation derived independently of the code under test
(hand-computed pixel values, raw ffmpeg decode, or reference images produced
before the feature was written).  Never generate the reference from the same
code path being verified — that is tautological.

---

## Section A — Headless Multi-Codec Automated Pass

### Selftest entry-points confirmed in `src/main.cpp`

| Selftest | argv switch | env var |
|---|---|---|
| General edit↔export parity | _(env-var only)_ | `VEDITOR_PARITY_SELFTEST=1` |
| Track-matte SSOT parity | `--selftest-trackmatte-parity` | `VEDITOR_TRACKMATTE_PARITY_SELFTEST=1` |

Both selftests respect `VEDITOR_E2E_CLIP=<path>` to point at a specific source
clip.  When the clip is missing they print a warning and exit 0 (CI-tolerant),
so **always supply a real file** when the intent is to exercise codec-specific
paths.

### Running the script

```bash
# Basic usage — run against every clip in footage/:
./scripts/realfootage_parity.sh footage/

# Override binary path (e.g. a debug build):
./scripts/realfootage_parity.sh footage/ build_win/Debug/v-simple-editor.exe
```

Supported extensions (case-insensitive): `.mp4 .mov .mkv .webm .avi .m4v`.

The script exits:
- **0** — every file passed every test
- **1** — at least one file failed at least one test
- **2** — `footage_dir` is missing, not a directory, or contains no video files
  (never silently passes on an empty directory)

### Interpreting the summary table

```
FILE                                          CODEC    RES        parity  trackmatte-parity  RESULT
────────────────────────────────────────────────────────────────────────────────────────────────────
sample_h264_1080p.mp4                         h264     1920x1080  PASS    PASS               PASS
sample_hevc_4k.mp4                            hevc     3840x2160  PASS    PASS               PASS
sample_prores.mov                             prores   1920x1080  PASS    PASS               PASS
```

- **CODEC / RES** are populated by `ffprobe`; if `ffprobe` is absent they show
  `?` — the test still runs, metadata is just unavailable.
- A **TIMEOUT** (300 s per invocation) counts as a FAIL for that cell.
- Each clip exercises two independent selftests (`parity` and
  `trackmatte-parity`); both must pass for `RESULT` to be `PASS`.

### Minimum recommended clip set

Include at least:
- One clip per row of the coverage matrix below.
- One variable-frame-rate (VFR) clip.
- One clip with an alpha channel (e.g. ProRes 4444 `.mov`).
- Clips at SD, 1080p, 4K, and vertical 9:16 if available.

---

## Section B — Manual GUI Preview vs Exported File

This procedure verifies that what is visible in the GUI preview exactly matches
the exported file for track-matte compositions (alpha and luma matte cases).

### Prerequisites

- A project file containing at least one track-matte composition:
  - **Alpha case**: a clip with a pre-multiplied alpha matte layer above it.
  - **Luma case**: a clip with a luminance-based matte layer above it.
- A chosen **reference timestamp** `T` (e.g. `00:00:03.000`) where the matte
  is clearly visible and non-trivial.

### Step 1 — Note the GUI preview frame

1. Open the project in v-simple-editor.
2. Scrub the timeline to timestamp `T`.
3. Take a screenshot of the preview monitor at full resolution.
   - Record the exact frame number or timecode displayed in the UI.
4. Optionally export a PNG from the GUI via **File → Export Frame** at `T`.

### Step 2 — Export the full composition

1. Set the export range to include `T` (a short range around `T` is fine).
2. Choose the target codec (start with H.264; repeat for other codecs in the
   matrix).
3. Export.  Note the output file path `EXPORTED_FILE`.

### Step 3 — Decode the exported file at the same timestamp

Use `ffmpeg` to extract the frame at timestamp `T`:

```bash
# Replace T with your chosen timestamp, e.g. 00:00:03.000
ffmpeg -ss T -i EXPORTED_FILE -frames:v 1 -q:v 1 decoded_frame.png
```

For frame-accurate extraction (especially for B-frame codecs), use:

```bash
ffmpeg -i EXPORTED_FILE -vf "select=eq(pts,PTS_VALUE)" -vsync 0 decoded_frame.png
```

where `PTS_VALUE` is the PTS of the target frame (obtainable via
`ffprobe -show_frames`).

### Step 4 — Visual and numerical diff

**Visual diff:**
```bash
# Side-by-side comparison (requires ImageMagick):
compare -metric PSNR gui_frame.png decoded_frame.png diff.png
```

**Numerical diff (pixel-exact check):**
```bash
# Should output PSNR = inf (identical) for lossless export,
# or > 40 dB for visually transparent lossy codecs.
ffmpeg -i gui_frame.png -i decoded_frame.png \
    -filter_complex psnr -f null -
```

### Step 5 — Pass criterion

- **Matte visible**: the track matte (alpha cutout or luma mask) is present in
  both the GUI screenshot and the decoded export frame — it has not been
  silently dropped.
- **Pixel identity**: for lossless export (PNG, ProRes 4444), PSNR = inf
  (zero difference).  For lossy codecs (H.264, H.265), PSNR > 45 dB across
  the matte boundary region.
- **No silent drop**: if the matte is absent from the exported frame but
  present in the GUI, the test **fails** regardless of PSNR.

Repeat Steps 2–5 for the luma matte case and for each codec in the matrix.

---

## Section C — Codec/Resolution Coverage Matrix

Fill in each cell as clips are tested.  Legend: **PASS** / **FAIL** / **NT**
(not tested).

| Codec | SD (≤ 576p) | 1080p | 4K (2160p) | Vertical 9:16 |
|---|---|---|---|---|
| H.264 | NT | NT | NT | NT |
| H.265 / HEVC | NT | NT | NT | NT |
| ProRes (422 / 4444) | NT | NT | NT | NT |
| VP9 | NT | NT | NT | NT |
| AV1 | NT | NT | NT | NT |
| DNxHD / DNxHR | NT | NT | NT | NT |

**Notes:**
- Include at least one **variable-frame-rate (VFR)** clip (common in screen
  recordings and smartphone footage).
- Include at least one **alpha-channel** clip (ProRes 4444 `.mov` recommended)
  to verify that the alpha matte path survives encode/decode round-trip.
- Mark cells FAIL immediately if either the `parity` or `trackmatte-parity`
  selftest fails for a clip of that codec/resolution combination.
- Update this matrix after each run of `scripts/realfootage_parity.sh` and
  commit the updated file so the coverage record is version-controlled.

---

## Independent-Comparator Principle (reference)

Per the project memory entry added after Sprint 22:

> Reference expectations must be derived independently of the code under test.
> Do not generate selftest reference frames by running the same render pipeline
> being verified — that is tautological and cannot catch systematic errors.
> Use: hand-computed pixel values, raw `ffmpeg` decode of independently
> produced files, or reference images created before the feature was written.

This applies to both the automated selftests invoked by
`scripts/realfootage_parity.sh` and the manual procedure in Section B.
