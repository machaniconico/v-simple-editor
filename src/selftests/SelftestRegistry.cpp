// src/selftests/SelftestRegistry.cpp
// PRD-SPLIT-MAIN-1: selftest dispatch table + 3 helper functions.
// The run<Foo>Selftest() bodies remain in src/main.cpp; this file only
// owns the routing (struct table + dispatchPre/Post/validateUnknown).

#include "SelftestRegistry.h"

#include <cstring>
#include <cmath>
#include <iostream>
#include <QString>
#include <QStringList>
#include <QCoreApplication>
#include <QDebug>
#include "../AutoColor.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../util/Logger.h"

// ---------------------------------------------------------------------------
// Forward declarations for the selftest functions implemented in src/main.cpp.
// Kept alphabetically sorted for ease of maintenance.
// ---------------------------------------------------------------------------
int runAIHighlightSelftest();
int runAcesColorSelftest();
int runAeFxBlurSelftest();
int runAeFxColorSelftest();
int runAeFxColor2Selftest();
int runAeFxDistortSelftest();
int runAeFxDistort2Selftest();
int runAeFxGenerateSelftest();
int runAeFxStylizeSelftest();
int runAdjustmentLayerSelftest();
int runAffinitySelftest();
int runAscCdlExportSelftest();
int runAtempoResolveSelftest();
int runOAuthMockE2eSelftest();
int runOAuthRefreshE2eSelftest();
int runAudioChannelMapSelftest();
int runAnimExportSelftest();
int runAudioBusSelftest();
int runAudioClipDragUndoSelftest();
int runAudioMixerSelftest();
int runAudioRestoreSelftest();
int runAutoClipGenSelftest();
int runAutoColorSelftest();
int runAutoColorPreserveGradeSelftest();
int runAutoDuckingSelftest();
int runAutoMatteSelftest();
int runBatchExportSelftest();
int runBezierEasingSelftest();
int runBlenderSelftest();
int runBroadcastCaptionSelftest();
int runCapcutCaptionSelftest();
int runCaptionSelftest();
int runChromaSelftest();
int runClipColorSelftest();
int runClipCurvesSelftest();
int runClipLutSelftest();
int runClipMaskSelftest();
int runClipParentParitySelftest();
int runClipIdtSelftest();
int runClipOdtSelftest();
int runCloudRenderSelftest();
int runCollabSelftest();
int runColorMatchApplySelftest();
int runColorMatchSelftest();
int runCommandSearchSelftest();
int runCredAuditLogSelftest();
int runCredTtlSelftest();
int runCredentialVaultSelftest();
int runDavinciSelftest();
int runDolbyVisionSelftest();
int runDvTimelineSelftest();
int runEdlExportSelftest();
int runE2eSelftest();
int runEasingSelftest();
int runEasingPresetsSelftest();
int runEffectKeyframeParitySelftest();
int runEffectPresetSelftest();
int runEffectTimingSelftest();
int runExposureAidsSelftest();
int runExportAuditSelftest();
int runExportRangeSelftest();
int runFrameExportSelftest();
int runFcpxmlSelftest();
int runFreezeFrameSelftest();
int runFrameIoSelftest();
int runGradeWheelWiringSelftest();
int runGraphEditorSelftest();
int runGradeLggSerializationSelftest();
int runHdrRoutingSelftest();
int runHdrSelftest();
int runHslSecondarySelftest();
int runHwPerfSelftest();
int runImportIngestSelftest();
int runImportSelftest();
int runInstagramSelftest();
int runKeyframeAnimParitySelftest();
int runKeyframeLoopSelftest();
int runLayerStyleSelftest();
int runLayerStyleUiSelftest();
int runLibavcoreDecodeSelftest();
int runLibavcoreEncodeSelftest();
int runLiveMatteResolveSelftest();
int runLoudnessExportWireSelftest();
int runLoudnessSelftest();
int runLowerThirdSelftest();
int runMatte16ParitySelftest();
int runMediaPoolDragSelftest();
int runMediaPoolSelftest();
int runMobileSelftest();
int runMographSelftest();
int runMotionBlurP2Selftest();
int runMotionBlurParitySelftest();
int runMotionPresetSelftest();
int runMultiCamSelftest();
int runNestSequenceSelftest();
int runObsSelftest();
int runOnionSkinSelftest();
int runParitySelftest();
int runPlanarSelftest();
#if __has_include("PlanarTrackerPreset.h")
int runPlanarPresetSelftest();
#define HAVE_PLANARTRACKER_PRESET 1
#endif
int runPlatformMockE2eSelftest();
int runPptxExportSelftest();
int runProExtSelftest();
int runProSelftest();
int runProjTmplSelftest();
int runProjectPresetSelftest();
int runProxySelftestV2();
int runRenderQueueAcesDecisionSelftest();
int runRenderQueueFpsRationalSelftest();
int runRightclickPausePrefSelftest();
int runRippleDeleteSelftest();
int runRgbParadeSelftest();
int runShortcutSelftest();
int runSilenceCutSelftest();
int runBeatDetectSelftest();
int runSafeZoneSelftest();
int runSmartEditSelftest();
int runSmartRenderSelftest();
int runSnsCoverSelftest();
int runSnsFitSelftest();
int runSocialSelftest();
int runSpatialPathSelftest();
int runSpectralEditSelftest();
int runStereoPanSelftest();
int runSubtitleCpsSelftest();
int runSubtitleKaraokeSelftest();
int runSubXlatSelftest();
int runSwsColorSelftest();
int runTextBasedEditSelftest();
int runTextSpacingSelftest();
int runText3dPreviewSelftest();
int runTextExportSelftest();
int runThreePointEditSelftest();
int runTrackMatteExportIntegrationSelftest();
int runTrackMatteParitySelftest();
int runTrackMatteReindexSelftest();
int runTrackMatteRm5ReorderSelftest();
int runTrackMatteRm6DuplicateSelftest();
int runTranscriptHighlighterOfflineSelftest();
int runTranscriptHighlighterSelftest();
int runTrackerPresetSelftest();
int runTrimOpsSelftest();
int runTwitchSelftest();
int runVfxSelftest();
int runVersionedSaveSelftest();
int runVideostabDeshakeSelftest();
int runVimeoSelftest();
int runWatermarkSelftest();
int runWbEyedropperSelftest();
int runWhisperTranscribeSelftest();
int runWhisperWordTimingSelftest();
int runWorkflowSelftest();
int runWorkspaceSelftest();
int runXUploadSelftest();
int runYoutubeSelftest();
int runYtdlpDownloaderSelftest();
int runPremiereXmlSelftest();
int runYoutubeChapterSelftest();
int runAutoProxyPolicySelftest();
int runCompositeFrameCacheSelftest();
int runPlaybackQualityPolicySelftest();
int runGpuCompositeFlagSelftest();
int runGpuCompositeMathSelftest();
int runHdrCompositeMathSelftest();
int runHdrExport16Selftest();
int runHdrIngestColorMetaSelftest();
int runHdrOverlayPolicySelftest();
int runGpuCompositeParitySelftest();
int runGpuCompositeParity16MatteSelftest();
int runGpuIdtParitySelftest();
int runGpuIdtMatteParitySelftest();
int runHdrCompositeParitySelftest();
int runPreview16PolicySelftest();

namespace {

bool audioClipDragUndoRequire(bool condition,
                              const char *gate,
                              const QString &message,
                              int &passed,
                              int &failed)
{
    if (condition) {
        ++passed;
        std::cout << "[audio-clip-drag-undo] PASS " << gate << '\n';
        return true;
    }

    ++failed;
    std::cerr << "[audio-clip-drag-undo] FAIL " << gate << ": "
              << message.toStdString() << '\n';
    return false;
}

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) <= 1e-9;
}

ClipInfo makeAudioClipDragUndoClip()
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("audio-clip-drag-undo.wav");
    clip.displayName = QStringLiteral("audio-clip-drag-undo");
    clip.duration = 10.0;
    return clip;
}

