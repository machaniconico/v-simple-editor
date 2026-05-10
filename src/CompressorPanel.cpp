#include "CompressorPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// Dials are int-valued; we scale by these factors to map UI -> double.
// Threshold maps -60..0 dB directly (no scaling).
// Ratio maps 100..5000 (= 1.00..50.00 with /100 scale).
// Attack maps 1..1000 (= 0.1..100.0 ms with /10 scale).
// Release maps 10..1000 (= 10..1000 ms direct).
// Knee maps 0..100 (= 0.0..10.0 dB with /10 scale).
// Makeup maps 0..240 (= 0..24 dB with /10 scale).
constexpr int kThreshMin = -60;
constexpr int kThreshMax = 0;
constexpr int kRatioMin = 100;
constexpr int kRatioMax = 5000;
constexpr int kAttackMin = 1;     // 0.1 ms
constexpr int kAttackMax = 1000;  // 100 ms
constexpr int kReleaseMin = 10;
constexpr int kReleaseMax = 1000;
constexpr int kKneeMin = 0;
constexpr int kKneeMax = 100;     // 10.0 dB
constexpr int kMakeupMin = 0;
constexpr int kMakeupMax = 240;   // 24.0 dB

QDial *makeDial(int lo, int hi, int initial) {
    auto *d = new QDial();
    d->setRange(lo, hi);
    d->setValue(initial);
    d->setNotchesVisible(true);
    d->setFixedSize(60, 60);
    return d;
}
} // namespace

CompressorPanel::CompressorPanel(QWidget *parent) : QWidget(parent) {
    buildUi();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(33); // ~30 Hz
    connect(m_pollTimer, &QTimer::timeout,
            this, &CompressorPanel::onPollMeter);
    m_pollTimer->start();
}

void CompressorPanel::setMixer(AudioMixer *mixer) {
    m_mixer = mixer;
}

void CompressorPanel::setTrackList(const QList<int> &trackIds) {
    const int prevId = currentTrackId();
    m_suppressEmit = true;
    m_trackCombo->clear();
    m_trackCombo->addItem(tr("Master"), 0);
    for (int id : trackIds) {
        if (id <= 0) continue;
        m_trackCombo->addItem(tr("A%1").arg(id), id);
    }
    // Restore previous selection if still present.
    int idx = m_trackCombo->findData(prevId);
    if (idx < 0) idx = 0;
    m_trackCombo->setCurrentIndex(idx);
    m_suppressEmit = false;
}

void CompressorPanel::loadSettings(int trackId,
                                   const AudioMixer::CompressorSettings &c) {
    int idx = m_trackCombo->findData(trackId);
    if (idx < 0) idx = 0;
    m_suppressEmit = true;
    m_trackCombo->setCurrentIndex(idx);
    applySettingsToControls(c);
    m_suppressEmit = false;
}

int CompressorPanel::currentTrackId() const {
    if (!m_trackCombo || m_trackCombo->count() == 0) return 0;
    return m_trackCombo->currentData().toInt();
}

void CompressorPanel::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // Header — track combobox + enable + limiter button.
    auto *header = new QHBoxLayout();
    header->setSpacing(6);
    header->addWidget(new QLabel(tr("Track:")));
    m_trackCombo = new QComboBox();
    m_trackCombo->addItem(tr("Master"), 0);
    header->addWidget(m_trackCombo, 1);

    m_enable = new QCheckBox(tr("Enable"));
    header->addWidget(m_enable);

    m_limiterMode = new QPushButton(tr("Limiter"));
    m_limiterMode->setToolTip(tr("Set ratio=20:1, attack=0.5ms, makeup=0 (hard limit)"));
    header->addWidget(m_limiterMode);

    root->addLayout(header);

    // Dials grid.
    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(2);
    auto addColumn = [&](int col, const QString &label,
                         QDial *&dial, QLabel *&value, int lo, int hi, int v) {
        dial = makeDial(lo, hi, v);
        auto *title = new QLabel(label);
        title->setAlignment(Qt::AlignHCenter);
        value = new QLabel();
        value->setAlignment(Qt::AlignHCenter);
        value->setMinimumWidth(50);
        grid->addWidget(title, 0, col, Qt::AlignHCenter);
        grid->addWidget(dial,  1, col, Qt::AlignHCenter);
        grid->addWidget(value, 2, col, Qt::AlignHCenter);
    };
    addColumn(0, tr("Threshold"), m_threshold, m_thresholdLabel,
              kThreshMin, kThreshMax, 0);
    addColumn(1, tr("Ratio"),     m_ratio,     m_ratioLabel,
              kRatioMin, kRatioMax, 100);
    addColumn(2, tr("Attack"),    m_attack,    m_attackLabel,
              kAttackMin, kAttackMax, 50);
    addColumn(3, tr("Release"),   m_release,   m_releaseLabel,
              kReleaseMin, kReleaseMax, 100);
    addColumn(4, tr("Knee"),      m_knee,      m_kneeLabel,
              kKneeMin, kKneeMax, 20);
    addColumn(5, tr("Make-up"),   m_makeup,    m_makeupLabel,
              kMakeupMin, kMakeupMax, 0);
    root->addLayout(grid);

    // GR meter.
    auto *meterRow = new QHBoxLayout();
    meterRow->addWidget(new QLabel(tr("GR:")));
    m_grMeter = new QProgressBar();
    m_grMeter->setRange(0, 240); // 0..24 dB scaled by 10
    m_grMeter->setValue(0);
    m_grMeter->setTextVisible(false);
    m_grMeter->setOrientation(Qt::Horizontal);
    m_grMeter->setFixedHeight(14);
    meterRow->addWidget(m_grMeter, 1);
    m_grValue = new QLabel(tr("0.0 dB"));
    m_grValue->setMinimumWidth(60);
    meterRow->addWidget(m_grValue);
    root->addLayout(meterRow);

    // Wire signals — every change collects + emits.
    auto connectDial = [&](QDial *d) {
        connect(d, &QDial::valueChanged,
                this, &CompressorPanel::onAnyControlChanged);
    };
    connectDial(m_threshold);
    connectDial(m_ratio);
    connectDial(m_attack);
    connectDial(m_release);
    connectDial(m_knee);
    connectDial(m_makeup);
    connect(m_enable, &QCheckBox::toggled,
            this, &CompressorPanel::onAnyControlChanged);
    connect(m_limiterMode, &QPushButton::clicked,
            this, &CompressorPanel::onLimiterModeClicked);
    connect(m_trackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CompressorPanel::onTrackChanged);

    onAnyControlChanged(); // refresh labels (no emit due to initial state)
}

