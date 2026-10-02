#include "SurfaceTool.h"
#include "GLPreview.h"
#include "Timeline.h"
#include "TrackMatteKey.h"
#include "playback/SnsFit.h"
#include "clipanim/ClipAnim.h"
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QMouseEvent>
#include <QApplication>
#include <QtMath>
#include <cmath>
#include <algorithm>
#include <functional>

namespace veditor { bool parentingEnabledFromEnv(); } // TimelineFrameRenderer.cpp

SurfaceTool::SurfaceTool(GLPreview *preview, QObject *parent)
    : QObject(parent)
    , m_preview(preview)
    , m_sourceSize(1920, 1080)
{
    m_quad = defaultQuad();
}

void SurfaceTool::setSourceSize(QSize size)
{
    if (size.isEmpty() || size == m_sourceSize)
        return;
    m_sourceSize = size;
}

void SurfaceTool::setQuad(const planartrack::Quad &quad)
{
    QPointF tlUv = sourcePxToUv(QPointF(quad.tl.x, quad.tl.y));
    QPointF trUv = sourcePxToUv(QPointF(quad.tr.x, quad.tr.y));
    QPointF brUv = sourcePxToUv(QPointF(quad.br.x, quad.br.y));
    QPointF blUv = sourcePxToUv(QPointF(quad.bl.x, quad.bl.y));
    m_quad.tl = { tlUv.x(), tlUv.y() };
    m_quad.tr = { trUv.x(), trUv.y() };
    m_quad.br = { brUv.x(), brUv.y() };
    m_quad.bl = { blUv.x(), blUv.y() };
    if (m_preview)
        m_preview->update();
}

planartrack::Quad SurfaceTool::currentQuad() const
{
    planartrack::Quad result;
    result.tl.x = m_quad.tl.x * m_sourceSize.width();
    result.tl.y = m_quad.tl.y * m_sourceSize.height();
    result.tr.x = m_quad.tr.x * m_sourceSize.width();
    result.tr.y = m_quad.tr.y * m_sourceSize.height();
    result.br.x = m_quad.br.x * m_sourceSize.width();
    result.br.y = m_quad.br.y * m_sourceSize.height();
    result.bl.x = m_quad.bl.x * m_sourceSize.width();
    result.bl.y = m_quad.bl.y * m_sourceSize.height();
    return result;
}

void SurfaceTool::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (m_preview)
        m_preview->update();
}

QPointF SurfaceTool::uvToSourcePx(const QPointF &uv) const
{
    return QPointF(uv.x() * m_sourceSize.width(),
                   uv.y() * m_sourceSize.height());
}

QPointF SurfaceTool::sourcePxToUv(const QPointF &px) const
{
    if (m_sourceSize.isEmpty())
        return QPointF(0.0, 0.0);
    return QPointF(px.x() / m_sourceSize.width(),
                   px.y() / m_sourceSize.height());
}