double selectedAudioPan(const Timeline &timeline)
{
    const auto &tracks = timeline.audioTracks();
    if (tracks.isEmpty() || !tracks[0] || tracks[0]->clips().isEmpty())
        return 0.0;
    return tracks[0]->clips()[0].pan;
}

double selectedAudioVolume(const Timeline &timeline)
{
    const auto &tracks = timeline.audioTracks();
    if (tracks.isEmpty() || !tracks[0] || tracks[0]->clips().isEmpty())
        return 0.0;
    return tracks[0]->clips()[0].volume;
}

bool sameDouble(double a, double b)
{
    return std::fabs(a - b) <= 1e-9;
}

} // namespace

int runAutoColorPreserveGradeSelftest()
{
    const int legacyResult = runAutoColorSelftest();

    int passed = 0;
    int failed = 0;
    autocolor::FrameStats stats;
    stats.r = {80, 120, 100.0};
    stats.g = {90, 130, 110.0};
    stats.b = {130, 170, 150.0};

    ColorCorrection existing;
    existing.brightness = -71.0;
    existing.contrast = -62.0;
    existing.temperature = -53.0;
    existing.tint = -44.0;
    existing.saturation = 24.0;
    existing.hue = -17.0;
    existing.gamma = 1.35;
    existing.highlights = -12.0;
    existing.shadows = 18.0;
    existing.exposure = 0.75;
    existing.liftR = -0.11;
    existing.liftG = 0.12;
    existing.liftB = -0.13;
    existing.gammaR = 0.21;
    existing.gammaG = -0.22;
    existing.gammaB = 0.23;
    existing.gainR = -0.31;
    existing.gainG = 0.32;
    existing.gainB = -0.33;

    const ColorCorrection expectedAutoFields = autocolor::autoCorrection(stats);
    const ColorCorrection corrected = autocolor::autoCorrection(stats, existing);
    const bool preservesManualGrade =
        sameDouble(corrected.saturation, existing.saturation)
        && sameDouble(corrected.hue, existing.hue)
        && sameDouble(corrected.gamma, existing.gamma)
        && sameDouble(corrected.highlights, existing.highlights)
        && sameDouble(corrected.shadows, existing.shadows)
        && sameDouble(corrected.exposure, existing.exposure)
        && sameDouble(corrected.liftR, existing.liftR)
        && sameDouble(corrected.liftG, existing.liftG)
        && sameDouble(corrected.liftB, existing.liftB)
        && sameDouble(corrected.gammaR, existing.gammaR)
        && sameDouble(corrected.gammaG, existing.gammaG)
        && sameDouble(corrected.gammaB, existing.gammaB)
        && sameDouble(corrected.gainR, existing.gainR)
        && sameDouble(corrected.gainG, existing.gainG)
        && sameDouble(corrected.gainB, existing.gainB);

    if (preservesManualGrade) {
        ++passed;
        std::cout << "[auto-color] G7 preserve manual grade fields: PASS\n";
    } else {
        ++failed;
        std::cerr << "[auto-color] G7 preserve manual grade fields: FAIL\n";
    }

    const bool overwritesAutoFields =
        sameDouble(corrected.temperature, expectedAutoFields.temperature)
        && sameDouble(corrected.tint, expectedAutoFields.tint)
        && sameDouble(corrected.contrast, expectedAutoFields.contrast)
        && sameDouble(corrected.brightness, expectedAutoFields.brightness)
        && (!sameDouble(corrected.temperature, existing.temperature)
            || !sameDouble(corrected.tint, existing.tint)
            || !sameDouble(corrected.contrast, existing.contrast)
            || !sameDouble(corrected.brightness, existing.brightness));
    if (overwritesAutoFields) {
        ++passed;
        std::cout << "[auto-color] G8 overwrite auto-owned fields: PASS\n";
    } else {
        ++failed;
        std::cerr << "[auto-color] G8 overwrite auto-owned fields: FAIL\n";
    }

    std::cout << "[auto-color] FIX-07 preservation summary: "
              << passed << " passed, " << failed << " failed\n";
    return legacyResult == 0 && failed == 0 ? 0 : 1;
}

