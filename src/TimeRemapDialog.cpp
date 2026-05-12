// ---------------------------------------------------------------------------
// TimeRemapDialog.cpp
//
// NOTE: CurveEditorWidget is a nested Q_OBJECT class defined in this TU.
// The final `#include "TimeRemapDialog.moc"` is required for AUTOMOC to pick
// up both QObject subclasses defined here.
// ---------------------------------------------------------------------------

#include "TimeRemapDialog.h"

#include <QBoxLayout>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <algorithm>
#include <cmath>
#include <functional>

// ===========================================================================
// CurveEditorWidget
// ===========================================================================
// A minimal 2-D curve editor.
//   X axis = output time (seconds)
//   Y axis = source time (seconds)
//
// Mouse interaction:
//   Left-click on empty space → add a key
//   Left-drag on a handle     → move the key
//   Right-click on a handle   → remove the key
//
// The widget is NOT authoritative; it reads/writes through TimeRemapDialog
// via the curveRef() accessor and emits curveEdited() after each change.
// ===========================================================================

class CurveEditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CurveEditorWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(300, 200);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }

    // The dialog sets this pointer to its internal curve before every repaint.
    void setCurvePtr(timeremap::TimeRemapCurve* c) { m_curve = c; }

    // x/y axis extents used for the graph mapping; set by the dialog.
    void setAxisRange(double xMin, double xMax, double yMin, double yMax)
    {
        m_xMin = xMin; m_xMax = xMax;
        m_yMin = yMin; m_yMax = yMax;
        update();
    }

signals:
    void curveEdited();

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRect r = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
        p.fillRect(rect(), QColor(40, 40, 40));
        p.fillRect(r, QColor(30, 30, 30));

        // Grid lines
        p.setPen(QColor(60, 60, 60));
        for (int i = 1; i < 4; ++i) {
            int x = r.left() + r.width()  * i / 4;
            int y = r.top()  + r.height() * i / 4;
            p.drawLine(x, r.top(), x, r.bottom());
            p.drawLine(r.left(), y, r.right(), y);
        }

        if (!m_curve) return;

        // Draw piecewise-linear curve
        const int nKeys = static_cast<int>(m_curve->keys.size());
        if (nKeys >= 2) {
            p.setPen(QPen(QColor(80, 200, 120), 2));
            for (int i = 0; i < nKeys - 1; ++i) {
                QPointF a = toWidget(m_curve->keys[i].outTime,  m_curve->keys[i].srcTime,  r);
                QPointF b = toWidget(m_curve->keys[i+1].outTime, m_curve->keys[i+1].srcTime, r);
                p.drawLine(a, b);
            }
        } else if (nKeys == 1) {
            // Flat line at constant srcTime
            double st = m_curve->keys[0].srcTime;
            QPointF a = toWidget(m_xMin, st, r);
            QPointF b = toWidget(m_xMax, st, r);
            p.setPen(QPen(QColor(80, 200, 120), 2));
            p.drawLine(a, b);
        }

        // Draw handles
        for (int i = 0; i < nKeys; ++i) {
            QPointF pt = toWidget(m_curve->keys[i].outTime, m_curve->keys[i].srcTime, r);
            bool isHot = (i == m_dragIndex || i == m_hoverIndex);
            p.setPen(Qt::NoPen);
            p.setBrush(isHot ? QColor(255, 220, 60) : QColor(200, 120, 60));
            p.drawEllipse(pt, kHandleR, kHandleR);
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (!m_curve) return;
        const QRect r = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);

        int hit = hitTest(e->pos(), r);

        if (e->button() == Qt::LeftButton) {
            if (hit >= 0) {
                m_dragIndex = hit;
                m_dragOffset = e->pos() - toWidget(m_curve->keys[hit].outTime,
                                                    m_curve->keys[hit].srcTime, r).toPoint();
            } else {
                // Add a key
                auto [ot, st] = fromWidget(e->pos(), r);
                m_curve->addKey(ot, st);
                // find the newly inserted key
                m_dragIndex = -1;
                for (int i = 0; i < static_cast<int>(m_curve->keys.size()); ++i) {
                    if (std::abs(m_curve->keys[i].outTime - ot) < 1e-9) {
                        m_dragIndex = i;
                        break;
                    }
                }
                emit curveEdited();
                update();
            }
        } else if (e->button() == Qt::RightButton) {
            if (hit >= 0) {
                m_curve->keys.remove(hit);
                m_dragIndex  = -1;
                m_hoverIndex = -1;
                emit curveEdited();
                update();
            }
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_curve) return;
        const QRect r = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);

        if (m_dragIndex >= 0 && (e->buttons() & Qt::LeftButton)) {
            QPoint adjusted = e->pos() - m_dragOffset;
            auto [ot, st] = fromWidget(adjusted, r);

            // Clamp outTime to keep keys monotonically ordered
            const int n = static_cast<int>(m_curve->keys.size());
            double minOt = (m_dragIndex > 0)     ? m_curve->keys[m_dragIndex - 1].outTime + 1e-6 : m_xMin;
            double maxOt = (m_dragIndex < n - 1) ? m_curve->keys[m_dragIndex + 1].outTime - 1e-6 : m_xMax;
            ot = std::max(minOt, std::min(maxOt, ot));
            st = std::max(m_yMin, std::min(m_yMax, st));

            m_curve->keys[m_dragIndex].outTime = ot;
            m_curve->keys[m_dragIndex].srcTime = st;
            emit curveEdited();
            update();
        } else {
            int hit = hitTest(e->pos(), r);
            if (hit != m_hoverIndex) {
                m_hoverIndex = hit;
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        m_dragIndex = -1;
    }

private:
    // Convert curve-space → widget-space
    QPointF toWidget(double ot, double st, const QRect& r) const
    {
        double xRange = m_xMax - m_xMin;
        double yRange = m_yMax - m_yMin;
        if (xRange < 1e-9) xRange = 1.0;
        if (yRange < 1e-9) yRange = 1.0;
        double wx = r.left() + (ot - m_xMin) / xRange * r.width();
        // Y axis: srcTime grows upward visually
        double wy = r.bottom() - (st - m_yMin) / yRange * r.height();
        return QPointF(wx, wy);
    }

    // Convert widget-space → curve-space
    std::pair<double, double> fromWidget(const QPoint& pt, const QRect& r) const
    {
        double xRange = m_xMax - m_xMin;
        double yRange = m_yMax - m_yMin;
        if (xRange < 1e-9) xRange = 1.0;
        if (yRange < 1e-9) yRange = 1.0;
        double ot = m_xMin + (pt.x() - r.left()) / static_cast<double>(r.width())  * xRange;
        double st = m_yMin + (r.bottom() - pt.y()) / static_cast<double>(r.height()) * yRange;
        return {ot, st};
    }

    int hitTest(const QPoint& pos, const QRect& r) const
    {
        if (!m_curve) return -1;
        for (int i = 0; i < static_cast<int>(m_curve->keys.size()); ++i) {
            QPointF pt = toWidget(m_curve->keys[i].outTime, m_curve->keys[i].srcTime, r);
            if (std::hypot(pos.x() - pt.x(), pos.y() - pt.y()) <= kHandleR + 2.0)
                return i;
        }
        return -1;
    }

    static constexpr int    kMargin  = 20;
    static constexpr double kHandleR = 6.0;

    timeremap::TimeRemapCurve* m_curve      = nullptr;
    int                        m_dragIndex  = -1;
    int                        m_hoverIndex = -1;
    QPoint                     m_dragOffset;

    double m_xMin = 0.0, m_xMax = 10.0;
    double m_yMin = 0.0, m_yMax = 10.0;
};

