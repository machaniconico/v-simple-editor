// US-707: SurfaceTool / MeshEditTool / PuppetPinTool map screen <-> source UV
// through the same clip fit (snsfit) + placement (clipgeom::resolveTransform)
// renderFrameAt applies after the mesh warp / puppet pins.
#include "../MeshEditTool.h"
#include "../PuppetPinTool.h"
#include "../SurfaceTool.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TrackMatteKey.h"
#include "../UndoManager.h"
#include "../WarpDistortion.h"
#include "../clipanim/ClipAnim.h"
#include "../libavcore/Encode.h"
#include "../playback/SnsFit.h"
#include "SelftestGate.h"

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>

namespace {
// Canvas twice the view so one canvas pixel is half a screen pixel.
const QSize kCanvas(1280, 720);
const QRectF kView(10, 20, 640, 360);

struct Placement {
    clipgeom::ClipTransform transform;
    QSize canvas = kCanvas;
    QRectF view = kView;
    QSize source;                  // empty = canvas-sized source, no fit
    bool fitContain = false;
    bool fitCover = false;
};

// Where the renderer's chain (snsfit::maybeFit -> clipgeom::renderLayer, as in
// TimelineFrameRenderer's overlay path) draws the source point `uv`: weighted
// centroid of a red 4x4 marker centred exactly on uv * source, mapped into view.
QPointF renderedScreenPoint(const Placement &p, const QPointF &uv, bool *found)
{
    const QSize sourceSize = p.source.isEmpty() ? p.canvas : p.source;
    QImage source(sourceSize, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::white);
    const int cx = qRound(uv.x() * sourceSize.width()), cy = qRound(uv.y() * sourceSize.height());
    for (int y = cy - 2; y < cy + 2; ++y)
        for (int x = cx - 2; x < cx + 2; ++x)
            source.setPixelColor(x, y, Qt::red);
    const QImage fitted = snsfit::maybeFit(source, p.fitContain, p.fitCover, p.canvas);
    const QImage placed = clipgeom::renderLayer(fitted, p.transform, p.canvas, /*smooth=*/true);
    double sx = 0.0, sy = 0.0, sw = 0.0;
    for (int y = 0; y < placed.height(); ++y)
        for (int x = 0; x < placed.width(); ++x) {
            const QColor c = placed.pixelColor(x, y);
            // White -> 0, opaque red -> 1; antialiased edges weigh in between.
            const double w = c.alphaF() * (1.0 - c.greenF());
            if (w <= 0.0) continue;
            sx += w * (x + 0.5);
            sy += w * (y + 0.5);
            sw += w;
        }
    *found = sw > 0.0;
    if (!*found) return {};
    return p.view.topLeft() + QPointF(sx / sw * p.view.width() / p.canvas.width(),
                                      sy / sw * p.view.height() / p.canvas.height());
}

// toWidget lands on the rendered point and widgetToUv brings it back to uv.
bool mapsLikeRenderer(const SurfaceTool &tool, const Placement &p, const QPointF &uv,
                      double screenTol, const char *label)
{
    bool found = false;
    const QPointF expected = renderedScreenPoint(p, uv, &found);
    const QPointF screen = tool.toWidget(uv, p.view);
    const QPointF back = tool.widgetToUv(expected, p.view);
    const double screenErr = std::hypot(screen.x() - expected.x(), screen.y() - expected.y());
    const double uvErr = std::max(std::abs(back.x() - uv.x()), std::abs(back.y() - uv.y()));
    const clipgeom::ClipTransform &t = p.transform;
    std::cerr << "surface-tool " << label << " scale=" << t.videoScale << " dx=" << t.videoDx
              << " rot=" << t.rotationDeg << " canvas=" << p.canvas.width() << "x"
              << p.canvas.height() << " source=" << p.source.width() << "x" << p.source.height()
              << " contain=" << p.fitContain << " cover=" << p.fitCover << " uv=(" << uv.x()
              << "," << uv.y() << ") rendered=(" << expected.x() << "," << expected.y()
              << ") toWidget=(" << screen.x() << "," << screen.y() << ") err=" << screenErr
              << "px widgetToUv=(" << back.x() << "," << back.y() << ") err=" << uvErr
              << " markerFound=" << found << '\n';
    return found && screenErr <= screenTol && uvErr <= 1e-3;
}

bool roundTripGate(selftest::GateTally &tally, int n, const Placement &p, const QPointF &uv,
                   double screenTol)
{
    SurfaceTool tool(nullptr);
    tool.setClipTransform(p.transform, p.canvas, p.source, p.fitContain, p.fitCover);
    const std::string label = "G" + std::to_string(n);
    return tally.gate(n, mapsLikeRenderer(tool, p, uv, screenTol, label.c_str()));
}

ClipInfo placedClip(const clipgeom::ClipTransform &t)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("surface-tool-selftest.mp4");
    clip.duration = clip.outPoint = 1.0;
    clip.videoScale = t.videoScale;
    clip.videoDx = t.videoDx;
    clip.videoDy = t.videoDy;
    clip.rotation2DDegrees = t.rotationDeg;
    return clip;
}

