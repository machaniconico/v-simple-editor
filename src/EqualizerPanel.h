#pragma once

#include <QWidget>

#include "AudioMixer.h"

class QComboBox;
class QSlider;
class QDial;
class QLabel;
class QGroupBox;

// EqualizerPanel — 4-band parametric EQ UI (Premiere/Audition parity).
//
// Bands: low shelf / low-mid peak / high-mid peak / high shelf.
// Per band: gain slider (-18..+18 dB), freq dial (log 20..20000 Hz), Q dial
// (0.1..10). A QComboBox selects the track being edited (master + each
// audio track). On any control change the panel emits eqChanged with the
// trackId and full EqSettings; the consumer (MainWindow) is expected to
// forward this to AudioMixer::setEqForTrack. Wiring of the menu/dock is
// deferred — this widget is self-contained and can be hosted anywhere.
class EqualizerPanel : public QWidget {
    Q_OBJECT
public:
    explicit EqualizerPanel(QWidget *parent = nullptr);
    ~EqualizerPanel() override = default;

    // Configure the track-selector dropdown. trackIds matches itemNames 1:1.
    // trackIds[i] is the int passed to AudioMixer::setEqForTrack when the
    // user picks itemNames[i]. Convention: 0 = Master, 1 = A1, 2 = A2, ...
    void setTracks(const QStringList &itemNames, const QList<int> &trackIds);

    // Replace the current panel state with the supplied EqSettings without
    // emitting eqChanged. Use after setTracks to seed the UI from existing
    // AudioMixer state.
    void setEqSettings(int trackId, const AudioMixer::EqSettings &eq);

    int currentTrackId() const;
    AudioMixer::EqSettings currentSettings() const;

signals:
    void eqChanged(int trackId, AudioMixer::EqSettings eq);

private slots:
    void onTrackSelectionChanged(int index);
    void onAnyControlChanged();

private:
    struct BandWidgets {
        QGroupBox *box = nullptr;
        QSlider *gain = nullptr;
        QDial *freq = nullptr;
        QDial *q = nullptr;
        QLabel *gainLabel = nullptr;
        QLabel *freqLabel = nullptr;
        QLabel *qLabel = nullptr;
    };

    void buildUi();
    BandWidgets buildBand(const QString &title,
                          double freqDefault,
                          bool peaking); // false = shelf, no Q control
    void emitChange();
    void blockBandSignals(bool block);
    void applyBandToWidgets(const BandWidgets &w,
                            const AudioMixer::EqBand &band,
                            bool peaking);
    AudioMixer::EqBand readBandFromWidgets(const BandWidgets &w,
                                           double defaultQ,
                                           bool peaking) const;

    // Logarithmic mapping helpers for the freq dial (dial value 0..1000 -> Hz).
    static int hzToDial(double hz);
    static double dialToHz(int dialVal);

    // Logarithmic mapping helpers for the Q dial (dial value 0..1000 -> Q).
    static int qToDial(double q);
    static double dialToQ(int dialVal);

    QComboBox *m_trackCombo = nullptr;
    BandWidgets m_low;
    BandWidgets m_lowMid;
    BandWidgets m_highMid;
    BandWidgets m_high;

    // Per-track snapshot so the user can switch tracks without losing edits
    // while a host widget delays applying the changes upstream.
    QHash<int, AudioMixer::EqSettings> m_perTrackSettings;
    int m_currentTrackId = 0;
    bool m_suppressSignals = false;
};
