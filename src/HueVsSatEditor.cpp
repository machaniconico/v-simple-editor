#include "HueVsSatEditor.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kMaxPoints = 24;
constexpr int kHitRadius = 8;
constexpr int kPointSize = 8;
constexpr int kPlotMargin = 6;
constexpr double kHueMax = 360.0;
constexpr double kSatMax = 2.0;
constexpr double kSatIdentity = 1.0;
}

HueVsSatEditor::HueVsSatEditor(QWidget *parent)
    : QWidget(parent)
{
    m_points = identityPoints();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto *headerRow = new QHBoxLayout;
    headerRow->addWidget(new QLabel(tr("Hue → Sat 倍率"), this));
    headerRow->addStretch(1);

    m_resetButton = new QPushButton(tr("リセット"), this);
    headerRow->addWidget(m_resetButton);

    layout->addLayout(headerRow);
    layout->addStretch(1); // paintEvent draws into the remaining area

    setMinimumSize(260, 180);

    connect(m_resetButton, &QPushButton::clicked,
            this, &HueVsSatEditor::onResetClicked);

    rebuildLut();
}

QVector<float> HueVsSatEditor::currentLut() const
{
    return m_lutCache;
}

bool HueVsSatEditor::isIdentity() const
{
    if (m_points.size() != 4) return false;
    for (const QPointF &p : m_points) {
        if (!qFuzzyCompare(p.y() + 1.0, kSatIdentity + 1.0))
            return false;
    }
    return true;
}

QVector<QPointF> HueVsSatEditor::identityPoints()
{
    // Four flat-identity stops at 0°, 120°, 240°, 360° (the four classic
    // hue anchors used by DaVinci's Hue vs Sat tool).
    return {
        QPointF(0.0,   kSatIdentity),
        QPointF(120.0, kSatIdentity),
        QPointF(240.0, kSatIdentity),
        QPointF(360.0, kSatIdentity),
    };
}

QRect HueVsSatEditor::plotRect() const
{
    int headerBottom = kPlotMargin;
    if (m_resetButton && m_resetButton->geometry().isValid()) {
        headerBottom = m_resetButton->geometry().bottom() + kPlotMargin;
    } else if (m_resetButton) {
        headerBottom = m_resetButton->sizeHint().height() + kPlotMargin;
    }
    QRect r = rect().adjusted(kPlotMargin, headerBottom,
                              -kPlotMargin, -kPlotMargin);
    if (r.width() <= 0 || r.height() <= 0)
        return QRect();
    return r;
}

QPointF HueVsSatEditor::pointToPixel(const QPointF &p, const QRect &plot) const
{
    if (plot.isEmpty()) return QPointF();
    double nx = p.x() / kHueMax;
    double ny = p.y() / kSatMax;
    double px = plot.left() + nx * (plot.width() - 1);
    // y axis: 0.0 = top (full desaturate), 2.0 = bottom (double saturation).
    // (Inverted vs RGB curves on purpose so that "drag down to desaturate"
    // matches DaVinci's convention.)
    double py = plot.top() + ny * (plot.height() - 1);
    return QPointF(px, py);
}

QPointF HueVsSatEditor::pixelToPoint(const QPointF &px, const QRect &plot) const
{
    if (plot.isEmpty()) return QPointF();
    double nx = (px.x() - plot.left()) / static_cast<double>(plot.width() - 1);
    double ny = (px.y() - plot.top())  / static_cast<double>(plot.height() - 1);
    nx = std::clamp(nx, 0.0, 1.0);
    ny = std::clamp(ny, 0.0, 1.0);
    return QPointF(nx * kHueMax, ny * kSatMax);
}

int HueVsSatEditor::hitTestPoint(const QPoint &widgetPos) const
{
    const QRect plot = plotRect();
    int best = -1;
    int bestDistSq = kHitRadius * kHitRadius;
    for (int i = 0; i < m_points.size(); ++i) {
        QPointF px = pointToPixel(m_points[i], plot);
        int dx = static_cast<int>(px.x() - widgetPos.x());
        int dy = static_cast<int>(px.y() - widgetPos.y());
        int d2 = dx * dx + dy * dy;
        if (d2 <= bestDistSq) {
            bestDistSq = d2;
            best = i;
        }
    }
    return best;
}

