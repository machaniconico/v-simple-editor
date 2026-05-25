#include <QApplication>
#include <QIcon>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
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

#include "MainWindow.h"
#include "util/CrashHandler.h"
#include "util/Logger.h"
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
    installCrashHandling();
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
    void runBrushInlineSelftest();
    runBrushInlineSelftest();
#endif

#if defined(VEDITOR_SNSPACK_SELFTEST)
    void runSnspackInlineSelftest();
    runSnspackInlineSelftest();
#endif

#if defined(VEDITOR_NODEGRAPH_SELFTEST)
    void runNodeGraphInlineSelftest();
    runNodeGraphInlineSelftest();
#endif

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
