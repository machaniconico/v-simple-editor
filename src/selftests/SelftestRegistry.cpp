// src/selftests/SelftestRegistry.cpp
// PRD-SPLIT-MAIN-1: selftest dispatch table + 3 helper functions.
// The run<Foo>Selftest() bodies remain in src/main.cpp; this file only
// owns the routing (struct table + dispatchPre/Post/validateUnknown).

#include "SelftestRegistry.h"

#include <cstring>
#include <iostream>
#include <QString>
#include <QStringList>
#include <QCoreApplication>
#include <QDebug>
#include "../util/Logger.h"

// ---------------------------------------------------------------------------
// Forward declarations for the selftest functions implemented in src/main.cpp.
// Kept alphabetically sorted for ease of maintenance.
// ---------------------------------------------------------------------------
int runAIHighlightSelftest();
int runAffinitySelftest();
int runOAuthMockE2eSelftest();
int runOAuthRefreshE2eSelftest();
int runAnimExportSelftest();
int runAudioBusSelftest();
int runAudioMixerSelftest();
int runAudioRestoreSelftest();
int runAutoClipGenSelftest();
int runBatchExportSelftest();
int runBlenderSelftest();
int runCaptionSelftest();
int runChromaSelftest();
int runCloudRenderSelftest();
int runCollabSelftest();
int runColorMatchSelftest();
int runCommandSearchSelftest();
int runCredAuditLogSelftest();
int runCredTtlSelftest();
int runCredentialVaultSelftest();
int runDavinciSelftest();
int runE2eSelftest();
int runEasingSelftest();
int runExportAuditSelftest();
int runFcpxmlSelftest();
int runFrameIoSelftest();
int runHdrRoutingSelftest();
int runHdrSelftest();
int runHwPerfSelftest();
int runImportSelftest();
int runInstagramSelftest();
int runLibavcoreDecodeSelftest();
int runLibavcoreEncodeSelftest();
int runLoudnessSelftest();
int runLowerThirdSelftest();
int runMediaPoolSelftest();
int runMobileSelftest();
int runMographSelftest();
int runMultiCamSelftest();
int runObsSelftest();
int runParitySelftest();
int runPlanarSelftest();
#if __has_include("PlanarTrackerPreset.h")
int runPlanarPresetSelftest();
#define HAVE_PLANARTRACKER_PRESET 1
#endif
int runPlatformMockE2eSelftest();
int runProExtSelftest();
int runProSelftest();
int runProjTmplSelftest();
int runProjectPresetSelftest();
int runProxySelftestV2();
int runShortcutSelftest();
int runSmartEditSelftest();
int runSocialSelftest();
int runSubXlatSelftest();
int runTextExportSelftest();
int runThreePointEditSelftest();
int runTrackMatteExportIntegrationSelftest();
int runTrackMatteParitySelftest();
int runTrackMatteReindexSelftest();
int runTrackMatteRm5ReorderSelftest();
int runTrackMatteRm6DuplicateSelftest();
int runTranscriptHighlighterSelftest();
int runTrackerPresetSelftest();
int runTrimOpsSelftest();
int runTwitchSelftest();
int runVfxSelftest();
int runVideostabDeshakeSelftest();
int runVimeoSelftest();
int runWatermarkSelftest();
int runWhisperTranscribeSelftest();
int runWorkflowSelftest();
int runXUploadSelftest();
int runYoutubeSelftest();
int runYtdlpDownloaderSelftest();
int runPremiereXmlSelftest();
int runYoutubeChapterSelftest();

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
    { "tracker-preset",    "VEDITOR_TRACKER_PRESET_SELFTEST",     runTrackerPresetSelftest,      false,
      "MotionTracker preset 7 built-in + Registry + JSON round-trip (7 gates)" },
#ifdef HAVE_PLANARTRACKER_PRESET
    { "planar-preset",     "VEDITOR_PLANAR_PRESET_SELFTEST",      runPlanarPresetSelftest,       false,
      "PlanarTracker preset 5 built-in + Registry + JSON round-trip (10 gates)" },
#endif
    { "project-preset",    "VEDITOR_PROJECT_PRESET_SELFTEST",     runProjectPresetSelftest,      false,
      "Tracker preset state persistence in ProjectFile save/load cycle (10 gates)" },
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
    // QApplication-required (needsQApplication=true) ----------------------
    { "parity",            "VEDITOR_PARITY_SELFTEST",             runParitySelftest,             true,
      "Preview vs export pixel-parity (S1-S11, framediff::mse, 10-bit HDR10)" },
    { "e2e",               "VEDITOR_E2E_SELFTEST",                runE2eSelftest,                true,
      "Real-media end-to-end smoke (ColorMatch decode + deHum + processAll)" },
    { "trackmatte-parity", "VEDITOR_TRACKMATTE_PARITY_SELFTEST",  runTrackMatteParitySelftest,   true,
      "Track matte SSOT pixel-match across 4 matte types (luma/alpha/chroma/inv)" },
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
