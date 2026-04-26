#include "Timeline.h"
#include "UndoManager.h"

// Small strip drawn above the V/A tracks that visualizes each text
// overlay's time range as a rounded rectangle with the (truncated) overlay
// text. The strip is rebuilt from Timeline's clip[0].textManager every
// time refreshTextStrip() runs. Each bar's left/right edge is draggable
// to adjust the corresponding overlay's startTime / endTime — the Timeline
// listens to textOverlayTimeChanged and writes the new values back through
// updateTextOverlayTime.
class TextStripWidget : public QWidget {
public:
    using TimeChangeCallback = std::function<void(int, double, double)>;
    using RowHeightChangeCallback = std::function<void(int)>;
    explicit TextStripWidget(QWidget *parent = nullptr) : QWidget(parent) {
        // Caller sets the height via setFixedHeight to match Timeline's
        // m_trackHeight so the text row looks like any V/A row.
        setStyleSheet("background-color: #2b2b2b;");
        setMouseTracking(true);
    }
    void setOverlays(const QVector<EnhancedTextOverlay> &overlays, double clipDurationSec) {
        m_overlays = overlays;
        m_clipDuration = clipDurationSec;
        rebuildRowLayout();
        update();
    }
    void setPixelsPerSecond(double pps) { m_pps = pps; update(); }
    // Baseline height used when there is only one overlay (or zero) —
    // Timeline sets this to m_trackHeight so the T row matches V1/A1.
    void setSingleRowHeight(int h) {
        m_singleRowHeight = qMax(20, h);
        rebuildRowLayout();
        update();
    }
    // Called with (overlayIndex, newStartTime, newEndTime) whenever the
    // user drags one of an overlay bar's edge handles. A function
    // callback avoids Q_OBJECT + MOC plumbing for this inline widget.
    void setTimeChangeCallback(TimeChangeCallback cb) { m_timeChangeCb = std::move(cb); }
    // Called with the new row height (pixels) while the user drags the
    // row's bottom edge. Timeline resizes the strip AND its paired header.
    void setRowHeightChangeCallback(RowHeightChangeCallback cb) { m_rowHeightCb = std::move(cb); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#2b2b2b"));
        p.setPen(QColor("#555"));
        p.drawLine(0, height() - 1, width(), height() - 1);
        if (m_overlays.isEmpty() || m_pps <= 0.0)
            return;
        for (int i = 0; i < m_overlays.size(); ++i) {
            QRect r = rectForOverlay(i);
            if (r.width() <= 0) continue;
            p.setBrush(QColor(90, 130, 210, 200));
            p.setPen(QColor(140, 170, 230));
            p.drawRoundedRect(r, 3, 3);
            // Edge trim handles (brighter when hovering or dragging).
            const bool leftHot  = (m_hoverIdx == i && m_hoverEdge == -1) || (m_dragIdx == i && m_dragEdge == -1);
            const bool rightHot = (m_hoverIdx == i && m_hoverEdge == 1)  || (m_dragIdx == i && m_dragEdge == 1);
            p.fillRect(QRect(r.left(), r.top(), 3, r.height()),
                       leftHot ? QColor(255, 220, 120, 230) : QColor(255, 255, 255, 120));
            p.fillRect(QRect(r.right() - 2, r.top(), 3, r.height()),
                       rightHot ? QColor(255, 220, 120, 230) : QColor(255, 255, 255, 120));
            const auto &ov = m_overlays[i];
            // T1, T2, T3 row label — shown at the left of every overlay bar
            // so the user can immediately identify which text row this is.
            const QString rowLabel = QString("T%1").arg(m_overlayRowIdx[i] + 1);
            p.setPen(QColor(255, 220, 120));
            QFont lf = p.font();
            lf.setPointSize(qBound(8, qRound(r.height() * 0.45), 14));
            lf.setBold(true);
            p.setFont(lf);
            const QFontMetrics lfm(lf);
            const int labelW = lfm.horizontalAdvance(rowLabel) + 6;
            p.drawText(r.adjusted(4, 0, 0, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, rowLabel);
            if (!ov.text.isEmpty() && r.width() > 14 + labelW) {
                p.setPen(Qt::white);
                QFont f = p.font();
                const int pt = qBound(8, qRound(r.height() * 0.42), 14);
                f.setPointSize(pt);
                f.setBold(false);
                p.setFont(f);
                const QString elided = QFontMetrics(f)
                    .elidedText(ov.text, Qt::ElideRight, r.width() - labelW - 6);
                p.drawText(r.adjusted(4 + labelW, 0, -4, 0),
                           Qt::AlignVCenter | Qt::AlignLeft, elided);
            }
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        // Row-height drag has priority — it doesn't depend on m_pps.
        if (m_rowHeightDragging) {
            const int newH = qBound(20, m_rowHeightDragStartH + (event->pos().y() - m_rowHeightDragStartY), 240);
            if (m_rowHeightCb)
                m_rowHeightCb(newH);
            return;
        }
        if (m_pps <= 0.0) {
            setCursor(Qt::ArrowCursor);
            return;
        }
        if (m_dragIdx >= 0 && m_dragIdx < m_overlays.size()) {
            const double tSec = qMax(0.0, event->pos().x() / m_pps);
            auto &ov = m_overlays[m_dragIdx];
            if (m_dragEdge == -1) {
                ov.startTime = qMin(tSec, ov.endTime - 0.1);
            } else if (m_dragEdge == 1) {
                ov.endTime = qMax(tSec, ov.startTime + 0.1);
            } else {
                // Body drag: translate the whole overlay preserving its
                // duration. The anchor is the press offset inside the bar
                // so the bar stays glued to the cursor.
                const double dur = qMax(0.1, m_dragOriginalEnd - m_dragOriginalStart);
                double newStart = tSec - m_dragAnchorOffsetSec;
                if (newStart < 0.0) newStart = 0.0;
                ov.startTime = newStart;
                ov.endTime = newStart + dur;
            }
            // Re-bin-pack so overlays that now overlap move to a new row.
            rebuildRowLayout();
            if (m_timeChangeCb)
                m_timeChangeCb(m_dragIdx, ov.startTime, ov.endTime);
            update();
            return;
        }
        // Bottom-edge hover (last 4 px) toggles the row-resize cursor so
        // the user sees the vertical drag affordance even with no overlays.
        if (event->pos().y() >= height() - 4) {
            setCursor(Qt::SizeVerCursor);
            return;
        }
        // Hover feedback — wider edge hit (7 px) so edges are easier to
        // grab, and SizeAllCursor over the body so the user sees it is
        // draggable as a whole.
        int hoverIdx = -1;
        int hoverEdge = 0;
        bool hoverBody = false;
        for (int i = 0; i < m_overlays.size(); ++i) {
            const QRect r = rectForOverlay(i);
            if (!r.contains(event->pos()))
                continue;
            if (qAbs(event->pos().x() - r.left()) <= 7) {
                hoverIdx = i; hoverEdge = -1; break;
            }
            if (qAbs(event->pos().x() - r.right()) <= 7) {
                hoverIdx = i; hoverEdge = 1; break;
            }
            hoverIdx = i; hoverEdge = 0; hoverBody = true; break;
        }
        if (hoverIdx != m_hoverIdx || hoverEdge != m_hoverEdge) {
            m_hoverIdx = hoverIdx;
            m_hoverEdge = hoverEdge;
            update();
        }
        if (hoverIdx < 0)
            setCursor(Qt::ArrowCursor);
        else if (hoverBody)
            setCursor(Qt::SizeAllCursor);
        else
            setCursor(Qt::SizeHorCursor);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) return;
        // Row-resize grip — bottom 4 px — starts a vertical drag that
        // changes THIS row's height independently of V/A tracks.
        if (event->pos().y() >= height() - 4) {
            m_rowHeightDragging = true;
            m_rowHeightDragStartY = event->pos().y();
            m_rowHeightDragStartH = height();
            setCursor(Qt::SizeVerCursor);
            return;
        }
        if (m_pps <= 0.0)
            return;
        for (int i = 0; i < m_overlays.size(); ++i) {
            const QRect r = rectForOverlay(i);
            if (!r.contains(event->pos())) continue;
            if (qAbs(event->pos().x() - r.left()) <= 7) {
                m_dragIdx = i; m_dragEdge = -1;
                return;
            }
            if (qAbs(event->pos().x() - r.right()) <= 7) {
                m_dragIdx = i; m_dragEdge = 1;
                return;
            }
            // Body drag — capture anchor so the bar slides with the cursor
            // without snapping its leading edge to the click point.
            m_dragIdx = i;
            m_dragEdge = 0;
            m_dragOriginalStart = m_overlays[i].startTime;
            m_dragOriginalEnd   = (m_overlays[i].endTime > 0.0)
                                    ? m_overlays[i].endTime
                                    : qMax(m_dragOriginalStart + 1.0, m_clipDuration);
            m_dragAnchorOffsetSec = (event->pos().x() / m_pps) - m_dragOriginalStart;
            return;
        }
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        m_dragIdx = -1;
        m_dragEdge = 0;
        m_rowHeightDragging = false;
        update();
    }
private:
    QRect rectForOverlay(int i) const {
        if (i < 0 || i >= m_overlays.size()) return QRect();
        const auto &ov = m_overlays[i];
        const double start = qMax(0.0, ov.startTime);
        const double end   = (ov.endTime > 0.0) ? ov.endTime
                                                : qMax(start + 1.0, m_clipDuration);
        if (end <= start) return QRect();
        const int x = static_cast<int>(start * m_pps);
        const int w = qMax(4, static_cast<int>((end - start) * m_pps));
        // Row assignment from bin-packing: overlapping overlays get stacked
        // on separate sub-rows so they don't visually overlap.
        const int rows = qMax(1, m_rowCount);
        const int slotH = qMax(8, (height() - 4) / rows);
        const int row = (i < m_overlayRowIdx.size()) ? m_overlayRowIdx[i] : 0;
        const int y = 2 + row * slotH;
        return QRect(x, y, w, slotH - 1);
    }
    // Each overlay gets its own row in insertion order (T1, T2, T3, ...)
    // so every new text input visibly adds a fresh row below the previous
    // ones — matches the user's mental model of "T1, T2, T3 grows on each
    // input" instead of bin-packing multiple non-overlapping overlays
    // into the same row. The widget height grows to fit all rows so the
    // user can see every overlay at a reasonable minimum per-row height.
    void rebuildRowLayout() {
        m_overlayRowIdx.clear();
        m_overlayRowIdx.resize(m_overlays.size());
        for (int i = 0; i < m_overlays.size(); ++i)
            m_overlayRowIdx[i] = i;
        m_rowCount = qMax(1, m_overlays.size());
        const int target = qMax(m_singleRowHeight, m_rowCount * kRowSlotHeight + 4);
        if (target != height() && m_rowHeightCb)
            m_rowHeightCb(target);
    }
    QVector<EnhancedTextOverlay> m_overlays;
    QVector<int> m_overlayRowIdx;  // row assignment per overlay
    int m_rowCount = 1;
    int m_singleRowHeight = 50;  // baseline when no overlays (matches V1 row)
    static constexpr int kRowSlotHeight = 26;
    double m_clipDuration = 0.0;
    double m_pps = 50.0;
    int m_dragIdx = -1;
    int m_dragEdge = 0;   // -1 = left edge, 1 = right edge, 0 = body (move)
    double m_dragOriginalStart = 0.0;
    double m_dragOriginalEnd = 0.0;
    double m_dragAnchorOffsetSec = 0.0;
    int m_hoverIdx = -1;
    int m_hoverEdge = 0;
    TimeChangeCallback m_timeChangeCb;
    RowHeightChangeCallback m_rowHeightCb;
    bool m_rowHeightDragging = false;
    int m_rowHeightDragStartY = 0;
    int m_rowHeightDragStartH = 0;
};
#include <optional>
#include <functional>
#include <cmath>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QWheelEvent>
#include <QDebug>
#include <QCoreApplication>
#include <QTimer>
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QPixmap>
#include <QPainter>
#include <QMenu>
#include <QSettings>
#include <QAction>
#include <QHash>

extern "C" {
#include <libavformat/avformat.h>
}

// --- PlayheadOverlay ---

PlayheadOverlay::PlayheadOverlay(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
}

void PlayheadOverlay::setPlayheadX(int x) { m_playheadX = x; update(); }

void PlayheadOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(0xFF, 0x44, 0x44), 2));
    painter.drawLine(m_playheadX, 0, m_playheadX, height());
    QPolygon tri;
    tri << QPoint(m_playheadX - 6, 0) << QPoint(m_playheadX + 6, 0) << QPoint(m_playheadX, 10);
    painter.setBrush(QColor(0xFF, 0x44, 0x44));
    painter.drawPolygon(tri);
}

void PlayheadOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_dragging = true;
    m_playheadX = qBound(0, event->pos().x(), width());
    grabMouse();
    emit playheadMoved(m_playheadX);
    event->accept();
    update();
}

void PlayheadOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    m_playheadX = qBound(0, event->pos().x(), width());
    emit playheadMoved(m_playheadX);
    event->accept();
    update();
}

void PlayheadOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    m_dragging = false;
    releaseMouse();
    m_playheadX = qBound(0, event->pos().x(), width());
    emit playheadReleased(m_playheadX);
    event->accept();
    update();
}

// --- TimeRuler ---

TimeRuler::TimeRuler(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(22);
    setStyleSheet("background-color: #2d2d2d;");
    setMouseTracking(true);
    setCursor(Qt::SizeHorCursor);
    setToolTip("Drag horizontally to zoom timeline (right=in, left=out)");
}

void TimeRuler::setPixelsPerSecond(double pps)
{
    pps = qBound(0.02, pps, 200.0);
    if (qFuzzyCompare(pps, m_pixelsPerSecond))
        return;
    m_pixelsPerSecond = pps;
    update();
}

