// RotoToolsDialog.cpp — US-PRO-7
// Qt Widgets only. No third-party deps.

#include "RotoToolsDialog.h"

#include "RotoAutoTrace.h"
#include "RotoTracking.h"
#include "RotoBrushTool.h"

#include <cmath>
#include <algorithm>

#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QRubberBand>
#include <QSize>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

// ============================================================
//  RotoCanvasWidget — private canvas (defined in this TU only)
// ============================================================

class RotoCanvasWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RotoCanvasWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(320, 240);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    // --- Data setters ---

    void setFrame(const QImage &img)
    {
        m_frame = img;
        update();
    }

    void setRotoPath(const RotoPath &path)
    {
        m_rotoPath = path;
        update();
    }

    const RotoPath &rotoPath() const { return m_rotoPath; }

    void setBrushMask(const QImage &mask)
    {
        m_brushMask = mask;
        update();
    }

    const QImage &brushMask() const { return m_brushMask; }

    // non-owning pointer — dialog owns the object
    void setBrushToolPtr(rotobrush::RotoBrushTool *tool) { m_brushTool = tool; }

    void setBrushMode(bool enabled)
    {
        m_brushMode = enabled;
        update();
    }

    bool brushMode() const { return m_brushMode; }

    void setSnapMode(bool enabled) { m_snapMode = enabled; }

    // Seed rect in frame-space coordinates.
    QRectF seedRect() const { return m_seedRect; }
    bool   hasSeedRect() const { return m_hasSeedRect; }
    void   clearSeedRect() { m_hasSeedRect = false; update(); }

    // Render the current canvas state off-screen (for canvasRender()).
    QImage renderToImage() const
    {
        if (m_frame.isNull())
            return QImage();
        QImage result(m_frame.size(), QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        QPainter p(&result);
        paintCanvas(p);
        return result;
    }

    // Coordinate helpers.
    double scale() const
    {
        if (m_frame.isNull() || width() == 0 || height() == 0)
            return 1.0;
        double sx = static_cast<double>(width())  / m_frame.width();
        double sy = static_cast<double>(height()) / m_frame.height();
        return std::min(sx, sy);
    }

    QPointF offset() const
    {
        if (m_frame.isNull())
            return QPointF(0.0, 0.0);
        double s = scale();
        return QPointF((width()  - m_frame.width()  * s) * 0.5,
                       (height() - m_frame.height() * s) * 0.5);
    }

    QPointF toFrame(QPoint widgetPos) const
    {
        QPointF o = offset();
        double s = scale();
        if (s <= 0.0) return QPointF();
        return QPointF((widgetPos.x() - o.x()) / s,
                       (widgetPos.y() - o.y()) / s);
    }

signals:
    void pathChanged();
    // Emitted when a snap-mode point drag is released: carries point index and
    // the raw frame-space drop position so the dialog can call snapPointToEdge.
    void pointReleased(int idx, QPointF framePos);
    // isFirst=true → beginStroke must be called; false → strokeTo
    void brushStroke(QPointF pos, bool isFirst);
    void seedRectChanged();

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!m_frame.isNull())
            paintCanvas(p);
    }

    void mousePressEvent(QMouseEvent *ev) override
    {
        if (ev->button() != Qt::LeftButton)
            return;

        QPointF fp = toFrame(ev->pos());

        if (m_brushMode) {
            m_brushDragging = true;
            m_lastBrushPos  = fp;
            emit brushStroke(fp, true);
            return;
        }

        if (m_snapMode) {
            int idx = nearestPointIndex(fp, 12.0);
            if (idx >= 0) {
                m_draggingPoint = true;
                m_dragPointIdx  = idx;
                return;
            }
        }

        // Start rubber-band.
        m_rbOrigin = ev->pos();
        if (!m_rubberBand)
            m_rubberBand = new QRubberBand(QRubberBand::Rectangle, this);
        m_rubberBand->setGeometry(QRect(m_rbOrigin, QSize()));
        m_rubberBand->show();
        m_hasSeedRect = false;
    }

    void mouseMoveEvent(QMouseEvent *ev) override
    {
        QPointF fp = toFrame(ev->pos());

        if (m_brushDragging) {
            emit brushStroke(fp, false);
            m_lastBrushPos = fp;
            return;
        }

        if (m_draggingPoint && m_dragPointIdx >= 0
                && m_dragPointIdx < static_cast<int>(m_rotoPath.points.size())) {
            // Live-move for visual feedback; snap applied on release.
            m_rotoPath.points[m_dragPointIdx].position = fp;
            update();
            return;
        }

        if (m_rubberBand && (ev->buttons() & Qt::LeftButton))
            m_rubberBand->setGeometry(QRect(m_rbOrigin, ev->pos()).normalized());
    }

    void mouseReleaseEvent(QMouseEvent *ev) override
    {
        if (ev->button() != Qt::LeftButton)
            return;

        if (m_brushDragging) {
            m_brushDragging = false;
            return;
        }

        if (m_draggingPoint) {
            int  releasedIdx = m_dragPointIdx;
            QPointF releasePos = toFrame(ev->pos());
            m_draggingPoint = false;
            m_dragPointIdx  = -1;
            if (m_snapMode && releasedIdx >= 0
                    && releasedIdx < static_cast<int>(m_rotoPath.points.size())) {
                // Snap: dialog handles the snapPointToEdge call via pointReleased.
                emit pointReleased(releasedIdx, releasePos);
            } else {
                emit pathChanged();
            }
            return;
        }

        if (m_rubberBand) {
            QRect geom = QRect(m_rbOrigin, ev->pos()).normalized();
            m_rubberBand->hide();
            if (geom.width() > 4 && geom.height() > 4) {
                QPointF tl = toFrame(geom.topLeft());
                QPointF br = toFrame(geom.bottomRight());
                m_seedRect    = QRectF(tl, br).normalized();
                m_hasSeedRect = true;
                emit seedRectChanged();
                update();
            }
        }
    }

