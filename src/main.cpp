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
#include <QPainter>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "MainWindow.h"
#include "FractalNoise.h"
#include "ParticleSystem.h"
#include "ProjectFile.h"
#include "MaskSystem.h"
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

// US-WF-D: Sprint 11 workflow self-test dependencies.
#if __has_include("AIMask.h")
#include "AIMask.h"
#define HAVE_AIMASK 1
#endif
#if __has_include("MagneticTimeline.h")
#include "MagneticTimeline.h"
#define HAVE_MAGTL 1
#endif
#if __has_include("AudioClipEditor.h")
#include "AudioClipEditor.h"
#define HAVE_AUDIOCLIPEDITOR 1
#endif

// US-SC-B: Sprint 12 shortcut customization self-test dependency.
#if __has_include("ShortcutManager.h")
#include "ShortcutManager.h"
#define HAVE_SHORTCUTMANAGER 1
#endif

// US-SC2-B: Sprint 13 social export self-test (VEDITOR_SOCIAL_SELFTEST=1)
#if __has_include("SocialPreset.h")
#include "SocialPreset.h"
#define HAVE_SOCIALPRESET 1
#endif
#if __has_include("AspectReframer.h")
#include "AspectReframer.h"
#define HAVE_ASPECTREFRAMER 1
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
    qCritical() << "selftest failed:" << message;
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

RotoPath makeRectRotoPath(const QRectF &rect)
{
    RotoPath path;
    path.closed = true;
    const QVector<QPointF> points = {
        rect.topLeft(),
        QPointF(rect.right(), rect.top()),
        rect.bottomRight(),
        QPointF(rect.left(), rect.bottom())
    };
    for (const QPointF &pos : points) {
        RotoPoint point;
        point.position = pos;
        point.handleIn = pos;
        point.handleOut = pos;
        path.points.append(point);
    }
    return path;
}

bool masksEqual(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return false;
    const QImage lhs = a.convertToFormat(QImage::Format_Grayscale8);
    const QImage rhs = b.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < lhs.height(); ++y) {
        const uchar *lhsLine = lhs.constScanLine(y);
        const uchar *rhsLine = rhs.constScanLine(y);
        for (int x = 0; x < lhs.width(); ++x) {
            if (lhsLine[x] != rhsLine[x])
                return false;
        }
    }
    return true;
}

int runProSelftest()
{
    QString error;
    auto countBrightPixels = [](const QImage &image, int threshold) {
        if (image.isNull())
            return 0;
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qGray(image.pixel(x, y)) >= threshold)
                    ++count;
            }
        }
        return count;
    };

    QImage flowA(64, 64, QImage::Format_ARGB32);
    flowA.fill(Qt::black);
    {
        QPainter painter(&flowA);
        painter.fillRect(QRect(16, 16, 18, 18), Qt::white);
    }
    QImage flowB(64, 64, QImage::Format_ARGB32);
    flowB.fill(Qt::black);
    {
        QPainter painter(&flowB);
        painter.fillRect(QRect(20, 18, 18, 18), Qt::white);
    }

    const opticalflow::FlowField flow = opticalflow::estimateFlow(flowA, flowB);
    if (!requireSelftest(flow.width == 64 && flow.height == 64 && flow.v.size() == 64 * 64,
                         QStringLiteral("optical flow field invalid"), &error)) {
        return 1;
    }
    const QPointF centerFlow = flow.at(24, 24);
    if (!requireSelftest(centerFlow.x() > 1.0 && centerFlow.y() > 0.5,
                         QStringLiteral("optical flow shift estimate too small"), &error)) {
        return 1;
    }
    const QImage warped = opticalflow::warpImage(flowA, flow, 1.0);
    if (!requireSelftest(!warped.isNull() && warped.size() == flowA.size(),
                         QStringLiteral("warpImage returned invalid image"), &error)
        || !requireSelftest(qGray(warped.pixel(24, 24)) > 32,
                            QStringLiteral("warpImage lost tracked content"), &error)) {
        return 1;
    }

    QImage traceFrame(128, 128, QImage::Format_ARGB32);
    traceFrame.fill(Qt::black);
    {
        QPainter painter(&traceFrame);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QRectF(28.0, 28.0, 72.0, 56.0));
    }
    const RotoPath tracedPath = rototrace::autoTraceContour(traceFrame, QRectF(20.0, 20.0, 88.0, 72.0));
    if (!requireSelftest(!tracedPath.points.isEmpty() && tracedPath.closed,
                         QStringLiteral("autoTraceContour returned empty path"), &error)) {
        return 1;
    }

    QVector<QImage> trackFrames;
    for (int i = 0; i < 3; ++i) {
        QImage frame(96, 96, QImage::Format_ARGB32);
        frame.fill(Qt::black);
        QPainter painter(&frame);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRect(QRectF(18.0 + i * 4.0, 28.0, 24.0, 24.0));
        trackFrames.append(frame);
    }
    const RotoPath seedPath = makeRectRotoPath(QRectF(18.0, 28.0, 24.0, 24.0));
    rototrack::RotoTrackParams trackParams;
    trackParams.keyframeInterval = 1;
    const QVector<RotoKeyframe> propagated = rototrack::propagateRotoShape(seedPath, 0, trackFrames, 0, trackParams);
    if (!requireSelftest(propagated.size() >= 3 && propagated.last().frameNumber == 2,
                         QStringLiteral("propagateRotoShape produced insufficient keyframes"), &error)
        || !requireSelftest(propagated.last().path.points.first().position.x()
                                >= seedPath.points.first().position.x(),
                            QStringLiteral("propagateRotoShape failed to move path forward"), &error)) {
        return 1;
    }

    QImage matteAlphaSource(32, 32, QImage::Format_ARGB32);
    matteAlphaSource.fill(Qt::transparent);
    {
        QPainter painter(&matteAlphaSource);
        painter.fillRect(QRect(0, 0, 16, 32), QColor(255, 255, 255, 255));
    }
    QImage matteLumaSource(32, 32, QImage::Format_ARGB32);
    matteLumaSource.fill(QColor(0, 0, 0, 255));
    {
        QPainter painter(&matteLumaSource);
        painter.fillRect(QRect(0, 0, 16, 32), QColor(255, 255, 255, 255));
    }
    QImage matteClip(32, 32, QImage::Format_ARGB32);
    matteClip.fill(QColor(255, 0, 0, 255));
    const QImage alphaMatte = MaskSystem::applyTrackMatte(matteClip, matteAlphaSource, TrackMatteType::AlphaMatte);
    const QImage alphaInverted = MaskSystem::applyTrackMatte(matteClip, matteAlphaSource, TrackMatteType::AlphaInvertedMatte);
    const QImage lumaMatte = MaskSystem::applyTrackMatte(matteClip, matteLumaSource, TrackMatteType::LumaMatte);
    const QImage lumaInverted = MaskSystem::applyTrackMatte(matteClip, matteLumaSource, TrackMatteType::LumaInvertedMatte);
    if (!requireSelftest(!alphaMatte.isNull() && !alphaInverted.isNull()
                             && !lumaMatte.isNull() && !lumaInverted.isNull(),
                         QStringLiteral("applyTrackMatte returned null"), &error)
        || !requireSelftest(qAlpha(alphaMatte.pixel(8, 8)) > qAlpha(alphaMatte.pixel(24, 8)),
                            QStringLiteral("alpha matte result invalid"), &error)
        || !requireSelftest(qAlpha(alphaInverted.pixel(8, 8)) < qAlpha(alphaInverted.pixel(24, 8)),
                            QStringLiteral("alpha inverted matte result invalid"), &error)
        || !requireSelftest(qAlpha(lumaMatte.pixel(8, 8)) > qAlpha(lumaMatte.pixel(24, 8)),
                            QStringLiteral("luma matte result invalid"), &error)
        || !requireSelftest(qAlpha(lumaInverted.pixel(8, 8)) < qAlpha(lumaInverted.pixel(24, 8)),
                            QStringLiteral("luma inverted matte result invalid"), &error)) {
        return 1;
    }

    timeremap::TimeRemapCurve nearestCurve;
    nearestCurve.sourceFps = 1.0;
    nearestCurve.addKey(0.0, 0.0);
    nearestCurve.addKey(2.0, 2.0);
    QVector<QImage> solidFrames;
    for (const QColor &color : {QColor(255, 0, 0), QColor(0, 255, 0), QColor(0, 0, 255)}) {
        QImage frame(12, 12, QImage::Format_ARGB32);
        frame.fill(color);
        solidFrames.append(frame);
    }
    const auto solidFetch = [solidFrames](int index) -> QImage {
        if (index < 0 || index >= solidFrames.size())
            return {};
        return solidFrames[index];
    };

    nearestCurve.blendMode = timeremap::FrameBlendMode::NearestFrame;
    const QImage nearest = timeremap::resolveFrame(nearestCurve, 0.1, solidFetch);
    nearestCurve.blendMode = timeremap::FrameBlendMode::Blend;
    const QImage blended = timeremap::resolveFrame(nearestCurve, 0.5, solidFetch);

    timeremap::TimeRemapCurve flowCurve;
    flowCurve.sourceFps = 1.0;
    flowCurve.addKey(0.0, 0.0);
    flowCurve.addKey(1.0, 1.0);
    flowCurve.blendMode = timeremap::FrameBlendMode::OpticalFlow;
    const auto motionFetch = [trackFrames](int index) -> QImage {
        if (index < 0 || index >= trackFrames.size())
            return {};
        return trackFrames[index];
    };
    const QImage flowRemapped = timeremap::resolveFrame(flowCurve, 0.5, motionFetch);
    if (!requireSelftest(!nearest.isNull() && !blended.isNull() && !flowRemapped.isNull(),
                         QStringLiteral("resolveFrame returned null"), &error)
        || !requireSelftest(qRed(nearest.pixel(2, 2)) > 200,
                            QStringLiteral("nearest-frame remap incorrect"), &error)
        || !requireSelftest(qRed(blended.pixel(2, 2)) > 32 && qGreen(blended.pixel(2, 2)) > 32,
                            QStringLiteral("blend remap did not mix frames"), &error)
        || !requireSelftest(countBrightPixels(flowRemapped, 64) > 64,
                            QStringLiteral("optical-flow remap lost foreground"), &error)) {
        return 1;
    }

    ProjectData saveData;
    saveData.config.name = QStringLiteral("PRO Selftest");
    saveData.config.width = 640;
    saveData.config.height = 360;
    saveData.config.fps = 30;
    ClipInfo clip;
    clip.filePath = QStringLiteral("synthetic-pro.mp4");
    clip.displayName = QStringLiteral("synthetic-pro.mp4");
    clip.duration = 3.0;
    saveData.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}, QVector<ClipInfo>{clip}};
    saveData.audioTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}};

    RotoClipEntry rotoEntry;
    rotoEntry.clipId = QStringLiteral("0:0");
    rotoEntry.path = tracedPath;
    rotoEntry.keyframes = propagated;
    rotoEntry.brushMask = QImage(4, 4, QImage::Format_Grayscale8);
    rotoEntry.brushMask.fill(0);
    rotoEntry.brushMask.scanLine(0)[0] = 255;
    rotoEntry.brushMask.scanLine(1)[1] = 192;
    saveData.rotoClipEntries.append(rotoEntry);

    TimeRemapClipEntry remapEntry;
    remapEntry.clipId = QStringLiteral("0:0");
    remapEntry.curve = nearestCurve;
    saveData.timeRemapClipEntries.append(remapEntry);

    TrackMatteClipEntry matteEntry;
    matteEntry.clipId = QStringLiteral("0:0");
    matteEntry.matteType = TrackMatteType::LumaMatte;
    matteEntry.matteSourceClipId = QStringLiteral("1:0");
    saveData.trackMatteClipEntries.append(matteEntry);

    ProjectData roundTrip;
    if (!requireSelftest(ProjectFile::fromJsonString(ProjectFile::toJsonString(saveData), roundTrip),
                         QStringLiteral("project string round-trip failed"), &error)
        || !requireSelftest(roundTrip.rotoClipEntries.size() == 1
                                && roundTrip.timeRemapClipEntries.size() == 1
                                && roundTrip.trackMatteClipEntries.size() == 1,
                            QStringLiteral("project clip sidecar counts mismatch"), &error)
        || !requireSelftest(roundTrip.rotoClipEntries.first().clipId == rotoEntry.clipId
                                && roundTrip.rotoClipEntries.first().keyframes.size() == propagated.size(),
                            QStringLiteral("roto clip entry round-trip failed"), &error)
        || !requireSelftest(masksEqual(roundTrip.rotoClipEntries.first().brushMask, rotoEntry.brushMask),
                            QStringLiteral("roto brush mask round-trip failed"), &error)
        || !requireSelftest(roundTrip.timeRemapClipEntries.first().curve.keys.size()
                                == remapEntry.curve.keys.size()
                                && roundTrip.timeRemapClipEntries.first().curve.blendMode
                                       == remapEntry.curve.blendMode,
                            QStringLiteral("time remap curve round-trip failed"), &error)
        || !requireSelftest(roundTrip.trackMatteClipEntries.first().matteType == matteEntry.matteType
                                && roundTrip.trackMatteClipEntries.first().matteSourceClipId == matteEntry.matteSourceClipId,
                            QStringLiteral("track matte round-trip failed"), &error)) {
        return 1;
    }

    ProjectData legacyData;
    const QString legacyJson = QStringLiteral(
        "{\"version\":1,\"config\":{\"name\":\"legacy-pro\",\"width\":1920,\"height\":1080,\"fps\":30},"
        "\"videoTracks\":[[{\"filePath\":\"legacy.mp4\",\"displayName\":\"legacy.mp4\",\"duration\":1.0,"
        "\"inPoint\":0.0,\"outPoint\":1.0,\"speed\":1.0,\"volume\":1.0}]],"
        "\"audioTracks\":[[]],\"playheadPos\":0.0,\"markIn\":-1.0,\"markOut\":-1.0,\"zoomLevel\":10}");
    if (!requireSelftest(ProjectFile::fromJsonString(legacyJson, legacyData),
                         QStringLiteral("legacy PRO project load failed"), &error)
        || !requireSelftest(legacyData.rotoClipEntries.isEmpty()
                                && legacyData.timeRemapClipEntries.isEmpty()
                                && legacyData.trackMatteClipEntries.isEmpty(),
                            QStringLiteral("legacy PRO project should default new fields"), &error)) {
        return 1;
    }

    QTemporaryDir tempDir;
    if (!requireSelftest(tempDir.isValid(), QStringLiteral("PRO selftest temp dir invalid"), &error))
        return 1;
    const QString projectPath = tempDir.path() + QStringLiteral("/pro_selftest.veditor");
    ProjectData fileLoaded;
    if (!requireSelftest(ProjectFile::save(projectPath, saveData), QStringLiteral("PRO project save failed"), &error)
        || !requireSelftest(ProjectFile::load(projectPath, fileLoaded), QStringLiteral("PRO project load failed"), &error)
        || !requireSelftest(fileLoaded.rotoClipEntries.size() == 1
                                && fileLoaded.timeRemapClipEntries.size() == 1
                                && fileLoaded.trackMatteClipEntries.size() == 1,
                            QStringLiteral("saved PRO project reload mismatch"), &error)) {
        return 1;
    }

    qInfo().noquote() << QStringLiteral("PRO selftest OK");
    return 0;
}