void restoreSingle(Timeline &timeline, const ClipInfo &clip)
{
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
        QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
}

// UV x-displacement of mesh handle 5 when dragged 10 screen px to the right.
double meshDragDisplacement(const clipgeom::ClipTransform &t, bool *pressed)
{
    Timeline timeline;
    restoreSingle(timeline, placedClip(t));
    MeshEditTool tool(&timeline, 0, 0);
    tool.setViewRect(kView);
    // Same derivation MainWindow passes when it installs the tool.
    tool.setClipTransform(SurfaceTool::renderedClipTransform(timeline, 0, 0, 0.0, kCanvas), kCanvas);
    SurfaceTool &base = tool;
    const QPoint handle = base.toWidget(QPointF(1.0 / 3.0, 1.0 / 3.0), kView).toPoint();
    *pressed = tool.handleMousePress(handle, Qt::LeftButton, Qt::NoModifier)
        && tool.handleMouseMove(handle + QPoint(10, 0), Qt::NoModifier);
    const MeshGrid grid = timeline.videoTracks()[0]->clips()[0].meshWarp;
    const double moved = grid.controlPoints.size() == 4 && grid.controlPoints[1].size() == 4
        ? grid.controlPoints[1][1].x() - 1.0 / 3.0
        : 0.0;
    tool.cancelDrag();
    return moved;
}

// US-805 parent-gate cases: opaque synthetic media through FrameEncoder.
bool writeOpaqueClip(const QString &path)
{
    libavcore::EncodeRequest req;
    req.width = 40;
    req.height = 24;
    req.fps = 10;
    req.fpsNum = 10;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(req)) return false;
    for (int i = 0; i < 30; ++i) {
        QImage frame(req.width, req.height, QImage::Format_RGB888);
        frame.fill(QColor(200, (i * 7) & 255, 60));
        if (!encoder.pushFrame(frame, i)) return false;
    }
    return !encoder.finalize();
}

// Bounds [left, right) x [top, bottom) of non-transparent pixels; null if none.
QRectF opaqueBounds(const QImage &frame)
{
    const QImage image = frame.convertToFormat(QImage::Format_ARGB32);
    int minX = -1, maxX = -1, minY = -1, maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) == 0) continue;
            if (minX < 0 || x < minX) minX = x;
            maxX = qMax(maxX, x);
            if (minY < 0) minY = y;
            maxY = y;
        }
    }
    if (minX < 0) return {};
    return QRectF(QPointF(minX, minY), QPointF(maxX + 1, maxY + 1));
}

ClipInfo nullParent(double dx, double inPoint, double outPoint, double duration)
{
    ClipInfo clip;
    clip.filePath = clipgeom::nullObjectFilePath();
    clip.duration = duration;
    clip.inPoint = inPoint;
    clip.outPoint = outPoint;
    clip.videoDx = dx;
    clip.opacity = 0.0;
    clip.visible = false;
    return clip;
}

