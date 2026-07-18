#include "Text3DExtrusionDialog.h"
#include "Camera3D.h"

#include <QByteArray>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVariant>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

namespace {

constexpr const char *kSourceCameraProperty = "vseText3DSourceCamera";

} // namespace

static void setButtonColor(QPushButton *btn, const QColor &c)
{
    btn->setStyleSheet(
        QStringLiteral("background-color: %1; border: 1px solid #888;").arg(c.name()));
}

// ---------------------------------------------------------------------------
// constructor
// ---------------------------------------------------------------------------

Text3DExtrusionDialog::Text3DExtrusionDialog(QWidget *parent)
    : QDialog(parent)
    , m_currentFont(QStringLiteral("Arial"), 72)
    , m_frontColor(220, 200, 120)
    , m_sideColor(180, 150, 90)
    , m_ambientColor(40, 40, 40)
{
    setWindowTitle(QStringLiteral("3Dテキスト押し出し設定"));
    setMinimumWidth(480);

    auto *rootLayout = new QVBoxLayout(this);

    // -----------------------------------------------------------------------
    // テキスト / フォント
    // -----------------------------------------------------------------------
    {
        auto *row = new QHBoxLayout;
        auto *lbl = new QLabel(QStringLiteral("テキスト:"), this);
        m_textEdit = new QLineEdit(this);
        m_textEdit->setPlaceholderText(QStringLiteral("テキストを入力…"));

        m_fontButton = new QPushButton(QStringLiteral("フォント…"), this);
        // Show current font family/size in button text
        m_fontButton->setText(
            QStringLiteral("フォント: %1 %2pt")
                .arg(m_currentFont.family())
                .arg(m_currentFont.pointSize()));

        row->addWidget(lbl);
        row->addWidget(m_textEdit, 1);
        row->addWidget(m_fontButton);
        rootLayout->addLayout(row);
    }

    // -----------------------------------------------------------------------
    // 押し出し設定
    // -----------------------------------------------------------------------
    m_extrudeBox = new QGroupBox(QStringLiteral("押し出し"), this);
    {
        auto *fl = new QFormLayout(m_extrudeBox);

        m_depthSpin = new QDoubleSpinBox(this);
        m_depthSpin->setRange(0.0, 5.0);
        m_depthSpin->setSingleStep(0.01);
        m_depthSpin->setDecimals(3);
        m_depthSpin->setValue(0.2);
        fl->addRow(QStringLiteral("深さ (depth):"), m_depthSpin);

        m_bevelDepthSpin = new QDoubleSpinBox(this);
        m_bevelDepthSpin->setRange(0.0, 1.0);
        m_bevelDepthSpin->setSingleStep(0.005);
        m_bevelDepthSpin->setDecimals(4);
        m_bevelDepthSpin->setValue(0.02);
        fl->addRow(QStringLiteral("ベベル深さ:"), m_bevelDepthSpin);

        m_bevelWidthSpin = new QDoubleSpinBox(this);
        m_bevelWidthSpin->setRange(0.0, 1.0);
        m_bevelWidthSpin->setSingleStep(0.005);
        m_bevelWidthSpin->setDecimals(4);
        m_bevelWidthSpin->setValue(0.02);
        fl->addRow(QStringLiteral("ベベル幅:"), m_bevelWidthSpin);

        m_bevelSegSpin = new QSpinBox(this);
        m_bevelSegSpin->setRange(0, 8);
        m_bevelSegSpin->setValue(2);
        fl->addRow(QStringLiteral("ベベルセグメント:"), m_bevelSegSpin);
    }
    rootLayout->addWidget(m_extrudeBox);

    // -----------------------------------------------------------------------
    // マテリアル
    // -----------------------------------------------------------------------
    m_materialBox = new QGroupBox(QStringLiteral("マテリアル"), this);
    {
        auto *fl = new QFormLayout(m_materialBox);

        // Front color
        m_frontColorBtn = new QPushButton(QStringLiteral("前面色"), this);
        setButtonColor(m_frontColorBtn, m_frontColor);
        fl->addRow(QStringLiteral("前面色:"), m_frontColorBtn);

        // Side color
        m_sideColorBtn = new QPushButton(QStringLiteral("側面色"), this);
        setButtonColor(m_sideColorBtn, m_sideColor);
        fl->addRow(QStringLiteral("側面色:"), m_sideColorBtn);

        // Ambient
        m_ambientBtn = new QPushButton(QStringLiteral("環境光色"), this);
        setButtonColor(m_ambientBtn, m_ambientColor);
        fl->addRow(QStringLiteral("環境光色:"), m_ambientBtn);

        // Light direction
        m_lightXSpin = new QDoubleSpinBox(this);
        m_lightXSpin->setRange(-5.0, 5.0);
        m_lightXSpin->setSingleStep(0.1);
        m_lightXSpin->setDecimals(3);
        m_lightXSpin->setValue(0.3);
        fl->addRow(QStringLiteral("ライト方向 X:"), m_lightXSpin);

        m_lightYSpin = new QDoubleSpinBox(this);
        m_lightYSpin->setRange(-5.0, 5.0);
        m_lightYSpin->setSingleStep(0.1);
        m_lightYSpin->setDecimals(3);
        m_lightYSpin->setValue(0.4);
        fl->addRow(QStringLiteral("ライト方向 Y:"), m_lightYSpin);

        m_lightZSpin = new QDoubleSpinBox(this);
        m_lightZSpin->setRange(-5.0, 5.0);
        m_lightZSpin->setSingleStep(0.1);
        m_lightZSpin->setDecimals(3);
        m_lightZSpin->setValue(-1.0);
        fl->addRow(QStringLiteral("ライト方向 Z:"), m_lightZSpin);
    }
    rootLayout->addWidget(m_materialBox);

    // -----------------------------------------------------------------------
    // 向き / スピン
    // -----------------------------------------------------------------------
    m_orientBox = new QGroupBox(QStringLiteral("向き / スピン"), this);
    {
        auto *fl = new QFormLayout(m_orientBox);

        m_yawSpin = new QDoubleSpinBox(this);
        m_yawSpin->setRange(-360.0, 360.0);
        m_yawSpin->setSingleStep(1.0);
        m_yawSpin->setDecimals(2);
        m_yawSpin->setValue(0.0);
        fl->addRow(QStringLiteral("基準ヨー (deg):"), m_yawSpin);

        m_pitchSpin = new QDoubleSpinBox(this);
        m_pitchSpin->setRange(-360.0, 360.0);
        m_pitchSpin->setSingleStep(1.0);
        m_pitchSpin->setDecimals(2);
        m_pitchSpin->setValue(0.0);
        fl->addRow(QStringLiteral("基準ピッチ (deg):"), m_pitchSpin);

        m_camDistSpin = new QDoubleSpinBox(this);
        m_camDistSpin->setRange(0.5, 50.0);
        m_camDistSpin->setSingleStep(0.1);
        m_camDistSpin->setDecimals(2);
        m_camDistSpin->setValue(3.0);
        fl->addRow(QStringLiteral("カメラ距離:"), m_camDistSpin);

        m_spinSpeedSpin = new QDoubleSpinBox(this);
        m_spinSpeedSpin->setRange(-2.0, 2.0);
        m_spinSpeedSpin->setSingleStep(0.05);
        m_spinSpeedSpin->setDecimals(3);
        m_spinSpeedSpin->setValue(0.0);
        fl->addRow(QStringLiteral("スピン速度 (rotAnimAxis.y):"), m_spinSpeedSpin);
    }
    rootLayout->addWidget(m_orientBox);

    // -----------------------------------------------------------------------
    // プレビュー
    // -----------------------------------------------------------------------
    {
        auto *previewBox = new QGroupBox(QStringLiteral("プレビュー"), this);
        auto *pvLayout   = new QVBoxLayout(previewBox);

        m_previewLabel = new QLabel(this);
        m_previewLabel->setFixedSize(320, 240);
        m_previewLabel->setAlignment(Qt::AlignCenter);
        m_previewLabel->setStyleSheet(QStringLiteral("background-color: #1a1a1a;"));

        auto *sliderRow = new QHBoxLayout;
        auto *sliderLbl = new QLabel(QStringLiteral("プレビュー時刻:"), this);
        m_timeSider = new QSlider(Qt::Horizontal, this);
        m_timeSider->setRange(0, 1000);
        m_timeSider->setValue(0);
        sliderRow->addWidget(sliderLbl);
        sliderRow->addWidget(m_timeSider, 1);

        pvLayout->addWidget(m_previewLabel, 0, Qt::AlignHCenter);
        pvLayout->addLayout(sliderRow);
        rootLayout->addWidget(previewBox);
    }

    // -----------------------------------------------------------------------
    // OK / Cancel
    // -----------------------------------------------------------------------
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    rootLayout->addWidget(m_buttonBox);

    // -----------------------------------------------------------------------
    // Connections
    // -----------------------------------------------------------------------
    connect(m_textEdit, &QLineEdit::textChanged,
            this, &Text3DExtrusionDialog::onTextChanged);
    connect(m_fontButton, &QPushButton::clicked,
            this, &Text3DExtrusionDialog::onFontButtonClicked);

    // Extrude spinboxes
    connect(m_depthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onExtrudeParamChanged);
    connect(m_bevelDepthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onExtrudeParamChanged);
    connect(m_bevelWidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onExtrudeParamChanged);
    connect(m_bevelSegSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { onExtrudeParamChanged(); });

    // Material color buttons
    connect(m_frontColorBtn, &QPushButton::clicked,
            this, &Text3DExtrusionDialog::onFrontColorClicked);
    connect(m_sideColorBtn, &QPushButton::clicked,
            this, &Text3DExtrusionDialog::onSideColorClicked);
    connect(m_ambientBtn, &QPushButton::clicked,
            this, &Text3DExtrusionDialog::onAmbientColorClicked);

    // Material light direction
    connect(m_lightXSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onExtrudeParamChanged);
    connect(m_lightYSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onExtrudeParamChanged);
    connect(m_lightZSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onExtrudeParamChanged);

    // Orientation / spin
    connect(m_yawSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onOrientationChanged);
    connect(m_pitchSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onOrientationChanged);
    connect(m_camDistSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onOrientationChanged);
    connect(m_spinSpeedSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &Text3DExtrusionDialog::onOrientationChanged);

    // Time slider
    connect(m_timeSider, &QSlider::valueChanged,
            this, &Text3DExtrusionDialog::onPreviewSliderChanged);

    // Dialog buttons
    connect(m_buttonBox, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    // Initial preview render
    updatePreview();
}

// ---------------------------------------------------------------------------
// setLayer
// ---------------------------------------------------------------------------

void Text3DExtrusionDialog::setLayer(const Text3DLayer &src)
{
    // Block signals during bulk load to avoid cascading updates
    m_textEdit->blockSignals(true);
    m_depthSpin->blockSignals(true);
    m_bevelDepthSpin->blockSignals(true);
    m_bevelWidthSpin->blockSignals(true);
    m_bevelSegSpin->blockSignals(true);
    m_lightXSpin->blockSignals(true);
    m_lightYSpin->blockSignals(true);
    m_lightZSpin->blockSignals(true);
    m_yawSpin->blockSignals(true);
    m_pitchSpin->blockSignals(true);
    m_camDistSpin->blockSignals(true);
    m_spinSpeedSpin->blockSignals(true);
    m_timeSider->blockSignals(true);

    // Serialise once — Text3DLayer exposes text/font/cameraDistance/rotAxis
    // only through JSON (no direct getters for those fields).
    const QJsonObject jo = src.toJson();
    setProperty(kSourceCameraProperty,
                QJsonDocument(src.camera().toJson()).toJson(QJsonDocument::Compact));

    // Text
    m_textEdit->setText(jo.value(QStringLiteral("text")).toString());

    // Font — keys: "fontFamily", "fontPointSize"
    {
        const QString family  = jo.value(QStringLiteral("fontFamily")).toString();
        const double  pointSz = jo.value(QStringLiteral("fontPointSize")).toDouble(72.0);
        if (!family.isEmpty())
            m_currentFont.setFamily(family);
        if (pointSz > 0.0)
            m_currentFont.setPointSizeF(pointSz);
        m_fontButton->setText(
            QStringLiteral("フォント: %1 %2pt")
                .arg(m_currentFont.family())
                .arg(m_currentFont.pointSize()));
    }

    // Extrude params
    m_depthSpin->setValue(src.extrudeDepth());
    m_bevelDepthSpin->setValue(src.extrudeBevelDepth());
    m_bevelWidthSpin->setValue(src.extrudeBevelWidth());
    m_bevelSegSpin->setValue(src.extrudeBevelSegments());

    // Material
    m_frontColor   = src.materialFrontColor();
    m_sideColor    = src.materialSideColor();
    m_ambientColor = src.materialAmbient();
    setButtonColor(m_frontColorBtn, m_frontColor);
    setButtonColor(m_sideColorBtn,  m_sideColor);
    setButtonColor(m_ambientBtn,    m_ambientColor);

    const QVector3D ld = src.materialLightDir();
    m_lightXSpin->setValue(static_cast<double>(ld.x()));
    m_lightYSpin->setValue(static_cast<double>(ld.y()));
    m_lightZSpin->setValue(static_cast<double>(ld.z()));

    // Orientation
    m_yawSpin->setValue(src.extrudeBaseYaw());
    m_pitchSpin->setValue(src.extrudeBasePitch());

    // Camera distance — from JSON ("cameraDistance" key)
    {
        const double cd = jo.value(QStringLiteral("cameraDistance")).toDouble(3.0);
        m_camDistSpin->setValue(cd > 0.0 ? cd : 3.0);
    }

    // Spin speed = rotationAnimAxis.y() — nested vector object in JSON
    {
        const QJsonObject rotAxis =
            jo.value(QStringLiteral("rotationAnimAxis")).toObject();
        m_spinSpeedSpin->setValue(
            rotAxis.value(QStringLiteral("y")).toDouble(0.0));
    }

    // Restore signals
    m_textEdit->blockSignals(false);
    m_depthSpin->blockSignals(false);
    m_bevelDepthSpin->blockSignals(false);
    m_bevelWidthSpin->blockSignals(false);
    m_bevelSegSpin->blockSignals(false);
    m_lightXSpin->blockSignals(false);
    m_lightYSpin->blockSignals(false);
    m_lightZSpin->blockSignals(false);
    m_yawSpin->blockSignals(false);
    m_pitchSpin->blockSignals(false);
    m_camDistSpin->blockSignals(false);
    m_spinSpeedSpin->blockSignals(false);
    m_timeSider->blockSignals(false);

    updatePreview();
}

// ---------------------------------------------------------------------------
// layer() — build and return a fully-configured Text3DLayer
// ---------------------------------------------------------------------------

void Text3DExtrusionDialog::buildLayer(Text3DLayer &out) const
{
    // Text + font
    out.setText(m_textEdit->text(), m_currentFont);

    // Enable extrude
    out.setExtrudeEnabled(true);

    // Extrude params
    out.setExtrude(
        m_depthSpin->value(),
        m_bevelDepthSpin->value(),
        m_bevelWidthSpin->value(),
        m_bevelSegSpin->value());

    // Material
    const QVector3D lightDir(
        static_cast<float>(m_lightXSpin->value()),
        static_cast<float>(m_lightYSpin->value()),
        static_cast<float>(m_lightZSpin->value()));
    out.setMaterial(m_frontColor, m_sideColor, m_ambientColor, lightDir);

    // Orientation
    out.setExtrudeYawPitch(m_yawSpin->value(), m_pitchSpin->value());

    // Camera distance
    out.setCameraDistance(m_camDistSpin->value());

    // Spin speed via rotationAnimAxis — only .y() is used by extrude path
    const QVector3D spinAxis(0.0f, static_cast<float>(m_spinSpeedSpin->value()), 0.0f);
    out.setRotationAnimAxis(spinAxis);

    const QByteArray cameraJson = property(kSourceCameraProperty).toByteArray();
    const QJsonDocument cameraDoc = QJsonDocument::fromJson(cameraJson);
    if (cameraDoc.isObject()) {
        Camera3D camera;
        camera.fromJson(cameraDoc.object());
        out.setCamera(camera);
    }
}

Text3DLayer *Text3DExtrusionDialog::layer(QObject *layerParent) const
{
    auto *result = new Text3DLayer(layerParent);
    buildLayer(*result);
    return result;
}

// ---------------------------------------------------------------------------
// updatePreview
// ---------------------------------------------------------------------------

void Text3DExtrusionDialog::updatePreview()
{
    // Map slider 0..1000 → 0..5 seconds
    const double timeVal = static_cast<double>(m_timeSider->value()) / 1000.0 * 5.0;

    // Use a temporary heap-allocated layer (QObject subclasses must not be
    // stack-allocated when their destructor may trigger child cleanup).
    Text3DLayer tmp(nullptr);
    buildLayer(tmp);

    Camera3D cam;  // default camera — all-default is fine for preview
    const QImage img = tmp.renderFrame(QSize(320, 240), timeVal, cam);

    if (!img.isNull()) {
        m_previewLabel->setPixmap(
            QPixmap::fromImage(img).scaled(
                m_previewLabel->size(),
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
    } else {
        m_previewLabel->clear();
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void Text3DExtrusionDialog::onTextChanged()
{
    updatePreview();
    emit layerChanged();
}

void Text3DExtrusionDialog::onFontButtonClicked()
{
    bool ok = false;
    const QFont chosen = QFontDialog::getFont(&ok, m_currentFont, this,
                                              QStringLiteral("フォントを選択"));
    if (ok) {
        m_currentFont = chosen;
        m_fontButton->setText(
            QStringLiteral("フォント: %1 %2pt")
                .arg(m_currentFont.family())
                .arg(m_currentFont.pointSize()));
        updatePreview();
        emit layerChanged();
    }
}

void Text3DExtrusionDialog::onExtrudeParamChanged()
{
    updatePreview();
    emit layerChanged();
}

void Text3DExtrusionDialog::onFrontColorClicked()
{
    const QColor chosen = QColorDialog::getColor(m_frontColor, this,
                                                 QStringLiteral("前面色を選択"));
    if (chosen.isValid()) {
        m_frontColor = chosen;
        setButtonColor(m_frontColorBtn, m_frontColor);
        updatePreview();
        emit layerChanged();
    }
}

void Text3DExtrusionDialog::onSideColorClicked()
{
    const QColor chosen = QColorDialog::getColor(m_sideColor, this,
                                                 QStringLiteral("側面色を選択"));
    if (chosen.isValid()) {
        m_sideColor = chosen;
        setButtonColor(m_sideColorBtn, m_sideColor);
        updatePreview();
        emit layerChanged();
    }
}

void Text3DExtrusionDialog::onAmbientColorClicked()
{
    const QColor chosen = QColorDialog::getColor(m_ambientColor, this,
                                                 QStringLiteral("環境光色を選択"));
    if (chosen.isValid()) {
        m_ambientColor = chosen;
        setButtonColor(m_ambientBtn, m_ambientColor);
        updatePreview();
        emit layerChanged();
    }
}

void Text3DExtrusionDialog::onOrientationChanged()
{
    updatePreview();
    emit layerChanged();
}

void Text3DExtrusionDialog::onPreviewSliderChanged()
{
    updatePreview();
    // Slider movement alone does not alter layer state; no layerChanged() here
}
