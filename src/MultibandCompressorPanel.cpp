#include "MultibandCompressorPanel.h"
#include "AudioMixer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <iterator>

namespace {
constexpr int kNoTrack = INT_MIN; // AudioMixer::kMasterTrackId (-1) is a valid target
}

MultibandCompressorPanel::MultibandCompressorPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("MultibandCompressorPanel"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    outer->setSpacing(6);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *container = new QWidget(scroll);
    auto *root = new QVBoxLayout(container);
    root->setContentsMargins(2, 2, 2, 2);
    root->setSpacing(8);

    // Header: Target + Enable + Preset
    auto *topForm = new QFormLayout();
    topForm->setContentsMargins(0, 0, 0, 0);
    m_target = new QComboBox(container);
    m_target->setObjectName(QStringLiteral("multiband_target"));
    topForm->addRow(tr("対象"), m_target);

    auto *enableRow = new QHBoxLayout();
    m_enabled = new QCheckBox(tr("有効"), container);
    m_enabled->setObjectName(QStringLiteral("multiband_enabled"));
    enableRow->addWidget(m_enabled);
    enableRow->addStretch();
    m_preset = new QComboBox(container);
    m_preset->setObjectName(QStringLiteral("multiband_preset"));
    m_preset->addItem(tr("プリセット選択..."), QString());
    for (const QString &id : trackfx::multibandCompPresetIds())
        m_preset->addItem(trackfx::multibandCompPresetLabel(id), id);
    enableRow->addWidget(m_preset);
    topForm->addRow(tr("状態"), enableRow);

    auto *globalRow = new QHBoxLayout();
    m_outputGain = new QDoubleSpinBox(container);
    m_outputGain->setObjectName(QStringLiteral("multiband_output_gain"));
    m_outputGain->setRange(-24.0, 24.0);
    m_outputGain->setSingleStep(0.5);
    m_outputGain->setDecimals(1);
    m_outputGain->setSuffix(tr(" dB"));
    globalRow->addWidget(new QLabel(tr("出力:"), container));
    globalRow->addWidget(m_outputGain);

    m_wetMix = new QDoubleSpinBox(container);
    m_wetMix->setObjectName(QStringLiteral("multiband_wet_mix"));
    m_wetMix->setRange(0.0, 100.0);
    m_wetMix->setSingleStep(5.0);
    m_wetMix->setDecimals(0);
    m_wetMix->setSuffix(QStringLiteral(" %"));
    globalRow->addWidget(new QLabel(tr("ミックス:"), container));
    globalRow->addWidget(m_wetMix);
    topForm->addRow(tr("全体"), globalRow);
    root->addLayout(topForm);

    // Crossovers (3 frequencies for 4 LR4 bands)
    auto *xoverGroup = new QGroupBox(tr("クロスオーバー周波数 (LR4)"), container);
    auto *xoverLayout = new QHBoxLayout(xoverGroup);
    const char *xoverLabels[3] = {"Low / Lo-Mid", "Lo-Mid / Hi-Mid", "Hi-Mid / High"};
    const double xoverMin[3] = {40.0, 100.0, 400.0};
    const double xoverMax[3] = {2000.0, 8000.0, 18000.0};
    for (int i = 0; i < trackfx::MultibandCompSettings::kCrossoverCount; ++i) {
        auto *col = new QVBoxLayout();
        auto *lbl = new QLabel(QString::fromLatin1(xoverLabels[i]), xoverGroup);
        lbl->setAlignment(Qt::AlignCenter);
        auto *spin = new QDoubleSpinBox(xoverGroup);
        spin->setObjectName(QStringLiteral("multiband_xover_%1").arg(i));
        spin->setRange(xoverMin[i], xoverMax[i]);
        spin->setSingleStep(50.0);
        spin->setDecimals(0);
        spin->setSuffix(QStringLiteral(" Hz"));
        col->addWidget(lbl);
        col->addWidget(spin);
        xoverLayout->addLayout(col);
        m_crossovers[i] = spin;
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &MultibandCompressorPanel::onAnyControlChanged);
        connect(spin, &QDoubleSpinBox::editingFinished,
                this, &MultibandCompressorPanel::finishGesture);
    }
    root->addWidget(xoverGroup);

    // 4 Band cards in a 2x2 grid
    auto *bandsGrid = new QGridLayout();
    bandsGrid->setSpacing(6);
    const QString bandTitles[4] = {
        tr("Low（低域）"),
        tr("Low-Mid（中低域）"),
        tr("High-Mid（中高域）"),
        tr("High（高域）"),
    };
    const auto makeSpin = [&](QWidget *p, const QString &name, double lo, double hi,
                              double step, int decimals, const QString &suffix) {
        auto *box = new QDoubleSpinBox(p);
        box->setObjectName(name);
        box->setRange(lo, hi);
        box->setSingleStep(step);
        box->setDecimals(decimals);
        box->setSuffix(suffix);
        connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &MultibandCompressorPanel::onAnyControlChanged);
        connect(box, &QDoubleSpinBox::editingFinished,
                this, &MultibandCompressorPanel::finishGesture);
        return box;
    };

    for (int b = 0; b < trackfx::MultibandCompSettings::kBandCount; ++b) {
        auto *group = new QGroupBox(bandTitles[b], container);
        auto *form = new QFormLayout(group);
        form->setContentsMargins(6, 6, 6, 6);
        form->setSpacing(4);
        auto &bc = m_bands[b];
        const QString prefix = QStringLiteral("multiband_b%1_").arg(b);

        bc.threshold = makeSpin(group, prefix + QStringLiteral("threshold"), -60.0, 0.0, 1.0, 1, tr(" dB"));
        bc.ratio = makeSpin(group, prefix + QStringLiteral("ratio"), 1.0, 20.0, 0.5, 1, QStringLiteral(":1"));
        bc.attack = makeSpin(group, prefix + QStringLiteral("attack"), 0.1, 100.0, 1.0, 1, tr(" ms"));
        bc.release = makeSpin(group, prefix + QStringLiteral("release"), 10.0, 1000.0, 10.0, 0, tr(" ms"));
        bc.knee = makeSpin(group, prefix + QStringLiteral("knee"), 0.0, 12.0, 0.5, 1, tr(" dB"));
        bc.makeup = makeSpin(group, prefix + QStringLiteral("makeup"), -12.0, 24.0, 0.5, 1, tr(" dB"));

        form->addRow(tr("しきい値"), bc.threshold);
        form->addRow(tr("レシオ"), bc.ratio);
        form->addRow(tr("アタック"), bc.attack);
        form->addRow(tr("リリース"), bc.release);
        form->addRow(tr("ニー"), bc.knee);
        form->addRow(tr("メイクアップ"), bc.makeup);

        auto *btnRow = new QHBoxLayout();
        bc.bypass = new QPushButton(tr("Bypass"), group);
        bc.bypass->setObjectName(prefix + QStringLiteral("bypass"));
        bc.bypass->setCheckable(true);
        bc.solo = new QPushButton(tr("Solo"), group);
        bc.solo->setObjectName(prefix + QStringLiteral("solo"));
        bc.solo->setCheckable(true);
        btnRow->addWidget(bc.bypass);
        btnRow->addWidget(bc.solo);
        form->addRow(btnRow);

        auto *grRow = new QHBoxLayout();
        bc.grMeter = new QProgressBar(group);
        bc.grMeter->setRange(0, 240); // 0.0 .. 24.0 dB in 0.1 dB units
        bc.grMeter->setValue(0);
        bc.grMeter->setTextVisible(false);
        bc.grMeter->setMaximumHeight(10);
        bc.grLabel = new QLabel(QStringLiteral("0.0 dB"), group);
        bc.grLabel->setMinimumWidth(48);
        grRow->addWidget(bc.grMeter, 1);
        grRow->addWidget(bc.grLabel);
        form->addRow(tr("GR"), grRow);

        connect(bc.bypass, &QPushButton::toggled, this, &MultibandCompressorPanel::onAnyControlChanged);
        connect(bc.bypass, &QPushButton::toggled, this, &MultibandCompressorPanel::finishGesture);
        connect(bc.solo, &QPushButton::toggled, this, &MultibandCompressorPanel::onSoloToggled);

        bandsGrid->addWidget(group, b / 2, b % 2);
    }
    root->addLayout(bandsGrid);
    root->addStretch();

    scroll->setWidget(container);
    outer->addWidget(scroll);

    connect(m_enabled, &QCheckBox::toggled, this, &MultibandCompressorPanel::onAnyControlChanged);
    connect(m_enabled, &QCheckBox::toggled, this, &MultibandCompressorPanel::finishGesture);
    connect(m_outputGain, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MultibandCompressorPanel::onAnyControlChanged);
    connect(m_outputGain, &QDoubleSpinBox::editingFinished,
            this, &MultibandCompressorPanel::finishGesture);
    connect(m_wetMix, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MultibandCompressorPanel::onAnyControlChanged);
    connect(m_wetMix, &QDoubleSpinBox::editingFinished,
            this, &MultibandCompressorPanel::finishGesture);
    connect(m_preset, QOverload<int>::of(&QComboBox::activated),
            this, &MultibandCompressorPanel::onPresetActivated);
    connect(m_target, &QComboBox::currentIndexChanged,
            this, &MultibandCompressorPanel::onTargetChanged);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(33); // ~30 Hz live GR meter polling
    connect(m_pollTimer, &QTimer::timeout, this, &MultibandCompressorPanel::onPollMeters);

    showSettings(trackfx::MultibandCompSettings{});
}

