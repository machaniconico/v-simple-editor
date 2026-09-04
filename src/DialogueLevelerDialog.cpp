#include "DialogueLevelerDialog.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>
#include <QVBoxLayout>

DialogueLevelerDialog::DialogueLevelerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("ダイアログレベラー"));
    setMinimumWidth(340);

    m_targetSpin = new QDoubleSpinBox(this);
    m_targetSpin->setRange(-45.0, -5.0);
    m_targetSpin->setDecimals(1);
    m_targetSpin->setSingleStep(1.0);
    m_targetSpin->setValue(-18.0);
    m_targetSpin->setSuffix(QStringLiteral(" LUFS"));

    m_smoothingSpin = new QDoubleSpinBox(this);
    m_smoothingSpin->setRange(0.0, 5.0);
    m_smoothingSpin->setDecimals(2);
    m_smoothingSpin->setSingleStep(0.1);
    m_smoothingSpin->setValue(0.5);
    m_smoothingSpin->setSuffix(QStringLiteral(" 秒"));

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("目標ラウドネス:"), m_targetSpin);
    form->addRow(QStringLiteral("強さ (平滑化):"), m_smoothingSpin);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("適用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(
        QStringLiteral("キャンセル"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

double DialogueLevelerDialog::targetLufs() const
{
    return m_targetSpin ? m_targetSpin->value() : -18.0;
}

double DialogueLevelerDialog::smoothingSec() const
{
    return m_smoothingSpin ? m_smoothingSpin->value() : 0.5;
}
