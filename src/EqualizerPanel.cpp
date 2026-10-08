#include "EqualizerPanel.h"

#include <QComboBox>
#include <QSlider>
#include <QDial>
#include <QLabel>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <vector>

#include <cmath>

namespace {
constexpr int kFreqDialMin = 0;
constexpr int kFreqDialMax = 1000;
constexpr int kQDialMin = 0;
constexpr int kQDialMax = 1000;
constexpr int kGainMin = -18;
constexpr int kGainMax = 18;
constexpr double kFreqLogMin = 20.0;
constexpr double kFreqLogMax = 20000.0;
constexpr double kQLogMin = 0.1;
constexpr double kQLogMax = 10.0;
} // namespace

int EqualizerPanel::hzToDial(double hz) {
    if (hz <= kFreqLogMin) return kFreqDialMin;
    if (hz >= kFreqLogMax) return kFreqDialMax;
    const double r = std::log(hz / kFreqLogMin) / std::log(kFreqLogMax / kFreqLogMin);
    return static_cast<int>(std::round(r * kFreqDialMax));
}

double EqualizerPanel::dialToHz(int dialVal) {
    const double r = static_cast<double>(dialVal) / kFreqDialMax;
    return kFreqLogMin * std::pow(kFreqLogMax / kFreqLogMin, r);
}

int EqualizerPanel::qToDial(double q) {
    if (q <= kQLogMin) return kQDialMin;
    if (q >= kQLogMax) return kQDialMax;
    const double r = std::log(q / kQLogMin) / std::log(kQLogMax / kQLogMin);
    return static_cast<int>(std::round(r * kQDialMax));
}

double EqualizerPanel::dialToQ(int dialVal) {
    const double r = static_cast<double>(dialVal) / kQDialMax;
    return kQLogMin * std::pow(kQLogMax / kQLogMin, r);
}

EqualizerPanel::EqualizerPanel(QWidget *parent) : QWidget(parent) {
    buildUi();
}

EqualizerPanel::BandWidgets EqualizerPanel::buildBand(const QString &title,
                                                      const QString &key,
                                                      double freqDefault,
                                                      bool peaking) {
    BandWidgets w;
    w.box = new QGroupBox(title, this);
    auto *layout = new QGridLayout(w.box);

    // Gain slider (vertical, -18..+18 dB).
    w.gain = new QSlider(Qt::Vertical, w.box);
    w.gain->setRange(kGainMin, kGainMax);
    w.gain->setValue(0);
    w.gain->setTickPosition(QSlider::TicksRight);
    w.gain->setTickInterval(6);
    w.gain->setObjectName(QStringLiteral("eq_%1_gain").arg(key));
    w.gainLabel = new QLabel(QStringLiteral("0 dB"), w.box);
    w.gainLabel->setAlignment(Qt::AlignCenter);

    // Freq dial (logarithmic).
    w.freq = new QDial(w.box);
    w.freq->setRange(kFreqDialMin, kFreqDialMax);
    w.freq->setValue(hzToDial(freqDefault));
    w.freq->setNotchesVisible(true);
    w.freq->setObjectName(QStringLiteral("eq_%1_freq").arg(key));
    w.freqLabel = new QLabel(w.box);
    w.freqLabel->setAlignment(Qt::AlignCenter);

    // Q dial (peaking only — shelves use a fixed Q).
    if (peaking) {
        w.q = new QDial(w.box);
        w.q->setRange(kQDialMin, kQDialMax);
        w.q->setValue(qToDial(1.0));
        w.q->setNotchesVisible(true);
        w.q->setObjectName(QStringLiteral("eq_%1_q").arg(key));
        w.qLabel = new QLabel(w.box);
        w.qLabel->setAlignment(Qt::AlignCenter);
    }

    layout->addWidget(new QLabel(QStringLiteral("Gain"), w.box), 0, 0, Qt::AlignCenter);
    layout->addWidget(w.gain, 1, 0, Qt::AlignHCenter);
    layout->addWidget(w.gainLabel, 2, 0, Qt::AlignCenter);

    layout->addWidget(new QLabel(QStringLiteral("Freq"), w.box), 3, 0, Qt::AlignCenter);
    layout->addWidget(w.freq, 4, 0, Qt::AlignHCenter);
    layout->addWidget(w.freqLabel, 5, 0, Qt::AlignCenter);

    if (peaking) {
        layout->addWidget(new QLabel(QStringLiteral("Q"), w.box), 6, 0, Qt::AlignCenter);
        layout->addWidget(w.q, 7, 0, Qt::AlignHCenter);
        layout->addWidget(w.qLabel, 8, 0, Qt::AlignCenter);
    }

    // Wire change signals.
    connect(w.gain, &QSlider::valueChanged, this, &EqualizerPanel::onAnyControlChanged);
    connect(w.freq, &QDial::valueChanged, this, &EqualizerPanel::onAnyControlChanged);
    if (peaking) {
        connect(w.q, &QDial::valueChanged, this, &EqualizerPanel::onAnyControlChanged);
    }
    // Undo gestures: a drag ends on release; wheel / keyboard steps are complete
    // on their own (N wheel notches = N gestures).
    for (QAbstractSlider *control : {static_cast<QAbstractSlider *>(w.gain),
                                     static_cast<QAbstractSlider *>(w.freq),
                                     static_cast<QAbstractSlider *>(w.q)}) {
        if (!control) continue;
        connect(control, &QAbstractSlider::valueChanged, this, [this, control] {
            if (!control->isSliderDown()) finishGesture();
        });
        connect(control, &QAbstractSlider::sliderReleased, this, &EqualizerPanel::finishGesture);
    }
    return w;
}