void MultibandCompressorPanel::setMixer(AudioMixer *mixer)
{
    m_mixer = mixer;
    if (m_mixer)
        m_pollTimer->start();
    else
        m_pollTimer->stop();
}

void MultibandCompressorPanel::showSettings(const trackfx::MultibandCompSettings &s)
{
    const bool previous = m_suppressSignals;
    m_suppressSignals = true;
    m_enabled->setChecked(s.enabled);
    m_outputGain->setValue(s.outputGainDb);
    m_wetMix->setValue(s.wetMix * 100.0);
    for (int i = 0; i < trackfx::MultibandCompSettings::kCrossoverCount; ++i)
        m_crossovers[i]->setValue(s.crossoverHz[i]);
    for (int b = 0; b < trackfx::MultibandCompSettings::kBandCount; ++b) {
        const auto &cfg = s.bands[b];
        auto &bc = m_bands[b];
        bc.threshold->setValue(cfg.thresholdDb);
        bc.ratio->setValue(cfg.ratio);
        bc.attack->setValue(cfg.attackMs);
        bc.release->setValue(cfg.releaseMs);
        bc.knee->setValue(cfg.kneeDb);
        bc.makeup->setValue(cfg.makeupDb);
        bc.bypass->setChecked(cfg.bypass);
        bc.solo->setChecked(cfg.solo);
    }
    m_suppressSignals = previous;
}

