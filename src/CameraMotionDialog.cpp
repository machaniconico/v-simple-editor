#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>

#include "CameraMotionDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int    kPreviewW        = 320;
static constexpr int    kPreviewH        = 240;
static constexpr int    kSliderMax       = 1000;
static constexpr double kPreviewMaxSec   = 5.0;

// Ground grid: points on the XZ plane, y = 0, covering -5..5 at 1-unit spacing
static QVector<QVector3D> buildGridPoints()
{
    QVector<QVector3D> pts;
    for (int x = -5; x <= 5; ++x)
        for (int z = -5; z <= 5; ++z)
            pts.append(QVector3D(static_cast<float>(x), 0.0f, static_cast<float>(z)));
    return pts;
}

// ---------------------------------------------------------------------------
// Shared property-name list — matches Camera3DProperty enum order.
// Hoisted here to avoid the duplicate static-local anti-pattern.
// ---------------------------------------------------------------------------

namespace {

const QStringList &propNames()
{
    static const QStringList kNames = {
        QStringLiteral("PositionX"),
        QStringLiteral("PositionY"),
        QStringLiteral("PositionZ"),
        QStringLiteral("TargetX"),
        QStringLiteral("TargetY"),
        QStringLiteral("TargetZ"),
        QStringLiteral("Fov"),
        QStringLiteral("Roll")
    };
    return kNames;
}

// Interpolation display names, matching KeyframePoint::Interpolation enum order.
const QStringList &interpNames()
{
    static const QStringList kInterp = {
        QStringLiteral("Linear"),
        QStringLiteral("EaseIn"),
        QStringLiteral("EaseOut"),
        QStringLiteral("EaseInOut"),
        QStringLiteral("Hold"),
        QStringLiteral("Bezier"),
        QStringLiteral("エラスティック"),
        QStringLiteral("バウンス"),
        QStringLiteral("バック(オーバーシュート)")
    };
    return kInterp;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Preset names (Japanese as specified)
// ---------------------------------------------------------------------------
enum PresetIndex {
    PresetNone = 0,
    PresetDollyZoom,
    PresetPan,
    PresetOrbit,
    PresetHandheld,
    PresetEarthquake,
    PresetSubtleDrift
};

// ---------------------------------------------------------------------------
// CameraMotionDialog — constructor
// ---------------------------------------------------------------------------

CameraMotionDialog::CameraMotionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("カメラモーション設定"));
    buildUi();
    refreshFromCamera();   // initialise widgets from default Camera3D
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CameraMotionDialog::setCamera(const Camera3D &cam)
{
    m_camera = cam;
    refreshFromCamera();
}

Camera3D CameraMotionDialog::camera() const
{
    Camera3D result;
    result.setCamera(baseStateFromWidgets());
    result.setShake(shakeFromWidgets());

    // Replay keyframe table rows
    const int rows = m_kfTable->rowCount();
    for (int r = 0; r < rows; ++r) {
        QTableWidgetItem *timeItem  = m_kfTable->item(r, 0);
        QComboBox *propCombo        = qobject_cast<QComboBox *>(m_kfTable->cellWidget(r, 1));
        QTableWidgetItem *valItem   = m_kfTable->item(r, 2);
        QComboBox *interpCombo      = qobject_cast<QComboBox *>(m_kfTable->cellWidget(r, 3));
        if (!timeItem || !propCombo || !valItem)
            continue;

        double t   = timeItem->text().toDouble();
        double val = valItem->text().toDouble();
        int propIdx = propCombo->currentIndex();
        if (propIdx < 0 || propIdx >= static_cast<int>(Camera3DProperty::Count))
            continue;

        // Read interpolation; fall back to Linear if the widget is absent (defensive).
        KeyframePoint::Interpolation interp = KeyframePoint::Linear;
        if (interpCombo) {
            int iIdx = interpCombo->currentIndex();
            if (iIdx >= 0 && iIdx <= static_cast<int>(KeyframePoint::BackOut))
                interp = static_cast<KeyframePoint::Interpolation>(iIdx);
        }

        auto prop = static_cast<Camera3DProperty>(propIdx);
        KeyframeTrack *tr = result.track(prop);
        if (tr)
            tr->addKeyframe(t, val, interp);
    }

    return result;
}

void CameraMotionDialog::setSceneLayers(const QVector<CompositeLayer> &layers)
{
    m_sceneLayers = layers;
    refreshPreview();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void CameraMotionDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    // ---- Preset combo ----
    {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("プリセット:")));
        m_presetCombo = new QComboBox(this);
        m_presetCombo->addItem(QStringLiteral("なし"));
        m_presetCombo->addItem(QStringLiteral("ドリーズーム"));
        m_presetCombo->addItem(QStringLiteral("パン"));
        m_presetCombo->addItem(QStringLiteral("オービット"));
        m_presetCombo->addItem(QStringLiteral("手持ち"));
        m_presetCombo->addItem(QStringLiteral("地震"));
        m_presetCombo->addItem(QStringLiteral("微ドリフト"));
        row->addWidget(m_presetCombo);
        row->addStretch();
        mainLayout->addLayout(row);
    }

    // ---- Base camera group ----
    m_baseCameraGroup = new QGroupBox(QStringLiteral("基本カメラ"), this);
    {
        auto *fl = new QFormLayout(m_baseCameraGroup);

        auto makeSpinBox = [this](double min, double max, double step, double def) {
            auto *sb = new QDoubleSpinBox(this);
            sb->setRange(min, max);
            sb->setSingleStep(step);
            sb->setDecimals(3);
            sb->setValue(def);
            return sb;
        };

        m_posX = makeSpinBox(-9999.0, 9999.0, 0.1, 0.0);
        m_posY = makeSpinBox(-9999.0, 9999.0, 0.1, 0.0);
        m_posZ = makeSpinBox(-9999.0, 9999.0, 0.1, 0.0);
        m_tgtX = makeSpinBox(-9999.0, 9999.0, 0.1, 0.0);
        m_tgtY = makeSpinBox(-9999.0, 9999.0, 0.1, 0.0);
        m_tgtZ = makeSpinBox(-9999.0, 9999.0, 0.1, -1.0);
        m_fov  = makeSpinBox(10.0, 170.0, 1.0, 60.0);
        m_roll = makeSpinBox(-360.0, 360.0, 1.0, 0.0);

        fl->addRow(QStringLiteral("位置 X"), m_posX);
        fl->addRow(QStringLiteral("位置 Y"), m_posY);
        fl->addRow(QStringLiteral("位置 Z"), m_posZ);
        fl->addRow(QStringLiteral("ターゲット X"), m_tgtX);
        fl->addRow(QStringLiteral("ターゲット Y"), m_tgtY);
        fl->addRow(QStringLiteral("ターゲット Z"), m_tgtZ);
        fl->addRow(QStringLiteral("FOV (度)"), m_fov);
        fl->addRow(QStringLiteral("ロール (度)"), m_roll);
    }
    mainLayout->addWidget(m_baseCameraGroup);

    // ---- Keyframes group ----
    m_kfGroup = new QGroupBox(QStringLiteral("キーフレーム"), this);
    {
        auto *vl = new QVBoxLayout(m_kfGroup);

        m_kfTable = new QTableWidget(0, 4, this);
        m_kfTable->setHorizontalHeaderLabels({
            QStringLiteral("時刻(s)"),
            QStringLiteral("プロパティ"),
            QStringLiteral("値"),
            QStringLiteral("補間")
        });
        m_kfTable->horizontalHeader()->setStretchLastSection(true);
        m_kfTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_kfTable->setMinimumHeight(120);
        vl->addWidget(m_kfTable);

        auto *btnRow = new QHBoxLayout;
        m_addKfBtn    = new QPushButton(QStringLiteral("追加"), this);
        m_removeKfBtn = new QPushButton(QStringLiteral("削除"), this);
        btnRow->addWidget(m_addKfBtn);
        btnRow->addWidget(m_removeKfBtn);
        btnRow->addStretch();
        vl->addLayout(btnRow);
    }
    mainLayout->addWidget(m_kfGroup);

    // ---- Shake group ----
    m_shakeGroup = new QGroupBox(QStringLiteral("シェイク"), this);
    {
        auto *fl = new QFormLayout(m_shakeGroup);

        m_shakeEnabled = new QCheckBox(QStringLiteral("有効"), this);
        fl->addRow(m_shakeEnabled);

        auto makeD = [this](double min, double max, double step, double def) {
            auto *sb = new QDoubleSpinBox(this);
            sb->setRange(min, max);
            sb->setSingleStep(step);
            sb->setDecimals(3);
            sb->setValue(def);
            return sb;
        };

        m_shakeFreq   = makeD(0.01, 100.0,  0.1, 4.0);
        m_shakePosX   = makeD(0.0,  1000.0, 0.1, 0.0);
        m_shakePosY   = makeD(0.0,  1000.0, 0.1, 0.0);
        m_shakePosZ   = makeD(0.0,  1000.0, 0.1, 0.0);
        m_shakeRotDeg = makeD(0.0,  360.0,  0.1, 0.0);
        m_shakeSmooth = makeD(0.01, 10.0,   0.1, 1.0);

        m_shakeSeed = new QSpinBox(this);
        m_shakeSeed->setRange(0, 2000000000);
        m_shakeSeed->setValue(1);

        fl->addRow(QStringLiteral("周波数"),          m_shakeFreq);
        fl->addRow(QStringLiteral("位置振幅 X"),      m_shakePosX);
        fl->addRow(QStringLiteral("位置振幅 Y"),      m_shakePosY);
        fl->addRow(QStringLiteral("位置振幅 Z"),      m_shakePosZ);
        fl->addRow(QStringLiteral("回転振幅 (度)"),   m_shakeRotDeg);
        fl->addRow(QStringLiteral("シード"),          m_shakeSeed);
        fl->addRow(QStringLiteral("スムーズネス"),    m_shakeSmooth);
    }
    mainLayout->addWidget(m_shakeGroup);

    // ---- Preview area ----
    {
        auto *previewGroup = new QGroupBox(QStringLiteral("プレビュー"), this);
        auto *pvl = new QVBoxLayout(previewGroup);

        m_previewLabel = new QLabel(this);
        m_previewLabel->setFixedSize(kPreviewW, kPreviewH);
        m_previewLabel->setStyleSheet(QStringLiteral("background: #111;"));
        m_previewLabel->setAlignment(Qt::AlignCenter);
        pvl->addWidget(m_previewLabel, 0, Qt::AlignHCenter);

        auto *sliderRow = new QHBoxLayout;
        sliderRow->addWidget(new QLabel(QStringLiteral("プレビュー時刻:")));
        m_previewSlider = new QSlider(Qt::Horizontal, this);
        m_previewSlider->setRange(0, kSliderMax);
        m_previewSlider->setValue(0);
        sliderRow->addWidget(m_previewSlider);
        pvl->addLayout(sliderRow);

        mainLayout->addWidget(previewGroup);
    }

    // ---- Dialog buttons ----
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(m_buttonBox);

    // ---- Connections ----

    // Base camera spinboxes
    auto connectDSB = [this](QDoubleSpinBox *sb) {
        connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &CameraMotionDialog::onBaseCameraEdited);
    };
    connectDSB(m_posX); connectDSB(m_posY); connectDSB(m_posZ);
    connectDSB(m_tgtX); connectDSB(m_tgtY); connectDSB(m_tgtZ);
    connectDSB(m_fov);  connectDSB(m_roll);

    // Shake widgets
    connect(m_shakeEnabled, &QCheckBox::toggled,
            this, &CameraMotionDialog::onShakeEdited);
    auto connectShakeDSB = [this](QDoubleSpinBox *sb) {
        connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &CameraMotionDialog::onShakeEdited);
    };
    connectShakeDSB(m_shakeFreq);
    connectShakeDSB(m_shakePosX);
    connectShakeDSB(m_shakePosY);
    connectShakeDSB(m_shakePosZ);
    connectShakeDSB(m_shakeRotDeg);
    connectShakeDSB(m_shakeSmooth);
    connect(m_shakeSeed, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &CameraMotionDialog::onShakeEdited);

    // Preset
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraMotionDialog::onPresetChanged);

    // Keyframe buttons
    connect(m_addKfBtn,    &QPushButton::clicked, this, &CameraMotionDialog::onAddKeyframe);
    connect(m_removeKfBtn, &QPushButton::clicked, this, &CameraMotionDialog::onRemoveKeyframe);

    // Preview slider
    connect(m_previewSlider, &QSlider::valueChanged,
            this, &CameraMotionDialog::onPreviewTimeChanged);

    // Dialog buttons
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ---------------------------------------------------------------------------
// Widget <-> Camera3D synchronisation
// ---------------------------------------------------------------------------

