#include "NoiseReductionPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// Dials are int-valued; we scale by these factors to map UI -> double.
// Threshold     maps -60..0 dB direct (signed). UI displays as "-XX dB".
// Reduction     maps   0..40 dB direct.
// Attack        maps   1..500 (= 0.1..50.0 ms with /10 scale).
// Release       maps  10..1000 ms direct.
// Manual Floor  maps -80..-30 dB direct (signed).
constexpr int kThresholdMin   = -60;
constexpr int kThresholdMax   =   0;
constexpr int kReductionMin   =   0;
constexpr int kReductionMax   =  40;
constexpr int kAttackMin      =   1;   // 0.1 ms
constexpr int kAttackMax      = 500;   // 50.0 ms
constexpr int kReleaseMin     =  10;
constexpr int kReleaseMax     = 1000;
constexpr int kManualFloorMin = -80;
constexpr int kManualFloorMax = -30;

// Auto-floor estimate readout poll cadence — the audio worker thread
// updates m_estimatedFloorDb every readData fragment (~340 ms typical),
// so a 250 ms GUI poll is responsive without burning cycles.
constexpr int kFloorPollIntervalMs = 250;

QDial *makeDial(int lo, int hi, int initial) {
    auto *d = new QDial();
    d->setRange(lo, hi);
    d->setValue(initial);
    d->setNotchesVisible(true);
    d->setFixedSize(60, 60);
    return d;
}
} // namespace

NoiseReductionPanel::NoiseReductionPanel(QWidget *parent) : QWidget(parent) {
    buildUi();
}

void NoiseReductionPanel::setMixer(AudioMixer *mixer) {
    m_mixer = mixer;
}

void NoiseReductionPanel::setTrackList(const QList<int> &trackIds) {
    const int prevId = currentTrackId();
    m_suppressEmit = true;
    m_trackCombo->clear();
    m_trackCombo->addItem(tr("Master"), 0);
    for (int id : trackIds) {
        if (id <= 0) continue;
        m_trackCombo->addItem(tr("A%1").arg(id), id);
    }
    int idx = m_trackCombo->findData(prevId);
    if (idx < 0) idx = 0;
    m_trackCombo->setCurrentIndex(idx);
    m_suppressEmit = false;
}

void NoiseReductionPanel::loadSettings(
        int trackId, const AudioMixer::NoiseReductionSettings &nr) {
    int idx = m_trackCombo->findData(trackId);
    if (idx < 0) idx = 0;
    m_suppressEmit = true;
    m_trackCombo->setCurrentIndex(idx);
    applySettingsToControls(nr);
    m_suppressEmit = false;
}

int NoiseReductionPanel::currentTrackId() const {
    if (!m_trackCombo || m_trackCombo->count() == 0) return 0;
    return m_trackCombo->currentData().toInt();
}

void NoiseReductionPanel::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // Header — track combobox + enable checkbox.
    auto *header = new QHBoxLayout();
    header->setSpacing(6);
    header->addWidget(new QLabel(tr("Track:")));
    m_trackCombo = new QComboBox();
    m_trackCombo->addItem(tr("Master"), 0);
    header->addWidget(m_trackCombo, 1);

    m_enable = new QCheckBox(tr("Enable"));
    header->addWidget(m_enable);
    root->addLayout(header);

    // Dials grid — 5 columns matching Threshold / Reduction / Attack /
    // Release / Manual Floor.
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
        value->setMinimumWidth(60);
        grid->addWidget(title, 0, col, Qt::AlignHCenter);
        grid->addWidget(dial,  1, col, Qt::AlignHCenter);
        grid->addWidget(value, 2, col, Qt::AlignHCenter);
    };
    addColumn(0, tr("Threshold"),    m_threshold,   m_thresholdLabel,
              kThresholdMin,   kThresholdMax,   -20);
    addColumn(1, tr("Reduction"),    m_reduction,   m_reductionLabel,
              kReductionMin,   kReductionMax,   12);
    addColumn(2, tr("Attack"),       m_attack,      m_attackLabel,
              kAttackMin,      kAttackMax,      50);   // 5.0 ms
    addColumn(3, tr("Release"),      m_release,     m_releaseLabel,
              kReleaseMin,     kReleaseMax,     200);
    addColumn(4, tr("Manual Floor"), m_manualFloor, m_manualFloorLabel,
              kManualFloorMin, kManualFloorMax, -50);
    root->addLayout(grid);

    // Auto-floor row — checkbox + real-time floor estimate readout.
    auto *autoRow = new QHBoxLayout();
    autoRow->setSpacing(8);
    m_autoFloor = new QCheckBox(tr("Auto Floor"));
    m_autoFloor->setChecked(true);
    autoRow->addWidget(m_autoFloor);
    autoRow->addWidget(new QLabel(tr("Estimated floor:")));
    m_floorReadout = new QLabel(tr("-- dB"));
    m_floorReadout->setMinimumWidth(80);
    autoRow->addWidget(m_floorReadout);
    autoRow->addStretch(1);
    root->addLayout(autoRow);

    // Wire signals — every change collects + emits.
    auto connectDial = [&](QDial *d) {
        connect(d, &QDial::valueChanged,
                this, &NoiseReductionPanel::onAnyControlChanged);
    };
    connectDial(m_threshold);
    connectDial(m_reduction);
    connectDial(m_attack);
    connectDial(m_release);
    connectDial(m_manualFloor);
    connect(m_enable, &QCheckBox::toggled,
            this, &NoiseReductionPanel::onAnyControlChanged);
    connect(m_autoFloor, &QCheckBox::toggled,
            this, &NoiseReductionPanel::onAutoFloorToggled);
    connect(m_trackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NoiseReductionPanel::onTrackChanged);

    // Floor estimate readout poll — only meaningful when the audio
    // engine is running and the track has the NR stage enabled. Always
    // running but cheap (one map lookup + label setText).
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kFloorPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout,
            this, &NoiseReductionPanel::onPollFloorReadout);
    m_pollTimer->start();

    onAnyControlChanged();        // refresh labels (no emit due to initial state)
    updateFloorReadoutEnabled();  // initial enable/disable for manual-floor dial
}

