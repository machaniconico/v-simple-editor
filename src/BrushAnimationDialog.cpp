#include "BrushAnimationDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QGroupBox>
#include <QFontComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>

BrushAnimationDialog::BrushAnimationDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Brush Animation"));
    setModal(true);
    setMinimumWidth(380);

    setupUI();
}

void BrushAnimationDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    auto *paramsGroup = new QGroupBox(tr("Parameters"), this);
    auto *formLayout = new QFormLayout(paramsGroup);

    // Text input
    m_textEdit = new QLineEdit(this);
    m_textEdit->setText("Sample");
    formLayout->addRow(tr("Text:"), m_textEdit);

    // Font family
    m_fontCombo = new QFontComboBox(this);
    formLayout->addRow(tr("Font:"), m_fontCombo);

    // Font size
    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setRange(6, 200);
    m_fontSizeSpin->setValue(48);
    formLayout->addRow(tr("Size:"), m_fontSizeSpin);

    // Brush width
    m_brushWidthSpin = new QDoubleSpinBox(this);
    m_brushWidthSpin->setRange(0.5, 32.0);
    m_brushWidthSpin->setSingleStep(0.5);
    m_brushWidthSpin->setValue(4.0);
    m_brushWidthSpin->setSuffix(" px");
    formLayout->addRow(tr("Brush Width:"), m_brushWidthSpin);

    // Duration
    m_durationSpin = new QDoubleSpinBox(this);
    m_durationSpin->setRange(0.1, 60.0);
    m_durationSpin->setSingleStep(0.1);
    m_durationSpin->setValue(2.0);
    m_durationSpin->setSuffix(" s");
    formLayout->addRow(tr("Duration:"), m_durationSpin);

    // Mode
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(tr("Per Character"), static_cast<int>(PerCharacter));
    m_modeCombo->addItem(tr("Per Stroke"), static_cast<int>(PerStroke));
    m_modeCombo->setCurrentIndex(1); // default: PerStroke
    formLayout->addRow(tr("Mode:"), m_modeCombo);

    mainLayout->addWidget(paramsGroup);

    // OK / Cancel buttons
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(m_buttonBox);
}

BrushAnimationDialog::BrushAnimationParams BrushAnimationDialog::params() const
{
    BrushAnimationParams p;
    p.text = m_textEdit->text();

    QFont font = m_fontCombo->currentFont();
    font.setPointSize(m_fontSizeSpin->value());
    p.font = font;

    p.brushWidth = m_brushWidthSpin->value();
    p.durationSec = m_durationSpin->value();
    p.mode = static_cast<BrushAnimationMode>(m_modeCombo->currentData().toInt());
    p.basePosition = QPointF(0, 0);

    return p;
}