private:
    // Build a QPainterPath from m_rotoPath in frame-space coordinates.
    QPainterPath buildBezierPath() const
    {
        const QVector<RotoPoint> &pts = m_rotoPath.points;
        if (pts.isEmpty())
            return QPainterPath();

        QPainterPath pp;
        pp.moveTo(pts[0].position);

        int n = static_cast<int>(pts.size());
        for (int i = 1; i < n; ++i) {
            const RotoPoint &prev = pts[i - 1];
            const RotoPoint &curr = pts[i];
            // Handles are ABSOLUTE coords (see RotoTracking.h, Rotoscope.cpp).
            pp.cubicTo(prev.handleOut, curr.handleIn, curr.position);
        }

        if (m_rotoPath.closed && n > 1) {
            const RotoPoint &last  = pts[n - 1];
            const RotoPoint &first = pts[0];
            pp.cubicTo(last.handleOut, first.handleIn, first.position);
            pp.closeSubpath();
        }

        return pp;
    }

    // Shared paint logic used by paintEvent and renderToImage.
    void paintCanvas(QPainter &p) const
    {
        double s = scale();
        QPointF o = offset();

        p.save();
        p.translate(o);
        p.scale(s, s);

        // 1. Frame image.
        p.drawImage(QPointF(0.0, 0.0), m_frame);

        // 2. Brush mask overlay.
        if (m_brushMode && m_brushTool) {
            QImage overlay = m_brushTool->previewOverlay(QColor(0, 220, 60, 170));
            if (!overlay.isNull())
                p.drawImage(QPointF(0.0, 0.0), overlay);
        }

        // 3. Roto path.
        if (!m_rotoPath.points.isEmpty()) {
            QPainterPath pp = buildBezierPath();

            // Semi-transparent fill.
            p.fillPath(pp, QColor(255, 220, 0, 35));

            // Outline — cosmetic pen stays 1.5 screen pixels regardless of scale.
            QPen pen(QColor(255, 200, 0), 1.5);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(pp);

            // Control point dots.
            QPen ptPen(Qt::white, 1.0);
            ptPen.setCosmetic(true);
            p.setPen(ptPen);
            p.setBrush(QColor(255, 100, 0));
            double r = 4.0 / s;  // fixed 4 screen pixels
            for (const auto &pt : m_rotoPath.points)
                p.drawEllipse(pt.position, r, r);
        }

        // 4. Seed rect.
        if (m_hasSeedRect) {
            QPen seedPen(QColor(100, 200, 255), 1.0);
            seedPen.setCosmetic(true);
            seedPen.setStyle(Qt::DashLine);
            p.setPen(seedPen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(m_seedRect);
        }

        p.restore();
    }

    // Returns index of nearest control point within maxDist frame-pixels, or -1.
    int nearestPointIndex(QPointF fp, double maxDist) const
    {
        int    best     = -1;
        double bestDist = maxDist * maxDist;
        const QVector<RotoPoint> &pts = m_rotoPath.points;
        for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
            double dx = pts[i].position.x() - fp.x();
            double dy = pts[i].position.y() - fp.y();
            double d2 = dx * dx + dy * dy;
            if (d2 < bestDist) {
                bestDist = d2;
                best     = i;
            }
        }
        return best;
    }

    // State.
    QImage   m_frame;
    RotoPath m_rotoPath;
    QImage   m_brushMask;

    // Non-owning pointer; null until brush mode is first entered.
    rotobrush::RotoBrushTool *m_brushTool = nullptr;

    bool    m_brushMode     = false;
    bool    m_snapMode      = false;
    bool    m_brushDragging = false;
    bool    m_draggingPoint = false;
    int     m_dragPointIdx  = -1;
    QPointF m_lastBrushPos;

    // Rubber-band for seed rect selection.
    QRubberBand *m_rubberBand  = nullptr;
    QPoint       m_rbOrigin;
    QRectF       m_seedRect;
    bool         m_hasSeedRect = false;
};