int runAudioClipDragUndoSelftest()
{
    std::cout << "[audio-clip-drag-undo] selftest start\n";
    int passed = 0;
    int failed = 0;

    Timeline timeline;
    const ClipInfo clip = makeAudioClipDragUndoClip();
    timeline.videoTracks()[0]->addClip(clip);
    timeline.audioTracks()[0]->addClip(clip);
    // Select the video clip only — Timeline's non-additive selection handler
    // clears every other track, so selecting the audio clip afterwards would
    // deselect V1 and setClipVolume/setClipPan (keyed to V1's selection)
    // would early-return without mutating anything.
    timeline.videoTracks()[0]->setSelectedClip(0);

    UndoManager *undo = timeline.undoManager();
    const int baseIndex = undo ? undo->currentIndex() : -1;

    timeline.setClipPan(-0.25, false);
    timeline.setClipPan(0.40, false);
    timeline.setClipPan(0.75, false);
    audioClipDragUndoRequire(undo && undo->currentIndex() == baseIndex,
                             "G1 drag pan ticks do not record undo",
                             QStringLiteral("expected undo index %1, got %2")
                                 .arg(baseIndex).arg(undo ? undo->currentIndex() : -1),
                             passed, failed);
    audioClipDragUndoRequire(nearlyEqual(selectedAudioPan(timeline), 0.75),
                             "G2 drag pan ticks remain live",
                             QStringLiteral("expected live pan 0.75, got %1")
                                 .arg(selectedAudioPan(timeline), 0, 'f', 6),
                             passed, failed);

    timeline.setClipPan(0.75, true);
    const int afterDragIndex = baseIndex + 1;
    audioClipDragUndoRequire(undo && undo->currentIndex() == afterDragIndex,
                             "G3 drag pan release records one undo",
                             QStringLiteral("expected undo index %1, got %2")
                                 .arg(afterDragIndex).arg(undo ? undo->currentIndex() : -1),
                             passed, failed);

    timeline.setClipPan(-0.10);
    audioClipDragUndoRequire(undo && undo->currentIndex() == afterDragIndex + 1,
                             "G4 keyboard/default pan records one undo",
                             QStringLiteral("expected undo index %1, got %2")
                                 .arg(afterDragIndex + 1).arg(undo ? undo->currentIndex() : -1),
                             passed, failed);
    audioClipDragUndoRequire(nearlyEqual(selectedAudioPan(timeline), -0.10),
                             "G5 keyboard/default pan remains live",
                             QStringLiteral("expected pan -0.10, got %1")
                                 .arg(selectedAudioPan(timeline), 0, 'f', 6),
                             passed, failed);

    const int beforeVolumeDragIndex = undo ? undo->currentIndex() : -1;
    timeline.setClipVolume(0.50, false);
    timeline.setClipVolume(1.20, false);
    audioClipDragUndoRequire(undo && undo->currentIndex() == beforeVolumeDragIndex,
                             "G6 drag volume ticks do not record undo",
                             QStringLiteral("expected undo index %1, got %2")
                                 .arg(beforeVolumeDragIndex).arg(undo ? undo->currentIndex() : -1),
                             passed, failed);
    audioClipDragUndoRequire(nearlyEqual(selectedAudioVolume(timeline), 1.20),
                             "G7 drag volume ticks remain live",
                             QStringLiteral("expected live volume 1.20, got %1")
                                 .arg(selectedAudioVolume(timeline), 0, 'f', 6),
                             passed, failed);

    timeline.setClipVolume(1.20, true);
    audioClipDragUndoRequire(undo && undo->currentIndex() == beforeVolumeDragIndex + 1,
                             "G8 drag volume release records one undo",
                             QStringLiteral("expected undo index %1, got %2")
                                 .arg(beforeVolumeDragIndex + 1)
                                 .arg(undo ? undo->currentIndex() : -1),
                             passed, failed);

    std::cout << "[audio-clip-drag-undo] summary: "
              << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

namespace selftests {

bool requireSelftest(bool condition, const QString &message, QString *error)
{
    if (condition)
        return true;
    if (error)
        *error = message;
    qCritical() << "selftest failed:" << message;
    return false;
}

// ---------------------------------------------------------------------------
// PRD-ARGV-TABLE: central selftest dispatch table.
// Each entry maps a --selftest=<name> argument to its implementation function.
// needsQApplication==false entries are dispatched before QApplication is
// constructed (QApp-free path, safe for WSL direct exec without GUI hang).
// needsQApplication==true entries are dispatched after QApplication(argc,argv).
// PRD-ENV-TABLE: envVar is the VEDITOR_*_SELFTEST env var name for this entry,
// or nullptr when no env-gate exists (e.g. proxy: env-gate was reverted in
// PRD-PROXY-CLEAN US-PXC-3). The env-gate dispatch loop below uses this field
// as the single source of truth, replacing ~52 individual if-statements.
// ---------------------------------------------------------------------------
const ArgvSelftestEntry kArgvSelftests[] = {
    // QApplication-free (needsQApplication=false) -------------------------
    { "proxy",             nullptr,                               runProxySelftestV2,            false,
      "ProxyManager regression gate (libavcore + QSettings struct defaults, 6 gates)" },
    { "hdr-routing",       "VEDITOR_HDR_ROUTING_SELFTEST",        runHdrRoutingSelftest,         false,
      "HDR10 3-way routing (in-process 8-bit / in-process HDR / subprocess fallback)" },
    { "renderqueue-aces",  "VEDITOR_RENDERQUEUE_ACES_SELFTEST",   runRenderQueueAcesDecisionSelftest, false,
      "RenderQueue ACES/ODT ownership decision for 8-bit vs 16-bit tonemap paths (2 gates)" },
    { "renderqueue-fps-rational", "VEDITOR_RENDERQUEUE_FPS_RATIONAL_SELFTEST", runRenderQueueFpsRationalSelftest, false,
      "RenderQueue in-process export fps rational derivation (6 gates)" },
    { "loudness-export-wire", "VEDITOR_LOUDNESS_EXPORT_WIRE_SELFTEST", runLoudnessExportWireSelftest, false,
      "RenderQueue loudness gain to ffmpeg audio filter argument wiring (4 gates)" },
    { "versioned-save",   "VEDITOR_VERSIONED_SAVE_SELFTEST",    runVersionedSaveSelftest,      false,
      "Increment and Save filename resolver: numbered suffix, unnumbered v002, collision skip, digit width" },
    { "export-range",      "VEDITOR_EXPORT_RANGE_SELFTEST",       runExportRangeSelftest,       false,
      "Marked In/Out export frame-range helper (5 gates)" },
    { "tracker-preset",    "VEDITOR_TRACKER_PRESET_SELFTEST",     runTrackerPresetSelftest,      false,
      "MotionTracker preset 7 built-in + Registry + JSON round-trip (7 gates)" },
#ifdef HAVE_PLANARTRACKER_PRESET
    { "planar-preset",     "VEDITOR_PLANAR_PRESET_SELFTEST",      runPlanarPresetSelftest,       false,
      "PlanarTracker preset 5 built-in + Registry + JSON round-trip (10 gates)" },
#endif
    { "project-preset",    "VEDITOR_PROJECT_PRESET_SELFTEST",     runProjectPresetSelftest,      false,
      "Tracker preset state persistence in ProjectFile save/load cycle (10 gates)" },
    { "grade-lgg-serialization", "VEDITOR_GRADE_LGG_SERIALIZATION_SELFTEST", runGradeLggSerializationSelftest, false,
      "ColorCorrection LGG 9-field project serialization round-trip/default-omit/backward-compat gates" },
    { "aihighlight",       "VEDITOR_AIHIGHLIGHT_SELFTEST",        runAIHighlightSelftest,        false,
      "AIHighlight config defaults / Highlight struct helpers (singleton-free, 6 gates)" },
    { "videostab-deshake", "VEDITOR_VIDEOSTAB_DESHAKE_SELFTEST",  runVideostabDeshakeSelftest,   false,
      "VideoStabilizer in-process deshake filter-graph + cancel UX (9 gates)" },
    { "cred-ttl",          "VEDITOR_CRED_TTL_SELFTEST",           runCredTtlSelftest,            false,
      "CredentialStore TTL helper (setExpiry/getExpiry/clearExpiry/isExpired) round-trip + past/future detection (8 gates). QApplication 不要." },
    { "credential-vault",  "VEDITOR_CREDENTIAL_VAULT_SELFTEST",   runCredentialVaultSelftest,    false,
      "Windows Credential Manager (wincred.h) backend roundtrip: store/retrieve/erase/exists, Unicode, long value, multi-target isolation (10 gates)." },
    { "cred-audit-log",    "VEDITOR_CRED_AUDIT_LOG_SELFTEST",     runCredAuditLogSelftest,       false,
      "CredentialAuditLog 10 gate (file create / append / 7 labels JSONL / mask / ISO8601 / 1MB rotation / re-append / 10-thread safety / readEntries round-trip / purgeOlderThanDays). needsQApp=false (QtConcurrent uses default pool)." },
    { "ytdlp-downloader",  "VEDITOR_YTDLP_DOWNLOADER_SELFTEST",  runYtdlpDownloaderSelftest,    false,
      "yt-dlp downloader stub (Phase 6 Wave 1 FOUNDATION, filled in US-6A-4: 6 gate URL detect / outtmpl / subprocess mock / progress / cancel / completion)" },
    { "premiere-xml",      "VEDITOR_PREMIERE_XML_SELFTEST",       runPremiereXmlSelftest,        false,
      "Premiere XML (FCP7) exporter stub (Phase 6 Wave 1 FOUNDATION, filled in US-6E-3: 4 gate combined / individual / multi-sequence / DOCTYPE)" },
    { "youtube-chapter",   "VEDITOR_YOUTUBE_CHAPTER_SELFTEST",    runYoutubeChapterSelftest,     false,
      "YouTube chapter generator stub (Phase 6 Wave 1 FOUNDATION, filled in US-6F-3: 3 gate M:SS / H:MM:SS / intro auto-insert)" },
    { "media-pool",        "VEDITOR_MEDIA_POOL_SELFTEST",         runMediaPoolSelftest,          false,
      "MediaPool model: asset/bin/smartbin CRUD + search + JSON round-trip" },
    { "three-point-edit",  "VEDITOR_THREE_POINT_EDIT_SELFTEST",   runThreePointEditSelftest,     false,
      "ThreePointEdit engine: selection->clip, validate, overwrite plan" },
    { "trim-ops",          "VEDITOR_TRIM_OPS_SELFTEST",           runTrimOpsSelftest,            false,
      "TrimOps engine: ripple/roll/slip/slide + bounds" },
    { "audio-bus",         "VEDITOR_AUDIO_BUS_SELFTEST",          runAudioBusSelftest,           false,
      "AudioBusRouting: bus/submix/aux-send gain resolution + cycle guard" },
    { "audio-channel-map", "VEDITOR_AUDIO_CHANNEL_MAP_SELFTEST",  runAudioChannelMapSelftest,    false,
      "Per-clip audio channel mapping: Stereo byte identity, Fill L/R, swap, mono, ProjectFile round-trip, export pan chain" },
    { "stereo-pan",        "VEDITOR_STEREO_PAN_SELFTEST",         runStereoPanSelftest,          false,
      "Per-clip stereo balance pan: identity, L/R attenuation, half pan, ProjectFile round-trip" },
    { "atempo-resolve",    "VEDITOR_ATEMPO_RESOLVE_SELFTEST",     runAtempoResolveSelftest,      false,
      "Audio atempo enable resolver truth table: env force OR per-clip flag" },
    { "auto-ducking",      "VEDITOR_AUTO_DUCKING_SELFTEST",        runAutoDuckingSelftest,        false,
      "AutoDucking: thresholded VO envelope + non-destructive BGM gain keyframes" },
    { "bezier-easing",     "VEDITOR_BEZIER_EASING_SELFTEST",      runBezierEasingSelftest,       false,
      "Per-keyframe cubic-bezier easing: legacy invariance, identity curve, ease-in, JSON round-trip (4 gates)" },
    { "easing-presets",    "VEDITOR_EASING_PRESETS_SELFTEST",     runEasingPresetsSelftest,      false,
      "Elastic/Bounce/Back keyframe easing presets: math, overshoot, legacy invariance, JSON round-trip (6 gates)" },
    { "keyframe-loop",     "VEDITOR_KEYFRAME_LOOP_SELFTEST",       runKeyframeLoopSelftest,       false,
      "AE-ANIM-3 loopOut motion keyframes: None/Cycle/PingPong/Continue sampling + JSON omission (6 gates)" },
    { "motion-preset",     "VEDITOR_MOTION_PRESET_SELFTEST",       runMotionPresetSelftest,       false,
      "Motion preset library: built-in ids, generated motion keyframes, deterministic replacement (6 gates)" },
    { "spatial-path",      "VEDITOR_SPATIAL_PATH_SELFTEST",        runSpatialPathSelftest,        false,
      "AE-ANIM-2 spatial Bezier position path: no-handle invariance, curve, endpoints, JSON, degenerate guards (5 gates)" },
    { "spectral-edit",     "VEDITOR_SPECTRAL_EDIT_SELFTEST",      runSpectralEditSelftest,       false,
      "SpectralEngine: FFT/STFT/iSTFT round-trip + region attenuation" },
    { "sws-color",         "VEDITOR_SWS_COLOR_SELFTEST",          runSwsColorSelftest,           false,
      "swscale color matrix helper: colorspace/range/tag resolution + coefficient proof (8 gates)" },
    { "rgb-parade",        "VEDITOR_RGB_PARADE_SELFTEST",         runRgbParadeSelftest,          false,
      "RGB Parade scope: per-column R/G/B channel distribution and offscreen render dimensions (4 gates)" },
    { "aces-color",        "VEDITOR_ACES_COLOR_SELFTEST",         runAcesColorSelftest,          false,
      "AcesColor: primaries matrices, Bradford, IDT/RRT/ODT round-trip" },
    { "ae-fx-blur",        "VEDITOR_AE_FX_BLUR_SELFTEST",         runAeFxBlurSelftest,           false,
      "AE-FX-1 Blur Family: Gaussian, directional, radial implementation gates (G1-G6)" },
    { "ae-fx-color",       "VEDITOR_AE_FX_COLOR_SELFTEST",        runAeFxColorSelftest,          false,
      "AE-FX-3 Color Family: levels, tint, blackwhite, exposure, hue/saturation gates (G1-G6)" },
    { "ae-fx-color2",      "VEDITOR_AE_FX_COLOR2_SELFTEST",       runAeFxColor2Selftest,         true,
      "AE-FX-6 Color/Channel Family: curves, mixer, vibrance, filter, tritone, brightness/contrast gates (G1-G6)" },
    { "ae-fx-distort",     "VEDITOR_AE_FX_DISTORT_SELFTEST",      runAeFxDistortSelftest,        false,
      "AE-FX-4 Distort & Glitch Family: RGB split, wave warp, ripple, VHS glitch gates (G1-G6)" },
    { "ae-fx-distort2",    "VEDITOR_AE_FX_DISTORT2_SELFTEST",     runAeFxDistort2Selftest,       false,
      "AE-FX-7 Distort/Transform Family: bulge, twirl, mirror, polar, motion tile, corner pin gates (G1-G7)" },
    { "ae-fx-generate",    "VEDITOR_AE_FX_GENERATE_SELFTEST",     runAeFxGenerateSelftest,       false,
      "AE-FX-5 Generate Family: gradient ramp, fill, bloom, scanlines, halftone gates (G1-G6)" },
    { "ae-fx-stylize",     "VEDITOR_AE_FX_STYLIZE_SELFTEST",      runAeFxStylizeSelftest,        false,
      "AE-FX-2 Stylize Family: glow, edges, emboss, posterize, threshold, solarize gates (G1-G7)" },
    { "effect-preset", "VEDITOR_EFFECT_PRESET_SELFTEST", runEffectPresetSelftest, false,
      "FXP-1 effect preset stack JSON save/load/apply with optional effect keyframes (5 gates)" },
    { "dolby-vision",      "VEDITOR_DOLBY_VISION_SELFTEST",       runDolbyVisionSelftest,        false,
      "DolbyVision: PQ ST.2084 math + metadata + DV XML generation" },
    { "dv-timeline",       "VEDITOR_DV_TIMELINE_SELFTEST",         runDvTimelineSelftest,         false,
      "DvTimeline: build per-shot DV metadata from timeline spans" },
    { "broadcast-cc",      "VEDITOR_BROADCAST_CC_SELFTEST",       runBroadcastCaptionSelftest,   false,
      "BroadcastCaption: CEA-608 byte-pair/parity + SCC export + CEA-708 packet" },
    { "auto-matte",        "VEDITOR_AUTO_MATTE_SELFTEST",         runAutoMatteSelftest,          false,
      "AutoMatte: difference matte, morphology, feather, composite" },
    { "edl-export",        "VEDITOR_EDL_EXPORT_SELFTEST",         runEdlExportSelftest,          false,
      "EdlExport: CMX3600 timecode + event lines + drop-frame" },
    { "workspace",         "VEDITOR_WORKSPACE_SELFTEST",          runWorkspaceSelftest,          false,
      "WorkspaceManager: named layout CRUD + base64 blob JSON round-trip" },
    { "text-based-edit",   "VEDITOR_TEXT_BASED_EDIT_SELFTEST",    runTextBasedEditSelftest,      false,
      "TextBasedEdit: transcript search + deletion range merge + kept complement" },
    { "text3d-preview",    "VEDITOR_TEXT3D_PREVIEW_SELFTEST",     runText3dPreviewSelftest,      true,
      "Text3D preview camera persistence and proxy policy gates" },
    { "transcript-highlighter-offline", "VEDITOR_TRANSCRIPT_HIGHLIGHTER_OFFLINE_SELFTEST", runTranscriptHighlighterOfflineSelftest, false,
      "TranscriptHighlighter offline deterministic scorer (headless, 8 gates)" },
    { "pptx-export",      "VEDITOR_PPTX_EXPORT_SELFTEST",        runPptxExportSelftest,         false,
      "PptxExport: OOXML part tree + store-ZIP/CRC32 + slide XML well-formed + escaping" },
    { "asc-cdl-export",   "VEDITOR_ASC_CDL_EXPORT_SELFTEST",     runAscCdlExportSelftest,       false,
      "AscCdlExport: ASC CDL v1.01 .cc/.ccc/.cdl XML + LGG->SOP mapping + 6-decimal format + guards (13 gates)" },
    { "clip-color",       "VEDITOR_CLIP_COLOR_SELFTEST",         runClipColorSelftest,          false,
      "ClipColor: per-clip 色メタデータ模型 — defaultSdr / JSON round-trip / token 逆写像 / describe / codec transfer mapping (14 gates)" },
    { "clip-mask",        "VEDITOR_CLIP_MASK_SELFTEST",          runClipMaskSelftest,           false,
      "Bezier clip masks: model JSON, ProjectFile default omission, per-clip maskSystem round-trip" },
    { "clip-parent-parity", "VEDITOR_CLIP_PARENT_PARITY_SELFTEST", runClipParentParitySelftest, true,
      "Clip parenting: render parity, composeParented math, cycle/self/null guards" },
    { "clip-idt",         "VEDITOR_CLIP_IDT_SELFTEST",           runClipIdtSelftest,            false,
      "Clip IDT: per-clip RGBA64 premul input color transform into common 16bit blend space (13 gates)" },
    { "clip-odt",         "VEDITOR_CLIP_ODT_SELFTEST",           runClipOdtSelftest,            false,
      "Clip ODT: linear Rec2020 RGBA64 premul output transform + ACES tonemap contract (10 gates)" },
    { "silence-cut",      "VEDITOR_SILENCE_CUT_SELFTEST",        runSilenceCutSelftest,         false,
      "SilenceCut RMS silence detection: keep/silence segmentation + consistency (9 gates)" },
    { "beat-detect",      "VEDITOR_BEAT_DETECT_SELFTEST",         runBeatDetectSelftest,         false,
      "BeatDetect energy-flux onset detection + median-interval BPM estimate (7 gates)" },
    { "safe-zone",        "VEDITOR_SAFE_ZONE_SELFTEST",           runSafeZoneSelftest,           false,
      "SafeZone: SNS platform UI guide rects + apply display-local overlay (9 gates)" },
    { "onion-skin",       "VEDITOR_ONION_SKIN_SELFTEST",          runOnionSkinSelftest,          false,
      "OnionSkin: disabled/empty/opacity-zero no-op + display blend invariant (4 gates)" },
    { "capcut-caption",   "VEDITOR_CAPCUT_CAPTION_SELFTEST",      runCapcutCaptionSelftest,      false,
      "CapCut-style caption presets: shape, uniqueness, distinction, background semantics, non-default" },
    { "auto-color",       "VEDITOR_AUTO_COLOR_SELFTEST",          runAutoColorPreserveGradeSelftest, false,
      "AutoColor: frame stats + gray-world WB + brightness/contrast closed-loop proof + manual grade preservation" },
    { "wb-eyedropper",    "VEDITOR_WB_EYEDROPPER_SELFTEST",       runWbEyedropperSelftest,       false,
      "WB eyedropper inverse temperature/tint helper + closed-loop VideoEffect forward proof" },
    { "subtitle-cps",     "VEDITOR_SUBTITLE_CPS_SELFTEST",        runSubtitleCpsSelftest,        false,
      "Subtitle CPS readability helper: visible character counting, zero-duration sentinel, strict threshold" },
    { "subtitle-karaoke", "VEDITOR_SUBTITLE_KARAOKE_SELFTEST",    runSubtitleKaraokeSelftest,    false,
      "SubtitleKaraoke: active word half-open interval + spoken count boundaries (8 gates)" },
    { "sns-fit",          "VEDITOR_SNS_FIT_SELFTEST",            runSnsFitSelftest,             false,
      "snsfit::containGeom/containInAspectCanvas pure geometry" },
    { "sns-cover",        "VEDITOR_SNS_COVER_SELFTEST",          runSnsCoverSelftest,           false,
      "snsfit::coverGeom/coverInAspectCanvas pure geometry" },
    { "composite-frame-cache", "VEDITOR_COMPOSITE_FRAME_CACHE_SELFTEST", runCompositeFrameCacheSelftest, false,
      "CompositeFrameCache: LRU eviction + key hashing + hit/miss accounting" },
    { "playback-quality-policy", "VEDITOR_PLAYBACK_QUALITY_POLICY_SELFTEST", runPlaybackQualityPolicySelftest, false,
      "PlaybackQualityPolicy: adaptive quality level selection + drop-frame heuristics" },
    { "rightclick-pause-pref", "VEDITOR_RIGHTCLICK_PAUSE_PREF_SELFTEST", runRightclickPausePrefSelftest, false,
      "Right-click pause preference resolver: default, booleans, numeric strings, invalid fallback" },
    { "gpu-composite-flag", "VEDITOR_GPU_COMPOSITE_FLAG_SELFTEST", runGpuCompositeFlagSelftest, false,
      "GPU合成フラグ解決(env OR settings)の純粋リゾルバ検証" },
    { "gpu-composite-math", "VEDITOR_GPU_COMPOSITE_MATH_SELFTEST", runGpuCompositeMathSelftest, false,
      "GpuCompositeMath: paint order, layer transform matrix, premul source-over, matte validity (15 gates)" },
    { "hdr-composite-math", "VEDITOR_HDR_COMPOSITE_MATH_SELFTEST", runHdrCompositeMathSelftest, false,
      "HdrCompositeMath: 16-bit premul source-over + 8-bit SSOT parity + extra-precision proof + paint order (6 gates)" },
    { "hdr-export16",     "VEDITOR_HDR_EXPORT16_SELFTEST",       runHdrExport16Selftest,       false,
      "TlrCompose16: export SSOT の matte-free 16bit RGBA64 積層→to8bit パリティ + env フラグ + extra-precision (8 gates)" },
    { "hdr-ingest-colormeta", "VEDITOR_HDR_INGEST_COLORMETA_SELFTEST", runHdrIngestColorMetaSelftest, false,
      "Stage10 ingest ColorMeta derivation from codec primaries/transfer/bit-depth + pixfmt depth extraction (14 gates)" },
    { "hdr-overlay-policy", "VEDITOR_HDR_OVERLAY_POLICY_SELFTEST", runHdrOverlayPolicySelftest, false,
      "Stage4 overlay RGBA64 デコード述語: VEDITOR_HDR_OVERLAY flag×isHdr 真理値表 + env トグル + 混在→8bit ルート確認 (8 gates)" },
    { "preview16-policy", "VEDITOR_PREVIEW16_POLICY_SELFTEST", runPreview16PolicySelftest, false,
      "preview16 policy predicate gates" },
    { "live-matte-resolve", "VEDITOR_LIVE_MATTE_RESOLVE_SELFTEST", runLiveMatteResolveSelftest, false,
      "LiveMatteResolve: clipId->index matte source resolution (STAGE4B) — valid + self/base/unknown rejection + no-matte/multi-matte (6 gates)" },
    { "auto-proxy-policy",  "VEDITOR_AUTO_PROXY_POLICY_SELFTEST",  runAutoProxyPolicySelftest,     false,
      "AutoProxyPolicy: heavy-clip detection (width/codec) + useProxyFor/generateFor split + dedup (12 gates)" },
    { "exposure-aids",      "VEDITOR_EXPOSURE_AIDS_SELFTEST",      runExposureAidsSelftest,        false,
      "ExposureAids: luma709 oracle + False Color zones + Zebra stripe period + Focus Peaking edge/threshold + dimension/OOB safety (11 gates)" },
    { "import-ingest",      "VEDITOR_IMPORT_INGEST_SELFTEST",      runImportIngestSelftest,        false,
      "ImportHub ingest previews: mesh wireframe determinism + empty placeholder (QApplication-free)" },
    { "layer-style",        "VEDITOR_LAYER_STYLE_SELFTEST",        runLayerStyleSelftest,          false,
      "LayerStyle: identity fast path, drop shadow, stroke, and project JSON omission/round-trip" },
    { "layer-style-ui",     "VEDITOR_LAYER_STYLE_UI_SELFTEST",     runLayerStyleUiSelftest,       true,
      "Layer Style UI: default identity, JSON round-trip, enabled-flag identity, and Timeline setter/getter gates (V1 legacy + track-aware)" },
    { "smart-render",       "VEDITOR_SMART_RENDER_SELFTEST",       runSmartRenderSelftest,        false,
      "Smart Render T4: conservative stream-copy eligibility predicate + env gate" },
    // QApplication-required (needsQApplication=true) ----------------------
    { "adjustment-layer", "VEDITOR_ADJUSTMENT_LAYER_SELFTEST", runAdjustmentLayerSelftest, true,
      "Adjustment clip model: V1-wins z-order, behind-only effect stack, byte-identical off path, JSON true-only persistence" },
    { "frame-export",      "VEDITOR_FRAME_EXPORT_SELFTEST",        runFrameExportSelftest,        true,
      "Still-frame export: renderFrameAt SSOT -> PNG/JPEG save engine + PNG pixel-identical reread" },
    { "freeze-frame",      "VEDITOR_FREEZE_FRAME_SELFTEST",        runFreezeFrameSelftest,        true,
      "Freeze Frame: split at playhead, one-key hold curve, renderFrameAt export hold, save/load sync, one-step undo" },
    { "clip-curves",       "VEDITOR_CLIP_CURVES_SELFTEST",         runClipCurvesSelftest,         true,
      "Clip RGB/Luma curves: unset/identity byte identity, renderFrameAt reflection, ProjectFile round-trip" },
    { "hsl-secondary",     "VEDITOR_HSL_SECONDARY_SELFTEST",       runHslSecondarySelftest,       true,
      "HSL secondary qualifier: per-clip ProjectFile round-trip + renderFrameAt reflection + off-path byte identity" },
    { "clip-lut",          "VEDITOR_CLIP_LUT_SELFTEST",            runClipLutSelftest,            true,
      "Clip LUT: per-clip ProjectFile round-trip + renderFrameAt LUT reflection + no-LUT byte-identical gate" },
    { "colormatch-apply",  "VEDITOR_COLORMATCH_APPLY_SELFTEST",    runColorMatchApplySelftest,    true,
      "ColorMatch apply: generated .cube -> selected clip LUT fields + undo + renderFrameAt reflection" },
    { "parity",            "VEDITOR_PARITY_SELFTEST",             runParitySelftest,             true,
      "Preview vs export pixel-parity (S1-S11, framediff::mse, 10-bit HDR10)" },
    { "keyframe-anim-parity", "VEDITOR_KEYFRAME_ANIM_PARITY_SELFTEST", runKeyframeAnimParitySelftest, true,
      "Render-time per-clip motion/opacity keyframe evaluation parity (5 gates, byte-identical no-keyframe guard)" },
    { "motion-blur-p2", "VEDITOR_MOTION_BLUR_P2_SELFTEST", runMotionBlurP2Selftest, true,
      "Motion blur P2 activation: env force OR per-clip opt-in + ProjectFile true-only persistence (5 gates)" },
    { "motion-blur-parity", "VEDITOR_MOTION_BLUR_PARITY_SELFTEST", runMotionBlurParitySelftest, true,
      "Export SSOT motion blur OFF-path parity + premultiplied temporal averaging helper (3 gates)" },
    { "effect-keyframe-parity", "VEDITOR_EFFECT_KEYFRAME_PARITY_SELFTEST", runEffectKeyframeParitySelftest, true,
      "Render-time per-clip effect parameter keyframe evaluation parity (4 gates, byte-identical no-keyframe guard)" },
    { "effect-timing", "VEDITOR_EFFECT_TIMING_SELFTEST", runEffectTimingSelftest, true,
      "Clip-local VideoEffect active range filtering + ProjectFile JSON round-trip (4 gates)" },
    { "mediapool-drag", "VEDITOR_MEDIAPOOL_DRAG_SELFTEST", runMediaPoolDragSelftest, true,
      "MediaPool drag&drop: single/multi selection mimeData file URL generation" },
    { "text-spacing", "VEDITOR_TEXT_SPACING_SELFTEST", runTextSpacingSelftest, true,
      "Text letter/line spacing: default byte-identical, spacing widens render, toJson/fromJson roundtrip" },
    { "grade-wheel-wiring", "VEDITOR_GRADE_WHEEL_WIRING_SELFTEST", runGradeWheelWiringSelftest, true,
      "LGG wheels + WB -> per-clip ColorCorrection wiring: value mapping, restore, render reflection" },
    { "graph-editor", "VEDITOR_GRAPH_EDITOR_SELFTEST", runGraphEditorSelftest, true,
      "GraphEditor model roundtrip, panel-edit undo restore, and ClipAnim evaluation parity" },
    { "nest-sequence", "VEDITOR_NEST_SEQUENCE_SELFTEST", runNestSequenceSelftest, true,
      "Nested sequences: recursive render, cycle/depth guards, audio flatten, store roundtrip, no-nest byte identity" },
    { "ripple-delete", "VEDITOR_RIPPLE_DELETE_SELFTEST", runRippleDeleteSelftest, true,
      "Ripple delete and gap close: all-track time-range ripple, one-step undo, and no-selection no-op" },
    { "e2e",               "VEDITOR_E2E_SELFTEST",                runE2eSelftest,                true,
      "Real-media end-to-end smoke (ColorMatch decode + deHum + processAll)" },
    { "trackmatte-parity", "VEDITOR_TRACKMATTE_PARITY_SELFTEST",  runTrackMatteParitySelftest,   true,
      "Track matte SSOT pixel-match across 4 matte types (luma/alpha/chroma/inv)" },
    { "gpu-composite-parity", "VEDITOR_GPU_COMPOSITE_PARITY_SELFTEST", runGpuCompositeParitySelftest, true,
      "GPU (GpuLayerCompositor FBO) vs CPU SSOT parity, matte-free scenes (SSIM>=0.98, MAE<=3..6/255, 7 gates, graceful GL skip)" },
    { "gpu-composite-parity-16-matte", "VEDITOR_GPU_COMPOSITE_PARITY_16_MATTE_SELFTEST", runGpuCompositeParity16MatteSelftest, true,
      "GPU RGBA16 track-matte (composite16Matte) vs CPU 16-bit matte oracle, all 4 matte types + premul + precision proof, graceful GL skip" },
    { "gpu-idt-parity", "VEDITOR_GPU_IDT_PARITY_SELFTEST", runGpuIdtParitySelftest, true,
      "GpuIdt: per-fragment IDT (eotf->mat3->oetf) vs aces:: CPU oracle (9 gates incl Linear transfer, graceful GL skip)" },
    { "gpu-idt-matte-parity", "VEDITOR_GPU_IDT_MATTE_PARITY_SELFTEST", runGpuIdtMatteParitySelftest, true,
      "GpuIdtMatte: per-fragment IDT matte path vs IDT-then-compositeReference16Matte oracle" },
    { "hdr-composite-parity", "VEDITOR_HDR_COMPOSITE_PARITY_SELFTEST", runHdrCompositeParitySelftest, true,
      "GPU RGBA16 FBO (composite16) vs CPU 16-bit oracle (HdrCompositeMath), matte-free (8 gates incl 16-bit precision proof, graceful GL skip)" },
    { "matte16-parity", "VEDITOR_MATTE16_PARITY_SELFTEST", runMatte16ParitySelftest, true,
      "16-bit RGBA64 track-matte (trackmatte16::composeExport) vs 8-bit trackmatte:: SSOT parity, all 4 matte types + 16-bit precision + ODT luma-space proof (8 gates)" },
    { "vfx",               "VEDITOR_VFX_SELFTEST",                runVfxSelftest,                true,
      "VFX module smoke: Sprint-6 effect graph + GLSL primitive pipeline" },
    { "pro",               "VEDITOR_PRO_SELFTEST",                runProSelftest,                true,
      "Pro/advanced toolset smoke (color scopes, envelope, multicam prep)" },
    { "mograph",           "VEDITOR_MOGRAPH_SELFTEST",            runMographSelftest,            true,
      "Motion graphics module smoke (title renderer, keyframe graph)" },
    { "hwperf",            "VEDITOR_HWPERF_SELFTEST",             runHwPerfSelftest,             true,
      "Hardware perf probe: codec / encoder availability on current GPU" },
    { "proext",            "VEDITOR_PROEXT_SELFTEST",             runProExtSelftest,             true,
      "Pro extensions module smoke (LUT pipeline, HDR scope, noise reduction)" },
    { "shortcut",          "VEDITOR_SHORTCUT_SELFTEST",           runShortcutSelftest,           true,
      "ShortcutManager binding registration and conflict-detection tests" },
    { "social",            "VEDITOR_SOCIAL_SELFTEST",             runSocialSelftest,             true,
      "Social/sharing module smoke (Sprint-12 SNS pipeline stubs)" },
    { "caption",           "VEDITOR_CAPTION_SELFTEST",            runCaptionSelftest,            true,
      "Caption/subtitle module smoke (SRT/VTT parse, burn-in, track model)" },
    { "whisper-transcribe", "VEDITOR_WHISPER_TRANSCRIBE_SELFTEST", runWhisperTranscribeSelftest,  true,
      "Whisper transcription service scaffold smoke" },
    { "whisper-word-timing", "VEDITOR_WHISPER_WORD_TIMING_SELFTEST", runWhisperWordTimingSelftest, false,
      "Whisper word-level timestamps: JSON full parse -> speech::Word -> SubtitleWord monotonic/boundary gates" },
    { "transcript-highlighter", "VEDITOR_TRANSCRIPT_HIGHLIGHTER_SELFTEST", runTranscriptHighlighterSelftest, true,
      "Transcript highlighter scaffold smoke" },
    { "auto-clip-gen",     "VEDITOR_AUTO_CLIP_GEN_SELFTEST",      runAutoClipGenSelftest,        true,
      "Auto clip generator scaffold smoke" },
    { "planar",            "VEDITOR_PLANAR_SELFTEST",             runPlanarSelftest,             true,
      "Planar tracker primitive smoke (homography solver, pre-Preset era)" },
    { "mobile",            "VEDITOR_MOBILE_SELFTEST",             runMobileSelftest,             true,
      "Mobile export module smoke (iOS/Android container + bitrate profiles)" },
    { "obs",               "VEDITOR_OBS_SELFTEST",                runObsSelftest,                true,
      "OBS scene import module smoke (Sprint-16 .json scene ingestion)" },
    { "affinity",          "VEDITOR_AFFINITY_SELFTEST",           runAffinitySelftest,           true,
      "Affinity Designer/Photo import smoke (Sprint-16 asset bridge)" },
    { "blender",           "VEDITOR_BLENDER_SELFTEST",            runBlenderSelftest,            true,
      "Blender .blend import smoke (Sprint-16 3-D asset pipeline stub)" },
    { "import",            "VEDITOR_IMPORT_SELFTEST",             runImportSelftest,             true,
      "Generic external import smoke (format registry + clip ingest path)" },
    { "youtube",           "VEDITOR_YOUTUBE_SELFTEST",            runYoutubeSelftest,            true,
      "YouTube OAuth2 + resumable-upload manager + progress-dialog smoke" },
    { "collab",            "VEDITOR_COLLAB_SELFTEST",             runCollabSelftest,             true,
      "Collaboration smoke: Comments, ProjectShare, and History module stubs" },
    { "colormatch",        "VEDITOR_COLORMATCH_SELFTEST",         runColorMatchSelftest,         true,
      "AI ColorMatch analyzer + 3D-LUT generator + dialog smoke" },
    { "command-search",    "VEDITOR_COMMAND_SEARCH_SELFTEST",     runCommandSearchSelftest,      true,
      "Command palette search scaffold smoke" },
    { "vimeo",             "VEDITOR_VIMEO_SELFTEST",              runVimeoSelftest,              true,
      "Vimeo upload pipeline smoke (auth token + chunked upload stub)" },
    { "twitch",            "VEDITOR_TWITCH_SELFTEST",             runTwitchSelftest,             true,
      "Twitch broadcast pipeline smoke (RTMP push + stream-key stub)" },
    { "frameio",           "VEDITOR_FRAMEIO_SELFTEST",            runFrameIoSelftest,            true,
      "Frame.io collaboration smoke (asset upload + review-link stub)" },
    { "davinci",           "VEDITOR_DAVINCI_SELFTEST",            runDavinciSelftest,            true,
      "DaVinci Resolve XML round-trip I/O smoke (timeline + grade export)" },
    { "fcpxml",            "VEDITOR_FCPXML_SELFTEST",             runFcpxmlSelftest,             true,
      "FCPXML import/export round-trip smoke (Final Cut Pro X interchange)" },
    { "smartedit",         "VEDITOR_SMARTEDIT_SELFTEST",          runSmartEditSelftest,          true,
      "AI smart-edit pipeline smoke (scene-detect + auto-cut + beat-sync)" },
    { "cloudrender",       "VEDITOR_CLOUDRENDER_SELFTEST",        runCloudRenderSelftest,        true,
      "Cloud render pipeline smoke (job queue + progress polling stub)" },
    { "xupload",           "VEDITOR_XUPLOAD_SELFTEST",            runXUploadSelftest,            true,
      "X (Twitter) media upload pipeline smoke (chunked upload + tweet stub)" },
    { "instagram",         "VEDITOR_INSTAGRAM_SELFTEST",          runInstagramSelftest,          true,
      "Instagram upload pipeline smoke (reel + story container stub)" },
    { "projtmpl",          "VEDITOR_PROJTMPL_SELFTEST",           runProjTmplSelftest,           true,
      "Project template module smoke (preset bundle save/load + apply)" },
    { "loudness",          "VEDITOR_LOUDNESS_SELFTEST",           runLoudnessSelftest,           true,
      "BS.1770 loudness measurement smoke (integrated LUFS + true peak)" },
    { "hdr",               "VEDITOR_HDR_SELFTEST",                runHdrSelftest,                true,
      "HDR tone-mapping smoke (HLG/HDR10 display transform, Sprint-21)" },
    { "multicam",          "VEDITOR_MULTICAM_SELFTEST",           runMultiCamSelftest,           true,
      "Multicam edit module smoke (angle sync, cut-away, monitor layout)" },
    { "batchexport",       "VEDITOR_BATCHEXPORT_SELFTEST",        runBatchExportSelftest,        true,
      "Batch export pipeline smoke (queue manager + per-item preset binding)" },
    { "chroma",            "VEDITOR_CHROMA_SELFTEST",             runChromaSelftest,             true,
      "Chroma key (greenscreen) module smoke (spill-suppress + matte refine)" },
    { "audiorestore",      "VEDITOR_AUDIORESTORE_SELFTEST",       runAudioRestoreSelftest,       true,
      "Audio restoration smoke: deHum filter + deNoise spectral subtraction" },
    { "animexport",        "VEDITOR_ANIMEXPORT_SELFTEST",         runAnimExportSelftest,         true,
      "Animated GIF / WebP export smoke (palette quantize + LZW encode)" },
    { "easing",            "VEDITOR_EASING_SELFTEST",             runEasingSelftest,             true,
      "Animation easing curve module smoke (bezier, spring, step functions)" },
    { "subxlat",           "VEDITOR_SUBXLAT_SELFTEST",            runSubXlatSelftest,            true,
      "Subtitle translation pipeline smoke (locale map + subtitle track swap)" },
    { "lowerthird",        "VEDITOR_LOWERTHIRD_SELFTEST",         runLowerThirdSelftest,         true,
      "Lower-third title module smoke (template render + animator keyframes)" },
    { "watermark",         "VEDITOR_WATERMARK_SELFTEST",          runWatermarkSelftest,          true,
      "Watermark overlay smoke (tile / corner placement + opacity blend)" },
    { "libavcore-encode",  "VEDITOR_LIBAVCORE_ENCODE_SELFTEST",   runLibavcoreEncodeSelftest,    true,
      "libavcore::FrameEncoder h264_mf in-process encode + AAC audio round-trip" },
    { "libavcore-decode",  "VEDITOR_LIBAVCORE_DECODE_SELFTEST",   runLibavcoreDecodeSelftest,    true,
      "libavcore::MediaDecoder frame extraction + pixel-format conversion" },
    { "exportaudit",       "VEDITOR_EXPORTAUDIT_SELFTEST",        runExportAuditSelftest,        true,
      "Export audit stub-boundary tracker (STUB_AUDIT coverage regression)" },
    { "textexport",        "VEDITOR_TEXTEXPORT_SELFTEST",         runTextExportSelftest,         true,
      "Text overlay export pipeline smoke (MSE threshold, AE-text parity)" },
    { "workflow",          "VEDITOR_WORKFLOW_SELFTEST",           runWorkflowSelftest,           true,
      "Workflow / scripting module smoke (macro record + replay pipeline)" },
    { "audio-clip-drag-undo", "VEDITOR_AUDIO_CLIP_DRAG_UNDO_SELFTEST", runAudioClipDragUndoSelftest, true,
      "AudioClipEditor drag ticks live-update without filling undo; release/default calls record once" },
    { "audiomixer",        "VEDITOR_AUDIOMIXER_SELFTEST",         runAudioMixerSelftest,         true,
      "Audio mixer module smoke (Sprint-23 bus routing + send/return stubs)" },
    { "oauth-mock-e2e",   "VEDITOR_OAUTH_MOCK_SELFTEST",        runOAuthMockE2eSelftest,       true,
      "OAuth + Upload pipeline を localhost mock HTTP server で exercise する 10 gate e2e selftest" },
    { "oauth-refresh-e2e", "VEDITOR_OAUTH_REFRESH_E2E_SELFTEST", runOAuthRefreshE2eSelftest,    true,
      "OAuth refresh_token auto-trigger e2e (YT 5 + Vimeo 5 gates via mock server). needs QApplication." },
    { "platform-mock-e2e", "VEDITOR_PLATFORM_MOCK_SELFTEST",      runPlatformMockE2eSelftest,    true,
      "Instagram Graph API + X Media Upload を localhost mock 経由で e2e exercise (13 gates: server + 5 IG + 7 X). QApplication 必要。" },
};

const std::size_t kArgvSelftestsCount = sizeof(kArgvSelftests) / sizeof(kArgvSelftests[0]);

// ---------------------------------------------------------------------------
// dispatchPreQApplication: handle --selftest=list / --selftest=help /
// QApp-free entries. Must run before QApplication is constructed so that
// headless WSL execution never triggers GUI init / hang.
// ---------------------------------------------------------------------------
std::optional<int> dispatchPreQApplication(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--selftest=", 11) != 0) continue;
        const char* req = a + 11;

        if (std::strcmp(req, "list") == 0) {
            // PRD-ARGV-EXTRAS: print argv-switch + env-gate name pair per entry,
            // so CI / dev workflows can pick whichever path suits the runner.
            for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
                const auto& e = kArgvSelftests[j];
                std::cout << "--selftest=" << e.name;
                if (e.envVar) std::cout << "  (env: " << e.envVar << ")";
                else          std::cout << "  (env: -)";
                std::cout << '\n';
            }
            std::cout << "--selftest=all  (env: VEDITOR_ALL_SELFTEST)  # full sweep\n";
            return 0;
        }

        if (std::strcmp(req, "help") == 0) {
            // PRD-SELFTEST-HELP: print name + description + env block for every entry.
            std::cout << "V Simple Editor — selftest entry points (" << kArgvSelftestsCount << ")\n";
            std::cout << "Usage: ./v-simple-editor.exe --selftest=<name>\n";
            std::cout << "   or: VEDITOR_<NAME>_SELFTEST=1 ./v-simple-editor.exe\n";
            std::cout << "   or: --selftest=all  (full sweep)\n";
            std::cout << "\n";
            for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
                const auto& e = kArgvSelftests[j];
                std::cout << "  --selftest=" << e.name << "\n";
                std::cout << "      " << (e.description ? e.description : "(no description)") << "\n";
                if (e.envVar) std::cout << "      env: " << e.envVar << "\n";
                std::cout << "\n";
            }
            return 0;
        }

        if (std::strcmp(req, "all") == 0) {
            // Defer to dispatchPostQApplication: --selftest=all must also run
            // QApplication-required entries, so it needs the Qt context.
            return std::nullopt;
        }

        // Match QApp-free entries only.
        for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
            const auto& e = kArgvSelftests[j];
            if (e.needsQApplication) continue;
            if (std::strcmp(req, e.name) == 0) {
                writeLogLine("INFO",
                    QString("argv-switch: --selftest=%1").arg(QString::fromLatin1(e.name)));
                return e.fn();
            }
        }
    }
    return std::nullopt;
}