AudioMixer::NoiseReductionSettings
NoiseReductionPanel::collectSettings() const {
    AudioMixer::NoiseReductionSettings nr;
    nr.thresholdDb   = static_cast<double>(m_threshold->value());
    nr.reductionDb   = static_cast<double>(m_reduction->value());
    nr.attackMs      = m_attack->value() / 10.0;
    nr.releaseMs     = static_cast<double>(m_release->value());
    nr.manualFloorDb = static_cast<double>(m_manualFloor->value());
    nr.autoFloor     = m_autoFloor->isChecked();
    nr.enabled       = m_enable->isChecked();
    return nr;
}

void NoiseReductionPanel::applySettingsToControls(
        const AudioMixer::NoiseReductionSettings &nr) {
    m_threshold->setValue(qBound(kThresholdMin,
        static_cast<int>(qRound(nr.thresholdDb)), kThresholdMax));
    m_reduction->setValue(qBound(kReductionMin,
        static_cast<int>(qRound(nr.reductionDb)), kReductionMax));
    m_attack->setValue(qBound(kAttackMin,
        static_cast<int>(qRound(nr.attackMs * 10.0)), kAttackMax));
    m_release->setValue(qBound(kReleaseMin,
        static_cast<int>(qRound(nr.releaseMs)), kReleaseMax));
    m_manualFloor->setValue(qBound(kManualFloorMin,
        static_cast<int>(qRound(nr.manualFloorDb)), kManualFloorMax));
    m_autoFloor->setChecked(nr.autoFloor);
    m_enable->setChecked(nr.enabled);
    updateFloorReadoutEnabled();
    onAnyControlChanged();
}

void NoiseReductionPanel::updateFloorReadoutEnabled() {
    if (!m_manualFloor || !m_manualFloorLabel || !m_autoFloor) return;
    const bool manual = !m_autoFloor->isChecked();
    m_manualFloor->setEnabled(manual);
    m_manualFloorLabel->setEnabled(manual);
}

void NoiseReductionPanel::onAnyControlChanged() {
    const auto nr = collectSettings();
    if (m_thresholdLabel)
        m_thresholdLabel->setText(tr("%1 dB").arg(nr.thresholdDb, 0, 'f', 0));
    if (m_reductionLabel)
        m_reductionLabel->setText(tr("%1 dB").arg(nr.reductionDb, 0, 'f', 0));
    if (m_attackLabel)
        m_attackLabel->setText(tr("%1 ms").arg(nr.attackMs, 0, 'f', 1));
    if (m_releaseLabel)
        m_releaseLabel->setText(tr("%1 ms").arg(nr.releaseMs, 0, 'f', 0));
    if (m_manualFloorLabel)
        m_manualFloorLabel->setText(tr("%1 dB").arg(nr.manualFloorDb, 0, 'f', 0));
    emitChange();
}

void NoiseReductionPanel::onTrackChanged(int /*idx*/) {
    if (m_suppressEmit) return;
    if (m_mixer) {
        const auto nr = m_mixer->noiseReductionForTrack(currentTrackId());
        m_suppressEmit = true;
        applySettingsToControls(nr);
        m_suppressEmit = false;
    }
}

void NoiseReductionPanel::onAutoFloorToggled(bool /*checked*/) {
    updateFloorReadoutEnabled();
    onAnyControlChanged();
}

void NoiseReductionPanel::onPollFloorReadout() {
    if (!m_mixer || !m_floorReadout) return;
    const double floorDb = m_mixer->estimatedNoiseFloorDb(currentTrackId());
    m_floorReadout->setText(tr("%1 dB").arg(floorDb, 0, 'f', 1));
}

void NoiseReductionPanel::emitChange() {
    if (m_suppressEmit) return;
    emit noiseReductionChanged(currentTrackId(), collectSettings());
}
