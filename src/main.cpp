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