namespace {

// Legacy trackmatte argv switches (pre-PRD-ARGV-UNIFY): kept for back-compat.
// Each maps directly to a function in parity_matte_selftests.cpp.
std::optional<int> dispatchLegacyTrackmatte(const QStringList& args)
{
    if (args.contains(QStringLiteral("--selftest-trackmatte-parity"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-parity");
        return runTrackMatteParitySelftest();
    }

    if (args.contains(QStringLiteral("--selftest-trackmatte-export-integration"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-export-integration");
        return runTrackMatteExportIntegrationSelftest();
    }

    if (args.contains(QStringLiteral("--selftest-trackmatte-reindex"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-reindex");
        return runTrackMatteReindexSelftest();
    }

    if (args.contains(QStringLiteral("--selftest-trackmatte-rm6-duplicate"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-rm6-duplicate");
        return runTrackMatteRm6DuplicateSelftest();
    }

    if (args.contains(QStringLiteral("--selftest-trackmatte-rm5-reorder"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-rm5-reorder");
        return runTrackMatteRm5ReorderSelftest();
    }

    return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// dispatchPostQApplication: --selftest=all sweep + QApp-required argv-switch
// entries + env-gate (VEDITOR_*_SELFTEST) loop.
// Call AFTER QApplication has been constructed.
// ---------------------------------------------------------------------------
std::optional<int> dispatchPostQApplication(const QStringList& args)
{
    if (auto rc = dispatchLegacyTrackmatte(args)) {
        return rc;
    }

    // PRD-ARGV-ALL: --selftest=all runs every entry sequentially.
    // PRD-ARGV-EXTRAS: VEDITOR_ALL_SELFTEST=1 mirrors --selftest=all.
    if (args.contains(QStringLiteral("--selftest=all"))
        || qEnvironmentVariableIntValue("VEDITOR_ALL_SELFTEST") != 0) {
        writeLogLine("INFO", "argv-switch: --selftest=all (full sweep)");
        int passed = 0;
        int failed = 0;
        for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
            const auto& e = kArgvSelftests[j];
            writeLogLine("INFO",
                QString("[--selftest=all] running --selftest=%1").arg(QString::fromLatin1(e.name)));
            const int rc = e.fn();
            if (rc == 0) {
                ++passed;
                writeLogLine("INFO",
                    QString("[--selftest=all] --selftest=%1 PASS").arg(QString::fromLatin1(e.name)));
            } else {
                ++failed;
                writeLogLine("CRIT",
                    QString("[--selftest=all] --selftest=%1 FAIL rc=%2")
                        .arg(QString::fromLatin1(e.name)).arg(rc));
            }
        }
        writeLogLine("INFO",
            QString("[--selftest=all] summary: %1 PASS, %2 FAIL").arg(passed).arg(failed));
        return failed == 0 ? 0 : failed;
    }

    // PRD-ARGV-TABLE: QApp-required argv-switch entries.
    for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
        const auto& e = kArgvSelftests[j];
        if (!e.needsQApplication) continue;
        const QString sw = QStringLiteral("--selftest=%1").arg(QString::fromLatin1(e.name));
        if (args.contains(sw)) {
            writeLogLine("INFO", QString("argv-switch: %1").arg(sw));
            return e.fn();
        }
    }

    // PRD-ENV-TABLE: env-gate dispatch via kArgvSelftests envVar field.
    // Together with the argv-switch loops above, kArgvSelftests is the
    // single source of truth for selftest entry-point routing.
    for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
        const auto& e = kArgvSelftests[j];
        if (!e.envVar) continue;
        if (qEnvironmentVariableIntValue(e.envVar) != 0) {
            writeLogLine("INFO",
                QString("running %1").arg(QString::fromLatin1(e.envVar)));
            return e.fn();
        }
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// validateUnknown: if any --selftest=<name> arg did not match list / help /
// all / a table entry, emit a friendly error and return exit code 2 instead
// of silently launching the GUI.
// ---------------------------------------------------------------------------
std::optional<int> validateUnknown(const QStringList& args)
{
    for (const QString& arg : args) {
        if (!arg.startsWith(QStringLiteral("--selftest="))) continue;
        const QString req = arg.mid(11);
        if (req == QStringLiteral("list") || req == QStringLiteral("all")
            || req == QStringLiteral("help")) continue;
        bool known = false;
        for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
            if (req == QString::fromLatin1(kArgvSelftests[j].name)) {
                known = true;
                break;
            }
        }
        if (!known) {
            std::cerr << "[ERROR] unknown selftest: " << arg.toStdString() << '\n';
            std::cerr << "Run with --selftest=list to see available selftests.\n";
            return 2;
        }
    }
    return std::nullopt;
}

} // namespace selftests
