#include "LoudnessPanel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "LoudnessAnalyzer.h"

namespace {
struct Preset {
    const char *label;
    double targetLUFS;
};
constexpr Preset kPresets[] = {
    { "YouTube (-14 LUFS)",       -14.0 },
    { "Spotify (-14 LUFS)",       -14.0 },
    { "Apple Music (-16 LUFS)",   -16.0 },
    { "\u653E\u9001 EBU R128 (-23 LUFS)", -23.0 },
    { "\u6620\u753B (-24 LUFS)",  -24.0 },
    { "\u30AB\u30B9\u30BF\u30E0...",          0.0 },  // value from spin
};
constexpr int kCustomIndex = static_cast<int>(Custom);
} // namespace

LoudnessPanel::LoudnessPanel(QWidget *parent) : QWidget(parent) {
    buildUi();
}

void LoudnessPanel::setMeasurement(double integratedLUFS, double momentaryLUFS,
                                    double shortTermLUFS, double truePeakDBTP) {
    m_hasMeasurement = true;
    m_integratedLUFS = integratedLUFS;

    if (m_integratedLabel)
        m_integratedLabel->setText(
            QStringLiteral("%1 LUFS").arg(integratedLUFS, 0, 'f', 1));
    if (m_momentaryLabel)
        m_momentaryLabel->setText(
            QStringLiteral("%1 LUFS").arg(momentaryLUFS, 0, 'f', 1));
    if (m_shortTermLabel)
        m_shortTermLabel->setText(
            QStringLiteral("%1 LUFS").arg(shortTermLUFS, 0, 'f', 1));
    if (m_truePeakLabel)
        m_truePeakLabel->setText(
            QStringLiteral("%1 dBTP").arg(truePeakDBTP, 0, 'f', 1));

    updateGauge();
    updateHint();
}

double LoudnessPanel::selectedTargetLUFS() const {
    int idx = m_deliveryTarget ? m_deliveryTarget->currentIndex() : 0;
    if (idx == kCustomIndex && m_customSpin)
        return m_customSpin->value();
    if (idx >= 0 && idx < static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0])))
        return kPresets[idx].targetLUFS;
    return -14.0; // safe default
}

void LoudnessPanel::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // --- Readout labels (2x2 grid via nested H/V layouts) ---
    auto addReadout = [&](const QString &title, QLabel *&value) {
        auto *row = new QHBoxLayout();
        row->setSpacing(4);
        auto *t = new QLabel(title);
        t->setMinimumWidth(90);
        value = new QLabel(QStringLiteral("--"));
        value->setMinimumWidth(80);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(t);
        row->addWidget(value, 1);
        root->addLayout(row);
    };

    addReadout(QStringLiteral("Integrated:"),  m_integratedLabel);
    addReadout(QStringLiteral("Momentary:"),   m_momentaryLabel);
    addReadout(QStringLiteral("Short-term:"),  m_shortTermLabel);
    addReadout(QStringLiteral("True Peak:"),   m_truePeakLabel);

    // --- Separator ---
    auto *sep = new QLabel();
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep);

    // --- Delivery target combo ---
    auto *targetRow = new QHBoxLayout();
    targetRow->setSpacing(4);
    targetRow->addWidget(new QLabel(QStringLiteral("Delivery:")));
    m_deliveryTarget = new QComboBox();
    for (const auto &p : kPresets)
        m_deliveryTarget->addItem(QStringLiteral("%1").arg(p.label));
    targetRow->addWidget(m_deliveryTarget, 1);
    root->addLayout(targetRow);

    // --- Custom LUFS spin (disabled until 'Custom' is selected) ---
    auto *customRow = new QHBoxLayout();
    customRow->setSpacing(4);
    customRow->addWidget(new QLabel(QStringLiteral("Custom LUFS:")));
    m_customSpin = new QDoubleSpinBox();
    m_customSpin->setRange(-40.0, 0.0);
    m_customSpin->setSingleStep(0.5);
    m_customSpin->setDecimals(1);
    m_customSpin->setValue(-14.0);
    m_customSpin->setSuffix(" LUFS");
    m_customSpin->setEnabled(false);
    customRow->addWidget(m_customSpin, 1);
    root->addLayout(customRow);

    // --- Normalize button ---
    m_normalizeBtn = new QPushButton(
        QStringLiteral("\u30BF\u30FC\u30B2\u30C3\u30C8\u306B\u6B63\u898F\u5316"));
    root->addWidget(m_normalizeBtn);

    // --- Gauge (current vs target bar) ---
    m_gauge = new QProgressBar();
    m_gauge->setRange(-40, 0);
    m_gauge->setValue(0);
    m_gauge->setFormat(QStringLiteral("Current: -- / Target: --"));
    root->addWidget(m_gauge);

    // --- Hint label ---
    m_hintLabel = new QLabel();
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 10px;"));
    root->addWidget(m_hintLabel);

    // --- Signals ---
    connect(m_normalizeBtn, &QPushButton::clicked,
            this, &LoudnessPanel::onNormalizeClicked);
    connect(m_deliveryTarget, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LoudnessPanel::onDeliveryTargetChanged);

    updateHint();
}

void LoudnessPanel::onNormalizeClicked() {
    if (!m_hasMeasurement) {
        if (m_hintLabel)
            m_hintLabel->setText(
                QStringLiteral("\u9996\u3081\u3066\u8A08\u6E2C\u3057\u3066\u304F\u3060\u3055\u3044"));
        return;
    }

    const double target = selectedTargetLUFS();
    const double gainDb = target - m_integratedLUFS;
    emit normalizeRequested(target, gainDb);
}

void LoudnessPanel::onDeliveryTargetChanged(int index) {
    const bool isCustom = (index == kCustomIndex);
    if (m_customSpin)
        m_customSpin->setEnabled(isCustom);
    updateGauge();
}

void LoudnessPanel::updateGauge() {
    if (!m_gauge) return;

    if (!m_hasMeasurement) {
        m_gauge->setFormat(QStringLiteral("Current: -- / Target: --"));
        return;
    }

    const double target = selectedTargetLUFS();
    m_gauge->setFormat(
        QStringLiteral("Current: %1 / Target: %2 LUFS")
            .arg(m_integratedLUFS, 0, 'f', 1)
            .arg(target, 0, 'f', 1));

    // Clamp integrated to gauge range for visual display
    const int val = qBound(-40, static_cast<int>(qRound(m_integratedLUFS)), 0);
    m_gauge->setValue(val);
}

void LoudnessPanel::updateHint() {
    if (!m_hintLabel) return;
    if (m_hasMeasurement) {
        const double target = selectedTargetLUFS();
        const double gainDb = target - m_integratedLUFS;
        m_hintLabel->setText(
            QStringLiteral("Apply %1 dB gain to reach %2 LUFS")
                .arg(gainDb, 0, 'f', 1)
                .arg(target, 0, 'f', 1));
    } else {
        m_hintLabel->clear();
    }
}