void CameraMotionDialog::refreshFromCamera()
{
    blockAllSignals(true);

    // Base state
    applyStateToWidgets(m_camera.camera());

    // Shake
    applyShakeToWidgets(m_camera.shake());

    // Keyframe table: iterate every property track and collect keyframe rows
    m_kfTable->setRowCount(0);

    const int propCount = static_cast<int>(Camera3DProperty::Count);
    for (int pi = 0; pi < propCount; ++pi) {
        auto prop = static_cast<Camera3DProperty>(pi);
        const KeyframeTrack *tr = m_camera.track(prop);
        if (!tr || !tr->hasKeyframes())
            continue;
        const auto &kfs = tr->keyframes();
        for (const KeyframePoint &kf : kfs) {
            int row = m_kfTable->rowCount();
            m_kfTable->insertRow(row);

            m_kfTable->setItem(row, 0,
                new QTableWidgetItem(QString::number(kf.time, 'f', 3)));

            auto *propCombo = new QComboBox();
            for (const QString &n : propNames())
                propCombo->addItem(n);
            propCombo->setCurrentIndex(pi);
            m_kfTable->setCellWidget(row, 1, propCombo);

            m_kfTable->setItem(row, 2,
                new QTableWidgetItem(QString::number(kf.value, 'f', 4)));

            auto *interpCombo = new QComboBox();
            for (const QString &n : interpNames())
                interpCombo->addItem(n);
            interpCombo->setCurrentIndex(static_cast<int>(kf.interpolation));
            connect(interpCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &CameraMotionDialog::onBaseCameraEdited);
            m_kfTable->setCellWidget(row, 3, interpCombo);
        }
    }

    blockAllSignals(false);

    refreshPreview();
}

