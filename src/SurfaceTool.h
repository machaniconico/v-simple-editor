#pragma once

#include <QObject>
#include <QSize>
#include <QRectF>
#include <QPolygonF>
class QPainter;
#include <QPoint>
#include <QMenu>
#include <QTransform>
#include <functional>
#include "ClipGeometry.h"
#include "PlanarTracker.h"

class GLPreview;
class Timeline;

class SurfaceTool : public QObject
{
    Q_OBJECT

public:
    explicit SurfaceTool(GLPreview *preview, QObject *parent = nullptr);

    ~SurfaceTool() override = default;
    void setViewRect(const QRectF &rect) { m_viewRect = rect; }
    virtual int hitTestCorner(const QPoint &widgetPos, const QRectF &letterbox) const;

    // Clip placement renderFrameAt applies after mesh warp / puppet pins:
    // snsfit contain/cover of the `sourceSize` frame (when the flags ask and the
    // aspect differs), then resolveTransform on `canvas`. Tool UV stays source
    // UV; hit test, drag and overlay drawing all go through this one pair of maps.
    void setClipTransform(const clipgeom::ClipTransform &transform, QSize canvas,
                          QSize sourceSize = QSize(), bool fitContain = false, bool fitCover = false);
    // Current project canvas, re-read on every syncClipTransform (repaint).
    void setCanvasProvider(std::function<QSize()> provider) { m_canvasProvider = std::move(provider); }
    // False when the placement cannot be inverted (e.g. scale 0): the clip has
    // no visible area, so handles are neither drawn nor hit.
    bool clipMapValid() const { return m_clipMapValid; }
    // What renderFrameAt places (trackIdx, clipIdx) with at timelineSec:
    // effectiveTransformAt, then the parent chain via composeParented.
    static clipgeom::ClipTransform renderedClipTransform(const Timeline &timeline, int trackIdx,
                                                         int clipIdx, double timelineSec, QSize canvas);
    QPointF widgetToUv(const QPointF &widgetPos, const QRectF &viewRect) const;
    QPointF toWidget(const QPointF &uv, const QRectF &viewRect) const;

    void setSourceSize(QSize size);
    void setQuad(const planartrack::Quad &quad);
    planartrack::Quad currentQuad() const;
    void setEnabled(bool enabled);
    virtual bool isEnabled() const { return m_enabled; }

    // Called by GLPreview to paint the overlay via QPainter
    virtual void paintOverlay(QPainter &painter, const QRectF &letterbox);

    // Mouse event dispatch — returns true if the event was consumed
    virtual bool handleMousePress(const QPoint &widgetPos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    virtual bool handleMouseMove(const QPoint &widgetPos, Qt::KeyboardModifiers modifiers);
    virtual bool handleMouseRelease(const QPoint &widgetPos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);

signals:
    void cornersChanged(const planartrack::Quad &quad);

protected:
    QPointF widgetDeltaToUv(const QPointF &delta, const QRectF &viewRect) const;
    // Outline of a circle of `radius` x (source short side) around centerUv, as
    // the puppet warp measures it (WarpDistortion.h applyNormalized), mapped
    // point by point through the same fit + placement (an ellipse in general).
    QPolygonF sourceCircleToWidget(const QPointF &centerUv, double radius,
                                   const QRectF &viewRect) const;
    // Re-reads the placement at the playhead once setClipTransform gave a canvas.
    void syncClipTransform(const Timeline &timeline, int trackIdx, int clipIdx);

private:
    enum CornerIndex { CornerTL = 0, CornerTR = 1, CornerBR = 2, CornerBL = 3 };

    QPoint uvToWidget(const QPointF &uv, const QRectF &letterbox) const;
    QPointF uvToSourcePx(const QPointF &uv) const;
    QPointF sourcePxToUv(const QPointF &px) const;
    planartrack::Quad defaultQuad() const;
    void applyCtrlSnap(QPointF &uv, int draggedIndex, Qt::KeyboardModifiers modifiers) const;

    QRectF m_viewRect;
    GLPreview *m_preview;
    QSize m_sourceSize;
    planartrack::Quad m_quad;       // stored in normalized UV [0,1]x[0,1]
    bool m_enabled = false;
    int m_draggingCorner = -1;      // -1 = idle
    QMenu *m_contextMenu = nullptr;
    clipgeom::ClipTransform m_clipTransform;
    QSize m_clipCanvas;
    QSize m_clipSourceSize;
    std::function<QSize()> m_canvasProvider;
    bool m_hasClipTransform = false;    // false = identity: plain view normalization
    bool m_clipMapValid = true;
    QTransform m_placement;             // source UV -> canvas UV
    QTransform m_inversePlacement;      // canvas UV -> source UV
};
