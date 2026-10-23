#include "ShapeModifierDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QVBoxLayout>

ShapeModifierDialog::ShapeModifierDialog(const ShapeModifiers &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("シェイプモディファイア"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("クリップの先頭のシェイプを編集します。"), this));
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
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
    contentLayout->addWidget(repeaterBox);

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
    contentLayout->addWidget(trimBox);

    auto *roundBox = new QGroupBox(QStringLiteral("角の丸め"), this);
    auto *roundForm = new QFormLayout(roundBox);
    m_roundCorners = new QCheckBox(QStringLiteral("有効"), roundBox);
    m_roundCorners->setChecked(initial.roundCorners.enabled);
    roundForm->addRow(m_roundCorners);
    m_radius = spin(roundForm, QStringLiteral("半径"), QStringLiteral("radius"),
                    0, 1000000, initial.roundCorners.radius, QStringLiteral(" px"));
    contentLayout->addWidget(roundBox);

    auto intSpin = [this](QFormLayout *form, const QString &label, const QString &name,
                          int low, int high, int value) {
        auto *box = new QSpinBox(this);
        box->setObjectName(name);
        box->setRange(low, high);
        box->setValue(value);
        form->addRow(label, box);
        connect(box, &QSpinBox::valueChanged, this, [this](int) { emit modifiersChanged(); });
        return box;
    };
    auto *zigBox = new QGroupBox(QStringLiteral("ジグザグ"), this);
    auto *zigForm = new QFormLayout(zigBox);
    m_zigZag = new QCheckBox(QStringLiteral("有効"), zigBox);
    m_zigZag->setChecked(initial.zigZag.enabled);
    zigForm->addRow(m_zigZag);
    m_zigSize = spin(zigForm, QStringLiteral("大きさ"), QStringLiteral("zigSize"),
                     -1000000, 1000000, initial.zigZag.size, QStringLiteral(" px"));
    m_ridges = intSpin(zigForm, QStringLiteral("線分あたりの山数"), QStringLiteral("ridges"),
                       1, 100, initial.zigZag.ridgesPerSegment);
    m_zigSmooth = new QCheckBox(QStringLiteral("なめらか"), zigBox);
    m_zigSmooth->setChecked(initial.zigZag.smooth);
    zigForm->addRow(m_zigSmooth);
    contentLayout->addWidget(zigBox);

    auto *wiggleBox = new QGroupBox(QStringLiteral("パスのウィグル (静的)"), this);
    auto *wiggleForm = new QFormLayout(wiggleBox);
    m_wiggle = new QCheckBox(QStringLiteral("有効"), wiggleBox);
    m_wiggle->setChecked(initial.wigglePath.enabled);
    wiggleForm->addRow(m_wiggle);
    m_wiggleAmp = spin(wiggleForm, QStringLiteral("振幅"), QStringLiteral("wiggleAmplitude"),
                       -1000000, 1000000, initial.wigglePath.amplitude, QStringLiteral(" px"));
    m_wiggleDetail = intSpin(wiggleForm, QStringLiteral("細かさ"), QStringLiteral("wiggleDetail"),
                             1, 100, initial.wigglePath.detail);
    m_wiggleSeed = intSpin(wiggleForm, QStringLiteral("シード"), QStringLiteral("wiggleSeed"),
                           -1000000, 1000000, initial.wigglePath.seed);
    contentLayout->addWidget(wiggleBox);
    contentLayout->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    connect(m_roundCorners, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_zigZag, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_zigSmooth, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_wiggle, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_repeater, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_trim, &QCheckBox::toggled, this, [this](bool) { emit modifiersChanged(); });
    connect(m_copies, &QSpinBox::valueChanged, this, [this](int) { emit modifiersChanged(); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("適用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(content->sizeHint().width() + scroll->verticalScrollBar()->sizeHint().width() + 24,
           qMin(content->sizeHint().height() + 120, 640));
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
    m.roundCorners.enabled = m_roundCorners->isChecked();
    m.roundCorners.radius = m_radius->value();
    m.zigZag.enabled = m_zigZag->isChecked();
    m.zigZag.size = m_zigSize->value();
    m.zigZag.ridgesPerSegment = m_ridges->value();
    m.zigZag.smooth = m_zigSmooth->isChecked();
    m.wigglePath.enabled = m_wiggle->isChecked();
    m.wigglePath.amplitude = m_wiggleAmp->value();
    m.wigglePath.detail = m_wiggleDetail->value();
    m.wigglePath.seed = m_wiggleSeed->value();
    return m;
}
