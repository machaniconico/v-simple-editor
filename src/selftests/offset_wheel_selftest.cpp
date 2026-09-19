#include "../VideoEffect.h"
#include "../GLPreview.h"
#include "../ColorGradingPanel.h"
#include "../ColorWheelWidget.h"
#include "../ProjectFile.h"
#include "../EffectPreset.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpToolRegistry.h"
#include <QApplication>
#include <QEventLoop>
#include <QOpenGLContext>
#include <QJsonDocument>
#include <QTimer>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
bool pixelsEqual(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()) return false;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), a.width()*a.depth()/8)) return false;
    return true;
}
// Compare RGB or signed RGB deltas, retaining the first out-of-tolerance
// coordinate and original samples so failures distinguish setup from math.
bool compareRgb(const char *label, const QImage &cpu, const QImage &gpu,
                const QImage *cpuBase = nullptr, const QImage *gpuBase = nullptr,
                int tolerance = 1)
{
    if (cpu.isNull() || gpu.isNull() || cpu.size() != gpu.size()
        || (cpuBase && (cpuBase->isNull() || cpuBase->size() != cpu.size()))
        || (gpuBase && (gpuBase->isNull() || gpuBase->size() != gpu.size()))) {
        std::fprintf(stderr, "G3 %s: invalid readback/size CPU=%dx%d GPU=%dx%d\n",
            label, cpu.width(), cpu.height(), gpu.width(), gpu.height());
        return false;
    }
    double error = 0;
    int maxError = 0, firstX = -1, firstY = -1;
    for (int y = 0; y < cpu.height(); ++y) for (int x = 0; x < cpu.width(); ++x) {
        const QColor a = cpu.pixelColor(x,y), b = gpu.pixelColor(x,y);
        const QColor a0 = cpuBase ? cpuBase->pixelColor(x,y) : QColor(0,0,0);
        const QColor b0 = gpuBase ? gpuBase->pixelColor(x,y) : QColor(0,0,0);
        for (int d : {std::abs((a.red()-a0.red())-(b.red()-b0.red())),
                      std::abs((a.green()-a0.green())-(b.green()-b0.green())),
                      std::abs((a.blue()-a0.blue())-(b.blue()-b0.blue()))}) {
            error += d;
            maxError = qMax(maxError,d);
            if (d > tolerance && firstX < 0) { firstX = x; firstY = y; }
        }
    }
    const double meanError = error/(double(cpu.width())*cpu.height()*3);
    const bool ok = meanError <= tolerance && maxError <= tolerance;
    std::fprintf(stderr, "G3 %s: %s mean=%.6f max=%d tolerance=%d\n",
        label, ok ? "OK" : "MISMATCH", meanError, maxError, tolerance);
    if (firstX >= 0) {
        const QColor a = cpu.pixelColor(firstX,firstY), b = gpu.pixelColor(firstX,firstY);
        std::fprintf(stderr, "G3 %s: first=(%d,%d) CPU=(%d,%d,%d) GPU=(%d,%d,%d)\n",
            label, firstX, firstY, a.red(), a.green(), a.blue(), b.red(), b.green(), b.blue());
        if (cpuBase && gpuBase) {
            const QColor a0 = cpuBase->pixelColor(firstX,firstY), b0 = gpuBase->pixelColor(firstX,firstY);
            std::fprintf(stderr, "G3 %s: base CPU=(%d,%d,%d) GPU=(%d,%d,%d) delta CPU=(%d,%d,%d) GPU=(%d,%d,%d)\n",
                label, a0.red(), a0.green(), a0.blue(), b0.red(), b0.green(), b0.blue(),
                a.red()-a0.red(), a.green()-a0.green(), a.blue()-a0.blue(),
                b.red()-b0.red(), b.green()-b0.green(), b.blue()-b0.blue());
        }
    }
    return ok;
}
bool sameOffset(const ColorCorrection &a, const ColorCorrection &b)
{
    return std::abs(a.offsetR-b.offsetR) < 1e-6
        && std::abs(a.offsetG-b.offsetG) < 1e-6
        && std::abs(a.offsetB-b.offsetB) < 1e-6;
}
}