void TimeRuler::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x2d, 0x2d, 0x2d));
    painter.setPen(QPen(QColor(0xaa, 0xaa, 0xaa), 1));

    const int w = width();
    const int h = height();

    int majorIntervalSec = 1;
    if (m_pixelsPerSecond < 0.2) majorIntervalSec = 3600;       // 1h marks for 4h+ clips
    else if (m_pixelsPerSecond < 0.5) majorIntervalSec = 1800;  // 30 min
    else if (m_pixelsPerSecond < 1.0) majorIntervalSec = 600;   // 10 min
    else if (m_pixelsPerSecond < 2.0) majorIntervalSec = 300;
    else if (m_pixelsPerSecond < 3.0) majorIntervalSec = 120;
    else if (m_pixelsPerSecond < 4.0) majorIntervalSec = 60;
    else if (m_pixelsPerSecond < 8.0) majorIntervalSec = 30;
    else if (m_pixelsPerSecond < 16.0) majorIntervalSec = 10;
    else if (m_pixelsPerSecond < 40.0) majorIntervalSec = 5;
    else if (m_pixelsPerSecond < 80.0) majorIntervalSec = 2;

    const int majorPx = qMax(1, static_cast<int>(majorIntervalSec * m_pixelsPerSecond));
    if (majorPx <= 0) return;

    const int minorPerMajor = 5;
    const int minorPx = qMax(1, majorPx / minorPerMajor);

    QFont font = painter.font();
    font.setPointSize(8);
    painter.setFont(font);

    for (int x = 0; x < w; x += minorPx) {
        const bool isMajor = (x % majorPx) == 0;
        if (isMajor) {
            painter.setPen(QPen(QColor(0xdd, 0xdd, 0xdd), 1));
            painter.drawLine(x, h - 8, x, h);
            const int totalSec = static_cast<int>(x / m_pixelsPerSecond);
            const int hours = totalSec / 3600;
            const int mins = (totalSec % 3600) / 60;
            const int secs = totalSec % 60;
            QString label = (hours > 0)
                ? QString("%1:%2:%3").arg(hours).arg(mins, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'))
                : QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
            painter.drawText(QRect(x + 2, 0, 60, h - 8), Qt::AlignLeft | Qt::AlignVCenter, label);
        } else {
            painter.setPen(QPen(QColor(0x77, 0x77, 0x77), 1));
            painter.drawLine(x, h - 4, x, h);
        }
    }

    painter.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
    painter.drawLine(0, h - 1, w, h - 1);
}

void TimeRuler::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    // Use global (screen) X, not widget-local X. While dragging, setZoomLevel
    // resizes the ruler and the scroll area may auto-scroll, which would
    // shift the ruler's widget-relative origin under the cursor and cause the
    // drag delta to spiral — zoom→resize→shift→bigger delta→more zoom, runaway.
    m_dragStartX = event->globalPosition().toPoint().x();
    m_dragStartPps = m_pixelsPerSecond;
    grabMouse();
    emit zoomDragStarted();
    event->accept();
}

void TimeRuler::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int dx = event->globalPosition().toPoint().x() - m_dragStartX;
    // Multiplicative drag: each pixel multiplies zoom by ~2%. Works uniformly
    // from 0.02 pps (20h clip fits) up to 200 pps (frame-precise).
    const double factor = std::pow(1.02, static_cast<double>(dx));
    const double newPps = qBound(0.02, m_dragStartPps * factor, 200.0);
    if (!qFuzzyCompare(newPps, m_pixelsPerSecond)) {
        m_pixelsPerSecond = newPps;
        emit zoomChanged(newPps);
        update();
    }
    event->accept();
}

void TimeRuler::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    releaseMouse();
    emit zoomDragEnded();
    event->accept();
}

// --- TimelineTrack ---

TimelineTrack::TimelineTrack(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(m_rowHeight);
    setMaximumHeight(m_rowHeight);
    setMouseTracking(true);
    setAcceptDrops(true);
}

void TimelineTrack::setRowHeight(int h)
{
    h = qBound(20, h, 300);
    if (h == m_rowHeight) return;
    m_rowHeight = h;
    setMinimumHeight(m_rowHeight);
    setMaximumHeight(m_rowHeight);
    update();
}

void TimelineTrack::addClip(const ClipInfo &clip) { m_clips.append(clip); updateMinimumWidth(); update(); emit modified(); }
void TimelineTrack::insertClip(int index, const ClipInfo &clip)
{
    if (index < 0 || index > m_clips.size()) index = m_clips.size();
    m_clips.insert(index, clip); updateMinimumWidth(); update(); emit modified();
}

void TimelineTrack::removeClip(int index)
{
    if (index < 0 || index >= m_clips.size()) return;
    m_clips.removeAt(index);
    QList<int> newSel;
    for (int s : m_selectedClips) {
        if (s < index) newSel.append(s);
        else if (s > index) newSel.append(s - 1);
    }
    m_selectedClips = newSel;
    const int primary = m_selectedClips.isEmpty() ? -1 : m_selectedClips.last();
    updateMinimumWidth(); update(); emit selectionChanged(primary, false); emit modified();
}

void TimelineTrack::moveClip(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_clips.size()) return;
    if (toIndex < 0 || toIndex >= m_clips.size()) return;
    if (fromIndex == toIndex) return;
    ClipInfo clip = m_clips[fromIndex];
    m_clips.removeAt(fromIndex);
    m_clips.insert(toIndex, clip);
    m_selectedClips = {toIndex};
    updateMinimumWidth(); update();
    emit selectionChanged(toIndex, false);
    emit clipMoved(fromIndex, toIndex); emit modified();
}

void TimelineTrack::splitClipAt(int index, double localSeconds)
{
    if (index < 0 || index >= m_clips.size()) return;
    ClipInfo &original = m_clips[index];
    double effectiveStart = original.inPoint;
    double effectiveEnd = (original.outPoint > 0.0) ? original.outPoint : original.duration;
    double splitPoint = effectiveStart + localSeconds * original.speed;
    if (splitPoint <= effectiveStart + 0.1 || splitPoint >= effectiveEnd - 0.1) return;
    ClipInfo secondHalf = original;
    secondHalf.inPoint = splitPoint;
    secondHalf.outPoint = effectiveEnd;
    original.outPoint = splitPoint;
    m_clips.insert(index + 1, secondHalf);
    updateMinimumWidth(); update(); emit modified();
}

void TimelineTrack::applyDragMove(int clipIdx, double leadIn, double nextLeadIn)
{
    if (clipIdx < 0 || clipIdx >= m_clips.size()) return;
    m_clips[clipIdx].leadInSec = qMax(0.0, leadIn);
    if (nextLeadIn >= 0.0 && clipIdx + 1 < m_clips.size())
        m_clips[clipIdx + 1].leadInSec = qMax(0.0, nextLeadIn);
    updateMinimumWidth();
    update();
}

void TimelineTrack::removeClipPreservingDownstream(int index)
{
    if (index < 0 || index >= m_clips.size()) return;
    const double absorbed = m_clips[index].leadInSec + m_clips[index].effectiveDuration();
    m_clips.removeAt(index);
    // Push the vacated time into the next clip's leadInSec so everything
    // downstream stays at the same absolute timeline position.
    if (index < m_clips.size())
        m_clips[index].leadInSec += absorbed;
    QList<int> newSel;
    for (int s : m_selectedClips) {
        if (s < index) newSel.append(s);
        else if (s > index) newSel.append(s - 1);
    }
    m_selectedClips = newSel;
    const int primary = m_selectedClips.isEmpty() ? -1 : m_selectedClips.last();
    updateMinimumWidth(); update();
    emit selectionChanged(primary, false);
    emit modified();
}

void TimelineTrack::insertClipPreservingDownstream(int index, const ClipInfo &clip, double leadInSec)
{
    if (index < 0) index = 0;
    if (index > m_clips.size()) index = m_clips.size();
    ClipInfo newClip = clip;
    newClip.leadInSec = qMax(0.0, leadInSec);
    const double consumed = newClip.leadInSec + newClip.effectiveDuration();
    m_clips.insert(index, newClip);
    // The existing clip that now sits at index+1 used to start at
    //   (prevEnd + oldLeadIn)
    // and must continue to start at that same absolute time. After the
    // insertion, prevEnd effectively advanced by `consumed`, so we shave
    // `consumed` off its leadInSec. If we'd go negative the plan was bad.
    if (index + 1 < m_clips.size()) {
        double &nextLead = m_clips[index + 1].leadInSec;
        nextLead -= consumed;
        if (nextLead < 0) nextLead = 0;
    }
    updateMinimumWidth(); update();
    emit modified();
}

TimelineTrack::DropPlan TimelineTrack::planDrop(double dropTime, double clipDuration) const
{
    DropPlan plan{};
    if (dropTime < 0.0) dropTime = 0.0;
    double cursor = 0.0; // end time of the previous clip (i.e. start of the gap before clip[i])
    for (int i = 0; i < m_clips.size(); ++i) {
        const double clipStart = cursor + m_clips[i].leadInSec;
        const double clipEnd = clipStart + m_clips[i].effectiveDuration();
        // Does the dropped clip fit entirely within the gap before clip[i]?
        if (dropTime + clipDuration <= clipStart + 1e-6) {
            plan.insertIdx = i;
            plan.newLeadIn = qMax(0.0, dropTime - cursor);
            plan.nextLeadInDelta = plan.newLeadIn + clipDuration;
            plan.valid = true;
            return plan;
        }
        // Overlap with clip[i] — no valid slot here.
        if (dropTime < clipEnd - 1e-6)
            return plan; // invalid
        cursor = clipEnd;
    }
    // After the last clip: always valid, just appends.
    plan.insertIdx = m_clips.size();
    plan.newLeadIn = qMax(0.0, dropTime - cursor);
    plan.nextLeadInDelta = 0.0;
    plan.valid = true;
    return plan;
}

void TimelineTrack::setClips(const QVector<ClipInfo> &clips) { m_clips = clips; updateMinimumWidth(); update(); }
void TimelineTrack::setSelectedClip(int index) {
    QList<int> newSel;
    if (index >= 0) newSel.append(index);
    if (m_selectedClips == newSel) return;
    m_selectedClips = newSel;
    update();
    emit selectionChanged(index, false);
}

void TimelineTrack::toggleClipSelection(int index) {
    if (index < 0) return;
    if (m_selectedClips.contains(index)) {
        m_selectedClips.removeAll(index);
    } else {
        m_selectedClips.append(index);
    }
    update();
    const int primary = m_selectedClips.isEmpty() ? -1 : m_selectedClips.last();
    emit selectionChanged(primary, true);
}

void TimelineTrack::clearClipSelection() {
    if (m_selectedClips.isEmpty()) return;
    m_selectedClips.clear();
    update();
    emit selectionChanged(-1, false);
}

void TimelineTrack::moveSelectedClipsGroup(int targetIndex)
{
    if (m_selectedClips.size() < 2) return;
    if (targetIndex < 0 || targetIndex > m_clips.size()) return;

    QList<int> sortedSel = m_selectedClips;
    std::sort(sortedSel.begin(), sortedSel.end());

    // Bail if any source index is invalid.
    for (int idx : sortedSel)
        if (idx < 0 || idx >= m_clips.size()) return;

    // Extract moved clips in order.
    QVector<ClipInfo> movedClips;
    movedClips.reserve(sortedSel.size());
    for (int idx : sortedSel) movedClips.append(m_clips[idx]);

    // Compute insert position relative to the list with sources removed.
    int adjustedTarget = targetIndex;
    for (int idx : sortedSel)
        if (idx < targetIndex) adjustedTarget--;

    // Remove sources from highest to lowest so indices stay valid.
    for (int i = sortedSel.size() - 1; i >= 0; --i)
        m_clips.removeAt(sortedSel[i]);

    adjustedTarget = qBound(0, adjustedTarget, m_clips.size());

    for (int i = 0; i < movedClips.size(); ++i)
        m_clips.insert(adjustedTarget + i, movedClips[i]);

    // New selection is the consecutive range of inserted clips.
    QList<int> newSel;
    for (int i = 0; i < movedClips.size(); ++i) newSel.append(adjustedTarget + i);
    m_selectedClips = newSel;

    updateMinimumWidth(); update();
    emit selectionChanged(newSel.last(), false);
    emit clipMoved(sortedSel.first(), adjustedTarget);
    emit modified();
}

void TimelineTrack::setPixelsPerSecond(double pps)
{
    m_pixelsPerSecond = qBound(0.02, pps, 200.0);
    updateMinimumWidth();
    update();
}

int TimelineTrack::clipAtX(int x) const
{
    int cx = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        cx += qMax(0, static_cast<int>(m_clips[i].leadInSec * m_pixelsPerSecond));
        int w = qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
        if (x >= cx && x < cx + w) return i;
        cx += w;
    }
    return -1;
}

int TimelineTrack::clipStartX(int index) const
{
    int x = 0;
    for (int i = 0; i < index && i < m_clips.size(); ++i) {
        x += qMax(0, static_cast<int>(m_clips[i].leadInSec * m_pixelsPerSecond));
        x += qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
    }
    if (index >= 0 && index < m_clips.size())
        x += qMax(0, static_cast<int>(m_clips[index].leadInSec * m_pixelsPerSecond));
    return x;
}

double TimelineTrack::xToSeconds(int x) const { return static_cast<double>(x) / m_pixelsPerSecond; }
int TimelineTrack::secondsToX(double seconds) const { return static_cast<int>(seconds * m_pixelsPerSecond); }

int TimelineTrack::snapToEdge(int x) const
{
    if (!m_snapEnabled) return x;
    int cx = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        cx += qMax(0, static_cast<int>(m_clips[i].leadInSec * m_pixelsPerSecond));
        if (qAbs(x - cx) <= SNAP_THRESHOLD) return cx;
        cx += qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
        if (qAbs(x - cx) <= SNAP_THRESHOLD) return cx;
    }
    return x;
}

void TimelineTrack::updateMinimumWidth()
{
    qint64 totalWidth = 0;
    for (const auto &c : m_clips) {
        totalWidth += qMax<qint64>(0, static_cast<qint64>(c.leadInSec * m_pixelsPerSecond));
        qint64 w = qMax<qint64>(20,
            static_cast<qint64>(c.effectiveDuration() * m_pixelsPerSecond));
        totalWidth += w;
    }
    // Qt widget width is int; cap to a sane upper bound. Large widgets break
    // backing-store allocation and painter clipping, but we need to allow
    // multi-clip sequences to span well past the original ~5h cap. Timeline's
    // ensureSequenceFitsViewport() auto-zooms-out when content exceeds the
    // viewport so the user normally never hits this hard cap.
    constexpr qint64 kMaxWidth = 2000000;
    totalWidth = qMin(totalWidth, kMaxWidth);
    setMinimumWidth(static_cast<int>(totalWidth) + 100);
}