void crossDissolve(ClipInfo &out, ClipInfo &in, TransitionAlignment alignment)
{
    out.trailOut.type = in.leadIn.type = TransitionType::CrossDissolve;
    out.trailOut.duration = in.leadIn.duration = 1.0;
    out.trailOut.alignment = in.leadIn.alignment = alignment;
}
} // namespace

int runSurfaceToolSelftest()
{
    selftest::GateTally tally;

    // G1: identity placement keeps the plain view normalization.
    {
        SurfaceTool plain(nullptr), identity(nullptr);
        identity.setClipTransform(clipgeom::ClipTransform{}, kCanvas);
        bool ok = true;
        double worstRoundTrip = 0.0;
        for (int y = 20; y <= 380; y += 45)
            for (int x = 10; x <= 650; x += 80) {
                const QPointF p(x + 0.25, y + 0.75);
                const QPointF linear((p.x() - kView.x()) / kView.width(),
                                     (p.y() - kView.y()) / kView.height());
                for (const SurfaceTool *tool : {&plain, &identity}) {
                    const QPointF uv = tool->widgetToUv(p, kView);
                    const QPointF back = tool->toWidget(uv, kView);
                    worstRoundTrip = std::max(worstRoundTrip,
                                              std::hypot(back.x() - p.x(), back.y() - p.y()));
                    ok &= uv == linear;
                }
            }
        std::cerr << "surface-tool G1 linearMatch=" << ok << " worstRoundTrip=" << worstRoundTrip
                  << "px\n";
        tally.gate(1, ok && worstRoundTrip <= 0.5);
    }

    roundTripGate(tally, 2, Placement{{0.5, 0.25, 0.0, 0.0}}, QPointF(0.5, 0.5), 0.5);
    roundTripGate(tally, 3, Placement{{1.0, 0.0, 0.0, 90.0}}, QPointF(0.25, 0.5), 0.5);

    // G4: a 10px drag moves a half-scale clip's mesh point twice as far in UV.
    {
        bool pressedDefault = false, pressedHalf = false;
        const double moveDefault = meshDragDisplacement(clipgeom::ClipTransform{}, &pressedDefault);
        const double moveHalf =
            meshDragDisplacement(clipgeom::ClipTransform{0.5, 0.0, 0.0, 0.0}, &pressedHalf);
        const double ratio = moveDefault != 0.0 ? moveHalf / moveDefault : 0.0;
        std::cerr << "surface-tool G4 defaultDuv=" << moveDefault << " (pressed=" << pressedDefault
                  << ") halfScaleDuv=" << moveHalf << " (pressed=" << pressedHalf
                  << ") ratio=" << ratio << '\n';
        tally.gate(4, pressedDefault && pressedHalf
                          && std::abs(moveDefault - 10.0 / kView.width()) < 1e-9
                          && std::abs(ratio - 2.0) < 1e-6);
    }

    // G5 / G6: a 4:3 source contained / covered in the 16:9 canvas, then placed.
    roundTripGate(tally, 5, Placement{{0.5, 0.25, 0.0, 0.0}, kCanvas, kView, QSize(960, 720), true, false},
                  QPointF(0.25, 0.25), 3.0);
    roundTripGate(tally, 6, Placement{{0.5, 0.25, 0.0, 0.0}, kCanvas, kView, QSize(960, 720), false, true},
                  QPointF(0.25, 0.25), 3.0);

    // G7: the project canvas changes (16:9 -> 9:16) while the tool is open; the
    // next repaint re-reads it, so a rotated clip's handles follow the new render.
    {
        const clipgeom::ClipTransform rotated{1.0, 0.1, 0.0, 90.0};
        Timeline timeline;
        restoreSingle(timeline, placedClip(rotated));
        MeshEditTool tool(&timeline, 0, 0);
        QSize canvas = kCanvas;
        tool.setCanvasProvider([&canvas]() { return canvas; });
        tool.setClipTransform(SurfaceTool::renderedClipTransform(timeline, 0, 0, 0.0, canvas), canvas);
        const SurfaceTool &base = tool;
        const bool before = mapsLikeRenderer(base, Placement{rotated}, QPointF(0.25, 0.5), 0.5,
                                             "G7 before");
        canvas = QSize(720, 1280);
        const QRectF portraitView(10, 20, 360, 640);
        const Placement portrait{rotated, canvas, portraitView};
        bool staleFound = false;
        const QPointF staleExpected = renderedScreenPoint(portrait, QPointF(0.25, 0.5), &staleFound);
        const QPointF stale = base.toWidget(QPointF(0.25, 0.5), portraitView);
        std::cerr << "surface-tool G7 staleErr="
                  << std::hypot(stale.x() - staleExpected.x(), stale.y() - staleExpected.y())
                  << "px (before repaint)\n";
        QImage surface(400, 700, QImage::Format_ARGB32_Premultiplied);
        surface.fill(Qt::transparent);
        {
            QPainter painter(&surface);
            tool.paintOverlay(painter, portraitView);
        }
        const bool after = mapsLikeRenderer(base, portrait, QPointF(0.25, 0.5), 0.5, "G7 after");
        tally.gate(7, before && after);
    }

    // G8: scale 0 cannot be inverted: no handles drawn or hit, no pin placed.
    {
        Timeline timeline;
        restoreSingle(timeline, placedClip(clipgeom::ClipTransform{0.0, 0.0, 0.0, 0.0}));
        MeshEditTool mesh(&timeline, 0, 0);
        mesh.setViewRect(kView);
        mesh.setClipTransform(SurfaceTool::renderedClipTransform(timeline, 0, 0, 0.0, kCanvas), kCanvas);
        int hits = 0;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                const QPoint p = (kView.topLeft() + QPointF(c / 3.0 * kView.width(),
                                                            r / 3.0 * kView.height())).toPoint();
                hits += mesh.hitTest(p) >= 0;
                hits += mesh.handleMousePress(p, Qt::LeftButton, Qt::NoModifier);
            }
        QImage surface(700, 400, QImage::Format_ARGB32_Premultiplied);
        surface.fill(Qt::transparent);
        {
            QPainter painter(&surface);
            mesh.paintOverlay(painter, kView);
        }
        int painted = 0;
        for (int y = 0; y < surface.height(); ++y)
            for (int x = 0; x < surface.width(); ++x)
                painted += qAlpha(surface.pixel(x, y)) != 0;
        PuppetPinTool puppet(&timeline, 0, 0);
        puppet.setViewRect(kView);
        puppet.setClipTransform(SurfaceTool::renderedClipTransform(timeline, 0, 0, 0.0, kCanvas), kCanvas);
        const bool pinPlaced = puppet.handleMousePress(kView.center().toPoint(), Qt::LeftButton,
                                                       Qt::NoModifier);
        const int pins = timeline.videoTracks()[0]->clips()[0].puppetPins.size();
        std::cerr << "surface-tool G8 meshMapValid=" << mesh.clipMapValid()
                  << " puppetMapValid=" << puppet.clipMapValid() << " meshHitsOrPresses=" << hits
                  << " paintedPx=" << painted << " pinPressAccepted=" << pinPlaced
                  << " pins=" << pins << '\n';
        tally.gate(8, !mesh.clipMapValid() && !puppet.clipMapValid() && hits == 0 && painted == 0
                          && !pinPlaced && pins == 0);
    }

    // G9: the puppet influence circle drawn by PuppetPinTool (960x720 source
    // covered into 1280x720, placed at scale 0.5 / dx 0.25, 640x360 view,
    // radius 0.25) matches where the renderer's chain actually deforms:
    // puppetpin::applyNormalized -> snsfit::maybeFit -> clipgeom::renderLayer.
    {
        const QSize sourceSize(960, 720);
        const clipgeom::ClipTransform t{0.5, 0.25, 0.0, 0.0};
        WarpPin pin;
        pin.originalPos = QPointF(0.5, 0.5);
        pin.deformedPos = QPointF(0.52, 0.52);
        pin.radius = 0.25;
        ClipInfo clip = placedClip(t);
        clip.fitCover = true;
        clip.puppetPins = {pin};
        Timeline timeline;
        restoreSingle(timeline, clip);
        PuppetPinTool tool(&timeline, 0, 0);
        tool.setCanvasProvider([]() { return kCanvas; });
        tool.setClipTransform(SurfaceTool::renderedClipTransform(timeline, 0, 0, 0.0, kCanvas),
                              kCanvas, sourceSize, true, true);
        QImage overlay(int(kView.right()) + 10, int(kView.bottom()) + 10,
                       QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        {
            QPainter painter(&overlay);
            tool.paintOverlay(painter, kView);
        }
        QImage source(sourceSize, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor(x * 255 / source.width(), y * 255 / source.height(), 80));
        const auto place = [&](const QImage &frame) {
            return clipgeom::renderLayer(snsfit::maybeFit(frame, false, true, kCanvas), t, kCanvas, true);
        };
        const QImage plain = place(source);
        const QImage warped = place(puppetpin::applyNormalized(source, {pin}));
        // Rendered pin centre in canvas pixels, from the same marker probe as G5/G6.
        bool found = false;
        const QPointF centre = renderedScreenPoint(
            Placement{t, kCanvas, kView, sourceSize, false, true}, pin.originalPos, &found);
        const QPointF centreCanvas((centre.x() - kView.x()) * kCanvas.width() / kView.width(),
                                   (centre.y() - kView.y()) * kCanvas.height() / kView.height());
        const auto changed = [&](int x, int y) {
            const QColor a = plain.pixelColor(x, y), b = warped.pixelColor(x, y);
            return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) >= 4;
        };
        const auto isOutline = [&](int x, int y) {
            const QColor c = overlay.pixelColor(x, y);
            return c.alpha() > 0 && c.red() > 200 && c.green() > 150 && c.green() < 230 && c.blue() < 120;
        };
        // Extent of each along the four axes through the centre, in screen px from the centre.
        const int cxC = qRound(centreCanvas.x()), cyC = qRound(centreCanvas.y());
        const int cxS = qRound(centre.x()), cyS = qRound(centre.y());
        double renderedExt[4] = {0, 0, 0, 0}, drawnExt[4] = {0, 0, 0, 0};
        const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int d = 0; d < 4; ++d) {
            for (int k = 1;; ++k) {
                const int x = cxC + dirs[d][0] * k, y = cyC + dirs[d][1] * k;
                if (x < 0 || y < 0 || x >= kCanvas.width() || y >= kCanvas.height()) break;
                if (changed(x, y)) renderedExt[d] = (k + 0.5) * kView.width() / kCanvas.width();
            }
            for (int k = 1;; ++k) {
                const int x = cxS + dirs[d][0] * k, y = cyS + dirs[d][1] * k;
                if (x < 0 || y < 0 || x >= overlay.width() || y >= overlay.height()) break;
                if (isOutline(x, y)) drawnExt[d] = k;
            }
        }
        bool ok = found;
        std::cerr << "surface-tool G9 centre=(" << centre.x() << "," << centre.y() << ")";
        const char *names[4] = {"right", "left", "down", "up"};
        for (int d = 0; d < 4; ++d) {
            std::cerr << ' ' << names[d] << " rendered=" << renderedExt[d] << " drawn=" << drawnExt[d];
            ok &= renderedExt[d] > 0 && std::abs(renderedExt[d] - drawnExt[d]) <= 3.0;
        }
        std::cerr << '\n';
        tally.gate(9, ok);
    }

    // G10: a click on a contain letterbox bar (outside the clip's drawn area)
    // adds no pin; a click on the picture does.
    {
        ClipInfo clip = placedClip(clipgeom::ClipTransform{});
        clip.fitContain = true;
        Timeline timeline;
        restoreSingle(timeline, clip);
        PuppetPinTool tool(&timeline, 0, 0);
        tool.setViewRect(kView);
        tool.setClipTransform(SurfaceTool::renderedClipTransform(timeline, 0, 0, 0.0, kCanvas),
                              kCanvas, QSize(960, 720), true, false);
        // 960x720 contained in 1280x720: bars are canvas x < 160 and x >= 1120.
        const QPoint bar(int(kView.x()) + 40, int(kView.center().y()));
        const QPoint picture = kView.center().toPoint();
        const bool barAccepted = tool.handleMousePress(bar, Qt::LeftButton, Qt::NoModifier);
        const int pinsAfterBar = timeline.videoTracks()[0]->clips()[0].puppetPins.size();
        const bool pictureAccepted = tool.handleMousePress(picture, Qt::LeftButton, Qt::NoModifier)
            && tool.handleMouseRelease(picture, Qt::LeftButton, Qt::NoModifier);
        const auto pins = timeline.videoTracks()[0]->clips()[0].puppetPins;
        std::cerr << "surface-tool G10 barUv=(" << tool.widgetToUv(bar, kView).x() << ","
                  << tool.widgetToUv(bar, kView).y() << ") barAccepted=" << barAccepted
                  << " pinsAfterBar=" << pinsAfterBar << " pictureAccepted=" << pictureAccepted
                  << " pins=" << pins.size();
        if (pins.size() == 1)
            std::cerr << " pinUv=(" << pins[0].originalPos.x() << "," << pins[0].originalPos.y() << ")";
        std::cerr << '\n';
        tally.gate(10, !barAccepted && pinsAfterBar == 0 && pictureAccepted && pins.size() == 1
                           && std::abs(pins[0].originalPos.x() - 0.5) < 1e-9);
    }

    // G11 / G12 (US-805): renderedClipTransform's parent gate is
    // tlrender::parentLayerAt, so in the three forms where a separate interval
    // check used to disagree the tool placement (static call, and MeshEditTool /
    // PuppetPinTool after a repaint re-reads it at the playhead) equals what
    // renderFrameAt draws. Tracks: V1 Null base, V2 parent(s), V3 child (media,
    // scale 0.3, videoDx 0.1). Canvas 400x200, view == canvas (1 px = 1 px).
    // `expectedParentLocal` < 0 means the renderer draws the child raw.
    QTemporaryDir mediaDir;
    const QString media = mediaDir.path() + QStringLiteral("/surface-parent.mp4");
    const bool haveMedia = mediaDir.isValid() && writeOpaqueClip(media);
    const QSize renderCanvas(400, 200);
    const QRectF renderView(QPointF(0, 0), QSizeF(renderCanvas));
    // `childKeys` (G13): the child gets these x keys and a nominal [0, 2) with
    // a 1 s Start cross dissolve into a Null, so it is drawn past its end.
    // Empty `parents`: the child has no parent link.
    const auto parentCase = [&](const char *label, const QVector<ClipInfo> &parents,
                                int parentClip, double timelineSec, double expectedParentLocal,
                                const KeyframeTrack *childKeys = nullptr) {
        ClipInfo base = nullParent(0.0, 0.0, 4.0, 4.0);
        ClipInfo child;
        child.filePath = media;
        child.duration = child.outPoint = 3.0;
        child.videoScale = 0.3;
        child.videoDx = 0.1;
        QVector<ClipInfo> childTrack{child};
        if (childKeys) {
            childTrack[0].outPoint = 2.0;
            childTrack[0].keyframes.addTrack(*childKeys);
            childTrack.append(nullParent(0.0, 0.0, 1.0, 3.0));
            crossDissolve(childTrack[0], childTrack[1], TransitionAlignment::Start);
        }
        Timeline tl;
        tl.restoreFromProject(QVector<QVector<ClipInfo>>{{base}, parents, childTrack},
                              QVector<QVector<ClipInfo>>{}, timelineSec, -1.0, -1.0, 100);
        tl.setProjectOutputConfig(renderCanvas.width(), renderCanvas.height(), true);
        if (!parents.isEmpty())
            tl.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, parentClip));
        const auto hit = parents.isEmpty() ? std::optional<tlrender::ParentLayerHit>{}
            : tlrender::parentLayerAt(&tl, qRound64(timelineSec * 1e6), 1, parentClip);
        const auto childHit = tlrender::parentLayerAt(&tl, qRound64(timelineSec * 1e6), 2, 0);
        // Expected placement, built from the clips directly: child at its own
        // clip-local time (starts at 0), parent at the renderer's clip-local time.
        const clipgeom::ClipTransform childRaw = clipanim::effectiveTransformAt(
            tl.videoTracks()[2]->clips()[0], timelineSec);
        const clipgeom::ClipTransform expected = expectedParentLocal < 0.0 ? childRaw
            : clipgeom::composeParented(childRaw, clipanim::effectiveTransformAt(
                  tl.videoTracks()[1]->clips()[parentClip], expectedParentLocal), renderCanvas);
        const clipgeom::ClipTransform got =
            SurfaceTool::renderedClipTransform(tl, 2, 0, timelineSec, renderCanvas);
        const bool sameTransform = std::abs(got.videoScale - expected.videoScale) < 1e-9
            && std::abs(got.videoDx - expected.videoDx) < 1e-9
            && std::abs(got.videoDy - expected.videoDy) < 1e-9
            && std::abs(got.rotationDeg - expected.rotationDeg) < 1e-9;
        const QRectF toolRect = clipgeom::resolveTransform(got, renderCanvas)
            .mapRect(renderView);
        const QRectF drawn = opaqueBounds(tlrender::renderFrameAt(
            &tl, qRound64(timelineSec * 1e6), renderCanvas));
        // The tools' own placement after a repaint (syncClipTransform at the playhead).
        const auto toolBounds = [&](SurfaceTool &tool) {
            tool.setCanvasProvider([&]() { return renderCanvas; });
            tool.setClipTransform(clipgeom::ClipTransform{}, renderCanvas);
            QImage surface(renderCanvas, QImage::Format_ARGB32_Premultiplied);
            surface.fill(Qt::transparent);
            {
                QPainter painter(&surface);
                tool.paintOverlay(painter, renderView);
            }
            return QRectF(tool.toWidget(QPointF(0, 0), renderView),
                          tool.toWidget(QPointF(1, 1), renderView)).normalized();
        };
        MeshEditTool mesh(&tl, 2, 0);
        PuppetPinTool puppet(&tl, 2, 0);
        const QRectF meshRect = toolBounds(mesh);
        const QRectF puppetRect = toolBounds(puppet);
        const auto edgeErr = [&](const QRectF &r) {
            if (drawn.isNull() || r.isNull()) return 1e9;
            return std::max({std::abs(r.left() - drawn.left()), std::abs(r.right() - drawn.right()),
                             std::abs(r.top() - drawn.top()), std::abs(r.bottom() - drawn.bottom())});
        };
        const double staticErr = edgeErr(toolRect), meshErr = edgeErr(meshRect),
                     puppetErr = edgeErr(puppetRect);
        const auto rectText = [](const QRectF &r) {
            return "[" + std::to_string(r.left()) + "," + std::to_string(r.top()) + " "
                + std::to_string(r.right()) + "," + std::to_string(r.bottom()) + "]";
        };
        std::cerr << "surface-tool " << label << " timelineSec=" << timelineSec
                  << " parentHit=" << (hit ? 1 : 0)
                  << " parentLocalSec=" << (hit ? hit->clipLocalSeconds : -1.0)
                  << " parentSourceSec=" << (hit ? hit->sourceSeconds : -1.0)
                  << " decodable=" << (hit && hit->decodable ? 1 : 0)
                  << " expectedParentLocalSec=" << expectedParentLocal
                  << " childRenderLocalSec=" << (childHit ? childHit->clipLocalSeconds : -1.0)
                  << " childClampedLocalSec="
                  << qBound(0.0, timelineSec, tl.videoTracks()[2]->clips()[0].effectiveDuration())
                  << " gotDx=" << got.videoDx << " expectedDx=" << expected.videoDx
                  << " sameTransform=" << sameTransform << " drawn=" << rectText(drawn)
                  << " static=" << rectText(toolRect) << " mesh=" << rectText(meshRect)
                  << " puppet=" << rectText(puppetRect) << " errPx(static/mesh/puppet)="
                  << staticErr << "/" << meshErr << "/" << puppetErr << '\n';
        return sameTransform && staticErr <= 1.5 && meshErr <= 1.5 && puppetErr <= 1.5;
    };
    if (!haveMedia) {
        tally.skip(11, "synthetic media could not be encoded");
        tally.skip(12, "synthetic media could not be encoded");
        tally.skip(13, "synthetic media could not be encoded");
    } else {
        // G11 (a): offline parent (regular clip, file missing, videoDx=+0.2).
        // The renderer drops it, so the child is drawn raw.
        bool ok11 = true;
        {
            ClipInfo offline;
            offline.filePath = mediaDir.path() + QStringLiteral("/offline-parent.mp4");
            offline.duration = offline.outPoint = 3.0;
            offline.videoDx = 0.2;
            ok11 = !QFile::exists(offline.filePath)
                && parentCase("G11a offline-parent", {offline}, 0, 1.0, -1.0) && ok11;
        }
        // G11 (b): V2 A [0,2) (dx -0.2) and B [2,4) (dx +0.2) overlap on
        // [1.5, 2.5) through a 1 s Center cross dissolve. At 2.2 s the renderer
        // draws A only: a child of B is drawn raw (although 2.2 s is in B's
        // nominal interval), a child of A is composed with A at clip-local 2.2 s
        // (although 2.2 s is past A's nominal end).
        {
            ClipInfo a = nullParent(-0.2, 0.0, 2.0, 3.0);
            ClipInfo b = nullParent(0.2, 1.0, 3.0, 3.0);
            crossDissolve(a, b, TransitionAlignment::Center);
            ok11 = parentCase("G11b overlap child-of-B", {a, b}, 1, 2.2, -1.0) && ok11;
            ok11 = parentCase("G11b overlap child-of-A", {a, b}, 0, 2.2, 2.2) && ok11;
        }
        tally.gate(11, ok11);
        // G12 (c): TransitionAlignment::Start trail (US-706 r6 layout): the
        // parent (inPoint 1, outPoint 3, dx +0.2) is still drawn at 2.5 s, past
        // its nominal end 2.0 s. Keyed variant: x 0 s 0.0 -> 3 s 0.3, so the
        // parent must be evaluated at clip-local 2.5 s, not clamped to 2.0 s.
        {
            ClipInfo a = nullParent(0.2, 1.0, 3.0, 4.0);
            ClipInfo b = nullParent(0.0, 1.0, 3.0, 4.0);
            crossDissolve(a, b, TransitionAlignment::Start);
            bool ok12 = parentCase("G12 start-trail", {a, b}, 0, 2.5, 2.5);
            KeyframeTrack track(QStringLiteral("motion.position.x"), 0.0);
            track.addKeyframe(0.0, 0.0);
            track.addKeyframe(3.0, 0.3);
            a.keyframes.addTrack(track);
            ok12 = parentCase("G12 start-trail-keyed", {a, b}, 0, 2.5, 2.5) && ok12;
            tally.gate(12, ok12);
        }
        // G13 (US-805 r1): the tool's own clip has x keys (clip-local 0 s -0.3
        // -> 3 s +0.3) and is in its Start trail at 2.5 s. The renderer draws
        // it at clip-local 2.5 s (x 0.2), not the nominal-end clamp 2.0 s (x 0.1,
        // 40 px off). Unparented, and parented to a V2 Null (dx +0.1).
        {
            KeyframeTrack keys(QStringLiteral("motion.position.x"), 0.0);
            keys.addKeyframe(0.0, -0.3);
            keys.addKeyframe(3.0, 0.3);
            bool ok13 = parentCase("G13 keyed-child-trail unparented", {}, 0, 2.5, -1.0, &keys);
            ok13 = parentCase("G13 keyed-child-trail parented",
                              {nullParent(0.1, 0.0, 3.0, 3.0)}, 0, 2.5, 2.5, &keys) && ok13;
            tally.gate(13, ok13);
        }
    }

    return tally.finish("surface-tool");
}
