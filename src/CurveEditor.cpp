#include "CurveEditor.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kMaxPoints = 16;
constexpr int kHitRadius = 8;       // widget pixels for control-point hit test
constexpr int kPointSize = 8;       // diameter of drawn control point
constexpr int kPlotMargin = 6;      // inner padding around the 256x256 plot
}

CurveEditor::CurveEditor(QWidget *parent)
    : QWidget(parent)
{
    for (int i = 0; i < ChannelCount; ++i)
        m_points[i] = identityPoints();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto *headerRow = new QHBoxLayout;
    headerRow->addWidget(new QLabel(tr("チャンネル:"), this));

    m_channelCombo = new QComboBox(this);
    m_channelCombo->addItem(tr("R"));
    m_channelCombo->addItem(tr("G"));
    m_channelCombo->addItem(tr("B"));
    m_channelCombo->addItem(tr("Luma"));
    headerRow->addWidget(m_channelCombo, 1);

    m_resetButton = new QPushButton(tr("リセット"), this);
    headerRow->addWidget(m_resetButton);

    layout->addLayout(headerRow);
    layout->addStretch(1); // paintEvent draws into the remaining area

    setMinimumSize(260, 260);

    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CurveEditor::onChannelChanged);
    connect(m_resetButton, &QPushButton::clicked,
            this, &CurveEditor::onResetClicked);

    rebuildAllLuts();
}

QVector<QVector<int>> CurveEditor::currentCurves() const
{
    QVector<QVector<int>> out;
    out.reserve(ChannelCount);
    for (int i = 0; i < ChannelCount; ++i)
        out.append(m_lutCache[i]);
    return out;
}

bool CurveEditor::isIdentity() const
{
    for (int i = 0; i < ChannelCount; ++i) {
        const auto &pts = m_points[i];
        if (pts.size() != 2) return false;
        if (qFuzzyCompare(pts[0].x() + 1.0, 0.0 + 1.0)
            && qFuzzyCompare(pts[0].y() + 1.0, 0.0 + 1.0)
            && qFuzzyCompare(pts[1].x() + 1.0, 255.0 + 1.0)
            && qFuzzyCompare(pts[1].y() + 1.0, 255.0 + 1.0)) {
            continue;
        }
        return false;
    }
    return true;
}

QVector<QPointF> CurveEditor::identityPoints()
{
    return { QPointF(0.0, 0.0), QPointF(255.0, 255.0) };
}

QVector<QVector<QPointF>> CurveEditor::allPoints() const
{
    QVector<QVector<QPointF>> out;
    out.reserve(ChannelCount);
    for (int i = 0; i < ChannelCount; ++i)
        out.append(m_points[i]);
    return out;
}

