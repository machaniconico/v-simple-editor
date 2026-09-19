#pragma once

#include <QObject>
#include <QSize>
#include <QRectF>
class QPainter;
#include <QPoint>
#include <QMenu>
#include "PlanarTracker.h"

class GLPreview;

class SurfaceTool : public QObject
{
    Q_OBJECT

public:
    explicit SurfaceTool(GLPreview *preview, QObject *parent = nullptr);

    ~SurfaceTool() override = default;
    void setViewRect(const QRectF &rect) { m_viewRect = rect; }
    virtual int hitTestCorner(const QPoint &widgetPos, const QRectF &letterbox) const;

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

private:
    enum CornerIndex { CornerTL = 0, CornerTR = 1, CornerBR = 2, CornerBL = 3 };

    QPoint uvToWidget(const QPointF &uv, const QRectF &letterbox) const;
    QPointF widgetToUv(const QPoint &widgetPos, const QRectF &letterbox) const;
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
};