void EqualizerPanel::buildUi() {
    auto *root = new QVBoxLayout(this);

    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel(QStringLiteral("Track:"), this));
    m_trackCombo = new QComboBox(this);
    m_trackCombo->setObjectName(QStringLiteral("eq_track"));
    m_trackCombo->addItem(QStringLiteral("Master"), QVariant(0));
    top->addWidget(m_trackCombo, /*stretch=*/1);
    connect(m_trackCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &EqualizerPanel::onTrackSelectionChanged);
    root->addLayout(top);

    auto *bands = new QHBoxLayout();
    m_low     = buildBand(QStringLiteral("Low (Shelf)"),     QStringLiteral("low"),     80.0,    /*peaking=*/false);
    m_lowMid  = buildBand(QStringLiteral("Low-Mid"),         QStringLiteral("lowmid"),  250.0,   /*peaking=*/true);
    m_highMid = buildBand(QStringLiteral("High-Mid"),        QStringLiteral("highmid"), 3000.0,  /*peaking=*/true);
    m_high    = buildBand(QStringLiteral("High (Shelf)"),    QStringLiteral("high"),    10000.0, /*peaking=*/false);
    bands->addWidget(m_low.box);
    bands->addWidget(m_lowMid.box);
    bands->addWidget(m_highMid.box);
    bands->addWidget(m_high.box);
    root->addLayout(bands);

    // Initialize labels by reading the default state.
    onAnyControlChanged();
    m_gestureDirty = false; // the label refresh above is not a user gesture
    // Seed master settings cache so currentSettings() returns sensible defaults.
    m_perTrackSettings.insert(0, AudioMixer::EqSettings{});
    m_currentTrackId = 0;
}

void EqualizerPanel::setTracks(const QStringList &itemNames, const QList<int> &trackIds) {
    QSignalBlocker block(m_trackCombo);
    m_trackCombo->clear();
    const int n = qMin(itemNames.size(), trackIds.size());
    for (int i = 0; i < n; ++i) {
        m_trackCombo->addItem(itemNames[i], QVariant(trackIds[i]));
        if (!m_perTrackSettings.contains(trackIds[i]))
            m_perTrackSettings.insert(trackIds[i], AudioMixer::EqSettings{});
    }
    if (m_trackCombo->count() > 0) {
        m_trackCombo->setCurrentIndex(0);
        m_currentTrackId = m_trackCombo->itemData(0).toInt();
        // Reseeding never emits: the panel guards itself, not only its host.
        QScopedValueRollback<bool> suppress(m_suppressSignals, true);
        const auto blockers = blockBandSignals();
        applyBandToWidgets(m_low,
                           m_perTrackSettings[m_currentTrackId].low, false);
        applyBandToWidgets(m_lowMid,
                           m_perTrackSettings[m_currentTrackId].lowMid, true);
        applyBandToWidgets(m_highMid,
                           m_perTrackSettings[m_currentTrackId].highMid, true);
        applyBandToWidgets(m_high,
                           m_perTrackSettings[m_currentTrackId].high, false);
        onAnyControlChanged();
    }
}

void EqualizerPanel::setEqSettings(int trackId, const AudioMixer::EqSettings &eq) {
    m_perTrackSettings.insert(trackId, eq);
    if (trackId == m_currentTrackId) {
        // Apply without emitting eqChanged.
        QScopedValueRollback<bool> suppress(m_suppressSignals, true);
        {
            const auto blockers = blockBandSignals();
            applyBandToWidgets(m_low,     eq.low,     false);
            applyBandToWidgets(m_lowMid,  eq.lowMid,  true);
            applyBandToWidgets(m_highMid, eq.highMid, true);
            applyBandToWidgets(m_high,    eq.high,    false);
        }
        // Refresh labels (these depend on widget values, not signals); still
        // suppressed so the refresh itself never reports a user edit.
        onAnyControlChanged();
    }
}