void MultibandCompressorPanel::setTracks(const QStringList &itemNames, const QList<int> &trackIds)
{
    const int previousId = m_currentTrackId;
    {
        const QSignalBlocker blocker(m_target);
        m_target->clear();
        for (int i = 0; i < qMin(itemNames.size(), trackIds.size()); ++i)
            m_target->addItem(itemNames[i], trackIds[i]);
        const int index = m_target->findData(previousId);
        m_target->setCurrentIndex(index >= 0 ? index : (m_target->count() > 0 ? 0 : -1));
    }
    m_currentTrackId = m_target->currentIndex() >= 0 ? m_target->currentData().toInt() : kNoTrack;
    setEnabled(m_target->count() > 0);
    for (auto it = m_perTrackSettings.begin(); it != m_perTrackSettings.end();)
        it = trackIds.contains(it.key()) ? std::next(it) : m_perTrackSettings.erase(it);
    showSettings(m_perTrackSettings.value(m_currentTrackId));
}

void MultibandCompressorPanel::setSettings(int trackId, const trackfx::MultibandCompSettings &settings)
{
    m_perTrackSettings.insert(trackId, settings);
    if (trackId == m_currentTrackId)
        showSettings(settings);
}

int MultibandCompressorPanel::targetCount() const
{
    return m_target->count();
}

