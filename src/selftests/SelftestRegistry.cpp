// src/selftests/SelftestRegistry.cpp
// PRD-SPLIT-MAIN-1: selftest dispatch table + 3 helper functions.
// The run<Foo>Selftest() bodies remain in their owning selftest translation
// units; this file only owns the routing (struct table + dispatch helpers).

#include "SelftestRegistry.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <QString>
#include <QStringList>
#include <QCoreApplication>
#include <QDebug>
#include <QRegularExpression>
#include "../AutoColor.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../util/Logger.h"
#include "../util/HangDump.h"

// ---------------------------------------------------------------------------
// Forward declarations for the selftest functions implemented in src/main.cpp.
// Kept alphabetically sorted for ease of maintenance.
// ---------------------------------------------------------------------------
int runAIHighlightSelftest();
int runAcesColorSelftest();
int runAdjustmentLayerSelftest();
int runAeFxBlurSelftest();
int runAeFxColor2Selftest();
int runAeFxColorSelftest();
int runAeFxDistort2Selftest();
int runAeFxDistortSelftest();
int runAeFxGenerateSelftest();
int runAeFxStylizeSelftest();
int runAffinitySelftest();
int runAlignDistributeSelftest();
int runAnimExportSelftest();
int runAscCdlExportSelftest();
int runAtempoResolveSelftest();
int runAudioBusSelftest();
int runAudioChannelMapSelftest();
int runAudioClipDragUndoSelftest();
int runAudioFixtureSelftest();
int runAudioKeyframesSelftest();
int runAudioMixerSelftest();
int runAudioRestoreSelftest();
int runAudioTrackFxSelftest();
int runAudioXfadeSelftest();
int runAutoClipGenSelftest();
int runAutoColorPreserveGradeSelftest();
int runAutoColorSelftest();
int runAutoDuckingSelftest();
int runAutoMatteSelftest();
int runAutoProxyPolicySelftest();
int runBatchExportE2ESelftest();
int runBatchExportSelftest();
int runBeatDetectSelftest();
int runBezierEasingSelftest();
int runBlenderSelftest();
int runBroadcastCaptionSelftest();
int runBundleAdjustSelftest();
int runCamera3DSelftest();
int runCameraSolveSelftest();
int runCamtrackSolveSelftest();
int runCamtrackUiSelftest();
int runCapcutCaptionSelftest();
int runCaptionEditSelftest();
int runCaptionOverlayBuilderSelftest();
int runCaptionSelftest();
int runChromaSelftest();
int runClipColorSelftest();
int runClipCurvesSelftest();
int runClipEnableSelftest();
int runClipFilmstripSelftest();
int runClipGroupSelftest();
int runClipIdtSelftest();
int runClipLutSelftest();
int runClipMaskSelftest();
int runClipOdtSelftest();
int runClipParentParitySelftest();
int runCloudRenderSelftest();
int runCollabSelftest();
int runColorMatchApplySelftest();
int runColorMatchSelftest();
int runColorWarperSelftest();
int runCommandSearchSelftest();
int runCompositeFrameCacheSelftest();
int runCredAuditLogSelftest();
int runCredTtlSelftest();
int runCredentialVaultSelftest();
int runDavinciSelftest();
int runDeEsserSelftest();
int runDeflickerSelftest();
int runDialogueLevelerSelftest();
int runDolbyVisionSelftest();
int runDvTimelineSelftest();
int runDynzoomSelftest();
int runE2eSelftest();
int runEasingPresetsSelftest();
int runEasingSelftest();
int runEdlExportSelftest();
int runEdlImportSelftest();
int runEffectKeyframeParitySelftest();
int runEffectLibrarySelftest();
int runEffectMaskSelftest();
int runEffectPresetSelftest();
int runEffectTimingSelftest();
int runExportAlphaSelftest();
int runExportAuditSelftest();
int runExportCfgSelftest();
int runExportMetaSelftest();
int runExportPresetsSelftest();
int runExportProSelftest();
int runExportRangeSelftest();
int runExposureAidsSelftest();
int runFcpxmlImportSelftest();
int runFcpxmlSelftest();
int runFeatureTrackerSelftest();
int runFlipSolidSelftest();
int runFrameClipboardSelftest();
int runFrameExportSelftest();
int runFrameIoSelftest();
int runFreezeFrameSelftest();
int runFxGrainEchoSelftest();
int runGateCountProbeSelftest();
int runGateTallySelftest();
int runGpuCompositeFlagSelftest();
int runGpuCompositeMathSelftest();
int runGpuCompositeParity16MatteSelftest();
int runGpuCompositeParitySelftest();
int runGpuIdtMatteParitySelftest();
int runGpuIdtParitySelftest();
int runGradeKeyframeExtSelftest();
int runGradeKeyframeSelftest();
int runGradeLggSerializationSelftest();
int runGradeLogWheelSelftest();
int runGradeWheelWiringSelftest();
int runGraphEditorSelftest();
int runHdrCompositeMathSelftest();
int runHdrCompositeParitySelftest();
int runHdrExport16Selftest();
int runHdrIngestColorMetaSelftest();
int runHdrOverlayPolicySelftest();
int runHdrRoutingSelftest();
int runHdrSelftest();
int runHslSecondarySelftest();
int runHwPerfSelftest();
int runImgSequenceSelftest();
int runImportIngestSelftest();
int runImportSelftest();
int runInstagramSelftest();
int runKeyersSelftest();
int runKeyframeAnimParitySelftest();
int runKeyframeCopySelftest();
int runKeyframeLoopSelftest();
int runKeywordBinsSelftest();
int runLayerStyleSelftest();
int runLayerStyleUiSelftest();
int runLegacyKfSelftest();
int runLensDistortSelftest();
int runLibavcoreDecodeSelftest();
int runLibavcoreEncodeSelftest();
int runLight3DSelftest();
int runLinkedTrimSelftest();
int runLiveMatteResolveSelftest();
int runLogToRec709Selftest();
int runLoudnessAutoSelftest();
int runLoudnessExportWireSelftest();
int runLoudnessPersistSelftest();
int runLoudnessSelftest();
int runLowerThirdSelftest();
int runMainWindowLifecycleSelftest();
int runMatte16ParitySelftest();
int runMatteSsotSelftest();
int runMcpSelftest();
int runMediaPoolDragSelftest();
int runMediaPoolRatingSelftest();
int runMediaPoolSelftest();
int runMediaPoolThumbsSelftest();
int runMediaRelinkSelftest();
int runMeshWarpSelftest();
int runMixerDockSelftest();
int runMobileSelftest();
int runMographSelftest();
int runMorphCutSelftest();
int runMotionBlurP2Selftest();
int runMotionBlurParitySelftest();
int runMotionPresetSelftest();
int runMultiCamSelftest();
int runMusicRemixSelftest();
int runNestSequenceSelftest();
int runNoisePrintSelftest();
int runOAuthMockE2eSelftest();
int runOAuthRefreshE2eSelftest();
int runObjectRemovalSelftest();
int runObsSelftest();
int runOffsetWheelSelftest();
int runOnionSkinSelftest();
int runParentGateSelftest();
int runParitySelftest();
#if __has_include("PlanarTrackerPreset.h")
int runPlanarPresetSelftest();
#define HAVE_PLANARTRACKER_PRESET 1
#endif
int runPlanarSelftest();
int runPlatformMockE2eSelftest();
int runPlaybackQualityPolicySelftest();
int runPptxExportSelftest();
int runPrecomposeE2ESelftest();
int runPremiereXmlSelftest();
int runPreview16PolicySelftest();
int runProExtSelftest();
int runProSelftest();
int runProjTmplSelftest();
int runProjectDiffSelftest();
int runProjectPresetSelftest();
int runProxySelftestV2();
int runPuppetPinSelftest();
int runReduceProjectSelftest();
int runRenderFixtureSelftest();
int runRenderInPlaceSelftest();
int runRenderNotifySelftest();
int runRenderQueueAcesDecisionSelftest();
int runRenderQueueFpsRationalSelftest();
int runReverseClipSelftest();
int runRgbParadeSelftest();
int runRightclickPausePrefSelftest();
int runRippleDeleteSelftest();
int runRollingShutterSelftest();
int runRotoSelftest();
int runSafeLeaveColorSelftest();
int runSafeZoneSelftest();
int runSeqFromClipSelftest();
int runSequenceDockSelftest();
int runSfmTwoViewSelftest();
int runShapeClipSelftest();
int runShortcutSelftest();
int runSilenceCutSelftest();
int runSmartEditSelftest();
int runSmartRenderSelftest();
int runSnsCoverSelftest();
int runSnsFitSelftest();
int runSocialSelftest();
int runSpatialPathSelftest();
int runSpecialCompositeSelftest();
int runSpectralEditSelftest();
int runSpectrumSelftest();
int runStereoPanSelftest();
int runStillCompareSelftest();
int runSubMovtextSelftest();
int runSubXlatSelftest();
int runSubtitleCpsSelftest();
int runSubtitleKaraokeSelftest();
int runSurfaceToolSelftest();
int runSwsColorSelftest();
int runTcburnSelftest();
int runText3dPreviewSelftest();
int runTextBasedEditSelftest();
int runTextExportSelftest();
int runTextSpacingSelftest();
int runThreePointEditSelftest();
int runTimelineErgoSelftest();
int runTimelineSkimSelftest();
int runTrackManageSelftest();
int runTrackMatteExportIntegrationSelftest();
int runTrackMatteParitySelftest();
int runTrackMatteReindexSelftest();
int runTrackMatteRm5ReorderSelftest();
int runTrackMatteRm6DuplicateSelftest();
int runTrackOpsSelftest();
int runTrackerPresetSelftest();
int runTranscriptHighlighterOfflineSelftest();
int runTranscriptHighlighterSelftest();
int runTransitionExportSelftest();
int runTransitionParamsSelftest();
int runTrimOpsSelftest();
int runTwitchSelftest();
int runVersionedSaveSelftest();
int runVfxFootageSelftest();
int runVfxGeneratorsSelftest();
int runVfxSelftest();
int runVideoPlayerParentSelftest();
int runVideostabDeshakeSelftest();
int runVimeoSelftest();
int runVoiceIsolationSelftest();
int runWarpFxSelftest();
int runWatchdogProbeSelftest();
int runWatchdogSelftest();
int runWatermarkSelftest();
int runWbEyedropperSelftest();
int runWhisperGuideSelftest();
int runWhisperTranscribeSelftest();
int runWhisperWordTimingSelftest();
int runWorkflowSelftest();
int runWorkspaceSelftest();
int runXUploadSelftest();
int runYoutubeChapterSelftest();
int runYoutubeSelftest();
int runYtdlpDownloaderSelftest();

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
// US-700: per-entry watchdog. Each arm spawns a detached std::thread holding
// its own shared state, so disarm never has to join and a selftest that
// calls exit() cannot trip std::thread's joinable-destructor terminate.
// ---------------------------------------------------------------------------
namespace {

struct WatchdogState {
    std::mutex mutex;
    std::condition_variable cv;
    bool cancelled = false;
};

std::mutex g_watchdogMutex;
std::shared_ptr<WatchdogState> g_watchdogState;

int watchdogTimeoutSec()
{
    const char* raw = std::getenv("VEDITOR_SELFTEST_TIMEOUT_SEC");
    if (!raw || !*raw)
        return 900;
    char* end = nullptr;
    const long v = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || v < 0 || v > 86400 * 7)
        return 900;
    return static_cast<int>(v);
}

} // namespace