void HueVsSatEditor::sanitizePoints()
{
    if (m_points.size() < 2) {
        m_points = identityPoints();
        return;
    }
    std::sort(m_points.begin(), m_points.end(),
              [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    m_points.first().setX(0.0);
    m_points.last().setX(kHueMax);
    for (auto &p : m_points) {
        p.setX(std::clamp(p.x(), 0.0, kHueMax));
        p.setY(std::clamp(p.y(), 0.0, kSatMax));
    }
}

void HueVsSatEditor::rebuildLut()
{
    m_lutCache.resize(256);
    if (m_points.size() < 2) {
        for (int i = 0; i < 256; ++i)
            m_lutCache[i] = static_cast<float>(kSatIdentity);
        return;
    }

    // Linear interpolation between consecutive control points (sufficient
    // for a hue/sat tool — Catmull-Rom can overshoot below 0 which would
    // invert saturation, undesirable here).
    int seg = 0;
    for (int i = 0; i < 256; ++i) {
        // Bin center maps to the midpoint of [i/256, (i+1)/256] on the
        // hue axis, which keeps the LUT symmetric around the wrap-around.
        double hueDeg = (static_cast<double>(i) + 0.5) * kHueMax / 256.0;
        while (seg + 1 < m_points.size() - 1 && m_points[seg + 1].x() < hueDeg)
            ++seg;
        const QPointF &a = m_points[seg];
        const QPointF &b = m_points[seg + 1];
        double y;
        if (b.x() <= a.x()) {
            y = a.y();
        } else {
            double t = (hueDeg - a.x()) / (b.x() - a.x());
            t = std::clamp(t, 0.0, 1.0);
            y = a.y() + t * (b.y() - a.y());
        }
        m_lutCache[i] = static_cast<float>(std::clamp(y, 0.0, kSatMax));
    }
}

void HueVsSatEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.fillRect(rect(), QColor(32, 32, 32));

    const QRect plot = plotRect();
    if (plot.isEmpty())
        return;

    // Rainbow gradient background — full-saturation HSL sweep across the
    // hue axis. Stepped stops make the colour rainbow look continuous
    // even on small widget widths.
    {
        QLinearGradient grad(plot.left(), 0, plot.right(), 0);
        const int stops = 24;
        for (int s = 0; s <= stops; ++s) {
            double t = static_cast<double>(s) / stops;
            QColor c = QColor::fromHslF(t, 1.0, 0.5);
            grad.setColorAt(t, c);
        }
        // Vertical fade so the rainbow doesn't visually compete with the
        // curve overlay — slightly translucent middle band.
        QImage stripe(plot.width(), plot.height(), QImage::Format_ARGB32_Premultiplied);
        stripe.fill(Qt::transparent);
        QPainter sp(&stripe);
        sp.setRenderHint(QPainter::Antialiasing, false);
        sp.fillRect(stripe.rect(), grad);
        sp.end();
        p.setOpacity(0.55);
        p.drawImage(plot.topLeft(), stripe);
        p.setOpacity(1.0);
    }

    // Plot frame
    p.setPen(QColor(80, 80, 80));
    p.drawRect(plot);

    // Grid: 6 vertical (every 60°), 4 horizontal (sat 0/0.5/1.0/1.5/2.0).
    p.setPen(QColor(60, 60, 60));
    for (int i = 1; i < 6; ++i) {
        int gx = plot.left() + (plot.width() - 1) * i / 6;
        p.drawLine(gx, plot.top(), gx, plot.bottom());
    }
    for (int i = 1; i < 4; ++i) {
        int gy = plot.top() + (plot.height() - 1) * i / 4;
        p.drawLine(plot.left(), gy, plot.right(), gy);
    }

    // 1.0 identity midline (gray) — drawn brighter so the user sees where
    // "no change" sits.
    p.setPen(QPen(QColor(160, 160, 160), 1.0, Qt::DashLine));
    QPointF idLeft  = pointToPixel(QPointF(0.0,    kSatIdentity), plot);
    QPointF idRight = pointToPixel(QPointF(kHueMax, kSatIdentity), plot);
    p.drawLine(idLeft, idRight);

    // Curve from the LUT (256 samples) — the actual rendered curve, so the
    // user sees the same shape as the GPU shader applies.
    if (m_lutCache.size() == 256) {
        QColor curveColor(220, 220, 220);
        p.setPen(QPen(curveColor, 2.0));
        QPainterPath path;
        QPointF first = pointToPixel(QPointF(0.0, m_lutCache[0]), plot);
        path.moveTo(first);
        for (int x = 1; x < 256; ++x) {
            double hueDeg = (static_cast<double>(x) + 0.5) * kHueMax / 256.0;
            path.lineTo(pointToPixel(QPointF(hueDeg, m_lutCache[x]), plot));
        }
        path.lineTo(pointToPixel(QPointF(kHueMax, m_lutCache[255]), plot));
        p.drawPath(path);
    }

    // Control points
    {
        p.setBrush(QColor(255, 255, 255));
        p.setPen(QColor(0, 0, 0));
        for (const QPointF &pt : m_points) {
            QPointF px = pointToPixel(pt, plot);
            p.drawEllipse(px, kPointSize / 2.0, kPointSize / 2.0);
        }
    }
}

void HueVsSatEditor::mousePressEvent(QMouseEvent *event)
{
    const QRect plot = plotRect();
    if (plot.isEmpty()) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::RightButton) {
        int idx = hitTestPoint(event->pos());
        if (idx >= 0 && m_points.size() > 2
            && idx != 0 && idx != m_points.size() - 1) {
            m_points.remove(idx);
            sanitizePoints();
            rebuildLut();
            update();
            emit hueVsSatChanged(m_lutCache);
        }
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    int hit = hitTestPoint(event->pos());
    if (hit >= 0) {
        m_draggingPoint = hit;
        m_pointWasMoved = false;
        return;
    }

    if (!plot.contains(event->pos())) return;
    if (m_points.size() >= kMaxPoints) return;

    QPointF newPt = pixelToPoint(QPointF(event->pos()), plot);
    m_points.append(newPt);
    sanitizePoints();
    int newIdx = -1;
    for (int i = 0; i < m_points.size(); ++i) {
        if (qFuzzyCompare(m_points[i].x() + 1.0, newPt.x() + 1.0)
            && qFuzzyCompare(m_points[i].y() + 1.0, newPt.y() + 1.0)) {
            newIdx = i;
            break;
        }
    }
    m_draggingPoint = newIdx;
    m_pointWasMoved = true;
    rebuildLut();
    update();
    emit hueVsSatChanged(m_lutCache);
}

void HueVsSatEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggingPoint < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QRect plot = plotRect();
    if (plot.isEmpty()) return;

    if (m_draggingPoint >= m_points.size()) {
        m_draggingPoint = -1;
        return;
    }

    QPointF p = pixelToPoint(QPointF(event->pos()), plot);

    if (m_draggingPoint == 0) {
        p.setX(0.0);
    } else if (m_draggingPoint == m_points.size() - 1) {
        p.setX(kHueMax);
    } else {
        double minX = m_points[m_draggingPoint - 1].x() + 0.5;
        double maxX = m_points[m_draggingPoint + 1].x() - 0.5;
        p.setX(std::clamp(p.x(), minX, maxX));
    }
    p.setY(std::clamp(p.y(), 0.0, kSatMax));
    m_points[m_draggingPoint] = p;
    m_pointWasMoved = true;

    rebuildLut();
    update();
    emit hueVsSatChanged(m_lutCache);
}

void HueVsSatEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_draggingPoint >= 0 && event->button() == Qt::LeftButton) {
        m_draggingPoint = -1;
        sanitizePoints();
        rebuildLut();
        update();
        if (m_pointWasMoved)
            emit hueVsSatChanged(m_lutCache);
        m_pointWasMoved = false;
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void HueVsSatEditor::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

void HueVsSatEditor::onResetClicked()
{
    m_points = identityPoints();
    m_draggingPoint = -1;
    rebuildLut();
    update();
    emit hueVsSatChanged(m_lutCache);
}