int MultibandCompressorPanel::currentTrackId() const
{
    return m_currentTrackId;
}

trackfx::MultibandCompSettings MultibandCompressorPanel::currentSettings() const
{
    trackfx::MultibandCompSettings s;
    s.enabled = m_enabled->isChecked();
    s.outputGainDb = m_outputGain->value();
    s.wetMix = m_wetMix->value() / 100.0;
    for (int i = 0; i < trackfx::MultibandCompSettings::kCrossoverCount; ++i)
        s.crossoverHz[i] = m_crossovers[i]->value();
    for (int b = 0; b < trackfx::MultibandCompSettings::kBandCount; ++b) {
        const auto &bc = m_bands[b];
        auto &dst = s.bands[b];
        dst.thresholdDb = bc.threshold->value();
        dst.ratio = bc.ratio->value();
        dst.attackMs = bc.attack->value();
        dst.releaseMs = bc.release->value();
        dst.kneeDb = bc.knee->value();
        dst.makeupDb = bc.makeup->value();
        dst.bypass = bc.bypass->isChecked();
        dst.solo = bc.solo->isChecked();
    }
    return s;
}

void MultibandCompressorPanel::onTargetChanged()
{
    if (m_currentTrackId != kNoTrack)
        m_perTrackSettings.insert(m_currentTrackId, currentSettings());
    m_currentTrackId = m_target->currentIndex() >= 0 ? m_target->currentData().toInt() : kNoTrack;
    showSettings(m_perTrackSettings.value(m_currentTrackId));
}

void MultibandCompressorPanel::onPresetActivated(int index)
{
    if (m_suppressSignals || m_currentTrackId == kNoTrack || index <= 0) return;
    const QString presetId = m_preset->itemData(index).toString();
    if (presetId.isEmpty()) return;
    const auto preset = trackfx::multibandCompPreset(presetId);
    showSettings(preset);
    {
        const QSignalBlocker blocker(m_preset);
        m_preset->setCurrentIndex(0);
    }
    m_perTrackSettings.insert(m_currentTrackId, preset);
    m_gestureDirty = true;
    ++m_emitCount;
    emit settingsChanged(m_currentTrackId, preset);
    finishGesture();
}

void MultibandCompressorPanel::onAnyControlChanged()
{
    if (m_suppressSignals || m_currentTrackId == kNoTrack) return;
    const auto settings = currentSettings();
    m_perTrackSettings.insert(m_currentTrackId, settings);
    m_gestureDirty = true;
    ++m_emitCount;
    emit settingsChanged(m_currentTrackId, settings);
}

void MultibandCompressorPanel::onSoloToggled()
{
    // Band Solo is preview audition only — never marks an Undo gesture.
    if (m_suppressSignals || m_currentTrackId == kNoTrack) return;
    const auto settings = currentSettings();
    m_perTrackSettings.insert(m_currentTrackId, settings);
    ++m_emitCount;
    emit settingsChanged(m_currentTrackId, settings);
}

void MultibandCompressorPanel::finishGesture()
{
    if (!m_gestureDirty || m_suppressSignals || m_currentTrackId == kNoTrack) return;
    m_gestureDirty = false;
    emit gestureFinished(m_currentTrackId);
}

void MultibandCompressorPanel::onPollMeters()
{
    if (!m_mixer || m_currentTrackId == kNoTrack || !isVisible()) return;
    const auto gr = m_mixer->multibandGainReductionDb(m_currentTrackId);
    for (int b = 0; b < trackfx::MultibandCompSettings::kBandCount; ++b) {
        const double db = std::clamp(gr[b], 0.0, 24.0);
        m_bands[b].grMeter->setValue(static_cast<int>(std::lround(db * 10.0)));
        m_bands[b].grLabel->setText(db > 0.05
            ? QStringLiteral("-%1 dB").arg(db, 0, 'f', 1)
            : QStringLiteral("0.0 dB"));
    }
}
