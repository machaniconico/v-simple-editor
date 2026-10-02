// US-707: SurfaceTool / MeshEditTool / PuppetPinTool map screen <-> source UV
// through the same clip fit (snsfit) + placement (clipgeom::resolveTransform)
// renderFrameAt applies after the mesh warp / puppet pins.
#include "../MeshEditTool.h"
#include "../PuppetPinTool.h"
#include "../SurfaceTool.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../WarpDistortion.h"
#include "../playback/SnsFit.h"
#include "SelftestGate.h"

#include <QImage>
#include <QPainter>
#include <cmath>
#include <iostream>
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

    return tally.finish("surface-tool");
}
