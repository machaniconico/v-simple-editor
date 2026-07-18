#include "ColorManagementDialog.h"

#include <algorithm>
#include <array>

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QVBoxLayout>

namespace {

// プレビュー用の代表色 (中間グレー)。sRGB 等の符号値 [0..1] を想定。
constexpr double kMidGray = 0.18;

// aces::ColorSpace を列挙する順序 (UI 表示順)。colorSpaceName で日本語ではなく
// 安定名を表示する (sRGB / Rec709 / ... )。
const std::array<aces::ColorSpace, 7> kColorSpaceOrder = {
    aces::ColorSpace::sRGB,
    aces::ColorSpace::Rec709,
    aces::ColorSpace::Rec2020,
    aces::ColorSpace::DisplayP3,
    aces::ColorSpace::ACEScg,
    aces::ColorSpace::ACES2065_1,
    aces::ColorSpace::LinearSRGB,
};

// [0..1] の符号値を 0..255 の 8bit にクランプ変換する。
int toByte(double v)
{
    const double clamped = std::clamp(v, 0.0, 1.0);
    return static_cast<int>(clamped * 255.0 + 0.5);
}

} // namespace

ColorManagementDialog::ColorManagementDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("カラーマネジメント (ACES)"));

    auto *rootLayout = new QVBoxLayout(this);

    // --- 有効化チェック ---
    m_enabledCheck = new QCheckBox(
        QStringLiteral("ACES カラーマネジメントを有効化"), this);
    rootLayout->addWidget(m_enabledCheck);

    // --- 色空間選択 ---
    auto *spaceGroup = new QGroupBox(QStringLiteral("色空間"), this);
    auto *formLayout = new QFormLayout(spaceGroup);

    m_inputCombo   = new QComboBox(spaceGroup);
    m_workingCombo = new QComboBox(spaceGroup);
    m_outputCombo  = new QComboBox(spaceGroup);

    populateColorSpaceCombo(m_inputCombo,   aces::ColorSpace::sRGB);
    populateColorSpaceCombo(m_workingCombo, aces::ColorSpace::ACEScg);
    populateColorSpaceCombo(m_outputCombo,  aces::ColorSpace::Rec709);

    formLayout->addRow(QStringLiteral("入力色空間"),   m_inputCombo);
    formLayout->addRow(QStringLiteral("作業色空間"),   m_workingCombo);
    formLayout->addRow(QStringLiteral("出力色空間"),   m_outputCombo);
    rootLayout->addWidget(spaceGroup);

    // --- プレビュー (中間グレーを process した結果) ---
    auto *previewGroup = new QGroupBox(
        QStringLiteral("プレビュー (中間グレー 18%)"), this);
    auto *previewLayout = new QHBoxLayout(previewGroup);

    m_previewSwatch = new QLabel(previewGroup);
    m_previewSwatch->setFixedSize(48, 48);
    m_previewSwatch->setFrameShape(QFrame::Box);
    m_previewSwatch->setAutoFillBackground(true);

    m_previewValue = new QLabel(previewGroup);
    m_previewValue->setTextInteractionFlags(Qt::TextSelectableByMouse);

    previewLayout->addWidget(m_previewSwatch);
    previewLayout->addWidget(m_previewValue, 1);
    rootLayout->addWidget(previewGroup);

    // --- OK / キャンセル ---
    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    rootLayout->addWidget(buttonBox);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 変更でプレビューを更新。
    connect(m_enabledCheck, &QCheckBox::toggled,
            this, &ColorManagementDialog::onSettingsChanged);
    connect(m_inputCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ColorManagementDialog::onSettingsChanged);
    connect(m_workingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ColorManagementDialog::onSettingsChanged);
    connect(m_outputCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ColorManagementDialog::onSettingsChanged);

    updatePreview();
}

void ColorManagementDialog::setPipeline(const aces::AcesPipeline &pipeline)
{
    m_enabledCheck->setChecked(pipeline.enabled);

    auto selectSpace = [](QComboBox *combo, aces::ColorSpace cs) {
        const int idx = combo->findData(static_cast<int>(cs));
        if (idx >= 0)
            combo->setCurrentIndex(idx);
    };
    selectSpace(m_inputCombo,   pipeline.input);
    selectSpace(m_workingCombo, pipeline.working);
    selectSpace(m_outputCombo,  pipeline.output);

    updatePreview();
}

aces::AcesPipeline ColorManagementDialog::pipeline() const
{
    aces::AcesPipeline p;
    p.enabled = m_enabledCheck->isChecked();
    p.input   = colorSpaceFromCombo(m_inputCombo);
    p.working = colorSpaceFromCombo(m_workingCombo);
    p.output  = colorSpaceFromCombo(m_outputCombo);
    return p;
}

void ColorManagementDialog::onSettingsChanged()
{
    updatePreview();
}

void ColorManagementDialog::populateColorSpaceCombo(QComboBox *combo,
                                                    aces::ColorSpace cs)
{
    combo->clear();
    for (aces::ColorSpace space : kColorSpaceOrder)
        combo->addItem(aces::colorSpaceName(space), static_cast<int>(space));
    const int idx = combo->findData(static_cast<int>(cs));
    if (idx >= 0)
        combo->setCurrentIndex(idx);
}

aces::ColorSpace ColorManagementDialog::colorSpaceFromCombo(const QComboBox *combo)
{
    bool ok = false;
    const int raw = combo->currentData().toInt(&ok);
    if (!ok)
        return aces::ColorSpace::sRGB;
    return static_cast<aces::ColorSpace>(raw);
}

void ColorManagementDialog::updatePreview()
{
    const aces::AcesPipeline p = pipeline();
    const aces::Vec3 inputColor = {kMidGray, kMidGray, kMidGray};
    const aces::Vec3 out = aces::process(p, inputColor);

    const QColor swatch(toByte(out[0]), toByte(out[1]), toByte(out[2]));

    QPalette pal = m_previewSwatch->palette();
    pal.setColor(QPalette::Window, swatch);
    m_previewSwatch->setPalette(pal);

    m_previewValue->setText(
        QStringLiteral("入力 (中間グレー) → 出力 RGB = (%1, %2, %3)")
            .arg(out[0], 0, 'f', 3)
            .arg(out[1], 0, 'f', 3)
            .arg(out[2], 0, 'f', 3));
}
