#include "SequenceSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSize>
#include <QSpinBox>
#include <QtGlobal>
#include <QVBoxLayout>

namespace {

constexpr int kCustomIndex = 3;

} // namespace

SequenceSettingsDialog::SequenceSettingsDialog(const ProjectConfig &initial,
                                               QWidget *parent)
    : QDialog(parent)
    , m_config(initial)
{
    setWindowTitle(QStringLiteral("プロジェクト設定"));
    setModal(true);
    setMinimumWidth(360);
    setupUi();
    selectInitialResolution();
}

void SequenceSettingsDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    auto *form = new QFormLayout();

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setText(m_config.name);
    form->addRow(QStringLiteral("名前:"), m_nameEdit);

    m_resolutionCombo = new QComboBox(this);
    m_resolutionCombo->addItem(QStringLiteral("1920 x 1080 (16:9)"), QSize(1920, 1080));
    m_resolutionCombo->addItem(QStringLiteral("1080 x 1920 (9:16)"), QSize(1080, 1920));
    m_resolutionCombo->addItem(QStringLiteral("1080 x 1080 (1:1)"), QSize(1080, 1080));
    m_resolutionCombo->addItem(QStringLiteral("カスタム"), QSize());
    form->addRow(QStringLiteral("解像度:"), m_resolutionCombo);

    auto *customLayout = new QHBoxLayout();
    m_widthSpin = new QSpinBox(this);
    m_widthSpin->setRange(2, 7680);
    m_widthSpin->setSingleStep(2);
    m_widthSpin->setSuffix(QStringLiteral(" px"));
    m_heightSpin = new QSpinBox(this);
    m_heightSpin->setRange(2, 7680);
    m_heightSpin->setSingleStep(2);
    m_heightSpin->setSuffix(QStringLiteral(" px"));
    customLayout->addWidget(m_widthSpin);
    customLayout->addWidget(new QLabel(QStringLiteral("x"), this));
    customLayout->addWidget(m_heightSpin);
    form->addRow(QStringLiteral("カスタム:"), customLayout);

    m_fpsSpin = new QSpinBox(this);
    m_fpsSpin->setRange(1, 240);
    m_fpsSpin->setSuffix(QStringLiteral(" fps"));
    m_fpsSpin->setValue(m_config.fps > 0 ? m_config.fps : 30);
    form->addRow(QStringLiteral("フレームレート:"), m_fpsSpin);

    mainLayout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    mainLayout->addWidget(buttons);

    connect(m_resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SequenceSettingsDialog::onPresetChanged);
    connect(m_widthSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SequenceSettingsDialog::onCustomResolutionChanged);
    connect(m_heightSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SequenceSettingsDialog::onCustomResolutionChanged);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_config.name = m_nameEdit->text();
        m_config.width = evenDimension(m_widthSpin->value());
        m_config.height = evenDimension(m_heightSpin->value());
        m_config.fps = m_fpsSpin->value();
        m_config.explicitOutputResolution = true;
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SequenceSettingsDialog::selectInitialResolution()
{
    const QSize initialSize(evenDimension(m_config.width),
                            evenDimension(m_config.height));

    int matchIndex = kCustomIndex;
    for (int i = 0; i < m_resolutionCombo->count(); ++i) {
        const QSize presetSize = m_resolutionCombo->itemData(i).toSize();
        if (presetSize.isValid() && presetSize == initialSize) {
            matchIndex = i;
            break;
        }
    }

    {
        const QSignalBlocker comboBlocker(m_resolutionCombo);
        const QSignalBlocker widthBlocker(m_widthSpin);
        const QSignalBlocker heightBlocker(m_heightSpin);
        m_resolutionCombo->setCurrentIndex(matchIndex);
        m_widthSpin->setValue(initialSize.width());
        m_heightSpin->setValue(initialSize.height());
    }
    onPresetChanged(matchIndex);
}

void SequenceSettingsDialog::onPresetChanged(int index)
{
    const QSize presetSize = m_resolutionCombo->itemData(index).toSize();
    const bool custom = !presetSize.isValid();
    m_widthSpin->setEnabled(custom);
    m_heightSpin->setEnabled(custom);

    if (custom)
        return;

    m_updating = true;
    m_widthSpin->setValue(presetSize.width());
    m_heightSpin->setValue(presetSize.height());
    m_updating = false;
}

void SequenceSettingsDialog::onCustomResolutionChanged()
{
    if (m_updating)
        return;
    if (m_resolutionCombo->currentIndex() == kCustomIndex)
        return;

    const QSignalBlocker blocker(m_resolutionCombo);
    m_resolutionCombo->setCurrentIndex(kCustomIndex);
    m_widthSpin->setEnabled(true);
    m_heightSpin->setEnabled(true);
}

int SequenceSettingsDialog::evenDimension(int value)
{
    return qMax(2, value & ~1);
}
