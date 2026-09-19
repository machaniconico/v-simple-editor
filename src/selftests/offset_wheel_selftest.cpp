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
    offsetwheel::resetCallCountForTest();
    bool identity = true;
    for (int i = 0; i < 3; ++i) {
        ColorCorrection cc;
        if (i) { cc.brightness = 5; cc.liftR = 0.1; cc.gammaG = 0.2; }
        if (i == 2) cc.logMidB = 0.15;
        offsetwheel::setDisabledForTest(true);
        const auto baseline = VideoEffectProcessor::applyColorCorrection(input, cc);
        offsetwheel::setDisabledForTest(false);
        identity &= pixelsEqual(baseline, VideoEffectProcessor::applyColorCorrection(input,cc));
    }
    check(1, identity && offsetwheel::callCountForTest() == 0);

    ColorCorrection cc;
    cc.offsetR = 0.125; cc.offsetG = -0.125; cc.offsetB = 0.0625;
    const auto cpu = VideoEffectProcessor::applyColorCorrection(input,cc);
    bool shift = !cc.isDefault() && offsetwheel::callCountForTest() > 0;
    for (int x : {0, 63}) {
        const QColor a = input.pixelColor(x,x), b = cpu.pixelColor(x,x);
        shift &= std::abs(b.red()-a.red()-0.0625*255) <= 1
            && std::abs(b.green()-a.green()+0.0625*255) <= 1
            && std::abs(b.blue()-a.blue()-0.03125*255) <= 1;
    }
    check(2, shift);

    // Real production shader and framebuffer, not a reimplementation of GLSL.
    GLPreview preview;
    // Override GLPreview's normal 320x180 minimum to avoid letterboxing.
    preview.setFixedSize(64,64);
    preview.show();
    QApplication::processEvents();
    preview.setColorCorrection(cc);
    preview.setOffset({cc.offsetR,cc.offsetG,cc.offsetB});
    preview.displayFrame(input);
    QApplication::processEvents();
    QImage gpu = preview.grabFramebuffer();
    bool parity = preview.context() && preview.context()->isValid() && !gpu.isNull();
    if (parity) {
        gpu = gpu.scaled(cpu.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
        double error = 0;
        int maxError = 0;
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            const auto a = cpu.pixelColor(x,y), b = gpu.pixelColor(x,y);
            for (int d : {std::abs(a.red()-b.red()), std::abs(a.green()-b.green()), std::abs(a.blue()-b.blue())}) {
                error += d; maxError = qMax(maxError,d);
            }
        }
        // Strict untransformed 8-bit parity: at most one quantization step.
        parity = error/(64*64*3) <= 1.0 && maxError <= 1;
        preview.setColorCorrection(ColorCorrection{});
        preview.setOffset({0,0,0});
        const auto neutral = preview.grabFramebuffer().convertToFormat(QImage::Format_RGB888);
        preview.setEffectsEnabled(false);
        const auto bypass = preview.grabFramebuffer().convertToFormat(QImage::Format_RGB888);
        parity &= pixelsEqual(neutral,bypass);
    } else {
        std::fprintf(stderr, "G3: usable OpenGL context required for GPU acceptance\n");
    }
    preview.hide();
    check(3, parity);

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