void EqualizerPanel::onTrackSelectionChanged(int index) {
    if (index < 0 || index >= m_trackCombo->count()) return;
    const int trackId = m_trackCombo->itemData(index).toInt();
    m_currentTrackId = trackId;
    if (!m_perTrackSettings.contains(trackId))
        m_perTrackSettings.insert(trackId, AudioMixer::EqSettings{});
    setEqSettings(trackId, m_perTrackSettings[trackId]);
}

void EqualizerPanel::onAnyControlChanged() {
    AudioMixer::EqSettings eq;
    eq.low     = readBandFromWidgets(m_low,     /*defaultQ=*/0.7, false);
    eq.lowMid  = readBandFromWidgets(m_lowMid,  /*defaultQ=*/1.0, true);
    eq.highMid = readBandFromWidgets(m_highMid, /*defaultQ=*/1.0, true);
    eq.high    = readBandFromWidgets(m_high,    /*defaultQ=*/0.7, false);

    // Update label text for each band.
    auto fmtBand = [](const BandWidgets &w, const AudioMixer::EqBand &b, bool peaking) {
        if (w.gainLabel) {
            w.gainLabel->setText(QStringLiteral("%1 dB").arg(static_cast<int>(b.gainDb)));
        }
        if (w.freqLabel) {
            if (b.freq >= 1000.0)
                w.freqLabel->setText(QStringLiteral("%1 kHz").arg(b.freq / 1000.0, 0, 'f', 1));
            else
                w.freqLabel->setText(QStringLiteral("%1 Hz").arg(static_cast<int>(b.freq)));
        }
        if (peaking && w.qLabel) {
            w.qLabel->setText(QStringLiteral("Q %1").arg(b.q, 0, 'f', 2));
        }
    };
    fmtBand(m_low,     eq.low,     false);
    fmtBand(m_lowMid,  eq.lowMid,  true);
    fmtBand(m_highMid, eq.highMid, true);
    fmtBand(m_high,    eq.high,    false);

    m_perTrackSettings.insert(m_currentTrackId, eq);
    if (!m_suppressSignals) {
        ++m_emitCount;
        m_gestureDirty = true;
        emit eqChanged(m_currentTrackId, eq);
    }
}

void EqualizerPanel::finishGesture() {
    if (!m_gestureDirty || m_suppressSignals) return;
    m_gestureDirty = false;
    emit gestureFinished(m_currentTrackId);
}

std::vector<QSignalBlocker> EqualizerPanel::blockBandSignals() {
    // QSignalBlocker restores each control's previous blocked state on exit,
    // so a caller that blocked a control first keeps its protection.
    std::vector<QSignalBlocker> blockers;
    for (const BandWidgets *w : {&m_low, &m_lowMid, &m_highMid, &m_high})
        for (QObject *o : {static_cast<QObject *>(w->gain), static_cast<QObject *>(w->freq),
                           static_cast<QObject *>(w->q)})
            if (o) blockers.emplace_back(o);
    return blockers;
}

void EqualizerPanel::applyBandToWidgets(const BandWidgets &w,
                                        const AudioMixer::EqBand &band,
                                        bool peaking) {
    if (w.gain) w.gain->setValue(qBound(kGainMin, static_cast<int>(std::round(band.gainDb)), kGainMax));
    if (w.freq) w.freq->setValue(hzToDial(band.freq));
    if (peaking && w.q) w.q->setValue(qToDial(band.q));
}

AudioMixer::EqBand EqualizerPanel::readBandFromWidgets(const BandWidgets &w,
                                                       double defaultQ,
                                                       bool peaking) const {
    AudioMixer::EqBand b;
    b.gainDb = w.gain ? static_cast<double>(w.gain->value()) : 0.0;
    b.freq = w.freq ? dialToHz(w.freq->value()) : 1000.0;
    b.q = (peaking && w.q) ? dialToQ(w.q->value()) : defaultQ;
    b.enabled = true;
    return b;
}

int EqualizerPanel::currentTrackId() const {
    return m_currentTrackId;
}

AudioMixer::EqSettings EqualizerPanel::currentSettings() const {
    return m_perTrackSettings.value(m_currentTrackId, AudioMixer::EqSettings{});
}
