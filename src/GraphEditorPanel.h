#pragma once

#include <QColor>
#include <QDockWidget>
#include <QPointer>
#include <QString>
#include <QVector>
#include "Keyframe.h"

class GraphEditorCurveView;
class QComboBox;
class QLabel;
class QListWidget;
class Timeline;

struct GraphEditorCurveTrack {
    QString propertyName;
    QString displayName;
    KeyframeTrack track;
    QColor color;
};

class GraphEditorPanel : public QDockWidget
{
    Q_OBJECT

public:
    explicit GraphEditorPanel(QWidget *parent = nullptr);

    void setTimeline(Timeline *timeline);

public slots:
    void setSelectedClip(int trackIdx, int clipIdx);
    void setPlayheadSeconds(double seconds);
    void refreshFromTimeline();

private:
    void rebuildForSelection();
    void showEmptyState(const QString &message);
    void syncLoopOutControl();

    QPointer<Timeline> m_timeline;
    int m_trackIdx = -1;
    int m_clipIdx = -1;
    double m_clipStartSeconds = 0.0;
    double m_clipDurationSeconds = 0.0;
    double m_playheadSeconds = 0.0;
    // Preserve the user's property choice across the transient (-1, -1)
    // selection emitted while Timeline restores an undo/redo snapshot.
    QString m_selectedProperty;

    QLabel *m_statusLabel = nullptr;
    QListWidget *m_trackList = nullptr;
    QComboBox *m_loopOutCombo = nullptr;
    GraphEditorCurveView *m_curveView = nullptr;
    QVector<GraphEditorCurveTrack> m_tracks;
};