void SurfaceTool::setClipTransform(const clipgeom::ClipTransform &transform, QSize canvas,
                                   QSize sourceSize, bool fitContain, bool fitCover)
{
    m_clipTransform = transform;
    m_clipCanvas = canvas;
    m_clipSourceSize = sourceSize;
    m_hasClipTransform = false;
    m_clipMapValid = true;
    m_placement = m_inversePlacement = QTransform();
    if (canvas.isEmpty())
        return;
    // Source UV -> fitted-frame UV, with the same geometry snsfit::maybeFit
    // draws (cover wins over contain; matching aspects are left alone).
    QTransform fit;
    const bool fitted = snsfit::shouldFit(fitContain, fitCover, canvas, sourceSize);
    if (fitted) {
        const double aspect = canvas.width() / static_cast<double>(canvas.height());
        const double sw = sourceSize.width(), sh = sourceSize.height();
        if (fitCover) {
            const snsfit::CoverGeom g = snsfit::coverGeom(sourceSize, aspect);
            fit = QTransform(sw / g.croppedSize.width(), 0, 0, sh / g.croppedSize.height(),
                             -g.srcCropRect.x() / double(g.croppedSize.width()),
                             -g.srcCropRect.y() / double(g.croppedSize.height()));
        } else {
            const snsfit::ContainGeom g = snsfit::containGeom(sourceSize, aspect);
            fit = QTransform(sw / g.intermediateCanvas.width(), 0, 0,
                             sh / g.intermediateCanvas.height(),
                             g.contentRect.x() / double(g.intermediateCanvas.width()),
                             g.contentRect.y() / double(g.intermediateCanvas.height()));
        }
    }
    // Identity keeps the plain view normalization bit-for-bit.
    if (!fitted && transform.videoScale == 1.0 && transform.videoDx == 0.0
        && transform.videoDy == 0.0 && transform.rotationDeg == 0.0)
        return;
    const QTransform placement = clipgeom::resolveTransform(transform, canvas);
    bool invertible = false;
    placement.inverted(&invertible);
    if (!invertible) {
        m_clipMapValid = false;
        return;
    }
    // renderLayer scales the (fitted) frame to the canvas, so fitted UV * canvas
    // is the canvas-sized source pixel resolveTransform expects.
    const QTransform toPixels = QTransform::fromScale(canvas.width(), canvas.height());
    const QTransform toUv = QTransform::fromScale(1.0 / canvas.width(), 1.0 / canvas.height());
    m_placement = fit * toPixels * placement * toUv;
    m_inversePlacement = m_placement.inverted(&invertible);
    if (!invertible) {
        m_clipMapValid = false;
        return;
    }
    m_hasClipTransform = true;
}

void SurfaceTool::syncClipTransform(const Timeline &timeline, int trackIdx, int clipIdx)
{
    const QSize canvas = m_canvasProvider ? m_canvasProvider() : m_clipCanvas;
    const auto &tracks = timeline.videoTracks();
    if (canvas.isEmpty() || trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx]
        || clipIdx < 0 || clipIdx >= tracks[trackIdx]->clips().size())
        return;
    const ClipInfo &clip = tracks[trackIdx]->clips()[clipIdx];
    setClipTransform(renderedClipTransform(timeline, trackIdx, clipIdx,
                                           timeline.playheadPosition(), canvas),
                     canvas, m_clipSourceSize, clip.fitContain, clip.fitCover);
}