void TimelineTrack::paintEvent(QPaintEvent *event)
{
    static int paintCount = 0;
    if (++paintCount <= 5) {
        qInfo() << "TimelineTrack::paintEvent #" << paintCount
                << "clips=" << m_clips.size() << "pps=" << m_pixelsPerSecond;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect visibleRect = event ? event->rect() : QRect(0, 0, width(), height());
    int x = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        // Leading gap (leadInSec) before this clip, created by left-trim so the
        // clip's right edge stays anchored. Skip drawing over it — the dark
        // track background shows through.
        x += qMax(0, static_cast<int>(m_clips[i].leadInSec * m_pixelsPerSecond));
        // Cap the drawn width so pathological long clips don't explode paint
        // (raised in lockstep with updateMinimumWidth's kMaxWidth so multi-clip
        // sequences place subsequent clips at the correct x position).
        int clipWidth = qMax(20, static_cast<int>(
            qMin<double>(2000000.0,
                         m_clips[i].effectiveDuration() * m_pixelsPerSecond)));
        QRect clipRect(x, 0, clipWidth, m_rowHeight);
        QColor color = (i % 2 == 0) ? QColor(0x44, 0x88, 0xCC) : QColor(0x44, 0xAA, 0x88);
        const bool isSelected = m_selectedClips.contains(i);
        if (isSelected) color = color.lighter(140);
        if (m_dragMode == DragMode::MoveClip && i == m_dropTargetIndex) {
            painter.setPen(QPen(Qt::yellow, 3));
            painter.drawLine(x, 0, x, m_rowHeight);
        }
        painter.fillRect(clipRect, color);
        // Premiere-style effect indicator: a 3 px purple bar along the top
        // edge of any clip with a non-default color correction OR any video
        // effect applied. The bar spans the full clip width so the user can
        // immediately see the affected time range.
        const bool hasEffects = !m_clips[i].colorCorrection.isDefault()
                             || !m_clips[i].effects.isEmpty();
        if (hasEffects) {
            const QRect fxBar(x, 0, clipWidth, 3);
            painter.fillRect(fxBar, QColor(170, 100, 230, 230));
        }
        // Trim handle indicators — visible on all clips so users can discover
        // the edge-drag-to-trim affordance without having to hunt for it.
        const QColor trimColor = isSelected ? QColor(255, 200, 60, 220)
                                            : QColor(255, 255, 255, 120);
        painter.fillRect(QRect(x, 0, TRIM_HANDLE_WIDTH, m_rowHeight), trimColor);
        painter.fillRect(QRect(x + clipWidth - TRIM_HANDLE_WIDTH, 0, TRIM_HANDLE_WIDTH, m_rowHeight), trimColor);
        if (isSelected) {
            painter.setPen(QPen(Qt::yellow, 2));
            painter.drawRect(clipRect.adjusted(1, 1, -1, -1));
        } else {
            painter.setPen(QPen(Qt::white, 1));
            painter.drawRect(clipRect);
        }
        // Draw waveform if available — only for the visible x range to avoid
        // drawing 100k+ lines for very long clips.
        if (!m_clips[i].waveform.isEmpty()) {
            painter.setPen(QPen(QColor(100, 200, 150, 180), 1));
            const auto &wf = m_clips[i].waveform;
            const int wfPeakCount = wf.peaks.size();
            if (wfPeakCount > 0 && clipWidth > 2) {
                const int midY = m_rowHeight / 2;
                const int maxAmp = m_rowHeight / 2 - 4;

                // Intersect with visible rect to skip drawing offscreen pixels.
                const int pxStart = qMax(0, visibleRect.left() - x);
                const int pxEnd   = qMin(clipWidth, visibleRect.right() - x + 1);

                for (int px = pxStart; px < pxEnd; ++px) {
                    // Map px (0..clipWidth-1) linearly to peaks (0..wfPeakCount-1).
                    qint64 peakIdx = static_cast<qint64>(px) * wfPeakCount / clipWidth;
                    if (peakIdx < 0 || peakIdx >= wfPeakCount) continue;
                    const float amp = wf.peaks[static_cast<int>(peakIdx)];
                    const int h = static_cast<int>(amp * maxAmp);
                    painter.drawLine(x + px, midY - h, x + px, midY + h);
                }
            }
        }

        // Label with speed indicator
        painter.setPen(Qt::white);
        QRect textRect = clipRect.adjusted(8, 4, -8, -4);
        double dur = m_clips[i].effectiveDuration();
        int mins = static_cast<int>(dur) / 60;
        int secs = static_cast<int>(dur) % 60;
        QString label = m_clips[i].displayName;
        if (m_clips[i].speed != 1.0)
            label += QString(" [%1x]").arg(m_clips[i].speed, 0, 'f', 1);
        label += QString(" %1:%2").arg(mins, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(label, Qt::ElideRight, textRect.width()));
        x += clipWidth;
    }

    // Cross-track drop preview. Cyan bar = valid drop, red bar = overlap.
    if (m_dropIndicatorX >= 0) {
        const QColor color = m_dropIndicatorValid
            ? QColor(100, 220, 255, 200)
            : QColor(255, 80, 80, 200);
        painter.setPen(QPen(color, 3));
        painter.drawLine(m_dropIndicatorX, 0, m_dropIndicatorX, m_rowHeight);
        if (!m_dropIndicatorValid) {
            // Crosshatched band so the invalid state reads at a glance even
            // on top of waveform noise.
            painter.fillRect(m_dropIndicatorX - 2, 0, 4, m_rowHeight,
                             QColor(255, 80, 80, 90));
        }
    }

    // Visual indication of hidden / muted state — overlay the whole track.
    if (m_hidden) {
        painter.fillRect(rect(), QColor(0, 0, 0, 140));
        painter.setPen(QPen(QColor(180, 180, 180), 1, Qt::DashLine));
        painter.drawLine(0, 0, width(), height());
    }
    if (m_muted) {
        painter.fillRect(rect(), QColor(120, 30, 30, 60));
    }

    // Resize handle hint at the bottom edge of the track. A faint horizontal
    // bar tells the user the row height can be dragged here.
    painter.fillRect(0, height() - 2, width(), 2, QColor(70, 70, 70));
}

void TimelineTrack::mousePressEvent(QMouseEvent *event)
{
    if (m_locked) {
        // Let Qt still route scroll/playhead clicks upward if needed, but
        // block every local edit mode (drag/trim/split). The simplest and
        // safest way: accept the event and return immediately so no
        // DragMode / TrimMode is entered.
        event->accept();
        return;
    }
    // Bottom-edge drag → row height resize. Detected before the seek/click
    // handling so the user can resize even when clicking inside a clip area.
    if (event->button() == Qt::LeftButton
        && event->pos().y() >= m_rowHeight - RESIZE_HANDLE_HEIGHT) {
        m_resizingHeight = true;
        m_resizeStartY = event->globalPosition().toPoint().y();
        m_resizeStartHeight = m_rowHeight;
        setCursor(Qt::SizeVerCursor);
        grabMouse();
        event->accept();
        return;
    }

    const bool additive = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);

    // Right-click on a clip → context menu. We fire the signal and let
    // Timeline assemble / show the menu so it can coordinate cross-track
    // operations (delete, unlink, etc.) that can't be done from inside a
    // single TimelineTrack.
    if (event->button() == Qt::RightButton) {
        const int hit = clipAtX(event->pos().x());
        if (hit >= 0) {
            emit clipContextMenuRequested(hit, event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::LeftButton && !additive)
        emit seekRequested(qMax(0.0, xToSeconds(event->pos().x())));

    int clickedClip = clipAtX(event->pos().x());
    if (event->button() == Qt::LeftButton && clickedClip >= 0) {
        if (additive) {
            toggleClipSelection(clickedClip);
            emit clipClicked(clickedClip);
            return;
        }
        // Preserve a multi-selection when the user starts a group drag by
        // clicking (without modifier) on a clip that's already part of it.
        // Otherwise, replace the selection with the clicked clip.
        const bool partOfMulti = m_selectedClips.size() > 1
                                 && m_selectedClips.contains(clickedClip);
        if (!partOfMulti) setSelectedClip(clickedClip);
        emit clipClicked(clickedClip);
        int cx = clipStartX(clickedClip);
        int clipWidth = qMax(20, static_cast<int>(m_clips[clickedClip].effectiveDuration() * m_pixelsPerSecond));
        int localX = event->pos().x() - cx;
        if (localX <= TRIM_HANDLE_WIDTH) {
            m_dragMode = DragMode::TrimLeft; m_dragClipIndex = clickedClip;
            m_dragStartX = event->pos().x(); m_dragOriginalValue = m_clips[clickedClip].inPoint;
            m_dragOriginalLeadIn = m_clips[clickedClip].leadInSec;
        } else if (localX >= clipWidth - TRIM_HANDLE_WIDTH) {
            m_dragMode = DragMode::TrimRight; m_dragClipIndex = clickedClip;
            m_dragStartX = event->pos().x();
            m_dragOriginalValue = m_clips[clickedClip].outPoint > 0 ? m_clips[clickedClip].outPoint : m_clips[clickedClip].duration;
        } else {
            m_dragMode = DragMode::MoveClip; m_dragClipIndex = clickedClip;
            m_dragStartX = event->pos().x();
            m_dragOriginalLeadIn = m_clips[clickedClip].leadInSec;
            m_dragOriginalLeadInNext = (clickedClip + 1 < m_clips.size())
                ? m_clips[clickedClip + 1].leadInSec
                : -1.0;
            m_dropTargetIndex = -1;
            // Tell Timeline so it can snapshot every linked partner's
            // leadInSec state — the drag will stay in sync as long as we
            // broadcast deltas through linkedDragDelta each move.
            emit linkedDragStarted(clickedClip);
        }
    } else if (clickedClip < 0 && !additive) {
        setSelectedClip(-1);
        emit emptyAreaClicked();
    }
}

void TimelineTrack::mouseMoveEvent(QMouseEvent *event)
{
    if (m_resizingHeight) {
        const int globalY = event->globalPosition().toPoint().y();
        const int delta = globalY - m_resizeStartY;
        const int newH = qBound(20, m_resizeStartHeight + delta, 300);
        if (newH != m_rowHeight) {
            setRowHeight(newH);
            emit rowHeightChanged(newH);
        }
        event->accept();
        return;
    }
    if (m_dragMode == DragMode::MoveClip && m_dragClipIndex >= 0 && m_dragClipIndex < m_clips.size()) {
        const int y = event->pos().y();
        // If the user has pulled the cursor far enough vertically out of our
        // row, promote the in-track leadInSec drag into a full Qt drag so
        // other tracks can accept it as a drop. Revert the leadInSec we
        // tentatively applied so canceling the drag leaves the clip alone.
        if (y < -CROSS_TRACK_DRAG_THRESHOLD || y >= height() + CROSS_TRACK_DRAG_THRESHOLD) {
            // Snapshot the data we need before mutating the drag state.
            const int srcIdx = m_dragClipIndex;
            m_clips[srcIdx].leadInSec = m_dragOriginalLeadIn;
            if (m_dragOriginalLeadInNext >= 0.0 && srcIdx + 1 < m_clips.size())
                m_clips[srcIdx + 1].leadInSec = m_dragOriginalLeadInNext;
            const int grabOffsetPx = m_dragStartX - clipStartX(srcIdx);

            // Ask Timeline to roll back any linked partners it already
            // shifted — otherwise they'd be stuck at the interim positions
            // when the QDrag starts.
            emit linkedDragCancelled();

            // End the in-track drag BEFORE starting QDrag — drag->exec() runs
            // a nested event loop and we don't want a stale MoveClip state.
            m_dragMode = DragMode::None;
            m_dragClipIndex = -1;
            m_dragOriginalLeadInNext = -1.0;
            setCursor(Qt::ArrowCursor);
            updateMinimumWidth();
            update();

            // Build MIME payload: "srcIdx:grabOffsetPx".
            QByteArray payload;
            payload += QByteArray::number(srcIdx);
            payload += ':';
            payload += QByteArray::number(grabOffsetPx);
            auto *mime = new QMimeData();
            mime->setData("application/x-vse-clip", payload);

            auto *drag = new QDrag(this);
            drag->setMimeData(mime);
            // Ghost pixmap so the user sees what they're dragging.
            const int ghostW = qMax(20, static_cast<int>(
                m_clips[srcIdx].effectiveDuration() * m_pixelsPerSecond));
            QPixmap ghost(ghostW, m_rowHeight);
            ghost.fill(QColor(68, 136, 204, 180));
            QPainter gp(&ghost);
            gp.setPen(QPen(QColor(255, 255, 255, 220), 1));
            gp.drawRect(0, 0, ghostW - 1, m_rowHeight - 1);
            gp.drawText(ghost.rect().adjusted(4, 0, -4, 0),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        m_clips[srcIdx].displayName);
            gp.end();
            drag->setPixmap(ghost);
            drag->setHotSpot(QPoint(grabOffsetPx, m_rowHeight / 2));

            drag->exec(Qt::MoveAction);
            return;
        }

        // Positional drag: shift leadInSec + compensate the next clip so
        // downstream clips don't slide. Linked clips take a different path
        // — Timeline applies a globally-clamped delta to every partner
        // (including this one) so the V/A pair stays locked together even
        // when each track has different clamp room.
        const int dx = event->pos().x() - m_dragStartX;
        const double rawDeltaSec = static_cast<double>(dx) / m_pixelsPerSecond;
        const int linkGroup = m_clips[m_dragClipIndex].linkGroup;
        if (linkGroup > 0) {
            emit linkedDragDelta(m_dragClipIndex, rawDeltaSec);
            setCursor(Qt::ClosedHandCursor);
            return;
        }

        double deltaSec = rawDeltaSec;
        if (deltaSec < -m_dragOriginalLeadIn)
            deltaSec = -m_dragOriginalLeadIn;
        if (m_dragOriginalLeadInNext >= 0.0 && deltaSec > m_dragOriginalLeadInNext)
            deltaSec = m_dragOriginalLeadInNext;
        m_clips[m_dragClipIndex].leadInSec = m_dragOriginalLeadIn + deltaSec;
        if (m_dragOriginalLeadInNext >= 0.0
            && m_dragClipIndex + 1 < m_clips.size()) {
            m_clips[m_dragClipIndex + 1].leadInSec =
                m_dragOriginalLeadInNext - deltaSec;
        }
        setCursor(Qt::ClosedHandCursor);
        updateMinimumWidth();
        update();
        return;
    }
    if (m_dragMode == DragMode::TrimLeft || m_dragMode == DragMode::TrimRight) {
        if (m_dragClipIndex < 0 || m_dragClipIndex >= m_clips.size()) {
            m_dragMode = DragMode::None;
            m_dragClipIndex = -1;
            setCursor(Qt::ArrowCursor);
            return;
        }
        int snappedX = snapToEdge(event->pos().x());
        int dx = snappedX - m_dragStartX;
        double deltaSec = static_cast<double>(dx) / m_pixelsPerSecond;
        ClipInfo &clip = m_clips[m_dragClipIndex];
        if (m_dragMode == DragMode::TrimLeft) {
            double newIn = qMax(0.0, m_dragOriginalValue + deltaSec);
            double maxIn = (clip.outPoint > 0 ? clip.outPoint : clip.duration) - 0.1;
            clip.inPoint = qMin(newIn, maxIn);
            // Keep the clip's right edge anchored by shifting its leadInSec by
            // the same amount the inPoint changed. inPoint+ → leadInSec+
            // (clip slides right), inPoint− → leadInSec− (clip slides left,
            // clamped at 0 so it can't cross its neighbor).
            const double inPointDelta = clip.inPoint - m_dragOriginalValue;
            clip.leadInSec = qMax(0.0, m_dragOriginalLeadIn + inPointDelta);
        } else {
            double newOut = qMin(clip.duration, m_dragOriginalValue + deltaSec);
            clip.outPoint = qMax(newOut, clip.inPoint + 0.1);
        }
        updateMinimumWidth(); update(); return;
    }
    // Hover near the bottom edge → vertical resize cursor hint.
    if (event->pos().y() >= m_rowHeight - RESIZE_HANDLE_HEIGHT) {
        setCursor(Qt::SizeVerCursor);
        return;
    }

    int hover = clipAtX(event->pos().x());
    if (hover >= 0 && m_selectedClips.contains(hover)) {
        int cx = clipStartX(hover);
        int cw = qMax(20, static_cast<int>(m_clips[hover].effectiveDuration() * m_pixelsPerSecond));
        int lx = event->pos().x() - cx;
        setCursor((lx <= TRIM_HANDLE_WIDTH || lx >= cw - TRIM_HANDLE_WIDTH) ? Qt::SizeHorCursor : Qt::OpenHandCursor);
    } else { setCursor(Qt::ArrowCursor); }
}

void TimelineTrack::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_resizingHeight) {
        m_resizingHeight = false;
        releaseMouse();
        setCursor(Qt::ArrowCursor);
        emit rowHeightChanged(m_rowHeight);
        if (event) event->accept();
        return;
    }
    if (m_dragMode == DragMode::MoveClip
        && m_dragClipIndex >= 0 && m_dragClipIndex < m_clips.size()) {
        // Fire modified() so Timeline saves an undo snapshot and the playback
        // schedule rebuilds with the new leadInSec gaps.
        if (!qFuzzyCompare(m_clips[m_dragClipIndex].leadInSec, m_dragOriginalLeadIn))
            emit modified();
    }
    if (m_dragMode == DragMode::TrimLeft || m_dragMode == DragMode::TrimRight) emit modified();
    m_dragMode = DragMode::None; m_dragClipIndex = -1; m_dropTargetIndex = -1;
    m_dragOriginalLeadInNext = -1.0;
    setCursor(Qt::ArrowCursor); update();
}