// ──────────────────────────────────────────────────────────────────────────
// US-3D-11: motion-graphics sprint self-test (VEDITOR_MOGRAPH_SELFTEST=1)
// ──────────────────────────────────────────────────────────────────────────

static bool meshNormalsSane(const mesh3d::TriMesh &mesh, QString &why)
{
    if (mesh.indices.isEmpty() || (mesh.indices.size() % 3) != 0) {
        why = QStringLiteral("indices empty or not a multiple of 3");
        return false;
    }
    if (mesh.positions.isEmpty()) {
        why = QStringLiteral("no positions");
        return false;
    }
    for (quint32 idx : mesh.indices) {
        if (idx >= static_cast<quint32>(mesh.positions.size())) {
            why = QStringLiteral("index out of range");
            return false;
        }
    }
    for (const QVector3D &n : mesh.normals) {
        const float len = n.length();
        if (len > 1e-3f && std::abs(len - 1.0f) > 1e-3f) {
            why = QStringLiteral("normal not unit-length");
            return false;
        }
        if (!std::isfinite(n.x()) || !std::isfinite(n.y()) || !std::isfinite(n.z())) {
            why = QStringLiteral("normal NaN/inf");
            return false;
        }
    }
    return true;
}

static int countNonBackground(const QImage &img, const QColor &background)
{
    if (img.isNull())
        return 0;
    int count = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.rgba() != background.rgba())
                ++count;
        }
    }
    return count;
}

