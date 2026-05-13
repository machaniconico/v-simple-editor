#include "AudioClipEditor.h"

#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPaintEvent>
#include <QtMath>
#include <algorithm>

// ─── helpers ────────────────────────────────────────────────────────────────

static inline double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

// ─── construction ───────────────────────────────────────────────────────────

AudioClipEditor::AudioClipEditor(QWidget* parent)
    : QWidget(parent)
{
    m_durationMs = 10000;
    m_points     = { {0, 0.0}, {10000, 0.0} };

    setMinimumSize(400, 120);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
}

// ─── public API ─────────────────────────────────────────────────────────────

void AudioClipEditor::setClipDuration(qint64 durationMs)
{
    if (durationMs < 1) return;
    m_durationMs = durationMs;

    // Remove points whose timeMs exceeds the new duration (strict: > durationMs).
    QList<VolumeEnvelopePoint> kept;
    for (const auto& p : m_points) {
        if (p.timeMs <= m_durationMs) {
            kept.append(p);
        }
    }
    m_points = kept;

    // Ensure at least the two boundary points exist.
    bool hasStart = false, hasEnd = false;
    for (const auto& p : m_points) {
        if (p.timeMs == 0)           hasStart = true;
        if (p.timeMs == m_durationMs) hasEnd   = true;
    }
    if (!hasStart) m_points.prepend({0, 0.0});
    if (!hasEnd)   m_points.append({m_durationMs, 0.0});

    sortPoints();
    update();
}

qint64 AudioClipEditor::clipDuration() const
{
    return m_durationMs;
}

void AudioClipEditor::setEnvelope(const QList<VolumeEnvelopePoint>& points)
{
    m_points = points;
    sortPoints();
    update();
}

QList<VolumeEnvelopePoint> AudioClipEditor::envelope() const
{
    return m_points;
}

void AudioClipEditor::clearEnvelope()
{
    m_points = { {0, 0.0}, {m_durationMs, 0.0} };
    m_dragIndex = -1;
    update();
}

double AudioClipEditor::evaluateAt(qint64 timeMs) const
{
    if (m_points.size() <= 1) return 0.0;

    if (timeMs <= m_points.first().timeMs) return m_points.first().dB;
    if (timeMs >= m_points.last().timeMs)  return m_points.last().dB;

    for (int i = 1; i < m_points.size(); ++i) {
        const auto& a = m_points[i - 1];
        const auto& b = m_points[i];
        if (timeMs <= b.timeMs) {
            const double span = static_cast<double>(qMax(qint64(1), b.timeMs - a.timeMs));
            const double t    = static_cast<double>(timeMs - a.timeMs) / span;
            return lerp(a.dB, b.dB, t);
        }
    }
    return m_points.last().dB;
}

// ─── private helpers ─────────────────────────────────────────────────────────

QPointF AudioClipEditor::pointToPixel(const VolumeEnvelopePoint& p) const
{
    const double w = width()  - kMarginLeft - kMarginRight;
    const double h = height() - kMarginTop  - kMarginBottom;

    const double xRatio = (m_durationMs > 0)
                          ? static_cast<double>(p.timeMs) / static_cast<double>(m_durationMs)
                          : 0.0;
    const double yRatio = (p.dB - kDbMin) / (kDbMax - kDbMin);  // 0=bottom, 1=top

    const double px = kMarginLeft + xRatio * w;
    const double py = kMarginTop  + (1.0 - yRatio) * h;  // invert: top = +12 dB
    return { px, py };
}

VolumeEnvelopePoint AudioClipEditor::pixelToPoint(const QPointF& px) const
{
    const double w = width()  - kMarginLeft - kMarginRight;
    const double h = height() - kMarginTop  - kMarginBottom;

    const double xRatio = (w > 0.0) ? (px.x() - kMarginLeft) / w : 0.0;
    const double yRatio = (h > 0.0) ? (px.y() - kMarginTop)  / h : 0.0;  // 0=top

    const double tMs = qBound(0.0, xRatio * static_cast<double>(m_durationMs),
                              static_cast<double>(m_durationMs));
    const double dB  = qBound(kDbMin, kDbMin + (1.0 - yRatio) * (kDbMax - kDbMin), kDbMax);

    return { static_cast<qint64>(qRound(tMs)), dB };
}

int AudioClipEditor::hitTest(const QPointF& px, double hitRadiusPx) const
{
    for (int i = 0; i < m_points.size(); ++i) {
        const QPointF pp = pointToPixel(m_points[i]);
        const double dx  = px.x() - pp.x();
        const double dy  = px.y() - pp.y();
        if (qSqrt(dx * dx + dy * dy) <= hitRadiusPx) return i;
    }
    return -1;
}

void AudioClipEditor::sortPoints()
{
    std::stable_sort(m_points.begin(), m_points.end(),
                     [](const VolumeEnvelopePoint& a, const VolumeEnvelopePoint& b) {
                         return a.timeMs < b.timeMs;
                     });
}

void AudioClipEditor::emitChanged()
{
    emit envelopeChanged(m_points);
    update();
}

// ─── paint ───────────────────────────────────────────────────────────────────

void AudioClipEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int W = width();
    const int H = height();

    // Background
    p.fillRect(rect(), QColor(0x1e, 0x1e, 0x1e));

    // ── Grid ──────────────────────────────────────────────────────────────

    // Vertical time grid: 0.25, 0.50, 0.75
    {
        p.setPen(QPen(QColor(0x40, 0x40, 0x40), 1));
        for (double frac : { 0.25, 0.50, 0.75 }) {
            const int x = kMarginLeft + static_cast<int>(frac * (W - kMarginLeft - kMarginRight));
            p.drawLine(x, kMarginTop, x, H - kMarginBottom);
        }
    }

    // Horizontal dB grid
    {
        // 0 dB: brighter
        {
            const QPointF ref = pointToPixel({0, 0.0});
            p.setPen(QPen(QColor(0x80, 0x80, 0x80), 1));
            p.drawLine(QPointF(kMarginLeft, ref.y()), QPointF(W - kMarginRight, ref.y()));
        }
        // -12, -24, -36, -48 dB: dim
        p.setPen(QPen(QColor(0x30, 0x30, 0x30), 1));
        for (double db : { -12.0, -24.0, -36.0, -48.0 }) {
            const QPointF ref = pointToPixel({0, db});
            p.drawLine(QPointF(kMarginLeft, ref.y()), QPointF(W - kMarginRight, ref.y()));
        }
    }

    // ── dB axis labels ────────────────────────────────────────────────────
    {
        QFont f = font();
        f.setPointSize(7);
        p.setFont(f);
        p.setPen(QColor(0x80, 0x80, 0x80));

        for (double db : { 12.0, 0.0, -24.0, -60.0 }) {
            const QPointF ref = pointToPixel({0, db});
            const QString lbl = (db >= 0) ? QStringLiteral("+%1").arg(static_cast<int>(db))
                                           : QStringLiteral("%1").arg(static_cast<int>(db));
            p.drawText(QRectF(0, ref.y() - 8, kMarginLeft - 2, 16),
                       Qt::AlignRight | Qt::AlignVCenter, lbl);
        }
    }

    // ── Envelope polyline ─────────────────────────────────────────────────
    if (m_points.size() >= 2) {
        p.setPen(QPen(QColor(0x4c, 0xaf, 0x50), 2));
        for (int i = 1; i < m_points.size(); ++i) {
            p.drawLine(pointToPixel(m_points[i - 1]), pointToPixel(m_points[i]));
        }
    }

    // ── Points ────────────────────────────────────────────────────────────
    for (int i = 0; i < m_points.size(); ++i) {
        const QPointF pp = pointToPixel(m_points[i]);
        const bool selected = (i == m_dragIndex);
        const double r = selected ? 7.0 : 5.0;

        p.setBrush(QColor(0x4c, 0xaf, 0x50));
        if (selected) {
            p.setPen(QPen(QColor(0xff, 0xeb, 0x3b), 2));
        } else {
            p.setPen(QPen(QColor(0x4c, 0xaf, 0x50), 1));
        }
        p.drawEllipse(pp, r, r);
    }
}

// ─── mouse ────────────────────────────────────────────────────────────────────

void AudioClipEditor::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const int idx = hitTest(e->position());
    if (idx >= 0) {
        m_dragIndex = idx;
        update();
    }
}

void AudioClipEditor::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragIndex < 0 || m_dragIndex >= m_points.size()) return;

    VolumeEnvelopePoint np = pixelToPoint(e->position());

    // Clamp timeMs between neighbors (with ±1ms guard so order is preserved).
    qint64 tMin = 0;
    qint64 tMax = m_durationMs;
    if (m_dragIndex > 0)
        tMin = m_points[m_dragIndex - 1].timeMs + 1;
    if (m_dragIndex < m_points.size() - 1)
        tMax = m_points[m_dragIndex + 1].timeMs - 1;

    np.timeMs = qBound(tMin, np.timeMs, tMax);
    np.dB     = qBound(kDbMin, np.dB, kDbMax);

    m_points[m_dragIndex] = np;
    emitChanged();
}

void AudioClipEditor::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragIndex = -1;
        update();
    }
}

void AudioClipEditor::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    if (hitTest(e->position()) >= 0) return;  // hit existing point — ignore

    VolumeEnvelopePoint np = pixelToPoint(e->position());
    np.timeMs = qBound(qint64(0), np.timeMs, m_durationMs);
    np.dB     = qBound(kDbMin, np.dB, kDbMax);

    m_points.append(np);
    sortPoints();
    emitChanged();
}

void AudioClipEditor::contextMenuEvent(QContextMenuEvent* e)
{
    const int idx = hitTest(QPointF(e->pos()));
    if (idx < 0) return;

    QMenu menu(this);
    const bool isEndpoint = (idx == 0 || idx == m_points.size() - 1);

    QAction* delAct = menu.addAction(tr("この点を削除"));
    delAct->setEnabled(!isEndpoint);

    QAction* chosen = menu.exec(e->globalPos());
    if (chosen == delAct && !isEndpoint) {
        m_points.removeAt(idx);
        if (m_dragIndex >= m_points.size()) m_dragIndex = -1;
        emitChanged();
    }
}