clipgeom::ClipTransform SurfaceTool::renderedClipTransform(const Timeline &timeline, int trackIdx,
                                                           int clipIdx, double timelineSec, QSize canvas)
{
    const auto &tracks = timeline.videoTracks();
    if (trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx]
        || clipIdx < 0 || clipIdx >= tracks[trackIdx]->clips().size())
        return {};
    const auto clipStart = [&](int t, int c) {
        const auto &clips = tracks[t]->clips();
        double start = 0.0;
        for (int i = 0; i < c; ++i)
            start += qMax(0.0, clips[i].leadInSec) + clips[i].effectiveDuration();
        return start + qMax(0.0, clips[c].leadInSec);
    };
    const auto rawAt = [&](int t, int c) {
        const ClipInfo &clip = tracks[t]->clips()[c];
        return clipanim::effectiveTransformAt(
            clip, qBound(0.0, timelineSec - clipStart(t, c), clip.effectiveDuration()));
    };
    const QHash<QString, QString> parents = veditor::parentingEnabledFromEnv()
        ? timeline.clipParentEntries() : QHash<QString, QString>{};
    // Same guards as the renderer's parent resolve: the parent must be a
    // non-base layer drawn at timelineSec, else the child renders raw.
    const auto activeParent = [&](const QString &key, int *t, int *c) {
        const QStringList parts = key.split(QLatin1Char(':'));
        bool okTrack = false, okClip = false;
        *t = parts.size() == 2 ? parts[0].toInt(&okTrack) : -1;
        *c = parts.size() == 2 ? parts[1].toInt(&okClip) : -1;
        if (!okTrack || !okClip || *t <= 0 || *t >= tracks.size() || !tracks[*t]
            || tracks[*t]->isHidden() || *c < 0 || *c >= tracks[*t]->clips().size())
            return false;
        const ClipInfo &clip = tracks[*t]->clips()[*c];
        const double start = clipStart(*t, *c);
        return timelineSec >= start && timelineSec < start + clip.effectiveDuration()
            && !clip.isAdjustment
            && (clip.visible || clipgeom::isNullObjectFilePath(clip.filePath));
    };
    std::function<bool(int, int, int, QStringList &, clipgeom::ClipTransform *)> world =
        [&](int t, int c, int depth, QStringList &stack, clipgeom::ClipTransform *out) {
            *out = rawAt(t, c);
            if (depth > 8)
                return true;
            const QString key = trackMatteClipKey(t, c);
            const QString parentKey = parents.value(key);
            if (parentKey.isEmpty())
                return true;
            int parentTrack = -1, parentClip = -1;
            if (parentKey == key || stack.contains(parentKey)
                || !activeParent(parentKey, &parentTrack, &parentClip))
                return false;
            stack.append(key);
            clipgeom::ClipTransform parentWorld;
            const bool ok = world(parentTrack, parentClip, depth + 1, stack, &parentWorld);
            stack.removeLast();
            if (ok)
                *out = clipgeom::composeParented(*out, parentWorld, canvas);
            return ok;
        };
    clipgeom::ClipTransform result;
    QStringList stack;
    if (!world(trackIdx, clipIdx, 0, stack, &result))
        result = rawAt(trackIdx, clipIdx);
    return result;
}

QPointF SurfaceTool::toWidget(const QPointF &uv, const QRectF &viewRect) const
{
    const QPointF canvasUv = m_hasClipTransform ? m_placement.map(uv) : uv;
    return viewRect.topLeft() + QPointF(canvasUv.x() * viewRect.width(),
                                        canvasUv.y() * viewRect.height());
}

QPointF SurfaceTool::widgetToUv(const QPointF &widgetPos, const QRectF &viewRect) const
{
    if (viewRect.isEmpty())
        return QPointF(0.0, 0.0);
    const QPointF canvasUv((widgetPos.x() - viewRect.x()) / viewRect.width(),
                           (widgetPos.y() - viewRect.y()) / viewRect.height());
    return m_hasClipTransform ? m_inversePlacement.map(canvasUv) : canvasUv;
}

QPointF SurfaceTool::widgetDeltaToUv(const QPointF &delta, const QRectF &viewRect) const
{
    const QPointF canvasDelta(delta.x() / viewRect.width(), delta.y() / viewRect.height());
    if (!m_hasClipTransform)
        return canvasDelta;
    // Linear part only: a drag is a displacement, not a position.
    return QPointF(m_inversePlacement.m11() * canvasDelta.x() + m_inversePlacement.m21() * canvasDelta.y(),
                   m_inversePlacement.m12() * canvasDelta.x() + m_inversePlacement.m22() * canvasDelta.y());
}

QPolygonF SurfaceTool::sourceCircleToWidget(const QPointF &centerUv, double radius,
                                            const QRectF &viewRect) const
{
    // Unknown source size: the frame is assumed to have the canvas (or view) aspect.
    const QSizeF frame = !m_clipSourceSize.isEmpty() ? QSizeF(m_clipSourceSize)
        : !m_clipCanvas.isEmpty() ? QSizeF(m_clipCanvas) : viewRect.size();
    QPolygonF outline;
    if (frame.isEmpty())
        return outline;
    const double shortSide = qMin(frame.width(), frame.height());
    const double rx = radius * shortSide / frame.width();
    const double ry = radius * shortSide / frame.height();
    constexpr int kSegments = 96;
    for (int i = 0; i < kSegments; ++i) {
        const double a = 2.0 * M_PI * i / kSegments;
        outline << toWidget(centerUv + QPointF(rx * std::cos(a), ry * std::sin(a)), viewRect);
    }
    return outline;
}