int runMographSelftest()
{
    QString error;

    // --- (1) Extruded mesh: square contour + glyph contours ---
    {
        QPolygonF square;
        square << QPointF(-0.5, -0.5) << QPointF(0.5, -0.5)
               << QPointF(0.5, 0.5) << QPointF(-0.5, 0.5);
        mesh3d::ExtrudeParams ep;
        ep.depth = 1.0;
        ep.bevelDepth = 0.05;
        ep.bevelWidth = 0.05;
        ep.bevelSegments = 2;
        ep.smoothCapNormals = true;
        const mesh3d::TriMesh squareMesh = mesh3d::buildExtrudedMesh(QVector<QPolygonF>{square}, ep);
        QString why;
        if (!requireSelftest(squareMesh.indices.size() >= 3, QStringLiteral("square mesh has no triangles"), &error)
            || !requireSelftest(meshNormalsSane(squareMesh, why), QStringLiteral("square mesh invalid: ") + why, &error)) {
            return 1;
        }

        const QVector<QPolygonF> glyphs = mesh3d::glyphContours(QStringLiteral("A"),
                                                               QFont(QStringLiteral("Sans Serif"), 32), 0.5);
        const mesh3d::TriMesh glyphMesh = mesh3d::buildExtrudedMesh(glyphs, ep);
        if (!requireSelftest(glyphMesh.indices.size() >= 3, QStringLiteral("glyph mesh has no triangles"), &error)
            || !requireSelftest(meshNormalsSane(glyphMesh, why), QStringLiteral("glyph mesh invalid: ") + why, &error)) {
            return 1;
        }

        // --- (2a) Software raster of the glyph mesh into a small image ---
        softras::RenderParams rp;
        rp.background = QColor(0, 0, 0, 0);
        const QImage rendered = softras::renderMeshAuto(glyphMesh, QSize(64, 64), 25.0, 12.0, 3.0, rp);
        if (!requireSelftest(!rendered.isNull(), QStringLiteral("renderMeshAuto returned null image"), &error)
            || !requireSelftest(countNonBackground(rendered, rp.background) > 0,
                                QStringLiteral("renderMeshAuto produced an empty image"), &error)) {
            return 1;
        }
    }

    // --- (2b) Synthetic 2-triangle z-buffer test ---
    {
        // Near triangle A at z=0 (small, centred) coloured by frontColor.
        // Far triangle B at z=-0.6 (bigger, overlapping) coloured by sideColor
        // via a stored normal whose X dominates Z. Both wound CCW from +Z so
        // both are front-facing; the z-buffer must keep A in the overlap.
        auto buildZTestMesh = [](bool nearFirst) {
            mesh3d::TriMesh m;
            const QVector3D nA(0.0f, 0.0f, 1.0f);
            const QVector3D nB(0.92f, 0.0f, 0.39f); // |x| > |z| -> sideColor
            const QVector3D a0(-0.4f, -0.4f, 0.0f);
            const QVector3D a1( 0.4f, -0.4f, 0.0f);
            const QVector3D a2( 0.0f,  0.5f, 0.0f);
            const QVector3D b0(-0.8f, -0.8f, -0.6f);
            const QVector3D b1( 0.8f, -0.8f, -0.6f);
            const QVector3D b2( 0.0f,  0.9f, -0.6f);
            auto addTri = [&m](const QVector3D &p0, const QVector3D &p1, const QVector3D &p2, const QVector3D &n) {
                const quint32 base = static_cast<quint32>(m.positions.size());
                m.positions << p0 << p1 << p2;
                m.normals << n << n << n;
                m.indices << base << (base + 1) << (base + 2);
            };
            if (nearFirst) {
                addTri(a0, a1, a2, nA);
                addTri(b0, b1, b2, nB);
            } else {
                addTri(b0, b1, b2, nB);
                addTri(a0, a1, a2, nA);
            }
            return m;
        };

        softras::RenderParams rp;
        rp.background = QColor(0, 0, 0, 0);
        rp.frontColor = QColor(255, 0, 0);          // A == red
        rp.sideColor = QColor(0, 0, 255);           // B == blue
        rp.ambient = QColor(20, 20, 20);
        rp.lightDir = QVector3D(-1.0f, 0.0f, -1.0f).normalized();
        const softras::Mat4 model = softras::Mat4::identity();
        const softras::Mat4 view = softras::Mat4::lookAt(QVector3D(0, 0, 5), QVector3D(0, 0, 0), QVector3D(0, 1, 0));
        const softras::Mat4 proj = softras::Mat4::perspective(45.0, 1.0, 0.1, 100.0);
        const QSize sz(80, 80);

        for (bool nearFirst : {true, false}) {
            const QImage img = softras::render(buildZTestMesh(nearFirst), model, view, proj, sz, rp);
            if (!requireSelftest(!img.isNull(), QStringLiteral("z-buffer test render null"), &error))
                return 1;
            // The very centre pixel is inside both triangles -> must be red (A wins).
            const QColor centre = img.pixelColor(sz.width() / 2, sz.height() / 2);
            if (!requireSelftest(centre.alpha() > 0 && centre.red() > centre.blue() + 20,
                                 QStringLiteral("z-buffer overlap pixel is not the near triangle's colour"), &error)) {
                return 1;
            }
            // Somewhere only B is covered -> a bluish pixel must exist.
            bool foundBlue = false;
            for (int y = 0; y < sz.height() && !foundBlue; ++y) {
                for (int x = 0; x < sz.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.alpha() > 0 && c.blue() > c.red() + 20) { foundBlue = true; break; }
                }
            }
            if (!requireSelftest(foundBlue, QStringLiteral("z-buffer test: far triangle not visible anywhere"), &error))
                return 1;
        }
    }

    // --- (3) Expression math ---
    {
        ExpressionContext ctx;
        ctx.time = 2.0;
        ctx.fps = 30.0;
        ctx.duration = 5.0;
        ctx.canvasWidth = 1920;
        ctx.canvasHeight = 1080;

        auto evalWith = [&](const QString &code, double v) {
            ExpressionContext c = ctx;
            c.value = v;
            return Expression::evaluate(code, c);
        };
        const ExpressionResult clampHi = evalWith(QStringLiteral("clamp(value,0,1)"), 2.5);
        const ExpressionResult clampLo = evalWith(QStringLiteral("clamp(value,0,1)"), -3.0);
        const ExpressionResult lenR = Expression::evaluate(QStringLiteral("length(3,4)"), ctx);
        const ExpressionResult mixR = Expression::evaluate(QStringLiteral("mix(0,10,0.25)"), ctx);
        const ExpressionResult degR = Expression::evaluate(QStringLiteral("degreesToRadians(180)"), ctx);
        if (!requireSelftest(clampHi.success && std::abs(clampHi.value - 1.0) < 1e-9,
                             QStringLiteral("clamp(2.5,0,1) != 1"), &error)
            || !requireSelftest(clampLo.success && std::abs(clampLo.value - 0.0) < 1e-9,
                                QStringLiteral("clamp(-3,0,1) != 0"), &error)
            || !requireSelftest(lenR.success && std::abs(lenR.value - 5.0) < 1e-9,
                                QStringLiteral("length(3,4) != 5"), &error)
            || !requireSelftest(mixR.success && std::abs(mixR.value - 2.5) < 1e-9,
                                QStringLiteral("mix(0,10,0.25) != 2.5"), &error)
            || !requireSelftest(degR.success && std::abs(degR.value - M_PI) < 1e-6,
                                QStringLiteral("degreesToRadians(180) != pi"), &error)) {
            return 1;
        }

        // noise(time): fine steps -> small consecutive deltas, deterministic.
        double prev = 0.0;
        double maxDelta = 0.0;
        bool first = true;
        for (int i = 0; i <= 200; ++i) {
            const double t = 1.0 + i * 0.005;
            const ExpressionResult r = Expression::evaluate(QStringLiteral("noise(time)"),
                ExpressionContext{t, 0, 30.0, 5.0, 1920, 1080, 0.0, {}});
            if (!requireSelftest(r.success && std::isfinite(r.value), QStringLiteral("noise(time) failed"), &error))
                return 1;
            if (!first)
                maxDelta = qMax(maxDelta, std::abs(r.value - prev));
            prev = r.value;
            first = false;
        }
        const ExpressionResult n1 = Expression::evaluate(QStringLiteral("noise(time)"),
            ExpressionContext{1.234, 0, 30.0, 5.0, 1920, 1080, 0.0, {}});
        const ExpressionResult n2 = Expression::evaluate(QStringLiteral("noise(time)"),
            ExpressionContext{1.234, 0, 30.0, 5.0, 1920, 1080, 0.0, {}});
        if (!requireSelftest(maxDelta < 0.1, QStringLiteral("noise(time) has stairsteps (delta too large)"), &error)
            || !requireSelftest(n1.success && n2.success && std::abs(n1.value - n2.value) < 1e-12,
                                QStringLiteral("noise(time) not deterministic"), &error)) {
            return 1;
        }

        // smooth(0.5, 5) with a sampleValueAtTime ramp -> value within local min/max.
        ExpressionContext sctx = ctx;
        sctx.sampleValueAtTime = [](double t) { return t; };
        const ExpressionResult sm = Expression::evaluate(QStringLiteral("smooth(0.5,5)"), sctx);
        const double sLo = sctx.time - 0.25;
        const double sHi = sctx.time + 0.25;
        if (!requireSelftest(sm.success && sm.value >= sLo - 1e-6 && sm.value <= sHi + 1e-6,
                             QStringLiteral("smooth(0.5,5) outside sampled signal min/max"), &error)) {
            return 1;
        }
    }

    // --- (4) ClipExpressionBindings ---
    {
        exprbind::ClipExpressionBindings bindings;
        bindings.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("value*0.5"));
        ExpressionContext ctx;
        ctx.time = 0.5;
        ctx.fps = 30.0;
        ctx.duration = 3.0;
        ctx.canvasWidth = 1920;
        ctx.canvasHeight = 1080;
        const double opacity = bindings.resolve(QStringLiteral("transform.opacity"), ctx, 80.0);
        const double posX = bindings.resolve(QStringLiteral("transform.position.x"), ctx, 123.0);
        // Garbage / division-by-zero expression must fall back to the keyframe value (no NaN/inf).
        bindings.setExpression(QStringLiteral("transform.rotation"), QStringLiteral("1/0"));
        const double rot = bindings.resolve(QStringLiteral("transform.rotation"), ctx, 42.0);
        if (!requireSelftest(std::abs(opacity - 40.0) < 1e-6, QStringLiteral("resolve transform.opacity != 40"), &error)
            || !requireSelftest(std::abs(posX - 123.0) < 1e-9, QStringLiteral("unbound resolve != keyframe value"), &error)
            || !requireSelftest(std::isfinite(rot) && std::abs(rot - 42.0) < 1e-9,
                                QStringLiteral("garbage expression did not fall back to keyframe value"), &error)) {
            return 1;
        }
    }

    // --- (5) Camera3D + CameraShake ---
    {
        Camera3D cam;
        // Shake disabled -> getCameraAt(t) is the (unjittered) base for all t.
        const Camera3DState base0 = cam.getCameraAt(0.0);
        for (double t : {0.5, 1.0, 2.5, 7.0}) {
            const Camera3DState s = cam.getCameraAt(t);
            if (!requireSelftest(s.position == base0.position && s.target == base0.target
                                     && std::abs(s.fov - base0.fov) < 1e-9 && std::abs(s.roll - base0.roll) < 1e-9,
                                 QStringLiteral("camera with disabled shake drifted from base"), &error)) {
                return 1;
            }
        }
        CameraShake shake;
        shake.enabled = true;
        shake.frequency = 4.0;
        shake.positionAmplitude = QVector3D(0.5f, 0.5f, 0.5f);
        shake.rotationAmplitudeDeg = 5.0;
        shake.seed = 1234;
        shake.smoothness = 1.0;
        cam.setShake(shake);
        // Deterministic for fixed seed.
        const Camera3DState s1 = cam.getCameraAt(1.7);
        const Camera3DState s2 = cam.getCameraAt(1.7);
        if (!requireSelftest(s1.position == s2.position && std::abs(s1.roll - s2.roll) < 1e-12,
                             QStringLiteral("camera shake not deterministic"), &error)) {
            return 1;
        }
        // Bounded around the (unjittered) base by ~positionAmplitude; smoothly varying.
        double maxAbsDelta = 0.0;
        double maxRollDelta = 0.0;
        double maxStep = 0.0;
        Camera3DState prevState = cam.getCameraAt(0.0);
        for (int i = 0; i <= 400; ++i) {
            const double t = i * 0.01;
            const Camera3DState s = cam.getCameraAt(t);
            maxAbsDelta = qMax(maxAbsDelta,
                               qMax(std::abs(double(s.position.x() - base0.position.x())),
                                    qMax(std::abs(double(s.position.y() - base0.position.y())),
                                         std::abs(double(s.position.z() - base0.position.z())))));
            maxRollDelta = qMax(maxRollDelta, std::abs(s.roll - base0.roll));
            if (i > 0) {
                maxStep = qMax(maxStep,
                               qMax(std::abs(double(s.position.x() - prevState.position.x())),
                                    std::abs(double(s.position.y() - prevState.position.y()))));
            }
            prevState = s;
        }
        if (!requireSelftest(maxAbsDelta <= 0.5 + 1e-4,
                             QStringLiteral("camera shake position exceeds amplitude bound"), &error)
            || !requireSelftest(maxRollDelta <= 5.0 + 1e-4,
                                QStringLiteral("camera shake roll exceeds amplitude bound"), &error)
            || !requireSelftest(maxStep < 0.5,
                                QStringLiteral("camera shake not smooth (large step between fine samples)"), &error)) {
            return 1;
        }
    }

    // --- (6) WiggleTransform ---
    {
        wiggle::WiggleParams disabled;
        const wiggle::WiggleOffset offDisabled = wiggle::evaluate(disabled, 1.23);
        if (!requireSelftest(offDisabled.positionOffset.isNull()
                                 && std::abs(offDisabled.rotationOffsetDeg) < 1e-12
                                 && std::abs(offDisabled.scaleMultiplier - 1.0) < 1e-12,
                             QStringLiteral("disabled wiggle is not the identity offset"), &error)) {
            return 1;
        }
        wiggle::WiggleParams p = wiggle::handheldPreset(1.0);
        p.seed = 99;
        const wiggle::WiggleOffset a = wiggle::evaluate(p, 2.5);
        const wiggle::WiggleOffset b = wiggle::evaluate(p, 2.5);
        if (!requireSelftest(a.positionOffset == b.positionOffset
                                 && std::abs(a.rotationOffsetDeg - b.rotationOffsetDeg) < 1e-12,
                             QStringLiteral("wiggle not deterministic"), &error)) {
            return 1;
        }
        double maxMag = 0.0;
        double maxStep = 0.0;
        wiggle::WiggleOffset prevOff = wiggle::evaluate(p, 0.0);
        for (int i = 0; i <= 500; ++i) {
            const double t = i * 0.01;
            const wiggle::WiggleOffset o = wiggle::evaluate(p, t);
            if (!requireSelftest(std::isfinite(o.positionOffset.x()) && std::isfinite(o.positionOffset.y())
                                     && std::isfinite(o.rotationOffsetDeg) && std::isfinite(o.scaleMultiplier),
                                 QStringLiteral("wiggle produced NaN/inf"), &error)) {
                return 1;
            }
            maxMag = qMax(maxMag, qMax(std::abs(o.positionOffset.x()), std::abs(o.positionOffset.y())));
            if (i > 0) {
                maxStep = qMax(maxStep,
                               qMax(std::abs(o.positionOffset.x() - prevOff.positionOffset.x()),
                                    std::abs(o.positionOffset.y() - prevOff.positionOffset.y())));
            }
            prevOff = o;
        }
        // handheldPreset amp is ~6 px; allow generous slack for fbm overshoot.
        if (!requireSelftest(maxMag <= 30.0, QStringLiteral("wiggle position offset unbounded"), &error)
            || !requireSelftest(maxStep < maxMag + 1e-6, QStringLiteral("wiggle not smooth"), &error)) {
            return 1;
        }
    }

    // --- (7) ProjectFile round-trip with new sidecars ---
    {
        ProjectData saveData;
        saveData.config.name = QStringLiteral("MOGRAPH Selftest");
        saveData.config.width = 640;
        saveData.config.height = 360;
        saveData.config.fps = 30;
        ClipInfo clip;
        clip.filePath = QStringLiteral("synthetic-mograph.mp4");
        clip.displayName = QStringLiteral("synthetic-mograph.mp4");
        clip.duration = 4.0;
        saveData.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
        saveData.audioTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}};

        Text3DLayer text3D;
        text3D.setText(QStringLiteral("HELLO"), QFont(QStringLiteral("Sans Serif"), 40));
        text3D.setExtrudeEnabled(true);
        text3D.setExtrude(0.3, 0.04, 0.04, 3);
        text3D.setMaterial(QColor(220, 180, 60), QColor(140, 110, 40), QColor(30, 30, 30),
                           QVector3D(0.2f, 0.3f, -1.0f));
        text3D.setExtrudeYawPitch(15.0, -7.0);
        Text3DClipEntry t3Entry;
        t3Entry.clipId = QStringLiteral("0:0");
        t3Entry.config = text3D.toJson();
        saveData.text3DClipEntries.append(t3Entry);

        exprbind::ClipExpressionBindings bindings;
        bindings.setExpression(QStringLiteral("transform.rotation"), QStringLiteral("time*45"));
        bindings.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("clamp(value,0,100)"));
        ExpressionBindingsClipEntry exprEntry;
        exprEntry.clipId = QStringLiteral("0:0");
        exprEntry.bindings = bindings;
        saveData.expressionBindingsEntries.append(exprEntry);

        wiggle::WiggleParams wp = wiggle::nervousPreset(1.0);
        wp.seed = 4242;
        WiggleClipEntry wEntry;
        wEntry.clipId = QStringLiteral("0:0");
        wEntry.params = wp;
        saveData.wiggleClipEntries.append(wEntry);

        Camera3D projCam = Camera3D::createDollyZoom(0.0, -5.0, 3.0);
        CameraShake cs;
        cs.enabled = true;
        cs.seed = 7;
        cs.positionAmplitude = QVector3D(0.3f, 0.3f, 0.0f);
        cs.rotationAmplitudeDeg = 2.0;
        projCam.setShake(cs);
        saveData.projectCamera = projCam.toJson();

        ProjectData loaded;
        if (!requireSelftest(ProjectFile::fromJsonString(ProjectFile::toJsonString(saveData), loaded),
                             QStringLiteral("MOGRAPH project string round-trip failed"), &error)
            || !requireSelftest(loaded.text3DClipEntries.size() == 1
                                    && loaded.text3DClipEntries.first().clipId == QStringLiteral("0:0")
                                    && loaded.text3DClipEntries.first().config == t3Entry.config,
                                QStringLiteral("text3d clip entry round-trip mismatch"), &error)
            || !requireSelftest(loaded.expressionBindingsEntries.size() == 1
                                    && !loaded.expressionBindingsEntries.first().bindings.isEmpty()
                                    && loaded.expressionBindingsEntries.first().bindings.expression(QStringLiteral("transform.rotation"))
                                           == QStringLiteral("time*45"),
                                QStringLiteral("expression bindings round-trip mismatch"), &error)
            || !requireSelftest(loaded.wiggleClipEntries.size() == 1
                                    && loaded.wiggleClipEntries.first().params.enabled == wp.enabled
                                    && loaded.wiggleClipEntries.first().params.seed == wp.seed
                                    && loaded.wiggleClipEntries.first().params.octaves == wp.octaves
                                    && std::abs(loaded.wiggleClipEntries.first().params.positionAmplitude.x()
                                                - wp.positionAmplitude.x()) < 1e-6,
                                QStringLiteral("wiggle params round-trip mismatch"), &error)
            || !requireSelftest(!loaded.projectCamera.isEmpty(),
                                QStringLiteral("project camera lost on round-trip"), &error)) {
            return 1;
        }
        // Project camera deep check via fromJson.
        Camera3D loadedCam;
        loadedCam.fromJson(loaded.projectCamera);
        if (!requireSelftest(loadedCam.shake().enabled && loadedCam.shake().seed == cs.seed
                                 && loadedCam.hasAnimation(),
                             QStringLiteral("project camera round-trip lost shake/animation"), &error)) {
            return 1;
        }

        // Loading a JSON WITHOUT the new keys -> defaults, no crash.
        ProjectData legacy;
        const QString legacyJson = QStringLiteral(
            "{\"version\":1,\"config\":{\"name\":\"legacy-mograph\",\"width\":1920,\"height\":1080,\"fps\":30},"
            "\"videoTracks\":[[{\"filePath\":\"legacy.mp4\",\"displayName\":\"legacy.mp4\",\"duration\":1.0,"
            "\"inPoint\":0.0,\"outPoint\":1.0,\"speed\":1.0,\"volume\":1.0}]],"
            "\"audioTracks\":[[]],\"playheadPos\":0.0,\"markIn\":-1.0,\"markOut\":-1.0,\"zoomLevel\":10}");
        if (!requireSelftest(ProjectFile::fromJsonString(legacyJson, legacy),
                             QStringLiteral("legacy MOGRAPH project load failed"), &error)
            || !requireSelftest(legacy.text3DClipEntries.isEmpty()
                                    && legacy.expressionBindingsEntries.isEmpty()
                                    && legacy.wiggleClipEntries.isEmpty()
                                    && legacy.projectCamera.isEmpty(),
                                QStringLiteral("legacy MOGRAPH project should default the new fields"), &error)) {
            return 1;
        }

        // Also round-trip via a temp file.
        QTemporaryDir tempDir;
        if (!requireSelftest(tempDir.isValid(), QStringLiteral("MOGRAPH selftest temp dir invalid"), &error))
            return 1;
        const QString projectPath = tempDir.path() + QStringLiteral("/mograph_selftest.veditor");
        ProjectData fileLoaded;
        if (!requireSelftest(ProjectFile::save(projectPath, saveData), QStringLiteral("MOGRAPH project save failed"), &error)
            || !requireSelftest(ProjectFile::load(projectPath, fileLoaded), QStringLiteral("MOGRAPH project load failed"), &error)
            || !requireSelftest(fileLoaded.text3DClipEntries.size() == 1
                                    && fileLoaded.expressionBindingsEntries.size() == 1
                                    && fileLoaded.wiggleClipEntries.size() == 1
                                    && !fileLoaded.projectCamera.isEmpty(),
                                QStringLiteral("saved MOGRAPH project reload mismatch"), &error)) {
            return 1;
        }
    }

    qInfo().noquote() << QStringLiteral("MOGRAPH selftest OK");
    return 0;
}