// ===========================================================================
// TimeRemapDialog — implementation
// ===========================================================================

TimeRemapDialog::TimeRemapDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Time Remap — カーブ編集"));
    setMinimumSize(640, 600);

    // ---- Curve editor ----
    m_curveEditor = new CurveEditorWidget(this);
    m_curveEditor->setCurvePtr(&m_curve);

    // ---- Table ----
    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({tr("Output Time (s)"), tr("Source Time (s)")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setMinimumHeight(120);

    auto* addBtn    = new QPushButton(tr("Add Row"),    this);
    auto* removeBtn = new QPushButton(tr("Remove Row"), this);
    auto* tableButtonLayout = new QHBoxLayout;
    tableButtonLayout->addWidget(addBtn);
    tableButtonLayout->addWidget(removeBtn);
    tableButtonLayout->addStretch();

    // ---- Controls ----
    m_blendCombo = new QComboBox(this);
    m_blendCombo->addItem(tr("Nearest Frame"));   // index 0
    m_blendCombo->addItem(tr("Blend"));           // index 1
    m_blendCombo->addItem(tr("Optical Flow"));    // index 2

    m_fpsSpin = new QDoubleSpinBox(this);
    m_fpsSpin->setRange(1.0, 240.0);
    m_fpsSpin->setDecimals(3);
    m_fpsSpin->setValue(30.0);
    m_fpsSpin->setSuffix(tr(" fps"));

    auto* controlsForm = new QFormLayout;
    controlsForm->addRow(tr("Blend Mode:"),   m_blendCombo);
    controlsForm->addRow(tr("Source FPS:"),   m_fpsSpin);

    // ---- Preview ----
    m_previewSlider = new QSlider(Qt::Horizontal, this);
    m_previewSlider->setRange(0, 1000);  // refined in setSourceFrameCount
    m_previewSlider->setValue(0);

    m_previewLabel = new QLabel(this);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumSize(160, 90);
    m_previewLabel->setStyleSheet("background: #111; border: 1px solid #555;");
    m_previewLabel->setText(tr("(no fetcher)"));

    auto* previewLayout = new QHBoxLayout;
    previewLayout->addWidget(new QLabel(tr("Preview T:"), this));
    previewLayout->addWidget(m_previewSlider, 1);
    previewLayout->addWidget(m_previewLabel);

    // ---- Button box ----
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    // ---- Assemble layout ----
    auto* tableGroup = new QGroupBox(tr("Keyframes"), this);
    auto* tableVBox  = new QVBoxLayout(tableGroup);
    tableVBox->addWidget(m_table);
    tableVBox->addLayout(tableButtonLayout);

    auto* controlsGroup = new QGroupBox(tr("Controls"), this);
    controlsGroup->setLayout(controlsForm);

    auto* previewGroup = new QGroupBox(tr("Preview"), this);
    auto* previewVBox  = new QVBoxLayout(previewGroup);
    previewVBox->addLayout(previewLayout);

    auto* rightVBox = new QVBoxLayout;
    rightVBox->addWidget(tableGroup);
    rightVBox->addWidget(controlsGroup);
    rightVBox->addWidget(previewGroup);
    rightVBox->addStretch();

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    {
        auto* leftWidget = new QWidget(splitter);
        auto* leftVBox   = new QVBoxLayout(leftWidget);
        leftVBox->setContentsMargins(0, 0, 0, 0);
        leftVBox->addWidget(m_curveEditor);
        splitter->addWidget(leftWidget);
    }
    {
        auto* rightWidget = new QWidget(splitter);
        rightWidget->setLayout(rightVBox);
        splitter->addWidget(rightWidget);
    }
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(splitter, 1);
    mainLayout->addWidget(m_buttonBox);

    // ---- Connections ----
    connect(m_table, &QTableWidget::cellChanged,
            this, &TimeRemapDialog::onTableCellChanged);
    connect(addBtn,    &QPushButton::clicked, this, &TimeRemapDialog::onAddRow);
    connect(removeBtn, &QPushButton::clicked, this, &TimeRemapDialog::onRemoveRow);
    connect(m_blendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TimeRemapDialog::onBlendModeChanged);
    connect(m_fpsSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &TimeRemapDialog::onSourceFpsChanged);
    connect(m_previewSlider, &QSlider::valueChanged,
            this, &TimeRemapDialog::onPreviewSliderMoved);
    connect(m_curveEditor, &CurveEditorWidget::curveEdited,
            this, &TimeRemapDialog::onCurveEditorChanged);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Initialise with a default empty curve
    refreshAxisRange();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TimeRemapDialog::setCurve(const timeremap::TimeRemapCurve& c)
{
    m_curve = c;
    m_blockSignals = true;

    // Controls
    m_blendCombo->setCurrentIndex(static_cast<int>(m_curve.blendMode));
    m_fpsSpin->setValue(m_curve.sourceFps);

    // Table
    rebuildTable();

    m_blockSignals = false;

    refreshAxisRange();
    m_curveEditor->update();
}

timeremap::TimeRemapCurve TimeRemapDialog::curve() const
{
    return m_curve;
}

void TimeRemapDialog::setFrameFetcher(std::function<QImage(int)> f)
{
    m_fetcher = std::move(f);
    if (m_fetcher) {
        m_previewLabel->setText(QString());
    } else {
        m_previewLabel->setText(tr("(no fetcher)"));
    }
}

void TimeRemapDialog::setSourceFrameCount(int n)
{
    m_sourceFrameCount = std::max(1, n);
    m_previewSlider->setRange(0, m_sourceFrameCount - 1);
    refreshAxisRange();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void TimeRemapDialog::onTableCellChanged(int row, int col)
{
    if (m_blockSignals) return;
    Q_UNUSED(row); Q_UNUSED(col);
    syncCurveFromTable();
    refreshAxisRange();
    m_curveEditor->update();
    emitCurveChanged();
}

void TimeRemapDialog::onAddRow()
{
    // Append a new row at the end with sensible defaults
    double nextOt = 0.0;
    double nextSt = 0.0;
    if (!m_curve.keys.isEmpty()) {
        const auto& last = m_curve.keys.last();
        nextOt = last.outTime + 1.0;
        nextSt = last.srcTime + 1.0;
    }

    m_blockSignals = true;
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(QString::number(nextOt, 'f', 4)));
    m_table->setItem(row, 1, new QTableWidgetItem(QString::number(nextSt, 'f', 4)));
    m_blockSignals = false;

    syncCurveFromTable();
    refreshAxisRange();
    m_curveEditor->update();
    emitCurveChanged();
}

void TimeRemapDialog::onRemoveRow()
{
    const int row = m_table->currentRow();
    if (row < 0) return;

    m_blockSignals = true;
    m_table->removeRow(row);
    m_blockSignals = false;

    syncCurveFromTable();
    refreshAxisRange();
    m_curveEditor->update();
    emitCurveChanged();
}

void TimeRemapDialog::onBlendModeChanged(int index)
{
    if (m_blockSignals) return;
    m_curve.blendMode = static_cast<timeremap::FrameBlendMode>(index);
    emitCurveChanged();
}

void TimeRemapDialog::onSourceFpsChanged(double fps)
{
    if (m_blockSignals) return;
    m_curve.sourceFps = fps;
    refreshAxisRange();
    emitCurveChanged();
}

void TimeRemapDialog::onPreviewSliderMoved(int value)
{
    Q_UNUSED(value);
    updatePreview();
}

void TimeRemapDialog::onCurveEditorChanged()
{
    // The curve was edited in-place by CurveEditorWidget; sync the table.
    m_blockSignals = true;
    rebuildTable();
    m_blockSignals = false;
    emitCurveChanged();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void TimeRemapDialog::rebuildTable()
{
    m_table->setRowCount(0);
    const int n = static_cast<int>(m_curve.keys.size());
    m_table->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        m_table->setItem(i, 0, new QTableWidgetItem(
            QString::number(m_curve.keys[i].outTime, 'f', 4)));
        m_table->setItem(i, 1, new QTableWidgetItem(
            QString::number(m_curve.keys[i].srcTime, 'f', 4)));
    }
}

void TimeRemapDialog::syncCurveFromTable()
{
    const int n = m_table->rowCount();
    QVector<timeremap::TimeRemapKey> keys;
    keys.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QTableWidgetItem* itemOt = m_table->item(i, 0);
        const QTableWidgetItem* itemSt = m_table->item(i, 1);
        if (!itemOt || !itemSt) continue;
        bool okOt = false, okSt = false;
        double ot = itemOt->text().toDouble(&okOt);
        double st = itemSt->text().toDouble(&okSt);
        if (okOt && okSt) {
            timeremap::TimeRemapKey k;
            k.outTime = ot;
            k.srcTime = st;
            keys.append(k);
        }
    }
    // Sort by outTime to maintain monotonic order
    std::sort(keys.begin(), keys.end(), [](const timeremap::TimeRemapKey& a,
                                           const timeremap::TimeRemapKey& b) {
        return a.outTime < b.outTime;
    });
    m_curve.keys = keys;
}

void TimeRemapDialog::updatePreview()
{
    if (!m_fetcher) return;

    // Map slider value → output time
    const int sliderMax = std::max(1, m_previewSlider->maximum());
    const double duration = (m_curve.sourceFps > 0.0 && m_sourceFrameCount > 0)
        ? m_sourceFrameCount / m_curve.sourceFps
        : 10.0;
    const double T = (m_previewSlider->value() / static_cast<double>(sliderMax)) * duration;

    const QImage img = timeremap::resolveFrame(m_curve, T, m_fetcher);
    if (img.isNull()) {
        m_previewLabel->setText(tr("(null frame)"));
    } else {
        QPixmap pm = QPixmap::fromImage(img).scaled(
            m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_previewLabel->setPixmap(pm);
    }
}

void TimeRemapDialog::emitCurveChanged()
{
    if (!m_blockSignals)
        emit curveChanged();
}

void TimeRemapDialog::refreshAxisRange()
{
    double xMax = 10.0;
    double yMax = 10.0;

    if (m_curve.sourceFps > 0.0 && m_sourceFrameCount > 0) {
        yMax = m_sourceFrameCount / m_curve.sourceFps;
        xMax = yMax; // sensible default: output same length as source
    }
    if (!m_curve.keys.isEmpty()) {
        for (const auto& k : m_curve.keys) {
            xMax = std::max(xMax, k.outTime * 1.1);
            yMax = std::max(yMax, k.srcTime * 1.1);
        }
    }

    m_curveEditor->setAxisRange(0.0, xMax, 0.0, yMax);
}

// ---------------------------------------------------------------------------
// MOC include — required because CurveEditorWidget (Q_OBJECT) is defined in
// this TU, not in its own header.  AUTOMOC finds it via this directive.
// ---------------------------------------------------------------------------
#include "TimeRemapDialog.moc"
