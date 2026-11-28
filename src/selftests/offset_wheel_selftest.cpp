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
// Keep diagnostics for direct RGB comparisons. Display readbacks are compared
// with CPU only on flat inputs in the central video region, never as deltas.
bool compareRgb(const char *label, const QImage &cpu, const QImage &gpu,
                int tolerance = 1)
{
    if (cpu.isNull() || gpu.isNull() || cpu.size() != gpu.size()) {
        std::fprintf(stderr, "G3 %s: invalid readback/size CPU=%dx%d GPU=%dx%d\n",
            label, cpu.width(), cpu.height(), gpu.width(), gpu.height());
        return false;
    }
    double error = 0;
    int maxError = 0, firstX = -1, firstY = -1;
    for (int y = 0; y < cpu.height(); ++y) for (int x = 0; x < cpu.width(); ++x) {
        const QColor a = cpu.pixelColor(x,y), b = gpu.pixelColor(x,y);
        for (int d : {std::abs(a.red()-b.red()), std::abs(a.green()-b.green()),
                      std::abs(a.blue()-b.blue())}) {
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

    // Review option B: flat frames remove spatial sampling ambiguity. Compare
    // only the physical-pixel centre of letterboxRect(), without resampling.
    // Both backends execute production grading; there is no shader/math replica.
    GLPreview preview;
    preview.setFixedSize(64,64);
    preview.show();
    QApplication::processEvents();
    preview.displayFrame(input);
    const QImage probe = preview.grabFramebuffer();
    const bool contextOk = preview.context() && preview.context()->isValid() && !probe.isNull();
    if (!contextOk) {
        std::fprintf(stderr, "SKIP G3: no usable GL context/readback (headless/WSL)\n");
    } else {
        ColorGradingPanel gpuPanel;
        auto renderGpu = [&](const ColorCorrection &grade) {
            gpuPanel.setColorCorrection(grade);
            const ColorWheels cw = gpuPanel.currentWheels();
            // Same values and setter order as MainWindow's
            // applyColorWheelsToPreview. Do not also call setColorCorrection:
            // this gate exercises the wheels path, with all other effects neutral.
            const std::array<std::array<double,4>,3> values = {{
                {cw.lift.x(), cw.lift.y(), cw.lift.z(), cw.liftLuma},
                {cw.gamma.x(), cw.gamma.y(), cw.gamma.z(), cw.gammaLuma},
                {cw.gain.x(), cw.gain.y(), cw.gain.z(), cw.gainLuma}
            }};
            preview.setEffectsEnabled(true);
            preview.setLiftGammaGain(values);
            preview.setOffset({cw.offset.x(), cw.offset.y(), cw.offset.z()});
            preview.setLogWheels({{
                {cw.logShadow.x(), cw.logShadow.y(), cw.logShadow.z()},
                {cw.logMid.x(), cw.logMid.y(), cw.logMid.z()},
                {cw.logHigh.x(), cw.logHigh.y(), cw.logHigh.z()}
            }});
            return preview.grabFramebuffer().convertToFormat(QImage::Format_RGB888);
        };
        bool parityOk = true;
        // Dark/mid/bright inputs include low/high clamp cases. The mixed grade
        // distinguishes Stage 1 addition from addition after gamma or gain.
        ColorCorrection lift;
        lift.liftR = 0.125;
        ColorCorrection mixed = cc;
        mixed.liftR = 0.0625; mixed.liftG = -0.0625;
        mixed.gammaR = 0.5; mixed.gammaG = -0.5; mixed.gammaB = 0.25;
        mixed.gainR = 0.25; mixed.gainG = -0.25; mixed.gainB = 0.125;
        for (const QColor &color : {QColor(12,20,28), QColor(96,128,160), QColor(230,240,248)}) {
            QImage source(64,64,QImage::Format_RGB888);
            source.fill(color);
            preview.displayFrame(source);
            const QImage neutral = renderGpu(ColorCorrection{});
            const QRectF video = preview.letterboxRect();
            const double dpr = preview.devicePixelRatioF();
            const QRect roi(int(std::ceil((video.x()+video.width()*0.375)*dpr)),
                            int(std::ceil((video.y()+video.height()*0.375)*dpr)),
                            int(std::floor(video.width()*0.25*dpr)),
                            int(std::floor(video.height()*0.25*dpr)));
            std::fprintf(stderr, "G3: input=(%d,%d,%d) DPR=%.3f framebuffer=%dx%d ROI=(%d,%d,%d,%d)\n",
                color.red(), color.green(), color.blue(), dpr, neutral.width(), neutral.height(),
                roi.x(), roi.y(), roi.width(), roi.height());
            auto compareCpu = [&](const char *label, const ColorCorrection &grade, const QImage &gpu) {
                if (roi.isEmpty() || !gpu.rect().contains(roi)) {
                    std::fprintf(stderr, "G3 %s: invalid central video region\n", label);
                    return false;
                }
                QImage flatReference(roi.size(),QImage::Format_RGB888);
                flatReference.fill(color);
                return compareRgb(label, VideoEffectProcessor::applyColorCorrection(flatReference,grade),
                                  gpu.copy(roi));
            };
            const bool controlA = compareCpu("control-A neutral", ColorCorrection{}, neutral);
            preview.setEffectsEnabled(false);
            const auto bypass = preview.grabFramebuffer().convertToFormat(QImage::Format_RGB888);
            const bool neutralOk = compareRgb("neutral vs bypass", neutral, bypass, 0);
            parityOk &= controlA && neutralOk;
            if (!controlA) {
                std::fprintf(stderr, "G3: neutral reference mismatch; offset comparisons not evaluated\n");
                continue; // A failed baseline is a gate failure, never a delta fallback.
            }
            const bool controlB = compareCpu("control-B lift", lift, renderGpu(lift));
            const bool offsetOk = compareCpu("offset direct", cc, renderGpu(cc));
            const bool stageOk = compareCpu("offset with lift/gamma/gain", mixed, renderGpu(mixed));
            parityOk &= controlB && offsetOk && stageOk;
        }
        check(3, parityOk);
    }
    preview.hide();

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
    auto *timeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
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