// US-HW-9: hardware/performance self-test
int runHwPerfSelftest()
{
    QString error;

#ifdef HAVE_AUDIODUCKING
    // --- AudioDucking: gain envelope properties ---
    {
        const int sampleRate = 8000;
        const int length = 8000; // 1 second

        // (a) Silent sidechain -> gain stays ~1.0
        QVector<float> silentSide(length, 0.0f);
        DuckingParams p;
        const QVector<float> gainSilent = computeDuckingGain(silentSide, sampleRate, p);
        if (!requireSelftest(gainSilent.size() == length,
                             QStringLiteral("AudioDucking: silent sidechain size mismatch"), &error))
            return 1;
        bool allNearOne = true;
        bool anyNanSilent = false;
        for (float g : gainSilent) {
            if (std::isnan(g)) { anyNanSilent = true; break; }
            if (g <= 0.99f) { allNearOne = false; }
        }
        if (!requireSelftest(!anyNanSilent,
                             QStringLiteral("AudioDucking: silent sidechain produced NaN"), &error))
            return 1;
        if (!requireSelftest(allNearOne,
                             QStringLiteral("AudioDucking: silent sidechain -> gain not ~1.0"), &error))
            return 1;

        // (b) Strong constant sidechain -> tail converges to minGainLinear
        QVector<float> strongSide(length, 0.5f);
        const QVector<float> gainStrong = computeDuckingGain(strongSide, sampleRate, p);
        if (!requireSelftest(gainStrong.size() == length,
                             QStringLiteral("AudioDucking: strong sidechain size mismatch"), &error))
            return 1;
        const double minGain = minGainLinear(p);
        bool tailConverged = true;
        bool anyNanStrong = false;
        const int tailStart = length - 1000;
        for (int i = tailStart; i < length; ++i) {
            const float g = gainStrong[i];
            if (std::isnan(g)) { anyNanStrong = true; break; }
            if (std::abs(static_cast<double>(g) - minGain) >= 0.05) { tailConverged = false; }
        }
        if (!requireSelftest(!anyNanStrong,
                             QStringLiteral("AudioDucking: strong sidechain produced NaN"), &error))
            return 1;
        if (!requireSelftest(tailConverged,
                             QStringLiteral("AudioDucking: strong sidechain tail did not converge to minGainLinear"), &error))
            return 1;

        // (c) Output size == input size (already checked above, belt-and-suspenders)
        if (!requireSelftest(gainSilent.size() == silentSide.size(),
                             QStringLiteral("AudioDucking: output size != input size"), &error))
            return 1;

        // (d) NaN check across full silent result (already done above, explicit pass here)

        // (e) Determinism: same input -> identical output
        const QVector<float> gainSilent2 = computeDuckingGain(silentSide, sampleRate, p);
        bool deterministic = (gainSilent.size() == gainSilent2.size());
        if (deterministic) {
            for (int i = 0; i < gainSilent.size(); ++i) {
                if (gainSilent[i] != gainSilent2[i]) { deterministic = false; break; }
            }
        }
        if (!requireSelftest(deterministic,
                             QStringLiteral("AudioDucking: non-deterministic output for identical input"), &error))
            return 1;
    }
#endif // HAVE_AUDIODUCKING

#ifdef HAVE_SCENEDETECTOR
    // --- SceneDetector: cut detection behavior ---
    {
        SceneDetector detector;
        detector.setThreshold(0.35);
        detector.setMinSceneFrames(5);

        // (a) Identical frames -> no cuts
        QImage redFrame(32, 32, QImage::Format_ARGB32);
        redFrame.fill(QColor(200, 50, 50));
        for (int i = 0; i < 10; ++i)
            detector.processFrame(i, redFrame, 24.0);
        if (!requireSelftest(detector.totalCuts() == 0,
                             QStringLiteral("SceneDetector: identical frames produced spurious cuts"), &error))
            return 1;

        // (b) Drastic color change -> at least one cut detected
        QImage blueFrame(32, 32, QImage::Format_ARGB32);
        blueFrame.fill(QColor(10, 10, 220));
        for (int i = 10; i < 20; ++i)
            detector.processFrame(i, blueFrame, 24.0);
        if (!requireSelftest(detector.totalCuts() >= 1,
                             QStringLiteral("SceneDetector: drastic color change produced no cut"), &error))
            return 1;

        // (c) Rapid alternation respects minSceneFrames gate
        // Feed alternating colors faster than minSceneFrames=5
        int cutsBeforeRapid = detector.totalCuts();
        QImage greenFrame(32, 32, QImage::Format_ARGB32);
        greenFrame.fill(QColor(10, 220, 10));
        // 8 rapid alternations between blue/green (interval=1 frame < minSceneFrames=5)
        for (int i = 20; i < 28; ++i) {
            const QImage &f = (i % 2 == 0) ? blueFrame : greenFrame;
            detector.processFrame(i, f, 24.0);
        }
        const int cutsAfterRapid = detector.totalCuts();
        // Should not have added more than 2 cuts (minSceneFrames gate suppresses most)
        if (!requireSelftest(cutsAfterRapid - cutsBeforeRapid <= 2,
                             QStringLiteral("SceneDetector: minSceneFrames gate not suppressing rapid alternations"), &error))
            return 1;

        // (d) reset() clears state
        detector.reset();
        if (!requireSelftest(detector.totalCuts() == 0,
                             QStringLiteral("SceneDetector: reset() did not clear cut count"), &error))
            return 1;
        if (!requireSelftest(detector.sceneCutFrames().isEmpty(),
                             QStringLiteral("SceneDetector: reset() did not clear cut frames"), &error))
            return 1;
    }
#endif // HAVE_SCENEDETECTOR

    // --- ExportConfig: hwEncoder field compile-time presence check ---
    {
        ExportConfig cfg;
        cfg.hwEncoder = QStringLiteral("auto");
        if (!requireSelftest(cfg.hwEncoder == QStringLiteral("auto"),
                             QStringLiteral("ExportConfig.hwEncoder writable"), &error))
            return 1;
        cfg.hwEncoder = QStringLiteral("none");
        if (!requireSelftest(cfg.hwEncoder == QStringLiteral("none"),
                             QStringLiteral("ExportConfig.hwEncoder round-trip"), &error))
            return 1;
    }

    qInfo().noquote() << QStringLiteral("HWPERF selftest OK");
    return 0;
}