int runOffsetWheelSelftest()
{
    int passed = 0, failed = 0;
    auto check = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    QImage input(64, 64, QImage::Format_RGB888);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            input.setPixelColor(x,y,QColor(30+x*3, 35+y*3, 40+x+y));
    // Neutral grading must preserve the source bytes. Cross-build render-fixture
    // comparison covers the Wave baseline; no test switch runs in production.
    check(1, pixelsEqual(input, VideoEffectProcessor::applyColorCorrection(input, ColorCorrection{})));

    ColorCorrection cc;
    cc.offsetR = 0.125; cc.offsetG = -0.125; cc.offsetB = 0.0625;
    const auto cpu = VideoEffectProcessor::applyColorCorrection(input,cc);
    bool shift = !cc.isDefault();
    for (int x : {0, 63}) {
        const QColor a = input.pixelColor(x,x), b = cpu.pixelColor(x,x);
        shift &= std::abs(b.red()-a.red()-0.0625*255) <= 1
            && std::abs(b.green()-a.green()+0.0625*255) <= 1
            && std::abs(b.blue()-a.blue()-0.03125*255) <= 1;
    }
    check(2, shift);

    // Exercise the production shader. Never resize a readback: create the
    // source at the physical framebuffer size, including fractional-DPI setups.
    GLPreview preview;
    preview.setFixedSize(64,64);
    preview.show();
    QApplication::processEvents();
    preview.displayFrame(input);
    const QImage probe = preview.grabFramebuffer();
    const bool contextOk = preview.context() && preview.context()->isValid() && !probe.isNull();
    bool offsetOk = false, neutralOk = false;
    if (contextOk) {
        QImage source(probe.size(), QImage::Format_RGB888);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor(40 + 160*x/qMax(1,source.width()-1),
                    40 + 160*y/qMax(1,source.height()-1),
                    50 + 120*(x+y)/qMax(1,source.width()+source.height()-2)));
        preview.displayFrame(source);
        auto renderGpu = [&](const ColorCorrection &grade) {
            preview.setEffectsEnabled(true);
            preview.setColorCorrection(grade);
            // setColorCorrection does not populate the separate LGG uniforms.
            const std::array<std::array<double,4>,3> wheels = {{
                {grade.liftR, grade.liftG, grade.liftB, 0.0},
                {std::pow(2.0,grade.gammaR), std::pow(2.0,grade.gammaG), std::pow(2.0,grade.gammaB), 1.0},
                {grade.gainR, grade.gainG, grade.gainB, 0.0}
            }};
            preview.setLiftGammaGain(wheels);
            preview.setOffset({grade.offsetR,grade.offsetG,grade.offsetB});
            return preview.grabFramebuffer().convertToFormat(QImage::Format_RGB888);
        };
        const auto cpuNeutral = VideoEffectProcessor::applyColorCorrection(source, ColorCorrection{});
        const auto gpuNeutral = renderGpu(ColorCorrection{});
        const bool controlA = compareRgb("control-A neutral", cpuNeutral, gpuNeutral);
        ColorCorrection lift;
        lift.liftR = 0.125;
        const auto cpuLift = VideoEffectProcessor::applyColorCorrection(source, lift);
        const auto gpuLift = renderGpu(lift);
        const bool controlB = compareRgb("control-B lift", cpuLift, gpuLift);
        const auto cpuOffset = VideoEffectProcessor::applyColorCorrection(source, cc);
        const auto gpuOffset = renderGpu(cc);
        const bool directOffset = compareRgb("offset direct", cpuOffset, gpuOffset);
        // If the full preview pipeline differs even without offset, isolate
        // the signed change on each backend. Also require the lift control's
        // change to agree. No resampling, tolerance widening or GLSL replica.
        const bool liftDelta = compareRgb("control-B delta", cpuLift, gpuLift, &cpuNeutral, &gpuNeutral);
        const bool offsetDelta = compareRgb("offset delta", cpuOffset, gpuOffset, &cpuNeutral, &gpuNeutral);
        std::fprintf(stderr, "G3: comparison=%s controlA=%d controlB=%d direct=%d liftDelta=%d offsetDelta=%d DPR=%.3f framebuffer=%dx%d\n",
            controlA && controlB ? "direct+delta" : "delta (controls differ)",
            int(controlA), int(controlB), int(directOffset), int(liftDelta), int(offsetDelta),
            double(preview.devicePixelRatioF()), probe.width(), probe.height());
        offsetOk = liftDelta && offsetDelta && (!(controlA && controlB) || directOffset);
        const auto neutral = renderGpu(ColorCorrection{});
        preview.setEffectsEnabled(false);
        const auto bypass = preview.grabFramebuffer().convertToFormat(QImage::Format_RGB888);
        neutralOk = compareRgb("neutral vs bypass", neutral, bypass, nullptr, nullptr, 0);
    }
    if (!contextOk || !offsetOk || !neutralOk)
        std::fprintf(stderr, "G3: context/readback=%d offset-parity=%d neutral-bypass=%d\n",
            int(contextOk), int(offsetOk), int(neutralOk));
    preview.hide();
    check(3, contextOk && offsetOk && neutralOk);

    ClipInfo clip;
    clip.filePath = QStringLiteral("offset-wheel.mp4");
    clip.duration = clip.outPoint = 1;
    clip.colorCorrection.brightness = 1;
    ProjectData data, restored;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    const QString defaults = ProjectFile::toJsonString(data);
    bool roundtrip = !defaults.contains(QStringLiteral("offsetR"))
        && !defaults.contains(QStringLiteral("offsetG")) && !defaults.contains(QStringLiteral("offsetB"));
    clip.colorCorrection = cc;
    data.videoTracks[0][0] = clip;
    roundtrip &= ProjectFile::fromJsonString(ProjectFile::toJsonString(data),restored)
        && !restored.videoTracks.isEmpty() && !restored.videoTracks[0].isEmpty()
        && sameOffset(restored.videoTracks[0][0].colorCorrection,cc);

    ColorGradingPanel panel;
    const auto originalWheels = panel.currentWheels();
    int changes = 0;
    QObject::connect(&panel, &ColorGradingPanel::colorWheelsChanged, &panel,
        [&](const ColorWheels &) { ++changes; });
    auto *wheel = panel.findChild<ColorWheelWidget *>(QStringLiteral("offsetWheel"));
    if (wheel) wheel->colorChanged(cc.offsetR,cc.offsetG,cc.offsetB);
    QEventLoop loop;
    QTimer::singleShot(40,&loop,&QEventLoop::quit);
    loop.exec();
    roundtrip &= wheel && changes == 1 && panel.currentWheels() != originalWheels
        && sameOffset(panel.colorCorrection(),cc);

    MainWindow window;
    window.hide();
    auto *timeline = window.findChild<Timeline *>();
    mcp::McpToolRegistry registry;
    mcp::McpEditorTools tools(&window,&registry);
    tools.registerWriteTools();
    if (timeline && !timeline->videoTracks().isEmpty()) {
        clip.colorCorrection = ColorCorrection{};
        timeline->videoTracks()[0]->setClips({clip});
        timeline->undoManager()->clear();
        timeline->undoManager()->saveState(timeline->currentState(),QStringLiteral("offset baseline"));
        const auto serial = timeline->undoManager()->saveSerial();
        bool found = false;
        QString error;
        const QJsonObject result = registry.callTool(QStringLiteral("set_color_correction"),
            QJsonObject{{"clipIndex",0},{"values",QJsonObject{{"offsetR",cc.offsetR},
                {"offsetG",cc.offsetG},{"offsetB",cc.offsetB}}}}, &found,&error);
        roundtrip &= found && error.isEmpty() && result.value("ok").toBool()
            && sameOffset(timeline->videoTracks()[0]->clips()[0].colorCorrection,cc)
            && timeline->undoManager()->saveSerial() == serial+1;
        error.clear();
        registry.callTool(QStringLiteral("set_color_correction"),
            QJsonObject{{"clipIndex",0},{"values",QJsonObject{{"offsetR",2.01}}}}, &found,&error);
        roundtrip &= !error.isEmpty() && timeline->undoManager()->saveSerial() == serial+1;
        error.clear();
        const auto undone = registry.callTool(QStringLiteral("undo"),{},&found,&error);
        roundtrip &= found && error.isEmpty() && undone.value("ok").toBool()
            && sameOffset(timeline->videoTracks()[0]->clips()[0].colorCorrection,ColorCorrection{});
    } else roundtrip = false;
    check(4, roundtrip);

    clip.colorCorrection = cc;
    const auto preset = EffectPreset::fromClipStack(QStringLiteral("オフセット"),clip,false);
    const auto saved = QJsonDocument(preset.toJson()).toJson();
    const auto loaded = EffectPreset::fromJson(QJsonDocument::fromJson(saved).object());
    ClipInfo target;
    loaded.applyToClipStack(target,false);
    check(5, sameOffset(target.colorCorrection,cc)
        && !PresetLibrary::colorCorrectionToJson(ColorCorrection{}).contains("offsetR")
        && pixelsEqual(cpu,VideoEffectProcessor::applyColorCorrection(input,target.colorCorrection)));
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n",passed,failed);
    return failed;
}