void CameraMotionDialog::applyStateToWidgets(const Camera3DState &state)
{
    m_posX->setValue(static_cast<double>(state.position.x()));
    m_posY->setValue(static_cast<double>(state.position.y()));
    m_posZ->setValue(static_cast<double>(state.position.z()));
    m_tgtX->setValue(static_cast<double>(state.target.x()));
    m_tgtY->setValue(static_cast<double>(state.target.y()));
    m_tgtZ->setValue(static_cast<double>(state.target.z()));
    m_fov->setValue(state.fov);
    m_roll->setValue(state.roll);
}

void CameraMotionDialog::applyShakeToWidgets(const CameraShake &shake)
{
    m_shakeEnabled->setChecked(shake.enabled);
    m_shakeFreq->setValue(shake.frequency);
    m_shakePosX->setValue(static_cast<double>(shake.positionAmplitude.x()));
    m_shakePosY->setValue(static_cast<double>(shake.positionAmplitude.y()));
    m_shakePosZ->setValue(static_cast<double>(shake.positionAmplitude.z()));
    m_shakeRotDeg->setValue(shake.rotationAmplitudeDeg);
    m_shakeSeed->setValue(static_cast<int>(shake.seed));
    m_shakeSmooth->setValue(shake.smoothness);
}

Camera3DState CameraMotionDialog::baseStateFromWidgets() const
{
    Camera3DState s;
    s.position = QVector3D(
        static_cast<float>(m_posX->value()),
        static_cast<float>(m_posY->value()),
        static_cast<float>(m_posZ->value()));
    s.target = QVector3D(
        static_cast<float>(m_tgtX->value()),
        static_cast<float>(m_tgtY->value()),
        static_cast<float>(m_tgtZ->value()));
    s.fov  = m_fov->value();
    s.roll = m_roll->value();
    return s;
}

