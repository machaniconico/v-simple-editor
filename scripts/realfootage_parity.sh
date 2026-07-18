#!/usr/bin/env bash
# realfootage_parity.sh — real-footage env-gated selftest harness (US-006)
#
# USAGE
#   realfootage_parity.sh [OPTIONS] <clip>
#   REALFOOTAGE_CLIP=<clip> realfootage_parity.sh [OPTIONS]
#
#   <clip>  Path to a real video file (mp4 / mov / mkv / etc.)
#
# OPTIONS
#   -h, --help            Show this help and exit 0
#
# ENVIRONMENT
#   REALFOOTAGE_CLIP      Alternative to positional <clip> argument
#   REALFOOTAGE_BUILD=1   Run  cmd.exe /c cmake.exe --build build_win --config Release
#                         before executing selftests
#
# OUTPUT FORMAT
#   [REALFOOTAGE_PARITY] PASS: <testname>
#   [REALFOOTAGE_PARITY] FAIL: <testname> <reason>
#
# EXIT CODES
#   0  all selftests passed
#   1  one or more selftests failed
#   2  usage error (missing clip, file not found)
#
# NOTES
#   * Runs env-gated selftests via cmd.exe so env vars reach the Windows .exe
#     (WSL env is not propagated to Windows processes — reference_selftest_wsl_windows_env).
#   * Idempotent: temporary files live under a mktemp -d dir removed on exit.
#   * C++ sources are never modified by this script.

set -euo pipefail

# ── tmpdir ──────────────────────────────────────────────────────────────────
SCRIPT_TMPDIR="$(mktemp -d)"
trap 'rm -rf "$SCRIPT_TMPDIR"' EXIT

# ── usage ────────────────────────────────────────────────────────────────────
usage() {
    # Extract only the header block: lines starting with '# ' up to the first non-comment line
    sed -n '2,/^[^#]/{ /^# /s/^# //p }' "$0"
    exit 0
}

# ── arg parsing ──────────────────────────────────────────────────────────────
CLIP="${REALFOOTAGE_CLIP:-}"

for arg in "$@"; do
    case "$arg" in
        -h|--help) usage ;;
        -*) echo "Unknown option: $arg" >&2; exit 2 ;;
        *)  CLIP="$arg" ;;
    esac
done

if [[ -z "$CLIP" ]]; then
    echo "ERROR: real clip path required (arg \$1 or env REALFOOTAGE_CLIP)" >&2
    echo "Run  $(basename "$0") --help  for usage." >&2
    exit 2
fi

if [[ ! -f "$CLIP" ]]; then
    echo "ERROR: clip not found: '$CLIP'" >&2
    exit 2
fi

# ── locate repo root (needed for build and binary paths) ─────────────────────
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ── optional build ────────────────────────────────────────────────────────────
if [[ "${REALFOOTAGE_BUILD:-0}" == "1" ]]; then
    echo "[REALFOOTAGE_PARITY] INFO: REALFOOTAGE_BUILD=1 — building..."
    cmd.exe /c "cd /d \"$(wslpath -w "$REPO_ROOT")\" && cmake.exe --build build_win --config Release"
fi

# ── locate Windows binary (WSL → Windows path) ───────────────────────────────
EXE_WSL="${REPO_ROOT}/build_win/Release/v-simple-editor.exe"

if [[ ! -f "$EXE_WSL" ]]; then
    echo "[REALFOOTAGE_PARITY] INFO: binary not found at '${EXE_WSL}' — skipping exe-based selftests"
    EXE_WIN=""
else
    EXE_WIN="$(wslpath -w "$EXE_WSL")"
fi

CLIP_WIN="$(wslpath -w "$CLIP" 2>/dev/null || echo "$CLIP")"

