#pragma once

#include <QDockWidget>
#include <QPointer>
#include <QVector>

class AudioMixer;
class Timeline;
class TimelineTrack;
class QLabel;
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
    quint64 gainWriteCountForTest() const { return m_gainWrites; }

private:
    struct Strip {
        QPointer<TimelineTrack> track;
        QLabel *name = nullptr;
        QLabel *db = nullptr;
        QSlider *fader = nullptr;
        QPushButton *mute = nullptr;
        QPushButton *solo = nullptr;
        double startGain = 1.0;
        bool dragging = false;
    };
    void rebuild();
    void finishGesture(int index);
    QPointer<Timeline> m_timeline;
    QPointer<AudioMixer> m_mixer;
    QVector<Strip> m_strips;
    QTimer *m_refresh = nullptr;
    quint64 m_gainWrites = 0;
};