// ============================================================
//  RotoToolsDialog implementation
// ============================================================

RotoToolsDialog::RotoToolsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Roto Tools"));
    setMinimumSize(800, 560);

    // ---- Canvas ----
    m_canvas = new RotoCanvasWidget(this);

    connect(m_canvas, &RotoCanvasWidget::pathChanged, this, [this]() {
        m_rotoPath = m_canvas->rotoPath();
        emit rotoEdited();
    });

    connect(m_canvas, &RotoCanvasWidget::brushStroke,
            this, [this](QPointF pos, bool isFirst) {
        if (!m_brushTool)
            return;
        if (isFirst) {
            rotobrush::RotoBrushTool::Op op =
                (m_radAdd && m_radAdd->isChecked())
                    ? rotobrush::RotoBrushTool::Op::Add
                    : rotobrush::RotoBrushTool::Op::Subtract;
            m_brushTool->beginStroke(pos, op);
        } else {
            m_brushTool->strokeTo(pos);
        }
        m_canvas->setBrushMask(m_brushTool->mask());
    });

    connect(m_canvas, &RotoCanvasWidget::seedRectChanged,
            this, [this]() { m_canvas->update(); });

    // ---- Auto-Trace group ----
    auto *grpTrace = new QGroupBox(tr("Auto-Trace"), this);
    {
        auto *grpLayout = new QFormLayout(grpTrace);

        m_spnBlur = new QDoubleSpinBox(grpTrace);
        m_spnBlur->setRange(0.0, 20.0);
        m_spnBlur->setSingleStep(0.5);
        m_spnBlur->setValue(1.5);
        m_spnBlur->setToolTip(tr("Pre-blur sigma (pixels)"));
        grpLayout->addRow(tr("Blur radius:"), m_spnBlur);

        m_spnEdgeThresh = new QDoubleSpinBox(grpTrace);
        m_spnEdgeThresh->setRange(0.0, 360.0);
        m_spnEdgeThresh->setSingleStep(5.0);
        m_spnEdgeThresh->setValue(40.0);
        m_spnEdgeThresh->setToolTip(tr("Sobel magnitude threshold [0..255*sqrt2]"));
        grpLayout->addRow(tr("Edge threshold:"), m_spnEdgeThresh);

        m_spnMaxPoints = new QSpinBox(grpTrace);
        m_spnMaxPoints->setRange(4, 200);
        m_spnMaxPoints->setValue(40);
        grpLayout->addRow(tr("Max points:"), m_spnMaxPoints);

        m_btnAutoTrace = new QPushButton(tr("Auto-Trace"), grpTrace);
        grpLayout->addRow(m_btnAutoTrace);
    }

    // ---- Snap to edge ----
    m_chkSnapEdge = new QCheckBox(tr("Snap to edge (click near control point)"), this);
    m_chkSnapEdge->setToolTip(
        tr("Click within 12 px of a control point to snap it to the nearest strong edge."));

    // ---- Track Shape group ----
    auto *grpTrack = new QGroupBox(tr("Track Shape"), this);
    {
        auto *grpLayout = new QFormLayout(grpTrack);

        m_spnKeyInterval = new QSpinBox(grpTrack);
        m_spnKeyInterval->setRange(1, 120);
        m_spnKeyInterval->setValue(5);
        m_spnKeyInterval->setToolTip(tr("Emit a keyframe every N frames"));
        grpLayout->addRow(tr("Keyframe interval:"), m_spnKeyInterval);

        m_btnTrackShape = new QPushButton(tr("Track Shape"), grpTrack);
        grpLayout->addRow(m_btnTrackShape);
    }

    // ---- Brush group ----
    auto *grpBrush = new QGroupBox(tr("Brush Touch-up"), this);
    {
        auto *grpVLayout = new QVBoxLayout(grpBrush);

        m_chkBrushMode = new QCheckBox(tr("Brush mode"), grpBrush);
        grpVLayout->addWidget(m_chkBrushMode);

        auto *brushForm = new QFormLayout();

        m_sldRadius = new QSlider(Qt::Horizontal, grpBrush);
        m_sldRadius->setRange(1, 200);
        m_sldRadius->setValue(10);
        m_lblRadius = new QLabel(tr("10 px"), grpBrush);
        auto *radRow = new QHBoxLayout();
        radRow->addWidget(m_sldRadius, 1);
        radRow->addWidget(m_lblRadius, 0);
        brushForm->addRow(tr("Radius:"), radRow);

        m_sldHardness = new QSlider(Qt::Horizontal, grpBrush);
        m_sldHardness->setRange(0, 100);
        m_sldHardness->setValue(50);
        m_lblHardness = new QLabel(tr("0.50"), grpBrush);
        auto *hardRow = new QHBoxLayout();
        hardRow->addWidget(m_sldHardness, 1);
        hardRow->addWidget(m_lblHardness, 0);
        brushForm->addRow(tr("Hardness:"), hardRow);

        m_sldOpacity = new QSlider(Qt::Horizontal, grpBrush);
        m_sldOpacity->setRange(0, 100);
        m_sldOpacity->setValue(100);
        m_lblOpacity = new QLabel(tr("1.00"), grpBrush);
        auto *opRow = new QHBoxLayout();
        opRow->addWidget(m_sldOpacity, 1);
        opRow->addWidget(m_lblOpacity, 0);
        brushForm->addRow(tr("Opacity:"), opRow);

        grpVLayout->addLayout(brushForm);

        auto *opModeRow = new QHBoxLayout();
        m_radAdd      = new QRadioButton(tr("Add"),      grpBrush);
        m_radSubtract = new QRadioButton(tr("Subtract"), grpBrush);
        m_radAdd->setChecked(true);
        opModeRow->addWidget(m_radAdd);
        opModeRow->addWidget(m_radSubtract);
        grpVLayout->addLayout(opModeRow);

        auto *utilRow = new QHBoxLayout();
        m_btnBrushClear = new QPushButton(tr("Clear"), grpBrush);
        m_btnBrushFill  = new QPushButton(tr("Fill"),  grpBrush);
        m_btnBrushUndo  = new QPushButton(tr("Undo"),  grpBrush);
        utilRow->addWidget(m_btnBrushClear);
        utilRow->addWidget(m_btnBrushFill);
        utilRow->addWidget(m_btnBrushUndo);
        grpVLayout->addLayout(utilRow);
    }

    // ---- OK / Cancel ----
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    // ---- Side panel ----
    auto *sidePanel = new QWidget(this);
    sidePanel->setFixedWidth(264);
    auto *sidePanelLayout = new QVBoxLayout(sidePanel);
    sidePanelLayout->setContentsMargins(0, 0, 0, 0);
    sidePanelLayout->addWidget(grpTrace);
    sidePanelLayout->addWidget(m_chkSnapEdge);
    sidePanelLayout->addWidget(grpTrack);
    sidePanelLayout->addWidget(grpBrush);
    sidePanelLayout->addStretch(1);

    // ---- Content row: canvas | side panel ----
    auto *contentRow = new QHBoxLayout();
    contentRow->addWidget(m_canvas, 1);
    contentRow->addWidget(sidePanel, 0);

    // ---- Root layout (dialog layout) ----
    auto *rootVLayout = new QVBoxLayout(this);
    rootVLayout->addLayout(contentRow, 1);
    rootVLayout->addWidget(m_buttonBox, 0);

    // ---- Connect signals ----
    connect(m_btnAutoTrace,  &QPushButton::clicked, this, &RotoToolsDialog::onAutoTrace);
    connect(m_btnTrackShape, &QPushButton::clicked, this, &RotoToolsDialog::onTrackShape);
    connect(m_chkBrushMode,  &QCheckBox::toggled,  this, &RotoToolsDialog::onBrushModeToggled);
    connect(m_btnBrushClear, &QPushButton::clicked, this, &RotoToolsDialog::onBrushClear);
    connect(m_btnBrushFill,  &QPushButton::clicked, this, &RotoToolsDialog::onBrushFill);
    connect(m_btnBrushUndo,  &QPushButton::clicked, this, &RotoToolsDialog::onBrushUndo);
    connect(m_sldRadius,   &QSlider::valueChanged, this, &RotoToolsDialog::onBrushRadiusChanged);
    connect(m_sldHardness, &QSlider::valueChanged, this, &RotoToolsDialog::onBrushHardnessChanged);
    connect(m_sldOpacity,  &QSlider::valueChanged, this, &RotoToolsDialog::onBrushOpacityChanged);
    connect(m_chkSnapEdge, &QCheckBox::toggled, this, [this](bool on) {
        m_canvas->setSnapMode(on);
    });

    // Snap-to-edge: when snap mode is active and the user releases a dragged
    // control point, call snapPointToEdge and update the path.
    connect(m_canvas, &RotoCanvasWidget::pointReleased,
            this, [this](int idx, QPointF framePos) {
        if (m_currentFrame.isNull())
            return;
        QPointF snapped = rototrace::snapPointToEdge(
            m_currentFrame, framePos, 12.0, 1.0);
        if (idx >= 0 && idx < static_cast<int>(m_rotoPath.points.size())) {
            m_rotoPath.points[idx].position = snapped;
            m_canvas->setRotoPath(m_rotoPath);
            emit rotoEdited();
        }
    });

    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

