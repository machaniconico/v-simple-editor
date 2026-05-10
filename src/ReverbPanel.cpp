#include "ReverbPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
// Dials are int-valued; we scale by these factors to map UI -> double.
// Mix       maps 0..100 (= 0.0..1.0 with /100 scale, displayed as %).
// Decay     maps 1..50  (= 0.1..5.0 sec with /10 scale).
// Pre-delay maps 0..200 ms direct.
// Damping   maps 0..100 % direct.
// Width     maps 0..100 % direct.
constexpr int kMixMin = 0;
constexpr int kMixMax = 100;
constexpr int kDecayMin = 1;       // 0.1 sec
constexpr int kDecayMax = 50;      // 5.0 sec
constexpr int kPreDelayMin = 0;
constexpr int kPreDelayMax = 200;
constexpr int kDampingMin = 0;
constexpr int kDampingMax = 100;
constexpr int kWidthMin = 0;
constexpr int kWidthMax = 100;

// Preset table — each row matches the spec's preset values.
struct Preset {
    const char *name;
    int mix;        // %
    int decay;      // x10 sec
    int preDelay;   // ms
    int damping;    // %
    int width;      // %
    bool enabled;
};
constexpr Preset kPresetHall      = { "Hall",      30, 20, 40, 20, 80, true };
constexpr Preset kPresetRoom      = { "Room",      20,  6, 10, 40, 60, true };
constexpr Preset kPresetPlate     = { "Plate",     25, 15,  5, 10, 50, true };
constexpr Preset kPresetCathedral = { "Cathedral", 40, 40, 80, 15, 90, true };
constexpr Preset kPresetOff       = { "Off",        0, 10, 20, 30, 50, false };

QDial *makeDial(int lo, int hi, int initial) {
    auto *d = new QDial();
    d->setRange(lo, hi);
    d->setValue(initial);
    d->setNotchesVisible(true);
    d->setFixedSize(60, 60);
    return d;
}
} // namespace

ReverbPanel::ReverbPanel(QWidget *parent) : QWidget(parent) {
    buildUi();
}

void ReverbPanel::setMixer(AudioMixer *mixer) {
    m_mixer = mixer;
}

