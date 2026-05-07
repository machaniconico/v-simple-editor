#include "Timeline.h"
#include "ProxyManager.h"
#include "UndoManager.h"
#include "AudioMixer.h"
#include "OverlayDialogs.h"

namespace {
// Transition badge geometry. The badge width grows with duration so the
// user gets live visual feedback while dragging the resize handle (a 6 px
// band on the inward edge). The 18 px Y-band defines the hit-test area
// for both the press handler and the hover-cursor logic.
constexpr int kTransBadgeMinW    = 40;
constexpr int kTransBadgeMaxW    = 160;
constexpr int kTransBadgeHandleW = 6;
constexpr int kTransBadgeYTop    = 3;
constexpr int kTransBadgeH       = 18;
constexpr int kTransBadgeYBot    = kTransBadgeYTop + kTransBadgeH;

inline int transBadgeWidth(double durSec, double pps, int clipWidth) {
    const int desired = static_cast<int>(durSec * pps);
    return qBound(kTransBadgeMinW, desired,
                  qMin(kTransBadgeMaxW, clipWidth - 4));
}

// Volume-envelope (rubber-band) overlay state. Process-wide because the
// MainWindow menu toggle flips every audio row at once. Default OFF — the
// envelope is only drawn / hit-tested when the user opts in, so existing
// audio clip interaction (drag, trim, transition badge) is unchanged.
bool g_envelopeEditMode = false;
constexpr int kEnvelopePointRadiusPx = 5;
constexpr double kEnvelopeMaxGain = 2.0;     // matches ClipInfo::volume cap
constexpr double kEnvelopeHitRadiusPx = 5.0; // mouse hit radius for points

// Map a clip-local time + gain to widget pixel coords. The clipRect's top
// row is gain=2.0 (max), the bottom row is gain=0.0; gain=1.0 sits at the
// vertical centre. Time is laid out at the track pps.
inline double envelopeGainToY(double gain, int rowHeight) {
    const double clamped = qBound(0.0, gain, kEnvelopeMaxGain);
    return rowHeight * (1.0 - clamped / kEnvelopeMaxGain);
}
inline double envelopeYToGain(double y, int rowHeight) {
    if (rowHeight <= 0) return 1.0;
    const double t = qBound(0.0, 1.0 - y / static_cast<double>(rowHeight), 1.0);
    return t * kEnvelopeMaxGain;
}
} // namespace

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// RAII helper for one-shot FFmpeg audio decoding used by per-clip PCM peak
// analysis (normalizeAudioClipPeak). Mirrors AudioMixer::openEntry's pattern
// so we don't introduce a separate FFmpeg open path.
struct NormalizeDecoderCtx {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;

    bool open(const QString &path) {
        if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0)
            return false;
        const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (idx < 0) return false;
        AVCodecParameters *par = fmt->streams[idx]->codecpar;
        const AVCodec *c = avcodec_find_decoder(par->codec_id);
        if (!c) return false;
        codec = avcodec_alloc_context3(c);
        if (!codec) return false;
        if (avcodec_parameters_to_context(codec, par) < 0) return false;
        if (avcodec_open2(codec, c, nullptr) < 0) return false;
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        if (!pkt || !frame) return false;
        m_streamIdx = idx;
        return true;
    }

    ~NormalizeDecoderCtx() {
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
    }

    int streamIdx() const { return m_streamIdx; }
private:
    int m_streamIdx = -1;
};

// Definitions for the static envelope-mode getters declared in Timeline.h.
// Implemented out-of-line so the anonymous-namespace flag stays a single
// translation-unit-local symbol.
void TimelineTrack::setEnvelopeEditMode(bool on) { g_envelopeEditMode = on; }
bool TimelineTrack::envelopeEditMode() { return g_envelopeEditMode; }

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
#include <algorithm>
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
#include <QToolTip>
#include <QHash>
#include <QProgressDialog>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
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
    // Trailing pad past the last clip's end: large enough to comfortably
    // place the playhead a few seconds past the longest clip end at typical
    // zoom levels (~12 s at default 50 pps). Old value (+100 px) was just
    // ~2 s and made post-clip scrubbing feel cramped.
    constexpr int kTrailingPadPx = 600;
    setMinimumWidth(static_cast<int>(totalWidth) + kTrailingPadPx);
}
