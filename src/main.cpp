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

// PRD-SPLIT-MAIN-3 Phase 3-E: runTrackMatteRm6DuplicateSelftest and
// runTrackMatteRm5ReorderSelftest have been moved verbatim to
// src/selftests/parity_matte_selftests.cpp.
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
    // (moved from main.cpp in PRD-SPLIT-MAIN-2 Phase 2-2 and PRD-SPLIT-MAIN-3 Phase 3-E).
    int runTrackMatteParitySelftest();
    int runTrackMatteExportIntegrationSelftest();
    int runTrackMatteReindexSelftest();
    int runTrackMatteRm6DuplicateSelftest();
    int runTrackMatteRm5ReorderSelftest();

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
