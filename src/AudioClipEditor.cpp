#include "AudioClipEditor.h"

#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QMouseEvent>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QGridLayout>
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

// Band 1 = lowMid, Band 2 = highMid, Band 3 = low shelf, Band 4 = high shelf.
trackfx::EqBand& eqBandRef(trackfx::EqSettings& eq, int band)
{
    switch (band) {
    case 0: return eq.lowMid;
    case 1: return eq.highMid;
    case 2: return eq.low;
    default: return eq.high;
    }
}
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
    m_controlPanel->setObjectName(QStringLiteral("clipVolumePanRow"));
    auto* row = new QHBoxLayout(m_controlPanel);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(8);

    auto* volumeLabel = new QLabel(QStringLiteral("音量"), m_controlPanel);
    m_volumeSlider = new QSlider(Qt::Horizontal, m_controlPanel);
    m_volumeSlider->setObjectName(QStringLiteral("clipVolumeSlider"));
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
    m_panSlider->setObjectName(QStringLiteral("clipPanSlider"));
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

    m_clipEqPanel = new QWidget(this);
    m_clipEqPanel->setObjectName(QStringLiteral("clipEqPanel"));
    auto* eqGrid = new QGridLayout(m_clipEqPanel);
    eqGrid->setContentsMargins(8, 4, 8, 4);
    eqGrid->setSpacing(4);
    m_clipEqEnable = new QCheckBox(QStringLiteral("クリップ EQ"), m_clipEqPanel);
    m_clipEqEnable->setObjectName(QStringLiteral("clipEqEnable"));
    eqGrid->addWidget(m_clipEqEnable, 0, 0, 1, 2);
    static const char* const kBandNames[4] = {"Band 1 (低中域)", "Band 2 (高中域)",
                                              "Band 3 (低域シェルフ)", "Band 4 (高域シェルフ)"};
    for (int band = 0; band < 4; ++band) {
        const QString n = QString::number(band + 1);
        m_clipEqFreq[band] = new QDoubleSpinBox(m_clipEqPanel);
        m_clipEqFreq[band]->setObjectName(QStringLiteral("clipEqBand%1Freq").arg(n));
        m_clipEqFreq[band]->setRange(20.0, 20000.0);
        m_clipEqFreq[band]->setDecimals(1);
        m_clipEqFreq[band]->setSuffix(QStringLiteral(" Hz"));
        m_clipEqGain[band] = new QDoubleSpinBox(m_clipEqPanel);
        m_clipEqGain[band]->setObjectName(QStringLiteral("clipEqBand%1Gain").arg(n));
        m_clipEqGain[band]->setRange(-24.0, 24.0);
        m_clipEqGain[band]->setDecimals(1);
        m_clipEqGain[band]->setSuffix(QStringLiteral(" dB"));
        m_clipEqQ[band] = new QDoubleSpinBox(m_clipEqPanel);
        m_clipEqQ[band]->setObjectName(QStringLiteral("clipEqBand%1Q").arg(n));
        m_clipEqQ[band]->setRange(0.1, 10.0);
        m_clipEqQ[band]->setDecimals(2);
        eqGrid->addWidget(new QLabel(QString::fromUtf8(kBandNames[band]), m_clipEqPanel), band + 1, 0);
        eqGrid->addWidget(m_clipEqFreq[band], band + 1, 1);
        eqGrid->addWidget(m_clipEqGain[band], band + 1, 2);
        eqGrid->addWidget(m_clipEqQ[band], band + 1, 3);
        // One typed value = one valueChanged (Enter / focus-out), so one undo point.
        // Each spin writes only its own field so untouched values keep their stored precision.
        for (QDoubleSpinBox* spin : {m_clipEqFreq[band], m_clipEqGain[band], m_clipEqQ[band]})
            spin->setKeyboardTracking(false);
        connect(m_clipEqFreq[band], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, band](double v) {
            if (m_updatingControls) return;
            eqBandRef(m_clipEq, band).freq = v;
            emitClipEqChanged();
        });
        connect(m_clipEqGain[band], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, band](double v) {
            if (m_updatingControls) return;
            eqBandRef(m_clipEq, band).gainDb = v;
            emitClipEqChanged();
        });
        connect(m_clipEqQ[band], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, band](double v) {
            if (m_updatingControls) return;
            eqBandRef(m_clipEq, band).q = v;
            emitClipEqChanged();
        });
    }
    connect(m_clipEqEnable, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updatingControls) return;
        m_clipEqEnabled = on;
        emitClipEqChanged();
    });
    m_clipEqPanel->installEventFilter(this);
    m_clipEqPanel->hide();
    setClipEq(false, trackfx::EqSettings{});

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