// --- Cross-track drag & drop ---

static int parseDropPayload(const QByteArray &payload, int &grabOffsetPx)
{
    const int sep = payload.indexOf(':');
    if (sep <= 0) return -1;
    bool okIdx = false, okOff = false;
    const int srcIdx = payload.left(sep).toInt(&okIdx);
    grabOffsetPx = payload.mid(sep + 1).toInt(&okOff);
    if (!okIdx || !okOff) return -1;
    return srcIdx;
}

void TimelineTrack::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat("application/x-vse-clip"))
        event->acceptProposedAction();
    else
        event->ignore();
}

void TimelineTrack::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasFormat("application/x-vse-clip")) {
        event->ignore();
        return;
    }
    auto *src = qobject_cast<TimelineTrack *>(event->source());
    if (!src || src == this) {
        // Same-track drags are handled by the leadInSec path — nothing to do here.
        event->ignore();
        m_dropIndicatorX = -1;
        update();
        return;
    }
    int grabOffsetPx = 0;
    const int srcIdx = parseDropPayload(
        event->mimeData()->data("application/x-vse-clip"), grabOffsetPx);
    if (srcIdx < 0 || srcIdx >= src->m_clips.size()) {
        event->ignore();
        return;
    }
    const int dropX = event->position().toPoint().x() - grabOffsetPx;
    const double dropTime = qMax(0.0, dropX / m_pixelsPerSecond);
    const DropPlan plan = planDrop(dropTime, src->m_clips[srcIdx].effectiveDuration());
    m_dropIndicatorX = qMax(0, dropX);
    m_dropIndicatorValid = plan.valid;
    event->acceptProposedAction();
    update();
}

void TimelineTrack::dragLeaveEvent(QDragLeaveEvent *)
{
    if (m_dropIndicatorX != -1) {
        m_dropIndicatorX = -1;
        m_dropIndicatorValid = false;
        update();
    }
}

void TimelineTrack::dropEvent(QDropEvent *event)
{
    if (m_locked) {
        event->ignore();
        return;
    }
    const int oldIndicator = m_dropIndicatorX;
    m_dropIndicatorX = -1;
    m_dropIndicatorValid = false;
    if (oldIndicator >= 0) update();

    if (!event->mimeData()->hasFormat("application/x-vse-clip")) {
        event->ignore();
        return;
    }
    auto *src = qobject_cast<TimelineTrack *>(event->source());
    if (!src || src == this) {
        event->ignore();
        return;
    }
    int grabOffsetPx = 0;
    const int srcIdx = parseDropPayload(
        event->mimeData()->data("application/x-vse-clip"), grabOffsetPx);
    if (srcIdx < 0 || srcIdx >= src->m_clips.size()) {
        event->ignore();
        return;
    }
    const ClipInfo clipData = src->m_clips[srcIdx];
    const double clipDur = clipData.effectiveDuration();
    const int dropX = event->position().toPoint().x() - grabOffsetPx;
    const double dropTime = qMax(0.0, dropX / m_pixelsPerSecond);
    const DropPlan plan = planDrop(dropTime, clipDur);
    if (!plan.valid) {
        event->ignore();
        return;
    }

    // Order matters: remove from source first (while our indices are still
    // valid — source and this track have separate m_clips, so removing from
    // source never perturbs this track's indices, but doing it first also
    // matches the "move, not copy" semantics if src == this ever slips
    // through in the future).
    src->removeClipPreservingDownstream(srcIdx);
    insertClipPreservingDownstream(plan.insertIdx, clipData, plan.newLeadIn);

    event->acceptProposedAction();

    // Notify Timeline to move linked partners (V↔A) to the corresponding track.
    // Pass the absolute drop time so Timeline can place the partner at the same position.
    if (clipData.linkGroup > 0) {
        emit crossTrackDropped(plan.insertIdx, clipData.linkGroup, dropTime);
    }
}

// --- Timeline ---

Timeline::Timeline(QWidget *parent) : QWidget(parent)
{
    m_undoManager = new UndoManager(this);
    setupUI();
    saveUndoState("Initial state");
}

void Timeline::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *infoRow = new QHBoxLayout();
    m_infoLabel = new QLabel("Timeline", this);
    m_infoLabel->setStyleSheet("font-weight: bold; color: #ccc;");
    infoRow->addWidget(m_infoLabel, 1);

    // Track row-height controls. The minus/plus buttons let the user resize
    // every track in 10 px steps (clamped to 30..200) for dense or expanded
    // multi-track layouts.
    const QString sizeBtnStyle =
        "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
        "  border-radius: 3px; font-size: 13px; padding: 0 6px; }"
        "QPushButton:hover { background-color: #555; }";
    auto *rowHLabel = new QLabel(QStringLiteral("行高"), this);
    rowHLabel->setStyleSheet("color: #999; font-size: 11px;");
    auto *rowMinus = new QPushButton(QString::fromUtf8("\xE2\x88\x92"), this); // −
    auto *rowPlus  = new QPushButton(QString::fromUtf8("\x2B"), this);          // +
    rowMinus->setFixedSize(26, 22);
    rowPlus->setFixedSize(26, 22);
    rowMinus->setStyleSheet(sizeBtnStyle);
    rowPlus->setStyleSheet(sizeBtnStyle);
    rowMinus->setToolTip(QStringLiteral("行を低く"));
    rowPlus->setToolTip(QStringLiteral("行を高く"));
    connect(rowMinus, &QPushButton::clicked, this, &Timeline::decreaseTrackHeight);
    connect(rowPlus,  &QPushButton::clicked, this, &Timeline::increaseTrackHeight);
    infoRow->addWidget(rowHLabel);
    infoRow->addWidget(rowMinus);
    infoRow->addWidget(rowPlus);

    layout->addLayout(infoRow);

    // Horizontal split: [frozen header column | scrollable tracks area].
    // Track headers (mute/hide buttons + label) live in the header column on
    // the left and stay put while the user scrolls the tracks horizontally.
    auto *contentArea = new QWidget(this);
    auto *contentHbox = new QHBoxLayout(contentArea);
    contentHbox->setContentsMargins(0, 0, 0, 0);
    contentHbox->setSpacing(0);

    m_headerColumn = new QWidget(contentArea);
    m_headerColumn->setFixedWidth(kHeaderColumnWidth);
    m_headerColumn->setStyleSheet("background-color: #252525;");
    m_headerLayout = new QVBoxLayout(m_headerColumn);
    m_headerLayout->setContentsMargins(0, 0, 0, 0);
    m_headerLayout->setSpacing(2);
    // Magnet (snap) toggle in the header column's top area. Fixed height
    // matches the time ruler (22) + playhead overlay (15) so V1's header
    // aligns with the first track widget below.
    auto *magnetArea = new QWidget(m_headerColumn);
    magnetArea->setFixedHeight(22 + 15);
    magnetArea->setStyleSheet("background-color: #1f1f1f;");
    auto *magnetRow = new QHBoxLayout(magnetArea);
    magnetRow->setContentsMargins(6, 2, 6, 2);
    magnetRow->setSpacing(0);
    auto *magnetBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\xA7\xB2"), magnetArea); // 🧲
    magnetBtn->setCheckable(true);
    magnetBtn->setChecked(snapEnabled());
    magnetBtn->setFixedSize(32, 26);
    magnetBtn->setToolTip(QStringLiteral("マグネット / スナップ 切替"));
    magnetBtn->setStyleSheet(
        "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
        "  border-radius: 3px; font-size: 14px; padding: 0; }"
        "QPushButton:hover { background-color: #555; }"
        "QPushButton:checked { background-color: #3a7f4a; color: white;"
        "  border: 1px solid #6cbd7a; }");
    connect(magnetBtn, &QPushButton::toggled, this, [this](bool on) {
        setSnapEnabled(on);
    });
    magnetRow->addWidget(magnetBtn);
    magnetRow->addStretch();
    m_headerLayout->addWidget(magnetArea);

    m_scrollArea = new QScrollArea(contentArea);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setStyleSheet("background-color: #2a2a2a;");
    // Horizontal scrollbar removed — its role overlaps with the player's seek
    // bar above. Auto-fit zoom makes content fit the viewport in most cases;
    // the scroll position is still controllable via wheel / drag.
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto *tracksContainer = new QWidget();
    auto *tracksOuterLayout = new QVBoxLayout(tracksContainer);
    tracksOuterLayout->setContentsMargins(0, 0, 0, 0);
    tracksOuterLayout->setSpacing(0);

    m_timeRuler = new TimeRuler(tracksContainer);
    m_timeRuler->setPixelsPerSecond(m_zoomLevel);
    connect(m_timeRuler, &TimeRuler::zoomChanged, this, &Timeline::setZoomLevel);
    connect(m_timeRuler, &TimeRuler::zoomDragStarted, this, &Timeline::captureZoomAnchor);
    connect(m_timeRuler, &TimeRuler::zoomDragEnded, this, &Timeline::clearZoomAnchor);
    tracksOuterLayout->addWidget(m_timeRuler);

    m_playheadOverlay = new PlayheadOverlay(tracksContainer);
    m_playheadOverlay->setFixedHeight(15);
    m_playheadOverlay->setStyleSheet("background-color: #222;");
    connect(m_playheadOverlay, &PlayheadOverlay::playheadMoved, this, [this](int x) {
        m_playheadPos = m_videoTrack->xToSeconds(x);
        emit scrubPositionChanged(m_playheadPos);
    });
    connect(m_playheadOverlay, &PlayheadOverlay::playheadReleased, this, [this](int x) {
        m_playheadPos = m_videoTrack->xToSeconds(x);
        emit positionChanged(m_playheadPos);
    });
    tracksOuterLayout->addWidget(m_playheadOverlay);

    m_tracksWidget = new QWidget();
    m_tracksLayout = new QVBoxLayout(m_tracksWidget);
    m_tracksLayout->setContentsMargins(0, 0, 0, 0);
    m_tracksLayout->setSpacing(2);

    // Create initial V1 and A1.
    m_videoTrack = new TimelineTrack(this);
    m_audioTrack = new TimelineTrack(this);
    m_videoTracks.append(m_videoTrack);
    m_audioTracks.append(m_audioTrack);

    // Text strip lives inside the V group — above V1 so it's visually part
    // of the video category. Matching header spacer is added to the header
    // column at the same logical row so V1 stays aligned.
    m_textStrip = new TextStripWidget(m_tracksWidget);
    m_textStrip->setFixedHeight(m_trackHeight);
    m_textStrip->setSingleRowHeight(m_trackHeight);
    m_textStrip->setPixelsPerSecond(m_zoomLevel);
    m_textStrip->setTimeChangeCallback([this](int idx, double start, double end) {
        if (updateTextOverlayTime(idx, start, end))
            emit textOverlayTimeChanged(idx, start, end);
    });
    // Row-resize drag: resize ONLY the text row (m_textStrip + paired
    // header), independent of V/A track height, so the user can make the
    // T1 row taller without affecting the rest of the timeline. Once the
    // user drags, setTrackHeight stops overwriting this row's height.
    m_textStrip->setRowHeightChangeCallback([this](int newH) {
        if (m_textStrip)
            m_textStrip->setFixedHeight(newH);
        if (m_textStripHeader)
            m_textStripHeader->setFixedHeight(newH);
        m_textStripCustomHeight = newH;
    });
    m_textStripHeader = new QWidget(m_headerColumn);
    m_textStripHeader->setFixedHeight(m_trackHeight);
    m_textStripHeader->setStyleSheet(
        "background-color: #353535; color: #ddd; border-right: 1px solid #1a1a1a;");
    auto *thLayout = new QHBoxLayout(m_textStripHeader);
    thLayout->setContentsMargins(6, 0, 6, 0);
    auto *thLabel = new QLabel("T1", m_textStripHeader);
    thLabel->setStyleSheet("color: #ddd; font-size: 11px; font-weight: bold;");
    thLayout->addWidget(thLabel);
    thLayout->addStretch();
    m_headerLayout->addWidget(m_textStripHeader);
    m_tracksLayout->addWidget(m_textStrip);

    m_headerLayout->addWidget(createTrackHeader(m_videoTrack, "V1", false));
    m_tracksLayout->addWidget(m_videoTrack);

    // Boundary separator between the video and audio categories — 3 px
    // bright bar so the V/A split is immediately visible. Mirrored in the
    // header column so the row ordering stays aligned.
    m_vaSeparator = new QWidget(m_tracksWidget);
    m_vaSeparator->setFixedHeight(3);
    m_vaSeparator->setStyleSheet("background-color: #6aa0ff;");
    m_vaSeparatorHeader = new QWidget(m_headerColumn);
    m_vaSeparatorHeader->setFixedHeight(3);
    m_vaSeparatorHeader->setStyleSheet("background-color: #6aa0ff;");
    m_headerLayout->addWidget(m_vaSeparatorHeader);
    m_tracksLayout->addWidget(m_vaSeparator);

    m_headerLayout->addWidget(createTrackHeader(m_audioTrack, "A1", true));
    m_tracksLayout->addWidget(m_audioTrack);

    m_tracksLayout->addStretch();
    m_headerLayout->addStretch();

    tracksOuterLayout->addWidget(m_tracksWidget);
    m_scrollArea->setWidget(tracksContainer);

    // Install event filters so clicks on empty areas (below tracks, outer
    // container, scroll viewport) deselect all clips like clicks on a track's
    // empty area do.
    m_tracksWidget->installEventFilter(this);
    tracksContainer->installEventFilter(this);
    m_scrollArea->viewport()->installEventFilter(this);

    contentHbox->addWidget(m_headerColumn);
    contentHbox->addWidget(m_scrollArea, 1);

    layout->addWidget(contentArea);
    setStyleSheet("background-color: #333;");

    wireTrackSelection(m_videoTrack);
    wireTrackSelection(m_audioTrack);

    auto seekToTrackClick = [this](double seconds) {
        m_playheadPos = qMax(0.0, seconds);
        syncPlayheadOverlay();
        emit positionChanged(m_playheadPos);
    };
    connect(m_videoTrack, &TimelineTrack::seekRequested, this, seekToTrackClick);
    connect(m_audioTrack, &TimelineTrack::seekRequested, this, seekToTrackClick);

    syncPlayheadOverlay();
}