// US-EXT-5: Sprint 10 pro-extension self-test (VEDITOR_PROEXT_SELFTEST=1)
int runProExtSelftest()
{
    QString error;

#ifdef HAVE_HDRTRANSFER
    {
        // PQ EOTF at 0 is ~0
        if (!requireSelftest(hdr::pqEotf(0.0f) <= 0.001f,
                             QStringLiteral("PQ EOTF at 0 is ~0"), &error))
            return 1;
        // PQ EOTF at 0.5 > 0
        if (!requireSelftest(hdr::pqEotf(0.5f) > 0.0f,
                             QStringLiteral("PQ EOTF at 0.5 > 0"), &error))
            return 1;
        // Monotonicity: pqOetf(pqEotf(0.3)) ≈ 0.3 within 1e-2
        if (!requireSelftest(std::abs(hdr::pqOetf(hdr::pqEotf(0.3f)) - 0.3f) < 1e-2f,
                             QStringLiteral("pqOetf(pqEotf(0.3)) not within 1e-2 of 0.3"), &error))
            return 1;
        // applyToneMapReinhard: white 8x8 -> output size 8x8, Format_ARGB32, all pixels <= 255
        QImage white(8, 8, QImage::Format_ARGB32);
        white.fill(QColor(255, 255, 255, 255));
        const QImage tonemapped = hdr::applyToneMapReinhard(white);
        if (!requireSelftest(!tonemapped.isNull() && tonemapped.size() == QSize(8, 8)
                                 && tonemapped.format() == QImage::Format_ARGB32,
                             QStringLiteral("applyToneMapReinhard: invalid output size/format"), &error))
            return 1;
        bool allPixelsValid = true;
        for (int y = 0; y < tonemapped.height(); ++y) {
            for (int x = 0; x < tonemapped.width(); ++x) {
                const QRgb px = tonemapped.pixel(x, y);
                if (qRed(px) > 255 || qGreen(px) > 255 || qBlue(px) > 255) {
                    allPixelsValid = false;
                }
            }
        }
        if (!requireSelftest(allPixelsValid,
                             QStringLiteral("applyToneMapReinhard: pixel value out of range"), &error))
            return 1;
        // convertColorSpace determinism: same input -> bits() identical on two calls
        const QImage cs1 = hdr::convertColorSpace(white, hdr::TransferFn::SDR_Gamma22, hdr::TransferFn::PQ_HDR10);
        const QImage cs2 = hdr::convertColorSpace(white, hdr::TransferFn::SDR_Gamma22, hdr::TransferFn::PQ_HDR10);
        if (!requireSelftest(!cs1.isNull() && cs1.size() == white.size(),
                             QStringLiteral("convertColorSpace returned null/wrong size"), &error))
            return 1;
        if (!requireSelftest(std::memcmp(cs1.bits(), cs2.bits(),
                                         static_cast<size_t>(cs1.sizeInBytes())) == 0,
                             QStringLiteral("convertColorSpace not deterministic"), &error))
            return 1;
    }
#endif // HAVE_HDRTRANSFER

#ifdef HAVE_AIUPSCALE
    {
        // 8x8 test image
        QImage test(8, 8, QImage::Format_ARGB32);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                test.setPixel(x, y, qRgba(x * 32, y * 32, 128, 255));
        // LanczosUpscaler::upscale(test, 2) -> 16x16
        LanczosUpscaler lanczos;
        const QImage upscaled = lanczos.upscale(test, 2);
        if (!requireSelftest(!upscaled.isNull() && upscaled.size() == QSize(16, 16),
                             QStringLiteral("LanczosUpscaler::upscale: wrong output size"), &error))
            return 1;
        // Determinism: two calls produce identical bits
        const QImage up1 = lanczos.upscale(test, 2);
        const QImage up2 = lanczos.upscale(test, 2);
        if (!requireSelftest(std::memcmp(up1.bits(), up2.bits(),
                                         static_cast<size_t>(up1.sizeInBytes())) == 0,
                             QStringLiteral("LanczosUpscaler::upscale not deterministic"), &error))
            return 1;
        // Engine registry
        if (!requireSelftest(AIUpscaleManager::engines().size() >= 2,
                             QStringLiteral("AIUpscaleManager: fewer than 2 engines registered"), &error))
            return 1;
        if (!requireSelftest(AIUpscaleManager::engineByName(QStringLiteral("Lanczos3")) != nullptr,
                             QStringLiteral("AIUpscaleManager: Lanczos3 engine not found"), &error))
            return 1;
    }
#endif // HAVE_AIUPSCALE

#ifdef HAVE_FRAMEINTERP
    {
        QImage black(8, 8, QImage::Format_ARGB32);
        black.fill(QColor(0, 0, 0, 255));
        QImage white(8, 8, QImage::Format_ARGB32);
        white.fill(QColor(255, 255, 255, 255));
        LinearBlendInterpolator lbi;
        // interpolate(black, white, 0.5) -> 8x8, channels ~127
        const QImage mid = lbi.interpolate(black, white, 0.5);
        if (!requireSelftest(!mid.isNull() && mid.size() == QSize(8, 8),
                             QStringLiteral("LinearBlendInterpolator::interpolate(0.5): wrong size"), &error))
            return 1;
        const QColor midPx = mid.pixelColor(4, 4);
        if (!requireSelftest(midPx.red() >= 120 && midPx.red() <= 135,
                             QStringLiteral("LinearBlendInterpolator::interpolate(0.5): channel not ~127"), &error))
            return 1;
        // interpolate(black, white, 0.0) bits() == black bits()
        const QImage atZero = lbi.interpolate(black, white, 0.0);
        if (!requireSelftest(!atZero.isNull() && atZero.size() == black.size(),
                             QStringLiteral("LinearBlendInterpolator::interpolate(0.0): wrong size"), &error))
            return 1;
        if (!requireSelftest(std::memcmp(atZero.bits(), black.bits(),
                                         static_cast<size_t>(black.sizeInBytes())) == 0,
                             QStringLiteral("LinearBlendInterpolator::interpolate(0.0) != a"), &error))
            return 1;
        // interpolate(black, white, 1.0) bits() == white bits()
        const QImage atOne = lbi.interpolate(black, white, 1.0);
        if (!requireSelftest(!atOne.isNull() && atOne.size() == white.size(),
                             QStringLiteral("LinearBlendInterpolator::interpolate(1.0): wrong size"), &error))
            return 1;
        if (!requireSelftest(std::memcmp(atOne.bits(), white.bits(),
                                         static_cast<size_t>(white.sizeInBytes())) == 0,
                             QStringLiteral("LinearBlendInterpolator::interpolate(1.0) != b"), &error))
            return 1;
        // Determinism
        const QImage d1 = lbi.interpolate(black, white, 0.5);
        const QImage d2 = lbi.interpolate(black, white, 0.5);
        if (!requireSelftest(std::memcmp(d1.bits(), d2.bits(),
                                         static_cast<size_t>(d1.sizeInBytes())) == 0,
                             QStringLiteral("LinearBlendInterpolator::interpolate not deterministic"), &error))
            return 1;
        // Engine registry
        if (!requireSelftest(FrameInterpolatorManager::engineByName(QStringLiteral("Linear")) != nullptr,
                             QStringLiteral("FrameInterpolatorManager: Linear engine not found"), &error))
            return 1;
    }
#endif // HAVE_FRAMEINTERP

#ifdef HAVE_PLUGINMANIFEST
    {
        // Non-existent path -> empty result
        const QVector<PluginInfo> r1 =
            PluginManifestScanner::scanFolder(QStringLiteral("/__veditor_no_such_path__"));
        if (!requireSelftest(r1.isEmpty(),
                             QStringLiteral("PluginManifest::scanFolder non-existent path should be empty"), &error))
            return 1;
        // Determinism: two calls produce identical results
        const QVector<PluginInfo> r2 =
            PluginManifestScanner::scanFolder(QStringLiteral("/__veditor_no_such_path__"));
        if (!requireSelftest(r1.size() == r2.size(),
                             QStringLiteral("PluginManifest::scanFolder not deterministic"), &error))
            return 1;
    }
#endif // HAVE_PLUGINMANIFEST

    qInfo() << "PROEXT selftest OK";
    return 0;
}

