#include "../ProjectFile.h"
#include "../PuppetEngine.h"
#include "../PuppetPinTool.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "SelftestGate.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>

namespace {

QImage makeTransparentCharacterBar(int w = 200, int h = 200)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setPen(Qt::NoPen);
    // Vertical character silhouette in the middle: x in [82..118], y in [20..180]
    p.setBrush(QColor(230, 80, 50, 255));
    p.drawRect(82, 20, 36, 160);
    // Distinct blue head marker near top
    p.setBrush(QColor(40, 120, 240, 255));
    p.drawRect(84, 24, 32, 32);
    return img;
}

QImage makeOverlapFigure(int w = 200, int h = 200)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setPen(Qt::NoPen);
    // Left arm (bright red) + right torso (bright blue) connected by a base bar so they form one mesh
    p.setBrush(QColor(240, 30, 30, 255));
    p.drawRect(36, 36, 36, 120);
    p.setBrush(QColor(30, 70, 245, 255));
    p.drawRect(114, 36, 50, 120);
    p.setBrush(QColor(110, 110, 110, 255));
    p.drawRect(36, 140, 128, 28);
    return img;
}

QImage makeOpaqueChecker(int w = 160, int h = 120)
{
    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const bool c = (((x / 16) + (y / 16)) & 1) == 0;
            row[x] = c ? qRgba(210, 180, 90, 255) : qRgba(45, 95, 165, 255);
        }
    }
    return img;
}

double meanAbsDiffRgb(const QImage &a, const QImage &b)
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

QPointF opaqueCentroid(const QImage &img, int yMin, int yMax)
{
    const QImage argb = img.convertToFormat(QImage::Format_ARGB32);
    double sx = 0.0, sy = 0.0, wSum = 0.0;
    for (int y = std::max(0, yMin); y < std::min(argb.height(), yMax); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            const int a = qAlpha(row[x]);
            if (a > 128) {
                sx += x;
                sy += y;
                wSum += 1.0;
            }
        }
    }
    return (wSum > 0.0) ? QPointF(sx / wSum, sy / wSum) : QPointF(-1.0, -1.0);
}

} // namespace

int runPuppetArapSelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };
    puppet::clearAllRigs();

    // ------------------------------------------------------------------
    // G1: Alpha-aware silhouette mesh for transparent PNG vs full-frame mesh for opaque video
    // ------------------------------------------------------------------
    {
        const QImage png = makeTransparentCharacterBar(200, 200);
        const QImage vid = makeOpaqueChecker(200, 120);
        puppet::MeshSettings ms;
        ms.density = 14;
        ms.expansionPx = 6;
        ms.alphaThreshold = 8;

        const puppet::PuppetMesh mPng = puppet::buildMeshForImage(png, ms);
        const puppet::PuppetMesh mVid = puppet::buildMeshForImage(vid, ms);
        const int fullSquareTris = 14 * 14 * 2;
        const bool ok = mPng.alphaSilhouette
            && !mPng.triangles.isEmpty()
            && mPng.triangles.size() < static_cast<int>(fullSquareTris * 0.70)
            && !mVid.alphaSilhouette
            && !mVid.triangles.isEmpty()
            && mVid.restVertices.size() > 50;
        rec(1, ok,
            QStringLiteral("pngSil=%1 pngTris=%2 vidSil=%3 vidTris=%4")
                .arg(mPng.alphaSilhouette)
                .arg(mPng.triangles.size())
                .arg(mVid.alphaSilhouette)
                .arg(mVid.triangles.size()));
    }

    // ------------------------------------------------------------------
    // G2: 2-step ARAP limb bending on transparent PNG (top bends right, bottom stays anchored, bg alpha=0)
    // ------------------------------------------------------------------
    {
        const QImage png = makeTransparentCharacterBar(200, 200);
        puppet::PuppetRig rig;
        rig.mesh.density = 14;
        rig.mesh.expansionPx = 6;

        puppet::PuppetPin basePin;
        basePin.id = 1;
        basePin.type = puppet::PinType::Position;
        basePin.restPos = QPointF(0.50, 0.85);
        puppet::upsertPinKeyframe(basePin, {0.0, QPointF(0.50, 0.85), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});

        puppet::PuppetPin headPin;
        headPin.id = 2;
        headPin.type = puppet::PinType::Position;
        headPin.restPos = QPointF(0.50, 0.20);
        puppet::upsertPinKeyframe(headPin, {0.0, QPointF(0.72, 0.26), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});

        rig.pins = {basePin, headPin};
        const QImage bent = puppet::renderPuppetFrame(png, rig, 0.0);

        const QPointF topRest = opaqueCentroid(png, 20, 60);
        const QPointF topBent = opaqueCentroid(bent, 20, 80);
        const QPointF botRest = opaqueCentroid(png, 150, 180);
        const QPointF botBent = opaqueCentroid(bent, 150, 185);
        const int cornerAlpha = qAlpha(bent.pixel(5, 5));

        const bool ok = (topBent.x() - topRest.x() > 28.0)
            && (std::abs(botBent.x() - botRest.x()) < 10.0)
            && (cornerAlpha == 0);
        rec(2, ok,
            QStringLiteral("topShiftX=%1 botShiftX=%2 bgA=%3")
                .arg(topBent.x() - topRest.x(), 0, 'f', 1)
                .arg(botBent.x() - botRest.x(), 0, 'f', 1)
                .arg(cornerAlpha));
    }

    // ------------------------------------------------------------------
    // G3: Keyframe interpolation & easing curves (Linear vs EaseInOut vs Hold) + progressive frames
    // ------------------------------------------------------------------
    {
        puppet::PuppetPin pLin, pEase, pHold;
        pLin.id = 1; pEase.id = 2; pHold.id = 3;
        pLin.restPos = pEase.restPos = pHold.restPos = QPointF(0.50, 0.20);
        puppet::upsertPinKeyframe(pLin,  {0.0, QPointF(0.50, 0.20), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        puppet::upsertPinKeyframe(pLin,  {2.0, QPointF(0.70, 0.20), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        puppet::upsertPinKeyframe(pEase, {0.0, QPointF(0.50, 0.20), 0.0, 1.0, 0.0, puppet::EasingKind::EaseInOut});
        puppet::upsertPinKeyframe(pEase, {2.0, QPointF(0.70, 0.20), 0.0, 1.0, 0.0, puppet::EasingKind::EaseInOut});
        puppet::upsertPinKeyframe(pHold, {0.0, QPointF(0.50, 0.20), 0.0, 1.0, 0.0, puppet::EasingKind::Hold});
        puppet::upsertPinKeyframe(pHold, {2.0, QPointF(0.70, 0.20), 0.0, 1.0, 0.0, puppet::EasingKind::Hold});

        const double lin05  = puppet::evaluatePinAt(pLin,  0.5).position.x();
        const double ease05 = puppet::evaluatePinAt(pEase, 0.5).position.x();
        const double ease10 = puppet::evaluatePinAt(pEase, 1.0).position.x();
        const double hold15 = puppet::evaluatePinAt(pHold, 1.5).position.x();
        const double hold20 = puppet::evaluatePinAt(pHold, 2.0).position.x();

        const QImage png = makeTransparentCharacterBar(200, 200);
        puppet::PuppetRig rig;
        puppet::PuppetPin anchor;
        anchor.id = 10; anchor.restPos = QPointF(0.50, 0.85);
        puppet::upsertPinKeyframe(anchor, {0.0, QPointF(0.50, 0.85), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        rig.pins = {anchor, pEase};
        const QImage f0 = puppet::renderPuppetFrame(png, rig, 0.0);
        const QImage f1 = puppet::renderPuppetFrame(png, rig, 1.0);
        const QImage f2 = puppet::renderPuppetFrame(png, rig, 2.0);
        const double d01 = meanAbsDiffRgb(f0, f1);
        const double d12 = meanAbsDiffRgb(f1, f2);

        const bool ok = std::abs(lin05 - 0.55) < 1e-4
            && ease05 < lin05 - 0.01
            && std::abs(ease10 - 0.60) < 1e-4
            && std::abs(hold15 - 0.50) < 1e-4
            && std::abs(hold20 - 0.70) < 1e-4
            && d01 > 2.0 && d12 > 2.0;
        rec(3, ok,
            QStringLiteral("lin05=%1 ease05=%2 hold15=%3 d01=%4 d12=%5")
                .arg(lin05, 0, 'f', 3)
                .arg(ease05, 0, 'f', 3)
                .arg(hold15, 0, 'f', 3)
                .arg(d01, 0, 'f', 2)
                .arg(d12, 0, 'f', 2));
    }

    // ------------------------------------------------------------------
    // G4: Starch pin increases surrounding triangle stiffness & reduces local bend
    // ------------------------------------------------------------------
    {
        const QImage png = makeTransparentCharacterBar(200, 200);
        puppet::MeshSettings ms;
        ms.density = 14;
        ms.expansionPx = 6;
        const puppet::PuppetMesh baseMesh = puppet::buildMeshForImage(png, ms);

        puppet::PuppetRig softRig;
        puppet::PuppetPin pBot, pTop;
        pBot.id = 1; pBot.restPos = QPointF(0.50, 0.85);
        puppet::upsertPinKeyframe(pBot, {0.0, QPointF(0.50, 0.85), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        pTop.id = 2; pTop.restPos = QPointF(0.50, 0.18);
        puppet::upsertPinKeyframe(pTop, {0.0, QPointF(0.75, 0.25), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        softRig.pins = {pBot, pTop};

        puppet::PuppetRig stiffRig = softRig;
        puppet::PuppetPin starch;
        starch.id = 3;
        starch.type = puppet::PinType::Starch;
        starch.restPos = QPointF(0.50, 0.65);
        starch.radius = 0.35;
        starch.stiffness = 1.0;
        stiffRig.pins.append(starch);

        const auto defSoft  = puppet::solveArapMesh(baseMesh, puppet::evaluateRigAt(softRig, 0.0));
        const auto defStiff = puppet::solveArapMesh(baseMesh, puppet::evaluateRigAt(stiffRig, 0.0));

        double maxStiff = 0.0;
        for (double w : defStiff.base.triangleStiffness) maxStiff = std::max(maxStiff, w);

        // Measure total vertex displacement difference in the stiffened mid-lower band (y in 100..150)
        double softDisp = 0.0, stiffDisp = 0.0;
        int cnt = 0;
        for (int i = 0; i < baseMesh.restVertices.size(); ++i) {
            const QPointF &rv = baseMesh.restVertices[i];
            if (rv.y() >= 100.0 && rv.y() <= 150.0) {
                softDisp  += std::hypot(defSoft.deformedVertices[i].x() - rv.x(),
                                        defSoft.deformedVertices[i].y() - rv.y());
                stiffDisp += std::hypot(defStiff.deformedVertices[i].x() - rv.x(),
                                        defStiff.deformedVertices[i].y() - rv.y());
                ++cnt;
            }
        }
        const bool ok = (maxStiff > 6.0) && (cnt > 0) && (std::abs(softDisp - stiffDisp) > 1.0);
        rec(4, ok,
            QStringLiteral("maxStiff=%1 softDisp=%2 stiffDisp=%3")
                .arg(maxStiff, 0, 'f', 2)
                .arg(softDisp, 0, 'f', 1)
                .arg(stiffDisp, 0, 'f', 1));
    }

    // ------------------------------------------------------------------
    // G5: Bend & Advanced pins rotate and scale mesh cleanly around anchor
    // ------------------------------------------------------------------
    {
        const QImage png = makeTransparentCharacterBar(200, 200);
        puppet::PuppetRig bendRig;
        puppet::PuppetPin bendPin;
        bendPin.id = 1;
        bendPin.type = puppet::PinType::Bend;
        bendPin.restPos = QPointF(0.50, 0.50);
        bendPin.radius = 0.45;
        puppet::upsertPinKeyframe(bendPin, {0.0, QPointF(0.50, 0.50), 45.0, 1.30, 0.0, puppet::EasingKind::Linear});
        bendRig.pins = {bendPin};

        const QImage bentImg = puppet::renderPuppetFrame(png, bendRig, 0.0);
        const double bendDiff = meanAbsDiffRgb(png, bentImg);

        // Advanced pin translates + rotates simultaneously
        puppet::PuppetRig advRig;
        puppet::PuppetPin advPin;
        advPin.id = 2;
        advPin.type = puppet::PinType::Advanced;
        advPin.restPos = QPointF(0.50, 0.50);
        advPin.radius = 0.40;
        puppet::upsertPinKeyframe(advPin, {0.0, QPointF(0.62, 0.50), -35.0, 1.10, 0.0, puppet::EasingKind::Linear});
        advRig.pins = {advPin};
        const QImage advImg = puppet::renderPuppetFrame(png, advRig, 0.0);
        const QPointF advCen = opaqueCentroid(advImg, 0, 200);

        const bool ok = (bendDiff > 6.0) && (advCen.x() > 112.0);
        rec(5, ok,
            QStringLiteral("bendDiff=%1 advCenX=%2")
                .arg(bendDiff, 0, 'f', 2)
                .arg(advCen.x(), 0, 'f', 1));
    }

    // ------------------------------------------------------------------
    // G6: Overlap pin Z-depth sorting (flipping inFront reverses which limb draws on top)
    // ------------------------------------------------------------------
    {
        const QImage fig = makeOverlapFigure(200, 200);

        auto renderWithDepths = [&](double armDepth, double torsoDepth) {
            puppet::PuppetRig rig;
            rig.mesh.density = 16;
            rig.mesh.expansionPx = 4;

            // Anchor torso in place; pull red left arm right across blue torso!
            puppet::PuppetPin torsoAnchor;
            torsoAnchor.id = 1; torsoAnchor.type = puppet::PinType::Position;
            torsoAnchor.restPos = QPointF(0.70, 0.45);
            puppet::upsertPinKeyframe(torsoAnchor, {0.0, QPointF(0.70, 0.45), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});

            puppet::PuppetPin baseAnchor;
            baseAnchor.id = 2; baseAnchor.type = puppet::PinType::Position;
            baseAnchor.restPos = QPointF(0.70, 0.78);
            puppet::upsertPinKeyframe(baseAnchor, {0.0, QPointF(0.70, 0.78), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});

            puppet::PuppetPin armMove;
            armMove.id = 3; armMove.type = puppet::PinType::Position;
            armMove.restPos = QPointF(0.27, 0.35);
            puppet::upsertPinKeyframe(armMove, {0.0, QPointF(0.68, 0.42), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});

            puppet::PuppetPin armOv;
            armOv.id = 4; armOv.type = puppet::PinType::Overlap;
            armOv.restPos = QPointF(0.27, 0.42);
            armOv.radius = 0.25;
            puppet::upsertPinKeyframe(armOv, {0.0, armOv.restPos, 0.0, 1.0, armDepth, puppet::EasingKind::Linear});

            puppet::PuppetPin torsoOv;
            torsoOv.id = 5; torsoOv.type = puppet::PinType::Overlap;
            torsoOv.restPos = QPointF(0.70, 0.45);
            torsoOv.radius = 0.25;
            puppet::upsertPinKeyframe(torsoOv, {0.0, torsoOv.restPos, 0.0, 1.0, torsoDepth, puppet::EasingKind::Linear});

            rig.pins = {torsoAnchor, baseAnchor, armMove, armOv, torsoOv};
            return puppet::renderPuppetFrame(fig, rig, 0.0);
        };

        const QImage armFront = renderWithDepths(+60.0, -60.0);
        const QImage torsoFront = renderWithDepths(-60.0, +60.0);

        // Sample average R-B in the overlap zone around (136, 84)
        auto regionRB = [](const QImage &img) {
            const QImage argb = img.convertToFormat(QImage::Format_ARGB32);
            double rb = 0.0;
            int n = 0;
            for (int y = 76; y <= 94; ++y) {
                for (int x = 128; x <= 146; ++x) {
                    const QRgb px = argb.pixel(x, y);
                    rb += static_cast<double>(qRed(px)) - static_cast<double>(qBlue(px));
                    ++n;
                }
            }
            return rb / std::max(1, n);
        };
        const double rbArmFront = regionRB(armFront);
        const double rbTorsoFront = regionRB(torsoFront);

        const bool ok = (rbArmFront > 60.0) && (rbTorsoFront < -60.0);
        rec(6, ok,
            QStringLiteral("rbArmFront=%1 rbTorsoFront=%2")
                .arg(rbArmFront, 0, 'f', 1)
                .arg(rbTorsoFront, 0, 'f', 1));
    }

    // ------------------------------------------------------------------
    // G7: Opaque video frame ARAP deformation (full-frame grid, zero interior holes)
    // ------------------------------------------------------------------
    {
        const QImage vid = makeOpaqueChecker(160, 120);
        puppet::PuppetRig rig;
        rig.mesh.density = 12;

        puppet::PuppetPin p1, p2;
        p1.id = 1; p1.restPos = QPointF(0.20, 0.50);
        puppet::upsertPinKeyframe(p1, {0.0, QPointF(0.20, 0.50), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        p2.id = 2; p2.restPos = QPointF(0.75, 0.50);
        puppet::upsertPinKeyframe(p2, {0.0, QPointF(0.80, 0.35), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        rig.pins = {p1, p2};

        const QImage out = puppet::renderPuppetFrame(vid, rig, 0.0).convertToFormat(QImage::Format_ARGB32);
        const double diff = meanAbsDiffRgb(vid, out);
        // Check interior [40..120] x [45..75] has zero transparent seam cracks!
        int crackPixels = 0;
        for (int y = 45; y < 75; ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(out.constScanLine(y));
            for (int x = 40; x < 120; ++x) {
                if (qAlpha(row[x]) < 250) ++crackPixels;
            }
        }
        const bool ok = (diff > 5.0) && (crackPixels == 0);
        rec(7, ok, QStringLiteral("diff=%1 cracks=%2").arg(diff, 0, 'f', 2).arg(crackPixels));
    }

    // ------------------------------------------------------------------
    // G8: SSOT tlrender::renderFrameAt time-varying animation parity on transparent Shape clip
    // ------------------------------------------------------------------
    QTemporaryDir tmpDir;
    Timeline timeline;
    {
        ClipInfo clip;
        clip.displayName = QStringLiteral("Transparent Puppet Char");
        clip.duration = 3.0;
        clip.outPoint = 3.0;
        Shape body;
        body.type = ShapeType::Rectangle;
        body.position = QPointF(100.0, 100.0);
        body.properties.size = QSizeF(36.0, 150.0);
        body.fill.color = QColor(230, 80, 50, 255);
        body.stroke.enabled = false;
        clip.shapes.append(body);
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
                                    QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);

        puppet::PuppetRig rig;
        rig.mesh.density = 12;
        rig.nextPinId = 3;
        puppet::PuppetPin basePin, headPin;
        basePin.id = 1; basePin.restPos = QPointF(0.50, 0.82);
        puppet::upsertPinKeyframe(basePin, {0.0, QPointF(0.50, 0.82), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        headPin.id = 2; headPin.restPos = QPointF(0.50, 0.22);
        puppet::upsertPinKeyframe(headPin, {0.0, QPointF(0.50, 0.22), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        puppet::upsertPinKeyframe(headPin, {1.0, QPointF(0.75, 0.28), 0.0, 1.0, 0.0, puppet::EasingKind::Linear});
        rig.pins = {basePin, headPin};

        const bool committed = puppet::commitRigToTimeline(&timeline, 0, 0, rig);
        const QImage ssotT0 = tlrender::renderFrameAt(&timeline, 0, QSize(200, 200));
        const QImage ssotT1 = tlrender::renderFrameAt(&timeline, 1000000, QSize(200, 200));
        const double animDiff = meanAbsDiffRgb(ssotT0, ssotT1);
        const bool ok = committed && !ssotT0.isNull() && !ssotT1.isNull() && animDiff > 3.0;
        rec(8, ok, QStringLiteral("committed=%1 animDiff=%2").arg(committed).arg(animDiff, 0, 'f', 2));
    }

    // ------------------------------------------------------------------
    // G9: 1-step Undo / Redo & ProjectFile .veditor JSON persistence round-trip
    // ------------------------------------------------------------------
    {
        const QString uid = timeline.videoTracks()[0]->clips()[0].uid;
        const puppet::PuppetRig beforeRig = puppet::rigForClip(uid);

        puppet::PuppetRig editedRig = beforeRig;
        puppet::PuppetPin ov;
        ov.id = editedRig.nextPinId++;
        ov.name = QStringLiteral("オーバーラップ腕");
        ov.type = puppet::PinType::Overlap;
        ov.restPos = QPointF(0.45, 0.45);
        puppet::upsertPinKeyframe(ov, {0.5, ov.restPos, 0.0, 1.0, 42.0, puppet::EasingKind::EaseOut});
        editedRig.pins.append(ov);

        puppet::commitRigToTimeline(&timeline, 0, 0, editedRig, QStringLiteral("Overlap追加"));
        const int countAfterEdit = puppet::rigForClip(uid).pins.size();

        timeline.undo();
        const int countAfterUndo = puppet::rigForClip(uid).pins.size();

        timeline.redo();
        const int countAfterRedo = puppet::rigForClip(uid).pins.size();

        // ProjectFile round-trip
        const QString projPath = tmpDir.filePath(QStringLiteral("puppet_test.veditor"));
        ProjectData saveData;
        saveData.videoTracks = timeline.allVideoTracks();
        const bool saved = ProjectFile::save(projPath, saveData);
        puppet::clearAllRigs();
        const bool cleared = !puppet::hasRig(uid);
        ProjectData loadedData;
        const bool loaded = ProjectFile::load(projPath, loadedData);
        const puppet::PuppetRig restoredRig = puppet::rigForClip(uid);

        const bool ok = (countAfterEdit == 3) && (countAfterUndo == 2) && (countAfterRedo == 3)
            && saved && cleared && loaded && (restoredRig == editedRig);
        rec(9, ok,
            QStringLiteral("edit=%1 undo=%2 redo=%3 rt=%4")
                .arg(countAfterEdit).arg(countAfterUndo).arg(countAfterRedo)
                .arg(restoredRig == editedRig));
    }

    // ------------------------------------------------------------------
    // G10: PuppetPinTool ARAP interactive drag + Auto-Key + wireframe paint + PuppetRigDialog smoke
    // ------------------------------------------------------------------
    {
        const QString uid = timeline.videoTracks()[0]->clips()[0].uid;
        PuppetPinTool tool(&timeline, 0, 0);
        tool.setUseArapMode(true);
        tool.setToolMode(puppet::PinType::Position);
        tool.setTimeProvider([]() { return 0.5; });
        const QRectF viewRect(0, 0, 200, 200);
        tool.setViewRect(viewRect);
        tool.setClipTransform(SurfaceTool::renderedClipTransform(timeline, 0, 0, 0.5, QSize(200, 200)),
                              QSize(200, 200));

        const int pinsBefore = puppet::rigForClip(uid).pins.size();
        const bool pressed  = tool.handleMousePress(QPoint(100, 100), Qt::LeftButton, Qt::NoModifier);
        const bool moved    = tool.handleMouseMove(QPoint(130, 90), Qt::NoModifier);
        const bool released = tool.handleMouseRelease(QPoint(130, 90), Qt::LeftButton, Qt::NoModifier);
        const puppet::PuppetRig afterDrag = puppet::rigForClip(uid);

        // Paint overlay wireframe + color-coded handles into an offscreen QImage
        QImage overlayImg(200, 200, QImage::Format_ARGB32);
        overlayImg.fill(Qt::transparent);
        QPainter painter(&overlayImg);
        tool.paintOverlay(painter, viewRect);
        painter.end();

        int paintedPx = 0;
        for (int y = 0; y < 200; ++y) {
            const QRgb *r = reinterpret_cast<const QRgb *>(overlayImg.constScanLine(y));
            for (int x = 0; x < 200; ++x)
                if (qAlpha(r[x]) > 0) ++paintedPx;
        }

        // Construct PuppetRigDialog cleanly
        puppet::PuppetRigDialog dlg(&timeline, 0, 0, 0.5);
        const bool dlgOk = (dlg.selectedToolMode() == puppet::PinType::Position);

        timeline.undo();
        const int pinsAfterUndo = puppet::rigForClip(uid).pins.size();

        const bool hasAutoKey = !afterDrag.pins.isEmpty()
            && !afterDrag.pins.last().keyframes.isEmpty()
            && std::abs(afterDrag.pins.last().keyframes.last().timeSec - 0.5) < 1e-3;
        const bool ok = pressed && moved && released
            && (afterDrag.pins.size() == pinsBefore + 1)
            && hasAutoKey && (paintedPx > 200) && dlgOk
            && (pinsAfterUndo == pinsBefore);
        rec(10, ok,
            QStringLiteral("drag=%1/%2/%3 pins=%4->%5->%6 autoKey=%7 paintPx=%8 dlg=%9")
                .arg(pressed).arg(moved).arg(released)
                .arg(pinsBefore).arg(afterDrag.pins.size()).arg(pinsAfterUndo)
                .arg(hasAutoKey).arg(paintedPx).arg(dlgOk));
    }

    puppet::clearAllRigs();
    return tally.finish("puppet-arap");
}