QWidget *Timeline::createTrackHeader(TimelineTrack *track, const QString &name, bool isAudioRow)
{
    auto *w = new QWidget();
    w->setFixedHeight(m_trackHeight); // mirrors current row height
    w->setStyleSheet("QWidget { background-color: #303030; border-right: 1px solid #444; }");

    auto *hbox = new QHBoxLayout(w);
    hbox->setContentsMargins(4, 4, 4, 4);
    hbox->setSpacing(3);

    // Edit lock button (both track types). 🔓 unlocked / 🔒 locked.
    // When locked, mousePressEvent/dropEvent on the track early-return so
    // drag/trim/split/drop edits are blocked — playback is unaffected.
    auto *lockBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x93"), w); // 🔓
    lockBtn->setFixedSize(28, 28);
    lockBtn->setCheckable(true);
    lockBtn->setToolTip(QStringLiteral("編集ロック"));
    lockBtn->setStyleSheet(
        "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
        "  border-radius: 3px; font-size: 14px; padding: 0; }"
        "QPushButton:hover { background-color: #555; }"
        "QPushButton:checked { background-color: #cc8; color: #222; border: 1px solid #ffd; }");

    // Audio rows get a mute toggle; video rows get a hide toggle. Previously
    // both rows carried both icons which had no semantic fit on the wrong
    // track type.
    QPushButton *muteBtn = nullptr;
    QPushButton *hideBtn = nullptr;
    if (isAudioRow) {
        muteBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x8A"), w); // 🔊
        muteBtn->setFixedSize(28, 28);
        muteBtn->setCheckable(true);
        muteBtn->setToolTip(QStringLiteral("ミュート (audio)"));
        muteBtn->setStyleSheet(
            "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
            "  border-radius: 3px; font-size: 14px; padding: 0; }"
            "QPushButton:hover { background-color: #555; }"
            "QPushButton:checked { background-color: #c44; color: white; border: 1px solid #f88; }");
    } else {
        hideBtn = new QPushButton(QString::fromUtf8("\xE2\x97\x89"), w); // ◉
        hideBtn->setFixedSize(28, 28);
        hideBtn->setCheckable(true);
        hideBtn->setToolTip(QStringLiteral("非表示 (hide video)"));
        hideBtn->setStyleSheet(
            "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
            "  border-radius: 3px; font-size: 16px; padding: 0; }"
            "QPushButton:hover { background-color: #555; }"
            "QPushButton:checked { background-color: #666; color: #888; border: 1px solid #999; }");
    }

    auto *label = new QLabel(name, w);
    label->setStyleSheet(isAudioRow
        ? "QLabel { background: transparent; border: none; color: #44AA88; font-weight: bold; font-size: 12px; }"
        : "QLabel { background: transparent; border: none; color: #4488CC; font-weight: bold; font-size: 12px; }");

    hbox->addWidget(lockBtn);
    if (muteBtn) hbox->addWidget(muteBtn);
    if (hideBtn) hbox->addWidget(hideBtn);
    hbox->addWidget(label, 1);

    QPointer<TimelineTrack> trackPtr(track);
    QPointer<QPushButton> lockBtnPtr(lockBtn);
    QPointer<QPushButton> muteBtnPtr(muteBtn);
    QPointer<QPushButton> hideBtnPtr(hideBtn);
    QPointer<QWidget> headerPtr(w);

    // When the user drags the bottom edge of THIS track, resize the matching
    // header on the left so the row stays aligned.
    connect(track, &TimelineTrack::rowHeightChanged, this, [headerPtr](int newH) {
        if (headerPtr) headerPtr->setFixedHeight(newH);
    });

    connect(lockBtn, &QPushButton::toggled, this, [trackPtr, lockBtnPtr](bool checked) {
        if (!trackPtr) return;
        qInfo() << "Timeline: lock toggled =" << checked << "track=" << trackPtr.data();
        trackPtr->setLocked(checked);
        if (lockBtnPtr) {
            lockBtnPtr->setText(checked
                ? QString::fromUtf8("\xF0\x9F\x94\x92")    // 🔒
                : QString::fromUtf8("\xF0\x9F\x94\x93")); // 🔓
        }
    });

    if (muteBtn) {
        connect(muteBtn, &QPushButton::toggled, this, [this, trackPtr, muteBtnPtr](bool checked) {
            if (!trackPtr) return;
            qInfo() << "Timeline: mute toggled =" << checked << "track=" << trackPtr.data();
            trackPtr->setMuted(checked);
            if (muteBtnPtr) {
                muteBtnPtr->setText(checked
                    ? QString::fromUtf8("\xF0\x9F\x94\x87")    // 🔇
                    : QString::fromUtf8("\xF0\x9F\x94\x8A")); // 🔊
            }
            // Re-emit sequences so VideoPlayer picks up the audioMuted flag.
            emit sequenceChanged(computePlaybackSequence());
            emit audioSequenceChanged(computeAudioPlaybackSequence());
        });
    }
    if (hideBtn) {
        connect(hideBtn, &QPushButton::toggled, this, [this, trackPtr, hideBtnPtr](bool checked) {
            if (!trackPtr) return;
            qInfo() << "Timeline: hide toggled =" << checked << "track=" << trackPtr.data();
            trackPtr->setHidden(checked);
            if (hideBtnPtr) {
                hideBtnPtr->setText(checked
                    ? QString::fromUtf8("\xE2\x8A\x98")    // ⊘
                    : QString::fromUtf8("\xE2\x97\x89")); // ◉
            }
            emit sequenceChanged(computePlaybackSequence());
            emit audioSequenceChanged(computeAudioPlaybackSequence());
        });
    }

    return w;
}

void Timeline::addVideoTrack()
{
    int num = m_videoTracks.size() + 1;
    auto *track = new TimelineTrack(this);
    track->setPixelsPerSecond(m_zoomLevel);
    track->setSnapEnabled(snapEnabled());
    track->setRowHeight(m_trackHeight);

    // Young number on top: V1 stays at index 1 in the tracks layout
    // (textStrip occupies index 0), V2 inserts directly after V1, V3 after
    // V2, and so on. Header column has magnetArea at 0 and textStripHeader
    // at 1, so V1 header is at 2 and V2 header belongs at 3.
    const int trackIdx = m_videoTracks.size();
    m_tracksLayout->insertWidget(trackIdx + 1, track);
    m_headerLayout->insertWidget(trackIdx + 2,
        createTrackHeader(track, QString("V%1").arg(num), false));

    m_videoTracks.append(track);
    wireTrackSelection(track);
    updateInfoLabel();
}

void Timeline::addAudioTrack()
{
    int num = m_audioTracks.size() + 1;
    auto *track = new TimelineTrack(this);
    track->setPixelsPerSecond(m_zoomLevel);
    track->setSnapEnabled(snapEnabled());
    track->setRowHeight(m_trackHeight);

    // Insert at the end of the audio block (just before the trailing stretch
    // of each layout). Both layouts have a trailing stretch so count()-1 is
    // the right insert position for either.
    const int trackInsertIdx = m_tracksLayout->count() - 1;
    const int headerInsertIdx = m_headerLayout->count() - 1;
    m_tracksLayout->insertWidget(trackInsertIdx, track);
    m_headerLayout->insertWidget(headerInsertIdx,
        createTrackHeader(track, QString("A%1").arg(num), true));

    m_audioTracks.append(track);
    wireTrackSelection(track);
    updateInfoLabel();
}

void Timeline::addClip(const QString &filePath)
{
    AVFormatContext *fmt = nullptr;
    double duration = 0.0;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->duration > 0)
            duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        avformat_close_input(&fmt);
    }
    ClipInfo clip;
    clip.filePath = filePath;
    clip.displayName = QFileInfo(filePath).fileName();
    clip.duration = duration;
    // Every freshly-added clip gets a unique linkGroup shared by its video
    // and audio halves so the two stay locked together until the user
    // explicitly severs the sync via the clip's context menu.
    clip.linkGroup = allocateLinkGroup();

    // Import placement policy — user chooses between parallel stacking
    // (V1→V2→V3...) and append-to-first-track (V1/A1 continuous sequence).
    // Toggled from MainWindow's 環境設定 submenu.
    QSettings prefs("VSimpleEditor", "Preferences");
    const ImportPlacement placement = static_cast<ImportPlacement>(
        prefs.value("importPlacement", static_cast<int>(ImportPlacement::ParallelTrack)).toInt());

    int videoTrackIdx = -1;
    int audioTrackIdx = -1;
    if (placement == ImportPlacement::AppendToFirstTrack) {
        // Extend the existing sequence: always V1/A1, appending after
        // whatever clips already live there. Create V1/A1 if the project
        // starts with zero tracks.
        if (m_videoTracks.isEmpty())
            addVideoTrack();
        if (m_audioTracks.isEmpty())
            addAudioTrack();
        videoTrackIdx = 0;
        audioTrackIdx = 0;
    } else {
        // Premiere-style stacking: each drop lands on the first EMPTY video
        // track (creating V<n>/A<n> as needed). First drop fills V1/A1,
        // second fills V2/A2, and so on.
        for (int t = 0; t < m_videoTracks.size(); ++t) {
            if (m_videoTracks[t] && m_videoTracks[t]->clipCount() == 0) {
                videoTrackIdx = t;
                break;
            }
        }
        if (videoTrackIdx < 0) {
            addVideoTrack();
            videoTrackIdx = m_videoTracks.size() - 1;
        }
        for (int t = 0; t < m_audioTracks.size(); ++t) {
            if (m_audioTracks[t] && m_audioTracks[t]->clipCount() == 0) {
                audioTrackIdx = t;
                break;
            }
        }
        if (audioTrackIdx < 0) {
            addAudioTrack();
            audioTrackIdx = m_audioTracks.size() - 1;
        }
    }

    qInfo() << "Timeline::addClip routing video→V" << (videoTrackIdx + 1)
            << "audio→A" << (audioTrackIdx + 1)
            << "file=" << filePath;

    // Generate waveform async; apply to the AUDIO track that received the clip.
    auto *wfGen = new WaveformGenerator(this);
    QPointer<Timeline> self(this);
    const int waveAudioIdx = audioTrackIdx;
    connect(wfGen, &WaveformGenerator::waveformReady, this,
        [self, wfGen, waveAudioIdx](const QString &path, const WaveformData &data) {
            qInfo() << "Timeline::waveformReady slot peaks=" << data.peaks.size()
                    << "dur=" << data.duration << "audioIdx=" << waveAudioIdx;
            if (self && waveAudioIdx >= 0 && waveAudioIdx < self->m_audioTracks.size()) {
                auto *track = self->m_audioTracks[waveAudioIdx];
                if (track) {
                    auto clips = track->clips();
                    for (int i = 0; i < clips.size(); ++i) {
                        if (clips[i].filePath == path && clips[i].waveform.isEmpty()) {
                            clips[i].waveform = data;
                            track->setClips(clips);
                            break;
                        }
                    }
                }
            }
            wfGen->deleteLater();
        });
    wfGen->generateAsync(filePath);

    if (videoTrackIdx >= 0 && videoTrackIdx < m_videoTracks.size())
        m_videoTracks[videoTrackIdx]->addClip(clip);
    if (audioTrackIdx >= 0 && audioTrackIdx < m_audioTracks.size())
        m_audioTracks[audioTrackIdx]->addClip(clip);
    saveUndoState("Add clip");
    updateInfoLabel();
}

void Timeline::splitAtPlayhead()
{
    // Split only the clips the user has explicitly selected (on any track),
    // and only when the playhead lies inside that clip. This matches how
    // NLEs behave with a multi-selection: if you selected one clip you get
    // one cut; if nothing is selected, nothing happens.
    //
    // The right half of every cut gets a FRESH linkGroup so post-split
    // halves are treated as independent clips. A single shared map keeps
    // V_right and A_right in the same new group — the linked V/A pair
    // stays linked after the split, just with a new identity separate from
    // the left half.
    bool anySplit = false;
    QHash<int, int> oldGroupToNewGroup;
    auto splitTrack = [&](TimelineTrack *track) {
        if (!track) return;
        const QList<int> selected = track->selectedClips();
        if (selected.isEmpty()) return;
        QVector<ClipInfo> clips = track->clips();
        double accum = 0.0;
        for (int i = 0; i < clips.size(); ++i) {
            accum += clips[i].leadInSec;
            const double clipDur = clips[i].effectiveDuration();
            const double clipEnd = accum + clipDur;
            if (selected.contains(i)
                && m_playheadPos > accum + 0.05
                && m_playheadPos < clipEnd - 0.05) {
                track->splitClipAt(i, m_playheadPos - accum);
                auto updated = track->clips();
                if (i + 1 < updated.size()) {
                    const int oldGroup = updated[i + 1].linkGroup;
                    if (oldGroup > 0) {
                        auto it = oldGroupToNewGroup.find(oldGroup);
                        if (it == oldGroupToNewGroup.end())
                            it = oldGroupToNewGroup.insert(oldGroup, allocateLinkGroup());
                        updated[i + 1].linkGroup = it.value();
                        track->setClips(updated);
                    }
                }
                anySplit = true;
                return;
            }
            accum = clipEnd;
        }
    };
    for (auto *t : m_videoTracks) splitTrack(t);
    for (auto *t : m_audioTracks) splitTrack(t);
    if (anySplit) {
        saveUndoState("Split clip");
        updateInfoLabel();
    }
}

