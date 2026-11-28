#include "../MainWindow.h"
#include "../ProjectFile.h"
#include "../PuppetEngine.h"
#include "../PuppetPinTool.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpToolRegistry.h"
#include "SelftestGate.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>

#include <cmath>

namespace {

QImage makeCharPng(int w = 200, int h = 200)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(225, 80, 60, 255));
    p.drawRect(82, 24, 36, 152);
    return img;
}

double meanAbsDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull()) return 0.0;
    const QImage aa = a.convertToFormat(QImage::Format_ARGB32);
    const QImage bb = b.convertToFormat(QImage::Format_ARGB32);
    double sum = 0.0;
    const qint64 n = static_cast<qint64>(aa.width()) * aa.height() * 4;
    for (int y = 0; y < aa.height(); ++y) {
        const QRgb *ra = reinterpret_cast<const QRgb *>(aa.constScanLine(y));
        const QRgb *rb = reinterpret_cast<const QRgb *>(bb.constScanLine(y));
        for (int x = 0; x < aa.width(); ++x) {
            sum += std::abs(qRed(ra[x]) - qRed(rb[x]));
            sum += std::abs(qGreen(ra[x]) - qGreen(rb[x]));
            sum += std::abs(qBlue(ra[x]) - qBlue(rb[x]));
            sum += std::abs(qAlpha(ra[x]) - qAlpha(rb[x]));
        }
    }
    return sum / std::max<qint64>(1, n);
}

} // namespace