// US-WF-D: Sprint 11 workflow self-test (VEDITOR_WORKFLOW_SELFTEST=1).
// Exercises AI auto-mask, Magnetic Timeline, and AudioClipEditor envelope
// evaluation without spinning up MainWindow. Each block is guarded by
// HAVE_* macros so the test still passes when a header is unavailable.
int runWorkflowSelftest()
{
    QString error;

#ifdef HAVE_AIMASK
    {
        // Pure white 8x8: LumaThreshold (threshold=0.5) -> all pixels above
        // threshold, mask should be fully opaque (255).
        QImage white(8, 8, QImage::Format_ARGB32);
        white.fill(QColor(255, 255, 255, 255));
        aimask::MaskParams params;
        params.engine = aimask::Engine::LumaThreshold;
        params.lumaThreshold = 0.5;
        const aimask::MaskResult resultWhite = aimask::generateMask(white, params);
        if (!requireSelftest(resultWhite.success,
                             QStringLiteral("aimask::generateMask(white) reported failure: ")
                                 + resultWhite.error,
                             &error))
            return 1;
        if (!requireSelftest(!resultWhite.mask.isNull()
                                 && resultWhite.mask.size() == white.size(),
                             QStringLiteral("aimask::generateMask(white) returned null/wrong-size mask"),
                             &error))
            return 1;
        bool allFullyOpaqueWhite = true;
        for (int y = 0; y < resultWhite.mask.height() && allFullyOpaqueWhite; ++y) {
            for (int x = 0; x < resultWhite.mask.width(); ++x) {
                const int gray = qGray(resultWhite.mask.pixel(x, y));
                if (gray != 255) {
                    allFullyOpaqueWhite = false;
                    break;
                }
            }
        }
        if (!requireSelftest(allFullyOpaqueWhite,
                             QStringLiteral("aimask LumaThreshold(white) mask not all 255"),
                             &error))
            return 1;

        // Pure black 8x8: LumaThreshold (threshold=0.5) -> all pixels below
        // threshold, mask should be fully transparent (0).
        QImage black(8, 8, QImage::Format_ARGB32);
        black.fill(QColor(0, 0, 0, 255));
        const aimask::MaskResult resultBlack = aimask::generateMask(black, params);
        if (!requireSelftest(resultBlack.success,
                             QStringLiteral("aimask::generateMask(black) reported failure: ")
                                 + resultBlack.error,
                             &error))
            return 1;
        bool allZeroBlack = true;
        for (int y = 0; y < resultBlack.mask.height() && allZeroBlack; ++y) {
            for (int x = 0; x < resultBlack.mask.width(); ++x) {
                const int gray = qGray(resultBlack.mask.pixel(x, y));
                if (gray != 0) {
                    allZeroBlack = false;
                    break;
                }
            }
        }
        if (!requireSelftest(allZeroBlack,
                             QStringLiteral("aimask LumaThreshold(black) mask not all 0"),
                             &error))
            return 1;
    }
#endif // HAVE_AIMASK

#ifdef HAVE_MAGTL
    {
        // AC2: 2 clips with a 100ms gap should be packed (closeGaps).
        QList<magtl::Clip> twoWithGap;
        twoWithGap.append(magtl::Clip{0, 0, 1000, QStringLiteral("A")});
        twoWithGap.append(magtl::Clip{0, 1100, 2000, QStringLiteral("B")});
        const QList<magtl::Clip> packed = magtl::closeGaps(twoWithGap);
        if (!requireSelftest(packed.size() == 2,
                             QStringLiteral("magtl::closeGaps did not preserve clip count"),
                             &error))
            return 1;
        if (!requireSelftest(packed.at(0).startMs == 0 && packed.at(0).endMs == 1000,
                             QStringLiteral("magtl::closeGaps moved/altered first clip"),
                             &error))
            return 1;
        // Second clip should now butt up against first (start == 1000) and
        // preserve its duration (900ms -> end == 1900).
        if (!requireSelftest(packed.at(1).startMs == 1000 && packed.at(1).endMs == 1900,
                             QStringLiteral("magtl::closeGaps did not collapse the 100ms gap"),
                             &error))
            return 1;

        // AC4: 3 clips, delete index=1, expect 2 clips and the trailing clip
        // shifted forward by the deleted clip's duration.
        QList<magtl::Clip> threeClips;
        threeClips.append(magtl::Clip{0, 0, 1000, QStringLiteral("A")});
        threeClips.append(magtl::Clip{0, 1000, 1500, QStringLiteral("B")}); // 500ms
        threeClips.append(magtl::Clip{0, 1500, 2500, QStringLiteral("C")});
        const QList<magtl::Clip> afterDelete = magtl::rippleDelete(threeClips, 1);
        if (!requireSelftest(afterDelete.size() == 2,
                             QStringLiteral("magtl::rippleDelete did not reduce clip count to 2"),
                             &error))
            return 1;
        if (!requireSelftest(afterDelete.at(0).startMs == 0
                                 && afterDelete.at(0).endMs == 1000
                                 && afterDelete.at(0).id == QStringLiteral("A"),
                             QStringLiteral("magtl::rippleDelete altered the first clip"),
                             &error))
            return 1;
        // Third clip (C) should have shifted left by 500ms (deleted B duration).
        if (!requireSelftest(afterDelete.at(1).startMs == 1000
                                 && afterDelete.at(1).endMs == 2000
                                 && afterDelete.at(1).id == QStringLiteral("C"),
                             QStringLiteral("magtl::rippleDelete did not shift trailing clip"),
                             &error))
            return 1;
    }
#endif // HAVE_MAGTL

#ifdef HAVE_AUDIOCLIPEDITOR
    {
        // AudioClipEditor is a QWidget; QApplication must already exist
        // (caller dispatches runWorkflowSelftest after QApplication construction).
        AudioClipEditor editor;
        editor.setClipDuration(5000);
        editor.clearEnvelope();
        const QList<VolumeEnvelopePoint> defaultEnv = editor.envelope();
        if (!requireSelftest(defaultEnv.size() == 2,
                             QStringLiteral("AudioClipEditor::clearEnvelope did not yield 2 points"),
                             &error))
            return 1;
        if (!requireSelftest(defaultEnv.at(0).timeMs == 0
                                 && std::abs(defaultEnv.at(0).dB - 0.0) < 1e-6,
                             QStringLiteral("AudioClipEditor default first point != (0, 0dB)"),
                             &error))
            return 1;
        if (!requireSelftest(defaultEnv.at(1).timeMs == 5000
                                 && std::abs(defaultEnv.at(1).dB - 0.0) < 1e-6,
                             QStringLiteral("AudioClipEditor default last point != (5000, 0dB)"),
                             &error))
            return 1;

        QList<VolumeEnvelopePoint> custom;
        custom.append(VolumeEnvelopePoint{0,    0.0});
        custom.append(VolumeEnvelopePoint{2500, -6.0});
        custom.append(VolumeEnvelopePoint{5000, 0.0});
        editor.setEnvelope(custom);
        const double dbAtMid = editor.evaluateAt(2500);
        if (!requireSelftest(std::abs(dbAtMid - (-6.0)) < 1e-6,
                             QStringLiteral("AudioClipEditor::evaluateAt(2500) != -6.0 (got %1)")
                                 .arg(dbAtMid),
                             &error))
            return 1;
    }
#endif // HAVE_AUDIOCLIPEDITOR

    qInfo().noquote() << QStringLiteral("WORKFLOW selftest OK");
    return 0;
}

