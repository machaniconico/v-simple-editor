#include "SocialExportDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// コンストラクタ
// ---------------------------------------------------------------------------
SocialExportDialog::SocialExportDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("socialExportDialog"));
    setWindowTitle(tr("SNS 向けエクスポート"));
    resize(720, 540);

    // -----------------------------------------------------------------------
    // プリセット選択
    // -----------------------------------------------------------------------
    m_presetCombo = new QComboBox(this);
    const QList<social::Preset> presets = social::allPresets();
    for (const social::Preset& p : presets)
        m_presetCombo->addItem(p.displayName, p.id);

    m_presetInfoLabel = new QLabel(this);
    m_presetInfoLabel->setWordWrap(true);

    auto* presetForm = new QFormLayout;
    presetForm->addRow(tr("プラットフォーム:"), m_presetCombo);
    presetForm->addRow(tr("詳細:"),             m_presetInfoLabel);

    // -----------------------------------------------------------------------
    // リフレーミング
    // -----------------------------------------------------------------------
    m_reframeGroup = new QGroupBox(tr("リフレーミング (元動画 → ターゲット比率)"), this);

    m_modeCombo = new QComboBox(m_reframeGroup);
    for (const QString& modeName : reframe::availableModes())
        m_modeCombo->addItem(modeName);

    // 中心 X スライダー
    m_manualXSlider = new QSlider(Qt::Horizontal, m_reframeGroup);
    m_manualXSlider->setRange(0, 100);
    m_manualXSlider->setValue(50);
    m_manualXLabel = new QLabel(tr("X: 50%"), m_reframeGroup);

    // 中心 Y スライダー
    m_manualYSlider = new QSlider(Qt::Horizontal, m_reframeGroup);
    m_manualYSlider->setRange(0, 100);
    m_manualYSlider->setValue(50);
    m_manualYLabel = new QLabel(tr("Y: 50%"), m_reframeGroup);

    // ズーム スライダー
    m_zoomSlider = new QSlider(Qt::Horizontal, m_reframeGroup);
    m_zoomSlider->setRange(100, 400);
    m_zoomSlider->setValue(100);
    m_zoomLabel = new QLabel(tr("100%"), m_reframeGroup);

    auto* reframeForm = new QFormLayout(m_reframeGroup);
    reframeForm->addRow(tr("モード:"),   m_modeCombo);
    reframeForm->addRow(tr("中心 X:"),   m_manualXSlider);
    reframeForm->addRow(QString(),       m_manualXLabel);
    reframeForm->addRow(tr("中心 Y:"),   m_manualYSlider);
    reframeForm->addRow(QString(),       m_manualYLabel);
    reframeForm->addRow(tr("ズーム %:"), m_zoomSlider);
    reframeForm->addRow(QString(),       m_zoomLabel);

    // -----------------------------------------------------------------------
    // プレビュー (左: 元動画、右: 出力)
    // -----------------------------------------------------------------------
    auto* srcColumn = new QVBoxLayout;
    auto* srcTitle = new QLabel(tr("元動画"), this);
    srcTitle->setAlignment(Qt::AlignCenter);
    m_sourcePreviewLabel = new QLabel(tr("プレビューなし"), this);
    m_sourcePreviewLabel->setFixedSize(320, 180);
    m_sourcePreviewLabel->setAlignment(Qt::AlignCenter);
    {
        QPalette pp = m_sourcePreviewLabel->palette();
        pp.setColor(QPalette::Window, Qt::black);
        pp.setColor(QPalette::WindowText, Qt::white);
        m_sourcePreviewLabel->setPalette(pp);
        m_sourcePreviewLabel->setAutoFillBackground(true);
    }
    srcColumn->addWidget(srcTitle);
    srcColumn->addWidget(m_sourcePreviewLabel);

    auto* outColumn = new QVBoxLayout;
    auto* outTitle = new QLabel(tr("出力"), this);
    outTitle->setAlignment(Qt::AlignCenter);
    m_previewLabel = new QLabel(tr("プレビューなし"), this);
    m_previewLabel->setFixedSize(200, 356);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    {
        QPalette pp = m_previewLabel->palette();
        pp.setColor(QPalette::Window, Qt::black);
        pp.setColor(QPalette::WindowText, Qt::white);
        m_previewLabel->setPalette(pp);
        m_previewLabel->setAutoFillBackground(true);
    }
    outColumn->addWidget(outTitle);
    outColumn->addWidget(m_previewLabel);

    auto* previewRow = new QHBoxLayout;
    previewRow->addLayout(srcColumn);
    previewRow->addStretch();
    previewRow->addLayout(outColumn);

    // -----------------------------------------------------------------------
    // ボタン
    // -----------------------------------------------------------------------
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttonBox->button(QDialogButtonBox::Ok)->setText(tr("プロジェクトに適用"));

    // -----------------------------------------------------------------------
    // メインレイアウト
    // -----------------------------------------------------------------------
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(presetForm);
    mainLayout->addWidget(m_reframeGroup);
    mainLayout->addLayout(previewRow);
    mainLayout->addWidget(m_buttonBox);

    // -----------------------------------------------------------------------
    // シグナル接続
    // -----------------------------------------------------------------------
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SocialExportDialog::onPresetChanged);

    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SocialExportDialog::onReframeModeChanged);

    connect(m_manualXSlider, &QSlider::valueChanged,
            this, &SocialExportDialog::onManualOffsetXChanged);
    connect(m_manualYSlider, &QSlider::valueChanged,
            this, &SocialExportDialog::onManualOffsetYChanged);
    connect(m_zoomSlider, &QSlider::valueChanged,
            this, &SocialExportDialog::onZoomChanged);

    connect(m_buttonBox->button(QDialogButtonBox::Ok), &QPushButton::clicked,
            this, &SocialExportDialog::onExportClicked);
    connect(m_buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    // -----------------------------------------------------------------------
    // 初期状態
    // -----------------------------------------------------------------------
    onPresetChanged(0);
    onReframeModeChanged(0);
}

