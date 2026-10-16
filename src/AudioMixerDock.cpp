#include "AudioMixerDock.h"
#include "AudioMeterWidget.h"
#include "AudioBusRouting.h"
#include "AudioMixer.h"
#include "Timeline.h"
#include "UndoManager.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace {
constexpr int kSilence = -601;
double gainForPosition(int value)
{
    return value == kSilence ? 0.0 : std::pow(10.0, value / 200.0);
}
int positionForGain(double gain)
{
    return gain <= 0.0 ? kSilence : qBound(-600, qRound(200.0 * std::log10(gain)), 120);
}
constexpr int kPanSnap = 4;
QString panLabel(int value)
{
    return value == 0 ? QStringLiteral("C")
                      : QStringLiteral("%1%2").arg(value < 0 ? 'L' : 'R').arg(qAbs(value));
}
QString gainLabel(double gain)
{
    return gain <= 0.0 ? QStringLiteral("−∞ dB")
                       : QStringLiteral("%1 dB").arg(20.0 * std::log10(gain), 0, 'f', 1);
}
}

AudioMixerDock::AudioMixerDock(Timeline *timeline, AudioMixer *mixer, QWidget *parent)
    : QDockWidget(QStringLiteral("オーディオミキサー"), parent),
      m_timeline(timeline), m_mixer(mixer), m_refresh(new QTimer(this))
{
    setObjectName(QStringLiteral("AudioMixerDock"));
    hide();
    // Track appearance and externally changed gains have no dedicated signal.
    // Poll only while visible; signals handle structural and flag changes too.
    m_refresh->setInterval(50);
    connect(m_refresh, &QTimer::timeout, this, &AudioMixerDock::synchronize);
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) { synchronize(); m_refresh->start(); }
        else {
            m_refresh->stop();
            for (int i = 0; i < m_strips.size(); ++i) finishGesture(i);
        }
    });
    if (timeline) {
        connect(timeline, &Timeline::audioSequenceChanged, this, &AudioMixerDock::synchronize);
        connect(timeline, &Timeline::trackSoloChanged, this, &AudioMixerDock::synchronize);
        connect(timeline, &Timeline::trackStateRestored, this, &AudioMixerDock::synchronize);
        // Queued: let MainWindow remap the external mixer state first.
        connect(timeline, &Timeline::trackIndicesRemapped, this,
                &AudioMixerDock::synchronize, Qt::QueuedConnection);
        connect(timeline->undoManager(), &UndoManager::historyChanged, this,
                &AudioMixerDock::synchronize, Qt::QueuedConnection);
    }
    synchronize();
}

void AudioMixerDock::synchronize()
{
    if (!m_timeline || !m_mixer) return;
    const auto &tracks = m_timeline->audioTracks();
    bool changed = tracks.size() != m_strips.size();
    for (int i = 0; !changed && i < tracks.size(); ++i)
        changed = m_strips[i].track != tracks[i];
    if (changed) rebuild();
    for (int i = 0; i < m_strips.size(); ++i) {
        auto &strip = m_strips[i];
        strip.name->setText(strip.track->customName.isEmpty()
            ? QStringLiteral("A%1").arg(i + 1) : strip.track->customName);
        const QSignalBlocker muteBlock(strip.mute), soloBlock(strip.solo), faderBlock(strip.fader);
        strip.mute->setChecked(strip.track->isMuted());
        strip.solo->setChecked(strip.track->isSolo());
        const double gain = m_mixer->trackGain(i);
        if (!strip.dragging) strip.fader->setValue(positionForGain(gain));
        strip.db->setText(gainLabel(gain));
        const QSignalBlocker panBlock(strip.pan), busBlock(strip.bus);
        const int panPosition = qBound(-100, qRound(m_mixer->trackPan(i) * 100.0), 100);
        if (!strip.panDragging) strip.pan->setValue(panPosition);
        strip.panLabel->setText(panLabel(strip.pan->value()));
        refreshBusCombo(strip.bus, i);
    }
}

void AudioMixerDock::setBusRouting(const audiobus::AudioBusRouting *routing)
{
    m_busRouting = routing;
    synchronize();
}

