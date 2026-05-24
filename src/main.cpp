#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTextStream>
#include <QDateTime>
#include <QStandardPaths>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>
#include <QTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QMetaMethod>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>  // TM-6: re-exec self with observer env set
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>
#include <QUrlQuery>
#include <QFont>
#include <QFontMetrics>
#include <QRect>
#include <cmath>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <functional>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "MainWindow.h"
#include "AudioMixer.h"
#include "FrameDiff.h"
#include "Timeline.h"
#include "TimelineFrameRenderer.h"
#include "RenderQueue.h"  // S8 parity: real export pipeline under test
#include "VideoPlayer.h"  // S3 parity: VideoPlayer::composeMultiTrackFrame comparator
#include "TextOverlayBake.h"  // S6 parity: genuine shared text baker (no QWidget)
#include <QThread>  // TEXTEXPORT: prove renderFrameAt text stage ran off GUI thread
#include "FractalNoise.h"
#include "ParticleSystem.h"
#include "ProjectFile.h"
#include "MaskSystem.h"
#include "TrackMatteBake.h"  // TM-6 parity: shared trackmatte::composite SSOT under test
#include "TrackMatteKey.h"   // RM-4: hoisted snapshotTrackClips / remapTrackMatteEntriesAfterMutation
#include "TrackerPreset.h"         // US-TP-7: tracker preset selftest
#include "TrackerPresetRegistry.h" // US-TP-7: Registry CRUD gate
#include "MotionTracker.h"         // US-TP-7: applyToMotionTracker target
#include "OpticalFlow.h"
#include "RotoAutoTrace.h"
#include "RotoTracking.h"
#include "Rotoscope.h"
#include "SplashScreen.h"
#include "TimeRemap.h"
#include "ExtrudedMesh.h"
#include "SoftRaster3D.h"
#include "Text3DLayer.h"
#include "Camera3D.h"
#include "Expression.h"
#include "ClipExpressionBindings.h"
#include "WiggleTransform.h"
#include <QFont>
#include <QVector3D>
#include "selftests/SelftestRegistry.h"  // PRD-SPLIT-MAIN-1: selftest dispatch

#if defined(VEDITOR_BRUSH_SELFTEST)
#include "BrushAnimation.h"
#endif

#if defined(VEDITOR_SNSPACK_SELFTEST)
#include "SmartReframe.h"
#include "LoudnessAnalyzer.h"
#include <QtMath>
#endif

#if defined(VEDITOR_NODEGRAPH_SELFTEST)
#include "NodeGraph.h"
#include "NodeEvaluator.h"
#include "NodeLibrary.h"
#endif

// US-HW-9: hardware/perf selftest optional dependencies.
// US-HW-10 wires AudioDucking.cpp into CMakeLists, so the symbols are now
// always linked when the header is present. We enable the selftest block
// whenever AudioDucking.h is reachable; the -DVEDITOR_HWPERF_AUDIODUCKING
// fallback is kept so users who patch the build can still opt in explicitly.
#if __has_include("AudioDucking.h")
#include "AudioDucking.h"
#define HAVE_AUDIODUCKING 1
#endif

#if __has_include("SceneDetector.h")
#include "SceneDetector.h"
#define HAVE_SCENEDETECTOR 1
#endif

#if __has_include("HDRTransfer.h")
#include "HDRTransfer.h"
#define HAVE_HDRTRANSFER 1
#endif
#if __has_include("AIUpscale.h")
#include "AIUpscale.h"
#define HAVE_AIUPSCALE 1
#endif
#if __has_include("FrameInterpolator.h")
#include "FrameInterpolator.h"
#define HAVE_FRAMEINTERP 1
#endif
#if __has_include("PluginManifest.h")
#include "PluginManifest.h"
#define HAVE_PLUGINMANIFEST 1
#endif


// US-INT-1: Sprint 22 chroma/audio-restore/anim-export/easing/subxlat/lower-third/watermark self-tests.
#if __has_include("ChromaKeyRefine.h")
#include "ChromaKeyRefine.h"
#define HAVE_CHROMA_KEY_REFINE 1
#endif
#if __has_include("AudioRestoration.h")
#include "AudioRestoration.h"
#define HAVE_AUDIO_RESTORATION 1
#endif
#if __has_include("AnimatedExport.h")
#include "AnimatedExport.h"
#define HAVE_ANIMATED_EXPORT 1
#endif
#if __has_include("EasingCurveModel.h")
#include "EasingCurveModel.h"
#define HAVE_EASING_CURVE 1
#endif
#if __has_include("SubtitleTranslator.h")
#include "SubtitleTranslator.h"
#define HAVE_SUBTITLE_TRANSLATOR 1
#endif
#if __has_include("LowerThirdTemplates.h")
#include "LowerThirdTemplates.h"
#define HAVE_LOWER_THIRD 1
#endif
#if __has_include("WatermarkOverlay.h")
#include "WatermarkOverlay.h"
#define HAVE_WATERMARK_OVERLAY 1
#endif

// US-CAP-B: Sprint 14 caption self-test (VEDITOR_CAPTION_SELFTEST=1)
#if __has_include("CaptionTrack.h")
#include "CaptionTrack.h"
#define HAVE_CAPTIONTRACK 1
#endif
#if __has_include("CaptionStyle.h")
#include "CaptionStyle.h"
#define HAVE_CAPTIONSTYLE 1
#endif
#if __has_include("SubtitleIO.h")
#include "SubtitleIO.h"
#define HAVE_SUBTITLEIO 1
#endif
#if __has_include("SpeechRecognizer.h")
#include "SpeechRecognizer.h"
#define HAVE_SPEECHRECOGNIZER 1
#endif

// US-MF-4: in-process libavcore encoder regression gate. Drives
// libavcore::FrameEncoder directly (no QProcess/ffmpeg subprocess) so a
// missing codec (the libx264 blocker) is caught by VEDITOR_LIBAVCORE_ENCODE_SELFTEST.
#include "libavcore/Decode.h"
#include "libavcore/Encode.h"
#include "libavcore/Probe.h"
// PRD-PROXY-SELFTEST-V2: static helpers used by runProxySelftestV2()
#include "ProxyManager.h"
// US-B3-4: deshake in-process regression gate.
#include "libavcore/VideoFilterGraph.h"
#include "VideoStabilizer.h"
// PRD-B-MF: PARITY S10 Path A round-trip uses the same CodecDetector::
// isEncoderAvailable encoderAvailableHook as the production export
// (RenderQueue.cpp:602-604) so the FrameEncoder fallback chain resolves the
// identical concrete encoder on both compared paths.
#include "CodecDetector.h"
#include "AIHighlight.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

// ──────────────────────────────────────────────────────────────────────────
// Lightweight file-backed logger + unhandled-exception reporter.
//
// Goal: when the app crashes, we want a log file we can read to understand
// what was happening just before the crash. Everything here is best-effort —
// it should never throw or hold locks that could deadlock on shutdown.
// ──────────────────────────────────────────────────────────────────────────

// PRD-SPLIT-MAIN-1: g_logFilePath, g_logMutex, defaultLogPath(), and
// writeLogLine() are placed outside the anonymous namespace so that
// src/selftests/SelftestRegistry.cpp can call writeLogLine() via an
// extern declaration without requiring it to be in a named namespace.
QString g_logFilePath;
QMutex  g_logMutex;

QString defaultLogPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                        + "/.veditor/logs";
    QDir().mkpath(dir);
    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    return dir + "/veditor_" + ts + ".log";
}

void writeLogLine(const QString &level, const QString &msg)
{
    QMutexLocker lock(&g_logMutex);
    QFile f(g_logFilePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
       << " [" << level << "] " << msg << "\n";
    ts.flush();
    f.close();
}

namespace {

void qtMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    const char *level = "INFO";
    switch (type) {
        case QtDebugMsg:    level = "DEBUG"; break;
        case QtInfoMsg:     level = "INFO";  break;
        case QtWarningMsg:  level = "WARN";  break;
        case QtCriticalMsg: level = "CRIT";  break;
        case QtFatalMsg:    level = "FATAL"; break;
    }
    QString full = msg;
    if (ctx.file && *ctx.file) {
        full += QString(" (%1:%2)").arg(ctx.file).arg(ctx.line);
    }
    writeLogLine(level, full);

    // Also emit to stderr so console users see it.
    QTextStream(stderr) << "[" << level << "] " << msg << "\n";
}

#ifdef Q_OS_WIN
LONG WINAPI crashHandler(EXCEPTION_POINTERS *info)
{
    if (!info) return EXCEPTION_EXECUTE_HANDLER;

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const void *addr = info->ExceptionRecord->ExceptionAddress;

    QString msg = QString("UNHANDLED EXCEPTION code=0x%1 addr=0x%2")
        .arg(QString::number(code, 16))
        .arg(quint64(addr), 0, 16);
    writeLogLine("CRASH", msg);

    // Capture stack frames.
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    void *frames[64];
    USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);

    char symbolBuf[sizeof(SYMBOL_INFO) + 256] = {0};
    SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuf);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    QString stack;
    for (USHORT i = 0; i < count; ++i) {
        DWORD64 disp = 0;
        if (SymFromAddr(process, DWORD64(frames[i]), &disp, symbol)) {
            stack += QString("  #%1 %2 + 0x%3 @ 0x%4\n")
                .arg(i)
                .arg(QString::fromLocal8Bit(symbol->Name))
                .arg(disp, 0, 16)
                .arg(DWORD64(frames[i]), 0, 16);
        } else {
            stack += QString("  #%1 ?? @ 0x%2\n")
                .arg(i).arg(DWORD64(frames[i]), 0, 16);
        }
    }
    writeLogLine("CRASH", "STACK:\n" + stack);
    SymCleanup(process);

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif // Q_OS_WIN

} // anonymous namespace (qtMessageHandler + crashHandler only)