// ---------------------------------------------------------------------------
// setSampleFrame
// ---------------------------------------------------------------------------
void SocialExportDialog::setSampleFrame(const QImage& sample)
{
    m_sample = sample;
    if (!sample.isNull()) {
        const QImage thumb = sample.scaled(
            m_sourcePreviewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_sourcePreviewLabel->setPixmap(QPixmap::fromImage(thumb));
        m_sourcePreviewLabel->setText(QString());
    }
    updatePreview();
}

// ---------------------------------------------------------------------------
// selectedPreset
// ---------------------------------------------------------------------------
social::Preset SocialExportDialog::selectedPreset() const
{
    const QString id = m_presetCombo->currentData().toString();
    return social::presetById(id);
}

// ---------------------------------------------------------------------------
// reframeParams
// ---------------------------------------------------------------------------
reframe::ReframeParams SocialExportDialog::reframeParams() const
{
    reframe::ReframeParams p;
    p.sourceSize             = m_sample.size();
    p.targetSize             = selectedPreset().resolution;
    p.mode                   = reframe::modeFromString(m_modeCombo->currentText());
    p.manualOffsetNormalized = QPointF(m_manualXSlider->value() / 100.0,
                                       m_manualYSlider->value() / 100.0);
    p.zoom                   = m_zoomSlider->value() / 100.0;
    return p;
}

// ---------------------------------------------------------------------------
// onPresetChanged
// ---------------------------------------------------------------------------
void SocialExportDialog::onPresetChanged(int index)
{
    const QString id = m_presetCombo->itemData(index).toString();
    const social::Preset preset = social::presetById(id);

    const double vBitrateMbps = preset.videoBitrateBps / 1'000'000.0;
    m_presetInfoLabel->setText(
        QString(tr("解像度: %1x%2 | %3fps | %4Mbps | %5/%6 | 最大 %7 秒"))
            .arg(preset.resolution.width())
            .arg(preset.resolution.height())
            .arg(preset.targetFps)
            .arg(vBitrateMbps, 0, 'f', 1)
            .arg(preset.containerFormat)
            .arg(preset.videoCodec)
            .arg(preset.maxDurationSec));

    m_reframeGroup->setVisible(preset.requiresVerticalReframe);

    updatePreview();
}

// ---------------------------------------------------------------------------
// onReframeModeChanged
// ---------------------------------------------------------------------------
void SocialExportDialog::onReframeModeChanged(int /*index*/)
{
    updateReframeControlsEnabled();
    updatePreview();
}

// ---------------------------------------------------------------------------
// onManualOffsetXChanged
// ---------------------------------------------------------------------------
void SocialExportDialog::onManualOffsetXChanged(int value)
{
    m_manualXLabel->setText(QString(tr("X: %1%")).arg(value));
    updatePreview();
}

// ---------------------------------------------------------------------------
// onManualOffsetYChanged
// ---------------------------------------------------------------------------
void SocialExportDialog::onManualOffsetYChanged(int value)
{
    m_manualYLabel->setText(QString(tr("Y: %1%")).arg(value));
    updatePreview();
}

// ---------------------------------------------------------------------------
// onZoomChanged
// ---------------------------------------------------------------------------
void SocialExportDialog::onZoomChanged(int value)
{
    m_zoomLabel->setText(QString(tr("%1%")).arg(value));
    updatePreview();
}

// ---------------------------------------------------------------------------
// onExportClicked
// ---------------------------------------------------------------------------
void SocialExportDialog::onExportClicked()
{
    emit exportRequested(selectedPreset(), reframeParams());
    accept();
}

// ---------------------------------------------------------------------------
// updatePreview
// ---------------------------------------------------------------------------
void SocialExportDialog::updatePreview()
{
    if (m_sample.isNull()) {
        m_previewLabel->setText(tr("プレビューなし"));
        m_previewLabel->setPixmap(QPixmap());
        return;
    }

    // 元動画サムネ
    {
        const QImage thumb = m_sample.scaled(
            m_sourcePreviewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_sourcePreviewLabel->setPixmap(QPixmap::fromImage(thumb));
        m_sourcePreviewLabel->setText(QString());
    }

    // 出力プレビュー
    const reframe::ReframeResult result = reframe::applyReframe(m_sample, reframeParams());

    if (!result.success) {
        m_previewLabel->setPixmap(QPixmap());
        m_previewLabel->setText(tr("プレビュー生成失敗: %1").arg(result.error));
        return;
    }

    const QPixmap px = QPixmap::fromImage(result.previewImage)
                           .scaled(m_previewLabel->size(),
                                   Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
    m_previewLabel->setPixmap(px);
    m_previewLabel->setText(QString());
}

// ---------------------------------------------------------------------------
// updateReframeControlsEnabled
// ---------------------------------------------------------------------------
void SocialExportDialog::updateReframeControlsEnabled()
{
    const reframe::Mode mode = reframe::modeFromString(m_modeCombo->currentText());
    const bool isManual = (mode == reframe::Mode::Manual);

    m_manualXSlider->setEnabled(isManual);
    m_manualYSlider->setEnabled(isManual);
    m_zoomSlider->setEnabled(isManual);
    m_manualXLabel->setEnabled(isManual);
    m_manualYLabel->setEnabled(isManual);
    m_zoomLabel->setEnabled(isManual);
}