void Timeline::deleteSelectedClip()
{
    bool anyRemoved = false;
    // Walk every track and remove its selected clips (descending so indices
    // stay valid). Preserve downstream positions — we want the rest of the
    // sequence to stay put when a linked V/A pair is deleted out of the
    // middle of the timeline.
    auto deleteFrom = [&](TimelineTrack *t) {
        if (!t) return;
        QList<int> sel = t->selectedClips();
        if (sel.isEmpty()) return;
        std::sort(sel.begin(), sel.end(), std::greater<int>());
        for (int idx : sel) {
            t->removeClipPreservingDownstream(idx);
            anyRemoved = true;
        }
    };
    for (auto *t : m_videoTracks) deleteFrom(t);
    for (auto *t : m_audioTracks) deleteFrom(t);
    if (anyRemoved) {
        saveUndoState("Delete clip");
        updateInfoLabel();
    }
}

void Timeline::rippleDeleteSelectedClip() { deleteSelectedClip(); }
bool Timeline::hasSelection() const { return m_videoTrack->selectedClip() >= 0; }

bool Timeline::addTextOverlayToFirstVideoClip(const EnhancedTextOverlay &overlay)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    clips[0].textManager.addOverlay(overlay);
    track->setClips(clips);
    saveUndoState("Add text overlay");
    refreshTextStrip();
    return true;
}

bool Timeline::updateTextOverlayText(int overlayIndex, const QString &newText)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    auto &mgr = clips[0].textManager;
    if (overlayIndex < 0 || overlayIndex >= mgr.count())
        return false;
    mgr.overlay(overlayIndex).text = newText;
    track->setClips(clips);
    saveUndoState("Edit text overlay");
    refreshTextStrip();
    return true;
}

bool Timeline::updateTextOverlayRect(int overlayIndex, double x, double y, double width, double height)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    auto &mgr = clips[0].textManager;
    if (overlayIndex < 0 || overlayIndex >= mgr.count())
        return false;
    auto &ov = mgr.overlay(overlayIndex);
    ov.x = x;
    ov.y = y;
    ov.width = qMax(0.0, width);
    ov.height = qMax(0.0, height);
    track->setClips(clips);
    saveUndoState("Resize text overlay");
    refreshTextStrip();
    return true;
}

bool Timeline::setClipVideoTransform(int trackIdx, int clipIdx,
                                     double scale, double dx, double dy)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return false;
    auto *track = m_videoTracks[trackIdx];
    if (!track) return false;
    QVector<ClipInfo> clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return false;
    clips[clipIdx].videoScale = qBound(0.1, scale, 10.0);
    clips[clipIdx].videoDx = qBound(-5.0, dx, 5.0);
    clips[clipIdx].videoDy = qBound(-5.0, dy, 5.0);
    track->setClips(clips);
    emit sequenceChanged(computePlaybackSequence());
    return true;
}

bool Timeline::updateTextOverlayTime(int overlayIndex, double startTime, double endTime)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    auto &mgr = clips[0].textManager;
    if (overlayIndex < 0 || overlayIndex >= mgr.count())
        return false;
    mgr.overlay(overlayIndex).startTime = qMax(0.0, startTime);
    mgr.overlay(overlayIndex).endTime = qMax(startTime + 0.1, endTime);
    track->setClips(clips);
    // No undo snapshot during a live drag — otherwise every pixel of drag
    // pollutes the undo stack. Caller can add a snapshot on mouseRelease
    // if needed. For now, skip to keep the UX snappy.
    refreshTextStrip();
    return true;
}

void Timeline::refreshTextStrip()
{
    if (!m_textStrip)
        return;
    QVector<EnhancedTextOverlay> overlays;
    double clipDur = 0.0;
    if (!m_videoTracks.isEmpty() && m_videoTracks.first()) {
        const auto &clips = m_videoTracks.first()->clips();
        if (!clips.isEmpty()) {
            const auto &mgr = clips[0].textManager;
            for (int i = 0; i < mgr.count(); ++i)
                overlays.append(mgr.overlay(i));
            clipDur = clips[0].effectiveDuration();
        }
    }
    m_textStrip->setOverlays(overlays, clipDur);
    m_textStrip->setPixelsPerSecond(m_zoomLevel);
}

void Timeline::copySelectedClip()
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return;
    m_clipboard = m_videoTrack->clips()[sel];
}

void Timeline::pasteClip()
{
    if (!m_clipboard.has_value()) return;
    if (!m_videoTrack || !m_audioTrack) return;
    int insertAt = m_videoTrack->selectedClip() + 1;
    if (insertAt <= 0) insertAt = m_videoTrack->clipCount();
    const int maxIndex = qMin(m_videoTrack->clipCount(), m_audioTrack->clipCount());
    insertAt = qBound(0, insertAt, maxIndex);
    m_videoTrack->insertClip(insertAt, m_clipboard.value());
    m_audioTrack->insertClip(insertAt, m_clipboard.value());
    m_videoTrack->setSelectedClip(insertAt);
    m_audioTrack->setSelectedClip(insertAt);
    saveUndoState("Paste clip");
    updateInfoLabel();
}

void Timeline::cutSelectedClip()
{
    copySelectedClip();
    deleteSelectedClip();
}

void Timeline::unlinkClipGroup(int linkGroup)
{
    if (linkGroup <= 0) return;
    bool changed = false;
    auto strip = [&](TimelineTrack *t) {
        if (!t) return;
        auto clips = t->clips();
        bool local = false;
        for (auto &c : clips) {
            if (c.linkGroup == linkGroup) {
                c.linkGroup = 0;
                local = true;
            }
        }
        if (local) {
            t->setClips(clips);
            changed = true;
        }
    };
    for (auto *t : m_videoTracks) strip(t);
    for (auto *t : m_audioTracks) strip(t);
    if (changed) {
        saveUndoState("Unlink clip");
        updateInfoLabel();
    }
}

void Timeline::relinkClipAt(TimelineTrack *track, int clipIndex)
{
    if (!track || clipIndex < 0 || clipIndex >= track->clips().size()) return;

    const auto clickedClips = track->clips();
    const ClipInfo &clicked = clickedClips[clipIndex];
    if (clicked.linkGroup > 0) return;  // already linked — nothing to do

    auto clipTimelineStart = [](const QVector<ClipInfo> &clips, int idx) {
        double accum = 0.0;
        for (int i = 0; i <= idx && i < clips.size(); ++i) {
            accum += qMax(0.0, clips[i].leadInSec);
            if (i == idx) return accum;
            accum += clips[i].effectiveDuration();
        }
        return accum;
    };
    const double clickedStart = clipTimelineStart(clickedClips, clipIndex);

    // Find the best counterpart on the OPPOSITE track family (V→A or A→V).
    // Two-pass search: prefer UNLINKED candidates first (same filePath,
    // smallest |timelineStart - clickedStart|). Only fall back to an
    // already-linked candidate if no unlinked match exists — otherwise a
    // duplicate-imported clip or a split-clip neighbor could silently steal
    // the re-sync from the real counterpart.
    const bool clickedIsVideo = m_videoTracks.contains(track);
    const auto &oppositeTracks = clickedIsVideo ? m_audioTracks : m_videoTracks;

    struct Hit { TimelineTrack *track = nullptr; int idx = -1; double start = 0.0; };
    auto findBest = [&](bool requireUnlinked) {
        Hit best;
        double bestDist = 1e18;
        for (auto *t : oppositeTracks) {
            if (!t) continue;
            const auto &clips = t->clips();
            double accum = 0.0;
            for (int i = 0; i < clips.size(); ++i) {
                accum += qMax(0.0, clips[i].leadInSec);
                const double start = accum;
                accum += clips[i].effectiveDuration();
                if (clips[i].filePath != clicked.filePath) continue;
                if (requireUnlinked && clips[i].linkGroup > 0) continue;
                const double dist = qAbs(start - clickedStart);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = {t, i, start};
                }
            }
        }
        return best;
    };

    Hit hit = findBest(/*requireUnlinked=*/true);
    if (!hit.track) hit = findBest(/*requireUnlinked=*/false);
    if (!hit.track) return;

    // Snap the counterpart's timelineStart onto the clicked clip's start by
    // shifting its leadInSec, and compensate the NEXT clip's leadInSec so
    // downstream clips stay put (same trick as the linked-drag path). If
    // the move is clamped at 0 because the counterpart butts up against a
    // preceding neighbor, we settle for a partial alignment — the clips are
    // still re-grouped so further linked drags will track together.
    {
        auto clips = hit.track->clips();
        const double delta = clickedStart - hit.start;
        const double oldLeadIn = clips[hit.idx].leadInSec;
        const double newLeadIn = qMax(0.0, oldLeadIn + delta);
        const double applied = newLeadIn - oldLeadIn;
        clips[hit.idx].leadInSec = newLeadIn;
        if (hit.idx + 1 < clips.size()) {
            clips[hit.idx + 1].leadInSec =
                qMax(0.0, clips[hit.idx + 1].leadInSec - applied);
        }
        hit.track->setClips(clips);
    }

    const int targetGroup = (hit.track->clips()[hit.idx].linkGroup > 0)
        ? hit.track->clips()[hit.idx].linkGroup
        : allocateLinkGroup();

    auto stamp = [targetGroup](TimelineTrack *t, int idx) {
        auto clips = t->clips();
        if (idx < 0 || idx >= clips.size()) return;
        clips[idx].linkGroup = targetGroup;
        t->setClips(clips);
    };
    stamp(track, clipIndex);
    stamp(hit.track, hit.idx);

    saveUndoState("Relink clip");
    emit sequenceChanged(computePlaybackSequence());
    emit audioSequenceChanged(computeAudioPlaybackSequence());
    updateInfoLabel();
}

void Timeline::showClipContextMenu(TimelineTrack *track, int clipIndex, const QPoint &globalPos)
{
    if (!track || clipIndex < 0 || clipIndex >= track->clips().size()) return;

    // Make sure the right-clicked clip is the active selection — NLE users
    // expect "right click acts on the thing under the cursor".
    if (!track->isClipSelected(clipIndex))
        track->setSelectedClip(clipIndex);

    const int linkGroup = track->clips()[clipIndex].linkGroup;
    QMenu menu;
    QAction *cutAct = menu.addAction(QStringLiteral("カット"));
    QAction *copyAct = menu.addAction(QStringLiteral("コピー"));
    QAction *deleteAct = menu.addAction(QStringLiteral("削除"));
    menu.addSeparator();
    QAction *unlinkAct = menu.addAction(QStringLiteral("同期を切る"));
    unlinkAct->setEnabled(linkGroup > 0);
    QAction *relinkAct = menu.addAction(QStringLiteral("再同期"));
    relinkAct->setEnabled(linkGroup == 0);

    QAction *chosen = menu.exec(globalPos);
    if (!chosen) return;
    if (chosen == cutAct) cutSelectedClip();
    else if (chosen == copyAct) copySelectedClip();
    else if (chosen == deleteAct) deleteSelectedClip();
    else if (chosen == unlinkAct) unlinkClipGroup(linkGroup);
    else if (chosen == relinkAct) relinkClipAt(track, clipIndex);
}

void Timeline::undo()
{
    if (!canUndo()) return;
    restoreState(m_undoManager->undo());
    updateInfoLabel();
}

void Timeline::redo()
{
    if (!canRedo()) return;
    restoreState(m_undoManager->redo());
    updateInfoLabel();
}

bool Timeline::canUndo() const { return m_undoManager->canUndo(); }
bool Timeline::canRedo() const { return m_undoManager->canRedo(); }

void Timeline::setSnapEnabled(bool enabled)
{
    for (auto *t : m_videoTracks) t->setSnapEnabled(enabled);
    for (auto *t : m_audioTracks) t->setSnapEnabled(enabled);
    updateInfoLabel();
}

bool Timeline::snapEnabled() const {
    return m_videoTrack ? m_videoTrack->snapEnabled() : true;
}

// Zoom
void Timeline::zoomIn() { setZoomLevel(m_zoomLevel * 1.25); }
void Timeline::zoomOut() { setZoomLevel(m_zoomLevel / 1.25); }

void Timeline::captureZoomAnchor()
{
    // Record the playhead's current viewport column (or the viewport center
    // if the playhead is offscreen). setZoomLevel uses this as a fixed
    // reference point so the playhead doesn't slide sideways while the user
    // drags the ruler to zoom.
    if (!m_scrollArea || !m_scrollArea->viewport() || !m_videoTrack) {
        m_zoomAnchorViewportX = -1;
        return;
    }
    const QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
    const int scrollX = hbar ? hbar->value() : 0;
    const int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 0) {
        m_zoomAnchorViewportX = -1;
        return;
    }
    const int playheadX = m_videoTrack->secondsToX(m_playheadPos);
    if (playheadX >= scrollX && playheadX <= scrollX + viewportW)
        m_zoomAnchorViewportX = playheadX - scrollX;
    else
        m_zoomAnchorViewportX = viewportW / 2;
}

void Timeline::clearZoomAnchor()
{
    m_zoomAnchorViewportX = -1;
}

void Timeline::setZoomLevel(double pixelsPerSecond)
{
    m_zoomLevel = qBound(0.02, pixelsPerSecond, 200.0);
    for (auto *t : m_videoTracks) t->setPixelsPerSecond(m_zoomLevel);
    for (auto *t : m_audioTracks) t->setPixelsPerSecond(m_zoomLevel);
    if (m_timeRuler)
        m_timeRuler->setPixelsPerSecond(m_zoomLevel);
    if (m_textStrip)
        m_textStrip->setPixelsPerSecond(m_zoomLevel);

    if (m_zoomAnchorViewportX >= 0 && m_scrollArea && m_videoTrack) {
        // Force the scroll area to refresh its content width synchronously so
        // the horizontal scrollbar's max is current before setValue(). We
        // scope the layout activation and LayoutRequest to the scroll area so
        // it can't ripple into unrelated widgets.
        if (m_tracksWidget && m_tracksWidget->layout())
            m_tracksWidget->layout()->activate();
        if (QWidget *content = m_scrollArea->widget()) {
            if (content->layout()) content->layout()->activate();
            content->adjustSize();
        }
        QEvent layoutEvt(QEvent::LayoutRequest);
        QCoreApplication::sendEvent(m_scrollArea, &layoutEvt);

        // IMPORTANT: do NOT read hbar->value() here. The anchor is the
        // FIXED column captured at drag start; feeding the current (possibly
        // clamped) scroll value back into the anchor would create a runaway
        // loop on rapid drag events.
        if (QScrollBar *hbar = m_scrollArea->horizontalScrollBar()) {
            const int newPlayheadX = m_videoTrack->secondsToX(m_playheadPos);
            int target = newPlayheadX - m_zoomAnchorViewportX;
            if (target < 0) target = 0;
            hbar->setValue(target);
        }
        // Overlay update without auto-scroll (syncPlayheadOverlay would undo
        // our anchored scroll by re-centering on the playhead).
        if (m_playheadOverlay)
            m_playheadOverlay->setPlayheadX(m_videoTrack->secondsToX(m_playheadPos));
    } else {
        syncPlayheadOverlay();
    }
    updateInfoLabel();
}