void ReverbPanel::setTrackList(const QList<int> &trackIds) {
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

void ReverbPanel::loadSettings(int trackId,
                               const AudioMixer::ReverbSettings &r) {
    int idx = m_trackCombo->findData(trackId);
    if (idx < 0) idx = 0;
    m_suppressEmit = true;
    m_trackCombo->setCurrentIndex(idx);
    applySettingsToControls(r);
    m_suppressEmit = false;
}

int ReverbPanel::currentTrackId() const {
    if (!m_trackCombo || m_trackCombo->count() == 0) return 0;
    return m_trackCombo->currentData().toInt();
}

void ReverbPanel::buildUi() {
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

    // Dials grid — 5 columns.
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
    addColumn(0, tr("Mix"),       m_mix,      m_mixLabel,
              kMixMin, kMixMax, 0);
    addColumn(1, tr("Decay"),     m_decay,    m_decayLabel,
              kDecayMin, kDecayMax, 10);
    addColumn(2, tr("Pre-delay"), m_preDelay, m_preDelayLabel,
              kPreDelayMin, kPreDelayMax, 20);
    addColumn(3, tr("Damping"),   m_damping,  m_dampingLabel,
              kDampingMin, kDampingMax, 30);
    addColumn(4, tr("Width"),     m_width,    m_widthLabel,
              kWidthMin, kWidthMax, 50);
    root->addLayout(grid);

    // Preset row.
    auto *presets = new QHBoxLayout();
    presets->setSpacing(4);
    presets->addWidget(new QLabel(tr("Preset:")));
    auto makePreset = [&](const QString &name) {
        auto *btn = new QPushButton(name);
        btn->setMinimumWidth(70);
        connect(btn, &QPushButton::clicked,
                this, &ReverbPanel::onPresetClicked);
        presets->addWidget(btn);
        return btn;
    };
    m_presetHall      = makePreset(tr("Hall"));
    m_presetRoom      = makePreset(tr("Room"));
    m_presetPlate     = makePreset(tr("Plate"));
    m_presetCathedral = makePreset(tr("Cathedral"));
    m_presetOff       = makePreset(tr("Off"));
    presets->addStretch(1);
    root->addLayout(presets);

    // Wire signals — every change collects + emits.
    auto connectDial = [&](QDial *d) {
        connect(d, &QDial::valueChanged,
                this, &ReverbPanel::onAnyControlChanged);
    };
    connectDial(m_mix);
    connectDial(m_decay);
    connectDial(m_preDelay);
    connectDial(m_damping);
    connectDial(m_width);
    connect(m_enable, &QCheckBox::toggled,
            this, &ReverbPanel::onAnyControlChanged);
    connect(m_trackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ReverbPanel::onTrackChanged);

    onAnyControlChanged(); // refresh labels (no emit due to initial state)
}

AudioMixer::ReverbSettings ReverbPanel::collectSettings() const {
    AudioMixer::ReverbSettings r;
    r.mixRatio     = m_mix->value() / 100.0;
    r.decaySeconds = m_decay->value() / 10.0;
    r.preDelayMs   = static_cast<double>(m_preDelay->value());
    r.dampingHF    = static_cast<double>(m_damping->value());
    r.widthPercent = static_cast<double>(m_width->value());
    r.enabled      = m_enable->isChecked();
    return r;
}

void ReverbPanel::applySettingsToControls(
        const AudioMixer::ReverbSettings &r) {
    m_mix->setValue(qBound(kMixMin,
        static_cast<int>(qRound(r.mixRatio * 100.0)), kMixMax));
    m_decay->setValue(qBound(kDecayMin,
        static_cast<int>(qRound(r.decaySeconds * 10.0)), kDecayMax));
    m_preDelay->setValue(qBound(kPreDelayMin,
        static_cast<int>(qRound(r.preDelayMs)), kPreDelayMax));
    m_damping->setValue(qBound(kDampingMin,
        static_cast<int>(qRound(r.dampingHF)), kDampingMax));
    m_width->setValue(qBound(kWidthMin,
        static_cast<int>(qRound(r.widthPercent)), kWidthMax));
    m_enable->setChecked(r.enabled);
    onAnyControlChanged();
}

void ReverbPanel::onAnyControlChanged() {
    const auto r = collectSettings();
    if (m_mixLabel)
        m_mixLabel->setText(tr("%1 %").arg(r.mixRatio * 100.0, 0, 'f', 0));
    if (m_decayLabel)
        m_decayLabel->setText(tr("%1 s").arg(r.decaySeconds, 0, 'f', 1));
    if (m_preDelayLabel)
        m_preDelayLabel->setText(tr("%1 ms").arg(r.preDelayMs, 0, 'f', 0));
    if (m_dampingLabel)
        m_dampingLabel->setText(tr("%1 %").arg(r.dampingHF, 0, 'f', 0));
    if (m_widthLabel)
        m_widthLabel->setText(tr("%1 %").arg(r.widthPercent, 0, 'f', 0));
    emitChange();
}

void ReverbPanel::onTrackChanged(int /*idx*/) {
    if (m_suppressEmit) return;
    if (m_mixer) {
        const auto r = m_mixer->reverbForTrack(currentTrackId());
        m_suppressEmit = true;
        applySettingsToControls(r);
        m_suppressEmit = false;
    }
}

void ReverbPanel::onPresetClicked() {
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (!btn) return;
    const Preset *p = nullptr;
    if      (btn == m_presetHall)      p = &kPresetHall;
    else if (btn == m_presetRoom)      p = &kPresetRoom;
    else if (btn == m_presetPlate)     p = &kPresetPlate;
    else if (btn == m_presetCathedral) p = &kPresetCathedral;
    else if (btn == m_presetOff)       p = &kPresetOff;
    if (!p) return;

    m_suppressEmit = true;
    m_mix->setValue(p->mix);
    m_decay->setValue(p->decay);
    m_preDelay->setValue(p->preDelay);
    m_damping->setValue(p->damping);
    m_width->setValue(p->width);
    m_enable->setChecked(p->enabled);
    m_suppressEmit = false;
    onAnyControlChanged();
}

void ReverbPanel::emitChange() {
    if (m_suppressEmit) return;
    emit reverbChanged(currentTrackId(), collectSettings());
}
