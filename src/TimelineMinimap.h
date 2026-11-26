#pragma once

#include <QColor>
#include <QDialog>
#include <QRectF>
#include <QVector>
#include <QWidget>

class Timeline;

namespace minimap {

struct ClipBar {
    bool isAudio = false;
    int trackRow = 0;
    double startSec = 0.0;
    double endSec = 0.0;
    QColor color;
};

struct OverviewSnapshot {
    double totalDurationSec = 0.0;
    double playheadSec = 0.0;
    int totalRows = 0;
    QVector<ClipBar> bars;
    QVector<double> markerTimesSec;
};

// Pure geometry snapshot builder (DaVinci Resolve Cut Page Dual-Timeline Overview bar).
OverviewSnapshot buildOverviewSnapshot(const Timeline *timeline);

// Map normalized x in [0..1] to timeline seconds and vice versa.
double xToTimelineSec(double x, double widgetWidth, double totalDurationSec);
double timelineSecToX(double sec, double widgetWidth, double totalDurationSec);

// Interactive minimap widget displaying the full timeline overview, markers, playhead,
// and a draggable viewport highlight window.
class TimelineMinimapWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineMinimapWidget(Timeline *timeline, QWidget *parent = nullptr);

    void refreshSnapshot();
    void setViewportRange(double startSec, double endSec);
    double viewportStartSec() const { return m_viewStartSec; }
    double viewportEndSec() const { return m_viewEndSec; }
    const OverviewSnapshot &snapshot() const { return m_snap; }

signals:
    void seekRequested(double timelineSec);
    void viewportDragged(double newStartSec);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    Timeline *m_timeline = nullptr;
    OverviewSnapshot m_snap;
    double m_viewStartSec = 0.0;
    double m_viewEndSec = 10.0;
    bool m_dragging = false;
};

// Standalone overview dialog so users can pop out the Dual-Timeline Minimap anytime.
class TimelineMinimapDialog : public QDialog {
    Q_OBJECT
public:
    explicit TimelineMinimapDialog(Timeline *timeline, QWidget *parent = nullptr);
    TimelineMinimapWidget *minimap() const { return m_widget; }

private:
    TimelineMinimapWidget *m_widget = nullptr;
};

} // namespace minimap