void armWatchdog(const char* name)
{
    disarmWatchdog();
    const int sec = watchdogTimeoutSec();
    if (sec == 0) {
        std::cerr << "[watchdog] disabled" << std::endl;
        return;
    }
    const std::string entryName = name ? name : "?";
    std::cerr << "[watchdog] armed " << sec << "s (" << entryName << ")" << std::endl;

#ifdef _WIN32
    hangdump::prepare();
#endif
    auto state = std::make_shared<WatchdogState>();
    {
        std::lock_guard<std::mutex> lock(g_watchdogMutex);
        g_watchdogState = state;
    }
    std::thread([state, sec, entryName]() {
        std::unique_lock<std::mutex> lock(state->mutex);
        const bool cancelled = state->cv.wait_for(
            lock, std::chrono::seconds(sec), [&state]() { return state->cancelled; });
        if (cancelled)
            return;
        std::fflush(stdout);
        std::fprintf(stderr, "summary: TIMEOUT after %ds (selftest=%s)\n",
                     sec, entryName.c_str());
        std::fflush(stderr);
#ifdef _WIN32
        const hangdump::Result dump = hangdump::writeHangArtifacts(entryName.c_str());
        std::fprintf(stderr, "watchdog: dump %s %s stacks=%s elapsedMs=%lld\n",
                     dump.dumpWritten ? "written to" : "failed at",
                     dump.dumpPath, dump.stacksWritten ? "yes" : "no",
                     dump.elapsedMs);
#else
        std::fprintf(stderr, "watchdog: stack dump unsupported on this platform\n");
#endif
        std::fflush(stderr);
        std::_Exit(124);
    }).detach();
}

void disarmWatchdog()
{
    std::shared_ptr<WatchdogState> state;
    {
        std::lock_guard<std::mutex> lock(g_watchdogMutex);
        state.swap(g_watchdogState);
    }
    if (!state)
        return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->cancelled = true;
    }
    state->cv.notify_all();
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
    { "gate-tally", nullptr, runGateTallySelftest, false,
      "PASS / FAIL / SKIP tally, gate-count dispatch, and missing encoder (14 gates)" },
    { "gate-count-probe", "VEDITOR_GATE_COUNT_PROBE_SELFTEST", runGateCountProbeSelftest, false,
      "内部用 (2 gates)" },
    { "proxy",             nullptr,                               runProxySelftestV2,            false,
      "ProxyManager regression gate (libavcore + QSettings struct defaults) (6 gates)" },
    { "hdr-routing",       "VEDITOR_HDR_ROUTING_SELFTEST",        runHdrRoutingSelftest,         false,
      "HDR10 3-way routing (in-process 8-bit / in-process HDR / subprocess fallback)" },
    { "renderqueue-aces",  "VEDITOR_RENDERQUEUE_ACES_SELFTEST",   runRenderQueueAcesDecisionSelftest, false,
      "RenderQueue ACES/ODT ownership decision for 8-bit vs 16-bit tonemap paths (5 gates)" },
    { "renderqueue-fps-rational", "VEDITOR_RENDERQUEUE_FPS_RATIONAL_SELFTEST", runRenderQueueFpsRationalSelftest, false,
      "RenderQueue in-process export fps rational derivation (6 gates)" },
    { "legacy-kf", nullptr, runLegacyKfSelftest, true,
      "Legacy colorCorrection.* keyframe tracks migrate to grade.* on project load + dialog initial values + nested sequences (8 gates)" },
    { "loudness-auto", nullptr, runLoudnessAutoSelftest, true,
      "Automatic export loudness normalization (12 gates)" },
    { "loudness-persist", nullptr, runLoudnessPersistSelftest, true,
      "Project loudness gain and playback normalizer persistence (4 gates)" },
    { "loudness-export-wire", "VEDITOR_LOUDNESS_EXPORT_WIRE_SELFTEST", runLoudnessExportWireSelftest, false,
      "RenderQueue loudness gain to ffmpeg audio filter argument wiring (4 gates)" },
    { "versioned-save",   "VEDITOR_VERSIONED_SAVE_SELFTEST",    runVersionedSaveSelftest,      false,
      "Increment and Save filename resolver: numbered suffix, unnumbered v002, collision skip, digit width" },
    { "project-diff", "VEDITOR_PROJECT_DIFF_SELFTEST", runProjectDiffSelftest, false,
      "Structural project comparison: matching, properties, effects, transitions and epsilon (7 gates)" },
    { "img-sequence", "VEDITOR_IMG_SEQUENCE_SELFTEST", runImgSequenceSelftest, true,
      "PNG/TIFF sequence export, alpha, configuration and cancellation (6 gates)" },
    { "export-alpha", "VEDITOR_EXPORT_ALPHA_SELFTEST", runExportAlphaSelftest, true,
      "ProRes/VP9 alpha export and opaque default path (7 gates)" },
    { "export-cfg", "VEDITOR_EXPORT_CFG_SELFTEST", runExportCfgSelftest, true,
      "Shared ExportConfig job mapping and normal/mobile route parity (12 gates)" },
    { "export-meta", "VEDITOR_EXPORT_META_SELFTEST", runExportMetaSelftest, true,
      "Container metadata GUI wiring, encoding, defaults and presets (6 gates)" },
    { "export-presets", "VEDITOR_EXPORT_PRESETS_SELFTEST", runExportPresetsSelftest, true,
      "Export user preset persistence, name resolution and dialog restore (9 gates)" },
    { "export-pro",        "VEDITOR_EXPORT_PRO_SELFTEST",         runExportProSelftest,         false,
      "Export rate control options and queue persistence" },
    { "export-range",      "VEDITOR_EXPORT_RANGE_SELFTEST",       runExportRangeSelftest,       false,
      "Marked In/Out export frame-range helper (5 gates)" },
    { "tracker-preset",    "VEDITOR_TRACKER_PRESET_SELFTEST",     runTrackerPresetSelftest,      false,
      "MotionTracker preset 7 built-in + Registry + JSON round-trip (10 gates)" },
#ifdef HAVE_PLANARTRACKER_PRESET
    { "planar-preset",     "VEDITOR_PLANAR_PRESET_SELFTEST",      runPlanarPresetSelftest,       false,
      "PlanarTracker preset 5 built-in + Registry + JSON round-trip (10 gates)" },