int runPuppetPhase3Selftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };
    puppet::clearAllRigs();

    // ------------------------------------------------------------------
    // G1: Ramer-Douglas-Peucker Puppet Sketch simplification (line collapses to 2; wave stays accurate)
    // ------------------------------------------------------------------
    {
        QVector<puppet::SketchSample> straight;
        for (int i = 0; i <= 60; ++i) {
            const double t = i / 30.0;
            straight.append({t, QPointF(0.2 + 0.3 * t, 0.5)});
        }
        const auto simpLine = puppet::simplifySketchRdp(straight, 0.004, 1.0 / 30.0);

        QVector<puppet::SketchSample> wave;
        for (int i = 0; i <= 90; ++i) {
            const double t = i / 30.0; // 0..3s
            wave.append({t, QPointF(0.5 + 0.18 * std::sin(t * 3.14159265), 0.3 + 0.05 * t)});
        }
        const auto simpWave = puppet::simplifySketchRdp(wave, 0.005, 1.0 / 30.0);

        // Reconstruct via linear interpolation on simpWave and measure max UV error
        puppet::PuppetPin probePin;
        probePin.id = 1;
        probePin.type = puppet::PinType::Position;
        probePin.restPos = wave.first().posUv;
        puppet::applySketchToPin(probePin, wave, 0.005, puppet::EasingKind::Linear, true);
        double maxErr = 0.0;
        for (const auto &s : wave) {
            const QPointF eval = puppet::evaluatePinAt(probePin, s.timeSec).position;
            maxErr = std::max(maxErr, std::hypot(eval.x() - s.posUv.x(), eval.y() - s.posUv.y()));
        }

        const bool ok = (simpLine.size() == 2)
            && (simpWave.size() >= 6 && simpWave.size() <= 35)
            && (maxErr < 0.012);
        rec(1, ok,
            QStringLiteral("line=%1 wave=%2 maxErr=%3")
                .arg(simpLine.size()).arg(simpWave.size()).arg(maxErr, 0, 'f', 4));
    }

    // ------------------------------------------------------------------
    // G2: applySketchToPin replaces keys inside recorded range while preserving keys outside
    // ------------------------------------------------------------------
    {
        puppet::PuppetPin pin;
        pin.id = 2;
        pin.type = puppet::PinType::Position;
        pin.restPos = QPointF(0.5, 0.5);
        puppet::upsertPinKeyframe(pin, {0.0, QPointF(0.5, 0.5), 0, 1, 0, puppet::EasingKind::Linear});
        puppet::upsertPinKeyframe(pin, {1.0, QPointF(0.9, 0.9), 0, 1, 0, puppet::EasingKind::Linear}); // inside [0.5,2.0] -> replaced
        puppet::upsertPinKeyframe(pin, {5.0, QPointF(0.2, 0.8), 0, 1, 0, puppet::EasingKind::Linear}); // outside -> preserved

        QVector<puppet::SketchSample> recSamples;
        for (int i = 0; i <= 30; ++i) {
            const double t = 0.5 + 1.5 * (i / 30.0);
            recSamples.append({t, QPointF(0.5 + 0.1 * (i / 30.0), 0.4)});
        }
        const int written = puppet::applySketchToPin(pin, recSamples, 0.004, puppet::EasingKind::Linear, true);
        const QPointF atMid = puppet::evaluatePinAt(pin, 1.0).position;
        const QPointF atEnd = puppet::evaluatePinAt(pin, 5.0).position;
        const bool ok = (written == 2)
            && (std::abs(atMid.y() - 0.4) < 1e-3) // old 0.9 replaced!
            && (std::abs(atEnd.x() - 0.2) < 1e-3 && std::abs(atEnd.y() - 0.8) < 1e-3);
        rec(2, ok, QStringLiteral("written=%1 midY=%2 endX=%3").arg(written).arg(atMid.y(), 0, 'f', 2).arg(atEnd.x(), 0, 'f', 2));
    }

    // ------------------------------------------------------------------
    // G3: Interactive PuppetPinTool Ctrl+Drag Sketch + Trail Paint + 1-Step Undo
    // ------------------------------------------------------------------
    {
        Timeline tl;
        ClipInfo clip;
        clip.displayName = QStringLiteral("Sketch Char");
        clip.duration = 4.0;
        clip.outPoint = 4.0;
        tl.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, {}, 0.0, -1.0, -1.0, 100);

        puppet::PuppetRig initRig;
        initRig.nextPinId = 2;
        puppet::PuppetPin p;
        p.id = 1;
        p.type = puppet::PinType::Position;
        p.restPos = QPointF(0.5, 0.5);
        puppet::upsertPinKeyframe(p, {0.0, QPointF(0.5, 0.5), 0, 1, 0, puppet::EasingKind::Linear});
        initRig.pins = {p};
        puppet::commitRigToTimeline(&tl, 0, 0, initRig);
        const QString uid = tl.videoTracks()[0]->clips()[0].uid;

        PuppetPinTool tool(&tl, 0, 0);
        tool.setUseArapMode(true);
        tool.setTimeProvider([]() { return 0.5; });
        const QRectF view(0, 0, 200, 200);
        tool.setViewRect(view);
        tool.setClipTransform(SurfaceTool::renderedClipTransform(tl, 0, 0, 0.5, QSize(200, 200)), QSize(200, 200));

        const bool pressed = tool.handleMousePress(QPoint(100, 100), Qt::LeftButton, Qt::ControlModifier);
        const bool recStarted = tool.isSketchRecording();
        for (int i = 1; i <= 20; ++i) {
            const double t = 0.5 + i * 0.1;
            const int x = 100 + static_cast<int>(std::lround(35.0 * std::sin(i * 0.35)));
            const int y = 100 - i * 2;
            tool.recordSketchSampleAt(QPoint(x, y), t);
        }

        // Paint live recording trail + REC badge
        QImage overlay(200, 200, QImage::Format_ARGB32);
        overlay.fill(Qt::transparent);
        QPainter painter(&overlay);
        tool.paintOverlay(painter, view);
        painter.end();
        int redBadgePx = 0;
        for (int y = 8; y < 32; ++y)
            for (int x = 25; x < 195; ++x)
                if (qRed(overlay.pixel(x, y)) > 180 && qGreen(overlay.pixel(x, y)) < 80) ++redBadgePx;

        const bool released = tool.handleMouseRelease(QPoint(100, 60), Qt::LeftButton, Qt::ControlModifier);
        const int keysAfterSketch = puppet::rigForClip(uid).pins[0].keyframes.size();
        tl.undo();
        const int keysAfterUndo = puppet::rigForClip(uid).pins[0].keyframes.size();

        const bool ok = pressed && recStarted && released && !tool.isSketchRecording()
            && redBadgePx > 100 && keysAfterSketch >= 4 && keysAfterUndo == 1;
        rec(3, ok,
            QStringLiteral("rec=%1 badgePx=%2 keys=%3->%4")
                .arg(recStarted).arg(redBadgePx).arg(keysAfterSketch).arg(keysAfterUndo));
    }

    // ------------------------------------------------------------------
    // G4: Per-pin Wiggle secondary motion (deterministic, seed-independent, animated ARAP frames)
    // ------------------------------------------------------------------
    {
        puppet::PuppetPin pA, pB;
        pA.id = 1; pA.restPos = QPointF(0.5, 0.25);
        pA.dynamics.wiggleEnabled = true;
        pA.dynamics.wiggleFreqHz = 2.5;
        pA.dynamics.wiggleAmpUv = 0.08;
        pA.dynamics.wiggleSeed = 11;

        pB = pA;
        pB.dynamics.wiggleSeed = 77;

        const QPointF a0 = puppet::evaluatePinAt(pA, 0.0).position;
        const QPointF a1 = puppet::evaluatePinAt(pA, 0.37).position;
        const QPointF a1Repeat = puppet::evaluatePinAt(pA, 0.37).position;
        const QPointF b1 = puppet::evaluatePinAt(pB, 0.37).position;

        const QImage png = makeCharPng();
        puppet::PuppetRig rig;
        rig.nextPinId = 3;
        puppet::PuppetPin anchor;
        anchor.id = 2; anchor.restPos = QPointF(0.5, 0.85);
        rig.pins = {anchor, pA};
        const QImage f0 = puppet::renderPuppetFrame(png, rig, 0.0);
        const QImage f1 = puppet::renderPuppetFrame(png, rig, 0.37);
        const double diff = meanAbsDiff(f0, f1);

        const bool deterministic = (std::hypot(a1.x() - a1Repeat.x(), a1.y() - a1Repeat.y()) < 1e-9);
        const bool moved = (std::hypot(a1.x() - a0.x(), a1.y() - a0.y()) > 0.005);
        const bool seedDiff = (std::hypot(a1.x() - b1.x(), a1.y() - b1.y()) > 0.005);
        rec(4, deterministic && moved && seedDiff && diff > 1.0,
            QStringLiteral("det=%1 moved=%2 seedDiff=%3 imgDiff=%4")
                .arg(deterministic).arg(moved).arg(seedDiff).arg(diff, 0, 'f', 2));
    }

    // ------------------------------------------------------------------
    // G5: Per-pin Expression evaluation (exprY / exprRot) & 1-click Sway Presets
    // ------------------------------------------------------------------
    {
        puppet::PuppetPin pin;
        pin.id = 3;
        pin.type = puppet::PinType::Advanced;
        pin.restPos = QPointF(0.5, 0.4);
        pin.dynamics.exprY = QStringLiteral("value + sin(time * 3.14159265) * 0.1");
        pin.dynamics.exprRot = QStringLiteral("value + time * 30.0");

        const auto ep = puppet::evaluatePinAt(pin, 0.5);
        const bool exprOk = std::abs(ep.position.y() - 0.5) < 1e-3
            && std::abs(ep.rotationDeg - 15.0) < 1e-3;

        puppet::PuppetPin presetPin;
        presetPin.id = 4;
        puppet::applySwayPreset(presetPin, puppet::SwayPreset::Breathing);
        const bool breathOk = !presetPin.dynamics.exprY.isEmpty() && !presetPin.dynamics.wiggleEnabled;
        puppet::applySwayPreset(presetPin, puppet::SwayPreset::HairRibbon);
        const bool hairOk = presetPin.dynamics.wiggleEnabled && presetPin.dynamics.wiggleAmpUv > 0.01;
        puppet::applySwayPreset(presetPin, puppet::SwayPreset::None);
        const bool clearOk = presetPin.dynamics.isDefault();

        rec(5, exprOk && breathOk && hairOk && clearOk,
            QStringLiteral("y=%1 rot=%2 presets=%3/%4/%5")
                .arg(ep.position.y(), 0, 'f', 3).arg(ep.rotationDeg, 0, 'f', 1)
                .arg(breathOk).arg(hairOk).arg(clearOk));
    }

    // ------------------------------------------------------------------
    // G6: ProjectFile .veditor JSON round-trip with PinDynamics + PuppetRigDialog controls
    // ------------------------------------------------------------------
    QTemporaryDir tmpDir;
    {
        Timeline tl;
        ClipInfo clip;
        clip.displayName = QStringLiteral("Dyn Persist");
        clip.duration = 2.0;
        clip.outPoint = 2.0;
        tl.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, {}, 0.0, -1.0, -1.0, 100);

        puppet::PuppetRig rig;
        rig.nextPinId = 2;
        puppet::PuppetPin p;
        p.id = 1;
        p.type = puppet::PinType::Advanced;
        p.restPos = QPointF(0.5, 0.3);
        puppet::applySwayPreset(p, puppet::SwayPreset::WindGust);
        p.dynamics.exprY = QStringLiteral("value + sin(time * 2) * 0.02");
        rig.pins = {p};
        puppet::commitRigToTimeline(&tl, 0, 0, rig);
        const QString uid = tl.videoTracks()[0]->clips()[0].uid;

        const QString projPath = tmpDir.filePath(QStringLiteral("puppet_p3.veditor"));
        ProjectData saveData;
        saveData.videoTracks = tl.allVideoTracks();
        const bool saved = ProjectFile::save(projPath, saveData);
        puppet::clearAllRigs();
        ProjectData loadedData;
        const bool loaded = ProjectFile::load(projPath, loadedData);
        const puppet::PuppetRig restored = puppet::rigForClip(uid);

        puppet::PuppetRigDialog dlg(&tl, 0, 0, 0.25);
        const bool ok = saved && loaded && (restored == rig) && dlg.windowTitle().contains(QStringLiteral("パペット"));
        rec(6, ok, QStringLiteral("saved=%1 loaded=%2 eq=%3").arg(saved).arg(loaded).arg(restored == rig));
    }

    // ------------------------------------------------------------------
    // G7 & G8: Real MCP get_clip_puppet / set_clip_puppet round-trip + invalid expr refusal +
    //          1-step Undo + SSOT tlrender::renderFrameAt animation driven strictly via MCP!
    // ------------------------------------------------------------------
    {
        MainWindow win;
        Timeline *tl = win.findChild<Timeline *>();
        if (!tl) {
            rec(7, false, QStringLiteral("MainWindow has no Timeline child"));
            rec(8, false, QStringLiteral("MainWindow has no Timeline child"));
            puppet::clearAllRigs();
            return tally.finish("puppet-phase3");
        }
        ClipInfo clip;
        clip.displayName = QStringLiteral("MCP Puppet Shape");
        clip.duration = 3.0;
        clip.outPoint = 3.0;
        Shape body;
        body.type = ShapeType::Rectangle;
        body.position = QPointF(100.0, 100.0);
        body.properties.size = QSizeF(36.0, 150.0);
        body.fill.color = QColor(235, 80, 50, 255);
        body.stroke.enabled = false;
        clip.shapes.append(body);
        tl->restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, {}, 0.0, -1.0, -1.0, 100);

        mcp::McpToolRegistry registry;
        mcp::McpEditorTools tools(&win, &registry);
        tools.registerReadTools();
        tools.registerWriteTools();

        bool found = false;
        QString err;
        const QJsonObject get0 = registry.callTool(
            QStringLiteral("get_clip_puppet"),
            QJsonObject{{QStringLiteral("clipIndex"), 0}}, &found, &err);
        const bool emptyInitially = found && err.isEmpty() && !get0.value(QStringLiteral("present")).toBool();

        // Build MCP puppet payload with anchor + animated expression pin
        const QJsonObject puppetPayload{
            {QStringLiteral("mesh"), QJsonObject{{QStringLiteral("density"), 12}}},
            {QStringLiteral("pins"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), 1}, {QStringLiteral("type"), QStringLiteral("position")},
                    {QStringLiteral("rx"), 0.50}, {QStringLiteral("ry"), 0.82}
                },
                QJsonObject{
                    {QStringLiteral("id"), 2}, {QStringLiteral("type"), QStringLiteral("position")},
                    {QStringLiteral("rx"), 0.50}, {QStringLiteral("ry"), 0.22},
                    {QStringLiteral("dynamics"), QJsonObject{
                        {QStringLiteral("exprX"), QStringLiteral("value + time * 0.35")}
                    }}
                }
            }}
        };

        err.clear();
        const QJsonObject set1 = registry.callTool(
            QStringLiteral("set_clip_puppet"),
            QJsonObject{{QStringLiteral("clipIndex"), 0}, {QStringLiteral("puppet"), puppetPayload}},
            &found, &err);
        const bool setOk = found && err.isEmpty()
            && set1.value(QStringLiteral("ok")).toBool()
            && set1.value(QStringLiteral("changed")).toBool()
            && set1.value(QStringLiteral("present")).toBool();

        // No-op repeat must return changed=false
        err.clear();
        const QJsonObject setNoop = registry.callTool(
            QStringLiteral("set_clip_puppet"),
            QJsonObject{{QStringLiteral("clipIndex"), 0},
                        {QStringLiteral("puppet"), set1.value(QStringLiteral("puppet"))}},
            &found, &err);
        const bool noopOk = err.isEmpty() && !setNoop.value(QStringLiteral("changed")).toBool();

        // Invalid expression must be rejected cleanly
        const QJsonObject badPayload{
            {QStringLiteral("pins"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), 1}, {QStringLiteral("type"), QStringLiteral("position")},
                    {QStringLiteral("rx"), 0.5}, {QStringLiteral("ry"), 0.5},
                    {QStringLiteral("dynamics"), QJsonObject{
                        {QStringLiteral("exprX"), QStringLiteral("1 + * ) bad")}
                    }}
                }
            }}
        };
        err.clear();
        registry.callTool(
            QStringLiteral("set_clip_puppet"),
            QJsonObject{{QStringLiteral("clipIndex"), 0}, {QStringLiteral("puppet"), badPayload}},
            &found, &err);
        const bool badRefused = !err.isEmpty();

        // G8: Render SSOT frames at t=0.0s and t=0.6s driven purely via MCP!
        const QImage f0 = tlrender::renderFrameAt(tl, 0, QSize(200, 200));
        const QImage f1 = tlrender::renderFrameAt(tl, 600'000, QSize(200, 200));
        const double mcpAnimDiff = meanAbsDiff(f0, f1);

        // Clear via null -> 1 Undo restores!
        err.clear();
        const QJsonObject clearedRes = registry.callTool(
            QStringLiteral("set_clip_puppet"),
            QJsonObject{{QStringLiteral("clipIndex"), 0}, {QStringLiteral("puppet"), QJsonValue::Null}},
            &found, &err);
        const bool clearOk = err.isEmpty()
            && clearedRes.value(QStringLiteral("changed")).toBool()
            && !clearedRes.value(QStringLiteral("present")).toBool();

        tl->undo();
        err.clear();
        const QJsonObject afterUndo = registry.callTool(
            QStringLiteral("get_clip_puppet"),
            QJsonObject{{QStringLiteral("clipIndex"), 0}}, &found, &err);
        const bool undoRestored = err.isEmpty() && afterUndo.value(QStringLiteral("present")).toBool();

        rec(7, emptyInitially && setOk && noopOk && badRefused && clearOk && undoRestored,
            QStringLiteral("empty=%1 set=%2 noop=%3 badRef=%4 clr=%5 undo=%6")
                .arg(emptyInitially).arg(setOk).arg(noopOk).arg(badRefused).arg(clearOk).arg(undoRestored));
        rec(8, !f0.isNull() && !f1.isNull() && mcpAnimDiff > 3.0,
            QStringLiteral("mcpAnimDiff=%1").arg(mcpAnimDiff, 0, 'f', 2));
    }

    puppet::clearAllRigs();
    return tally.finish("puppet-phase3");
}
