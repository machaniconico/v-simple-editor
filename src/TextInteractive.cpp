#include "TextInteractive.h"
#include <QFontMetrics>
#include <cmath>

TextInteractive::TextInteractive(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
}

void TextInteractive::setOverlays(const QVector<EnhancedTextOverlay> &overlays)
{
    m_overlays = overlays;
    update();
}

void TextInteractive::setSelectedIndex(int index)
{
    m_selectedIndex = index;
    update();
}

QRect TextInteractive::overlayScreenRect(int index) const
{
    if (index < 0 || index >= m_overlays.size() || m_videoRect.isEmpty())
        return {};

    const auto &o = m_overlays[index];
    QFontMetrics fm(o.font);
    QRect textBounds = fm.boundingRect(QRect(0, 0, m_videoRect.width() - 40, 0),
        o.alignment | Qt::TextWordWrap, o.text);

    int w = static_cast<int>(textBounds.width() * o.scale);
    int h = static_cast<int>(textBounds.height() * o.scale);
    int cx = m_videoRect.x() + static_cast<int>(o.x * m_videoRect.width());
    int cy = m_videoRect.y() + static_cast<int>(o.y * m_videoRect.height());

    return QRect(cx - w / 2, cy - h / 2, w, h);
}

QPoint TextInteractive::rotateHandlePos(int index) const
{
    QRect rect = overlayScreenRect(index);
    return QPoint(rect.center().x(), rect.top() - ROTATE_HANDLE_DISTANCE);
}

TextInteractive::Handle TextInteractive::hitTest(const QPoint &pos, int overlayIndex) const
{
    QRect rect = overlayScreenRect(overlayIndex);
    if (rect.isEmpty()) return None;

    int hs = HANDLE_SIZE;

    // Rotate handle
    QPoint rh = rotateHandlePos(overlayIndex);
    if (QRect(rh.x() - hs, rh.y() - hs, hs * 2, hs * 2).contains(pos))
        return Rotate;

    // Corner handles
    if (QRect(rect.left() - hs, rect.top() - hs, hs * 2, hs * 2).contains(pos))
        return TopLeft;
    if (QRect(rect.right() - hs, rect.top() - hs, hs * 2, hs * 2).contains(pos))
        return TopRight;
    if (QRect(rect.left() - hs, rect.bottom() - hs, hs * 2, hs * 2).contains(pos))
        return BottomLeft;
    if (QRect(rect.right() - hs, rect.bottom() - hs, hs * 2, hs * 2).contains(pos))
        return BottomRight;

    // Move (inside rect)
    if (rect.adjusted(-4, -4, 4, 4).contains(pos))
        return Move;

    return None;
}

void TextInteractive::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Draw selection handles for selected overlay
    if (m_selectedIndex >= 0 && m_selectedIndex < m_overlays.size()) {
        QRect rect = overlayScreenRect(m_selectedIndex);
        if (!rect.isEmpty()) {
            // Selection border
            painter.setPen(QPen(QColor(0, 150, 255), 2, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect);

            // Corner handles
            painter.setPen(QPen(Qt::white, 1));
            painter.setBrush(QColor(0, 150, 255));
            int hs = HANDLE_SIZE;
            painter.drawRect(rect.left() - hs/2, rect.top() - hs/2, hs, hs);
            painter.drawRect(rect.right() - hs/2, rect.top() - hs/2, hs, hs);
            painter.drawRect(rect.left() - hs/2, rect.bottom() - hs/2, hs, hs);
            painter.drawRect(rect.right() - hs/2, rect.bottom() - hs/2, hs, hs);

            // Rotate handle
            QPoint rh = rotateHandlePos(m_selectedIndex);
            painter.setPen(QPen(QColor(0, 150, 255), 1));
            painter.drawLine(rect.center().x(), rect.top(), rh.x(), rh.y());
            painter.setBrush(QColor(255, 150, 0));
            painter.drawEllipse(rh, hs/2 + 1, hs/2 + 1);
        }
    }
}