RotoToolsDialog::~RotoToolsDialog()
{
    delete m_brushTool;
}

// ---- Public API ----

void RotoToolsDialog::setFrame(const QImage &frame)
{
    m_currentFrame = frame;
    m_canvas->setFrame(frame);
}

void RotoToolsDialog::setFrameSequence(const QVector<QImage> &frames, int firstFrameIndex)
{
    m_frames          = frames;
    m_firstFrameIndex = firstFrameIndex;
    if (!frames.isEmpty() && m_currentFrame.isNull())
        setFrame(frames.first());
}

void RotoToolsDialog::setRotoPath(const RotoPath &path)
{
    m_rotoPath = path;
    m_canvas->setRotoPath(path);
}

RotoPath RotoToolsDialog::rotoPath() const
{
    return m_rotoPath;
}

QImage RotoToolsDialog::brushMask() const
{
    if (m_brushTool)
        return m_brushTool->mask();
    return QImage();
}

QVector<RotoKeyframe> RotoToolsDialog::trackedKeyframes() const
{
    return m_trackedKeyframes;
}

QImage RotoToolsDialog::canvasRender() const
{
    return m_canvas->renderToImage();
}

// ---- Slots ----

void RotoToolsDialog::onAutoTrace()
{
    if (m_currentFrame.isNull())
        return;

    rototrace::RotoAutoTraceParams params;
    params.blurRadius    = m_spnBlur->value();
    params.edgeThreshold = m_spnEdgeThresh->value();
    params.maxPoints     = m_spnMaxPoints->value();
    // simplifyEpsilon left at default 2.0.

    QRectF seedRect;
    if (m_canvas->hasSeedRect()) {
        seedRect = m_canvas->seedRect();
    } else if (!m_rotoPath.points.isEmpty()) {
        double minX =  1e18, minY =  1e18;
        double maxX = -1e18, maxY = -1e18;
        for (const RotoPoint &pt : m_rotoPath.points) {
            minX = std::min(minX, pt.position.x());
            minY = std::min(minY, pt.position.y());
            maxX = std::max(maxX, pt.position.x());
            maxY = std::max(maxY, pt.position.y());
        }
        // Clamp to valid range before constructing rect.
        if (maxX > minX && maxY > minY)
            seedRect = QRectF(minX, minY, maxX - minX, maxY - minY);
        else
            seedRect = QRectF(0, 0, m_currentFrame.width(), m_currentFrame.height());
    } else {
        seedRect = QRectF(0, 0, m_currentFrame.width(), m_currentFrame.height());
    }

    RotoPath result = rototrace::autoTraceContour(m_currentFrame, seedRect, params);
    m_rotoPath = result;
    m_canvas->setRotoPath(result);
    m_canvas->clearSeedRect();
    emit rotoEdited();
}

