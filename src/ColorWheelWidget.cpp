#include "ColorWheelWidget.h"
#include <QPainterPath>
#include <QtMath>
#include <QConicalGradient>
#include <QRadialGradient>

ColorWheelWidget::ColorWheelWidget(const QString &label, QWidget *parent)
    : QWidget(parent), m_label(label)
{
    setMouseTracking(false);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void ColorWheelWidget::setColor(double r, double g, double b)
{
    if (m_r == r && m_g == g && m_b == b)
        return;
    m_r = qBound(-1.0, r, 1.0);
    m_g = qBound(-1.0, g, 1.0);
    m_b = qBound(-1.0, b, 1.0);
    update();
}

QRectF ColorWheelWidget::wheelRect() const
{
    double labelH = m_label.isEmpty() ? 0.0 : 18.0;
    double side = qMin(static_cast<double>(width()), static_cast<double>(height()) - labelH);
    double margin = 4.0;
    side -= margin * 2;
    double x = (width() - side) / 2.0;
    double y = margin;
    return QRectF(x, y, side, side);
}

double ColorWheelWidget::wheelRadius() const
{
    return wheelRect().width() / 2.0;
}

QPointF ColorWheelWidget::colorToPos() const
{
    QRectF wr = wheelRect();
    QPointF center = wr.center();
    double rad = wheelRadius();
    double dx = m_r;
    double dy = m_g;
    double dist = qSqrt(dx * dx + dy * dy);
    if (dist > 1.0) {
        dx /= dist;
        dy /= dist;
    }

    // Keep this as the exact inverse of updateFromPos() for representable
    // wheel colors so the indicator stays under the cursor while dragging.
    double x = center.x() + dx * rad * 0.8;
    double y = center.y() - dy * rad * 0.8;
    return QPointF(x, y);
}

void ColorWheelWidget::updateFromPos(const QPointF &pos)
{
    QRectF wr = wheelRect();
    QPointF center = wr.center();
    double rad = wheelRadius();

    double dx = (pos.x() - center.x()) / (rad * 0.8);
    double dy = -(pos.y() - center.y()) / (rad * 0.8);

    // Clamp to circle
    double dist = qSqrt(dx * dx + dy * dy);
    if (dist > 1.0) {
        dx /= dist;
        dy /= dist;
    }

    // Direct mapping: x-axis → R, y-axis → G, B inferred as complement
    m_r = qBound(-1.0, dx, 1.0);
    m_g = qBound(-1.0, dy, 1.0);
    m_b = qBound(-1.0, -(dx + dy) / 2.0, 1.0);

    update();
    emit colorChanged(m_r, m_g, m_b);
}

void ColorWheelWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QRectF wr = wheelRect();
    QPointF center = wr.center();
    double rad = wheelRadius();

    // Draw wheel background with conical color gradient
    QConicalGradient cg(center, 0);
    cg.setColorAt(0.0,    QColor(255, 0, 0, 180));      // Red
    cg.setColorAt(1.0/6,  QColor(255, 255, 0, 180));    // Yellow
    cg.setColorAt(2.0/6,  QColor(0, 255, 0, 180));      // Green
    cg.setColorAt(3.0/6,  QColor(0, 255, 255, 180));    // Cyan
    cg.setColorAt(4.0/6,  QColor(0, 0, 255, 180));      // Blue
    cg.setColorAt(5.0/6,  QColor(255, 0, 255, 180));    // Magenta
    cg.setColorAt(1.0,    QColor(255, 0, 0, 180));      // Red (wrap)

    p.setBrush(cg);
    p.setPen(QPen(QColor(60, 60, 60), 1.5));
    p.drawEllipse(wr);

    // Overlay radial gradient for center fade-to-gray
    QRadialGradient rg(center, rad);
    rg.setColorAt(0.0, QColor(80, 80, 80, 220));
    rg.setColorAt(0.7, QColor(80, 80, 80, 60));
    rg.setColorAt(1.0, QColor(80, 80, 80, 0));
    p.setBrush(rg);
    p.setPen(Qt::NoPen);
    p.drawEllipse(wr);

    // Draw crosshairs
    p.setPen(QPen(QColor(100, 100, 100, 120), 1));
    p.drawLine(QPointF(center.x() - rad, center.y()),
               QPointF(center.x() + rad, center.y()));
    p.drawLine(QPointF(center.x(), center.y() - rad),
               QPointF(center.x(), center.y() + rad));

    // Draw current position indicator
    QPointF dot = colorToPos();
    double dotR = 5.0;

    // Outer ring
    p.setPen(QPen(Qt::black, 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(dot, dotR + 1, dotR + 1);

    // Inner dot with current color hint
    int cr = qBound(0, static_cast<int>(128 + m_r * 127), 255);
    int cg2 = qBound(0, static_cast<int>(128 + m_g * 127), 255);
    int cb = qBound(0, static_cast<int>(128 + m_b * 127), 255);
    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(QColor(cr, cg2, cb));
    p.drawEllipse(dot, dotR, dotR);

    // Draw label
    if (!m_label.isEmpty()) {
        p.setPen(QColor(200, 200, 200));
        QFont f = font();
        f.setPointSize(9);
        f.setBold(true);
        p.setFont(f);
        QRectF labelRect(0, wr.bottom() + 2, width(), 18);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop, m_label);
    }
}

void ColorWheelWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        QRectF wr = wheelRect();
        QPointF center = wr.center();
        double dx = event->position().x() - center.x();
        double dy = event->position().y() - center.y();
        double dist = qSqrt(dx * dx + dy * dy);
        if (dist <= wheelRadius()) {
            m_dragging = true;
            updateFromPos(event->position());
        }
    }
}

void ColorWheelWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        updateFromPos(event->position());
    }
}

void ColorWheelWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
}

void ColorWheelWidget::mouseDoubleClickEvent(QMouseEvent *)
{
    // Reset to center on double-click
    m_r = 0.0;
    m_g = 0.0;
    m_b = 0.0;
    m_dragging = false;
    update();
    emit colorChanged(m_r, m_g, m_b);
}