CameraShake CameraMotionDialog::shakeFromWidgets() const
{
    CameraShake sh;
    sh.enabled    = m_shakeEnabled->isChecked();
    sh.frequency  = m_shakeFreq->value();
    sh.positionAmplitude = QVector3D(
        static_cast<float>(m_shakePosX->value()),
        static_cast<float>(m_shakePosY->value()),
        static_cast<float>(m_shakePosZ->value()));
    sh.rotationAmplitudeDeg = m_shakeRotDeg->value();
    sh.seed       = static_cast<unsigned int>(m_shakeSeed->value());
    sh.smoothness = m_shakeSmooth->value();
    return sh;
}

void CameraMotionDialog::applyWidgetsToCamera()
{
    // Rebuild m_camera from all widgets (base + shake + keyframes table).
    // We preserve the existing camera's keyframes by rebuilding via camera().
    m_camera = camera();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void CameraMotionDialog::onBaseCameraEdited()
{
    applyWidgetsToCamera();
    refreshPreview();
    emit cameraChanged();
}

void CameraMotionDialog::onShakeEdited()
{
    applyWidgetsToCamera();
    refreshPreview();
    emit cameraChanged();
}

void CameraMotionDialog::onPresetChanged(int index)
{
    Camera3DState base = baseStateFromWidgets();

    switch (index) {
    case PresetDollyZoom:
        m_camera = Camera3D::createDollyZoom(
            static_cast<double>(base.position.z()),
            static_cast<double>(base.position.z()) + 5.0,
            3.0);
        m_camera.setCamera(base);   // preserve user position/target
        break;

    case PresetPan:
        m_camera = Camera3D::createPanShot(
            static_cast<double>(base.position.x()),
            static_cast<double>(base.position.x()) + 5.0,
            3.0);
        m_camera.setCamera(base);
        break;

    case PresetOrbit:
        m_camera = Camera3D::createOrbitShot(base.target, 5.0, 4.0);
        m_camera.setCamera(base);
        break;

    case PresetHandheld:
        m_camera = Camera3D::createHandheld(base, 1.0);
        break;

    case PresetEarthquake:
        m_camera = Camera3D::createEarthquake(base, 1.0);
        break;

    case PresetSubtleDrift:
        m_camera = Camera3D::createSubtleDrift(base, 1.0);
        break;

    case PresetNone:
    default: {
        // Keep base state, clear shake
        Camera3D cleared;
        cleared.setCamera(base);
        CameraShake noShake;
        noShake.enabled = false;
        cleared.setShake(noShake);
        m_camera = cleared;
        break;
    }
    }

    blockAllSignals(true);
    applyStateToWidgets(m_camera.camera());
    applyShakeToWidgets(m_camera.shake());

    // Rebuild keyframe table from new camera
    m_kfTable->setRowCount(0);
    const int propCount = static_cast<int>(Camera3DProperty::Count);
    for (int pi = 0; pi < propCount; ++pi) {
        auto prop = static_cast<Camera3DProperty>(pi);
        const KeyframeTrack *tr = m_camera.track(prop);
        if (!tr || !tr->hasKeyframes())
            continue;
        for (const KeyframePoint &kf : tr->keyframes()) {
            int row = m_kfTable->rowCount();
            m_kfTable->insertRow(row);
            m_kfTable->setItem(row, 0,
                new QTableWidgetItem(QString::number(kf.time, 'f', 3)));
            auto *propCombo = new QComboBox();
            for (const QString &n : propNames())
                propCombo->addItem(n);
            propCombo->setCurrentIndex(pi);
            m_kfTable->setCellWidget(row, 1, propCombo);
            m_kfTable->setItem(row, 2,
                new QTableWidgetItem(QString::number(kf.value, 'f', 4)));
            auto *interpCombo = new QComboBox();
            for (const QString &n : interpNames())
                interpCombo->addItem(n);
            interpCombo->setCurrentIndex(static_cast<int>(kf.interpolation));
            connect(interpCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &CameraMotionDialog::onBaseCameraEdited);
            m_kfTable->setCellWidget(row, 3, interpCombo);
        }
    }

    blockAllSignals(false);

    refreshPreview();
    emit cameraChanged();
}

void CameraMotionDialog::onAddKeyframe()
{
    int row = m_kfTable->rowCount();
    m_kfTable->insertRow(row);

    // Default: current preview time, PositionX, 0.0, Linear
    m_kfTable->setItem(row, 0,
        new QTableWidgetItem(QString::number(previewTimeSec(), 'f', 3)));

    auto *propCombo = new QComboBox();
    for (const QString &n : propNames())
        propCombo->addItem(n);
    propCombo->setCurrentIndex(0);
    m_kfTable->setCellWidget(row, 1, propCombo);

    m_kfTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("0.000")));

    auto *interpCombo = new QComboBox();
    for (const QString &n : interpNames())
        interpCombo->addItem(n);
    interpCombo->setCurrentIndex(static_cast<int>(KeyframePoint::Linear));
    connect(interpCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraMotionDialog::onBaseCameraEdited);
    m_kfTable->setCellWidget(row, 3, interpCombo);

    applyWidgetsToCamera();
    refreshPreview();
    emit cameraChanged();
}

