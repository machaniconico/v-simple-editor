#pragma once

#include "Keyframe.h"

#include <QDialog>
#include <QString>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class Timeline;

namespace aepro {

// --- After Effects Speed Graph (|dv/dt| velocity analysis) ---

struct SpeedSample {
    double timeSec = 0.0;
    double value = 0.0;
    double speed = 0.0; // |dv/dt| in units/sec
};

struct SpeedCurveSummary {
    QString propertyName;
    QVector<SpeedSample> samples;
    QVector<double> keyframeTimes;
    double startSec = 0.0;
    double endSec = 0.0;
    double maxSpeed = 0.0;
    double avgSpeed = 0.0;
    double peakTimeSec = 0.0;
};

SpeedCurveSummary sampleSpeedCurve(const KeyframeTrack &track,
                                   double startSec,
                                   double endSec,
                                   int sampleCount = 64);

class SpeedGraphWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SpeedGraphWidget(QWidget *parent = nullptr);

    void setSummary(const SpeedCurveSummary &summary);
    const SpeedCurveSummary &summary() const { return m_summary; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    SpeedCurveSummary m_summary;
};

class SpeedGraphDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SpeedGraphDialog(const KeyframeManager &manager,
                              const QString &clipLabel,
                              double clipDurationSec,
                              QWidget *parent = nullptr);

    SpeedCurveSummary currentSummary() const;
    int trackCount() const;

private:
    void selectTrackIndex(int index);

    QVector<KeyframeTrack> m_tracks;
    double m_clipDurationSec = 1.0;
    QComboBox *m_trackCombo = nullptr;
    QLabel *m_statsLabel = nullptr;
    SpeedGraphWidget *m_graph = nullptr;
};

// --- After Effects Shy Layers (UI clutter filter; always rendered!) ---

bool isTrackShy(bool isAudio, int trackIdx);
void setTrackShy(bool isAudio, int trackIdx, bool shy);
bool isShyFilterEnabled();
void setShyFilterEnabled(bool enabled);
void clearShyState();

// Returns indices of tracks visible in the UI list. When shy filter is ON,
// shy tracks are omitted from the UI list while remaining rendered & exported.
QVector<int> visibleTrackIndices(int trackCount, bool isAudio);

class ShyLayersDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ShyLayersDialog(Timeline *timeline, QWidget *parent = nullptr);

    int visibleVideoTrackCount() const;
    int visibleAudioTrackCount() const;

private:
    void populate();
    void updateSummary();
    void saveAndAccept();

    Timeline *m_timeline = nullptr;
    QCheckBox *m_masterFilterCheck = nullptr;
    QListWidget *m_trackList = nullptr;
    QLabel *m_summaryLabel = nullptr;
};

} // namespace aepro