void CurveEditor::setAllPoints(const QVector<QVector<QPointF>> &pts)
{
    for (int i = 0; i < ChannelCount; ++i) {
        if (i < pts.size() && pts[i].size() >= 2) {
            m_points[i] = pts[i];
            std::sort(m_points[i].begin(), m_points[i].end(),
                      [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
            m_points[i].first().setX(0.0);
            m_points[i].last().setX(255.0);
            for (auto &p : m_points[i]) {
                p.setX(std::clamp(p.x(), 0.0, 255.0));
                p.setY(std::clamp(p.y(), 0.0, 255.0));
            }
        } else {
            m_points[i] = identityPoints();
        }
    }
    m_draggingPoint = -1;
    rebuildAllLuts();
    update();
    emit curvesChanged(currentCurves());
}

QRect CurveEditor::plotRect() const
{
    // Reserve top row for combo+reset (their layout sits above this rect),
    // then the plot fills the rest. Use the live combo geometry when laid
    // out so the header height tracks the real widget height across DPI.
    int headerBottom = kPlotMargin;
    if (m_channelCombo && m_channelCombo->geometry().isValid()) {
        headerBottom = m_channelCombo->geometry().bottom() + kPlotMargin;
    } else if (m_channelCombo) {
        headerBottom = m_channelCombo->sizeHint().height() + kPlotMargin;
    }
    QRect r = rect().adjusted(kPlotMargin, headerBottom,
                              -kPlotMargin, -kPlotMargin);
    if (r.width() <= 0 || r.height() <= 0)
        return QRect();
    int side = std::min(r.width(), r.height());
    int xOff = r.left() + (r.width() - side) / 2;
    int yOff = r.top() + (r.height() - side) / 2;
    return QRect(xOff, yOff, side, side);
}

QPointF CurveEditor::pointToPixel(const QPointF &p, const QRect &plot) const
{
    if (plot.isEmpty()) return QPointF();
    double nx = p.x() / 255.0;
    double ny = p.y() / 255.0;
    double px = plot.left() + nx * (plot.width() - 1);
    // y axis: image-space 0 (black) is bottom of the plot, 255 (white) is top.
    double py = plot.bottom() - ny * (plot.height() - 1);
    return QPointF(px, py);
}

QPointF CurveEditor::pixelToPoint(const QPointF &px, const QRect &plot) const
{
    if (plot.isEmpty()) return QPointF();
    double nx = (px.x() - plot.left()) / static_cast<double>(plot.width() - 1);
    double ny = (plot.bottom() - px.y()) / static_cast<double>(plot.height() - 1);
    nx = std::clamp(nx, 0.0, 1.0);
    ny = std::clamp(ny, 0.0, 1.0);
    return QPointF(nx * 255.0, ny * 255.0);
}

int CurveEditor::hitTestPoint(const QPoint &widgetPos) const
{
    const QRect plot = plotRect();
    const auto &pts = m_points[m_activeChannel];
    int best = -1;
    int bestDistSq = kHitRadius * kHitRadius;
    for (int i = 0; i < pts.size(); ++i) {
        QPointF px = pointToPixel(pts[i], plot);
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

void CurveEditor::sanitizeActivePoints()
{
    auto &pts = m_points[m_activeChannel];
    if (pts.size() < 2) {
        pts = identityPoints();
        return;
    }
    // Endpoints stay locked at x=0 and x=255 so the LUT is always defined
    // across the full input range.
    std::sort(pts.begin(), pts.end(),
              [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    pts.first().setX(0.0);
    pts.last().setX(255.0);
    for (auto &p : pts) {
        p.setX(std::clamp(p.x(), 0.0, 255.0));
        p.setY(std::clamp(p.y(), 0.0, 255.0));
    }
}

void CurveEditor::rebuildLutFor(int channel)
{
    QVector<int> &out = m_lutCache[channel];
    out.resize(256);
    const auto &pts = m_points[channel];
    if (pts.size() < 2) {
        for (int x = 0; x < 256; ++x) out[x] = x;
        return;
    }

    // Catmull-Rom spline through control points. We need a "previous" point
    // before the first control and a "next" after the last; reflect them so
    // the segments at the endpoints have well-defined tangents.
    auto getPoint = [&](int idx) -> QPointF {
        const int n = pts.size();
        if (idx < 0)  return QPointF(2.0 * pts[0].x() - pts[1].x(),
                                     2.0 * pts[0].y() - pts[1].y());
        if (idx >= n) return QPointF(2.0 * pts[n - 1].x() - pts[n - 2].x(),
                                     2.0 * pts[n - 1].y() - pts[n - 2].y());
        return pts[idx];
    };

    auto evalSegment = [&](int seg, double t, QPointF *out) {
        QPointF p0 = getPoint(seg - 1);
        QPointF p1 = getPoint(seg);
        QPointF p2 = getPoint(seg + 1);
        QPointF p3 = getPoint(seg + 2);
        double t2 = t * t;
        double t3 = t2 * t;
        // Standard Catmull-Rom coefficients (0.5 tension)
        double xv = 0.5 * ((2.0 * p1.x())
            + (-p0.x() + p2.x()) * t
            + (2.0 * p0.x() - 5.0 * p1.x() + 4.0 * p2.x() - p3.x()) * t2
            + (-p0.x() + 3.0 * p1.x() - 3.0 * p2.x() + p3.x()) * t3);
        double yv = 0.5 * ((2.0 * p1.y())
            + (-p0.y() + p2.y()) * t
            + (2.0 * p0.y() - 5.0 * p1.y() + 4.0 * p2.y() - p3.y()) * t2
            + (-p0.y() + 3.0 * p1.y() - 3.0 * p2.y() + p3.y()) * t3);
        *out = QPointF(xv, yv);
    };

    // Sample many parametric points along the spline, then resample by x.
    const int samplesPerSeg = 64;
    const int nSeg = pts.size() - 1;
    QVector<QPointF> samples;
    samples.reserve(nSeg * samplesPerSeg + 1);
    for (int s = 0; s < nSeg; ++s) {
        for (int i = 0; i < samplesPerSeg; ++i) {
            double t = static_cast<double>(i) / samplesPerSeg;
            QPointF q;
            evalSegment(s, t, &q);
            samples.append(q);
        }
    }
    samples.append(pts.last());

    // Sort samples by x and resample to integer x values 0..255 via linear
    // interpolation between adjacent samples. Catmull-Rom can produce small
    // x-monotonicity artifacts when control points are clustered, so we
    // also clamp y to [0,255].
    std::sort(samples.begin(), samples.end(),
              [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });

    int sIdx = 0;
    for (int x = 0; x < 256; ++x) {
        const double xd = static_cast<double>(x);
        while (sIdx + 1 < samples.size() - 1 && samples[sIdx + 1].x() < xd)
            ++sIdx;
        double y;
        if (samples[sIdx + 1].x() <= samples[sIdx].x()) {
            y = samples[sIdx].y();
        } else {
            double t = (xd - samples[sIdx].x())
                       / (samples[sIdx + 1].x() - samples[sIdx].x());
            t = std::clamp(t, 0.0, 1.0);
            y = samples[sIdx].y() + t * (samples[sIdx + 1].y() - samples[sIdx].y());
        }
        out[x] = std::clamp(static_cast<int>(std::lround(y)), 0, 255);
    }
    // Force endpoints exactly so identity is bit-exact.
    out[0]   = std::clamp(static_cast<int>(std::lround(pts.first().y())), 0, 255);
    out[255] = std::clamp(static_cast<int>(std::lround(pts.last().y())), 0, 255);
}

void CurveEditor::rebuildAllLuts()
{
    for (int i = 0; i < ChannelCount; ++i)
        rebuildLutFor(i);
}

void CurveEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Background
    p.fillRect(rect(), QColor(32, 32, 32));

    const QRect plot = plotRect();
    if (plot.isEmpty())
        return;

    // Plot frame
    p.setPen(QColor(80, 80, 80));
    p.drawRect(plot);

    // Grid lines (4x4)
    p.setPen(QColor(60, 60, 60));
    for (int i = 1; i < 4; ++i) {
        int gx = plot.left() + (plot.width() - 1) * i / 4;
        int gy = plot.top()  + (plot.height() - 1) * i / 4;
        p.drawLine(gx, plot.top(), gx, plot.bottom());
        p.drawLine(plot.left(), gy, plot.right(), gy);
    }

    // Diagonal reference (identity line)
    p.setPen(QColor(70, 70, 70));
    p.drawLine(pointToPixel(QPointF(0, 0), plot),
               pointToPixel(QPointF(255, 255), plot));

    // Faint guides for the inactive channels (not the active one) so the
    // user can see how other channels currently look.
    auto channelPenColor = [](int ch) -> QColor {
        switch (ch) {
            case ChannelR:    return QColor(220, 60,  60);
            case ChannelG:    return QColor(60,  200, 60);
            case ChannelB:    return QColor(80,  120, 240);
            case ChannelLuma: return QColor(220, 220, 220);
        }
        return QColor(200, 200, 200);
    };

    for (int ch = 0; ch < ChannelCount; ++ch) {
        if (ch == m_activeChannel) continue;
        QColor c = channelPenColor(ch);
        c.setAlpha(70);
        p.setPen(QPen(c, 1.0));
        QPainterPath path;
        const auto &lut = m_lutCache[ch];
        if (lut.size() == 256) {
            path.moveTo(pointToPixel(QPointF(0, lut[0]), plot));
            for (int x = 1; x < 256; ++x)
                path.lineTo(pointToPixel(QPointF(x, lut[x]), plot));
            p.drawPath(path);
        }
    }

    // Active channel curve
    {
        QColor c = channelPenColor(m_activeChannel);
        p.setPen(QPen(c, 2.0));
        QPainterPath path;
        const auto &lut = m_lutCache[m_activeChannel];
        if (lut.size() == 256) {
            path.moveTo(pointToPixel(QPointF(0, lut[0]), plot));
            for (int x = 1; x < 256; ++x)
                path.lineTo(pointToPixel(QPointF(x, lut[x]), plot));
            p.drawPath(path);
        }
    }

    // Active channel control points
    {
        QColor c = channelPenColor(m_activeChannel);
        p.setBrush(c);
        p.setPen(QColor(255, 255, 255));
        const auto &pts = m_points[m_activeChannel];
        for (const QPointF &pt : pts) {
            QPointF px = pointToPixel(pt, plot);
            p.drawEllipse(px, kPointSize / 2.0, kPointSize / 2.0);
        }
    }
}

void CurveEditor::mousePressEvent(QMouseEvent *event)
{
    const QRect plot = plotRect();
    if (plot.isEmpty()) {
        QWidget::mousePressEvent(event);
        return;
    }

    // Right-click on existing point → delete (if more than 2 points remain).
    if (event->button() == Qt::RightButton) {
        int idx = hitTestPoint(event->pos());
        if (idx >= 0) {
            auto &pts = m_points[m_activeChannel];
            if (pts.size() > 2) {
                pts.remove(idx);
                sanitizeActivePoints();
                rebuildLutFor(m_activeChannel);
                update();
                emit curvesChanged(currentCurves());
            }
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
        m_dragStart = event->pos();
        return;
    }

    // Add a new control point if the click is inside the plot and we have
    // room (max 16 per curve). The new point gets sorted by x in
    // sanitizeActivePoints, so the index can change after insert; resolve
    // it by matching the new point's x value.
    if (!plot.contains(event->pos())) return;
    auto &pts = m_points[m_activeChannel];
    if (pts.size() >= kMaxPoints) return;

    QPointF newPt = pixelToPoint(QPointF(event->pos()), plot);
    pts.append(newPt);
    sanitizeActivePoints();
    int newIdx = -1;
    for (int i = 0; i < pts.size(); ++i) {
        if (qFuzzyCompare(pts[i].x() + 1.0, newPt.x() + 1.0)
            && qFuzzyCompare(pts[i].y() + 1.0, newPt.y() + 1.0)) {
            newIdx = i;
            break;
        }
    }
    m_draggingPoint = newIdx;
    m_pointWasMoved = true;
    rebuildLutFor(m_activeChannel);
    update();
    emit curvesChanged(currentCurves());
}

void CurveEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggingPoint < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QRect plot = plotRect();
    if (plot.isEmpty()) return;

    auto &pts = m_points[m_activeChannel];
    if (m_draggingPoint >= pts.size()) {
        m_draggingPoint = -1;
        return;
    }

    QPointF p = pixelToPoint(QPointF(event->pos()), plot);

    // Endpoint x coordinates are locked.
    if (m_draggingPoint == 0)
        p.setX(0.0);
    else if (m_draggingPoint == pts.size() - 1)
        p.setX(255.0);
    else {
        // Interior points must stay strictly between their neighbours so the
        // sort order doesn't flip mid-drag.
        double minX = pts[m_draggingPoint - 1].x() + 0.5;
        double maxX = pts[m_draggingPoint + 1].x() - 0.5;
        p.setX(std::clamp(p.x(), minX, maxX));
    }
    p.setY(std::clamp(p.y(), 0.0, 255.0));
    pts[m_draggingPoint] = p;
    m_pointWasMoved = true;

    rebuildLutFor(m_activeChannel);
    update();
    emit curvesChanged(currentCurves());
}

void CurveEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_draggingPoint >= 0 && event->button() == Qt::LeftButton) {
        m_draggingPoint = -1;
        sanitizeActivePoints();
        rebuildLutFor(m_activeChannel);
        update();
        if (m_pointWasMoved)
            emit curvesChanged(currentCurves());
        m_pointWasMoved = false;
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void CurveEditor::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

void CurveEditor::onChannelChanged(int index)
{
    if (index < 0 || index >= ChannelCount) return;
    m_activeChannel = index;
    m_draggingPoint = -1;
    update();
}

void CurveEditor::onResetClicked()
{
    m_points[m_activeChannel] = identityPoints();
    m_draggingPoint = -1;
    rebuildLutFor(m_activeChannel);
    update();
    emit curvesChanged(currentCurves());
}