void CameraMotionDialog::onRemoveKeyframe()
{
    const QList<QTableWidgetItem *> sel = m_kfTable->selectedItems();
    if (sel.isEmpty())
        return;

    // Collect unique rows in descending order to remove cleanly
    QSet<int> rowSet;
    for (QTableWidgetItem *item : sel)
        rowSet.insert(item->row());
    QVector<int> rows(rowSet.begin(), rowSet.end());
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int r : rows)
        m_kfTable->removeRow(r);

    applyWidgetsToCamera();
    refreshPreview();
    emit cameraChanged();
}

void CameraMotionDialog::onPreviewTimeChanged(int /*sliderValue*/)
{
    refreshPreview();
    // Pose change due to slider does not alter the camera definition,
    // so we do not emit cameraChanged().
}

// ---------------------------------------------------------------------------
// Preview rendering
// ---------------------------------------------------------------------------

void CameraMotionDialog::refreshPreview()
{
    const QSize previewSize(kPreviewW, kPreviewH);
    QImage img(previewSize, QImage::Format_RGB32);
    img.fill(Qt::black);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing);

    // Build a temporary camera at the current preview time
    Camera3D tmp;
    const double t = previewTimeSec();

    // Get the camera pose at time t. When there are no keyframes the tracks
    // return their defaults (0,0,0 / 0,0,-1 / fov=60), not the base state set
    // by setCamera — so we fall back to camera() in that case.
    // getCameraAt() returns the keyframed base state with the procedural shake
    // (if enabled) already applied at time t.
    Camera3DState stateAtT = m_camera.hasAnimation()
        ? m_camera.getCameraAt(t)
        : m_camera.camera();

    tmp.setCamera(stateAtT);

    // Draw wireframe grid (XZ plane, y=0, -5..5)
    static const QVector<QVector3D> gridPts = buildGridPoints();

    painter.setPen(QPen(QColor(0, 180, 80), 1));
    for (const QVector3D &pt : gridPts) {
        QPointF p = tmp.projectTo2D(pt, previewSize);
        // Draw a small cross at each grid node
        painter.drawEllipse(p, 2.0, 2.0);
    }

    // Draw axis lines along the grid edges
    painter.setPen(QPen(QColor(80, 80, 80), 1));
    for (int x = -5; x <= 5; ++x) {
        QPointF a = tmp.projectTo2D(QVector3D(static_cast<float>(x), 0.0f, -5.0f), previewSize);
        QPointF b = tmp.projectTo2D(QVector3D(static_cast<float>(x), 0.0f,  5.0f), previewSize);
        painter.drawLine(a, b);
    }
    for (int z = -5; z <= 5; ++z) {
        QPointF a = tmp.projectTo2D(QVector3D(-5.0f, 0.0f, static_cast<float>(z)), previewSize);
        QPointF b = tmp.projectTo2D(QVector3D( 5.0f, 0.0f, static_cast<float>(z)), previewSize);
        painter.drawLine(a, b);
    }

    // Origin marker
    painter.setPen(QPen(Qt::white, 2));
    QPointF origin = tmp.projectTo2D(QVector3D(0.0f, 0.0f, 0.0f), previewSize);
    painter.drawEllipse(origin, 4.0, 4.0);

    // Time label
    painter.setPen(Qt::white);
    painter.drawText(4, 14,
        QStringLiteral("t = %1 s").arg(t, 0, 'f', 2));

    painter.end();

    m_previewLabel->setPixmap(QPixmap::fromImage(img));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

double CameraMotionDialog::previewTimeSec() const
{
    return (static_cast<double>(m_previewSlider->value()) / kSliderMax) * kPreviewMaxSec;
}

void CameraMotionDialog::blockAllSignals(bool block)
{
    m_posX->blockSignals(block);
    m_posY->blockSignals(block);
    m_posZ->blockSignals(block);
    m_tgtX->blockSignals(block);
    m_tgtY->blockSignals(block);
    m_tgtZ->blockSignals(block);
    m_fov->blockSignals(block);
    m_roll->blockSignals(block);

    m_shakeEnabled->blockSignals(block);
    m_shakeFreq->blockSignals(block);
    m_shakePosX->blockSignals(block);
    m_shakePosY->blockSignals(block);
    m_shakePosZ->blockSignals(block);
    m_shakeRotDeg->blockSignals(block);
    m_shakeSeed->blockSignals(block);
    m_shakeSmooth->blockSignals(block);

    m_presetCombo->blockSignals(block);
    m_previewSlider->blockSignals(block);
}
