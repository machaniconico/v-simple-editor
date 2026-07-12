#include "SurfaceTool.h"
#include "GLPreview.h"
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QMouseEvent>
#include <QApplication>
#include <cmath>

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

QPoint SurfaceTool::uvToWidget(const QPointF &uv, const QRectF &letterbox) const
{
    return QPoint(static_cast<int>(letterbox.x() + uv.x() * letterbox.width()),
                  static_cast<int>(letterbox.y() + uv.y() * letterbox.height()));
}

QPointF SurfaceTool::widgetToUv(const QPoint &widgetPos, const QRectF &letterbox) const
{
    if (letterbox.isEmpty())
        return QPointF(0.0, 0.0);
    return QPointF(static_cast<double>(widgetPos.x() - letterbox.x()) / letterbox.width(),
                   static_cast<double>(widgetPos.y() - letterbox.y()) / letterbox.height());
}

int SurfaceTool::hitTestCorner(const QPoint &widgetPos, const QRectF &letterbox) const
{
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
    if (!m_enabled || letterbox.isEmpty())
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

    const QRectF letterbox = m_preview->letterboxRect();
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
        m_contextMenu->popup(m_preview->mapToGlobal(widgetPos));
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

    const QRectF letterbox = m_preview->letterboxRect();
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
