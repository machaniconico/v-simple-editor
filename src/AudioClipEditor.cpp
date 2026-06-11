#include "AudioClipEditor.h"

#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSlider>
#include <QtMath>
#include <algorithm>

namespace {
thread_local int s_sliderDragUndoSuppressionDepth = 0;

// RAII guard so a throwing slot chain (e.g. bad_alloc in saveState) cannot
// leave the suppression depth stuck >0 and silently kill undo recording.
struct SliderDragUndoSuppressionScope {
    SliderDragUndoSuppressionScope() { ++s_sliderDragUndoSuppressionDepth; }
    ~SliderDragUndoSuppressionScope() { --s_sliderDragUndoSuppressionDepth; }
};
}

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

    m_controlPanel = new QWidget(this);
    auto* row = new QHBoxLayout(m_controlPanel);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(8);

    auto* volumeLabel = new QLabel(QStringLiteral("音量"), m_controlPanel);
    m_volumeSlider = new QSlider(Qt::Horizontal, m_controlPanel);
    m_volumeSlider->setRange(0, 200);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(150);
    m_volumeSpin = new QDoubleSpinBox(m_controlPanel);
    m_volumeSpin->setRange(0.0, 2.0);
    m_volumeSpin->setDecimals(2);
    m_volumeSpin->setSingleStep(0.01);
    m_volumeSpin->setValue(1.0);

    m_panLabel = new QLabel(QStringLiteral("パン (L/C/R)"), m_controlPanel);
    m_panSlider = new QSlider(Qt::Horizontal, m_controlPanel);
    m_panSlider->setRange(-100, 100);
    m_panSlider->setValue(0);
    m_panSlider->setFixedWidth(150);
    m_panSpin = new QDoubleSpinBox(m_controlPanel);
    m_panSpin->setRange(-1.0, 1.0);
    m_panSpin->setDecimals(2);
    m_panSpin->setSingleStep(0.01);
    m_panSpin->setValue(0.0);

    row->addWidget(volumeLabel);
    row->addWidget(m_volumeSlider);
    row->addWidget(m_volumeSpin);
    row->addSpacing(12);
    row->addWidget(m_panLabel);
    row->addWidget(m_panSlider);
    row->addWidget(m_panSpin);
    row->addStretch();

    connect(m_volumeSlider, &QSlider::sliderPressed, this, [this]() {
        m_volumeSliderDragging = true;
        m_volumeSliderDragStartValue = m_volumeSlider ? m_volumeSlider->value() : 100;
    });
    connect(m_volumeSlider, &QSlider::sliderReleased, this, [this]() {
        if (!m_volumeSliderDragging) return;
        const bool changed = m_volumeSlider
            && m_volumeSlider->value() != m_volumeSliderDragStartValue;
        m_volumeSliderDragging = false;
        if (changed)
            emitVolumeChangedForUndo(true);
    });
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_updatingControls) return;
        setVolume(static_cast<double>(value) / 100.0);
        emitVolumeChangedForUndo(!m_volumeSliderDragging);
    });
    connect(m_volumeSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
        if (m_updatingControls) return;
        setVolume(value);
        emitVolumeChangedForUndo(true);
    });
    connect(m_panSlider, &QSlider::sliderPressed, this, [this]() {
        m_panSliderDragging = true;
        m_panSliderDragStartValue = m_panSlider ? m_panSlider->value() : 0;
    });
    connect(m_panSlider, &QSlider::sliderReleased, this, [this]() {
        if (!m_panSliderDragging) return;
        const bool changed = m_panSlider
            && m_panSlider->value() != m_panSliderDragStartValue;
        m_panSliderDragging = false;
        if (changed)
            emitPanChangedForUndo(true);
    });
    connect(m_panSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_updatingControls) return;
        setPan(static_cast<double>(value) / 100.0);
        emitPanChangedForUndo(!m_panSliderDragging);
    });
    connect(m_panSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
        if (m_updatingControls) return;
        setPan(value);
        emitPanChangedForUndo(true);
    });

    setMinimumSize(560, 160);
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

void AudioClipEditor::setVolume(double volume)
{
    m_volume = qBound(0.0, volume, 2.0);
    m_updatingControls = true;
    if (m_volumeSlider) m_volumeSlider->setValue(qRound(m_volume * 100.0));
    if (m_volumeSpin) m_volumeSpin->setValue(m_volume);
    m_updatingControls = false;
}

double AudioClipEditor::volume() const
{
    return m_volume;
}

void AudioClipEditor::setPan(double pan)
{
    m_pan = qBound(-1.0, pan, 1.0);
    m_updatingControls = true;
    if (m_panSlider) m_panSlider->setValue(qRound(m_pan * 100.0));
    if (m_panSpin) m_panSpin->setValue(m_pan);
    m_updatingControls = false;
}

double AudioClipEditor::pan() const
{
    return m_pan;
}

bool AudioClipEditor::isSliderDragUndoSuppressed()
{
    return s_sliderDragUndoSuppressionDepth > 0;
}

// ─── private helpers ─────────────────────────────────────────────────────────

void AudioClipEditor::emitVolumeChangedForUndo(bool recordUndo)
{
    if (recordUndo) {
        emit volumeChanged(m_volume);
        return;
    }

    const SliderDragUndoSuppressionScope suppress;
    emit volumeChanged(m_volume);
}

void AudioClipEditor::emitPanChangedForUndo(bool recordUndo)
{
    if (recordUndo) {
        emit panChanged(m_pan);
        return;
    }

    const SliderDragUndoSuppressionScope suppress;
    emit panChanged(m_pan);
}

QPointF AudioClipEditor::pointToPixel(const VolumeEnvelopePoint& p) const
{
    const double w = width()  - kMarginLeft - kMarginRight;
    const double h = height() - kControlsHeight - kMarginTop - kMarginBottom;

    const double xRatio = (m_durationMs > 0)
                          ? static_cast<double>(p.timeMs) / static_cast<double>(m_durationMs)
                          : 0.0;
    const double yRatio = (p.dB - kDbMin) / (kDbMax - kDbMin);  // 0=bottom, 1=top

    const double px = kMarginLeft + xRatio * w;
    const double py = kControlsHeight + kMarginTop + (1.0 - yRatio) * h;  // invert: top = +12 dB
    return { px, py };
}

VolumeEnvelopePoint AudioClipEditor::pixelToPoint(const QPointF& px) const
{
    const double w = width()  - kMarginLeft - kMarginRight;
    const double h = height() - kControlsHeight - kMarginTop - kMarginBottom;

    const double xRatio = (w > 0.0) ? (px.x() - kMarginLeft) / w : 0.0;
    const double yRatio = (h > 0.0) ? (px.y() - kControlsHeight - kMarginTop) / h : 0.0;  // 0=top

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

void AudioClipEditor::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (m_controlPanel)
        m_controlPanel->setGeometry(0, 0, width(), kControlsHeight);
}

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
            p.drawLine(x, kControlsHeight + kMarginTop, x, H - kMarginBottom);
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
    if (e->position().y() < kControlsHeight) return;
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
    if (e->position().y() < kControlsHeight) return;
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
    if (e->pos().y() < kControlsHeight) return;
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