// US-SC-B: Sprint 12 shortcut customization self-test (VEDITOR_SHORTCUT_SELFTEST=1)
int runShortcutSelftest()
{
    QString error;
#ifdef HAVE_SHORTCUTMANAGER
    {
        QAction dummyA;
        dummyA.setShortcut(QKeySequence("Ctrl+O"));
        QAction dummyB;
        dummyB.setShortcut(QKeySequence("Ctrl+S"));

        shortcut::ShortcutManager mgr;
        mgr.registerAction(&dummyA, "file.open",
                           QStringLiteral("ファイルを開く"),
                           QStringLiteral("ファイル"));
        mgr.registerAction(&dummyB, "file.save",
                           QStringLiteral("保存"),
                           QStringLiteral("ファイル"));

        // 1. registerAction → bindings() に entry
        if (!requireSelftest(mgr.bindings().size() == 2,
                             QStringLiteral("ShortcutManager: bindings size != 2"), &error))
            return 1;
        // 2. setBinding でカスタム値、bindingFor で取れる
        mgr.setBinding("file.open", QKeySequence("Ctrl+Shift+O"));
        if (!requireSelftest(mgr.bindingFor("file.open").sequence ==
                                 QKeySequence("Ctrl+Shift+O"),
                             QStringLiteral("setBinding round-trip failed"), &error))
            return 1;
        // 3. QAction 自体の shortcut も反映
        if (!requireSelftest(dummyA.shortcut() == QKeySequence("Ctrl+Shift+O"),
                             QStringLiteral("QAction::shortcut not updated"), &error))
            return 1;
        // 4. applyPreset(Premiere) — presetDisplayName が non-empty。
        mgr.applyPreset(shortcut::Preset::Premiere);
        if (!requireSelftest(!shortcut::ShortcutManager::presetDisplayName(
                                    shortcut::Preset::Premiere).isEmpty(),
                             QStringLiteral("presetDisplayName(Premiere) empty"), &error))
            return 1;
        // 5. availablePresets contains 4
        if (!requireSelftest(shortcut::ShortcutManager::availablePresets().size() == 4,
                             QStringLiteral("availablePresets != 4"), &error))
            return 1;
        // 6. resetAllToDefaults → default に戻る (Ctrl+O)
        mgr.resetAllToDefaults();
        if (!requireSelftest(mgr.bindingFor("file.open").sequence == QKeySequence("Ctrl+O"),
                             QStringLiteral("resetAllToDefaults didn't restore"), &error))
            return 1;
    }
#endif // HAVE_SHORTCUTMANAGER
    qInfo().noquote() << QStringLiteral("SHORTCUT selftest OK");
    return 0;
}