# ── selftest registry ─────────────────────────────────────────────────────────
# Dynamically populated from grep:
#   grep -rhoP 'VEDITOR_[A-Z_]+_SELFTEST' src/ | sort -u
# All env-gated runtime selftests; sorted alphabetically; no duplicates.
SELFTESTS=(
    VEDITOR_AFFINITY_SELFTEST
    VEDITOR_ANIMEXPORT_SELFTEST
    VEDITOR_AUDIOMIXER_SELFTEST
    VEDITOR_AUDIORESTORE_SELFTEST
    VEDITOR_BATCHEXPORT_SELFTEST
    VEDITOR_BLENDER_SELFTEST
    VEDITOR_BRUSH_SELFTEST
    VEDITOR_CAPTION_SELFTEST
    VEDITOR_CHROMA_SELFTEST
    VEDITOR_CLOUDRENDER_SELFTEST
    VEDITOR_COLLAB_SELFTEST
    VEDITOR_COLORMATCH_SELFTEST
    VEDITOR_DAVINCI_SELFTEST
    VEDITOR_EASING_SELFTEST
    VEDITOR_EFFECT_PARAM_SCHEMA_SELFTEST
    VEDITOR_EXPORTAUDIT_SELFTEST
    VEDITOR_FCPXML_SELFTEST
    VEDITOR_FRAMEIO_SELFTEST
    VEDITOR_HDR_SELFTEST
    VEDITOR_HWPERF_SELFTEST
    VEDITOR_IMPORT_SELFTEST
    VEDITOR_INSTAGRAM_SELFTEST
    VEDITOR_LOUDNESS_SELFTEST
    VEDITOR_LOWERTHIRD_SELFTEST
    VEDITOR_MOBILE_SELFTEST
    VEDITOR_MOGRAPH_SELFTEST
    VEDITOR_MULTICAM_SELFTEST
    VEDITOR_NODEGRAPH_SELFTEST
    VEDITOR_OBS_SELFTEST
    VEDITOR_PARITY_SELFTEST
    VEDITOR_PLANAR_SELFTEST
    VEDITOR_PLANAR_TRACKER_SELFTEST
    VEDITOR_PROEXT_SELFTEST
    VEDITOR_PROJTMPL_SELFTEST
    VEDITOR_PRO_SELFTEST
    VEDITOR_SHORTCUT_SELFTEST
    VEDITOR_SMARTEDIT_SELFTEST
    VEDITOR_SNSPACK_SELFTEST
    VEDITOR_SOCIAL_SELFTEST
    VEDITOR_SUBXLAT_SELFTEST
    VEDITOR_TEXTEXPORT_SELFTEST
    VEDITOR_TRACKMATTE_EXPORT_INTEGRATION_SELFTEST
    VEDITOR_TRACKMATTE_PARITY_SELFTEST
    VEDITOR_TRACKMATTE_SELFTEST
    VEDITOR_TWITCH_SELFTEST
    VEDITOR_VFX_SELFTEST
    VEDITOR_VIMEO_SELFTEST
    VEDITOR_WATERMARK_SELFTEST
    VEDITOR_WORKFLOW_SELFTEST
    VEDITOR_XUPLOAD_SELFTEST
    VEDITOR_YOUTUBE_SELFTEST
)

# ── run selftests ─────────────────────────────────────────────────────────────
OVERALL_RC=0
TIMEOUT_SECS=120

echo "[REALFOOTAGE_PARITY] INFO: clip = $CLIP"
echo "[REALFOOTAGE_PARITY] INFO: ${#SELFTESTS[@]} env-gated selftests to run"

for TEST in "${SELFTESTS[@]}"; do
    if [[ -z "$EXE_WIN" ]]; then
        echo "[REALFOOTAGE_PARITY] FAIL: $TEST binary_not_found"
        OVERALL_RC=1
        continue
    fi

    # Build the cmd.exe command string: set envvars then run the exe
    # Use cmd.exe canonical quoting: set "VAR=value" to handle spaces in paths
    CMD_STR="set \"${TEST}=1\" && set \"VEDITOR_E2E_CLIP=${CLIP_WIN}\" && \"${EXE_WIN}\""

    RC=0
    timeout "$TIMEOUT_SECS" cmd.exe /c "$CMD_STR" \
        >"${SCRIPT_TMPDIR}/${TEST}.stdout" 2>"${SCRIPT_TMPDIR}/${TEST}.stderr" || RC=$?

    if [[ $RC -eq 0 ]]; then
        echo "[REALFOOTAGE_PARITY] PASS: $TEST"
    elif [[ $RC -eq 124 || $RC -eq 142 ]]; then
        echo "[REALFOOTAGE_PARITY] FAIL: $TEST timeout_after_${TIMEOUT_SECS}s"
        OVERALL_RC=1
    else
        REASON="exit_code_${RC}"
        # Append first stderr line if available for triage
        FIRST_ERR="$(head -1 "${SCRIPT_TMPDIR}/${TEST}.stderr" 2>/dev/null | tr -d '\r' || true)"
        if [[ -n "$FIRST_ERR" ]]; then
            REASON="${REASON}:${FIRST_ERR}"
        fi
        echo "[REALFOOTAGE_PARITY] FAIL: $TEST $REASON"
        OVERALL_RC=1
    fi
done

# ── real-clip existence verification (best-effort) ───────────────────────────
# If no headless export CLI path exists, confirm the clip is readable at minimum.
echo "[REALFOOTAGE_PARITY] INFO: verifying clip readability (no headless export path available)"
if [[ -r "$CLIP" && -s "$CLIP" ]]; then
    echo "[REALFOOTAGE_PARITY] PASS: clip_readable"
else
    echo "[REALFOOTAGE_PARITY] FAIL: clip_readable file_not_readable_or_empty"
    OVERALL_RC=1
fi

# ── summary ───────────────────────────────────────────────────────────────────
if [[ $OVERALL_RC -eq 0 ]]; then
    echo "[REALFOOTAGE_PARITY] PASS: all_tests"
else
    echo "[REALFOOTAGE_PARITY] FAIL: one_or_more_tests_failed" >&2
fi

exit $OVERALL_RC
