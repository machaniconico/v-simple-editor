#include "AIMaskDialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// コンストラクタ
// ---------------------------------------------------------------------------
AIMaskDialog::AIMaskDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("aiMaskDialog"));
    setWindowTitle(tr("AI マスク生成"));

    // --- エンジン選択 ---
    m_engineCombo = new QComboBox(this);
    for (const QString& name : aimask::availableEngines())
        m_engineCombo->addItem(name);

    // --- LumaThreshold パラメータ ---
    m_lumaSpin = new QDoubleSpinBox(this);
    m_lumaSpin->setRange(0.0, 1.0);
    m_lumaSpin->setSingleStep(0.05);
    m_lumaSpin->setDecimals(2);
    m_lumaSpin->setValue(0.5);

    // --- ColorRange パラメータ ---
    m_colorButton = new QPushButton(tr("色を選択..."), this);
    // ボタン背景でターゲット色を示す
    QPalette pal = m_colorButton->palette();
    pal.setColor(QPalette::Button, m_colorTarget);
    m_colorButton->setPalette(pal);
    m_colorButton->setAutoFillBackground(true);

    m_toleranceSpin = new QDoubleSpinBox(this);
    m_toleranceSpin->setRange(0.0, 1.0);
    m_toleranceSpin->setSingleStep(0.05);
    m_toleranceSpin->setDecimals(2);
    m_toleranceSpin->setValue(0.15);

    // --- ExternalPlugin パラメータ ---
    m_pluginIdEdit = new QLineEdit(this);
    m_pluginIdEdit->setPlaceholderText(tr("Plugin ID"));
    m_pluginIdEdit->setEnabled(false);

    // --- プレビュー領域 ---
    m_preview = new QLabel(tr("プレビューなし"), this);
    m_preview->setFixedSize(320, 180);
    m_preview->setAlignment(Qt::AlignCenter);
    {
        QPalette pp = m_preview->palette();
        pp.setColor(QPalette::Window, Qt::black);
        pp.setColor(QPalette::WindowText, Qt::white);
        m_preview->setPalette(pp);
        m_preview->setAutoFillBackground(true);
    }

    // --- 生成ボタン ---
    auto* generateBtn = new QPushButton(tr("生成"), this);

    // --- OK / Cancel ---
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    // --- フォームレイアウト ---
    auto* form = new QFormLayout;
    form->addRow(tr("エンジン:"),      m_engineCombo);
    form->addRow(tr("輝度閾値:"),      m_lumaSpin);
    form->addRow(tr("ターゲット色:"),  m_colorButton);
    form->addRow(tr("色許容範囲:"),    m_toleranceSpin);
    form->addRow(tr("プラグイン ID:"), m_pluginIdEdit);

    // --- メインレイアウト ---
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(form);
    mainLayout->addWidget(m_preview, 0, Qt::AlignHCenter);
    mainLayout->addWidget(generateBtn);
    mainLayout->addWidget(buttons);

    // --- シグナル接続 ---
    connect(m_engineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateWidgetStates(); });

    connect(m_colorButton, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_colorTarget, this, tr("ターゲット色を選択"));
        if (c.isValid()) {
            m_colorTarget = c;
            QPalette pal = m_colorButton->palette();
            pal.setColor(QPalette::Button, m_colorTarget);
            m_colorButton->setPalette(pal);
        }
    });

    connect(generateBtn, &QPushButton::clicked,
            this, &AIMaskDialog::onGenerateClicked);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateWidgetStates();
}

// ---------------------------------------------------------------------------
// エンジンに応じて widget の enabled/disabled を切替
// ---------------------------------------------------------------------------
void AIMaskDialog::updateWidgetStates()
{
    const aimask::Engine eng = aimask::engineFromString(m_engineCombo->currentText());

    m_lumaSpin->setEnabled(eng == aimask::Engine::LumaThreshold);
    m_colorButton->setEnabled(eng == aimask::Engine::ColorRange);
    m_toleranceSpin->setEnabled(eng == aimask::Engine::ColorRange);
    m_pluginIdEdit->setEnabled(eng == aimask::Engine::ExternalPlugin);
}

// ---------------------------------------------------------------------------
// setSourceImage — プレビューに元画像を表示
// ---------------------------------------------------------------------------
void AIMaskDialog::setSourceImage(const QImage& source)
{
    m_source = source;
    if (!source.isNull()) {
        const QImage thumb = source.scaled(320, 180, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
        m_preview->setPixmap(QPixmap::fromImage(thumb));
        m_preview->setText(QString());
    }
}

// ---------------------------------------------------------------------------
// params — 現在の UI 値を MaskParams に変換
// ---------------------------------------------------------------------------
aimask::MaskParams AIMaskDialog::params() const
{
    aimask::MaskParams p;
    p.engine         = aimask::engineFromString(m_engineCombo->currentText());
    p.lumaThreshold  = m_lumaSpin->value();
    p.colorTarget    = m_colorTarget;
    p.colorTolerance = m_toleranceSpin->value();
    p.pluginId       = m_pluginIdEdit->text();
    return p;
}

// ---------------------------------------------------------------------------
// maskImage — 最後に生成した mask を返す
// ---------------------------------------------------------------------------
QImage AIMaskDialog::maskImage() const
{
    return m_mask;
}

// ---------------------------------------------------------------------------
// onGenerateClicked — mask 生成してプレビューに表示
// ---------------------------------------------------------------------------
void AIMaskDialog::onGenerateClicked()
{
    const aimask::MaskResult result = aimask::generateMask(m_source, params());

    if (!result.success) {
        QMessageBox::warning(this, tr("マスク生成エラー"), result.error);
        return;
    }

    m_mask = result.mask;

    // Grayscale8 → ARGB32 に変換してからサムネ表示
    const QImage display = m_mask.convertToFormat(QImage::Format_ARGB32);
    const QImage thumb   = display.scaled(320, 180, Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
    m_preview->setPixmap(QPixmap::fromImage(thumb));
    m_preview->setText(QString());
}
