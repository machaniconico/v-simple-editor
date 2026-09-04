#include "ShapeModifierDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

ShapeModifierDialog::ShapeModifierDialog(const ShapeModifiers &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("シェイプモディファイア"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("クリップの先頭のシェイプを編集します。"), this));
    auto *repeaterBox = new QGroupBox(QStringLiteral("リピーター"), this);
    auto *repeatForm = new QFormLayout(repeaterBox);
    m_repeater = new QCheckBox(QStringLiteral("有効"), repeaterBox);
    m_repeater->setChecked(initial.repeater.enabled);
    repeatForm->addRow(m_repeater);
    m_copies = new QSpinBox(repeaterBox);
    m_copies->setRange(1, 1000);
    m_copies->setValue(initial.repeater.copies);
    repeatForm->addRow(QStringLiteral("コピー数"), m_copies);
    auto spin = [this](QFormLayout *form, const QString &label, const QString &name,
                       double low, double high, double value, const QString &suffix) {
        auto *box = new QDoubleSpinBox(this);
        box->setObjectName(name);
        box->setDecimals(4);
        box->setRange(low, high);
        box->setSuffix(suffix);
        box->setValue(value);
        form->addRow(label, box);
        connect(box, &QDoubleSpinBox::valueChanged, this,
                [this](double) { emit modifiersChanged(); });
        return box;
    };
    m_offsetX = spin(repeatForm, QStringLiteral("移動 X"), QStringLiteral("offsetX"),
                     -1000000, 1000000, initial.repeater.offset.x(), QStringLiteral(" px"));
    m_offsetY = spin(repeatForm, QStringLiteral("移動 Y"), QStringLiteral("offsetY"),
                     -1000000, 1000000, initial.repeater.offset.y(), QStringLiteral(" px"));
    m_rotation = spin(repeatForm, QStringLiteral("回転"), QStringLiteral("rotation"),
                      -36000, 36000, initial.repeater.rotationDeg, QStringLiteral("°"));
    m_scale = spin(repeatForm, QStringLiteral("拡大率"), QStringLiteral("scale"),
                   0, 10000, initial.repeater.scale * 100.0, QStringLiteral(" %"));
    m_opacity = spin(repeatForm, QStringLiteral("最後のコピーの不透明度"), QStringLiteral("opacityEnd"),
                     0, 100, initial.repeater.opacityEnd * 100.0, QStringLiteral(" %"));
    layout->addWidget(repeaterBox);

    auto *trimBox = new QGroupBox(QStringLiteral("パスのトリミング"), this);
    auto *trimForm = new QFormLayout(trimBox);
    m_trim = new QCheckBox(QStringLiteral("有効"), trimBox);
    m_trim->setChecked(initial.trim.enabled);
    trimForm->addRow(m_trim);
    m_start = spin(trimForm, QStringLiteral("開始"), QStringLiteral("startPct"),
                   0, 100, initial.trim.startPct, QStringLiteral(" %"));
    m_end = spin(trimForm, QStringLiteral("終了"), QStringLiteral("endPct"),
                 0, 100, initial.trim.endPct, QStringLiteral(" %"));
    m_offset = spin(trimForm, QStringLiteral("オフセット"), QStringLiteral("offsetPct"),
                    -1000000, 1000000, initial.trim.offsetPct, QStringLiteral(" %"));
    layout->addWidget(trimBox);
    connect(m_repeater, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_trim, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_copies, &QSpinBox::valueChanged, this, [this](int) { emit modifiersChanged(); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("適用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

ShapeModifiers ShapeModifierDialog::modifiers() const
{
    ShapeModifiers m;
    m.repeater.enabled = m_repeater->isChecked();
    m.repeater.copies = m_copies->value();
    m.repeater.offset = QPointF(m_offsetX->value(), m_offsetY->value());
    m.repeater.rotationDeg = m_rotation->value();
    m.repeater.scale = m_scale->value() / 100.0;
    m.repeater.opacityEnd = m_opacity->value() / 100.0;
    m.trim.enabled = m_trim->isChecked();
    m.trim.startPct = m_start->value();
    m.trim.endPct = m_end->value();
    m.trim.offsetPct = m_offset->value();
    return m;
}