AudioMixer::CompressorSettings CompressorPanel::collectSettings() const {
    AudioMixer::CompressorSettings c;
    c.thresholdDb = static_cast<double>(m_threshold->value());
    c.ratio       = m_ratio->value() / 100.0;
    c.attackMs    = m_attack->value() / 10.0;
    c.releaseMs   = static_cast<double>(m_release->value());
    c.kneeDb      = m_knee->value() / 10.0;
    c.makeupDb    = m_makeup->value() / 10.0;
    c.enabled     = m_enable->isChecked();
    return c;
}

void CompressorPanel::applySettingsToControls(
        const AudioMixer::CompressorSettings &c) {
    m_threshold->setValue(qBound(kThreshMin,
        static_cast<int>(qRound(c.thresholdDb)), kThreshMax));
    m_ratio->setValue(qBound(kRatioMin,
        static_cast<int>(qRound(c.ratio * 100.0)), kRatioMax));
    m_attack->setValue(qBound(kAttackMin,
        static_cast<int>(qRound(c.attackMs * 10.0)), kAttackMax));
    m_release->setValue(qBound(kReleaseMin,
        static_cast<int>(qRound(c.releaseMs)), kReleaseMax));
    m_knee->setValue(qBound(kKneeMin,
        static_cast<int>(qRound(c.kneeDb * 10.0)), kKneeMax));
    m_makeup->setValue(qBound(kMakeupMin,
        static_cast<int>(qRound(c.makeupDb * 10.0)), kMakeupMax));
    m_enable->setChecked(c.enabled);
    onAnyControlChanged();
}

void CompressorPanel::onAnyControlChanged() {
    const auto c = collectSettings();
    if (m_thresholdLabel)
        m_thresholdLabel->setText(tr("%1 dB").arg(c.thresholdDb, 0, 'f', 0));
    if (m_ratioLabel)
        m_ratioLabel->setText(tr("%1:1").arg(c.ratio, 0, 'f', 1));
    if (m_attackLabel)
        m_attackLabel->setText(tr("%1 ms").arg(c.attackMs, 0, 'f', 1));
    if (m_releaseLabel)
        m_releaseLabel->setText(tr("%1 ms").arg(c.releaseMs, 0, 'f', 0));
    if (m_kneeLabel)
        m_kneeLabel->setText(tr("%1 dB").arg(c.kneeDb, 0, 'f', 1));
    if (m_makeupLabel)
        m_makeupLabel->setText(tr("+%1 dB").arg(c.makeupDb, 0, 'f', 1));
    emitChange();
}

void CompressorPanel::onTrackChanged(int /*idx*/) {
    if (m_suppressEmit) return;
    // Pull the live settings from the mixer for the newly-selected track
    // so the dials reflect what is actually running.
    if (m_mixer) {
        const auto c = m_mixer->compressorForTrack(currentTrackId());
        m_suppressEmit = true;
        applySettingsToControls(c);
        m_suppressEmit = false;
    }
}

void CompressorPanel::onLimiterModeClicked() {
    // Brick-wall limiter shortcut: ratio=20, attack=0.5ms, makeup=0,
    // enable=on. Threshold/release/knee left as the user set them.
    m_suppressEmit = true;
    m_ratio->setValue(2000);    // 20.00:1
    m_attack->setValue(5);      // 0.5 ms
    m_makeup->setValue(0);
    m_enable->setChecked(true);
    m_suppressEmit = false;
    onAnyControlChanged();
}

void CompressorPanel::onPollMeter() {
    if (!m_mixer || !m_grMeter || !m_grValue) return;
    const double gr = m_mixer->currentGainReductionDb(currentTrackId());
    const double clamped = qBound(0.0, gr, 24.0);
    m_grMeter->setValue(static_cast<int>(qRound(clamped * 10.0)));
    m_grValue->setText(tr("-%1 dB").arg(clamped, 0, 'f', 1));
}

void CompressorPanel::emitChange() {
    if (m_suppressEmit) return;
    emit compressorChanged(currentTrackId(), collectSettings());
}