void RotoToolsDialog::onTrackShape()
{
    if (m_frames.isEmpty() || m_rotoPath.points.isEmpty())
        return;

    rototrack::RotoTrackParams params;
    params.keyframeInterval = m_spnKeyInterval->value();
    params.maxScaleDelta    = 0.25;
    params.searchMargin     = 24;
    params.minConfidence    = 0.4;

    m_trackedKeyframes = rototrack::propagateRotoShape(
        m_rotoPath,
        m_firstFrameIndex,
        m_frames,
        m_firstFrameIndex,
        params);

    emit rotoEdited();
}

void RotoToolsDialog::onBrushModeToggled(bool checked)
{
    m_canvas->setBrushMode(checked);
    if (checked) {
        ensureBrushMask();
    }
}

void RotoToolsDialog::onBrushClear()
{
    ensureBrushMask();
    m_brushTool->clear();
    m_canvas->setBrushMask(m_brushTool->mask());
}

void RotoToolsDialog::onBrushFill()
{
    ensureBrushMask();
    m_brushTool->fill();
    m_canvas->setBrushMask(m_brushTool->mask());
}

void RotoToolsDialog::onBrushUndo()
{
    if (m_brushTool) {
        m_brushTool->undo();
        m_canvas->setBrushMask(m_brushTool->mask());
    }
}