#endif
    { "project-preset",    "VEDITOR_PROJECT_PRESET_SELFTEST",     runProjectPresetSelftest,      false,
      "Tracker preset state persistence in ProjectFile save/load cycle (10 gates)" },
    { "grade-lgg-serialization", "VEDITOR_GRADE_LGG_SERIALIZATION_SELFTEST", runGradeLggSerializationSelftest, false,
      "ColorCorrection LGG 9-field project serialization round-trip/default-omit/backward-compat checks" },
    { "color-warper", "VEDITOR_COLOR_WARPER_SELFTEST", runColorWarperSelftest, false,
      "Hue/saturation mesh CPU math and persistence (6 gates)" },
    { "offset-wheel", "VEDITOR_OFFSET_WHEEL_SELFTEST", runOffsetWheelSelftest, true,
      "Offset wheel CPU/GPU, project, MCP and preset parity (5 gates)" },
    { "grade-log-wheel", "VEDITOR_GRADE_LOG_WHEEL_SELFTEST", runGradeLogWheelSelftest, false,
      "ColorCorrection Log Shadow/Midtone/Highlight CPU math and project serialization (5 gates)" },
    { "aihighlight",       "VEDITOR_AIHIGHLIGHT_SELFTEST",        runAIHighlightSelftest,        false,
      "AIHighlight config defaults / Highlight struct helpers (singleton-free) (6 gates)" },
    { "videostab-deshake", "VEDITOR_VIDEOSTAB_DESHAKE_SELFTEST",  runVideostabDeshakeSelftest,   false,
      "VideoStabilizer in-process deshake filter-graph + cancel UX (7 gates); source sections are numbered (1)-(5),(8),(9) with no (6)/(7)" },
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
    { "clip-filmstrip", "VEDITOR_CLIP_FILMSTRIP_SELFTEST", runClipFilmstripSelftest, true,
      "Timeline filmstrip: disabled path, shared thumbnails and tile/time mapping" },
    { "keyword-bins", "VEDITOR_KEYWORD_BINS_SELFTEST", runKeywordBinsSelftest, true,
      "Keyword bins: tag editing, virtual bins, SmartBin evaluation and defaults" },
    { "media-pool-thumbs", "VEDITOR_MEDIA_POOL_THUMBS_SELFTEST", runMediaPoolThumbsSelftest, true,
      "Media pool asynchronous thumbnails, RGBA frame grab and hover skimming" },
    { "media-pool-rating", "VEDITOR_MEDIA_POOL_RATING_SELFTEST", runMediaPoolRatingSelftest, false,
      "MediaPool ratings: JSON defaults/round-trip, filters, rename and search" },
    { "media-pool",        "VEDITOR_MEDIA_POOL_SELFTEST",         runMediaPoolSelftest,          false,
      "MediaPool model: asset/bin/smartbin CRUD + search + JSON round-trip" },
    { "three-point-edit",  "VEDITOR_THREE_POINT_EDIT_SELFTEST",   runThreePointEditSelftest,     false,
      "ThreePointEdit engine: selection->clip, validate, overwrite plan" },
    { "trim-ops",          "VEDITOR_TRIM_OPS_SELFTEST",           runTrimOpsSelftest,            false,
      "TrimOps engine: ripple/roll/slip/slide + bounds" },
    { "audio-keyframes", "VEDITOR_AUDIO_KEYFRAMES_SELFTEST", runAudioKeyframesSelftest, false,
      "Audio RMS envelope, keyframe mapping, smoothing, audioLevel expressions and determinism" },
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
    { "keyers", "VEDITOR_KEYERS_SELFTEST", runKeyersSelftest, false,
      "ルマキー・カラーキー: CPU alpha、合成、プリセット、スタック順序 (7 gates)" },
    { "keyframe-loop",     "VEDITOR_KEYFRAME_LOOP_SELFTEST",       runKeyframeLoopSelftest,       false,
      "AE-ANIM-3 loopOut motion keyframes: None/Cycle/PingPong/Continue sampling + JSON omission (6 gates)" },
    { "motion-preset",     "VEDITOR_MOTION_PRESET_SELFTEST",       runMotionPresetSelftest,       false,
      "Motion preset library: built-in ids, generated motion keyframes, deterministic replacement (6 gates)" },
    { "spatial-path",      "VEDITOR_SPATIAL_PATH_SELFTEST",        runSpatialPathSelftest,        false,
      "AE-ANIM-2 spatial Bezier position path and auto-orient (9 gates)" },
    { "special-composite", "VEDITOR_SPECIAL_COMPOSITE_SELFTEST",   runSpecialCompositeSelftest,   true,
      "Special-composite preview parity: time-remap + mesh warp preview == renderFrameAt, warp off parity, non-special clip skips path (4 gates)" },
    { "spectral-edit",     "VEDITOR_SPECTRAL_EDIT_SELFTEST",      runSpectralEditSelftest,       false,
      "SpectralEngine: FFT/STFT/iSTFT round-trip + region attenuation" },
    { "spectrum", nullptr, runSpectrumSelftest, true,
      "2048-point Hann FFT and visible-only master tap spectrum dock (4 gates)" },
    { "noise-print", "VEDITOR_NOISE_PRINT_SELFTEST", runNoisePrintSelftest, false,
      "Noise print: SNR, amplitude, bypass, stereo independence, validation and determinism (6 gates)" },
    { "voice-isolation",   "VEDITOR_VOICE_ISOLATION_SELFTEST",    runVoiceIsolationSelftest,     true,
      "Voice isolation: streaming STFT, complementary outputs, adaptive floor, smoothing, dialog responsiveness and harmonic guards (16 gates)" },
    { "sws-color",         "VEDITOR_SWS_COLOR_SELFTEST",          runSwsColorSelftest,           false,
      "swscale color matrix helper: colorspace/range/tag resolution + coefficient proof (8 gates)" },
    { "rgb-parade",        "VEDITOR_RGB_PARADE_SELFTEST",         runRgbParadeSelftest,          false,
      "RGB Parade scope: per-column R/G/B channel distribution and offscreen render dimensions (4 gates)" },
    { "aces-color",        "VEDITOR_ACES_COLOR_SELFTEST",         runAcesColorSelftest,          false,
      "AcesColor: primaries matrices, Bradford, IDT/RRT/ODT round-trip" },
    { "ae-fx-blur",        "VEDITOR_AE_FX_BLUR_SELFTEST",         runAeFxBlurSelftest,           false,
      "AE-FX-1 Blur Family: Gaussian, directional, radial implementation (7 gates)" },
    { "ae-fx-color",       "VEDITOR_AE_FX_COLOR_SELFTEST",        runAeFxColorSelftest,          false,
      "AE-FX-3 Color Family: levels, tint, blackwhite, exposure, hue/saturation (6 gates)" },
    { "ae-fx-color2",      "VEDITOR_AE_FX_COLOR2_SELFTEST",       runAeFxColor2Selftest,         true,
      "AE-FX-6 Color/Channel Family: curves, mixer, vibrance, filter, tritone, brightness/contrast (12 gates)" },
    { "log-to-rec709", "VEDITOR_LOG_TO_REC709_SELFTEST", runLogToRec709Selftest, false,
      "Camera Log decoding, output transfer, gamut and presets (G1-G7)" },
    { "lens-distort", "VEDITOR_LENS_DISTORT_SELFTEST", runLensDistortSelftest, false,
      "Lens distortion identity, geometry and preset round-trip (G1-G6)" },
    { "ae-fx-distort",     "VEDITOR_AE_FX_DISTORT_SELFTEST",      runAeFxDistortSelftest,        false,
      "AE-FX-4 Distort & Glitch Family: RGB split, wave warp, ripple, VHS glitch (7 gates)" },
    { "ae-fx-distort2",    "VEDITOR_AE_FX_DISTORT2_SELFTEST",     runAeFxDistort2Selftest,       false,
      "AE-FX-7 Distort/Transform Family: bulge, twirl, mirror, polar, motion tile, corner pin (7 gates)" },
    { "ae-fx-generate",    "VEDITOR_AE_FX_GENERATE_SELFTEST",     runAeFxGenerateSelftest,       false,
      "AE-FX-5 Generate Family: gradient ramp, fill, bloom, scanlines, halftone (6 gates)" },
    { "ae-fx-stylize",     "VEDITOR_AE_FX_STYLIZE_SELFTEST",      runAeFxStylizeSelftest,        false,
      "AE-FX-2 Stylize Family: glow, edges, emboss, posterize, threshold, solarize (9 gates)" },
    { "puppet-pin", "VEDITOR_PUPPET_PIN_SELFTEST", runPuppetPinSelftest, true,
      "正規化パペットピン・描画・保存・Undo・クリップ変換込みの配置 (6 gates)" },
    { "mesh-warp", "VEDITOR_MESH_WARP_SELFTEST", runMeshWarpSelftest, true,
      "正規化メッシュワープ・描画・保存・Undo・グリッド編集・4隅ピン (G1-G7)" },
    { "surface-tool", "VEDITOR_SURFACE_TOOL_SELFTEST", runSurfaceToolSelftest, true,
      "サーフェスツールの画面座標とソース UV をフィット・クリップ変換込みで往復・ドラッグ量・解像度変更・scale 0・影響円・余白クリック・親判定 3 形態・借用区間のキー付き子 (13 gates)" },
    { "rolling-shutter", "VEDITOR_ROLLING_SHUTTER_SELFTEST", runRollingShutterSelftest, true,
      "Rolling shutter: bypass, repair, direction, last frame, determinism, presets (G1-G6)" },
    { "fxgrain-echo",      "VEDITOR_FXGRAIN_ECHO_SELFTEST",       runFxGrainEchoSelftest,         false,
      "FilmGrain deterministic CPU effect + Echo temporal compositor + preview routing (16 gates)" },
    { "flip-solid", "VEDITOR_FLIP_SOLID_SELFTEST", runFlipSolidSelftest, true,
      "Flip CPU parity, involution, preset round-trip and solid insertion/undo (G1-G5)" },
    { "effect-mask", "VEDITOR_EFFECT_MASK_SELFTEST", runEffectMaskSelftest, true,
      "エフェクト単位マスク・保存・CPUパリティ・正規化座標・UI確定とプレビュー同期 (G1-G8)" },
    { "effect-preset", "VEDITOR_EFFECT_PRESET_SELFTEST", runEffectPresetSelftest, false,
      "FXP-1 effect preset stack JSON save/load/apply with optional effect keyframes (5 gates)" },
    { "effect-library", "VEDITOR_EFFECT_LIBRARY_SELFTEST", runEffectLibrarySelftest, false,
      "Effect Library: dynamic registry catalog, search/category indexes, favorites, preset round-trip, apply/preview parity, safe thumbnails, shader/plugin parameter routing, VFX registration and image application (13 gates)" },
    { "vfx-footage", "VEDITOR_VFX_FOOTAGE_SELFTEST", runVfxFootageSelftest, true,
      "VFX footage: missing-directory safety, extension/category scan, EffectLibrary integration, black-level, Screen, intensity, determinism, ClipInfo/upper-track placement, non-black representative thumbnail (11 gates); valid-media decode explicitly SKIP when absent" },
    { "dolby-vision",      "VEDITOR_DOLBY_VISION_SELFTEST",       runDolbyVisionSelftest,        false,
      "DolbyVision: PQ ST.2084 math + metadata + DV XML generation" },
    { "dv-timeline",       "VEDITOR_DV_TIMELINE_SELFTEST",         runDvTimelineSelftest,         false,
      "DvTimeline: build per-shot DV metadata from timeline spans" },
    { "broadcast-cc",      "VEDITOR_BROADCAST_CC_SELFTEST",       runBroadcastCaptionSelftest,   false,
      "BroadcastCaption: CEA-608 byte-pair/parity + SCC export + CEA-708 packet" },
    { "auto-matte",        "VEDITOR_AUTO_MATTE_SELFTEST",         runAutoMatteSelftest,          false,
      "AutoMatte: difference matte, morphology, feather, composite" },
    { "object-removal",    "VEDITOR_OBJECT_REMOVAL_SELFTEST",     runObjectRemovalSelftest,      false,
      "ObjectRemoval: temporal trust, background alignment, bounds, dilation, cache, composite (14 gates)" },
    { "bundle-adjust", "VEDITOR_BUNDLE_ADJUST_SELFTEST", runBundleAdjustSelftest, false,
      "可変サイズ線形ソルバ、多フレーム追跡、姿勢連結、Schurバンドル調整・密版同値性・規模上限 (9 gates)" },
    { "sfm-two-view", "VEDITOR_SFM_TWO_VIEW_SELFTEST", runSfmTwoViewSelftest, false,
      "Two-view essential pose, RANSAC, triangulation, degeneracy and determinism (6 gates)" },
    { "feature-tracker", "VEDITOR_FEATURE_TRACKER_SELFTEST", runFeatureTrackerSelftest, false,
      "Shi-Tomasi 特徴点検出とピラミッド LK 多点追跡 (6 gates)" },
    { "camtrack-solve", "VEDITOR_CAMTRACK_SOLVE_SELFTEST", runCamtrackSolveSelftest, true,
      "Camera tracking solve: noise-free nonplanar 64 points / 10 frames, substantial parallax; limits, rejection, undo, trim/speed/reverse, BA outliers/noise and backend parity (7 gates)" },
    { "camtrack-ui", "VEDITOR_CAMTRACK_UI_SELFTEST", runCamtrackUiSelftest, true,
      "KLT tracking UI, bounded decoding, planar compatibility and BA limits (11 gates)" },
    { "camera-solve",      "VEDITOR_CAMERA_SOLVE_SELFTEST",       runCameraSolveSelftest,        true,
      "Planar camera solver: SVD, continuity, confidence, camera keyframes and single-slot undo (8 gates)" },
    { "camera3d",           "VEDITOR_CAMERA3D_SELFTEST",           runCamera3DSelftest,           true,
      "Project camera: opt-in projection, view/roll, JSON, overlap and animated export parity (G1-G9)" },
    { "light3d",            "VEDITOR_LIGHT3D_SELFTEST",            runLight3DSelftest,            true,
      "3D lights: compositor/export parity, keyframe edits, dialog guards, JSON, determinism (17 gates)" },
    { "edl-import", "VEDITOR_EDL_IMPORT_SELFTEST", runEdlImportSelftest, true,
      "CMX3600 import, dissolve, offline media and sequence undo (13 gates)" },
    { "edl-export",        "VEDITOR_EDL_EXPORT_SELFTEST",         runEdlExportSelftest,          false,
      "EdlExport: CMX3600 timecode + event lines + drop-frame" },
    { "tcburn",            "VEDITOR_TCBURN_SELFTEST",             runTcburnSelftest,             true,
      "Timecode burn-in: disabled no-op, six positions, drop-frame, JSON and HH:MM:SS (9 gates)" },
    { "workspace",         "VEDITOR_WORKSPACE_SELFTEST",          runWorkspaceSelftest,          false,
      "WorkspaceManager: named layout CRUD + base64 blob JSON round-trip" },
    { "text-based-edit",   "VEDITOR_TEXT_BASED_EDIT_SELFTEST",    runTextBasedEditSelftest,      false,
      "TextBasedEdit: transcript search + deletion range merge + kept complement" },
    { "text3d-preview",    "VEDITOR_TEXT3D_PREVIEW_SELFTEST",     runText3dPreviewSelftest,      true,
      "Text3D preview camera persistence and proxy policy checks" },
    { "transcript-highlighter-offline", "VEDITOR_TRANSCRIPT_HIGHLIGHTER_OFFLINE_SELFTEST", runTranscriptHighlighterOfflineSelftest, false,
      "TranscriptHighlighter offline deterministic scorer (headless) (8 gates)" },
    { "pptx-export",      "VEDITOR_PPTX_EXPORT_SELFTEST",        runPptxExportSelftest,         false,
      "PptxExport: OOXML part tree + store-ZIP/CRC32 + slide XML well-formed + escaping" },
    { "asc-cdl-export",   "VEDITOR_ASC_CDL_EXPORT_SELFTEST",     runAscCdlExportSelftest,       false,
      "AscCdlExport: ASC CDL v1.01 .cc/.ccc/.cdl XML + LGG->SOP mapping + 6-decimal format + guards (13 gates)" },
    { "clip-color",       "VEDITOR_CLIP_COLOR_SELFTEST",         runClipColorSelftest,          false,
      "ClipColor: per-clip 色メタデータ模型 — defaultSdr / JSON round-trip / token 逆写像 / describe / codec transfer mapping (14 gates)" },
    { "clip-mask",        "VEDITOR_CLIP_MASK_SELFTEST",          runClipMaskSelftest,           false,
      "Bezier clip masks: model JSON, ProjectFile default omission, per-clip maskSystem round-trip" },
    { "align-distribute", "VEDITOR_ALIGN_DISTRIBUTE_SELFTEST", runAlignDistributeSelftest, true,
      "Canvas alignment, equal gaps, atomic undo, parent gate in 3 forms and keyed child in a borrowed interval (11 gates)" },
    { "clip-group", "VEDITOR_CLIP_GROUP_SELFTEST", runClipGroupSelftest, true,
      "Clip grouping, linked edits, undo and validation (5 gates)" },
    { "linked-trim", "VEDITOR_LINKED_TRIM_SELFTEST", runLinkedTrimSelftest, true,
      "Linked edge-drag trim across AV pairs / groups, Alt solo, clamps, locked partners and undo (9 gates)" },
    { "clip-enable", "VEDITOR_CLIP_ENABLE_SELFTEST", runClipEnableSelftest, true,
      "Clip enable: rendering, persistence, undo, audio and Null Object parenting (5 gates)" },
    { "clip-parent-parity", "VEDITOR_CLIP_PARENT_PARITY_SELFTEST", runClipParentParitySelftest, true,
      "Clip parenting: render parity, composeParented math, cycle/self/null guards" },
    { "clip-idt",         "VEDITOR_CLIP_IDT_SELFTEST",           runClipIdtSelftest,            false,
      "Clip IDT: per-clip RGBA64 premul input color transform into common 16bit blend space (13 gates)" },
    { "clip-odt",         "VEDITOR_CLIP_ODT_SELFTEST",           runClipOdtSelftest,            false,
      "Clip ODT: linear Rec2020 RGBA64 premul output transform + ACES tonemap contract (10 gates)" },
    { "silence-cut",      "VEDITOR_SILENCE_CUT_SELFTEST",        runSilenceCutSelftest,         false,
      "SilenceCut RMS silence detection: keep/silence segmentation + consistency (12 gates)" },
    { "beat-detect",      "VEDITOR_BEAT_DETECT_SELFTEST",         runBeatDetectSelftest,         false,
      "BeatDetect energy-flux onset detection + median-interval BPM estimate (7 gates)" },
    { "safe-leavecolor", "VEDITOR_SAFE_LEAVECOLOR_SELFTEST", runSafeLeaveColorSelftest, false,
      "放送セーフと色を残す (6 gates)" },
    { "safe-zone",        "VEDITOR_SAFE_ZONE_SELFTEST",           runSafeZoneSelftest,           false,
      "SafeZone: SNS platform UI guide rects + apply display-local overlay (13 gates)" },
    { "still-compare",    "VEDITOR_STILL_COMPARE_SELFTEST",       runStillCompareSelftest,       false,
      "StillCompare: horizontal/vertical wipe + split compositing + StillStore round-trip (5 gates)" },
    { "onion-skin",       "VEDITOR_ONION_SKIN_SELFTEST",          runOnionSkinSelftest,          false,
      "OnionSkin: disabled/empty/opacity-zero no-op + display blend invariant (4 gates)" },
    { "capcut-caption",   "VEDITOR_CAPCUT_CAPTION_SELFTEST",      runCapcutCaptionSelftest,      false,
      "CapCut-style caption presets: shape, uniqueness, distinction, background semantics, non-default" },
    { "auto-color",       "VEDITOR_AUTO_COLOR_SELFTEST",          runAutoColorPreserveGradeSelftest, false,
      "AutoColor: frame stats + gray-world WB + brightness/contrast closed-loop proof + manual grade preservation" },
    { "wb-eyedropper",    "VEDITOR_WB_EYEDROPPER_SELFTEST",       runWbEyedropperSelftest,       false,
      "WB eyedropper inverse temperature/tint helper + closed-loop VideoEffect forward proof" },
    { "sub-movtext", "VEDITOR_SUB_MOVTEXT_SELFTEST", runSubMovtextSelftest, true,
      "mov_text subtitle streams, burn-in modes, containers and queue wiring (6 gates)" },
    { "subtitle-cps",     "VEDITOR_SUBTITLE_CPS_SELFTEST",        runSubtitleCpsSelftest,        false,
      "Subtitle CPS readability helper: visible character counting, zero-duration sentinel, strict threshold" },
    { "subtitle-karaoke", "VEDITOR_SUBTITLE_KARAOKE_SELFTEST",    runSubtitleKaraokeSelftest,    false,
      "SubtitleKaraoke: active word half-open interval + spoken count boundaries (8 gates)" },
    { "sns-fit",          "VEDITOR_SNS_FIT_SELFTEST",            runSnsFitSelftest,             false,
      "snsfit::containGeom/containInAspectCanvas pure geometry (15 gates)" },
    { "sns-cover",        "VEDITOR_SNS_COVER_SELFTEST",          runSnsCoverSelftest,           false,
      "snsfit::coverGeom/coverInAspectCanvas pure geometry (12 gates)" },
    { "composite-frame-cache", "VEDITOR_COMPOSITE_FRAME_CACHE_SELFTEST", runCompositeFrameCacheSelftest, false,
      "CompositeFrameCache: LRU eviction + key hashing + hit/miss accounting" },
    { "playback-quality-policy", "VEDITOR_PLAYBACK_QUALITY_POLICY_SELFTEST", runPlaybackQualityPolicySelftest, false,
      "PlaybackQualityPolicy: adaptive quality level selection + drop-frame heuristics" },
    { "rightclick-pause-pref", "VEDITOR_RIGHTCLICK_PAUSE_PREF_SELFTEST", runRightclickPausePrefSelftest, false,
      "Right-click pause preference resolver: default, booleans, numeric strings, invalid fallback" },
    { "gpu-composite-flag", "VEDITOR_GPU_COMPOSITE_FLAG_SELFTEST", runGpuCompositeFlagSelftest, false,
      "GPU合成フラグ解決(env OR settings)の純粋リゾルバ検証" },
    { "gpu-composite-math", "VEDITOR_GPU_COMPOSITE_MATH_SELFTEST", runGpuCompositeMathSelftest, false,
      "GpuCompositeMath: paint order, layer transform matrix, premul source-over, matte validity (24 gates)" },
    { "hdr-composite-math", "VEDITOR_HDR_COMPOSITE_MATH_SELFTEST", runHdrCompositeMathSelftest, false,
      "HdrCompositeMath: 16-bit premul source-over + 8-bit SSOT parity + extra-precision proof + paint order (6 gates)" },
    { "hdr-export16",     "VEDITOR_HDR_EXPORT16_SELFTEST",       runHdrExport16Selftest,       false,
      "TlrCompose16: export SSOT の matte-free 16bit RGBA64 積層→to8bit パリティ + env フラグ + extra-precision (8 gates)" },
    { "hdr-ingest-colormeta", "VEDITOR_HDR_INGEST_COLORMETA_SELFTEST", runHdrIngestColorMetaSelftest, false,
      "Stage10 ingest ColorMeta derivation from codec primaries/transfer/bit-depth + pixfmt depth extraction (14 gates)" },
    { "hdr-overlay-policy", "VEDITOR_HDR_OVERLAY_POLICY_SELFTEST", runHdrOverlayPolicySelftest, false,
      "Stage4 overlay RGBA64 デコード述語: VEDITOR_HDR_OVERLAY flag×isHdr 真理値表 + env トグル + 混在→8bit ルート確認 (8 gates)" },
    { "preview16-policy", "VEDITOR_PREVIEW16_POLICY_SELFTEST", runPreview16PolicySelftest, false,
      "preview16 policy predicate checks" },
    { "live-matte-resolve", "VEDITOR_LIVE_MATTE_RESOLVE_SELFTEST", runLiveMatteResolveSelftest, false,
      "LiveMatteResolve: clipId->index matte source resolution (STAGE4B) — valid + self/base/unknown rejection + no-matte/multi-matte (8 gates)" },
    { "matte-ssot", "VEDITOR_MATTE_SSOT_SELFTEST", runMatteSsotSelftest, true,
      "Timeline matte carrier: concurrent getter/setter snapshot, delete/split/external remap of owner and source keys, undo/redo restoration (6 gates)" },
    { "auto-proxy-policy",  "VEDITOR_AUTO_PROXY_POLICY_SELFTEST",  runAutoProxyPolicySelftest,     false,
      "AutoProxyPolicy: heavy-clip detection (width/codec) + useProxyFor/generateFor split + dedup (12 gates)" },
    { "exposure-aids",      "VEDITOR_EXPOSURE_AIDS_SELFTEST",      runExposureAidsSelftest,        false,
      "ExposureAids: luma709 oracle + False Color zones + Zebra stripe period + Focus Peaking edge/threshold + dimension/OOB safety (11 gates)" },
    { "import-ingest",      "VEDITOR_IMPORT_INGEST_SELFTEST",      runImportIngestSelftest,        false,
      "ImportHub ingest previews: mesh wireframe determinism + empty placeholder (QApplication-free)" },
    { "dynzoom",           "VEDITOR_DYNZOOM_SELFTEST",           runDynzoomSelftest,             false,
      "Dynamic Zoom pure builder: identity, zoom/pan direction, clamping, clip-time endpoints, ignored legacy height, and public-track single source (7 gates)" },
    { "layer-style",        "VEDITOR_LAYER_STYLE_SELFTEST",        runLayerStyleSelftest,          false,
      "LayerStyle: identity fast path, drop shadow, stroke, and project JSON omission/round-trip" },
    { "layer-style-ui",     "VEDITOR_LAYER_STYLE_UI_SELFTEST",     runLayerStyleUiSelftest,       true,
      "Layer Style UI: default identity, JSON round-trip, enabled-flag identity, and Timeline setter/getter checks (V1 legacy + track-aware)" },
    { "smart-render",       "VEDITOR_SMART_RENDER_SELFTEST",       runSmartRenderSelftest,        false,
      "Smart Render T4: conservative stream-copy eligibility predicate + env gate" },
    { "whisper-guide",      nullptr,                                runWhisperGuideSelftest,       false,
      "Whisper CLI resolution and subtitle translation preference persistence (5 gates)" },
    // QApplication-required (needsQApplication=true) ----------------------
    { "render-in-place", nullptr, runRenderInPlaceSelftest, true,
      "Render in place: image, handles, decompose, undo, persistence, overlap rejection, composition, chroma rejection (8 gates)" },
    { "render-notify", nullptr, runRenderNotifySelftest, true,
      "Export-complete notification: OFF silent, ON message and counter, failure count, no notify before allCompleted (4 gates)" },
    { "deflicker",         "VEDITOR_DEFLICKER_SELFTEST",          runDeflickerSelftest,         true,
      "Deflicker: global luma/RGB, streaming apply, cancellation, source resolution, and mask bounds (14 gates)" },
    { "adjustment-layer", "VEDITOR_ADJUSTMENT_LAYER_SELFTEST", runAdjustmentLayerSelftest, true,
      "Adjustment clip model: V1-wins z-order, behind-only effect stack, byte-identical off path, JSON true-only persistence" },
    { "frame-export",      "VEDITOR_FRAME_EXPORT_SELFTEST",        runFrameExportSelftest,        true,
      "Still-frame export: renderFrameAt SSOT -> PNG/JPEG save engine + PNG pixel-identical reread" },
    { "frame-clipboard",   "VEDITOR_FRAME_CLIPBOARD_SELFTEST",     runFrameClipboardSelftest,     true,
      "Current-frame clipboard MIME + live Edit action/renderFrameAt integration (17 gates)" },
    { "freeze-frame",      "VEDITOR_FREEZE_FRAME_SELFTEST",        runFreezeFrameSelftest,        true,
      "Freeze Frame: split at playhead, one-key hold curve, renderFrameAt export hold, save/load sync, one-step undo" },
    { "reverse-clip",      "VEDITOR_REVERSE_CLIP_SELFTEST",        runReverseClipSelftest,        true,
      "Reverse clip: source-time fold, speed composition, default-omit persistence, speed-aware reversed PCM, renderer parity, nested hold interval" },
    { "clip-curves",       "VEDITOR_CLIP_CURVES_SELFTEST",         runClipCurvesSelftest,         true,
      "Clip RGB/Luma curves: unset/identity byte identity, renderFrameAt reflection, ProjectFile round-trip" },
    { "hsl-secondary",     "VEDITOR_HSL_SECONDARY_SELFTEST",       runHslSecondarySelftest,       true,
      "HSL secondary qualifier: per-clip ProjectFile round-trip + renderFrameAt reflection + off-path byte identity" },
    { "clip-lut",          "VEDITOR_CLIP_LUT_SELFTEST",            runClipLutSelftest,            true,
      "Clip LUT G1-G10: ProjectFile round-trip, render reflection, no-LUT identity, MainWindow menu application + undo" },
    { "shape-clip",        "VEDITOR_SHAPE_CLIP_SELFTEST",          runShapeClipSelftest,          true,
      "Shape clip G1-G10: JSON, raster SSOT, preview/export, trim/repeater, modifier dialog + undo, legacy brush byte identity" },
    { "colormatch-apply",  "VEDITOR_COLORMATCH_APPLY_SELFTEST",    runColorMatchApplySelftest,    true,
      "ColorMatch apply: generated .cube -> selected clip LUT fields + undo + renderFrameAt reflection" },
    { "audio-fixture", nullptr, runAudioFixtureSelftest, true,
      "Export PCM fixture: deterministic WAV data, track gain, flat EQ and band EQ (10 gates); VEDITOR_AUDIO_FIXTURE_OUT required" },
    { "render-fixture", "VEDITOR_RENDER_FIXTURE_SELFTEST", runRenderFixtureSelftest, true,
      "Deterministic acceptance-lane render/audio dump (G1-G3; VEDITOR_RENDER_FIXTURE_OUT required)" },
    { "parity",            "VEDITOR_PARITY_SELFTEST",             runParitySelftest,             true,
      "Preview vs export pixel-parity (S1-S11, framediff::mse, 10-bit HDR10)" },
    { "parent-gate", "VEDITOR_PARENT_GATE_SELFTEST", runParentGateSelftest, true, "tlrender::parentLayerAt shared parent-layer gate vs renderFrameAt: times, offline parent, overlap pick, Start trail, outside interval, per-layer call count, V1 empty/ended boundary (7 gates)" },
    { "video-player-parent", "VEDITOR_VIDEO_PLAYER_PARENT_SELFTEST", runVideoPlayerParentSelftest, true,
      "VideoPlayer real parent admission (cpu-composite / multi-track) vs tlrender::parentLayerAt: normal, V1, adjustment, disabled, overlap incoming, Start trail, offline parent, observation OFF, per-path records (9 gates)" },
    { "keyframe-copy", "VEDITOR_KEYFRAME_COPY_SELFTEST", runKeyframeCopySelftest, true,
      "Graph editor keyframe clipboard: payload, cross-clip paste, collision, undo, transform track creation and placeholder isolation (7 gates)" },
    { "keyframe-anim-parity", "VEDITOR_KEYFRAME_ANIM_PARITY_SELFTEST", runKeyframeAnimParitySelftest, true,
      "Render-time per-clip motion/opacity keyframe evaluation parity (6 gates); byte-identical no-keyframe guard" },
    { "motion-blur-p2", "VEDITOR_MOTION_BLUR_P2_SELFTEST", runMotionBlurP2Selftest, true,
      "Motion blur P2 activation: env force OR per-clip opt-in + ProjectFile true-only persistence (5 gates)" },
    { "motion-blur-parity", "VEDITOR_MOTION_BLUR_PARITY_SELFTEST", runMotionBlurParitySelftest, true,
      "Export SSOT motion blur OFF-path parity + premultiplied temporal averaging helper (7 gates)" },
    { "effect-keyframe-parity", "VEDITOR_EFFECT_KEYFRAME_PARITY_SELFTEST", runEffectKeyframeParitySelftest, true,
      "Render-time per-clip effect parameter keyframe evaluation parity (9 gates); byte-identical no-keyframe guard" },
    { "effect-timing", "VEDITOR_EFFECT_TIMING_SELFTEST", runEffectTimingSelftest, true,
      "Clip-local VideoEffect active range filtering + ProjectFile JSON round-trip (4 gates)" },
    { "mediapool-drag", "VEDITOR_MEDIAPOOL_DRAG_SELFTEST", runMediaPoolDragSelftest, true,
      "MediaPool drag&drop: single/multi selection mimeData file URL generation" },
    { "text-spacing", "VEDITOR_TEXT_SPACING_SELFTEST", runTextSpacingSelftest, true,
      "Text letter/line spacing: default byte-identical, spacing widens render, toJson/fromJson roundtrip" },
    { "grade-wheel-wiring", "VEDITOR_GRADE_WHEEL_WIRING_SELFTEST", runGradeWheelWiringSelftest, true,
      "LGG wheels + WB -> per-clip ColorCorrection wiring: value mapping, restore, render reflection" },
    { "grade-keyframe-ext", "VEDITOR_GRADE_KEYFRAME_EXT_SELFTEST", runGradeKeyframeExtSelftest, true,
      "HSL secondary and color warper keyframes: interpolation, static identity, and project roundtrip (5 gates)" },
    { "grade-keyframe", "VEDITOR_GRADE_KEYFRAME_SELFTEST", runGradeKeyframeSelftest, true,
      "ColorCorrection grade.* keyframes: render evaluation, byte identity, undo, and roundtrip (12 gates)" },
    { "graph-editor", "VEDITOR_GRAPH_EDITOR_SELFTEST", runGraphEditorSelftest, true,
      "GraphEditor edit/undo, per-track Loop Out UI, ProjectFile persistence, and ClipAnim evaluation parity" },
    { "seq-from-clip", "VEDITOR_SEQ_FROM_CLIP_SELFTEST", runSeqFromClipSelftest, true,
      "Sequence from clip: placement, AV links, notification, undo, reference preservation, nested menus, audio-only media and generated clip menus (8 gates)" },
    { "sequence-dock", "VEDITOR_SEQUENCE_DOCK_SELFTEST", runSequenceDockSelftest, true,
      "Sequence dock: list, rename/create undo, active tracks and project persistence (4 gates)" },
    { "nest-sequence", "VEDITOR_NEST_SEQUENCE_SELFTEST", runNestSequenceSelftest, true,
      "Nested sequences: recursive render, cycle/depth guards, audio flatten, store roundtrip, no-nest byte identity" },
    { "precompose-e2e", "VEDITOR_PRECOMPOSE_E2E_SELFTEST", runPrecomposeE2ESelftest, true,
      "MainWindow precompose UI-flow backend: sequence ref replacement plus single/double undo store cleanup (4 gates)" },
    { "mainwindow-lifecycle", "VEDITOR_MAINWINDOW_LIFECYCLE_SELFTEST", runMainWindowLifecycleSelftest, true,
      "MainWindow lifecycle: construct/destroy once, repeat three times, and clear dependent dock objects (3 gates)" },
    { "track-ops", "VEDITOR_TRACK_OPS_SELFTEST", runTrackOpsSelftest, true,
      "Track removal/reorder: header menus, external state undo, exact row restoration and project persistence (12 gates)" },
    { "track-manage", "VEDITOR_TRACK_MANAGE_SELFTEST", runTrackManageSelftest, true,
      "Track names/colors: project roundtrip, single-step undo and invalid targets (3 gates)" },
    { "timeline-skim", "VEDITOR_TIMELINE_SKIM_SELFTEST", runTimelineSkimSelftest, true,
      "Timeline hover skimming: default off, shared cache mapping, preview restoration and no undo (4 gates)" },
    { "timeline-ergo", "VEDITOR_TIMELINE_ERGO_SELFTEST", runTimelineErgoSelftest, true,
      "Timeline selection, blade, gaps, duplicate, nudge, marquee, zoom, timecode, playhead moves, labels and linked selection (12 gates)" },
    { "ripple-delete", "VEDITOR_RIPPLE_DELETE_SELFTEST", runRippleDeleteSelftest, true,
      "Ripple delete and gap close: all-track time-range ripple, one-step undo, and no-selection no-op" },
    { "roto", "VEDITOR_ROTO_SELFTEST", runRotoSelftest, true,
      "Roto carrier mutation keys, idempotence, undo, project JSON, queue restore, and drag reorder (39 gates)" },
    { "e2e",               "VEDITOR_E2E_SELFTEST",                runE2eSelftest,                true,
      "Real-media end-to-end smoke (ColorMatch decode + deHum + processAll)" },
    { "trackmatte-parity", "VEDITOR_TRACKMATTE_PARITY_SELFTEST",  runTrackMatteParitySelftest,   true,
      "Track matte SSOT pixel-match across 4 matte types (luma/alpha/chroma/inv)" },
    { "gpu-composite-parity", "VEDITOR_GPU_COMPOSITE_PARITY_SELFTEST", runGpuCompositeParitySelftest, true,
      "GPU (GpuLayerCompositor FBO) vs CPU SSOT parity, matte-free scenes (SSIM>=0.98, MAE<=3..6/255, graceful GL skip) (16 gates)" },
    { "gpu-composite-parity-16-matte", "VEDITOR_GPU_COMPOSITE_PARITY_16_MATTE_SELFTEST", runGpuCompositeParity16MatteSelftest, true,
      "GPU RGBA16 track-matte (composite16Matte) vs CPU 16-bit matte oracle, all 4 matte types + premul + precision proof, graceful GL skip" },
    { "gpu-idt-parity", "VEDITOR_GPU_IDT_PARITY_SELFTEST", runGpuIdtParitySelftest, true,
      "GpuIdt: per-fragment IDT (eotf->mat3->oetf) vs aces:: CPU oracle (9 gates); includes Linear transfer, graceful GL skip" },
    { "gpu-idt-matte-parity", "VEDITOR_GPU_IDT_MATTE_PARITY_SELFTEST", runGpuIdtMatteParitySelftest, true,
      "GpuIdtMatte: per-fragment IDT matte path vs IDT-then-compositeReference16Matte oracle" },
    { "hdr-composite-parity", "VEDITOR_HDR_COMPOSITE_PARITY_SELFTEST", runHdrCompositeParitySelftest, true,
      "GPU RGBA16 FBO (composite16) vs CPU 16-bit oracle (HdrCompositeMath), matte-free (8 gates); includes 16-bit precision proof, graceful GL skip" },
    { "matte16-parity", "VEDITOR_MATTE16_PARITY_SELFTEST", runMatte16ParitySelftest, true,
      "16-bit RGBA64 track-matte (trackmatte16::composeExport) vs 8-bit trackmatte:: SSOT parity, all 4 matte types + 16-bit precision + ODT luma-space proof (8 gates)" },
    { "vfx",               "VEDITOR_VFX_SELFTEST",                runVfxSelftest,                true,
      "VFX module smoke: Sprint-6 effect graph + GLSL primitive pipeline" },
    { "vfx-generators",    "VEDITOR_VFX_GENERATORS_SELFTEST",     runVfxGeneratorsSelftest,     true,
      "VFX generators: seven deterministic procedural generators, RGBA compositor output, keyframes, dynamic-library registration, and ShockWave UV displacement (18 gates)" },
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
    { "whisper-word-timing", "VEDITOR_WHISPER_WORD_TIMING_SELFTEST", runWhisperWordTimingSelftest, true,
      "Whisper word-level timestamps: JSON full parse -> speech::Word -> SubtitleWord monotonic/boundary checks" },
    { "caption-edit",     "VEDITOR_CAPTION_EDIT_SELFTEST",       runCaptionEditSelftest,        true,
      "Caption split/merge pure functions and editor dialog buttons (6 gates)" },
    { "caption-overlay-builder", "VEDITOR_CAPTION_OVERLAY_BUILDER_SELFTEST", runCaptionOverlayBuilderSelftest, true,
      "Single-word caption builder, V1 bulk apply/undo, persistence, and renderer boundary checks" },
    { "transcript-highlighter", "VEDITOR_TRANSCRIPT_HIGHLIGHTER_SELFTEST", runTranscriptHighlighterSelftest, true,
      "Transcript highlighter scaffold smoke" },
    { "media-relink",     "VEDITOR_MEDIA_RELINK_SELFTEST",       runMediaRelinkSelftest,         true,
      "Offline media path enumeration, nested sequences, undo-safe relink, and persistence (6 gates)" },
    { "reduce-project", "VEDITOR_REDUCE_PROJECT_SELFTEST", runReduceProjectSelftest, true,
      "Unused media removal: shared provider, nested/sidecar references, confirmation and disk preservation (4 gates)" },
    { "mcp",              "VEDITOR_MCP_SELFTEST",                runMcpSelftest,                true,
      "MCP server: JSON-RPC framing + tool registry dispatch + HTTP transport auth + write-tool validation + AI chat helpers (188 gates)" },
    { "music-remix",      nullptr,                                  runMusicRemixSelftest,        true,
      "Music Remix: beat-boundary shortening/extension, intro/outro retention, crossfade, and one-step undo" },
    { "deesser", nullptr, runDeEsserSelftest, true,
      "De-esser: disabled bypass, band-only attenuation, threshold, playback/export parity, persistence, live dock, seek counter, dock follows project/track changes (8 gates)" },
    { "mixer-dock", nullptr, runMixerDockSelftest, true,
      "Audio mixer dock: track strips, existing gain path, flags, gesture undo, hidden identity, message handler under Qt signal/slot lock (10 gates)" },
    { "dialogue-leveler", nullptr,                                runDialogueLevelerSelftest,    true,
      "Dialogue Leveler: K-weighted envelope, silence gate, gain clamps, determinism, undo, and persistence" },
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
    { "fcpxml-import", "VEDITOR_FCPXML_IMPORT_SELFTEST", runFcpxmlImportSelftest, true,
      "FCPXML roundtrip, rational times, connected-clip lanes (gap children, J-cuts, invalid parents, empty spine), warnings and sequence undo (17 gates)" },
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
    { "batchexport-e2e",   "VEDITOR_BATCHEXPORT_E2E_SELFTEST",    runBatchExportE2ESelftest,    true,
      "Batch export dialog task bridge: live Timeline/source tagging, fps/config precedence, and pause signal contract, user preset loudness resolution, timeline audio mix and tempFiles cleanup (23 gates)" },
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
    { "warp-fx", "VEDITOR_WARP_FX_SELFTEST", runWarpFxSelftest, false,
      "Parametric CPU warps: identity, geometry, determinism, presets and shared render path (7 gates)" },
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
    { "morph-cut", "VEDITOR_MORPH_CUT_SELFTEST", runMorphCutSelftest, false,
      "Morph Cut motion, endpoints, determinism, ordinals, flow timing and cache isolation (6 gates)" },
    { "transition-params", "VEDITOR_TRANSITION_PARAMS_SELFTEST", runTransitionParamsSelftest, true,
      "Wipe, barn-door, iris and clock softness, border, persistence and shared render step (10 gates)" },
    { "transition-export", "VEDITOR_TRANSITION_EXPORT_SELFTEST", runTransitionExportSelftest, true,
      "Video transition export/preview shared steps and no-transition identity (9 gates)" },
    { "audio-trackfx", "VEDITOR_AUDIO_TRACKFX_SELFTEST", runAudioTrackFxSelftest, true,
      "Track DSP parity, headroom, overlap ordering, panel IDs and master EQ playback/export, open-dock reseed (39 gates)" },
    { "audio-xfade",       "VEDITOR_AUDIO_XFADE_SELFTEST",         runAudioXfadeSelftest,       true,
      "Audio constant-power crossfade/fade API and export afade=qsin contract (11 gates); QApplication required for Timeline undo/mirror checks" },
    { "oauth-mock-e2e",   "VEDITOR_OAUTH_MOCK_SELFTEST",        runOAuthMockE2eSelftest,       true,
      "OAuth + Upload pipeline を localhost mock HTTP server で exercise する 10 gate e2e selftest" },
    { "oauth-refresh-e2e", "VEDITOR_OAUTH_REFRESH_E2E_SELFTEST", runOAuthRefreshE2eSelftest,    true,
      "OAuth refresh_token auto-trigger e2e via mock server (10 gates). needs QApplication." },
    { "platform-mock-e2e", "VEDITOR_PLATFORM_MOCK_SELFTEST",      runPlatformMockE2eSelftest,    true,
      "Instagram Graph API + X Media Upload を localhost mock 経由で e2e exercise (13 gates): server + 5 IG + 7 X. QApplication 必要。" },
    { "watchdog", "VEDITOR_WATCHDOG_SELFTEST", runWatchdogSelftest, false,
      "Selftest watchdog: timeout exit 124, hang dump, armed/disabled lines, MCP port-exhaustion warning, per-entry re-arm in sweep (7 gates)" },
    { "watchdog-probe", nullptr, runWatchdogProbeSelftest, false,
      "内部用 (--selftest=watchdog の子プロセス。VEDITOR_WATCHDOG_PROBE_MODE=hang|slow)" },
};