QPoint SurfaceTool::uvToWidget(const QPointF &uv, const QRectF &letterbox) const
{
    const QPointF p = toWidget(uv, letterbox);
    return QPoint(static_cast<int>(p.x()), static_cast<int>(p.y()));
}

int SurfaceTool::hitTestCorner(const QPoint &widgetPos, const QRectF &letterbox) const
{
    if (!m_clipMapValid)
        return -1;
    const planartrack::Point2D uv[4] = { m_quad.tl, m_quad.tr, m_quad.br, m_quad.bl };
    const int hitRadius = 10; // pixel radius for corner hit test
    for (int i = 0; i < 4; ++i) {
        QPoint cp = uvToWidget(QPointF(uv[i].x, uv[i].y), letterbox);
        if (QPoint(widgetPos.x() - cp.x(), widgetPos.y() - cp.y()).manhattanLength() <= hitRadius)
            return i;
    }
    return -1;
}

planartrack::Quad SurfaceTool::defaultQuad() const
{
    const double inset = 0.10; // 10% from each edge
    planartrack::Quad q;
    q.tl.x = inset;          q.tl.y = inset;
    q.tr.x = 1.0 - inset;    q.tr.y = inset;
    q.br.x = 1.0 - inset;    q.br.y = 1.0 - inset;
    q.bl.x = inset;          q.bl.y = 1.0 - inset;
    return q;
}

void SurfaceTool::applyCtrlSnap(QPointF &uv, int draggedIndex, Qt::KeyboardModifiers modifiers) const
{
    if (!(modifiers & Qt::ControlModifier))
        return;

    const planartrack::Point2D *corners[4] = { &m_quad.tl, &m_quad.tr, &m_quad.br, &m_quad.bl };
    const double snapThresholdPx = 8.0;

    // We need the letterbox to convert UV delta to widget pixels.
    // Use a conservative approach: snap based on UV fraction.
    // 8 preview pixels = 8 / min(letterbox dimension) in UV.
    // Since we don't have the letterbox here, we'll use a small UV epsilon.
    // The caller (GLPreview) will pass the letterbox, but for simplicity
    // we compute snap in widget space in handleMouseMove instead.
    Q_UNUSED(uv);
    Q_UNUSED(draggedIndex);
    Q_UNUSED(snapThresholdPx);
    Q_UNUSED(corners);
}

void SurfaceTool::paintOverlay(QPainter &painter, const QRectF &letterbox)
{
    if (!m_enabled || letterbox.isEmpty() || !m_clipMapValid)
        return;

    const planartrack::Point2D uv[4] = { m_quad.tl, m_quad.tr, m_quad.br, m_quad.bl };
    QPoint pts[4];
    for (int i = 0; i < 4; ++i)
        pts[i] = uvToWidget(QPointF(uv[i].x, uv[i].y), letterbox);

    // Draw connecting lines
    QPen linePen(QColor(0, 200, 255, 220));
    linePen.setWidth(2);
    painter.setPen(linePen);
    painter.setBrush(Qt::NoBrush);

    // Draw quad edges: TL->TR->BR->BL->TL
    const int edges[4][2] = { {0,1}, {1,2}, {2,3}, {3,0} };
    for (int e = 0; e < 4; ++e)
        painter.drawLine(pts[edges[e][0]], pts[edges[e][1]]);

    // Draw corner circles
    painter.setPen(QPen(QColor(255, 255, 255, 240), 2));
    painter.setBrush(QBrush(QColor(0, 150, 255, 200)));
    const int cornerRadius = 6;
    for (int i = 0; i < 4; ++i)
        painter.drawEllipse(pts[i], cornerRadius, cornerRadius);
}