// I/O markers
void Timeline::markIn() { m_markIn = m_playheadPos; updateInfoLabel(); }
void Timeline::markOut() { m_markOut = m_playheadPos; updateInfoLabel(); }

// Clip speed
void Timeline::setClipSpeed(double speed)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    speed = qMax(0.25, qMin(4.0, speed));

    // Modify clips directly (need mutable access)
    auto videoClips = m_videoTrack->clips();
    auto audioClips = m_audioTrack->clips();
    videoClips[sel].speed = speed;
    if (sel < audioClips.size()) audioClips[sel].speed = speed;
    m_videoTrack->setClips(videoClips);
    m_audioTrack->setClips(audioClips);

    saveUndoState(QString("Set speed %1x").arg(speed));
    updateInfoLabel();
}

void Timeline::setClipVolume(double volume)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    volume = qMax(0.0, qMin(2.0, volume));
    auto audioClips = m_audioTrack->clips();
    if (sel < audioClips.size()) {
        audioClips[sel].volume = volume;
        m_audioTrack->setClips(audioClips);
    }
    saveUndoState(QString("Set volume %1%").arg(static_cast<int>(volume * 100)));
}

// --- Phase 3: Color correction, effects, keyframes ---

void Timeline::setClipColorCorrection(const ColorCorrection &cc)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    clips[sel].colorCorrection = cc;
    m_videoTrack->setClips(clips);
    saveUndoState("Color correction");
}

void Timeline::setClipEffects(const QVector<VideoEffect> &effects)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    clips[sel].effects = effects;
    m_videoTrack->setClips(clips);
    saveUndoState("Video effects");
}

void Timeline::setClipKeyframes(const KeyframeManager &km)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    clips[sel].keyframes = km;
    m_videoTrack->setClips(clips);
    saveUndoState("Keyframes");
}

ColorCorrection Timeline::clipColorCorrection() const
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return {};
    return m_videoTrack->clips()[sel].colorCorrection;
}

QVector<VideoEffect> Timeline::clipEffects() const
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return {};
    return m_videoTrack->clips()[sel].effects;
}

KeyframeManager Timeline::clipKeyframes() const
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return {};
    return m_videoTrack->clips()[sel].keyframes;
}

double Timeline::selectedClipDuration() const
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return 0.0;
    return m_videoTrack->clips()[sel].effectiveDuration();
}

void Timeline::addAudioFile(const QString &filePath)
{
    AVFormatContext *fmt = nullptr;
    double duration = 0.0;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0)
            duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        avformat_close_input(&fmt);
    }
    ClipInfo clip;
    clip.filePath = filePath;
    clip.displayName = QFileInfo(filePath).fileName();
    clip.duration = duration;
    // Solo audio imports still get a unique linkGroup so they participate
    // in the selection/drag/delete plumbing; they just have no partner.
    clip.linkGroup = allocateLinkGroup();

    // Add to first audio track (or second if exists for BGM)
    TimelineTrack *target = m_audioTracks.size() > 1 ? m_audioTracks[1] : m_audioTrack;
    target->addClip(clip);
    saveUndoState("Add audio");
    updateInfoLabel();
}

void Timeline::toggleMuteTrack(int audioTrackIndex)
{
    if (audioTrackIndex < 0 || audioTrackIndex >= m_audioTracks.size()) return;
    auto *track = m_audioTracks[audioTrackIndex];
    track->setMuted(!track->isMuted());
    updateInfoLabel();
}

void Timeline::toggleSoloTrack(int audioTrackIndex)
{
    if (audioTrackIndex < 0 || audioTrackIndex >= m_audioTracks.size()) return;
    bool newSolo = !m_audioTracks[audioTrackIndex]->isSolo();
    // Clear all solo first, then set the target
    for (auto *t : m_audioTracks) t->setSolo(false);
    if (newSolo) m_audioTracks[audioTrackIndex]->setSolo(true);
    updateInfoLabel();
}

void Timeline::setPlayheadPosition(double seconds)
{
    m_playheadPos = qMax(0.0, seconds);
    syncPlayheadOverlay();
}

double Timeline::totalDuration() const
{
    // Maximum end time across ALL video tracks. Each track lays its clips out
    // sequentially from t=0, so each track's contribution is the sum of its
    // clip effective durations. Total sequence length is the longest track.
    double maxEnd = 0.0;
    for (auto *track : m_videoTracks) {
        if (!track) continue;
        double accum = 0.0;
        for (const auto &c : track->clips())
            accum += c.leadInSec + c.effectiveDuration();
        if (accum > maxEnd) maxEnd = accum;
    }
    return maxEnd;
}

void Timeline::onTrackClipClicked(int index)
{
    m_audioTrack->setSelectedClip(index);
    emit clipSelected(index);
}

bool Timeline::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton
            && !(me->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
            clearAllSelections();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Timeline::wireTrackSelection(TimelineTrack *track)
{
    connect(track, &TimelineTrack::selectionChanged, this, [this, track](int index, bool additive) {
        if (m_inLinkedSelectionSync) return;
        m_inLinkedSelectionSync = true;

        // Non-additive click clears every OTHER track first so at most one
        // clip (or one link group) is selected across the whole timeline.
        if (!additive && index >= 0) {
            auto clearOther = [track](TimelineTrack *t) {
                if (!t || t == track || t->selectedClip() < 0) return;
                t->setSelectedClip(-1);
            };
            for (auto *t : m_videoTracks) clearOther(t);
            for (auto *t : m_audioTracks) clearOther(t);
        }

        // Propagate selection to every clip that shares a non-zero linkGroup
        // so the linked V/A pair (and anything else the user has grouped)
        // moves as a unit.
        if (index >= 0 && index < track->clips().size()) {
            const int linkGroup = track->clips()[index].linkGroup;
            if (linkGroup > 0) {
                auto syncLinked = [linkGroup, additive](TimelineTrack *t) {
                    if (!t) return;
                    const auto &clips = t->clips();
                    for (int i = 0; i < clips.size(); ++i) {
                        if (clips[i].linkGroup == linkGroup && !t->isClipSelected(i)) {
                            if (additive) t->toggleClipSelection(i);
                            else t->setSelectedClip(i);
                        }
                    }
                };
                for (auto *t : m_videoTracks) if (t != track) syncLinked(t);
                for (auto *t : m_audioTracks) if (t != track) syncLinked(t);
            }
        }

        m_inLinkedSelectionSync = false;
        emit clipSelected(index);
    });
    connect(track, &TimelineTrack::emptyAreaClicked, this, [this]() {
        clearAllSelections();
    });
    connect(track, &TimelineTrack::modified, this, &Timeline::onTrackModified);
    connect(track, &TimelineTrack::clipContextMenuRequested, this,
        [this, track](int clipIndex, const QPoint &globalPos) {
            showClipContextMenu(track, clipIndex, globalPos);
        });
    connect(track, &TimelineTrack::linkedDragStarted, this,
        [this, track](int clipIdx) {
            captureLinkedDragPartners(track, clipIdx);
        });
    connect(track, &TimelineTrack::linkedDragDelta, this,
        [this](int, double rawDeltaSec) {
            applyLinkedDragDelta(rawDeltaSec);
        });
    connect(track, &TimelineTrack::linkedDragCancelled, this, [this]() {
        revertLinkedDragPartners();
        clearLinkedDragState();
    });
    connect(track, &TimelineTrack::crossTrackDropped, this,
        [this, track](int /*insertIdx*/, int linkGroup, double dropTime) {
            handleCrossTrackLinkedDrop(track, linkGroup, dropTime);
        });
}

void Timeline::captureLinkedDragPartners(TimelineTrack *source, int clipIdx)
{
    clearLinkedDragState();
    if (!source || clipIdx < 0 || clipIdx >= source->clips().size()) return;
    const int linkGroup = source->clips()[clipIdx].linkGroup;
    if (linkGroup <= 0) return;
    // Include the source clip in the partners list so Timeline is the
    // single authoritative applier — simpler than splitting the math
    // between TimelineTrack and Timeline.
    auto capture = [&](TimelineTrack *t) {
        if (!t) return;
        const auto &clips = t->clips();
        for (int i = 0; i < clips.size(); ++i) {
            if (clips[i].linkGroup == linkGroup) {
                LinkedDragPartner p;
                p.track = t;
                p.clipIdx = i;
                p.origLeadIn = clips[i].leadInSec;
                p.origLeadInNext = (i + 1 < clips.size())
                    ? clips[i + 1].leadInSec
                    : -1.0;
                m_linkedDragPartners.append(p);
            }
        }
    };
    for (auto *t : m_videoTracks) capture(t);
    for (auto *t : m_audioTracks) capture(t);
}

void Timeline::applyLinkedDragDelta(double rawDeltaSec)
{
    if (m_linkedDragPartners.isEmpty()) return;
    // Global clamp: the tightest allowed delta across every partner, so
    // no partner crosses its own neighbor even when others have more room.
    double minDelta = -1e18;
    double maxDelta = 1e18;
    for (const auto &p : m_linkedDragPartners) {
        const double lower = -p.origLeadIn;
        const double upper = (p.origLeadInNext >= 0.0) ? p.origLeadInNext : 1e18;
        if (lower > minDelta) minDelta = lower;
        if (upper < maxDelta) maxDelta = upper;
    }
    double clamped = rawDeltaSec;
    if (clamped < minDelta) clamped = minDelta;
    if (clamped > maxDelta) clamped = maxDelta;

    for (const auto &p : m_linkedDragPartners) {
        if (!p.track) continue;
        const double newLeadIn = p.origLeadIn + clamped;
        const double newNextLeadIn = (p.origLeadInNext >= 0.0)
            ? p.origLeadInNext - clamped
            : -1.0;
        p.track->applyDragMove(p.clipIdx, newLeadIn, newNextLeadIn);
    }
}

void Timeline::revertLinkedDragPartners()
{
    for (const auto &p : m_linkedDragPartners) {
        if (!p.track) continue;
        p.track->applyDragMove(p.clipIdx, p.origLeadIn, p.origLeadInNext);
    }
}

void Timeline::clearLinkedDragState()
{
    m_linkedDragPartners.clear();
}

void Timeline::handleCrossTrackLinkedDrop(TimelineTrack *destTrack, int linkGroup, double dropTime)
{
    if (linkGroup <= 0 || !destTrack) return;

    // Determine if dest is a video or audio track, and its index
    int destVideoIdx = m_videoTracks.indexOf(destTrack);
    int destAudioIdx = m_audioTracks.indexOf(destTrack);
    bool destIsVideo = (destVideoIdx >= 0);
    int destIdx = destIsVideo ? destVideoIdx : destAudioIdx;

    // Find the partner clips on the OTHER track type that share this linkGroup
    auto &partnerTracks = destIsVideo ? m_audioTracks : m_videoTracks;

    for (int ti = 0; ti < partnerTracks.size(); ++ti) {
        TimelineTrack *srcPartnerTrack = partnerTracks[ti];
        const auto &clips = srcPartnerTrack->clips();
        for (int ci = 0; ci < clips.size(); ++ci) {
            if (clips[ci].linkGroup != linkGroup) continue;

            // Found the linked partner. Determine the target partner track.
            // If dest is V[n], partner should go to A[n] (create if needed).
            // If dest is A[n], partner should go to V[n] (create if needed).
            auto &targetTracks = destIsVideo ? m_audioTracks : m_videoTracks;
            while (targetTracks.size() <= destIdx) {
                if (destIsVideo)
                    addAudioTrack();
                else
                    addVideoTrack();
            }
            TimelineTrack *targetTrack = targetTracks[destIdx];

            // If the partner is already on the target track, nothing to do
            if (srcPartnerTrack == targetTrack) return;

            // Copy the partner clip data and remove from source
            const ClipInfo partnerClip = srcPartnerTrack->clips()[ci];
            const double partnerDur = partnerClip.effectiveDuration();

            // Use planDrop on the target track to find the right insertion point.
            // Fall back to appending at end if the exact position can't fit.
            auto plan = targetTrack->planDrop(dropTime, partnerDur);
            if (!plan.valid) {
                // Append at end of track as fallback
                plan.valid = true;
                plan.insertIdx = targetTrack->clipCount();
                plan.newLeadIn = dropTime;
                // Subtract accumulated duration of existing clips
                double accum = 0.0;
                for (const auto &c : targetTrack->clips())
                    accum += c.leadInSec + c.effectiveDuration();
                plan.newLeadIn = qMax(0.0, dropTime - accum);
            }

            srcPartnerTrack->removeClipPreservingDownstream(ci);
            targetTrack->insertClipPreservingDownstream(plan.insertIdx, partnerClip, plan.newLeadIn);
            return;  // Only one partner per linkGroup on the other track type
        }
    }
}

void Timeline::clearAllSelections()
{
    bool changed = false;
    auto clearOne = [&changed](TimelineTrack *t) {
        if (!t || t->selectedClip() < 0) return;
        const bool was = t->blockSignals(true);
        t->setSelectedClip(-1);
        t->blockSignals(was);
        t->update();
        changed = true;
    };
    for (auto *t : m_videoTracks) clearOne(t);
    for (auto *t : m_audioTracks) clearOne(t);
    if (changed) emit clipSelected(-1);
}

void Timeline::onTrackModified()
{
    // Any clip add/remove/move/trim/split bubbles up here. Auto-fit the zoom
    // first so the timeline widget never exceeds the viewport width (otherwise
    // long-form sequences hit the kMaxWidth hard cap and tail clips get
    // visually clipped). Then recompute the flat playback schedule.
    ensureSequenceFitsViewport();
    emit sequenceChanged(computePlaybackSequence());
    emit audioSequenceChanged(computeAudioPlaybackSequence());
}

void Timeline::setTrackHeight(int h)
{
    h = qBound(30, h, 200);
    if (h == m_trackHeight) return;
    m_trackHeight = h;
    for (auto *t : m_videoTracks) if (t) t->setRowHeight(m_trackHeight);
    for (auto *t : m_audioTracks) if (t) t->setRowHeight(m_trackHeight);
    // Preserve a user-customized text row height — if the user has
    // dragged the T1 row's bottom edge, don't clobber that override when
    // the global V/A row height changes.
    if (m_textStrip && m_textStripCustomHeight <= 0)
        m_textStrip->setFixedHeight(m_trackHeight);
    if (m_headerLayout) {
        for (int i = 0; i < m_headerLayout->count(); ++i) {
            if (auto *item = m_headerLayout->itemAt(i)) {
                if (auto *hw = item->widget()) {
                    // Keep the V/A separator at its fixed 3 px height so
                    // it doesn't balloon with the rest of the headers.
                    if (hw == m_vaSeparatorHeader)
                        continue;
                    if (hw == m_textStripHeader && m_textStripCustomHeight > 0)
                        continue;
                    hw->setFixedHeight(m_trackHeight);
                }
            }
        }
    }
    updateInfoLabel();
}

void Timeline::increaseTrackHeight() { setTrackHeight(m_trackHeight + 10); }
void Timeline::decreaseTrackHeight() { setTrackHeight(m_trackHeight - 10); }

void Timeline::ensureSequenceFitsViewport()
{
    if (!m_scrollArea || !m_scrollArea->viewport())
        return;
    const double total = totalDuration();
    if (total <= 0.0)
        return;
    const int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 100)
        return;

    // Leave a bit of right-edge margin so the last clip end is visible.
    const int safeW = qMax(100, viewportW - 60);

    // Required pps to make the entire sequence fit safeW horizontally.
    const double requiredPps = static_cast<double>(safeW) / total;
    const double newPps = qBound(0.02, requiredPps, 200.0);

    // Only auto-zoom OUT — never zoom in (that's the user's prerogative).
    if (newPps < m_zoomLevel - 1e-6) {
        qInfo() << "Timeline::ensureSequenceFitsViewport autoZoom"
                << m_zoomLevel << "->" << newPps
                << "totalSec=" << total << "viewportW=" << viewportW;
        setZoomLevel(newPps);
    }
}