void RotoToolsDialog::onBrushRadiusChanged(int value)
{
    m_lblRadius->setText(tr("%1 px").arg(value));
    if (m_brushTool) {
        m_brushTool->setBrush(
            static_cast<double>(value),
            m_sldHardness->value() / 100.0,
            m_sldOpacity->value()  / 100.0);
    }
}

void RotoToolsDialog::onBrushHardnessChanged(int value)
{
    m_lblHardness->setText(QString::number(value / 100.0, 'f', 2));
    if (m_brushTool) {
        m_brushTool->setBrush(
            static_cast<double>(m_sldRadius->value()),
            value / 100.0,
            m_sldOpacity->value() / 100.0);
    }
}

void RotoToolsDialog::onBrushOpacityChanged(int value)
{
    m_lblOpacity->setText(QString::number(value / 100.0, 'f', 2));
    if (m_brushTool) {
        m_brushTool->setBrush(
            static_cast<double>(m_sldRadius->value()),
            m_sldHardness->value() / 100.0,
            value / 100.0);
    }
}

// ---- Private helpers ----

void RotoToolsDialog::ensureBrushMask()
{
    if (!m_brushTool) {
        m_brushTool = new rotobrush::RotoBrushTool();
        m_canvas->setBrushToolPtr(m_brushTool);
    }

    // Reference frame for mask dimensions.
    const QImage &ref = !m_currentFrame.isNull()
        ? m_currentFrame
        : (!m_frames.isEmpty() ? m_frames.first() : QImage());

    if (ref.isNull())
        return;

    // Create or resize mask if needed.
    const QImage &existing = m_brushTool->mask();
    if (existing.isNull() || existing.size() != ref.size()) {
        QImage blank(ref.size(), QImage::Format_Grayscale8);
        blank.fill(0);
        m_brushTool->setMask(blank);
        m_canvas->setBrushMask(m_brushTool->mask());
    }

    // Sync current slider values to tool.
    m_brushTool->setBrush(
        static_cast<double>(m_sldRadius->value()),
        m_sldHardness->value() / 100.0,
        m_sldOpacity->value()  / 100.0);
}

// Pull in MOC output for both Q_OBJECT classes defined in this TU.
#include "RotoToolsDialog.moc"