bool SurfaceTool::handleMousePress(const QPoint &widgetPos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(modifiers);
    if (!m_enabled)
        return false;

    const QRectF letterbox = m_preview ? m_preview->letterboxRect() : m_viewRect;
    if (letterbox.isEmpty()) return false;
    const int corner = hitTestCorner(widgetPos, letterbox);

    if (button == Qt::RightButton && corner >= 0) {
        // Show context menu to reset this corner
        if (m_contextMenu)
            m_contextMenu->deleteLater();
        m_contextMenu = new QMenu(m_preview);
        m_contextMenu->addAction(tr("Reset corner to default position"), this, [this, corner]() {
            planartrack::Quad def = defaultQuad();
            const planartrack::Point2D *defaults[4] = { &def.tl, &def.tr, &def.br, &def.bl };
            planartrack::Point2D *corners[4] = { &m_quad.tl, &m_quad.tr, &m_quad.br, &m_quad.bl };
            *corners[corner] = *defaults[corner];
            if (m_preview)
                m_preview->update();
            emit cornersChanged(currentQuad());
        });
        m_contextMenu->popup(m_preview ? m_preview->mapToGlobal(widgetPos) : widgetPos);
        return true;
    }

    if (button == Qt::LeftButton && corner >= 0) {
        m_draggingCorner = corner;
        return true;
    }

    return false;
}

bool SurfaceTool::handleMouseMove(const QPoint &widgetPos, Qt::KeyboardModifiers modifiers)
{
    if (!m_enabled || m_draggingCorner < 0)
        return false;

    const QRectF letterbox = m_preview ? m_preview->letterboxRect() : m_viewRect;
    if (letterbox.isEmpty()) return false;
    QPointF newUv = widgetToUv(widgetPos, letterbox);

    // Clamp to [0,1]
    newUv.setX(std::clamp(newUv.x(), 0.0, 1.0));
    newUv.setY(std::clamp(newUv.y(), 0.0, 1.0));

    // Ctrl-snap: align horizontally or vertically with nearest other corner
    // within 8 preview pixels of an axis match
    if (modifiers & Qt::ControlModifier) {
        const planartrack::Point2D *others[4] = { &m_quad.tl, &m_quad.tr, &m_quad.br, &m_quad.bl };
        const double snapPx = 8.0;
        const double lbW = letterbox.width();
        const double lbH = letterbox.height();

        for (int i = 0; i < 4; ++i) {
            if (i == m_draggingCorner) continue;
            const planartrack::Point2D &other = *others[i];

            // Check horizontal alignment (same Y)
            double otherWidgetY = letterbox.y() + other.y * lbH;
            double newWidgetY = letterbox.y() + newUv.y() * lbH;
            if (std::abs(newWidgetY - otherWidgetY) < snapPx)
                newUv.setY(other.y);

            // Check vertical alignment (same X)
            double otherWidgetX = letterbox.x() + other.x * lbW;
            double newWidgetX = letterbox.x() + newUv.x() * lbW;
            if (std::abs(newWidgetX - otherWidgetX) < snapPx)
                newUv.setX(other.x);
        }
    }

    // Update the dragged corner
    planartrack::Point2D *corners[4] = { &m_quad.tl, &m_quad.tr, &m_quad.br, &m_quad.bl };
    *corners[m_draggingCorner] = { newUv.x(), newUv.y() };

    if (m_preview)
        m_preview->update();

    return true;
}

bool SurfaceTool::handleMouseRelease(const QPoint &widgetPos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(widgetPos);
    Q_UNUSED(button);
    Q_UNUSED(modifiers);

    if (m_draggingCorner >= 0) {
        m_draggingCorner = -1;
        emit cornersChanged(currentQuad());
        return true;
    }
    return false;
}