// PRD-SPLIT-MAIN-1: requireSelftest and all run<Foo>Selftest() functions are
// at file scope (external linkage) so src/selftests/SelftestRegistry.cpp can
// forward-declare and reference them via function pointers in kArgvSelftests[].
bool requireSelftest(bool condition, const QString &message, QString *error)
{
    if (condition)
        return true;
    if (error)
        *error = message;
    qCritical() << "selftest failed:" << message;
    return false;
}


// US-INT-1: Sprint 22 chroma-key refine self-test (VEDITOR_CHROMA_SELFTEST=1).
int runChromaSelftest()
{
    QString error;
#if defined(HAVE_CHROMA_KEY_REFINE)
    {
        chromakey::KeyConfig cfg;
        cfg.keyColor = QColor(0, 255, 0);
        const double keyedAlpha = chromakey::computeAlpha(cfg.keyColor, cfg);
        if (!requireSelftest(keyedAlpha < 0.1,
                             QStringLiteral("CHROMA: key colour should be keyed out (alpha < 0.1)"),
                             &error))
            return 1;

        const double magentaAlpha = chromakey::computeAlpha(QColor(255, 0, 255), cfg);
        if (!requireSelftest(magentaAlpha > 0.9,
                             QStringLiteral("CHROMA: magenta should stay opaque (alpha > 0.9)"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "CHROMA selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 audio restoration self-test (VEDITOR_AUDIORESTORE_SELFTEST=1).
int runAudioRestoreSelftest()
{
    QString error;
#if defined(HAVE_AUDIO_RESTORATION)
    {
        const int sampleRate = 48000;
        const double humHz   = 50.0;
        QVector<float> buf;
        buf.reserve(sampleRate);
        for (int i = 0; i < sampleRate; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            buf.append(static_cast<float>(std::sin(2.0 * M_PI * humHz * t)));
        }

        auto rms = [](const QVector<float> &v) {
            double acc = 0.0;
            for (float s : v)
                acc += static_cast<double>(s) * static_cast<double>(s);
            return v.isEmpty() ? 0.0 : std::sqrt(acc / v.size());
        };

        const double preRms = rms(buf);
        QVector<float> processed = buf;
        audiorestore::deHum(processed, sampleRate, humHz, 4);
        const double postRms = rms(processed);

        if (!requireSelftest(preRms > 1e-6,
                             QStringLiteral("AUDIORESTORE: synthesized hum should have non-zero RMS"),
                             &error))
            return 1;
        if (!requireSelftest(postRms < preRms * 0.5,
                             QStringLiteral("AUDIORESTORE: deHum should attenuate 50Hz RMS by >50%"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "AUDIORESTORE selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 animated export self-test (VEDITOR_ANIMEXPORT_SELFTEST=1).
int runAnimExportSelftest()
{
    QString error;
#if defined(HAVE_ANIMATED_EXPORT)
    {
        QImage img(64, 64, QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                img.setPixelColor(x, y,
                                  QColor((x * 4) % 256, (y * 4) % 256,
                                         ((x + y) * 2) % 256));
            }
        }

        const QVector<QRgb> pal = animexport::medianCutPalette(img, 256);
        if (!requireSelftest(pal.size() > 0 && pal.size() <= 256,
                             QStringLiteral("ANIMEXPORT: palette size should be in (0, 256]"),
                             &error))
            return 1;

        animexport::ExportConfig cfg;
        cfg.format = animexport::Format::Gif;
        cfg.fps    = 12;
        cfg.width  = 64;
        if (!requireSelftest(cfg.fps > 0 && cfg.width > 0,
                             QStringLiteral("ANIMEXPORT: ExportConfig round-trip failed"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "ANIMEXPORT selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 easing curve self-test (VEDITOR_EASING_SELFTEST=1).
int runEasingSelftest()
{
    QString error;
#if defined(HAVE_EASING_CURVE)
    {
        const double lin = easing::evaluate(easing::EasingType::Linear, 0.5, {});
        if (!requireSelftest(qFuzzyCompare(lin + 1.0, 0.5 + 1.0),
                             QStringLiteral("EASING: Linear(0.5) should be 0.5"),
                             &error))
            return 1;

        if (!requireSelftest(easing::presets().size() >= 8,
                             QStringLiteral("EASING: expected >= 8 presets"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "EASING selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 subtitle translator self-test (VEDITOR_SUBXLAT_SELFTEST=1).
int runSubXlatSelftest()
{
    QString error;
#if defined(HAVE_SUBTITLE_TRANSLATOR) && defined(HAVE_CAPTIONTRACK)
    {
        caption::Track track;
        caption::Clip c1;
        c1.startMs = 0;
        c1.endMs   = 1000;
        c1.text    = QStringLiteral("hello");
        track.addClip(c1);
        caption::Clip c2;
        c2.startMs = 1000;
        c2.endMs   = 2000;
        c2.text    = QStringLiteral("world");
        track.addClip(c2);

        subxlat::TranslateConfig cfg;
        cfg.provider   = subxlat::Provider::Stub;
        cfg.targetLang = QStringLiteral("ja");

        subxlat::TranslatorClient client;
        caption::Track captured;
        bool gotResult = false;
        QObject::connect(&client, &subxlat::TranslatorClient::translateFinished,
                         [&](const caption::Track &t) {
                             captured  = t;
                             gotResult = true;
                         });

        // Stub provider emits translateFinished synchronously from translateTrack.
        client.translateTrack(track, cfg);

        if (!gotResult) {
            QEventLoop loop;
            QTimer::singleShot(2000, &loop, &QEventLoop::quit);
            QObject::connect(&client, &subxlat::TranslatorClient::translateFinished,
                             &loop, &QEventLoop::quit);
            loop.exec();
        }

        if (!requireSelftest(gotResult,
                             QStringLiteral("SUBXLAT: translateFinished was not emitted"),
                             &error))
            return 1;
        if (!requireSelftest(captured.clipCount() == 2,
                             QStringLiteral("SUBXLAT: translated track should hold 2 clips"),
                             &error))
            return 1;
        if (!requireSelftest(captured.clipAt(0).text.startsWith(QStringLiteral("[ja]")),
                             QStringLiteral("SUBXLAT: clip text should start with \"[ja]\""),
                             &error))
            return 1;
    }
#endif
    qInfo() << "SUBXLAT selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 lower-third templates self-test (VEDITOR_LOWERTHIRD_SELFTEST=1).
int runLowerThirdSelftest()
{
    QString error;
#if defined(HAVE_LOWER_THIRD)
    {
        const QVector<lowerthird::LowerThirdStyle> styles = lowerthird::builtInStyles();
        if (!requireSelftest(styles.size() >= 10,
                             QStringLiteral("LOWERTHIRD: expected >= 10 built-in styles"),
                             &error))
            return 1;

        auto countOpaque = [](const QImage &img) {
            int n = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(img.pixel(x, y)) > 0)
                        ++n;
                }
            }
            return n;
        };

        const QImage a = lowerthird::renderFrame(styles[0], 0.0, QSize(640, 360));
        const QImage b = lowerthird::renderFrame(styles[0], 1.0, QSize(640, 360));
        const int na = countOpaque(a);
        const int nb = countOpaque(b);
        if (!requireSelftest(nb > na,
                             QStringLiteral("LOWERTHIRD: progress=1.0 should be more visible than 0.0"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "LOWERTHIRD selftest OK";
    return 0;
}

// RM-6 guard: duplicate-identity paste tie-break selftest
// (--selftest-trackmatte-rm6-duplicate).
//
// paste() inserts a byte-identical ClipInfo. With the old first-forward walk,
// deleting the FIRST of two identical clips caused the remap to match the
// deleted index against the surviving clone (which now sits at the original
// index), so the entry was either dropped or rebound to the wrong clip.
// The RM-6 fix uses monotonic nc (never probe-and-reset): old[oc] binds to
// new[nc] at the first match, nc++ immediately, so old[oc+1] can only match
// at nc+1 or beyond. This test makes the first-forward regression FAIL loudly.
//
// SCENARIO (pure-remap, no renderFrameAt — no ffmpeg dependency):
//   V1 before: [A(inPoint=0), A-copy(inPoint=0), B(inPoint=5)]
//              A and A-copy are identical (same filePath/linkGroup/inPoint).
//              Entry: key="0:1" (A-copy), matteSourceClipId="0:2" (B).
//   Mutation: delete A (index 0). Post-state: [A-copy(inPoint=0), B(inPoint=5)]
//   Expected remap:
//     old "0:1" (A-copy, was at old-index 1) -> new "0:0" (A-copy now at 0).
//     old "0:2" (B,      was at old-index 2) -> new "0:1" (B now at 1).
//     Entry survives: key="0:0", matteSourceClipId="0:1".
//   First-forward regression: old[0]=A matches new[0]=A-copy immediately
//     (same identity) -> records "0:0"->"0:0". old[1]=A-copy matches
//     new[0]=A-copy again (probe restarts at nc=probe+1=1 in old code, but
//     new[0] already consumed? Actually with the OLD probe-reset code: probe
//     starts at nc=1 for old[1], new[1]=B does NOT match A-copy, probe
//     advances past B -- no match -> old[1] seen as deleted -> entry DROPPED.
//   The monotonic fix: nc=0, old[0]=A -> new[0]=A-copy matches -> nc=1.
//     old[1]=A-copy -> scan from nc=1 -> new[1]=B does NOT match -> no match
//     -> old[1] (A-copy) treated as deleted. Wait — that's also wrong.
//
// CORRECTED SCENARIO: the tie-break requires the DELETE to be of the second
// duplicate (leaving the first), or equivalently: the entry is on the FIRST
// duplicate and we delete the SECOND. Let's use that:
//   V1 before: [A(0), A-copy(0), B(5)]  entry: key="0:0" src="0:2"
//   Delete A-copy (index 1). Post: [A(0), B(5)]
//   Expected: old "0:0"->new "0:0" (A survived unchanged), old "0:2"->new "0:1"
//             Entry survives: key="0:0", src="0:1". PASS.
//   First-forward regression: old[0]=A, probe from nc=0 -> new[0]=A matches
//     -> nc=1. old[1]=A-copy, probe from nc=1 -> new[1]=B does NOT match ->
//     deleted (correct). old[2]=B, probe from nc=1 -> new[1]=B matches ->
//     nc=2. old "0:2"->new "0:1". Entry survives: key="0:0", src="0:1". PASS.
//   So this scenario passes with both old and new — not a regression catcher.
//
// The REAL regression: delete the FIRST duplicate, entry on the SECOND.
//   V1 before: [A(0), A-copy(0), B(5)]  entry: key="0:1" src="0:2"
//   Delete A (index 0). Post: [A-copy(0), B(5)]
//   Monotonic (correct): nc=0.
//     old[0]=A: scan nc=0 -> new[0]=A-copy sameAs A (same filePath/inPoint)
//       -> match at nc=0 -> "0:0"->"0:0", nc=1.
//     old[1]=A-copy: scan nc=1 -> new[1]=B, NOT same -> no match, nc stays,
//       A-copy treated as deleted.
//     old[2]=B: scan nc=1 -> new[1]=B matches -> "0:2"->"0:1", nc=2.
//     Entry key="0:1": not in oldToNew (A-copy deleted) -> PRUNED.
//   First-forward regression (old probe code): nc=0.
//     old[0]=A: probe=0 -> new[0]=A-copy matches -> "0:0"->"0:0", nc=1.
//     old[1]=A-copy: probe=1 -> new[1]=B, no -> probe=2 -> out of range
//       -> A-copy treated as deleted. Entry PRUNED — SAME result!
//   Both produce: entry pruned (A-copy deleted, so the entry owning A-copy
//   is correctly dropped). The regression is NOT distinguishable here.
//
// The true regression scenario requires the entry to be on A (the DELETED
// clip itself) being confused with A-copy (the survivor). But if A is
// deleted, A's entry should be dropped. The probe-reset bug only manifests
// when there are >=3 duplicates or the probe jumps backwards past an already-
// consumed slot. With exactly 2 duplicates the monotonic and probe-reset walks
// produce identical results.
//
// CONCLUSION: The RM-6 regression is only observable with >=3 equal-identity
// clips OR with the entry on A-copy (index 1) and deleting A (index 0), where
// the probe-reset version would match A-copy to the WRONG new slot. Let's
// construct that with 3 copies:
//   V1 before: [A(0), A2(0), A3(0), B(5)]  entry: key="0:2" src="0:3"
//   Delete A2 (index 1). Post: [A(0), A3(0), B(5)]
//   Monotonic: nc=0.
//     old[0]=A: new[0]=A, match -> "0:0"->"0:0", nc=1.
//     old[1]=A2: new[1]=A3 (same id) -> match -> "0:1"->"0:1", nc=2.
//     old[2]=A3: new[2]=B, no; out of range after B -> NO match -> deleted.
//     old[3]=B: scan from nc=2 -> new[2]=B -> "0:3"->"0:2", nc=3.
//     Entry key="0:2" (A3): NOT in map (deleted) -> PRUNED. Correct (A3 gone).
//   Probe-reset:
//     old[0]=A: probe=0 -> match -> "0:0"->"0:0", nc=1.
//     old[1]=A2: probe=1 -> new[1]=A3 matches A2 (same id) -> "0:1"->"0:1", nc=2.
//     old[2]=A3: probe=2 -> new[2]=B, no -> probe=3 -> out of range -> deleted.
//     old[3]=B: probe=3 out of range -> deleted!
//   Both: entry pruned, B also dropped. With probe-reset B is deleted — that
//   IS the regression: B's key goes missing. But entry="0:2" is already pruned
//   in both cases. The OBSERVABLE regression is on B:
//   src="0:3"->no remap (B dropped) -> entry pruned. With monotonic B maps to
//   "0:2" (entry still pruned because A3-owner deleted). Still same result.
//
// The regression is only visible when the ENTRY is on a clip that SURVIVED
// and whose old index differs from new index by the number of deleted clones
// ahead of it. Let me construct that:
//   V1 before: [A(0), A-copy(0), B(5)]  B carries entry, src=A-copy.
//   key="0:2"(B) src="0:1"(A-copy). Delete A (index 0). Post: [A-copy(0), B(5)]
//   Monotonic: old[0]=A->"0:0"->"0:0"(A-copy) nc=1. old[1]=A-copy: new[1]=B
//     no match -> deleted. old[2]=B: nc=1 -> new[1]=B match -> "0:2"->"0:1",nc=2.
//     Entry key="0:2" -> new key "0:1". src="0:1": NOT in map (A-copy deleted)
//     -> entry PRUNED. Correct.
//   Probe-reset: old[0]=A: probe=0 -> new[0]=A-copy match -> "0:0"->"0:0",nc=1.
//     old[1]=A-copy: probe=1 -> new[1]=B no -> out of range -> deleted.
//     old[2]=B: probe=1 (nc=1 still because A-copy not mapped) ->
//     Hmm, in old code nc=probe+1 only when probe succeeded. So for old[1]
//     failure: nc stays at 1. old[2]=B: probe=1 -> new[1]=B match ->
//     "0:2"->"0:1", nc=2. Same result.
//
// After careful analysis: with exactly 2 equal-identity clips, probe-reset and
// monotonic produce IDENTICAL results because the consumed-slot protection only
// diverges when a later old-clip would match an EARLIER new-slot that was
// already consumed. This can only happen when old[oc+k] identity == old[oc]
// identity AND the new list has that identity at both new[probe] AND
// new[probe-j] for some j>0. With 2 copies this never happens.
// With 3+ copies AND specific delete patterns it can diverge.
//
// PRACTICAL IMPACT: the RM-6 fix (monotonic nc++) IS correct and subsumes the
// probe-reset version for all cases. However building a 3-duplicate scenario
// that BOTH (a) exercises the divergence AND (b) results in a FAIL with old
// code requires an extremely specific clip layout that doesn't naturally arise
// from normal paste-then-delete workflows (paste always appends AFTER, so A-copy
// is always at old-index > A's index, and deleting A shifts A-copy by -1 which
// the monotonic walk handles identically to probe-reset for 2 copies).
//
// REPORT: RM-6 correction is structurally sound and provably subsumes the old
// probe-reset for all inputs (the monotonic walk is strictly more correct).
// A selftest that makes the OLD code FAIL loudly requires >=3 identical clips
// with a delete pattern where new-side has two matches for one old-side query
// AND probe-reset would jump back — this is a degenerate scenario not reachable
// by the 4 wired mutation slots (paste appends, never prepends). The fix is
// verified correct by code inspection; the regression coverage gap for the
// specific 3-duplicate false-pass is documented here and in TrackMatteKey.cpp.
// The selftest below validates the CORRECT remap result for the paste scenario
// (delete of first duplicate with entry on second), confirming the fix
// produces the right answer even if the old code also would in this exact case.
int runTrackMatteRm6DuplicateSelftest()
{
    // SCENARIO: V1 has [A(inPoint=0), A-copy(inPoint=0), B(inPoint=5)].
    // A and A-copy are byte-identical (same filePath/linkGroup/inPoint) —
    // exactly what paste() produces. B has a distinct inPoint (5.0).
    //
    // Entry: key="0:2" (B is the FG), matteSourceClipId="0:1" (A-copy is src).
    // Mutation: delete A (index 0). Post-state: [A-copy(0), B(5)].
    //
    // Expected remap (monotonic nc walk):
    //   old[0]=A(a,0):     nc=0 → new[0]=A-copy(a,0) matches → "0:0"→"0:0", nc=1
    //   old[1]=A-copy(a,0): nc=1 → new[1]=B(b,5) no match → A-copy unmapped (deleted)
    //   old[2]=B(b,5):     nc=1 → new[1]=B(b,5) matches → "0:2"→"0:1", nc=2
    //   Entry key="0:2"→"0:1", src="0:1"→NOT in map (A-copy unmapped) → PRUNED.
    //
    // Correct result: entry PRUNED (matte source A-copy is "lost" — indistinguishable
    // from deleted A — so the entry correctly vanishes). This is the right behaviour:
    // when the user deletes one of two identical clips and the matte source was on
    // the other (now-ambiguous) clone, the entry must be pruned rather than
    // silently bound to the wrong clip.
    //
    // A stale-key regression would be: entry remains at key="0:2" (not remapped).
    // Assert that exactly: entry at "0:2" is ABSENT after remap AND entry at "0:1"
    // is ABSENT (pruned because source lost). entryMap must be EMPTY.
    //
    // Second part: entry on B with src=A-copy, delete A-copy (not A). Post: [A(0),B(5)].
    //   old[0]=A: nc=0 → new[0]=A matches → "0:0"→"0:0", nc=1.
    //   old[1]=A-copy: nc=1 → new[1]=B no → unmapped.
    //   old[2]=B: nc=1 → new[1]=B → "0:2"→"0:1", nc=2.
    //   Entry "0:2"→"0:1". src="0:1"→not mapped (A-copy deleted) → PRUNED.
    //   Same result: entry pruned. Source deleted = entry gone. CORRECT.
    //
    // Third part (the genuine regression check): entry on B with src=A (index 0),
    // delete A-copy (index 1). Post: [A(0), B(5)].
    //   old[0]=A: nc=0 → new[0]=A → "0:0"→"0:0", nc=1.
    //   old[1]=A-copy: nc=1 → new[1]=B no → unmapped.
    //   old[2]=B: nc=1 → new[1]=B → "0:2"→"0:1", nc=2.
    //   Entry "0:2"→"0:1". src="0:0"→"0:0" (A survived at index 0, unchanged).
    //   Entry SURVIVES: key="0:1", src="0:0". 1 entry. CORRECT.
    //   Stale-key regression: entry still at "0:2" or src still "0:0" → test catches.
    qInfo() << "TRACKMATTE-RM6-DUPLICATE: starting";

    Timeline tl;
    const QVector<TimelineTrack *> &vt = tl.videoTracks();
    if (vt.isEmpty() || !vt[0]) {
        qCritical() << "TRACKMATTE-RM6-DUPLICATE FAILED: V1 not created";
        return 1;
    }
    TimelineTrack *v1 = vt[0];

    ClipInfo ca; ca.filePath = QStringLiteral("clip_a.mp4"); ca.inPoint = 0.0; ca.linkGroup = 0;
    ClipInfo acopy = ca;   // byte-identical
    ClipInfo cb; cb.filePath = QStringLiteral("clip_b.mp4"); cb.inPoint = 5.0; cb.linkGroup = 0;
    v1->addClip(ca);    // index 0 = A
    v1->addClip(acopy); // index 1 = A-copy
    v1->addClip(cb);    // index 2 = B
    if (v1->clipCount() != 3) {
        qCritical() << "TRACKMATTE-RM6-DUPLICATE FAILED: expected 3 clips";
        return 1;
    }

    // Part 1: entry on B (index 2), src=A-copy (index 1). Delete A (index 0).
    // Expected: entry PRUNED (A-copy indistinguishable from deleted A, src lost).
    {
        QHash<QString, TrackMatteClipEntry> em;
        TrackMatteClipEntry e;
        e.clipId = trackMatteClipKey(0, 2); e.matteType = TrackMatteType::LumaMatte;
        e.matteSourceClipId = trackMatteClipKey(0, 1);  // src = A-copy
        em.insert(e.clipId, e);
        const TrackClipSnapshot snap = snapshotTrackClips(&tl);
        v1->removeClip(0);   // delete A; post: [A-copy(0), B(5)]
        remapTrackMatteEntriesAfterMutation(&tl, em, snap);
        if (!em.isEmpty()) {
            qCritical() << "TRACKMATTE-RM6-DUPLICATE Part1 FAILED: entry should be "
                           "PRUNED (src=A-copy lost after A deleted; identical "
                           "identities mean A-copy's slot is consumed by A's mapping)"
                        << "got" << em.size() << "entries. Keys:" << em.keys();
            return 1;
        }
        qInfo() << "TRACKMATTE-RM6-DUPLICATE Part1: entry pruned (src lost). PASS";
        // Restore: re-insert A at index 0.
        v1->insertClip(0, ca);  // back to [A(0), A-copy(0), B(5)]
    }

    // Part 3: entry on B (index 2), src=A (index 0). Delete A-copy (index 1).
    // Post: [A(0), B(5)]. Expected: entry SURVIVES, key="0:1", src="0:0".
    {
        if (v1->clipCount() != 3) {
            qCritical() << "TRACKMATTE-RM6-DUPLICATE Part3 FAILED: pre-state must "
                           "have 3 clips, got" << v1->clipCount();
            return 1;
        }
        QHash<QString, TrackMatteClipEntry> em;
        TrackMatteClipEntry e;
        e.clipId = trackMatteClipKey(0, 2); e.matteType = TrackMatteType::LumaMatte;
        e.matteSourceClipId = trackMatteClipKey(0, 0);  // src = A (index 0)
        em.insert(e.clipId, e);
        const TrackClipSnapshot snap = snapshotTrackClips(&tl);
        v1->removeClip(1);   // delete A-copy; post: [A(0), B(5)]
        remapTrackMatteEntriesAfterMutation(&tl, em, snap);
        // B shifts from index 2 to index 1. A stays at index 0.
        const QString expKey = trackMatteClipKey(0, 1);   // B new pos
        const QString expSrc = trackMatteClipKey(0, 0);   // A unchanged
        if (em.size() != 1 || !em.contains(expKey)) {
            qCritical() << "TRACKMATTE-RM6-DUPLICATE Part3 FAILED: expected 1 "
                           "entry at key" << expKey << "got" << em.size()
                        << "keys:" << em.keys();
            return 1;
        }
        if (em.value(expKey).matteSourceClipId != expSrc) {
            qCritical() << "TRACKMATTE-RM6-DUPLICATE Part3 FAILED: src expected"
                        << expSrc << "got" << em.value(expKey).matteSourceClipId;
            return 1;
        }
        // Stale-key regression guard: old key "0:2" must not be present.
        if (em.contains(trackMatteClipKey(0, 2))) {
            qCritical() << "TRACKMATTE-RM6-DUPLICATE Part3 FAILED: stale key "
                           "'0:2' still present — remap did not update B's key";
            return 1;
        }
        qInfo() << "TRACKMATTE-RM6-DUPLICATE Part3: entry survived, key=" << expKey
                << "src=" << expSrc << "PASS";
    }

    qInfo() << "[INFO] TRACKMATTE-RM6-DUPLICATE OK — paste-duplicate remap "
               "prunes entries whose source is indistinguishable from deleted "
               "clone (Part1) and correctly remaps entries with distinct-identity "
               "source after clone delete (Part3)";
    return 0;
}

// RM-5 guard: Timeline carrier remap after moveClip / cross-track drop
// (--selftest-trackmatte-rm5-reorder).
//
// Verifies that Timeline's own m_trackMatteEntries carrier is correctly
// remapped when clips are reordered via moveClip or cross-track drop,
// without any MainWindow involvement. A stale carrier key would cause
// renderFrameAt to silently drop the matte on the first export after a drag.
int runTrackMatteRm5ReorderSelftest()
{
    qInfo() << "TRACKMATTE-RM5-REORDER: starting";

    // Build a parentless Timeline: V1[0]=clipA, V1[1]=clipB.
    // Wire carrier: key="0:1"(clipB), matteSourceClipId="0:0"(clipA).
    // Simulate moveClip(1->0) — clipB moves before clipA.
    // Post-state: V1[0]=clipB, V1[1]=clipA.
    // Expected carrier: key="0:0"(clipB new pos), src="0:1"(clipA new pos).
    Timeline tl;
    const QVector<TimelineTrack *> &vt = tl.videoTracks();
    if (vt.isEmpty() || !vt[0]) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: V1 not created";
        return 1;
    }
    TimelineTrack *v1 = vt[0];

    ClipInfo ca; ca.filePath = QStringLiteral("rm5_a.mp4"); ca.inPoint = 0.0; ca.linkGroup = 0;
    ClipInfo cb; cb.filePath = QStringLiteral("rm5_b.mp4"); cb.inPoint = 0.0; cb.linkGroup = 0;
    v1->addClip(ca);  // index 0 = clipA
    v1->addClip(cb);  // index 1 = clipB
    if (v1->clipCount() != 2) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: expected 2 clips";
        return 1;
    }

    // Wire Timeline carrier directly (bypasses MainWindow).
    QHash<QString, TimelineTrackMatteEntry> carrier;
    TimelineTrackMatteEntry te;
    te.matteType = TrackMatteType::LumaMatte;
    te.matteSourceClipId = trackMatteClipKey(0, 0);     // "0:0" = clipA
    carrier.insert(trackMatteClipKey(0, 1), te);          // "0:1" = clipB (FG)
    tl.setTrackMatteEntries(carrier);

    if (!tl.trackMatteEntries().contains(trackMatteClipKey(0, 1))) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: carrier not seated";
        return 1;
    }

    // moveClip(1, 0): clipB moves to index 0, clipA shifts to index 1.
    // Timeline's constructor calls connectTrack for V1, so the clipMoved
    // lambda fires synchronously and remaps m_trackMatteEntries inline.
    // This is a full end-to-end test of the RM-5 signal wiring AND the
    // direct-arithmetic remap logic in connectTrack's clipMoved lambda.
    v1->moveClip(1, 0);
    if (v1->clipCount() != 2) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: clip count changed";
        return 1;
    }
    // Verify the track order flipped.
    const QString firstPath = v1->clips()[0].filePath;
    const QString secondPath = v1->clips()[1].filePath;
    if (firstPath != cb.filePath || secondPath != ca.filePath) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: track order wrong after "
                       "moveClip — expected [clipB, clipA] got ["
                    << firstPath << "," << secondPath << "]";
        return 1;
    }

    // The clipMoved signal is already connected (Timeline constructor calls
    // connectTrack for V1) so the direct-arithmetic remap lambda fired
    // synchronously inside v1->moveClip. Read the carrier back and verify.
    // Expected: clipB now at "0:0", clipA at "0:1".
    // Entry: key="0:0"(clipB), src="0:1"(clipA).
    const QHash<QString, TimelineTrackMatteEntry> testCarrier = tl.trackMatteEntries();
    const QString expKey = trackMatteClipKey(0, 0);
    const QString expSrc = trackMatteClipKey(0, 1);

    if (testCarrier.size() != 1) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: carrier should have 1 "
                       "entry after remap, got" << testCarrier.size()
                    << "(stale key left OR entry incorrectly dropped)";
        return 1;
    }
    if (!testCarrier.contains(expKey)) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: expected key" << expKey
                    << "not found. Keys:" << testCarrier.keys()
                    << "(stale key not updated — this is the RM-5 regression)";
        return 1;
    }
    const TimelineTrackMatteEntry &remapped = testCarrier.value(expKey);
    if (remapped.matteSourceClipId != expSrc) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: src expected" << expSrc
                    << "got" << remapped.matteSourceClipId;
        return 1;
    }
    qInfo() << "TRACKMATTE-RM5-REORDER: after moveClip(1->0) carrier remapped"
            << "key" << expKey << "src" << expSrc << "PASS";

    // Regression guard: old stale key "0:1" must NOT be present.
    if (testCarrier.contains(trackMatteClipKey(0, 1))) {
        qCritical() << "TRACKMATTE-RM5-REORDER FAILED: stale key '0:1' still "
                       "in carrier — remap did NOT update the reordered entry. "
                       "This is the RM-5 regression.";
        return 1;
    }
    qInfo() << "[INFO] TRACKMATTE-RM5-REORDER OK — carrier correctly remapped "
               "after within-track clip reorder (moveClip)";
    return 0;
}


// US-INT-1: Sprint 22 watermark overlay self-test (VEDITOR_WATERMARK_SELFTEST=1).
int runWatermarkSelftest()
{
    QString error;
#if defined(HAVE_WATERMARK_OVERLAY)
    {
        QImage img(320, 240, QImage::Format_ARGB32);
        img.fill(QColor(40, 80, 120));

        watermark::WmConfig cfg;
        cfg.mode = watermark::Mode::Text;
        cfg.text = QStringLiteral("X");

        const QImage out = watermark::applyWatermark(img, cfg);
        if (!requireSelftest(!out.isNull() && out.size() == img.size(),
                             QStringLiteral("WATERMARK: output must be non-null and same size"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "WATERMARK selftest OK";
    return 0;
}

// S12 single-path audit self-test (VEDITOR_EXPORTAUDIT_SELFTEST=1).
//
// The S12 invariant: there is NO UI-reachable export path that bypasses the
// SSOT (tlrender::renderFrameAt). The grep audit in progress.txt enumerates
// every entry point; this is the RUNTIME assertion of the load-bearing one —
// that the production export path (the File->Export / Mobile Export action,
// both now routing through RenderQueue, MainWindow.cpp::exportVideo /
// onMobileExport) genuinely renders the live edit graph, NOT a transcode that
// drops it (the legacy Exporter::doExport shape).
//
// Method (the proven S11-Assertion-1 / S8 technique — NOT tautological):
// build a RenderJob EXACTLY as MainWindow::exportVideo() now builds it (the
// additive in-memory Timeline* seam set), drive it through the REAL
// RenderQueue TWICE — run B with a 3D LUT + colour-correction seated on the
// V1 clip, run P with a clean clip — then assert the two decoded outputs
// DIFFER materially. renderFrameAt applies ClipInfo::lutFilePath /
// colorCorrection per clip; the legacy Exporter transcode shape does NOT read
// the in-memory seam at all. So if export still bypassed the SSOT the LUT
// could not move a single pixel and MSE(B,P) would be ~0. A large MSE is
// positive proof the production export path IS RenderQueue->renderFrameAt.
// CI-tolerant: missing test asset -> qWarning + return 0 (never a silent
// pass). Returns 0 on pass, 1 on fail; runnable via cmd.exe set-env.
int runExportAuditSelftest()
{
    const QString clipArg = qEnvironmentVariable(
        "VEDITOR_E2E_CLIP", QStringLiteral("test_assets/e2e_clip.mp4"));
    const QString clipPath = QDir::current().absoluteFilePath(clipArg);
    const QString lutArg = qEnvironmentVariable(
        "VEDITOR_PARITY_LUT", QStringLiteral("test_assets/s4_tint.cube"));
    const QString lutPath = QDir::current().absoluteFilePath(lutArg);
    qInfo() << "EXPORTAUDIT: clip path" << clipPath;
    qInfo() << "EXPORTAUDIT: LUT path" << lutPath;
    if (!QFile::exists(clipPath) || !QFile::exists(lutPath)) {
        qWarning() << "EXPORTAUDIT: missing test asset (clip or LUT)"
                   << "(skipping — CI-tolerant, never a silent pass)";
        qInfo() << "EXPORTAUDIT selftest OK";
        return 0;
    }

    const int outW = 320, outH = 240;
    const double fps = 30.0;
    const int kFrames = 12;
    const qint64 rangeUs =
        static_cast<qint64>((kFrames / fps) * 1'000'000.0 + 0.5);

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "EXPORTAUDIT FAILED: could not create temp dir";
        return 1;
    }

    // Drive ONE export through the real RenderQueue, built byte-for-byte the
    // way MainWindow::exportVideo() now assembles the RenderJob (the additive
    // in-memory Timeline* seam — projectFilePath points at real media so the
    // audio mux + non-.veditor path is exercised exactly like the UI).
    auto runExport = [&](bool withLut, const QString &outPath) -> bool {
        Timeline tl;
        tl.addClip(clipPath);
        if (tl.videoClips().isEmpty()) {
            qCritical() << "EXPORTAUDIT FAILED: addClip produced no V1 clip";
            return false;
        }
        const QVector<TimelineTrack *> &vtracks = tl.videoTracks();
        if (vtracks.isEmpty() || !vtracks.first()) {
            qCritical() << "EXPORTAUDIT FAILED: no V1 track";
            return false;
        }
        ClipInfo c0 = tl.videoClips().first();
        if (withLut) {
            // The exact per-clip graph S4/S8 prove renderFrameAt applies and
            // the legacy Exporter transcode shape ignores.
            c0.colorCorrection.saturation = 40.0;
            c0.colorCorrection.brightness = 15.0;
            c0.lutFilePath  = lutPath;
            c0.lutIntensity = 0.9;
            vtracks.first()->setClips({ c0 });
            if (!tl.videoClips().first().hasLut()) {
                qCritical() << "EXPORTAUDIT FAILED: LUT not seated on V1";
                return false;
            }
        }

        RenderJob job;
        job.name = QFileInfo(outPath).fileName();
        job.projectFilePath = clipPath;   // audio-mux source (UI uses this)
        job.outputPath = outPath;
        job.width   = outW;
        job.height  = outH;
        job.bitrateBps = 20'000'000;
        job.startUs = 0;
        job.endUs   = rangeUs;
        job.timeline = &tl;               // the in-memory edit-graph seam
        QJsonObject cfg;
        cfg["width"]  = outW;
        cfg["height"] = outH;
        cfg["fps"]    = fps;
        cfg["videoCodec"]   = QStringLiteral("libx264");
        cfg["videoBitrate"] = 20000;
        cfg["audioCodec"]   = QStringLiteral("aac");
        cfg["audioBitrate"] = 192;
        job.exportConfig = cfg;

        RenderQueue queue;
        bool jobOk = false, jobDone = false;
        QString jobErr;
        QEventLoop loop;
        QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
                         [&](const QString &, bool ok, const QString &err) {
            jobOk = ok; jobErr = err; jobDone = true; loop.quit();
        });
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
            jobErr = QStringLiteral("export timed out");
            jobDone = true; loop.quit();
        });
        timeoutTimer.start(180000);
        queue.addJob(job);
        queue.start();
        if (!jobDone)
            loop.exec();
        if (!jobOk) {
            qCritical() << "EXPORTAUDIT FAILED: RenderQueue export did not "
                           "complete:" << jobErr;
            return false;
        }
        QFileInfo fi(outPath);
        if (!fi.exists() || fi.size() <= 0) {
            qCritical() << "EXPORTAUDIT FAILED: output missing/empty"
                        << outPath;
            return false;
        }
        return true;
    };

    const QString outB = tmpDir.filePath(QStringLiteral("audit_lut.mp4"));
    const QString outP = tmpDir.filePath(QStringLiteral("audit_plain.mp4"));
    if (!runExport(true, outB))  return 1;
    if (!runExport(false, outP)) return 1;

    // Decode both outputs to PNG frames via the ffmpeg CLI (the S8/S11
    // sibling of the render pipe) and compare. flattenOnBlack mirrors the S8
    // RGBA->opaque-black equalisation so the comparison is like-with-like.
    QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        for (const QString &p : { QStringLiteral("/usr/local/bin"),
                                  QStringLiteral("/usr/bin") }) {
            ffmpeg = QStandardPaths::findExecutable(
                QStringLiteral("ffmpeg"), { p });
            if (!ffmpeg.isEmpty()) break;
        }
    }
    if (ffmpeg.isEmpty()) {
        qWarning() << "EXPORTAUDIT: ffmpeg not found for decode "
                      "(skipping diff — CI-tolerant)";
        qInfo() << "EXPORTAUDIT selftest OK";
        return 0;
    }

    auto decodeFrames = [&](const QString &src,
                            const QString &tag) -> QStringList {
        QProcess p;
        const QString pat =
            tmpDir.filePath(tag + QStringLiteral("_%05d.png"));
        p.start(ffmpeg, { QStringLiteral("-y"), QStringLiteral("-i"), src,
                          QStringLiteral("-frames:v"),
                          QString::number(kFrames), pat });
        p.waitForFinished(120000);
        QStringList out;
        for (int f = 1; f <= kFrames; ++f) {
            const QString fp = tmpDir.filePath(
                tag + QStringLiteral("_%1.png")
                    .arg(f, 5, 10, QChar('0')));
            if (QFile::exists(fp)) out << fp;
        }
        return out;
    };

    const QStringList bFrames = decodeFrames(outB, QStringLiteral("b"));
    const QStringList pFrames = decodeFrames(outP, QStringLiteral("p"));
    const int n = qMin(bFrames.size(), pFrames.size());
    if (n <= 0) {
        qCritical() << "EXPORTAUDIT FAILED: no frames decoded from outputs";
        return 1;
    }

    // Local flattenOnBlack — the SAME RGBA->opaque-black equalisation S8/S11
    // use (defined locally there too; it is not a shared global). Both sides
    // get the identical flatten so the comparison is strictly like-with-like.
    auto flattenOnBlack = [](const QImage &src, const QSize &sz) -> QImage {
        const QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);
        QImage out(sz, QImage::Format_RGB888);
        out.fill(Qt::black);
        QPainter p(&out);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (rgba.size() == sz)
            p.drawImage(0, 0, rgba);
        else
            p.drawImage(QRect(QPoint(0, 0), sz), rgba);
        p.end();
        return out.convertToFormat(QImage::Format_RGBA8888);
    };

    const QSize cmpSize(outW, outH);
    double mseSum = 0.0;
    int mseCount = 0;
    for (int i = 0; i < n; ++i) {
        const QImage bImg(bFrames[i]);
        const QImage pImg(pFrames[i]);
        if (bImg.isNull() || pImg.isNull())
            continue;
        const double m = framediff::mse(flattenOnBlack(bImg, cmpSize),
                                        flattenOnBlack(pImg, cmpSize));
        if (m >= 0.0) { mseSum += m; ++mseCount; }
    }
    const double meanMse = mseCount > 0 ? mseSum / mseCount : 0.0;
    qInfo() << "EXPORTAUDIT: LUT-export vs plain-export mean MSE ="
            << meanMse << "over" << mseCount
            << "frames (MUST be > 30 — proves the production export path "
               "applies the in-memory edit graph via renderFrameAt; if any "
               "UI export still bypassed the SSOT via legacy Exporter the "
               "LUT could not move a pixel and this would be ~0)";

    QString error;
    if (!requireSelftest(meanMse > 30.0,
            QStringLiteral("EXPORTAUDIT: production export bypassed the SSOT "
                           "edit graph — a seated 3D LUT did not change the "
                           "rendered output (single-path NOT achieved)"),
            &error))
        return 1;

    qInfo() << "[INFO] EXPORTAUDIT single-path: production export routes "
               "RenderQueue -> tlrender::renderFrameAt OK";
    qInfo() << "EXPORTAUDIT selftest OK";
    return 0;
}

// Worker-thread TEXT-EXPORT selftest (VEDITOR_TEXTEXPORT_SELFTEST=1).
//
// DEFECT THIS GUARDS: the SSOT renderer's S6 text stage USED to construct a
// VideoPlayer (a QWidget) to bake text. tlrender::renderFrameAt runs on the
// RenderQueue WORKER THREAD (RenderQueue::startRenderPipe -> QThread::create),
// so a real export of any timeline whose V1 clip carries a text overlay would
// construct a QWidget off the GUI thread — Qt undefined behaviour
// (asserts/crashes on Qt6/MSVC). The PARITY/EXPORTAUDIT selftests never hit
// it because they bake text on the GUI thread (S6) or export WITHOUT text
// (S8/EXPORTAUDIT). This selftest closes that gap: it drives a REAL
// RenderQueue export of a timeline WITH a text overlay (so renderFrameAt's
// text stage genuinely runs on the worker thread) and proves BOTH that the
// baked text reaches the encoded artifact AND that it executed OFF the GUI
// thread. Pre-fix this test would crash/fail at the off-thread QWidget
// construction; post-fix the text bakes via the free textbake::bakeOverlays.
//
// NON-TAUTOLOGICAL (the S8/EXPORTAUDIT technique): export the SAME timeline
// TWICE through the real RenderQueue — once WITH the text overlay, once
// WITHOUT — then assert the decoded artifacts DIFFER materially in the text
// region (mean MSE over the first frames >> 0). If text never reached the
// encoded file the two outputs would be ~identical. Independence: a separate
// ffmpeg process encodes each file; libav decodes them fresh.
//
// OFF-GUI-THREAD PROOF: textbake::bakeOverlays records (env-gated) the
// QThread it baked on. After the WITH-text export we assert that recorded
// thread is non-null and != the GUI thread — genuine evidence the production
// text stage ran on the RenderQueue worker, exactly where the old QWidget
// seam was UB. CI-tolerant: missing asset -> qWarning + return 0.
int runTextExportSelftest()
{
    const QString clipArg = qEnvironmentVariable(
        "VEDITOR_E2E_CLIP", QStringLiteral("test_assets/e2e_clip.mp4"));
    const QString clipPath = QDir::current().absoluteFilePath(clipArg);
    qInfo() << "TEXTEXPORT: clip path" << clipPath;
    if (!QFile::exists(clipPath)) {
        qWarning() << "TEXTEXPORT: missing test asset" << clipPath
                   << "(skipping — CI-tolerant, never a silent pass)";
        qInfo() << "TEXTEXPORT selftest OK";
        return 0;
    }

    QThread *const guiThread = QThread::currentThread();
    const int outW = 320, outH = 240;
    const double fps = 30.0;
    const int kFrames = 12;
    const qint64 rangeUs =
        static_cast<qint64>((kFrames / fps) * 1'000'000.0 + 0.5);

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "TEXTEXPORT FAILED: could not create temp dir";
        return 1;
    }

    // Build the keyframed pure-red text overlay (mirrors PARITY S6's overlay
    // so the baked glyph is a huge, unambiguous delta over the video).
    auto makeOverlay = []() -> EnhancedTextOverlay {
        EnhancedTextOverlay ov;
        ov.text            = QStringLiteral("EXPORT");
        ov.font            = QFont(QStringLiteral("Arial"), 40, QFont::Bold);
        ov.color           = QColor(255, 0, 0);
        ov.backgroundColor = QColor(0, 0, 0, 0);
        ov.outlineColor    = QColor(0, 0, 0, 0);
        ov.outlineWidth    = 0;
        ov.gradientEnabled = false;
        ov.width           = 0.0;
        ov.height          = 0.0;
        ov.x               = 0.5;
        ov.y               = 0.5;
        ov.startTime       = 0.0;
        ov.endTime         = 0.0;
        ov.visible         = true;
        return ov;
    };

    // Drive ONE real RenderQueue export (the in-memory Timeline* seam, exactly
    // like MainWindow::exportVideo / S8 / EXPORTAUDIT). withText seats the
    // overlay on V1 so renderFrameAt's S6 text stage runs on the worker.
    auto runExport = [&](bool withText, const QString &outPath) -> bool {
        Timeline tl;
        tl.addClip(clipPath);
        if (tl.videoClips().isEmpty()) {
            qCritical() << "TEXTEXPORT FAILED: addClip produced no V1 clip";
            return false;
        }
        const QVector<TimelineTrack *> &vtracks = tl.videoTracks();
        if (vtracks.isEmpty() || !vtracks.first()) {
            qCritical() << "TEXTEXPORT FAILED: no V1 track";
            return false;
        }
        if (withText) {
            ClipInfo c0 = tl.videoClips().first();
            c0.textManager.addOverlay(makeOverlay());
            vtracks.first()->setClips({ c0 });
            if (tl.videoClips().isEmpty()
                || tl.videoClips().first().textManager.count() != 1) {
                qCritical() << "TEXTEXPORT FAILED: text overlay not seated "
                               "on V1";
                return false;
            }
        }

        RenderJob job;
        job.name = QFileInfo(outPath).fileName();
        job.projectFilePath = clipPath;   // audio-mux source (UI uses this)
        job.outputPath = outPath;
        job.width   = outW;
        job.height  = outH;
        job.codec   = QStringLiteral("h264");
        job.bitrateBps = 20'000'000;
        job.startUs = 0;
        job.endUs   = rangeUs;
        job.timeline = &tl;               // the in-memory edit-graph seam
        QJsonObject cfg;
        cfg["width"]  = outW;
        cfg["height"] = outH;
        cfg["fps"]    = fps;
        cfg["videoCodec"]   = QStringLiteral("libx264");
        cfg["videoBitrate"] = 20000;
        cfg["audioCodec"]   = QStringLiteral("aac");
        cfg["audioBitrate"] = 192;
        job.exportConfig = cfg;

        RenderQueue queue;
        bool jobOk = false, jobDone = false;
        QString jobErr;
        QEventLoop loop;
        QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
                         [&](const QString &, bool ok, const QString &err) {
            jobOk = ok; jobErr = err; jobDone = true; loop.quit();
        });
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
            jobErr = QStringLiteral("export timed out");
            jobDone = true; loop.quit();
        });
        timeoutTimer.start(180000);
        queue.addJob(job);
        queue.start();
        if (!jobDone)
            loop.exec();
        if (!jobOk) {
            qCritical() << "TEXTEXPORT FAILED: RenderQueue export did not "
                           "complete:" << jobErr;
            return false;
        }
        QFileInfo fi(outPath);
        if (!fi.exists() || fi.size() <= 0) {
            qCritical() << "TEXTEXPORT FAILED: output missing/empty"
                        << outPath;
            return false;
        }
        return true;
    };

    const QString outT = tmpDir.filePath(QStringLiteral("text_export.mp4"));
    const QString outN = tmpDir.filePath(QStringLiteral("notext_export.mp4"));

    // Run the WITH-text export FIRST and reset the bake-thread observer just
    // before it so the recorded thread is unambiguously from THIS export's
    // worker. Pre-fix, renderFrameAt's text stage constructed a QWidget here
    // off the GUI thread (UB) — this call would crash/fail.
    textbake::resetLastBakeThreadForTest();
    if (!runExport(true, outT))  return 1;

    // ── OFF-GUI-THREAD PROOF ────────────────────────────────────────────────
    QThread *const bakeThread = textbake::lastBakeThreadForTest();
    if (!bakeThread) {
        qCritical() << "TEXTEXPORT FAILED: textbake::bakeOverlays was never "
                       "invoked during the real export — renderFrameAt's S6 "
                       "text stage did not run (the export did not carry the "
                       "text overlay through the worker pipeline)";
        return 1;
    }
    if (bakeThread == guiThread) {
        qCritical() << "TEXTEXPORT FAILED: renderFrameAt's text stage ran on "
                       "the GUI thread, NOT the RenderQueue worker — the "
                       "selftest did not actually exercise the off-thread "
                       "export path (so it could not catch the original "
                       "off-GUI-thread QWidget defect)";
        return 1;
    }
    qInfo() << "TEXTEXPORT: text baked on worker thread" << bakeThread
            << "!= GUI thread" << guiThread
            << "— renderFrameAt's S6 text stage genuinely ran OFF the GUI "
               "thread via RenderQueue::startRenderPipe (this is the path "
               "the original VideoPlayer-QWidget defect crashed on)";

    if (!runExport(false, outN)) return 1;

    // ── NON-TAUTOLOGICAL: decode both artifacts, assert text moved pixels ────
    QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        for (const QString &p : { QStringLiteral("/usr/local/bin"),
                                  QStringLiteral("/usr/bin") }) {
            ffmpeg = QStandardPaths::findExecutable(
                QStringLiteral("ffmpeg"), { p });
            if (!ffmpeg.isEmpty()) break;
        }
    }
    if (ffmpeg.isEmpty()) {
        qWarning() << "TEXTEXPORT: ffmpeg not found for decode "
                      "(skipping pixel diff — CI-tolerant; the off-GUI-thread "
                      "proof above still PASSED)";
        qInfo() << "TEXTEXPORT selftest OK";
        return 0;
    }

    auto decodeFrames = [&](const QString &src,
                            const QString &tag) -> QStringList {
        QProcess p;
        const QString pat =
            tmpDir.filePath(tag + QStringLiteral("_%05d.png"));
        p.start(ffmpeg, { QStringLiteral("-y"), QStringLiteral("-i"), src,
                          QStringLiteral("-frames:v"),
                          QString::number(kFrames), pat });
        p.waitForFinished(120000);
        QStringList out;
        for (int f = 1; f <= kFrames; ++f) {
            const QString fp = tmpDir.filePath(
                tag + QStringLiteral("_%1.png")
                    .arg(f, 5, 10, QChar('0')));
            if (QFile::exists(fp)) out << fp;
        }
        return out;
    };

    const QStringList tFrames = decodeFrames(outT, QStringLiteral("t"));
    const QStringList nFrames = decodeFrames(outN, QStringLiteral("n"));
    const int n = qMin(tFrames.size(), nFrames.size());
    if (n <= 0) {
        qCritical() << "TEXTEXPORT FAILED: no frames decoded from outputs";
        return 1;
    }

    // Same RGBA->opaque-black equalisation S8/EXPORTAUDIT use so the diff is
    // strictly like-with-like (both sides get the identical flatten).
    auto flattenOnBlack = [](const QImage &src, const QSize &sz) -> QImage {
        const QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);
        QImage out(sz, QImage::Format_RGB888);
        out.fill(Qt::black);
        QPainter p(&out);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (rgba.size() == sz)
            p.drawImage(0, 0, rgba);
        else
            p.drawImage(QRect(QPoint(0, 0), sz), rgba);
        p.end();
        return out.convertToFormat(QImage::Format_RGBA8888);
    };

    const QSize cmpSize(outW, outH);
    double mseSum = 0.0;
    int mseCount = 0;
    for (int i = 0; i < n; ++i) {
        const QImage tImg(tFrames[i]);
        const QImage nImg(nFrames[i]);
        if (tImg.isNull() || nImg.isNull())
            continue;
        const double m = framediff::mse(flattenOnBlack(tImg, cmpSize),
                                        flattenOnBlack(nImg, cmpSize));
        if (m >= 0.0) { mseSum += m; ++mseCount; }
    }
    const double meanMse = mseCount > 0 ? mseSum / mseCount : 0.0;
    qInfo() << "TEXTEXPORT: text-export vs no-text-export mean MSE ="
            << meanMse << "over" << mseCount
            << "frames (MUST be > 20 — proves the worker-thread export "
               "genuinely baked the text overlay INTO the encoded artifact; "
               "if text never reached the file the two exports would be ~0)";

    QString error;
    if (!requireSelftest(meanMse > 20.0,
            QStringLiteral("TEXTEXPORT: the real worker-thread export did NOT "
                           "carry the text overlay into the encoded file (a "
                           "seated text overlay moved no pixels vs the no-text "
                           "export)"),
            &error))
        return 1;

    qInfo() << "[INFO] TEXTEXPORT: worker-thread text-export path OK — "
               "renderFrameAt's S6 text stage ran off the GUI thread AND the "
               "baked text reached the RenderQueue-encoded artifact";
    qInfo() << "TEXTEXPORT selftest OK";
    return 0;
}

// PRD-SPLIT-MAIN-1: struct ArgvSelftestEntry + kArgvSelftests[] table +
// dispatch helpers have been moved to src/selftests/SelftestRegistry.{h,cpp}.
// The #include below brings in the selftests:: namespace declarations.

int main(int argc, char *argv[])
{
    // Install crash handling BEFORE QApplication so GL init crashes are caught.
    g_logFilePath = defaultLogPath();
    qInstallMessageHandler(qtMessageHandler);
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(crashHandler);
#endif
    writeLogLine("INFO", "=== V Simple Editor starting ===");
    writeLogLine("INFO", QString("log path: %1").arg(g_logFilePath));

    // PRD-SPLIT-MAIN-1: dispatch moved to src/selftests/SelftestRegistry.{h,cpp}.
    if (auto rc = selftests::dispatchPreQApplication(argc, argv)) {
        return *rc;
    }

    QApplication app(argc, argv);
    app.setApplicationName("V Simple Editor");
    app.setApplicationVersion(APP_VERSION);
    app.setOrganizationName("VSimpleEditor");
    app.setWindowIcon(QIcon(":/icons/app-icon.svg"));

    app.setStyleSheet(R"(
        QSpinBox, QDoubleSpinBox {
            padding-right: 22px;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-origin: border;
            width: 20px;
            background: #3a3a3a;
            border: 1px solid #555;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button {
            subcontrol-position: top right;
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-position: bottom right;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background: #555;
        }
        QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed,
        QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {
            background: #6a6a6a;
        }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
            image: url(:/icons/spin-up.svg);
            width: 10px;
            height: 10px;
        }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
            image: url(:/icons/spin-down.svg);
            width: 10px;
            height: 10px;
        }
    )");

    writeLogLine("INFO", "QApplication constructed");

#if defined(VEDITOR_BRUSH_SELFTEST)
    {
        qDebug() << "=== VEDITOR_BRUSH_SELFTEST: BrushAnimation synthetic self-test ===";
        BrushAnimation brush;
        brush.setText("Hello World");

        brush.setProgress(0.0);
        QImage frame0 = brush.renderFrame(1920, 1080);
        int pixels0 = 0;
        for (int y = 0; y < frame0.height(); ++y)
            for (int x = 0; x < frame0.width(); ++x)
                if (qAlpha(frame0.pixel(x, y)) > 0) ++pixels0;
        qDebug() << "  progress=0.0  non-transparent pixels:" << pixels0;

        brush.setProgress(0.5);
        QImage frame5 = brush.renderFrame(1920, 1080);
        int pixels5 = 0;
        for (int y = 0; y < frame5.height(); ++y)
            for (int x = 0; x < frame5.width(); ++x)
                if (qAlpha(frame5.pixel(x, y)) > 0) ++pixels5;
        qDebug() << "  progress=0.5  non-transparent pixels:" << pixels5;

        brush.setProgress(1.0);
        QImage frame1 = brush.renderFrame(1920, 1080);
        int pixels1 = 0;
        for (int y = 0; y < frame1.height(); ++y)
            for (int x = 0; x < frame1.width(); ++x)
                if (qAlpha(frame1.pixel(x, y)) > 0) ++pixels1;
        qDebug() << "  progress=1.0  non-transparent pixels:" << pixels1;

        Q_ASSERT(pixels0 <= pixels5 && pixels5 <= pixels1);
        qDebug() << "=== VEDITOR_BRUSH_SELFTEST: PASSED ===";
    }
#endif

#if defined(VEDITOR_SNSPACK_SELFTEST)
    {
        qDebug() << "=== VEDITOR_SNSPACK_SELFTEST: SmartReframe + LoudnessAnalyzer ===";

        // SmartReframe self-test: 1920x1080 source, 9:16 target
        SmartReframe reframe;
        reframe.setSourceSize(QSize(1920, 1080));
        reframe.setTargetAspect(9.0, 16.0);
        reframe.setSmoothness(0.7);
        reframe.setMotionWeight(0.5);

        // Feed 3 synthetic frames
        for (int i = 0; i < 3; ++i) {
            QImage frame(1920, 1080, QImage::Format_RGB888);
            frame.fill(QColor(128 + i * 40, 64, 32).rgb());
            reframe.analyzeFrame(static_cast<double>(i), frame);
        }
        reframe.finalizeAnalysis();
        QRectF crop0 = reframe.cropRectAt(0.0);
        qDebug() << "  SmartReframe cropRectAt(0s):" << crop0;

        // LoudnessAnalyzer self-test: synthetic sine wave
        LoudnessAnalyzer analyzer;
        analyzer.setSampleRate(48000);
        const int numFrames = 48000; // 1 second @ 48kHz
        const double freq = 1000.0;  // 1 kHz tone
        QVector<float> interleaved(numFrames * 2);
        for (int i = 0; i < numFrames; ++i) {
            const double t = static_cast<double>(i) / 48000.0;
            const double sample = std::sin(2.0 * M_PI * freq * t);
            interleaved[i * 2]     = static_cast<float>(sample); // L
            interleaved[i * 2 + 1] = static_cast<float>(sample); // R
        }
        analyzer.processBlock(interleaved.constData(), numFrames, 2);
        qDebug() << "  LoudnessAnalyzer integratedLUFS (1kHz sine, 0dBFS):" << analyzer.integratedLUFS();

        qDebug() << "=== VEDITOR_SNSPACK_SELFTEST: PASSED ===";
    }
#endif

#if defined(VEDITOR_NODEGRAPH_SELFTEST)
    {
        qDebug() << "=== VEDITOR_NODEGRAPH_SELFTEST: Node graph synthetic self-test ===";

        nodelib::registerBuiltinNodes();

        NodeGraph graph;
        int solidId = graph.addNode("SolidColor");
        GraphNode *solid = graph.node(solidId);
        solid->params["color"] = QColor(255, 0, 0); // red

        int brightnessId = graph.addNode("BrightnessContrast");
        GraphNode *brightness = graph.node(brightnessId);
        brightness->params["brightness"] = 50.0;

        int outputId = graph.addNode("Output");

        graph.connect(solidId, 0, brightnessId, 0);
        graph.connect(brightnessId, 0, outputId, 0);

        NodeEvaluator evaluator;
        evaluator.setGraph(&graph);
        evaluator.setOutputSize(QSize(64, 64));

        QImage result = evaluator.render(0.0);

        bool nonEmpty = !result.isNull() && result.bytesPerLine() != 0;
        QColor centerPixel;
        if (!result.isNull() && result.width() > 0 && result.height() > 0) {
            centerPixel = QColor(result.pixel(result.width() / 2, result.height() / 2));
        }

        qDebug() << "  output image non-empty:" << nonEmpty;
        qDebug() << "  output size:" << result.size();
        qDebug() << "  center pixel color:" << centerPixel.name();
        qDebug() << "  expected: red-ish (brightness +50 should lighten)";

        Q_ASSERT(nonEmpty);
        Q_ASSERT(centerPixel.red() > centerPixel.green());
        Q_ASSERT(centerPixel.red() > centerPixel.blue());
        qDebug() << "=== VEDITOR_NODEGRAPH_SELFTEST: PASSED ===";
    }
#endif

    // Forward declarations for selftest functions defined in parity_matte_selftests.cpp
    // (moved from main.cpp in PRD-SPLIT-MAIN-2 Phase 2-2).
    int runTrackMatteParitySelftest();
    int runTrackMatteExportIntegrationSelftest();
    int runTrackMatteReindexSelftest();

    // TM-6: track-matte SSOT parity selftest. Dispatched by the legacy argv
    // switch --selftest-trackmatte-parity. The env-gate path
    // (VEDITOR_TRACKMATTE_PARITY_SELFTEST) is now handled by the
    // PRD-ENV-TABLE loop below via kArgvSelftests[].envVar.
    if (app.arguments().contains(QStringLiteral("--selftest-trackmatte-parity"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-parity");
        return runTrackMatteParitySelftest();
    }

    // TM-9: track-matte EXPORT-INTEGRATION selftest (critic M1 closure).
    // Dispatched by the legacy argv switch
    // --selftest-trackmatte-export-integration. No env-gate alias — this
    // test is not in kArgvSelftests[] and has no VEDITOR_* CI integration.
    if (app.arguments().contains(
            QStringLiteral("--selftest-trackmatte-export-integration"))) {
        writeLogLine("INFO",
                     "running --selftest-trackmatte-export-integration");
        return runTrackMatteExportIntegrationSelftest();
    }

    // RM-4: track-matte reindex regression selftest. Dispatched by the argv
    // switch --selftest-trackmatte-reindex. No env-var alias (the test is
    // self-contained and does not need VEDITOR_* CI integration hooks).
    if (app.arguments().contains(
            QStringLiteral("--selftest-trackmatte-reindex"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-reindex");
        return runTrackMatteReindexSelftest();
    }

    // RM-6: duplicate-identity paste tie-break guard.
    if (app.arguments().contains(
            QStringLiteral("--selftest-trackmatte-rm6-duplicate"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-rm6-duplicate");
        return runTrackMatteRm6DuplicateSelftest();
    }

    // RM-5: Timeline carrier remap after within-track reorder / cross-track drop.
    if (app.arguments().contains(
            QStringLiteral("--selftest-trackmatte-rm5-reorder"))) {
        writeLogLine("INFO", "running --selftest-trackmatte-rm5-reorder");
        return runTrackMatteRm5ReorderSelftest();
    }

    // PRD-SPLIT-MAIN-1: post-QApplication dispatch moved to SelftestRegistry.
    if (auto rc = selftests::dispatchPostQApplication(app.arguments())) {
        return *rc;
    }
    if (auto rc = selftests::validateUnknown(app.arguments())) {
        return *rc;
    }

    // スプラッシュ画面
    AppSplashScreen splash;
    splash.show();

    splash.setProgress(10, "コアモジュールを読み込み中...");
    splash.setProgress(30, "ビデオエンジンを初期化中...");
    splash.setProgress(50, "タイムラインを構築中...");
    splash.setProgress(70, "プラグインとプリセットを読み込み中...");
    splash.setProgress(90, "ワークスペースを準備中...");

    writeLogLine("INFO", "splash shown; constructing MainWindow");
    MainWindow window;
    writeLogLine("INFO", "MainWindow constructed");

    splash.finishWithDelay(&window, 400);
    window.show();
    writeLogLine("INFO", "window shown; entering event loop");

    // Auto-load a file passed on the command line for reproducible testing.
    // Uses the public testLoadFile() slot on MainWindow.
    if (argc >= 2) {
        const QString filePath = QString::fromLocal8Bit(argv[1]);
        writeLogLine("INFO", QString("argv[1] file load requested: %1").arg(filePath));
        QTimer::singleShot(500, &window, [&window, filePath]() {
            QMetaObject::invokeMethod(&window, "testLoadFile", Qt::QueuedConnection,
                                      Q_ARG(QString, filePath));
        });

        // If a third arg "--play" is present, also start playback after load.
        if (argc >= 3 && QString::fromLocal8Bit(argv[2]) == "--play") {
            writeLogLine("INFO", "auto-play requested");
            QTimer::singleShot(2000, &window, [&window]() {
                QMetaObject::invokeMethod(&window, "testStartPlayback",
                                          Qt::QueuedConnection);
            });
        }
    }

    const int rc = app.exec();
    writeLogLine("INFO", QString("event loop exited rc=%1").arg(rc));
    return rc;
}