QVector<PlaybackEntry> Timeline::computePlaybackSequence() const
{
    QVector<PlaybackEntry> result;
    if (m_videoTracks.isEmpty())
        return result;

    // PiP overlay resolution: every visible track contributes its full clip
    // intervals without mutual subtraction. The compositor stacks layers using
    // sourceTrack (V1 = front, higher = back). VideoPlayer's findActiveEntryAt
    // walks the sorted sequence in order, so the (timelineStart, sourceTrack
    // asc) sort below keeps V1 as the "primary" entry when multiple overlap.

    struct Interval {
        double timelineStart;
        double timelineEnd;
        double clipIn;
        double clipOut;
        double speed;
        QString filePath;
        int trackIdx;
        // US-T35 per-clip video source transform + owning clip index for
        // round-trip updates when the user drags the preview transform.
        double videoScale = 1.0;
        double videoDx = 0.0;
        double videoDy = 0.0;
        double opacity = 1.0;
        double volume = 1.0;
        int clipIdx = -1;
    };

    QVector<QVector<Interval>> trackIntervals;
    trackIntervals.reserve(m_videoTracks.size());
    for (int t = 0; t < m_videoTracks.size(); ++t) {
        QVector<Interval> ivs;
        auto *track = m_videoTracks[t];
        // Skip hidden video tracks entirely — they contribute nothing to playback.
        if (track && !track->isHidden()) {
            const auto &clips = track->clips();
            double accum = 0.0;
            for (int ci = 0; ci < clips.size(); ++ci) {
                const auto &c = clips[ci];
                accum += qMax(0.0, c.leadInSec); // leading gap before this clip
                const double clipDur = c.effectiveDuration();
                if (clipDur <= 0.0) continue;
                Interval iv;
                iv.timelineStart = accum;
                iv.timelineEnd = accum + clipDur;
                iv.clipIn = c.inPoint;
                iv.clipOut = (c.outPoint > 0.0) ? c.outPoint : c.duration;
                iv.speed = (c.speed > 0.0) ? c.speed : 1.0;
                iv.filePath = c.filePath;
                iv.trackIdx = t;
                iv.videoScale = c.videoScale;
                iv.videoDx = c.videoDx;
                iv.videoDy = c.videoDy;
                iv.opacity = c.opacity;
                iv.volume = c.volume;
                iv.clipIdx = ci;
                ivs.append(iv);
                accum += clipDur;
            }
        }
        trackIntervals.append(ivs);
    }

    QVector<Interval> visible;
    for (const auto &trackClips : trackIntervals) {
        for (const auto &iv : trackClips)
            visible.append(iv);
    }

    // (timelineStart, sourceTrack asc) so overlapping entries at the same time
    // list V1 first. findActiveEntryAt returns the first match, giving V1 the
    // "primary" playback role until the multi-layer compositor lands.
    std::sort(visible.begin(), visible.end(),
              [](const Interval &a, const Interval &b) {
                  if (a.timelineStart != b.timelineStart)
                      return a.timelineStart < b.timelineStart;
                  return a.trackIdx < b.trackIdx;
              });

    result.reserve(visible.size());
    for (const auto &iv : visible) {
        if (iv.timelineEnd - iv.timelineStart < 1e-6) continue;
        PlaybackEntry e;
        e.filePath = iv.filePath;
        e.clipIn = iv.clipIn;
        e.clipOut = iv.clipOut;
        e.timelineStart = iv.timelineStart;
        e.timelineEnd = iv.timelineEnd;
        e.speed = iv.speed;
        e.sourceTrack = iv.trackIdx;
        e.videoScale = iv.videoScale;
        e.videoDx = iv.videoDx;
        e.videoDy = iv.videoDy;
        e.opacity = iv.opacity;
        e.volume = iv.volume;
        e.sourceClipIndex = iv.clipIdx;
        qInfo() << "[SEQ] entry idx=" << result.size()
                << "tl=[" << iv.timelineStart << "," << iv.timelineEnd << "]"
                << "clip=[" << iv.clipIn << "," << iv.clipOut << "]"
                << "track=" << iv.trackIdx << "file=" << iv.filePath;
        // Audio routing: the corresponding A<n> track at the same index acts as
        // the audio mute switch for this entry's media file.
        if (iv.trackIdx >= 0 && iv.trackIdx < m_audioTracks.size()
            && m_audioTracks[iv.trackIdx] && m_audioTracks[iv.trackIdx]->isMuted()) {
            e.audioMuted = true;
        }
        result.append(e);
    }
    return result;
}

QVector<PlaybackEntry> Timeline::computeAudioPlaybackSequence() const
{
    // A1-wins resolution across A1..An, mirroring computePlaybackSequence's
    // V1-wins video logic. A1 is the primary audio layer; A2 only plays in
    // time ranges where A1 has a gap; A3 only fills A1+A2 gaps; and so on.
    // This is what lets V2 "fill" clips produce audible audio: when V2 is
    // showing a clip, the corresponding A2 clip (created together at import)
    // fills A1's gap and the audio schedule emits an A2 entry.
    QVector<PlaybackEntry> result;
    if (m_audioTracks.isEmpty())
        return result;

    struct Interval {
        double timelineStart;
        double timelineEnd;
        double clipIn;
        double clipOut;
        double speed;
        QString filePath;
        int trackIdx;
        bool muted;
        double volume = 1.0;
        int clipIdx = -1;
    };

    QVector<QVector<Interval>> trackIntervals;
    trackIntervals.reserve(m_audioTracks.size());
    for (int t = 0; t < m_audioTracks.size(); ++t) {
        QVector<Interval> ivs;
        auto *track = m_audioTracks[t];
        if (track && !track->isHidden()) {
            const auto &clips = track->clips();
            double accum = 0.0;
            for (int ci = 0; ci < clips.size(); ++ci) {
                const auto &c = clips[ci];
                accum += qMax(0.0, c.leadInSec);
                const double clipDur = c.effectiveDuration();
                if (clipDur <= 0.0) continue;
                Interval iv;
                iv.timelineStart = accum;
                iv.timelineEnd = accum + clipDur;
                iv.clipIn = c.inPoint;
                iv.clipOut = (c.outPoint > 0.0) ? c.outPoint : c.duration;
                iv.speed = (c.speed > 0.0) ? c.speed : 1.0;
                iv.filePath = c.filePath;
                iv.trackIdx = t;
                iv.muted = track->isMuted();
                iv.volume = c.volume;
                iv.clipIdx = ci;
                ivs.append(iv);
                accum += clipDur;
            }
        }
        trackIntervals.append(ivs);
    }

    auto subtractRange = [](const QVector<Interval> &input,
                            double cutStart, double cutEnd) -> QVector<Interval> {
        QVector<Interval> out;
        out.reserve(input.size());
        for (const auto &iv : input) {
            if (iv.timelineEnd <= cutStart || iv.timelineStart >= cutEnd) {
                out.append(iv);
                continue;
            }
            if (iv.timelineStart < cutStart) {
                Interval left = iv;
                left.timelineEnd = cutStart;
                left.clipOut = iv.clipIn + (cutStart - iv.timelineStart) * iv.speed;
                out.append(left);
            }
            if (iv.timelineEnd > cutEnd) {
                Interval right = iv;
                right.timelineStart = cutEnd;
                right.clipIn = iv.clipIn + (cutEnd - iv.timelineStart) * iv.speed;
                out.append(right);
            }
        }
        return out;
    };

    QVector<Interval> visible;
    for (int t = 0; t < trackIntervals.size(); ++t) {
        QVector<Interval> trackClips = trackIntervals[t];
        for (const auto &existing : visible) {
            trackClips = subtractRange(trackClips, existing.timelineStart, existing.timelineEnd);
        }
        for (const auto &iv : trackClips)
            visible.append(iv);
    }

    std::sort(visible.begin(), visible.end(),
              [](const Interval &a, const Interval &b) {
                  return a.timelineStart < b.timelineStart;
              });

    result.reserve(visible.size());
    for (const auto &iv : visible) {
        if (iv.timelineEnd - iv.timelineStart < 1e-6) continue;
        PlaybackEntry e;
        e.filePath = iv.filePath;
        e.clipIn = iv.clipIn;
        e.clipOut = iv.clipOut;
        e.timelineStart = iv.timelineStart;
        e.timelineEnd = iv.timelineEnd;
        e.speed = iv.speed;
        e.sourceTrack = iv.trackIdx;
        e.audioMuted = iv.muted;
        e.volume = iv.volume;
        e.sourceClipIndex = iv.clipIdx;
        result.append(e);
    }
    return result;
}

void Timeline::refreshPlaybackSequence()
{
    // External hook for proxy generation / proxy mode toggle. The clip graph
    // hasn't changed, but the resolved playback paths have, so just re-emit
    // the same sequences we'd produce for any other rebuild trigger and let
    // MainWindow's getProxyPath translation in the sequenceChanged handler
    // pick up the now-Ready proxies.
    emit sequenceChanged(computePlaybackSequence());
    emit audioSequenceChanged(computeAudioPlaybackSequence());
}

void Timeline::saveUndoState(const QString &description)
{
    m_undoManager->saveState(currentState(), description);
}

TimelineState Timeline::currentState() const
{
    TimelineState state;
    state.videoClips = m_videoTrack->clips();
    state.audioClips = m_audioTrack->clips();
    state.selectedClip = m_videoTrack->selectedClip();
    state.playheadPos = m_playheadPos;
    return state;
}

void Timeline::restoreState(const TimelineState &state)
{
    m_videoTrack->setClips(state.videoClips);
    m_audioTrack->setClips(state.audioClips);
    {
        const bool vWas = m_videoTrack->blockSignals(true);
        m_videoTrack->setSelectedClip(state.selectedClip);
        m_videoTrack->blockSignals(vWas);
        m_videoTrack->update();
        const bool aWas = m_audioTrack->blockSignals(true);
        m_audioTrack->setSelectedClip(state.selectedClip);
        m_audioTrack->blockSignals(aWas);
        m_audioTrack->update();
    }
    m_playheadPos = state.playheadPos;
    syncPlayheadOverlay();
    emit clipSelected(state.selectedClip);
    // setClips bypasses the modified() signal path; trigger explicitly so the
    // VideoPlayer rebuilds its sequence after undo/redo.
    emit sequenceChanged(computePlaybackSequence());
    emit audioSequenceChanged(computeAudioPlaybackSequence());
}

// --- Project save/load ---

QVector<QVector<ClipInfo>> Timeline::allVideoTracks() const
{
    QVector<QVector<ClipInfo>> result;
    for (const auto *t : m_videoTracks)
        result.append(t->clips());
    return result;
}

QVector<QVector<ClipInfo>> Timeline::allAudioTracks() const
{
    QVector<QVector<ClipInfo>> result;
    for (const auto *t : m_audioTracks)
        result.append(t->clips());
    return result;
}

void Timeline::restoreFromProject(const QVector<QVector<ClipInfo>> &videoTracks,
                                   const QVector<QVector<ClipInfo>> &audioTracks,
                                   double playhead, double markInVal, double markOutVal, int zoom)
{
    // Set first video/audio track clips
    if (!videoTracks.isEmpty())
        m_videoTrack->setClips(videoTracks[0]);
    if (!audioTracks.isEmpty())
        m_audioTrack->setClips(audioTracks[0]);

    // Add extra video tracks
    for (int i = 1; i < videoTracks.size(); ++i) {
        if (i >= m_videoTracks.size()) addVideoTrack();
        m_videoTracks[i]->setClips(videoTracks[i]);
    }

    // Add extra audio tracks
    for (int i = 1; i < audioTracks.size(); ++i) {
        if (i >= m_audioTracks.size()) addAudioTrack();
        m_audioTracks[i]->setClips(audioTracks[i]);
    }

    m_playheadPos = playhead;
    m_markIn = markInVal;
    m_markOut = markOutVal;
    setZoomLevel(zoom);
    syncPlayheadOverlay();
    saveUndoState("Load project");
    updateInfoLabel();
    // setClips bypasses modified(); trigger sequence rebuild explicitly.
    emit sequenceChanged(computePlaybackSequence());
    emit audioSequenceChanged(computeAudioPlaybackSequence());
}

void Timeline::syncPlayheadOverlay()
{
    if (!m_playheadOverlay || !m_videoTrack)
        return;

    const int playheadX = m_videoTrack->secondsToX(m_playheadPos);
    m_playheadOverlay->setPlayheadX(playheadX);

    if (m_scrollArea) {
        QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
        if (hbar) {
            const int viewportW = m_scrollArea->viewport() ? m_scrollArea->viewport()->width() : 0;
            const int scrollX = hbar->value();
            const int margin = qMax(40, viewportW / 10);
            if (viewportW > 0 && (playheadX < scrollX + margin || playheadX > scrollX + viewportW - margin)) {
                int target = playheadX - viewportW / 2;
                target = qBound(hbar->minimum(), target, hbar->maximum());
                hbar->setValue(target);
            }
        }
    }
}

void Timeline::updateInfoLabel()
{
    QString info = QString("Timeline - %1 clip(s) | Zoom %2 pps | %3")
        .arg(m_videoTrack->clipCount())
        .arg(m_zoomLevel, 0, 'f', 2)
        .arg(snapEnabled() ? "Snap ON" : "Snap OFF");
    if (m_videoTracks.size() > 1 || m_audioTracks.size() > 1)
        info += QString(" | V%1 A%2").arg(m_videoTracks.size()).arg(m_audioTracks.size());
    if (hasMarkedRange())
        info += QString(" | I/O: %1s-%2s").arg(m_markIn, 0, 'f', 1).arg(m_markOut, 0, 'f', 1);
    m_infoLabel->setText(info);
}