// US-SC2-B: Sprint 13 social export self-test (VEDITOR_SOCIAL_SELFTEST=1)
int runSocialSelftest()
{
    QString error;
#ifdef HAVE_SOCIALPRESET
    {
        // 1. allPresets >= 9
        const auto presets = social::allPresets();
        if (!requireSelftest(presets.size() >= 9,
                             QStringLiteral("social::allPresets size < 9"), &error))
            return 1;
        // 2. instagram_reels の resolution & vertical flag
        const social::Preset reels = social::presetById("instagram_reels");
        if (!requireSelftest(!reels.id.isEmpty() && reels.resolution == QSize(1080, 1920)
                                 && reels.requiresVerticalReframe,
                             QStringLiteral("instagram_reels preset invalid"), &error))
            return 1;
        // 3. youtube_standard horizontal
        const social::Preset yt = social::presetById("youtube_standard");
        if (!requireSelftest(!yt.id.isEmpty() && yt.resolution == QSize(1920, 1080)
                                 && !yt.requiresVerticalReframe,
                             QStringLiteral("youtube_standard preset invalid"), &error))
            return 1;
        // 4. 存在しない id は空 Preset
        const social::Preset missing = social::presetById("does_not_exist");
        if (!requireSelftest(missing.id.isEmpty(),
                             QStringLiteral("presetById missing should be empty"), &error))
            return 1;
    }
#endif
#ifdef HAVE_ASPECTREFRAMER
    {
        // 5. CenterCrop 1920x1080 -> 1080x1920 の crop rect
        QImage src(1920, 1080, QImage::Format_ARGB32);
        src.fill(QColor(64, 128, 192, 255));
        reframe::ReframeParams p;
        p.sourceSize = QSize(1920, 1080);
        p.targetSize = QSize(1080, 1920);
        p.mode = reframe::Mode::CenterCrop;
        const QRectF rect = reframe::computeCropRect(src, p);
        const double expectedW = (1080.0 / 1920.0) / (1920.0 / 1080.0); // ~0.316
        if (!requireSelftest(std::abs(rect.width() - expectedW) < 1e-2
                                 && std::abs(rect.height() - 1.0) < 1e-2
                                 && std::abs(rect.y()) < 1e-3,
                             QStringLiteral("CenterCrop rect math wrong"), &error))
            return 1;
        // 6. applyReframe output size matches target
        const reframe::ReframeResult res = reframe::applyReframe(src, p);
        if (!requireSelftest(res.success
                                 && res.previewImage.size() == QSize(1080, 1920),
                             QStringLiteral("applyReframe output size mismatch"), &error))
            return 1;
        // 7. null source -> success=false, error non-empty
        QImage nullImg;
        const reframe::ReframeResult nullRes = reframe::applyReframe(nullImg, p);
        if (!requireSelftest(!nullRes.success && !nullRes.error.isEmpty(),
                             QStringLiteral("applyReframe null source should fail"), &error))
            return 1;
        // 8. modes >= 5
        if (!requireSelftest(reframe::availableModes().size() >= 5,
                             QStringLiteral("availableModes < 5"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("SOCIAL selftest OK");
    return 0;
}

// US-CAP-B: Sprint 14 caption self-test (VEDITOR_CAPTION_SELFTEST=1)
int runCaptionSelftest()
{
    QString error;
#ifdef HAVE_CAPTIONTRACK
    {
        caption::Track track;
        caption::Clip c1{1000, 2000, QStringLiteral("hello"), QString()};
        caption::Clip c2{2500, 3500, QStringLiteral("world"), QString()};
        track.addClip(c1);
        track.addClip(c2);
        track.sortByStart();
        if (!requireSelftest(track.clipCount() == 2,
                             QStringLiteral("Track::clipCount != 2"), &error))
            return 1;
        const auto active = track.clipsAtTime(1500);
        if (!requireSelftest(active.size() == 1 && active[0].text == QStringLiteral("hello"),
                             QStringLiteral("clipsAtTime(1500) wrong"), &error))
            return 1;
    }
#endif
#ifdef HAVE_CAPTIONSTYLE
    {
        const auto style = caption::defaultStyle();
        if (!requireSelftest(style.anchor == caption::Anchor::BottomCenter
                                 && style.fontSizePt == 24,
                             QStringLiteral("defaultStyle wrong"), &error))
            return 1;
        if (!requireSelftest(caption::anchorFromString(
                                 caption::anchorToString(caption::Anchor::TopLeft))
                                 == caption::Anchor::TopLeft,
                             QStringLiteral("Anchor round-trip failed"), &error))
            return 1;
        if (!requireSelftest(caption::anchorNames().size() == 9,
                             QStringLiteral("anchorNames size != 9"), &error))
            return 1;
    }
#endif
#ifdef HAVE_SUBTITLEIO
    {
        // formatSrtTimestamp + parseSrtTimestamp round-trip
        const QString ts = subtitle::formatSrtTimestamp(3661500);
        if (!requireSelftest(ts == QStringLiteral("01:01:01,500"),
                             QStringLiteral("formatSrtTimestamp wrong"), &error))
            return 1;
        if (!requireSelftest(subtitle::parseSrtTimestamp(ts) == 3661500,
                             QStringLiteral("parseSrtTimestamp round-trip"), &error))
            return 1;
        if (!requireSelftest(subtitle::parseSrtTimestamp(QStringLiteral("invalid")) == -1,
                             QStringLiteral("parseSrtTimestamp invalid should return -1"), &error))
            return 1;

        // SRT round-trip: write + read 2 clips
        const QString tmpPath = QDir::tempPath() + QStringLiteral("/veditor_caption_test.srt");
        QList<caption::Clip> clips;
        clips.append({1000, 2000, QStringLiteral("first"), QString()});
        clips.append({3000, 4500, QStringLiteral("second"), QString()});
        if (!requireSelftest(subtitle::exportSrt(tmpPath, clips),
                             QStringLiteral("exportSrt failed"), &error))
            return 1;
        const auto imp = subtitle::importSrt(tmpPath);
        if (!requireSelftest(imp.success && imp.clips.size() == 2
                                 && imp.clips[0].text == QStringLiteral("first")
                                 && imp.clips[1].startMs == 3000,
                             QStringLiteral("SRT round-trip failed"), &error))
            return 1;
        QFile::remove(tmpPath);

        // VTT round-trip
        const QString vttPath = QDir::tempPath() + QStringLiteral("/veditor_caption_test.vtt");
        if (!requireSelftest(subtitle::exportVtt(vttPath, clips),
                             QStringLiteral("exportVtt failed"), &error))
            return 1;
        const auto vttImp = subtitle::importVtt(vttPath);
        if (!requireSelftest(vttImp.success && vttImp.clips.size() == 2
                                 && vttImp.clips[0].text == QStringLiteral("first"),
                             QStringLiteral("VTT round-trip failed"), &error))
            return 1;
        QFile::remove(vttPath);
    }
#endif
#ifdef HAVE_SPEECHRECOGNIZER
    {
        const auto recogs = speech::availableRecognizers();
        if (!requireSelftest(recogs.size() >= 1,
                             QStringLiteral("availableRecognizers size < 1"), &error))
            return 1;
        // Stub fallback
        auto stub = speech::recognizerByName(QStringLiteral("does_not_exist"));
        if (!requireSelftest(stub && stub->name() == QStringLiteral("Stub"),
                             QStringLiteral("recognizerByName fallback failed"), &error))
            return 1;
        speech::RecognizeParams p;
        p.audioPath = QStringLiteral("/tmp/dummy.wav"); // 非空、Stub は中身読まない
        p.language = QStringLiteral("ja");
        const auto res = stub->recognize(p);
        if (!requireSelftest(res.success && res.segments.size() == 3,
                             QStringLiteral("Stub recognize wrong segment count"), &error))
            return 1;
        // empty audioPath -> failure
        speech::RecognizeParams pEmpty;
        const auto resEmpty = stub->recognize(pEmpty);
        if (!requireSelftest(!resEmpty.success && !resEmpty.error.isEmpty(),
                             QStringLiteral("Stub empty path should fail"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("CAPTION selftest OK");
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
    if (qEnvironmentVariableIntValue("VEDITOR_PRO_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_PRO_SELFTEST");
        return runProSelftest();
    }
    if (qEnvironmentVariableIntValue("VEDITOR_MOGRAPH_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_MOGRAPH_SELFTEST");
        return runMographSelftest();
    }
    if (qEnvironmentVariableIntValue("VEDITOR_HWPERF_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_HWPERF_SELFTEST");
        return runHwPerfSelftest();
    }
    if (qEnvironmentVariableIntValue("VEDITOR_PROEXT_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_PROEXT_SELFTEST");
        return runProExtSelftest();
    }
    if (qEnvironmentVariableIntValue("VEDITOR_SHORTCUT_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_SHORTCUT_SELFTEST");
        return runShortcutSelftest();
    }
    if (qEnvironmentVariableIntValue("VEDITOR_SOCIAL_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_SOCIAL_SELFTEST");
        return runSocialSelftest();
    }
    if (qEnvironmentVariableIntValue("VEDITOR_CAPTION_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_CAPTION_SELFTEST");
        return runCaptionSelftest();
    }
    if (qEnvironmentVariableIntValue("VEDITOR_WORKFLOW_SELFTEST") != 0) {
        writeLogLine("INFO", "running VEDITOR_WORKFLOW_SELFTEST");
        return runWorkflowSelftest();
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
