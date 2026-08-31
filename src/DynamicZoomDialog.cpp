#include "DynamicZoomDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVariant>
#include <QVBoxLayout>

#include <initializer_list>

namespace {

enum PresetIndex {
    ZoomInIndex,
    ZoomOutIndex,
    PanLeftIndex,
    PanRightIndex,
    PanUpIndex,
    PanDownIndex,
    CustomIndex
};

QDoubleSpinBox *makeRectSpinBox(QWidget *parent, double minimum)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minimum, 1.0);
    spin->setDecimals(3);
    spin->setSingleStep(0.01);
    spin->setAccelerated(true);
    return spin;
}

} // namespace

DynamicZoomDialog::DynamicZoomDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("ダイナミックズーム"));
    setModal(true);
    setMinimumWidth(560);

    auto *root = new QVBoxLayout(this);
    auto *presetRow = new QFormLayout();
    m_presetCombo = new QComboBox(this);
    m_presetCombo->addItems({
        QStringLiteral("ズームイン"),
        QStringLiteral("ズームアウト"),
        QStringLiteral("左パン"),
        QStringLiteral("右パン"),
        QStringLiteral("上パン"),
        QStringLiteral("下パン"),
        QStringLiteral("カスタム")
    });
    presetRow->addRow(QStringLiteral("プリセット"), m_presetCombo);

    m_easingCombo = new QComboBox(this);
    m_easingCombo->addItem(QStringLiteral("リニア (Linear)"),
                           static_cast<int>(dynzoom::Easing::Linear));
    m_easingCombo->addItem(QStringLiteral("イーズイン・アウト (EaseInOut)"),
                           static_cast<int>(dynzoom::Easing::EaseInOut));
    m_easingCombo->setCurrentIndex(1);
    presetRow->addRow(QStringLiteral("イージング"), m_easingCombo);
    root->addLayout(presetRow);

    const auto makeRectGroup = [this](const QString& title,
                                      RectEditors *editors) {
        auto *group = new QGroupBox(title, this);
        auto *form = new QFormLayout(group);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        editors->cx = makeRectSpinBox(group, 0.0);
        editors->cy = makeRectSpinBox(group, 0.0);
        editors->w = makeRectSpinBox(group, 0.01);
        editors->h = makeRectSpinBox(group, 0.01);
        editors->h->setEnabled(false);
        editors->h->setToolTip(
            QStringLiteral("キャンバスのアスペクト比に合わせて自動設定されます"));
        form->addRow(QStringLiteral("中心 X (cx)"), editors->cx);
        form->addRow(QStringLiteral("中心 Y (cy)"), editors->cy);
        form->addRow(QStringLiteral("幅 (w)"), editors->w);
        form->addRow(QStringLiteral("高さ (h、自動)"), editors->h);
        return group;
    };
    auto *frames = new QHBoxLayout();
    frames->addWidget(makeRectGroup(QStringLiteral("開始枠"), &m_startEditors));
    frames->addWidget(makeRectGroup(QStringLiteral("終了枠"), &m_endEditors));
    root->addLayout(frames);

    auto *swapRow = new QHBoxLayout();
    swapRow->addStretch();
    auto *swapButton = new QPushButton(QStringLiteral("スワップ"), this);
    swapButton->setToolTip(QStringLiteral("開始枠と終了枠を入れ替えます"));
    swapRow->addWidget(swapButton);
    root->addLayout(swapRow);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("適用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    root->addWidget(buttons);

    connect(m_presetCombo, &QComboBox::currentIndexChanged,
            this, &DynamicZoomDialog::applyPreset);
    const RectEditors editorSets[] = {m_startEditors, m_endEditors};
    for (const RectEditors& editors : editorSets) {
        for (QDoubleSpinBox *spin : {editors.cx, editors.cy}) {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                    this, [this](double) { markCustom(); });
        }
        connect(editors.w, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this, height = editors.h](double width) {
            const QSignalBlocker blocker(height);
            height->setValue(width);
            markCustom();
        });
    }
    connect(swapButton, &QPushButton::clicked, this, [this]() {
        const dynzoom::Rect oldStart = startRect();
        const dynzoom::Rect oldEnd = endRect();
        m_updatingPreset = true;
        writeRect(m_startEditors, oldEnd);
        writeRect(m_endEditors, oldStart);
        m_updatingPreset = false;
        markCustom();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    applyPreset(ZoomInIndex);
}

dynzoom::Rect DynamicZoomDialog::readRect(const RectEditors& editors)
{
    return dynzoom::Rect{
        editors.cx->value(), editors.cy->value(),
        editors.w->value()
    };
}

void DynamicZoomDialog::writeRect(const RectEditors& editors,
                                  const dynzoom::Rect& rect)
{
    const QSignalBlocker cxBlocker(editors.cx);
    const QSignalBlocker cyBlocker(editors.cy);
    const QSignalBlocker wBlocker(editors.w);
    const QSignalBlocker hBlocker(editors.h);
    editors.cx->setValue(rect.cx);
    editors.cy->setValue(rect.cy);
    editors.w->setValue(rect.w);
    editors.h->setValue(rect.w);
}

dynzoom::Rect DynamicZoomDialog::startRect() const
{
    return readRect(m_startEditors);
}

dynzoom::Rect DynamicZoomDialog::endRect() const
{
    return readRect(m_endEditors);
}

dynzoom::Easing DynamicZoomDialog::easing() const
{
    return static_cast<dynzoom::Easing>(m_easingCombo->currentData().toInt());
}

void DynamicZoomDialog::applyPreset(int presetIndex)
{
    if (presetIndex == CustomIndex)
        return;

    dynzoom::Rect start = dynzoom::presetRect(dynzoom::Preset::Full);
    dynzoom::Rect end = start;
    switch (presetIndex) {
    case ZoomInIndex:
        end = dynzoom::presetRect(dynzoom::Preset::ZoomIn);
        break;
    case ZoomOutIndex:
        start = dynzoom::presetRect(dynzoom::Preset::ZoomOut);
        break;
    case PanLeftIndex:
        start = dynzoom::presetRect(dynzoom::Preset::PanRight);
        end = dynzoom::presetRect(dynzoom::Preset::PanLeft);
        break;
    case PanRightIndex:
        start = dynzoom::presetRect(dynzoom::Preset::PanLeft);
        end = dynzoom::presetRect(dynzoom::Preset::PanRight);
        break;
    case PanUpIndex:
        start = dynzoom::presetRect(dynzoom::Preset::PanDown);
        end = dynzoom::presetRect(dynzoom::Preset::PanUp);
        break;
    case PanDownIndex:
        start = dynzoom::presetRect(dynzoom::Preset::PanUp);
        end = dynzoom::presetRect(dynzoom::Preset::PanDown);
        break;
    default:
        return;
    }

    m_updatingPreset = true;
    writeRect(m_startEditors, start);
    writeRect(m_endEditors, end);
    m_updatingPreset = false;
}

void DynamicZoomDialog::markCustom()
{
    if (m_updatingPreset || m_presetCombo->currentIndex() == CustomIndex)
        return;
    const QSignalBlocker blocker(m_presetCombo);
    m_presetCombo->setCurrentIndex(CustomIndex);
}