void AudioMixerDock::refreshBusCombo(QComboBox *combo, int trackIndex)
{
    // Caller blocks the combo's signals.
    const auto buses = m_busRouting ? m_busRouting->buses() : QVector<audiobus::AudioBus>();
    bool same = combo->count() == buses.size() + 1;
    for (int b = 0; same && b < buses.size(); ++b)
        same = combo->itemData(b + 1).toInt() == buses[b].id
            && combo->itemText(b + 1) == buses[b].name;
    if (!same) {
        combo->clear();
        combo->addItem(tr("マスター"), -1);
        for (const auto &bus : buses) combo->addItem(bus.name, bus.id);
    }
    const int at = combo->findData(m_busRouting ? m_busRouting->trackBus(trackIndex) : -1);
    combo->setCurrentIndex(at < 0 ? 0 : at);
}

void AudioMixerDock::rebuild()
{
    // Old callbacks cannot address a newly reordered track.
    if (QWidget *old = widget()) { old->hide(); old->deleteLater(); }
    m_strips.clear();
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *container = new QWidget(scroll);
    auto *row = new QHBoxLayout(container);
    for (int i = 0; i < m_timeline->audioTrackCount(); ++i) {
        Strip strip;
        strip.track = m_timeline->trackAt(true, i);
        auto *channel = new QWidget(container);
        channel->setObjectName(QStringLiteral("mixerStrip%1").arg(i));
        auto *column = new QVBoxLayout(channel);
        strip.name = new QLabel(channel);
        strip.name->setObjectName(QStringLiteral("mixerName%1").arg(i));
        strip.name->setAlignment(Qt::AlignCenter);
        strip.db = new QLabel(channel);
        strip.db->setObjectName(QStringLiteral("mixerDb%1").arg(i));
        strip.db->setAlignment(Qt::AlignCenter);
        strip.fader = new QSlider(Qt::Vertical, channel);
        strip.fader->setObjectName(QStringLiteral("mixerFader%1").arg(i));
        strip.fader->setAccessibleName(QStringLiteral("トラック %1 の音量").arg(i + 1));
        strip.fader->setRange(kSilence, 120);
        strip.fader->setPageStep(30);
        strip.fader->setMinimumHeight(150);
        strip.fader->setValue(positionForGain(m_mixer->trackGain(i)));
        strip.panLabel = new QLabel(channel);
        strip.panLabel->setObjectName(QStringLiteral("mixerPanLabel%1").arg(i));
        strip.panLabel->setAlignment(Qt::AlignCenter);
        strip.pan = new QSlider(Qt::Horizontal, channel);
        strip.pan->setObjectName(QStringLiteral("mixerPan%1").arg(i));
        strip.pan->setAccessibleName(QStringLiteral("トラック %1 のパン").arg(i + 1));
        strip.pan->setRange(-100, 100);
        strip.pan->setPageStep(10);
        strip.pan->setValue(qBound(-100, qRound(m_mixer->trackPan(i) * 100.0), 100));
        strip.panLabel->setText(panLabel(strip.pan->value()));
        strip.bus = new QComboBox(channel);
        strip.bus->setObjectName(QStringLiteral("mixerBus%1").arg(i));
        strip.bus->setAccessibleName(QStringLiteral("トラック %1 の出力先バス").arg(i + 1));
        {
            const QSignalBlocker busInit(strip.bus);
            refreshBusCombo(strip.bus, i);
        }
        auto *meter = new AudioMeterWidget(channel);
        meter->setObjectName(QStringLiteral("mixerMeter%1").arg(i));
        meter->setTrackIndex(i);
        meter->setFixedWidth(26);
        // This strip exposes only the controls supported here.
        meter->setContextMenuPolicy(Qt::PreventContextMenu);
        auto *levels = new QHBoxLayout;
        levels->addWidget(strip.fader);
        levels->addWidget(meter);
        strip.mute = new QPushButton(QStringLiteral("ミュート"), channel);
        strip.solo = new QPushButton(QStringLiteral("ソロ"), channel);
        strip.mute->setObjectName(QStringLiteral("mixerMute%1").arg(i));
        strip.solo->setObjectName(QStringLiteral("mixerSolo%1").arg(i));
        strip.mute->setCheckable(true);
        strip.solo->setCheckable(true);
        column->addWidget(strip.name);
        column->addLayout(levels);
        column->addWidget(strip.db);
        column->addWidget(strip.panLabel);
        column->addWidget(strip.pan);
        column->addWidget(strip.bus);
        column->addWidget(strip.mute);
        column->addWidget(strip.solo);
        row->addWidget(channel);
        m_strips.append(strip);
        const QPointer<TimelineTrack> identity = strip.track;
        const auto valid = [this, i, identity, control = strip.fader]() {
            return m_timeline && m_mixer && identity
                && m_timeline->trackAt(true, i) == identity
                && i < m_strips.size() && m_strips[i].track == identity
                && m_strips[i].fader == control;
        };
        connect(strip.mute, &QPushButton::clicked, channel, [this, i, valid]() {
            if (valid()) { m_timeline->toggleMuteTrack(i); synchronize(); }
        });
        connect(strip.solo, &QPushButton::clicked, channel, [this, i, valid]() {
            if (valid()) { m_timeline->toggleSoloTrack(i); synchronize(); }
        });
        connect(strip.fader, &QSlider::sliderPressed, channel, [this, i, valid]() {
            if (!valid()) return;
            m_strips[i].startGain = m_mixer->trackGain(i);
            m_strips[i].dragging = true;
        });
        connect(strip.fader, &QSlider::valueChanged, channel, [this, i, valid](int value) {
            if (!valid()) return;
            auto &s = m_strips[i];
            const double before = m_mixer->trackGain(i);
            const double after = gainForPosition(value);
            if (before == after) return;
            m_mixer->setTrackGain(i, after);
            ++m_gainWrites;
            s.db->setText(gainLabel(after));
            // Keyboard, wheel and groove clicks are complete single actions.
            if (!s.dragging) m_timeline->saveUndoState(QStringLiteral("トラック音量を変更"));
        });
        connect(strip.fader, &QSlider::sliderReleased, channel, [this, i, valid]() {
            if (valid()) finishGesture(i);
        });
        const auto panValid = [this, i, valid, control = strip.pan]() {
            return valid() && m_strips[i].pan == control;
        };
        connect(strip.pan, &QSlider::sliderPressed, channel, [this, i, panValid]() {
            if (!panValid()) return;
            m_strips[i].startPan = m_mixer->trackPan(i);
            m_strips[i].panDragging = true;
        });
        connect(strip.pan, &QSlider::valueChanged, channel, [this, i, panValid](int value) {
            if (!panValid()) return;
            auto &s = m_strips[i];
            // Mouse drags stick to the centre; keyboard steps must be able to leave it.
            if (s.panDragging && value != 0 && qAbs(value) <= kPanSnap) {
                const QSignalBlocker block(s.pan);
                s.pan->setValue(0);
                value = 0;
            }
            s.panLabel->setText(panLabel(value));
            const double before = m_mixer->trackPan(i);
            const double after = value / 100.0;
            if (before == after) return;
            m_mixer->setTrackPan(i, after);
            ++m_gainWrites;
            if (!s.panDragging) m_timeline->saveUndoState(tr("トラックのパンを変更"));
        });
        connect(strip.pan, &QSlider::sliderReleased, channel, [this, i, panValid]() {
            if (panValid()) finishGesture(i);
        });
        connect(strip.bus, &QComboBox::currentIndexChanged, channel, [this, i, valid](int at) {
            if (!valid() || !m_busRouting || at < 0) return;
            const int busId = m_strips[i].bus->itemData(at).toInt();
            if (m_busRouting->trackBus(i) == busId) return;
            ++m_gainWrites;
            emit trackBusRequested(i, busId);
        });
        connect(m_mixer, &AudioMixer::levelChanged, meter,
                [meter, i](int track, float pl, float pr, float rl, float rr) {
            if (track == i) meter->setLevels(pl, pr, rl, rr);
        });
    }
    row->addStretch();
    scroll->setWidget(container);
    setWidget(scroll);
}

void AudioMixerDock::finishGesture(int index)
{
    auto &strip = m_strips[index];
    if (!m_timeline || !m_mixer) return;
    const bool sameTrack = m_timeline->trackAt(true, index) == strip.track;
    if (strip.dragging) {
        strip.dragging = false;
        if (sameTrack && strip.startGain != m_mixer->trackGain(index))
            m_timeline->saveUndoState(QStringLiteral("トラック音量を変更"));
    }
    if (strip.panDragging) {
        strip.panDragging = false;
        if (sameTrack && strip.startPan != m_mixer->trackPan(index))
            m_timeline->saveUndoState(tr("トラックのパンを変更"));
    }
}