void AudioClipEditor::setClipEq(bool enabled, const trackfx::EqSettings& eq)
{
    m_clipEqEnabled = enabled;
    m_clipEq = eq;
    m_updatingControls = true;
    m_clipEqEnable->setChecked(enabled);
    for (int band = 0; band < 4; ++band) {
        const trackfx::EqBand& eb = eqBandRef(m_clipEq, band);
        m_clipEqFreq[band]->setValue(eb.freq);
        m_clipEqGain[band]->setValue(eb.gainDb);
        m_clipEqQ[band]->setValue(eb.q);
    }
    m_updatingControls = false;
}

bool AudioClipEditor::clipEqEnabled() const
{
    return m_clipEqEnabled;
}

trackfx::EqSettings AudioClipEditor::clipEq() const
{
    return m_clipEq;
}

void AudioClipEditor::setClipEqVisible(bool visible)
{
    m_clipEqPanel->setVisible(visible);
    layoutClipEqPanel();
    update();
}

void AudioClipEditor::setVolumePanVisible(bool visible)
{
    m_controlPanel->setVisible(visible);
}

void AudioClipEditor::setEnvelopeEditable(bool editable)
{
    if (m_envelopeEditable == editable) return;
    m_envelopeEditable = editable;
    m_dragIndex = -1;
    update();
}

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

int AudioClipEditor::eqReserve() const
{
    return (m_clipEqPanel && !m_clipEqPanel->isHidden()) ? clipEqPanelHeight() : 0;
}

// Computed per call so a runtime theme change (spin padding / min-height) is picked up.
int AudioClipEditor::clipEqPanelHeight() const
{
    return std::max(m_clipEqPanel->sizeHint().height(), m_clipEqPanel->layout()->minimumSize().height());
}

void AudioClipEditor::layoutClipEqPanel()
{
    const int reserve = eqReserve();
    if (minimumHeight() != 160 + reserve)
        setMinimumSize(560, 160 + reserve);
    const int panelH = clipEqPanelHeight();
    m_clipEqPanel->setGeometry(0, height() - panelH, width(), panelH);
}

void AudioClipEditor::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::StyleChange && m_clipEqPanel && !m_clipEqPanel->isHidden())
        layoutClipEqPanel();
}

bool AudioClipEditor::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == m_clipEqPanel && e->type() == QEvent::LayoutRequest && !m_clipEqPanel->isHidden())
        layoutClipEqPanel();
    return QWidget::eventFilter(watched, e);
}

void AudioClipEditor::emitClipEqChanged()
{
    emit clipEqChanged(m_clipEqEnabled, m_clipEq);
}

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
    const double h = height() - kControlsHeight - kMarginTop - kMarginBottom - eqReserve();

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
    const double h = height() - kControlsHeight - kMarginTop - kMarginBottom - eqReserve();

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
    if (m_clipEqPanel)
        layoutClipEqPanel();
}

void AudioClipEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int W = width();
    const int H = height() - eqReserve();

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
        p.setPen(QPen(m_envelopeEditable ? QColor(0x4c, 0xaf, 0x50) : QColor(0x60, 0x60, 0x60), 2));
        for (int i = 1; i < m_points.size(); ++i) {
            p.drawLine(pointToPixel(m_points[i - 1]), pointToPixel(m_points[i]));
        }
    }

    // ── Points ────────────────────────────────────────────────────────────
    for (int i = 0; i < m_points.size(); ++i) {
        const QPointF pp = pointToPixel(m_points[i]);
        const bool selected = (i == m_dragIndex);
        const double r = selected ? 7.0 : 5.0;

        const QColor pointColor = m_envelopeEditable ? QColor(0x4c, 0xaf, 0x50) : QColor(0x60, 0x60, 0x60);
        p.setBrush(pointColor);
        if (!m_envelopeEditable) {
            p.setPen(QPen(pointColor, 1));
        } else if (selected) {
            p.setPen(QPen(QColor(0xff, 0xeb, 0x3b), 2));
        } else {
            p.setPen(QPen(pointColor, 1));
        }
        p.drawEllipse(pp, r, r);
    }
}

// ─── mouse ────────────────────────────────────────────────────────────────────

void AudioClipEditor::mousePressEvent(QMouseEvent* e)
{
    if (!m_envelopeEditable) return;
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
    if (!m_envelopeEditable) return;
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
    if (!m_envelopeEditable) return;
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
    if (!m_envelopeEditable) return;
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
