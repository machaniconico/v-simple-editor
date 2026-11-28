#include "TimelineMinimap.h"
#include "Timeline.h"

#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

namespace minimap {

OverviewSnapshot buildOverviewSnapshot(const Timeline *timeline)
{
    OverviewSnapshot snap;
    if (!timeline)
        return snap;

    snap.playheadSec = qMax(0.0, timeline->playheadPosition());
    int row = 0;

    auto scanTracks = [&](const QVector<TimelineTrack *> &tracks, bool isAudio) {
        for (const TimelineTrack *tr : tracks) {
            if (!tr) continue;
            double cursor = 0.0;
            for (const ClipInfo &c : tr->clips()) {
                cursor += qMax(0.0, c.leadInSec);
                const double dur = qMax(0.0, c.effectiveDuration());
                if (dur > 0.0) {
                    ClipBar bar;
                    bar.isAudio = isAudio;
                    bar.trackRow = row;
                    bar.startSec = cursor;
                    bar.endSec = cursor + dur;
                    bar.color = isAudio ? QColor(52, 211, 153, 215)
                                        : (c.isAdjustment ? QColor(192, 132, 252, 215)
                                                          : QColor(96, 165, 250, 220));
                    snap.bars.append(bar);
                    snap.totalDurationSec = qMax(snap.totalDurationSec, bar.endSec);
                }
                cursor += dur;
            }
            ++row;
        }
    };

    scanTracks(timeline->videoTracks(), false);
    scanTracks(timeline->audioTracks(), true);
    snap.totalRows = qMax(1, row);

    for (const Marker &m : timeline->markers()) {
        const double t = static_cast<double>(m.timelineUs) / 1e6;
        snap.markerTimesSec.append(t);
        snap.totalDurationSec = qMax(snap.totalDurationSec, t);
    }
    snap.totalDurationSec = qMax(snap.totalDurationSec, snap.playheadSec);
    return snap;
}

double xToTimelineSec(double x, double widgetWidth, double totalDurationSec)
{
    if (widgetWidth <= 1.0 || totalDurationSec <= 0.0)
        return 0.0;
    const double u = qBound(0.0, x / widgetWidth, 1.0);
    return u * totalDurationSec;
}

double timelineSecToX(double sec, double widgetWidth, double totalDurationSec)
{
    if (widgetWidth <= 1.0 || totalDurationSec <= 0.0)
        return 0.0;
    const double u = qBound(0.0, sec / totalDurationSec, 1.0);
    return u * widgetWidth;
}

TimelineMinimapWidget::TimelineMinimapWidget(Timeline *timeline, QWidget *parent)
    : QWidget(parent)
    , m_timeline(timeline)
{
    setObjectName(QStringLiteral("timelineMinimapWidget"));
    setMinimumHeight(56);
    setMaximumHeight(110);
    setCursor(Qt::PointingHandCursor);
    refreshSnapshot();
}

void TimelineMinimapWidget::refreshSnapshot()
{
    m_snap = buildOverviewSnapshot(m_timeline);
    update();
}

void TimelineMinimapWidget::setViewportRange(double startSec, double endSec)
{
    m_viewStartSec = qMax(0.0, startSec);
    m_viewEndSec = qMax(m_viewStartSec, endSec);
    update();
}

void TimelineMinimapWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(20, 24, 34));

    const double W = width();
    const double H = height();
    const double dur = qMax(0.1, m_snap.totalDurationSec);
    const double padY = 6.0;
    const double usableH = qMax(8.0, H - padY * 2.0);
    const double rowH = usableH / qMax(1, m_snap.totalRows);

    // Draw clip bars across entire sequence
    for (const ClipBar &b : m_snap.bars) {
        const double x0 = timelineSecToX(b.startSec, W, dur);
        const double x1 = qMax(x0 + 2.0, timelineSecToX(b.endSec, W, dur));
        const double y = padY + b.trackRow * rowH + 1.0;
        const double h = qMax(3.0, rowH - 2.0);
        p.fillRect(QRectF(x0, y, x1 - x0, h), b.color);
    }

    // Draw markers as golden ticks
    p.setPen(QPen(QColor(251, 191, 36), 1.5));
    for (double mt : m_snap.markerTimesSec) {
        const double x = timelineSecToX(mt, W, dur);
        p.drawLine(QPointF(x, 2.0), QPointF(x, H - 2.0));
    }

    // Draw active viewport highlight window
    if (m_viewEndSec > m_viewStartSec) {
        const double vx0 = timelineSecToX(m_viewStartSec, W, dur);
        const double vx1 = qMax(vx0 + 8.0, timelineSecToX(m_viewEndSec, W, dur));
        p.fillRect(QRectF(vx0, 1.0, vx1 - vx0, H - 2.0), QColor(255, 255, 255, 36));
        p.setPen(QPen(QColor(226, 232, 240, 200), 1.5));
        p.drawRect(QRectF(vx0, 1.0, vx1 - vx0, H - 2.0));
    }

    // Draw playhead line
    const double px = timelineSecToX(m_snap.playheadSec, W, dur);
    p.setPen(QPen(QColor(248, 113, 113), 2.0));
    p.drawLine(QPointF(px, 0.0), QPointF(px, H));
}

void TimelineMinimapWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    const double sec = xToTimelineSec(event->position().x(), width(), m_snap.totalDurationSec);
    m_snap.playheadSec = sec;
    update();
    emit seekRequested(sec);
}

void TimelineMinimapWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    const double sec = xToTimelineSec(event->position().x(), width(), m_snap.totalDurationSec);
    m_snap.playheadSec = sec;
    const double span = qMax(0.0, m_viewEndSec - m_viewStartSec);
    if (span > 0.0) {
        m_viewStartSec = qBound(0.0, sec - span * 0.5, qMax(0.0, m_snap.totalDurationSec - span));
        m_viewEndSec = m_viewStartSec + span;
        emit viewportDragged(m_viewStartSec);
    }
    update();
    emit seekRequested(sec);
}

void TimelineMinimapWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        m_dragging = false;
}

TimelineMinimapDialog::TimelineMinimapDialog(Timeline *timeline, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("タイムライン俯瞰ミニマップ (全体ビュー)"));
    resize(720, 150);
    auto *root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("クリックまたはドラッグでシーケンス全体の任意位置へ瞬時にシークできます:"), this));
    m_widget = new TimelineMinimapWidget(timeline, this);
    root->addWidget(m_widget);
    auto *closeBtn = new QPushButton(QStringLiteral("閉じる"), this);
    root->addWidget(closeBtn, 0, Qt::AlignRight);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}

} // namespace minimap
