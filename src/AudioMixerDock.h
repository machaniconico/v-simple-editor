#pragma once

#include <QDockWidget>
#include <QPointer>
#include <QVector>

namespace audiobus { class AudioBusRouting; }
class AudioMixer;
class Timeline;
class TimelineTrack;
class QLabel;
class QComboBox;
class QSlider;
class QPushButton;
class QTimer;

// UI only: all gain and playback flags remain owned by the existing engine.
class AudioMixerDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit AudioMixerDock(Timeline *timeline, AudioMixer *mixer, QWidget *parent = nullptr);
    int stripCount() const { return int(m_strips.size()); }
    void synchronize();
    // Read-only view of the bus list and per-track assignment; MainWindow owns it.
    void setBusRouting(const audiobus::AudioBusRouting *routing);
    // Counts every pan / gain write and bus request issued from this Dock's controls.
    quint64 gainWriteCountForTest() const { return m_gainWrites; }

signals:
    void trackBusRequested(int track, int busId);

private:
    struct Strip {
        QPointer<TimelineTrack> track;
        QLabel *name = nullptr;
        QLabel *db = nullptr;
        QSlider *fader = nullptr;
        QPushButton *mute = nullptr;
        QPushButton *solo = nullptr;
        QLabel *panLabel = nullptr;
        QSlider *pan = nullptr;
        QComboBox *bus = nullptr;
        double startGain = 1.0;
        double startPan = 0.0;
        bool dragging = false;
        bool panDragging = false;
    };
    void rebuild();
    void finishGesture(int index);
    void refreshBusCombo(QComboBox *combo, int trackIndex);
    QPointer<Timeline> m_timeline;
    QPointer<AudioMixer> m_mixer;
    const audiobus::AudioBusRouting *m_busRouting = nullptr;
    QVector<Strip> m_strips;
    QTimer *m_refresh = nullptr;
    quint64 m_gainWrites = 0;
};
