// Sprint 22 — US-EASE-1: modeless easing-curve editor dialog impl.
#include "EasingCurveEditorDialog.h"

#include <QPainter>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QPen>
#include <cmath>

// ---------------------------------------------------------------------------
// CurveWidget
// ---------------------------------------------------------------------------

namespace {
constexpr int   kMargin       = 24;   // px padding around the unit square
constexpr qreal kHandleRadius = 6.0;  // px
constexpr qreal kHandleHit    = 12.0; // px pick radius
}

CurveWidget::CurveWidget(QWidget *parent)
    : QWidget(parent) {
    setMinimumSize(260, 260);
    setMouseTracking(false);
}

void CurveWidget::setBezier(const easing::CubicBezier &bez) {
    m_bez = bez;
    update();
    emit curveChanged();
}

QPointF CurveWidget::toWidget(double nx, double ny) const {
    const double w = width()  - 2.0 * kMargin;
    const double h = height() - 2.0 * kMargin;
    // y is inverted: normalized 0 -> bottom, 1 -> top.
    return QPointF(kMargin + nx * w,
                   kMargin + (1.0 - ny) * h);
}

QPointF CurveWidget::toNormalized(const QPointF &p) const {
    const double w = width()  - 2.0 * kMargin;
    const double h = height() - 2.0 * kMargin;
    const double nx = (w > 0.0) ? (p.x() - kMargin) / w : 0.0;
    const double ny = (h > 0.0) ? 1.0 - (p.y() - kMargin) / h : 0.0;
    return QPointF(nx, ny);
}

void CurveWidget::paintEvent(QPaintEvent * /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.fillRect(rect(), QColor(30, 30, 34));

    // Unit-square border.
    const QPointF bl = toWidget(0.0, 0.0);
    const QPointF tr = toWidget(1.0, 1.0);
    const QRectF box(QPointF(bl.x(), tr.y()), QPointF(tr.x(), bl.y()));

    // Grid (quarters).
    p.setPen(QPen(QColor(60, 60, 66), 1));
    for (int i = 1; i < 4; ++i) {
        const double f = i / 4.0;
        p.drawLine(toWidget(f, 0.0), toWidget(f, 1.0));
        p.drawLine(toWidget(0.0, f), toWidget(1.0, f));
    }
    p.setPen(QPen(QColor(110, 110, 120), 1));
    p.drawRect(box);

    // Handle guide lines from the fixed endpoints.
    const QPointF h1 = toWidget(m_bez.x1, m_bez.y1);
    const QPointF h2 = toWidget(m_bez.x2, m_bez.y2);
    p.setPen(QPen(QColor(120, 140, 200, 160), 1, Qt::DashLine));
    p.drawLine(toWidget(0.0, 0.0), h1);
    p.drawLine(toWidget(1.0, 1.0), h2);

    // Sampled curve: (t, evaluate(CubicBezier, t)) for t = 0..1 step 0.01.
    QPolygonF poly;
    for (int i = 0; i <= 100; ++i) {
        const double t = i / 100.0;
        const double y = easing::evaluate(easing::EasingType::CubicBezier, t, m_bez);
        poly << toWidget(t, y);
    }
    p.setPen(QPen(QColor(90, 200, 140), 2));
    p.drawPolyline(poly);

    // Draggable handles.
    p.setPen(QPen(QColor(230, 230, 240), 1));
    p.setBrush(QColor(90, 140, 230));
    p.drawEllipse(h1, kHandleRadius, kHandleRadius);
    p.setBrush(QColor(230, 140, 90));
    p.drawEllipse(h2, kHandleRadius, kHandleRadius);
}

void CurveWidget::mousePressEvent(QMouseEvent *event) {
    const QPointF pos = event->position();
    const QPointF h1 = toWidget(m_bez.x1, m_bez.y1);
    const QPointF h2 = toWidget(m_bez.x2, m_bez.y2);

    const double d1 = std::hypot(pos.x() - h1.x(), pos.y() - h1.y());
    const double d2 = std::hypot(pos.x() - h2.x(), pos.y() - h2.y());

    if (d1 <= kHandleHit && d1 <= d2)
        m_dragHandle = 0;
    else if (d2 <= kHandleHit)
        m_dragHandle = 1;
    else
        m_dragHandle = -1;
}

void CurveWidget::mouseMoveEvent(QMouseEvent *event) {
    if (m_dragHandle < 0)
        return;
    QPointF n = toNormalized(event->position());

    // Clamp X to [0,1] (CSS requires monotone x for the bezier solver to be
    // well-defined); Y is free to allow back/overshoot style curves.
    double nx = n.x();
    if (nx < 0.0) nx = 0.0;
    if (nx > 1.0) nx = 1.0;
    const double ny = n.y();

    if (m_dragHandle == 0) {
        m_bez.x1 = nx;
        m_bez.y1 = ny;
    } else {
        m_bez.x2 = nx;
        m_bez.y2 = ny;
    }
    update();
    emit curveChanged();
}

void CurveWidget::mouseReleaseEvent(QMouseEvent * /*event*/) {
    m_dragHandle = -1;
}

// ---------------------------------------------------------------------------
// EasingCurveEditorDialog
// ---------------------------------------------------------------------------

EasingCurveEditorDialog::EasingCurveEditorDialog(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Easing Curve Editor"));
    setModal(false); // modeless unless the caller uses exec()

    auto *layout = new QVBoxLayout(this);

    m_presetCombo = new QComboBox(this);
    const QVector<easing::NamedCurve> ps = easing::presets();
    for (const easing::NamedCurve &nc : ps)
        m_presetCombo->addItem(nc.name);
    layout->addWidget(m_presetCombo);

    m_curve = new CurveWidget(this);
    layout->addWidget(m_curve, 1);

    m_valueLabel = new QLabel(this);
    m_valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_valueLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    layout->addWidget(buttons);

    connect(m_presetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EasingCurveEditorDialog::onPresetChanged);
    connect(m_curve, &CurveWidget::curveChanged,
            this, &EasingCurveEditorDialog::onCurveEdited);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (!ps.isEmpty())
        m_curve->setBezier(ps.front().bez);
    onPresetChanged(m_presetCombo->currentIndex());
}

void EasingCurveEditorDialog::setInitialCurve(double x1, double y1, double x2, double y2) {
    m_presetCombo->setCurrentIndex(-1);
    m_curve->setBezier(easing::CubicBezier{x1, y1, x2, y2});
    updateValueLabel();
}

void EasingCurveEditorDialog::getCurve(double &x1, double &y1, double &x2, double &y2) const {
    const easing::CubicBezier b = m_curve->bezier();
    x1 = b.x1;
    y1 = b.y1;
    x2 = b.x2;
    y2 = b.y2;
}

void EasingCurveEditorDialog::onPresetChanged(int index) {
    const QVector<easing::NamedCurve> ps = easing::presets();
    if (index >= 0 && index < ps.size())
        m_curve->setBezier(ps.at(index).bez);
    updateValueLabel();
}

void EasingCurveEditorDialog::onCurveEdited() {
    updateValueLabel();
}

void EasingCurveEditorDialog::updateValueLabel() {
    const easing::CubicBezier b = m_curve->bezier();
    m_valueLabel->setText(
        tr("cubic-bezier(%1, %2, %3, %4)")
            .arg(b.x1, 0, 'f', 3)
            .arg(b.y1, 0, 'f', 3)
            .arg(b.x2, 0, 'f', 3)
            .arg(b.y2, 0, 'f', 3));
}