const std::size_t kArgvSelftestsCount = sizeof(kArgvSelftests) / sizeof(kArgvSelftests[0]);

namespace {

struct EntryTally {
    bool active = false;
    bool reported = false;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
};

enum class GateCountStatus { Verified, Mismatch, Unverified, NoDeclaration };

std::mutex g_entryTallyMutex;
EntryTally g_entryTally;
GateCountStatus g_lastGateCountStatus = GateCountStatus::Unverified;

} // namespace

void reportTally(int passed, int failed, int skipped)
{
    std::lock_guard<std::mutex> lock(g_entryTallyMutex);
    if (!g_entryTally.active) return;
    g_entryTally.reported = true;
    g_entryTally.passed += passed;
    g_entryTally.failed += failed;
    g_entryTally.skipped += skipped;
}

static int runEntryChecked(const ArgvSelftestEntry &e)
{
    {
        std::lock_guard<std::mutex> lock(g_entryTallyMutex);
        g_entryTally = EntryTally{};
        g_entryTally.active = true;
    }
    const int rc = e.fn();
    EntryTally tally;
    {
        std::lock_guard<std::mutex> lock(g_entryTallyMutex);
        tally = g_entryTally;
        g_entryTally.active = false;
    }

    if (!tally.reported) {
        g_lastGateCountStatus = GateCountStatus::Unverified;
        std::cerr << "gate-count: unverified (no GateTally) (selftest="
                  << e.name << ")\n";
        return rc;
    }

    const int ran = tally.passed + tally.failed + tally.skipped;
    static const QRegularExpression declaration(QStringLiteral("\\(([0-9]+) gates\\)"));
    const QRegularExpressionMatch match = declaration.match(QString::fromUtf8(
        e.description ? e.description : ""));
    bool validNumber = false;
    const int declared = match.hasMatch() ? match.captured(1).toInt(&validNumber) : 0;
    if (!validNumber) {
        g_lastGateCountStatus = GateCountStatus::NoDeclaration;
        std::cerr << "gate-count: ran " << ran << ", no declaration (selftest="
                  << e.name << ")\n";
        return rc;
    }
    if (declared != ran) {
        g_lastGateCountStatus = GateCountStatus::Mismatch;
        std::cerr << "FAIL gate-count: declared " << declared << ", ran " << ran
                  << " (selftest=" << e.name << ")\n";
        return rc + 1;
    }
    g_lastGateCountStatus = GateCountStatus::Verified;
    std::cerr << "gate-count: declared " << declared << ", ran " << ran << '\n';
    return rc;
}

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
                armWatchdog(e.name);
                const int rc = runEntryChecked(e);
                disarmWatchdog();
                return rc;
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
        armWatchdog("selftest-trackmatte-parity");
        const int rc = runTrackMatteParitySelftest();
        disarmWatchdog();
        return rc;
    }

    if (args.contains(QStringLiteral("--selftest-trackmatte-export-integration"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-export-integration");
        armWatchdog("selftest-trackmatte-export-integration");
        const int rc = runTrackMatteExportIntegrationSelftest();
        disarmWatchdog();
        return rc;
    }

    if (args.contains(QStringLiteral("--selftest-trackmatte-reindex"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-reindex");
        armWatchdog("selftest-trackmatte-reindex");
        const int rc = runTrackMatteReindexSelftest();
        disarmWatchdog();
        return rc;
    }

    if (args.contains(QStringLiteral("--selftest-trackmatte-rm6-duplicate"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-rm6-duplicate");
        armWatchdog("selftest-trackmatte-rm6-duplicate");
        const int rc = runTrackMatteRm6DuplicateSelftest();
        disarmWatchdog();
        return rc;
    }

    if (args.contains(QStringLiteral("--selftest-trackmatte-rm5-reorder"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-rm5-reorder");
        armWatchdog("selftest-trackmatte-rm5-reorder");
        const int rc = runTrackMatteRm5ReorderSelftest();
        disarmWatchdog();
        return rc;
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
        // US-700: VEDITOR_ALL_SELFTEST_FILTER = ordered, comma-separated list
        // (duplicates allowed). Unset / empty keeps the full table order.
        // A nonempty filter with an empty item (e.g. "," or "a,,b") is rejected.
        std::vector<const ArgvSelftestEntry*> sweep;
        const QString filterRaw = qEnvironmentVariable("VEDITOR_ALL_SELFTEST_FILTER");
        const QStringList filter = filterRaw.isEmpty()
            ? QStringList() : filterRaw.split(QLatin1Char(','));
        for (const QString& raw : filter) {
            const QString name = raw.trimmed();
            if (name.isEmpty()) {
                std::cerr << "empty item in VEDITOR_ALL_SELFTEST_FILTER: \""
                          << filterRaw.toStdString() << "\"" << std::endl;
                return 2;
            }
            const ArgvSelftestEntry* found = nullptr;
            for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
                if (name == QString::fromLatin1(kArgvSelftests[j].name)) {
                    found = &kArgvSelftests[j];
                    break;
                }
            }
            if (!found) {
                std::cerr << "unknown selftest in VEDITOR_ALL_SELFTEST_FILTER: "
                          << name.toStdString() << std::endl;
                return 2;
            }
            sweep.push_back(found);
        }
        if (filter.isEmpty()) {
            for (std::size_t j = 0; j < kArgvSelftestsCount; ++j)
                sweep.push_back(&kArgvSelftests[j]);
        }
        writeLogLine("INFO", "argv-switch: --selftest=all (full sweep)");
        int passed = 0;
        int failed = 0;
        int verified = 0;
        int mismatch = 0;
        int unverified = 0;
        int noDeclaration = 0;
        for (const ArgvSelftestEntry* entry : sweep) {
            const auto& e = *entry;
            writeLogLine("INFO",
                QString("[--selftest=all] running --selftest=%1").arg(QString::fromLatin1(e.name)));
            armWatchdog(e.name);
            const int rc = runEntryChecked(e);
            disarmWatchdog();
            switch (g_lastGateCountStatus) {
            case GateCountStatus::Verified: ++verified; break;
            case GateCountStatus::Mismatch: ++mismatch; break;
            case GateCountStatus::Unverified: ++unverified; break;
            case GateCountStatus::NoDeclaration: ++noDeclaration; break;
            }
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
        std::cerr << "gate-count summary: verified " << verified
                  << ", mismatch " << mismatch << ", unverified " << unverified
                  << ", no-declaration " << noDeclaration << '\n';
        return failed == 0 ? 0 : failed;
    }

    // PRD-ARGV-TABLE: QApp-required argv-switch entries.
    for (std::size_t j = 0; j < kArgvSelftestsCount; ++j) {
        const auto& e = kArgvSelftests[j];
        if (!e.needsQApplication) continue;
        const QString sw = QStringLiteral("--selftest=%1").arg(QString::fromLatin1(e.name));
        if (args.contains(sw)) {
            writeLogLine("INFO", QString("argv-switch: %1").arg(sw));
            armWatchdog(e.name);
            const int rc = runEntryChecked(e);
            disarmWatchdog();
            return rc;
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
            armWatchdog(e.name);
            const int rc = runEntryChecked(e);
            disarmWatchdog();
            return rc;
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