void TextInteractive::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;

    // Check selected overlay handles first
    if (m_selectedIndex >= 0) {
        Handle h = hitTest(event->pos(), m_selectedIndex);
        if (h != None) {
            m_activeHandle = h;
            m_dragStart = event->pos();
            m_dragStartX = m_overlays[m_selectedIndex].x;
            m_dragStartY = m_overlays[m_selectedIndex].y;
            m_dragStartScale = m_overlays[m_selectedIndex].scale;
            m_dragStartRotation = m_overlays[m_selectedIndex].rotation;
            return;
        }
    }

    // Check all overlays for click
    for (int i = m_overlays.size() - 1; i >= 0; --i) {
        QRect rect = overlayScreenRect(i);
        if (rect.adjusted(-4, -4, 4, 4).contains(event->pos())) {
            m_selectedIndex = i;
            m_activeHandle = Move;
            m_dragStart = event->pos();
            m_dragStartX = m_overlays[i].x;
            m_dragStartY = m_overlays[i].y;
            emit overlaySelected(i);
            update();
            return;
        }
    }

    // Clicked on nothing
    m_selectedIndex = -1;
    m_activeHandle = None;
    emit overlaySelected(-1);
    update();
}

void TextInteractive::mouseMoveEvent(QMouseEvent *event)
{
    if (m_activeHandle == None || m_selectedIndex < 0 || m_videoRect.isEmpty()) {
        // Update cursor based on hover
        if (m_selectedIndex >= 0) {
            Handle h = hitTest(event->pos(), m_selectedIndex);
            switch (h) {
            case Move:        setCursor(Qt::SizeAllCursor); break;
            case TopLeft:     case BottomRight: setCursor(Qt::SizeFDiagCursor); break;
            case TopRight:    case BottomLeft:  setCursor(Qt::SizeBDiagCursor); break;
            case Rotate:      setCursor(Qt::CrossCursor); break;
            default:          setCursor(Qt::ArrowCursor); break;
            }
        }
        return;
    }

    QPoint delta = event->pos() - m_dragStart;

    switch (m_activeHandle) {
    case Move: {
        double dx = static_cast<double>(delta.x()) / m_videoRect.width();
        double dy = static_cast<double>(delta.y()) / m_videoRect.height();
        double newX = qBound(0.0, m_dragStartX + dx, 1.0);
        double newY = qBound(0.0, m_dragStartY + dy, 1.0);
        emit overlayMoved(m_selectedIndex, newX, newY);
        break;
    }
    case TopLeft: case TopRight: case BottomLeft: case BottomRight: {
        QRect origRect = overlayScreenRect(m_selectedIndex);
        if (origRect.isEmpty()) break;
        double dist = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
        double sign = (delta.x() + delta.y() > 0) ? 1.0 : -1.0;
        double scaleFactor = 1.0 + sign * dist / 200.0;
        double newScale = qBound(0.1, m_dragStartScale * scaleFactor, 5.0);
        emit overlayResized(m_selectedIndex, newScale);
        break;
    }
    case Rotate: {
        QRect rect = overlayScreenRect(m_selectedIndex);
        QPoint center = rect.center();
        double angle = std::atan2(event->pos().y() - center.y(),
                                   event->pos().x() - center.x());
        double startAngle = std::atan2(m_dragStart.y() - center.y(),
                                        m_dragStart.x() - center.x());
        double deltaAngle = (angle - startAngle) * 180.0 / M_PI;
        emit overlayRotated(m_selectedIndex, m_dragStartRotation + deltaAngle);
        break;
    }
    default: break;
    }

    update();
}

void TextInteractive::mouseReleaseEvent(QMouseEvent *)
{
    m_activeHandle = None;
}

void TextInteractive::mouseDoubleClickEvent(QMouseEvent *event)
{
    for (int i = m_overlays.size() - 1; i >= 0; --i) {
        QRect rect = overlayScreenRect(i);
        if (rect.adjusted(-4, -4, 4, 4).contains(event->pos())) {
            emit overlayDoubleClicked(i);
            return;
        }
    }
}
