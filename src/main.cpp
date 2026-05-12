#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QStandardPaths>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>
#include <QTimer>
#include <QMetaObject>
#include <QTemporaryDir>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "MainWindow.h"
#include "FractalNoise.h"
#include "ParticleSystem.h"
#include "ProjectFile.h"
#include "SplashScreen.h"

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

// ──────────────────────────────────────────────────────────────────────────
// Lightweight file-backed logger + unhandled-exception reporter.
//
// Goal: when the app crashes, we want a log file we can read to understand
// what was happening just before the crash. Everything here is best-effort —
// it should never throw or hold locks that could deadlock on shutdown.
// ──────────────────────────────────────────────────────────────────────────

namespace {

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

bool requireSelftest(bool condition, const QString &message, QString *error)
{
    if (condition)
        return true;
    if (error)
        *error = message;
    qCritical() << "VFX selftest failed:" << message;
    return false;
}

int runVfxSelftest()
{
    QString error;

    ParticleEmitterConfig particleCfg;
    particleCfg.emitRate = 120.0;
    particleCfg.maxParticles = 256;
    particleCfg.lifeMin = 0.8;
    particleCfg.lifeMax = 1.5;
    particleCfg.gravity = QPointF(0.0, 300.0);
    particleCfg.collisionFloor = true;
    particleCfg.floorY = 0.8;
    particleCfg.restitution = 0.35;
    particleCfg.turbulenceAmount = 40.0;
    ForceField field;
    field.kind = ForceField::PointRepel;
    field.position = QPointF(0.5, 0.35);
    field.strength = 240.0;
    field.radius = 0.3;
    particleCfg.forceFields.append(field);

    ParticleSystem particleSystem;
    particleSystem.setConfig(particleCfg);
    particleSystem.update(0.5);
    particleSystem.update(0.5);
    const QImage particleFrame = particleSystem.renderFrame(QSize(320, 180), 0.0);
    if (!requireSelftest(particleSystem.particleCount() > 0, QStringLiteral("particle system emitted no particles"), &error)
        || !requireSelftest(!particleFrame.isNull(), QStringLiteral("particle system render failed"), &error)) {
        return 1;
    }

    for (FractalNoise::FractalKind kind : {FractalNoise::FractalKind::FBm,
                                          FractalNoise::FractalKind::Turbulence,
                                          FractalNoise::FractalKind::Ridged}) {
        FractalNoise::Params params;
        params.kind = kind;
        params.octaves = 4;
        params.frequency = 3.25;
        const QImage fractal = FractalNoise::render(QSize(96, 64), 0.42, params, true);
        if (!requireSelftest(!fractal.isNull(), QStringLiteral("fractal noise render failed"), &error))
            return 1;
        if (!requireSelftest(fractal.size() == QSize(96, 64), QStringLiteral("fractal noise size mismatch"), &error))
            return 1;
    }

    QImage source(96, 64, QImage::Format_RGB888);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor((x * 7) % 255, (y * 11) % 255, ((x + y) * 5) % 255));
        }
    }
    const VideoEffect displacement = VideoEffect::createDisplacementMap(18.0, 7.0, 0);
    const QImage displaced = VideoEffectProcessor::applyEffect(source, displacement);
    if (!requireSelftest(!displaced.isNull(), QStringLiteral("displacement map returned null image"), &error)
        || !requireSelftest(displaced.size() == source.size(), QStringLiteral("displacement map size mismatch"), &error)
        || !requireSelftest(displacement.param1 == 18.0 && displacement.param2 == 7.0 && displacement.param3 == 0.0,
                            QStringLiteral("displacement effect params mutated unexpectedly"), &error)) {
        return 1;
    }

    ProjectData saveData;
    saveData.config.name = QStringLiteral("VFX Selftest");
    saveData.config.width = 640;
    saveData.config.height = 360;
    saveData.config.fps = 30;
    ClipInfo clip;
    clip.filePath = QStringLiteral("synthetic-particle.mp4");
    clip.displayName = QStringLiteral("synthetic-particle.mp4");
    clip.duration = 2.0;
    clip.effects.append(displacement);
    saveData.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    saveData.audioTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}};
    ParticleClipEntry particleEntry;
    particleEntry.trackIndex = 0;
    particleEntry.clipIndex = 0;
    particleEntry.clipFilePath = clip.filePath;
    particleEntry.config = particleCfg;
    saveData.particleClipEntries.append(particleEntry);
    saveData.vfxState.glow.enabled = true;
    saveData.vfxState.glow.threshold = 0.65f;
    saveData.vfxState.glow.radius = 11.0f;
    saveData.vfxState.glow.intensity = 1.25f;
    saveData.vfxState.bloom.enabled = true;
    saveData.vfxState.bloom.threshold = 0.55f;
    saveData.vfxState.bloom.intensity = 1.4f;
    saveData.vfxState.bloom.spread = 0.45f;
    saveData.vfxState.chromaticAberration.enabled = true;
    saveData.vfxState.chromaticAberration.amount = 4.0f;
    saveData.vfxState.chromaticAberration.radialFalloff = 2.5f;
    saveData.vfxState.lightWrap.enabled = true;
    saveData.vfxState.lightWrap.amount = 0.4f;
    saveData.vfxState.lightWrap.radius = 12.0f;

    const QString json = ProjectFile::toJsonString(saveData);
    ProjectData loadedData;
    if (!requireSelftest(ProjectFile::fromJsonString(json, loadedData),
                         QStringLiteral("ProjectFile::fromJsonString failed"), &error)) {
        return 1;
    }
    if (!requireSelftest(loadedData.particleClipEntries.size() == 1,
                         QStringLiteral("particle clip entry count mismatch"), &error)
        || !requireSelftest(loadedData.particleClipEntries.first().config.forceFields.size() == 1,
                            QStringLiteral("particle force field round-trip failed"), &error)
        || !requireSelftest(loadedData.particleClipEntries.first().config.collisionFloor,
                            QStringLiteral("particle collision floor lost on round-trip"), &error)
        || !requireSelftest(std::abs(loadedData.particleClipEntries.first().config.turbulenceAmount
                                     - particleCfg.turbulenceAmount) < 0.001,
                            QStringLiteral("particle turbulence round-trip failed"), &error)
        || !requireSelftest(loadedData.videoTracks.size() == 1
                            && loadedData.videoTracks.first().size() == 1
                            && loadedData.videoTracks.first().first().effects.size() == 1
                            && std::abs(loadedData.videoTracks.first().first().effects.first().param1 - 18.0) < 0.001
                            && std::abs(loadedData.videoTracks.first().first().effects.first().param2 - 7.0) < 0.001
                            && std::abs(loadedData.videoTracks.first().first().effects.first().param3) < 0.001,
                            QStringLiteral("video effect param round-trip failed"), &error)
        || !requireSelftest(loadedData.vfxState.glow.enabled
                            && std::abs(loadedData.vfxState.glow.threshold - 0.65f) < 0.001f
                            && loadedData.vfxState.bloom.enabled
                            && std::abs(loadedData.vfxState.chromaticAberration.amount - 4.0f) < 0.001f
                            && loadedData.vfxState.lightWrap.enabled,
                            QStringLiteral("VFX preview state round-trip failed"), &error)) {
        return 1;
    }

    ProjectData legacyData;
    const QString legacyJson = QStringLiteral(
        "{\"version\":1,\"config\":{\"name\":\"legacy\",\"width\":1920,\"height\":1080,\"fps\":30},"
        "\"videoTracks\":[[{\"filePath\":\"legacy.mp4\",\"displayName\":\"legacy.mp4\",\"duration\":1.0,"
        "\"inPoint\":0.0,\"outPoint\":1.0,\"speed\":1.0,\"volume\":1.0,"
        "\"effects\":[{\"type\":10,\"enabled\":true,\"param1\":9.0,\"param2\":2.0,\"param3\":1.0,\"keyColor\":\"#00ff00\"}]}]],"
        "\"audioTracks\":[[]],\"playheadPos\":0.0,\"markIn\":-1.0,\"markOut\":-1.0,\"zoomLevel\":10}");
    if (!requireSelftest(ProjectFile::fromJsonString(legacyJson, legacyData),
                         QStringLiteral("legacy project load failed"), &error)
        || !requireSelftest(legacyData.particleClipEntries.isEmpty(),
                            QStringLiteral("legacy project should default particle entries to empty"), &error)
        || !requireSelftest(!legacyData.vfxState.glow.enabled && !legacyData.vfxState.bloom.enabled,
                            QStringLiteral("legacy project should default VFX state"), &error)) {
        return 1;
    }

    QTemporaryDir tempDir;
    if (!requireSelftest(tempDir.isValid(), QStringLiteral("temporary directory for project save is invalid"), &error))
        return 1;
    const QString projectPath = tempDir.path() + QStringLiteral("/vfx_selftest.veditor");
    if (!requireSelftest(ProjectFile::save(projectPath, saveData), QStringLiteral("ProjectFile::save failed"), &error))
        return 1;
    ProjectData fileLoaded;
    if (!requireSelftest(ProjectFile::load(projectPath, fileLoaded), QStringLiteral("ProjectFile::load failed"), &error)
        || !requireSelftest(fileLoaded.particleClipEntries.size() == 1 && fileLoaded.vfxState.lightWrap.enabled,
                            QStringLiteral("saved project did not reload VFX state"), &error)) {
        return 1;
    }

    qInfo().noquote() << QStringLiteral("VFX selftest OK");
    return 0;
}

} // anonymous namespace

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

    if (qEnvironmentVariableIntValue("VEDITOR_VFX_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_VFX_SELFTEST");
        return runVfxSelftest();
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
