#include "Timeline.h"
#include "VideoPlayer.h"
#include "SilenceCut.h"
#include "BeatDetect.h"
#include "ThreePointEdit.h"
#include "TrimOps.h"
#include "TrackMatteKey.h"
#include "ProxyManager.h"
#include "UndoTrace.h"
#include "UndoManager.h"
#include "AudioMixer.h"
#include "OverlayDialogs.h"
#include "WaveformGenerator.h"
#include "color/ClipColor.h"
#include "playback/HdrIngestProbe.h"
#include "playback/hdringest_flag.h"
#include "playback/PixFmtDepth.h"
#include "playback/SnsFit.h"
#include "util/RcPause.h"

#include <algorithm>
#include <QMessageBox>

// STAGE4B: PlaybackEntry::matteTypeOrdinal stores TrackMatteType as a plain int
// (PlaybackTypes.h must stay MaskSystem.h-free). This is the SSOT populate site
// and already sees TrackMatteType (via Timeline.h -> MaskSystem.h), so we pin
// the ordinal correspondence here. If TrackMatteType is ever reordered, these
// fire at compile time before any silent matte mis-application can occur.
static_assert(static_cast<int>(TrackMatteType::None) == 0,
              "PlaybackEntry::matteTypeOrdinal 0 must mean TrackMatteType::None");
static_assert(static_cast<int>(TrackMatteType::AlphaMatte) == 1,
              "PlaybackEntry::matteTypeOrdinal 1 must mean TrackMatteType::AlphaMatte");
static_assert(static_cast<int>(TrackMatteType::AlphaInvertedMatte) == 2,
              "PlaybackEntry::matteTypeOrdinal 2 must mean TrackMatteType::AlphaInvertedMatte");
static_assert(static_cast<int>(TrackMatteType::LumaMatte) == 3,
              "PlaybackEntry::matteTypeOrdinal 3 must mean TrackMatteType::LumaMatte");
static_assert(static_cast<int>(TrackMatteType::LumaInvertedMatte) == 4,
              "PlaybackEntry::matteTypeOrdinal 4 must mean TrackMatteType::LumaInvertedMatte");

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
#include <libavcodec/packet.h>
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
#include <QHelpEvent>
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

// --- MarkerLane ---
// 16px-tall strip painted above the time ruler. Reads markers directly from
// the owning Timeline so there's no per-marker QObject and no manual
// add/remove syncing — every paint reflects the current QVector<Marker>.

MarkerLane::MarkerLane(Timeline *timeline, QWidget *parent)
    : QWidget(parent), m_timeline(timeline)
{
    setFixedHeight(kLaneHeight);
    setStyleSheet("background-color: #232323;");
    setMouseTracking(true);
    // Enable hover tooltips via QHelpEvent without per-marker setToolTip.
    setAttribute(Qt::WA_Hover, true);
}

void MarkerLane::setPixelsPerSecond(double pps)
{
    if (qFuzzyCompare(pps, m_pixelsPerSecond))
        return;
    m_pixelsPerSecond = qBound(0.02, pps, 200.0);
    update();
}

void MarkerLane::setScrollOffset(int offsetX)
{
    if (offsetX == m_scrollOffset)
        return;
    m_scrollOffset = offsetX;
    update();
}

int MarkerLane::markerScreenX(qint64 timelineUs) const
{
    // contentX = timelineUs / 1e6 * pps. Subtract scroll offset so the lane
    // pans together with the tracks below.
    const double seconds = static_cast<double>(timelineUs) / 1'000'000.0;
    const int contentX = static_cast<int>(seconds * m_pixelsPerSecond);
    return contentX - m_scrollOffset;
}

int MarkerLane::hitTestMarker(const QPoint &pos) const
{
    if (!m_timeline) return -1;
    const int half = kTriangleSize / 2 + kHitPad;
    // Reverse iterate so a topmost (rightmost-painted) marker wins on overlap.
    const auto &markers = m_timeline->markers();
    for (int i = markers.size() - 1; i >= 0; --i) {
        const int x = markerScreenX(markers[i].timelineUs);
        if (qAbs(pos.x() - x) <= half && pos.y() <= kLaneHeight)
            return markers[i].id;
    }
    return -1;
}

void MarkerLane::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x23, 0x23, 0x23));
    // Bottom hairline so the lane visually sits as a tray above the ruler.
    painter.setPen(QPen(QColor(0x3a, 0x3a, 0x3a), 1));
    painter.drawLine(0, height() - 1, width(), height() - 1);

    if (!m_timeline) return;
    const auto &markers = m_timeline->markers();
    if (markers.isEmpty()) return;

    painter.setRenderHint(QPainter::Antialiasing, true);
    const int half = kTriangleSize / 2;
    const int top = 2;                  // small top gap so triangle base
    const int tipY = top + kTriangleSize; // hangs cleanly off the ruler line.

    // MK-1: span (duration) markers first, so the triangle/label below
    // paints on top of the bar. A point marker (durationUs == 0) draws no
    // bar — it stays byte-identical to the legacy single-triangle look.
    for (const auto &m : markers) {
        if (m.durationUs <= 0) continue;
        const int x0 = markerScreenX(m.timelineUs);
        const int x1 = markerScreenX(m.timelineUs + m.durationUs);
        // Cull bars fully off either edge.
        if (x1 <= 0 || x0 >= width()) continue;
        const int barLeft  = qMax(0, x0);
        const int barRight = qMin(width(), x1);
        const int barW = qMax(1, barRight - barLeft);
        const QColor base = m.color.isValid() ? m.color : QColor("#ff5050");
        QColor barFill = base;
        barFill.setAlpha(80);  // half-transparent so the ruler shows through
        // Fill the full lane height minus the bottom hairline.
        painter.setPen(Qt::NoPen);
        painter.setBrush(barFill);
        painter.drawRect(QRect(barLeft, 0, barW, height() - 1));
    }

    for (const auto &m : markers) {
        const int x = markerScreenX(m.timelineUs);
        // Cull off-screen triangles cheaply — important for the
        // 100-marker spec acceptance.
        if (x + half < 0 || x - half > width()) continue;

        QPolygonF tri;
        tri << QPointF(x - half, top)
            << QPointF(x + half, top)
            << QPointF(x,        tipY);
        const QColor fill = m.color.isValid() ? m.color : QColor("#ff5050");
        painter.setBrush(fill);
        painter.setPen(QPen(fill.darker(140), 1));
        painter.drawPolygon(tri);
    }
}

void MarkerLane::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int id = hitTestMarker(event->pos());
    if (id >= 0) {
        emit markerClicked(id);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

bool MarkerLane::event(QEvent *event)
{
    // Hover tooltip — show label + H:MM:SS.mmm time. Computed lazily so
    // there's no per-marker setToolTip overhead even with hundreds of
    // markers.
    if (event->type() == QEvent::ToolTip && m_timeline) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        const int id = hitTestMarker(helpEvent->pos());
        if (id >= 0) {
            const Marker mk = m_timeline->markerById(id);
            const qint64 us = mk.timelineUs;
            const qint64 totalMs = us / 1000;
            const int hours = static_cast<int>(totalMs / 3'600'000);
            const int mins  = static_cast<int>((totalMs / 60'000) % 60);
            const int secs  = static_cast<int>((totalMs / 1000) % 60);
            const int millis = static_cast<int>(totalMs % 1000);
            const QString timeStr = QString("%1:%2:%3.%4")
                .arg(hours)
                .arg(mins,   2, 10, QChar('0'))
                .arg(secs,   2, 10, QChar('0'))
                .arg(millis, 3, 10, QChar('0'));
            const QString text = mk.label.isEmpty()
                ? timeStr
                : (mk.label + QChar('\n') + timeStr);
            QToolTip::showText(helpEvent->globalPos(), text, this);
            return true;
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QWidget::event(event);
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
    constexpr double kSplitEps = 1e-6;
    if (splitPoint <= effectiveStart + kSplitEps || splitPoint >= effectiveEnd - kSplitEps) return;
    ClipInfo secondHalf = original;
    secondHalf.inPoint = splitPoint;
    secondHalf.outPoint = effectiveEnd;
    secondHalf.leadInSec = 0.0;
    original.outPoint = splitPoint;
    m_clips.insert(index + 1, secondHalf);
    updateMinimumWidth(); update(); emit modified();
}

bool TimelineTrack::applyTrim(int clipIndex, trimops::TrimType type,
                              double deltaSec, QString *errorOut)
{
    // 純粋エンジン trimops:: が境界判定と in-place 変更を担う。成功時のみ
    // split/insert と同じ後処理 (幅再計算 + 再描画 + modified 発火) を行う。
    // 失敗時は m_clips が一切変更されないので何もせず false を返す。
    if (!trimops::applyTrim(m_clips, clipIndex, type, deltaSec, errorOut))
        return false;
    updateMinimumWidth(); update(); emit modified();
    return true;
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

void TimelineTrack::insertClip3Point(double timelineStartSec, const ClipInfo &clip)
{
    // ripple insert: T を跨ぐ位置に割り込み、後続クリップを右へ押し出す。
    // 配置 index と先行ギャップ leadIn は純粋エンジンに委ねる。
    double leadIn = 0.0;
    const int index = threepoint::insertIndexForTime(m_clips, timelineStartSec, &leadIn);
    insertClipPreservingDownstream(index, clip, leadIn);
    // insertClipPreservingDownstream が update() / emit modified() を済ませている。
}

void TimelineTrack::overwriteClip3Point(double timelineStartSec, const ClipInfo &clip)
{
    const threepoint::OverwritePlan plan =
        threepoint::planOverwrite(m_clips, timelineStartSec, clip.effectiveDuration());
    if (!plan.valid)
        return;
    // SM-1 notes の実行順序を厳守する:
    //   (1) tail を先に分割 (head 分割の +1 ズレが tail index に波及しないように)
    if (plan.splitTailIndex >= 0)
        splitClipAt(plan.splitTailIndex, plan.splitTailLocalSec);
    //   (2) head を分割
    if (plan.splitHeadIndex >= 0)
        splitClipAt(plan.splitHeadIndex, plan.splitHeadLocalSec);
    //   (3) [T,T+L) に完全に収まる既存クリップ群を、 downstream の絶対位置を
    //       保ったまま末尾側から削除 (前から消すと後続インデックスがズレる)。
    for (int i = plan.removeCount - 1; i >= 0; --i)
        removeClipPreservingDownstream(plan.removeFromIndex + i);
    //   (4) 新クリップを T 開始に配置 (先行ギャップ insertLeadInSec)。
    insertClipPreservingDownstream(plan.insertIndex, clip, plan.insertLeadInSec);
    // 各プリミティブが update() / emit modified() を内包しているので追加処理は不要。
}

bool TimelineTrack::rippleDeleteTimeRange(double startSec, double endSec)
{
    // TB-3: タイムラインの [startSec, endSec) を削除し、後続クリップを左へ詰める。
    // 既存の split/remove プリミティブだけで構成し、時間↔leadInSec モデルは
    // effectiveDuration() / leadInSec の積み上げ (insertClip3Point などと同じ規約)
    // に従う。境界は安全に no-op。
    constexpr double kEps = 1e-6;
    if (m_clips.isEmpty())
        return false;
    if (endSec - startSec <= kEps)   // 空 / 反転 / 微小範囲
        return false;
    if (startSec < 0.0) startSec = 0.0;
    if (endSec - startSec <= kEps)
        return false;

    bool changed = false;

    // (1) 範囲境界で跨ぐクリップを分割する。overwriteClip3Point と同じく、
    //     末尾境界 (endSec) を先に分割してから先頭境界 (startSec) を分割すると、
    //     先頭分割で生じる +1 インデックスズレが末尾側に波及しない。
    //     splitClipAt は clip-local 秒を取るので、各クリップのタイムライン開始時刻を
    //     leadInSec 積み上げで求め、ローカル秒へ換算する。
    auto splitAtTimelineSec = [&](double t) {
        double cursor = 0.0;
        for (int i = 0; i < m_clips.size(); ++i) {
            const double clipStart = cursor + m_clips[i].leadInSec;
            const double clipEnd = clipStart + m_clips[i].effectiveDuration();
            if (t > clipStart + kEps && t < clipEnd - kEps) {
                const int beforeCount = m_clips.size();
                splitClipAt(i, t - clipStart);   // clip-local 秒
                if (m_clips.size() != beforeCount)
                    changed = true;
                return;
            }
            cursor = clipEnd;
        }
    };
    splitAtTimelineSec(endSec);
    splitAtTimelineSec(startSec);

    // (2) [startSec, endSec) に完全に収まるクリップ群を末尾側から削除する。
    //     removeClipPreservingDownstream は削除クリップの footprint を次クリップの
    //     leadInSec に押し込むため、この時点では後続の絶対位置は保たれる
    //     (= 削除区間が空ギャップとして残る)。後ろから消すのは前から消すと
    //     後続インデックスがズレるため。
    QVector<int> toRemove;
    {
        double cursor = 0.0;
        for (int i = 0; i < m_clips.size(); ++i) {
            const double clipStart = cursor + m_clips[i].leadInSec;
            const double clipEnd = clipStart + m_clips[i].effectiveDuration();
            if (clipStart >= startSec - kEps && clipEnd <= endSec + kEps)
                toRemove.append(i);
            cursor = clipEnd;
        }
    }
    if (!toRemove.isEmpty())
        changed = true;
    for (int j = toRemove.size() - 1; j >= 0; --j)
        removeClipPreservingDownstream(toRemove[j]);

    // (3) リップル: 削除区間ぶん後続クリップを左へ詰める。削除区間の直後に
    //     位置するクリップ (タイムライン開始が startSec 以降の最初のクリップ) の
    //     leadInSec から削除長を引く。removeClipPreservingDownstream が空きを
    //     leadInSec に積んでいるので、ここで引くと後続全体が左シフトする。
    const double deletedLen = endSec - startSec;
    double cursor = 0.0;
    for (int i = 0; i < m_clips.size(); ++i) {
        const double clipStart = cursor + m_clips[i].leadInSec;
        if (clipStart >= startSec - kEps) {
            double &lead = m_clips[i].leadInSec;
            const double newLead = qMax(0.0, lead - deletedLen);
            if (qAbs(newLead - lead) > kEps) {
                lead = newLead;
                changed = true;
            }
            break;
        }
        cursor = clipStart + m_clips[i].effectiveDuration();
    }

    if (!changed)
        return false;
    updateMinimumWidth(); update(); emit modified();
    return true;
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
        // Clip body colour reflects edit STATE, not list position: a clip
        // that's just moved must keep the same colour as before the move.
        // Plain clips are blue; clips with a color correction or any video
        // effect applied switch to green so the user can see at a glance
        // which clips are processed. The previous index-parity colouring
        // (i % 2) made every move flip the colour, which read as "the clip
        // mutated" even though only its position changed.
        const bool hasEffects = !m_clips[i].colorCorrection.isDefault()
                             || !m_clips[i].effects.isEmpty();
        QColor color = hasEffects ? QColor(0x44, 0xAA, 0x88)
                                  : QColor(0x44, 0x88, 0xCC);
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

        // Transition badge. Pill at the affected corner — top-left for
        // leadIn (FadeIn), top-right for trailOut (FadeOut / CrossDissolve
        // / Wipe / Slide). Coloured per type so the user can tell them
        // apart at a glance, with abbrev + duration text. Geometry constants
        // and the duration-tracking width come from the file-scope helpers
        // so paint, hit-test, and hover all agree.
        auto transitionAbbrev = [](TransitionType type) -> QString {
            switch (type) {
                case TransitionType::None:               return "";
                case TransitionType::FadeIn:             return "Fi";
                case TransitionType::FadeOut:            return "Fo";
                case TransitionType::CrossDissolve:      return "X";
                case TransitionType::WipeLeft:           return "WL";
                case TransitionType::WipeRight:          return "WR";
                case TransitionType::WipeUp:             return "WU";
                case TransitionType::WipeDown:           return "WD";
                case TransitionType::SlideLeft:          return "SL";
                case TransitionType::SlideRight:         return "SR";
                case TransitionType::SlideUp:            return "SU";
                case TransitionType::SlideDown:          return "SD";
                case TransitionType::DipToBlack:         return "DB";
                case TransitionType::DipToWhite:         return "DW";
                case TransitionType::IrisRound:          return "IR";
                case TransitionType::IrisBox:            return "IB";
                case TransitionType::ClockWipe:          return "CW";
                case TransitionType::BarnDoorHorizontal: return "BH";
                case TransitionType::BarnDoorVertical:   return "BV";
                case TransitionType::PushLeft:           return "PL";
                case TransitionType::PushRight:          return "PR";
                case TransitionType::PushUp:             return "PU";
                case TransitionType::PushDown:           return "PD";
                case TransitionType::CrossZoom:          return "CZ";
                case TransitionType::FilmDissolve:       return "Fd";
                case TransitionType::SpinCW:             return "S+";
                case TransitionType::SpinCCW:            return "S-";
                case TransitionType::DitherDissolve:     return "Dt";
                case TransitionType::IrisRoundClose:     return "ir";
                case TransitionType::IrisBoxClose:       return "ib";
                case TransitionType::BarnDoorHClose:     return "bH";
                case TransitionType::BarnDoorVClose:     return "bV";
                case TransitionType::ClockWipeCCW:       return "cw";
                case TransitionType::WhipPanLeft:        return "<<";
                case TransitionType::WhipPanRight:       return ">>";
                case TransitionType::Glitch:             return "G!";
                case TransitionType::LightLeak:          return "LL";
                case TransitionType::FlipHorizontal:     return "FH";
                case TransitionType::FlipVertical:       return "FV";
                case TransitionType::LensFlare:          return "Lf";
                case TransitionType::FilmBurn:           return "FB";
                case TransitionType::Pixelate:           return "Px";
                case TransitionType::BlurDissolve:       return "Bl";
                case TransitionType::CameraShake:        return "Sk";
                case TransitionType::ColorChannelShift:  return "Ch";
            }
            return "";
        };
        // Single colour for every transition type. The badge abbreviation
        // (Fi / X / WL / ...) already encodes the type, so the colour's job
        // is to mark the badge as a *transition* and keep it visually
        // distinct from the effect indicators (purple bar / green clip
        // body). Amber sits in the warm half of the wheel where neither of
        // those reside, so transitions read as their own category.
        auto transitionColor = [](TransitionType type) -> QColor {
            Q_UNUSED(type);
            return QColor(255, 200, 80);
        };
        auto paintTransitionBadge = [&](const Transition &t, int badgeX, bool handleOnRight) {
            if (t.type == TransitionType::None) return;
            const int badgeW = transBadgeWidth(t.duration, m_pixelsPerSecond, clipWidth);
            if (badgeW < 12) return;
            const QColor base = transitionColor(t.type);
            const QRect badge(badgeX, kTransBadgeYTop, badgeW, kTransBadgeH);

            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setBrush(base);
            painter.setPen(QPen(base.darker(160), 1));
            painter.drawRoundedRect(badge, 4, 4);
            painter.setRenderHint(QPainter::Antialiasing, false);

            // Drag handle indicator. Two short vertical lines on the inward
            // edge of the badge; the actual hit-test zone is 6 px wide.
            const int handleX = handleOnRight ? badge.right() - 4 : badge.left() + 1;
            painter.setPen(QPen(base.darker(220), 1));
            painter.drawLine(handleX,     badge.top() + 3, handleX,     badge.bottom() - 3);
            painter.drawLine(handleX + 2, badge.top() + 3, handleX + 2, badge.bottom() - 3);

            // Two-tier label: full ("X 1.0s") if it fits, otherwise just
            // the duration ("1.0s") so the user can always read seconds.
            // The type is still encoded in the badge color when the abbrev
            // is dropped, and hover gives a tooltip with the full name.
            painter.setPen(QPen(QColor(20, 10, 0), 1));
            painter.setFont(QFont("Arial", 8, QFont::Bold));
            const QString full = QString("%1 %2s")
                .arg(transitionAbbrev(t.type))
                .arg(t.duration, 0, 'f', 1);
            const QString compact = QString("%1s").arg(t.duration, 0, 'f', 1);
            const QString tiny = QString::number(qRound(t.duration)) + "s";
            const auto fm = painter.fontMetrics();
            const int avail = badgeW - 8;
            QString shown = (fm.horizontalAdvance(full) <= avail) ? full
                          : (fm.horizontalAdvance(compact) <= avail) ? compact
                          : (fm.horizontalAdvance(tiny) <= avail) ? tiny
                          : fm.elidedText(compact, Qt::ElideRight, avail);
            painter.drawText(badge.adjusted(4, 0, -4, 0),
                             Qt::AlignCenter, shown);
        };
        if (m_clips[i].leadIn.type != TransitionType::None) {
            // leadIn handle sits on the badge's right edge — drag right to
            // grow into the clip.
            paintTransitionBadge(m_clips[i].leadIn, x + 2, /*handleOnRight*/ true);
        }
        if (m_clips[i].trailOut.type != TransitionType::None) {
            const int badgeW = transBadgeWidth(m_clips[i].trailOut.duration,
                                               m_pixelsPerSecond, clipWidth);
            // trailOut handle sits on the badge's left edge — drag left to
            // grow toward the cut.
            paintTransitionBadge(m_clips[i].trailOut,
                                 x + clipWidth - badgeW - 2,
                                 /*handleOnRight*/ false);
        }

        // Volume-envelope (rubber band) overlay. Drawn on top of the
        // waveform so the line is always legible. Active only on audio
        // rows when the user has flipped the global edit-mode toggle.
        if (m_isAudioTrack && g_envelopeEditMode) {
            const auto &env = m_clips[i].volumeEnvelope;
            const double effDur = m_clips[i].effectiveDuration();
            if (effDur > 0.0) {
                if (env.isEmpty()) {
                    // No automation yet — show a dashed reference line at
                    // the static volume so the user can see where a click
                    // would land.
                    const double y = envelopeGainToY(m_clips[i].volume, m_rowHeight);
                    painter.setPen(QPen(QColor(255, 215, 60, 230), 2, Qt::DashLine));
                    painter.drawLine(QPointF(x, y), QPointF(x + clipWidth, y));
                } else {
                    // Polyline through the envelope points, with implicit
                    // endpoints clamped to the first / last gain so the line
                    // spans the whole clip even when only interior points
                    // are authored.
                    QVector<QPointF> pts;
                    pts.reserve(env.size() + 2);
                    pts.append(QPointF(x,
                                       envelopeGainToY(env.first().gain, m_rowHeight)));
                    for (const auto &p : env) {
                        const double frac = qBound(0.0, p.time / effDur, 1.0);
                        pts.append(QPointF(x + frac * clipWidth,
                                           envelopeGainToY(p.gain, m_rowHeight)));
                    }
                    pts.append(QPointF(x + clipWidth,
                                       envelopeGainToY(env.last().gain, m_rowHeight)));

                    painter.setPen(QPen(QColor(255, 215, 60, 230), 2));
                    for (int k = 0; k + 1 < pts.size(); ++k)
                        painter.drawLine(pts[k], pts[k + 1]);

                    // Solid white circles for each authored point. The two
                    // synthetic endpoints (indices 0 and last) are not drawn
                    // because they aren't editable.
                    painter.setBrush(QColor(255, 255, 255));
                    painter.setPen(QPen(QColor(40, 30, 0), 1));
                    for (int k = 1; k + 1 < pts.size(); ++k) {
                        painter.drawEllipse(pts[k],
                                            kEnvelopePointRadiusPx,
                                            kEnvelopePointRadiusPx);
                    }
                }
            }
        }
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

    // Snap line visual feedback — yellow vertical line fading over 200 ms.
    if (m_timeline) {
        const int sx = m_timeline->snapLineX();
        if (sx >= 0) {
            const int alpha = m_timeline->snapLineAlpha();
            painter.setPen(QPen(QColor(255, 220, 0, alpha), 1));
            painter.drawLine(sx, 0, sx, m_rowHeight);
        }
    }
}

void TimelineTrack::mousePressEvent(QMouseEvent *event)
{
    if (m_locked) { event->accept(); return; }
    if (event->button() == Qt::LeftButton
        && event->pos().y() >= m_rowHeight - RESIZE_HANDLE_HEIGHT) {
        m_resizingHeight = true;
        m_resizeStartY = event->globalPosition().toPoint().y();
        m_resizeStartHeight = m_rowHeight;
        setCursor(Qt::SizeVerCursor); grabMouse();
        event->accept(); return;
    }
    const int clipIndex = clipAtX(event->pos().x());
    if (tryHitEnvelopeKeyframe(event, clipIndex)) return;
    const bool additive = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (event->button() == Qt::RightButton && clipIndex >= 0) {
        emit clipContextMenuRequested(clipIndex, event->globalPosition().toPoint());
        event->accept(); return;
    }
    QRectF clipRect;
    if (clipIndex >= 0) {
        const int cx = clipStartX(clipIndex);
        const int cw = qMax(20, static_cast<int>(
            m_clips[clipIndex].effectiveDuration() * m_pixelsPerSecond));
        clipRect = QRectF(cx, 0, cw, m_rowHeight);
        if (event->button() == Qt::LeftButton && !additive
            && tryHitTransitionDragHandle(event, clipIndex, clipRect))
            return;
    }
    if (event->button() == Qt::LeftButton && !additive)
        emit seekRequested(qMax(0.0, xToSeconds(event->pos().x())));
    if (event->button() == Qt::LeftButton && clipIndex >= 0) {
        if (additive) {
            toggleClipSelection(clipIndex); emit clipClicked(clipIndex); return;
        }
        const bool partOfMulti = m_selectedClips.size() > 1
                                  && m_selectedClips.contains(clipIndex);
        if (!partOfMulti) setSelectedClip(clipIndex);
        emit clipClicked(clipIndex);
        if (tryHitTransitionBadge(event, clipIndex)) return;
        if (tryHitTrimEdge(event, clipIndex, clipRect)) return;
        handleBodyClick(event, clipIndex);
    } else if (event->button() == Qt::LeftButton && !additive) {
        setSelectedClip(-1); emit emptyAreaClicked();
    }
}

bool TimelineTrack::tryHitEnvelopeKeyframe(QMouseEvent *ev, int clipIndex)
{
    if (!g_envelopeEditMode || !m_isAudioTrack)
        return false;
    if (ev->button() != Qt::LeftButton && ev->button() != Qt::RightButton)
        return false;
    if (clipIndex < 0 || clipIndex >= m_clips.size())
        return false;
    ClipInfo &clip = m_clips[clipIndex];
    const double effDur = clip.effectiveDuration();
    if (effDur <= 0.0) return false;
    const QPoint pos = ev->pos();
    const int clipStartXpx = clipStartX(clipIndex);
    const int clipWidth = qMax(20, static_cast<int>(
        qMin<double>(2000000.0, effDur * m_pixelsPerSecond)));
    const double localPx = pos.x() - clipStartXpx;
    if (localPx < 0.0 || localPx > clipWidth) return false;
    int hitPt = -1;
    for (int k = 0; k < clip.volumeEnvelope.size(); ++k) {
        const auto &pt = clip.volumeEnvelope[k];
        const double frac = qBound(0.0, pt.time / effDur, 1.0);
        const double px = frac * clipWidth;
        const double py = envelopeGainToY(pt.gain, m_rowHeight);
        const double dx = localPx - px;
        const double dy = pos.y() - py;
        if (dx * dx + dy * dy <= kEnvelopeHitRadiusPx * kEnvelopeHitRadiusPx) {
            hitPt = k;
            break;
        }
    }
    const bool altHeld = ev->modifiers() & Qt::AltModifier;
    if (ev->button() == Qt::RightButton) {
        if (hitPt >= 0) {
            clip.volumeEnvelope.remove(hitPt);
            update();
            emit modified();
            ev->accept();
            return true;
        }
        return false;
    }
    if (hitPt >= 0) {
        m_envelopeDragClipIdx = clipIndex;
        m_envelopeDragPointIdx = hitPt;
        ev->accept();
        return true;
    }
    if (altHeld) {
        const bool inBadgeBand = pos.y() >= kTransBadgeYTop
                                  && pos.y() <= kTransBadgeYBot;
        const bool clipHasTrans = clip.leadIn.type != TransitionType::None
                                   || clip.trailOut.type != TransitionType::None;
        if (!(inBadgeBand && clipHasTrans)) {
            AudioGainPoint np;
            np.time = qBound(0.0,
                (localPx / clipWidth) * effDur, effDur);
            np.gain = envelopeYToGain(pos.y(), m_rowHeight);
            int insertAt = 0;
            while (insertAt < clip.volumeEnvelope.size()
                   && clip.volumeEnvelope[insertAt].time <= np.time)
                ++insertAt;
            clip.volumeEnvelope.insert(insertAt, np);
            m_envelopeDragClipIdx = clipIndex;
            m_envelopeDragPointIdx = insertAt;
            update();
            emit modified();
            ev->accept();
            return true;
        }
    }
    return false;
}

bool TimelineTrack::tryHitTransitionDragHandle(QMouseEvent *ev, int clipIndex, const QRectF &clipRect)
{
    if (ev->button() != Qt::LeftButton) return false;
    const int cy = ev->pos().y();
    if (cy < kTransBadgeYTop || cy > kTransBadgeYBot) return false;
    if (clipIndex < 0 || clipIndex >= m_clips.size()) return false;
    const int cx = static_cast<int>(clipRect.x());
    const int cw = static_cast<int>(clipRect.width());
    const auto &clip = m_clips[clipIndex];
    if (clip.leadIn.type != TransitionType::None && clip.leadIn.duration > 0.0) {
        const int badgeX = cx + 2;
        const int badgeW = transBadgeWidth(clip.leadIn.duration, m_pixelsPerSecond, cw);
        const int handleX = badgeX + badgeW - kTransBadgeHandleW;
        if (ev->pos().x() >= handleX && ev->pos().x() <= handleX + kTransBadgeHandleW) {
            setSelectedClip(clipIndex);
            emit clipClicked(clipIndex);
            m_dragMode = DragMode::TransitionLeadInResize;
            m_dragClipIndex = clipIndex;
            m_dragStartX = ev->pos().x();
            m_dragOriginalTransitionDuration = clip.leadIn.duration;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return true;
        }
    }
    if (clip.trailOut.type != TransitionType::None && clip.trailOut.duration > 0.0) {
        const int badgeW = transBadgeWidth(clip.trailOut.duration, m_pixelsPerSecond, cw);
        const int badgeX = cx + cw - badgeW - 2;
        if (ev->pos().x() >= badgeX && ev->pos().x() <= badgeX + kTransBadgeHandleW) {
            setSelectedClip(clipIndex);
            emit clipClicked(clipIndex);
            m_dragMode = DragMode::TransitionTrailOutResize;
            m_dragClipIndex = clipIndex;
            m_dragStartX = ev->pos().x();
            m_dragOriginalTransitionDuration = clip.trailOut.duration;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return true;
        }
    }
    return false;
}

bool TimelineTrack::tryHitTransitionBadge(QMouseEvent *ev, int clipIndex)
{
    if (ev->button() != Qt::LeftButton) return false;
    if (clipIndex < 0 || clipIndex >= m_clips.size()) return false;
    const int cy = ev->pos().y();
    if (cy < kTransBadgeYTop || cy > kTransBadgeYBot) return false;
    const auto &clip = m_clips[clipIndex];
    const int cx = clipStartX(clipIndex);
    const int cw = qMax(20, static_cast<int>(
        clip.effectiveDuration() * m_pixelsPerSecond));
    const int px = ev->pos().x();
    if (clip.leadIn.type != TransitionType::None && clip.leadIn.duration > 0.0) {
        const int badgeW = transBadgeWidth(clip.leadIn.duration, m_pixelsPerSecond, cw);
        const int badgeX = cx + 2;
        // Body region of the leadIn badge (between badge start and the
        // resize handle). Swallow the click so it doesn't fall through to
        // trim-edge or body-click handling — clicking a transition badge
        // should never select / drag the clip body.
        if (px >= badgeX && px < badgeX + badgeW - kTransBadgeHandleW) {
            ev->accept();
            return true;
        }
    }
    if (clip.trailOut.type != TransitionType::None && clip.trailOut.duration > 0.0) {
        const int badgeW = transBadgeWidth(clip.trailOut.duration, m_pixelsPerSecond, cw);
        const int badgeX = cx + cw - badgeW - 2;
        if (px >= badgeX + kTransBadgeHandleW && px < badgeX + badgeW) {
            ev->accept();
            return true;
        }
    }
    return false;
}

bool TimelineTrack::tryHitTrimEdge(QMouseEvent *ev, int clipIndex, const QRectF &clipRect)
{
    if (ev->button() != Qt::LeftButton) return false;
    if (clipIndex < 0 || clipIndex >= m_clips.size()) return false;
    const int cx = static_cast<int>(clipRect.x());
    const int clipWidth = static_cast<int>(clipRect.width());
    const int localX = ev->pos().x() - cx;
    if (localX <= TRIM_HANDLE_WIDTH) {
        m_dragMode = DragMode::TrimLeft;
        m_dragClipIndex = clipIndex;
        m_dragStartX = ev->pos().x();
        m_dragOriginalValue = m_clips[clipIndex].inPoint;
        m_dragOriginalLeadIn = m_clips[clipIndex].leadInSec;
        return true;
    }
    if (localX >= clipWidth - TRIM_HANDLE_WIDTH) {
        m_dragMode = DragMode::TrimRight;
        m_dragClipIndex = clipIndex;
        m_dragStartX = ev->pos().x();
        m_dragOriginalValue = m_clips[clipIndex].outPoint > 0
            ? m_clips[clipIndex].outPoint : m_clips[clipIndex].duration;
        return true;
    }
    return false;
}

void TimelineTrack::handleBodyClick(QMouseEvent *ev, int clipIndex)
{
    if (ev->button() != Qt::LeftButton) return;
    if (clipIndex < 0 || clipIndex >= m_clips.size()) return;
    m_dragMode = DragMode::MoveClip;
    m_dragClipIndex = clipIndex;
    m_dragStartX = ev->pos().x();
    m_dragOriginalLeadIn = m_clips[clipIndex].leadInSec;
    m_dragOriginalClipStartX = clipStartX(clipIndex);
    m_dragOriginalLeadInNext = (clipIndex + 1 < m_clips.size())
        ? m_clips[clipIndex + 1].leadInSec : -1.0;
    m_dropTargetIndex = -1;
    // Pre-collect snap targets at drag start for O(log n) per-tick lookups.
    if (m_snapEnabled && m_timeline) {
        const qint64 playheadUs = static_cast<qint64>(
            m_timeline->playheadPosition() * 1'000'000.0);
        m_timeline->snapEngine().collectFromTimeline(
            *m_timeline, playheadUs, m_timeline->markerManager());
    }
    emit linkedDragStarted(clipIndex);
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
    if (m_envelopeDragClipIdx >= 0 && m_envelopeDragPointIdx >= 0
        && m_envelopeDragClipIdx < m_clips.size()) {
        ClipInfo &clip = m_clips[m_envelopeDragClipIdx];
        if (m_envelopeDragPointIdx < clip.volumeEnvelope.size()) {
            const double effDur = clip.effectiveDuration();
            if (effDur > 0.0) {
                const int clipStartXpx = clipStartX(m_envelopeDragClipIdx);
                const int clipWidth = qMax(20, static_cast<int>(
                    qMin<double>(2000000.0, effDur * m_pixelsPerSecond)));
                const double localPx = qBound(0.0,
                    static_cast<double>(event->pos().x() - clipStartXpx),
                    static_cast<double>(clipWidth));
                double newTime = qBound(0.0, (localPx / clipWidth) * effDur, effDur);
                // Clamp to neighbours: cannot cross prev/next keyframe.
                // Find the closest points on either side (excluding the one
                // being dragged) and enforce a 1 ms minimum gap.
                double prevTime = 0.0;
                double nextTime = effDur;
                for (int k = 0; k < clip.volumeEnvelope.size(); ++k) {
                    if (k == m_envelopeDragPointIdx) continue;
                    const double t = clip.volumeEnvelope[k].time;
                    if (t <= newTime && t > prevTime) prevTime = t;
                    if (t >= newTime && t < nextTime) nextTime = t;
                }
                if (prevTime > 0.0) prevTime += 0.001; // 1 ms after prev
                if (nextTime < effDur) nextTime -= 0.001; // 1 ms before next
                clip.volumeEnvelope[m_envelopeDragPointIdx].time =
                    qBound(prevTime, newTime, nextTime);
                clip.volumeEnvelope[m_envelopeDragPointIdx].gain =
                    envelopeYToGain(event->pos().y(), m_rowHeight);
                update();
            }
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
        int adjustedDx = dx;

        // Snap: adjust delta so the clip's left edge magnetises to the
        // nearest target (other clip edges, playhead, t=0, markers).
        if (m_snapEnabled && m_timeline) {
            const int candidateX = m_dragOriginalClipStartX + dx;
            const double candidateSec = static_cast<double>(candidateX) / m_pixelsPerSecond;
            const qint64 candidateUs = static_cast<qint64>(candidateSec * 1e6);
            const qint64 toleranceUs = static_cast<qint64>(
                SNAP_THRESHOLD / m_pixelsPerSecond * 1e6);
            const auto match = m_timeline->snapEngine().findMatch(candidateUs, toleranceUs);
            if (match.has_value()) {
                const double snappedSec = static_cast<double>(match->timeUs) / 1e6;
                const int snappedX = static_cast<int>(snappedSec * m_pixelsPerSecond);
                adjustedDx = snappedX - m_dragOriginalClipStartX;
                m_timeline->triggerSnapLine(snappedX);
            }
        }

        const double rawDeltaSec = static_cast<double>(adjustedDx) / m_pixelsPerSecond;
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
    if (m_dragMode == DragMode::TransitionLeadInResize
        || m_dragMode == DragMode::TransitionTrailOutResize) {
        if (m_dragClipIndex < 0 || m_dragClipIndex >= m_clips.size()) {
            m_dragMode = DragMode::None;
            m_dragClipIndex = -1;
            setCursor(Qt::ArrowCursor);
            return;
        }
        ClipInfo &clip = m_clips[m_dragClipIndex];
        const double maxDur = qMax(0.1, clip.effectiveDuration());
        const int dx = event->pos().x() - m_dragStartX;
        const double deltaSec = static_cast<double>(dx) / m_pixelsPerSecond;
        if (m_dragMode == DragMode::TransitionLeadInResize) {
            // Drag right grows leadIn, drag left shrinks it.
            clip.leadIn.duration =
                qBound(0.1, m_dragOriginalTransitionDuration + deltaSec, maxDur);
        } else {
            // trailOut handle is on the badge's left edge — drag left grows
            // it (deltaSec negative), drag right shrinks it.
            clip.trailOut.duration =
                qBound(0.1, m_dragOriginalTransitionDuration - deltaSec, maxDur);
        }
        setCursor(Qt::SizeHorCursor);
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
    // Transition badge hover takes priority — only fires inside the badge
    // Y-band so ordinary clip-body hover still routes to OpenHand / SizeHor.
    // Two zones: handle (6 px inward edge) → SizeHorCursor; badge body →
    // tooltip with full transition name + duration so the user can read
    // them even when the badge is too narrow to display the label.
    if (hover >= 0) {
        const int cy = event->pos().y();
        if (cy >= kTransBadgeYTop && cy <= kTransBadgeYBot) {
            const int cx = clipStartX(hover);
            const int cw = qMax(20, static_cast<int>(
                m_clips[hover].effectiveDuration() * m_pixelsPerSecond));
            const auto &c = m_clips[hover];
            const int px = event->pos().x();
            auto tipFor = [](const Transition &t) {
                return QString("%1 — %2 s")
                    .arg(Transition::typeName(t.type))
                    .arg(t.duration, 0, 'f', 2);
            };
            if (c.leadIn.type != TransitionType::None && c.leadIn.duration > 0.0) {
                const int badgeX = cx + 2;
                const int badgeW = transBadgeWidth(c.leadIn.duration,
                                                   m_pixelsPerSecond, cw);
                const int handleX = badgeX + badgeW - kTransBadgeHandleW;
                if (px >= handleX && px <= handleX + kTransBadgeHandleW) {
                    setCursor(Qt::SizeHorCursor);
                    QToolTip::showText(event->globalPosition().toPoint(),
                                       tipFor(c.leadIn), this);
                    return;
                }
                if (px >= badgeX && px < badgeX + badgeW) {
                    QToolTip::showText(event->globalPosition().toPoint(),
                                       tipFor(c.leadIn), this);
                }
            }
            if (c.trailOut.type != TransitionType::None && c.trailOut.duration > 0.0) {
                const int badgeW = transBadgeWidth(c.trailOut.duration,
                                                   m_pixelsPerSecond, cw);
                const int badgeX = cx + cw - badgeW - 2;
                if (px >= badgeX && px <= badgeX + kTransBadgeHandleW) {
                    setCursor(Qt::SizeHorCursor);
                    QToolTip::showText(event->globalPosition().toPoint(),
                                       tipFor(c.trailOut), this);
                    return;
                }
                if (px >= badgeX && px < badgeX + badgeW) {
                    QToolTip::showText(event->globalPosition().toPoint(),
                                       tipFor(c.trailOut), this);
                }
            }
        }
    }
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
    if (m_envelopeDragClipIdx >= 0 && m_envelopeDragPointIdx >= 0
        && m_envelopeDragClipIdx < m_clips.size()) {
        ClipInfo &clip = m_clips[m_envelopeDragClipIdx];
        if (m_envelopeDragPointIdx < clip.volumeEnvelope.size()) {
            // The dragged point may have crossed neighbours during the move
            // — re-sort so the AudioMixer evaluator's monotonic-time
            // assumption holds.
            std::sort(clip.volumeEnvelope.begin(), clip.volumeEnvelope.end(),
                      [](const AudioGainPoint &a, const AudioGainPoint &b) {
                          return a.time < b.time;
                      });
        }
        m_envelopeDragClipIdx = -1;
        m_envelopeDragPointIdx = -1;
        update();
        emit modified();
        emit interactionCompleted(QStringLiteral("Move volume keyframe"));
        if (event) event->accept();
        return;
    }
    if (m_dragMode == DragMode::MoveClip
        && m_dragClipIndex >= 0 && m_dragClipIndex < m_clips.size()) {
        // Fire modified() so Timeline saves an undo snapshot and the playback
        // schedule rebuilds with the new leadInSec gaps.
        if (!qFuzzyCompare(m_clips[m_dragClipIndex].leadInSec, m_dragOriginalLeadIn)) {
            emit modified();
            emit interactionCompleted(QStringLiteral("Move clip"));
        }
    }
    if (m_dragMode == DragMode::TrimLeft || m_dragMode == DragMode::TrimRight) {
        emit modified();
        emit interactionCompleted(QStringLiteral("Trim clip"));
    }
    if ((m_dragMode == DragMode::TransitionLeadInResize
         || m_dragMode == DragMode::TransitionTrailOutResize)
        && m_dragClipIndex >= 0 && m_dragClipIndex < m_clips.size()) {
        const ClipInfo &clip = m_clips[m_dragClipIndex];
        const double newDur = (m_dragMode == DragMode::TransitionLeadInResize)
            ? clip.leadIn.duration
            : clip.trailOut.duration;
        // Threshold guards against an unintended click-without-drag from
        // emitting modified() and pushing an empty undo entry.
        if (qAbs(newDur - m_dragOriginalTransitionDuration) > 0.01) {
            emit modified();
            emit interactionCompleted(QStringLiteral("Resize transition"));
        }
    }
    m_dragMode = DragMode::None; m_dragClipIndex = -1; m_dropTargetIndex = -1;
    m_dragOriginalLeadInNext = -1.0;
    m_dragOriginalTransitionDuration = 0.0;
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
    // Coalesce sequenceChanged storm: drag/scrub events used to emit on
    // every mouse-move (~30 Hz), each driving AudioMixer::seekTo +
    // QAudioSink stop/restart synchronously on the main thread. The 50 ms
    // single-shot timer collapses bursts to one emission per quiescent
    // window without delaying user-visible feedback (≤ 1 frame at 20 fps).
    m_emitSequenceTimer = new QTimer(this);
    m_emitSequenceTimer->setSingleShot(true);
    m_emitSequenceTimer->setInterval(50);
    connect(m_emitSequenceTimer, &QTimer::timeout, this, &Timeline::emitSequenceChangedNow);
    setupUI();
    saveUndoState("Initial state");
}

void Timeline::scheduleEmitSequenceChanged()
{
    if (m_emitSequenceTimer)
        m_emitSequenceTimer->start();
}

void Timeline::emitSequenceChangedNow()
{
    // Direct emit — never call scheduleEmitSequenceChanged() here, that
    // would restart the debounce timer and recurse.
    undotrace::log("emitSeqNow:enter");
    emit sequenceChanged(computePlaybackSequence());
    emit audioSequenceChanged(computeAudioPlaybackSequence());
    undotrace::log("emitSeqNow:exit");
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
    // must equal the total of all fixed-height widgets that sit above the
    // first track row on the right side: marker-lane (16 px) + time-ruler
    // (22 px) + playhead-overlay (15 px) = 53 px. The header column's
    // QVBoxLayout spacing (2 px) fires once after this widget, so we
    // subtract 2 px so the V/A separator on the left lines up with the
    // V/A separator on the right (otherwise the header column drifts 2 px
    // below the track column at the V/A boundary).
    auto *magnetArea = new QWidget(m_headerColumn);
    m_magnetArea = magnetArea;
    // 16 = MarkerLane::kLaneHeight (private, can't reference directly)
    // 22 = TimeRuler fixed height, 15 = PlayheadOverlay fixed height
    // -2 = compensates for the 2 px QVBoxLayout spacing that fires once
    //      after this widget before the first track header row.
    magnetArea->setFixedHeight(16 + 22 + 15 - 2);
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
    // AlwaysOn (not AsNeeded) so the bar stays visible while the timeline
    // pane is shrunk via the splitter — every track stays reachable.
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    auto *tracksContainer = new QWidget();
    auto *tracksOuterLayout = new QVBoxLayout(tracksContainer);
    tracksOuterLayout->setContentsMargins(0, 0, 0, 0);
    tracksOuterLayout->setSpacing(0);

    // Marker lane sits ABOVE the time ruler so colored triangles align with
    // ruler ticks and hover tooltips don't overlap track widgets. The lane
    // mirrors m_zoomLevel + scrollX so triangle positions track ruler ticks.
    m_markerLane = new MarkerLane(this, tracksContainer);
    m_markerLane->setPixelsPerSecond(m_zoomLevel);
    connect(m_markerLane, &MarkerLane::markerClicked,
            this, &Timeline::markerClicked);
    tracksOuterLayout->addWidget(m_markerLane);

    m_timeRuler = new TimeRuler(tracksContainer);
    m_timeRuler->setPixelsPerSecond(m_zoomLevel);
    connect(m_timeRuler, &TimeRuler::zoomChanged, this, &Timeline::setZoomLevel);
    connect(m_timeRuler, &TimeRuler::zoomDragStarted, this, &Timeline::captureZoomAnchor);
    connect(m_timeRuler, &TimeRuler::zoomDragEnded, this, &Timeline::clearZoomAnchor);
    tracksOuterLayout->addWidget(m_timeRuler);

    m_playheadOverlay = new PlayheadOverlay(tracksContainer);
    m_playheadOverlay->setFixedHeight(15);
    m_playheadOverlay->setStyleSheet("background-color: #222;");
    // Helper that clamps a candidate playhead content-x into the central 50%
    // of the visible viewport. Returns the bar position to draw and updates
    // m_playheadDragViewportX as a side effect (used by the auto-scroll timer).
    // Tighter than the older 70% so the timeline reveals more upcoming
    // content before the playhead leaves the comfort zone.
    auto resolvePlayheadDragX = [this](int contentX) -> int {
        int barX = contentX;
        if (m_scrollArea && m_scrollArea->viewport()) {
            QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
            if (hbar) {
                const int viewportW = m_scrollArea->viewport()->width();
                const int scrollX = hbar->value();
                const int viewportX = contentX - scrollX;
                const int leftZone = static_cast<int>(viewportW * 0.25);
                const int rightZone = static_cast<int>(viewportW * 0.75);
                if (viewportW > 0) {
                    // Only pin the bar at the 15% / 85% boundary if the
                    // timeline can actually keep scrolling in that direction.
                    // When the scrollbar is already at its rail (timeline
                    // start at min, last-clip-end + trailing pad at max),
                    // pinning would leave the start (content x = 0) and the
                    // end-of-content unreachable — the user could never drag
                    // the bar to position 0 or past the last clip.
                    if (viewportX < leftZone && scrollX > hbar->minimum())
                        barX = scrollX + leftZone;
                    else if (viewportX > rightZone && scrollX < hbar->maximum())
                        barX = scrollX + rightZone;
                }
                m_playheadDragViewportX = viewportX;
            }
        }
        return barX;
    };

    connect(m_playheadOverlay, &PlayheadOverlay::playheadMoved, this, [this, resolvePlayheadDragX](int x) {
        // First event of a press-drag cycle: place the bar exactly where the
        // user clicked (no clamp, no scroll) so a plain click on a position
        // in the outer 15% still lands the playhead there. Auto-scroll only
        // engages on subsequent moves.
        const bool firstEvent = !m_playheadDragging;
        m_playheadDragging = true;
        if (firstEvent) {
            m_playheadDragMoved = false;
            m_playheadPos = m_videoTrack->xToSeconds(x);
            if (m_playheadOverlay)
                m_playheadOverlay->setPlayheadX(x);
            if (m_scrollArea && m_scrollArea->horizontalScrollBar())
                m_playheadDragViewportX = x - m_scrollArea->horizontalScrollBar()->value();
            emit scrubPositionChanged(m_playheadPos);
            return;
        }
        m_playheadDragMoved = true;
        const int barX = resolvePlayheadDragX(x);
        m_playheadPos = m_videoTrack->xToSeconds(barX);
        if (m_playheadOverlay)
            m_playheadOverlay->setPlayheadX(barX);
        if (m_playheadAutoScrollTimer && !m_playheadAutoScrollTimer->isActive())
            m_playheadAutoScrollTimer->start();
        emit scrubPositionChanged(m_playheadPos);
    });
    connect(m_playheadOverlay, &PlayheadOverlay::playheadReleased, this, [this, resolvePlayheadDragX](int x) {
        const bool useClamp = m_playheadDragMoved;
        m_playheadDragging = false;
        m_playheadDragMoved = false;
        if (m_playheadAutoScrollTimer)
            m_playheadAutoScrollTimer->stop();
        // Pure click (no drag) lands the bar exactly where the user clicked
        // — even inside the outer 15% scroll zone — so a single click in any
        // visible area is a precise scrub. A release after an actual drag,
        // however, must keep the clamped position so the bar matches what
        // the user saw mid-drag instead of snapping to the raw cursor x.
        const int barX = useClamp ? resolvePlayheadDragX(x) : x;
        m_playheadPos = m_videoTrack->xToSeconds(barX);
        if (m_playheadOverlay)
            m_playheadOverlay->setPlayheadX(barX);
        emit positionChanged(m_playheadPos);
    });

    if (!m_playheadAutoScrollTimer) {
        m_playheadAutoScrollTimer = new QTimer(this);
        m_playheadAutoScrollTimer->setInterval(16); // ~60 Hz while drag-scrolling
        connect(m_playheadAutoScrollTimer, &QTimer::timeout,
                this, &Timeline::onPlayheadAutoScrollTick);
    }
    tracksOuterLayout->addWidget(m_playheadOverlay);

    m_tracksWidget = new QWidget();
    m_tracksLayout = new QVBoxLayout(m_tracksWidget);
    m_tracksLayout->setContentsMargins(0, 0, 0, 0);
    m_tracksLayout->setSpacing(2);

    // Create initial V1 and A1.
    m_videoTrack = new TimelineTrack(this);
    m_audioTrack = new TimelineTrack(this);
    m_audioTrack->setIsAudioTrack(true);
    m_videoTrack->setTimeline(this);
    m_audioTrack->setTimeline(this);
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
    // Belt-and-suspenders sync: whatever resize path actually changes the
    // textStrip's height (callback, layout invalidation, refreshTextStrip
    // re-entry, etc.), mirror it onto the header column's matching row so
    // the V1 lock/mute icons never drift away from the V1 clip block. The
    // explicit setFixedHeight calls in the row-height callback should keep
    // these in sync, but the resize-event filter is the empirical fallback
    // for any path that bypasses the callback.
    {
        struct HeightFollower : public QObject {
            QPointer<QWidget> follower;
            bool eventFilter(QObject *obj, QEvent *e) override {
                if (e->type() == QEvent::Resize) {
                    if (auto *src = qobject_cast<QWidget*>(obj)) {
                        if (follower && follower->height() != src->height())
                            follower->setFixedHeight(src->height());
                    }
                }
                return false;
            }
        };
        auto *follower = new HeightFollower;
        follower->follower = m_textStripHeader;
        follower->setParent(m_textStrip);
        m_textStrip->installEventFilter(follower);
    }
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

    // Keep the marker lane's content-x offset in sync with horizontal
    // scrolling so triangles stay aligned with their underlying clips
    // while the user pans. Cheap signal — only fires on scrollbar value
    // changes, not on every paint.
    if (m_markerLane) {
        if (QScrollBar *hbar = m_scrollArea->horizontalScrollBar()) {
            connect(hbar, &QScrollBar::valueChanged,
                    m_markerLane, &MarkerLane::setScrollOffset);
        }
    }

    // Install event filters so clicks on empty areas (below tracks, outer
    // container, scroll viewport) deselect all clips like clicks on a track's
    // empty area do.
    m_tracksWidget->installEventFilter(this);
    tracksContainer->installEventFilter(this);
    m_scrollArea->viewport()->installEventFilter(this);

    // Wrap the header column in its own QScrollArea so the V/A header
    // rows scroll vertically in lockstep with the right-side track scroll.
    // Without this, scrolling the tracks down leaves the header labels
    // pinned at the top while the clip rows slide upward — the lock /
    // mute icons drift away from the clips they label.
    auto *headerScrollArea = new QScrollArea(contentArea);
    headerScrollArea->setWidgetResizable(true);
    headerScrollArea->setFrameShape(QFrame::NoFrame);
    headerScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    headerScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    headerScrollArea->setFixedWidth(kHeaderColumnWidth);
    headerScrollArea->setStyleSheet("background-color: #252525;");
    headerScrollArea->setWidget(m_headerColumn);

    // Sync vertical scroll: when the user scrolls the tracks side, the
    // header column must follow so V1's icons stay next to the V1 row.
    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            headerScrollArea->verticalScrollBar(), &QScrollBar::setValue);

    contentHbox->addWidget(headerScrollArea);
    contentHbox->addWidget(m_scrollArea, 1);

    layout->addWidget(contentArea);
    setStyleSheet("background-color: #333;");

    // Floor for the splitter so the preview can grow vertically; tracks that
    // overflow this height stay reachable via the always-on scroll bar above.
    setMinimumHeight(150);

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
            scheduleEmitSequenceChanged();
            scheduleEmitSequenceChanged();
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
            scheduleEmitSequenceChanged();
            scheduleEmitSequenceChanged();
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
    track->setTimeline(this);

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
    track->setIsAudioTrack(true);
    track->setPixelsPerSecond(m_zoomLevel);
    track->setSnapEnabled(snapEnabled());
    track->setRowHeight(m_trackHeight);
    track->setTimeline(this);

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

void Timeline::repaintAudioTracks()
{
    for (auto *t : m_audioTracks)
        if (t) t->update();
}

void Timeline::applyDuckingFromTrack(int voiceTrackIdx,
                                     double duckGain,
                                     double attackSec,
                                     double releaseSec)
{
    if (voiceTrackIdx < 0 || voiceTrackIdx >= m_audioTracks.size()) return;
    auto *voice = m_audioTracks[voiceTrackIdx];
    if (!voice) return;

    // Build the voice timeline footprint as a list of (start, end) seconds.
    // Each clip lays out sequentially as leadInSec gap + effectiveDuration.
    QVector<QPair<double, double>> voiceRanges;
    {
        double accum = 0.0;
        for (const auto &c : voice->clips()) {
            accum += c.leadInSec;
            const double dur = c.effectiveDuration();
            if (dur > 0.0) voiceRanges.append({accum, accum + dur});
            accum += dur;
        }
    }
    if (voiceRanges.isEmpty()) return;

    duckGain = qBound(0.0, duckGain, 2.0);
    attackSec = qMax(0.0, attackSec);
    releaseSec = qMax(0.0, releaseSec);

    bool anyChanged = false;
    for (int t = 0; t < m_audioTracks.size(); ++t) {
        if (t == voiceTrackIdx) continue;
        auto *bgm = m_audioTracks[t];
        if (!bgm) continue;

        auto bgmClips = bgm->clips();
        bool trackChanged = false;
        double bgmAccum = 0.0;
        for (auto &bc : bgmClips) {
            bgmAccum += bc.leadInSec;
            const double bgmStart = bgmAccum;
            const double bgmEnd = bgmAccum + bc.effectiveDuration();
            const double staticGain = bc.volume;
            QVector<AudioGainPoint> env;

            for (const auto &vr : voiceRanges) {
                const double duckStart = vr.first - attackSec;
                const double duckEnd   = vr.second + releaseSec;
                if (duckEnd <= bgmStart || duckStart >= bgmEnd) continue;

                // Clip the four envelope anchors to the BGM clip's local
                // time window (0..effectiveDuration). Any anchor that lands
                // outside the clip is clamped to the boundary.
                const double bgmDur = bgmEnd - bgmStart;
                const double localAttackStart = qBound(0.0,
                    duckStart - bgmStart, bgmDur);
                const double localVoiceStart  = qBound(0.0,
                    vr.first  - bgmStart, bgmDur);
                const double localVoiceEnd    = qBound(0.0,
                    vr.second - bgmStart, bgmDur);
                const double localReleaseEnd  = qBound(0.0,
                    duckEnd   - bgmStart, bgmDur);

                env.append({localAttackStart, staticGain});
                env.append({localVoiceStart,  duckGain});
                env.append({localVoiceEnd,    duckGain});
                env.append({localReleaseEnd,  staticGain});
            }

            if (!env.isEmpty()) {
                std::sort(env.begin(), env.end(),
                          [](const AudioGainPoint &a, const AudioGainPoint &b) {
                              return a.time < b.time;
                          });
                if (bc.volumeEnvelope.isEmpty()) {
                    bc.volumeEnvelope = env;
                } else {
                    // Merge duck envelope with existing user envelope:
                    // for each duck point, insert it taking MIN gain so
                    // user boost points still allow ducking down but don't
                    // override a manual fade.
                    QVector<AudioGainPoint> merged = bc.volumeEnvelope;
                    std::sort(merged.begin(), merged.end(),
                              [](const AudioGainPoint &a, const AudioGainPoint &b) {
                                  return a.time < b.time;
                              });
                    const int origEnvSize = merged.size();
                    for (const auto &dp : env) {
                        // Find the existing gain at this time by linear
                        // interpolation between adjacent ORIGINAL points
                        // (search only within the pre-existing envelope).
                        double existingGain = dp.gain;
                        if (origEnvSize == 0) {
                            existingGain = 1.0;
                        } else if (origEnvSize == 1) {
                            existingGain = merged[0].gain;
                        } else {
                            for (int i = 0; i < origEnvSize; ++i) {
                                if (merged[i].time >= dp.time) {
                                    if (i == 0) {
                                        existingGain = merged[0].gain;
                                    } else {
                                        const double t0 = merged[i - 1].time;
                                        const double t1 = merged[i].time;
                                        const double g0 = merged[i - 1].gain;
                                        const double g1 = merged[i].gain;
                                        if (t1 - t0 > 0.0) {
                                            const double ratio = (dp.time - t0) / (t1 - t0);
                                            existingGain = g0 + ratio * (g1 - g0);
                                        } else {
                                            existingGain = g0;
                                        }
                                    }
                                    break;
                                }
                                if (i == origEnvSize - 1) {
                                    existingGain = merged[i].gain;
                                }
                            }
                        }
                        merged.append({dp.time, qMin(existingGain, dp.gain)});
                    }
                    std::sort(merged.begin(), merged.end(),
                              [](const AudioGainPoint &a, const AudioGainPoint &b) {
                                  return a.time < b.time;
                              });
                    // De-duplicate points within 1 ms
                    QVector<AudioGainPoint> deduped;
                    for (const auto &p : merged) {
                        if (!deduped.isEmpty() && qAbs(p.time - deduped.last().time) <= 0.001) {
                            // Take the min gain at this time cluster
                            deduped.last().gain = qMin(deduped.last().gain, p.gain);
                        } else {
                            deduped.append(p);
                        }
                    }
                    bc.volumeEnvelope = deduped;
                }
                trackChanged = true;
            }
            bgmAccum += bc.effectiveDuration();
        }
        if (trackChanged) {
            bgm->setClips(bgmClips);
            anyChanged = true;
        }
    }

    if (anyChanged) {
        saveUndoState(QString("BGM auto-duck (voice = A%1)").arg(voiceTrackIdx + 1));
        scheduleEmitSequenceChanged();
        repaintAudioTracks();
    }
}

void Timeline::addClip(const QString &filePath)
{
    AVFormatContext *fmt = nullptr;
    double duration = 0.0;
    // Phase 1e Win #11 — auto-proxy for heavy video sources. AV1 SW/HW
    // decode runs 4–10x heavier than H.264 (no SIMD-friendly inverse
    // transforms in libdav1d; the D3D11VA path still pays full-res
    // av_hwframe_transfer_data per tick), and 1440p+ sources stress
    // GPU↔CPU bandwidth in multi-track playback regardless of codec.
    // The architect/tracer/critic audit traced user-reported multi-second
    // freezes to AV1 1440p × 2 multi-track with no proxy entries
    // registered — getProxyPath returned the original path so V2/V3
    // pool decoders opened the full-res AV1 source directly. ProxyManager
    // already supports background h264 proxy generation; the gap was that
    // Timeline::addClip never enqueued it. Trigger generation here on
    // import so the existing sequenceChanged → getProxyPath wiring picks
    // up the proxy as soon as it lands. Kept opt-out via
    // VEDITOR_AUTO_PROXY_DISABLE=1 in case a user wants to skip the
    // first-import encode wait, and gated on resolution/codec to avoid
    // generating proxies for clips that already play smoothly.
    bool wantsAutoProxy = false;
    bool videoStreamFound = false;
    int capturedPrimaries = 0;
    int capturedTrc = 0;
    int capturedBitDepth = 8;
    bool capturedHasHdrMeta = false;
    // ソース映像の素のピクセル寸法 (アスペクト既定フィット判定用)。probe ループで
    // 最初の映像ストリームから捕捉する。0 = 未取得 (probe 失敗/静止画非対応など)。
    int srcVideoW = 0;
    int srcVideoH = 0;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->duration > 0)
            duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream *st = fmt->streams[i];
            if (!st || !st->codecpar
                || st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                continue;
            const int w = st->codecpar->width;
            const int h = st->codecpar->height;
            videoStreamFound = true;
            srcVideoW = w;
            srcVideoH = h;
            const hdringest::ColorInputs colorInputs =
                hdringest::captureColorInputs(st->codecpar);
            capturedPrimaries = colorInputs.primaries;
            capturedTrc = colorInputs.trc;
            capturedBitDepth = colorInputs.bitDepth;
            capturedHasHdrMeta = colorInputs.hasHdrMeta;
            const bool isAv1  = st->codecpar->codec_id == AV_CODEC_ID_AV1;
            const bool isQhdPlus = (w >= 2560) || (h >= 1440);
            // h.264 1080p+ も対象 (cinemascope や ultra-wide 含めるため OR)。
            // 1920×800 シネスコや 3840×800 ultra-wide も raw decode 負荷は
            // 1080p に匹敵するので bandwidth 観点で OR の方が semantic に近い。
            // single-track 編集なら直接 decode で問題ないが、PiP 4 並列の中に
            // 1080p raw が混ざると compose に追いつかなくなり smooth な PiP
            // source (proxy 360p) と並べたとき差が出る。MultiTrackOnly mode の
            // videoTrackIdx >= 1 gate (下記 switch) が V1 単体編集を保護する
            // ので過剰 encode は起きない。
            const bool isHdPlus = (w >= 1920) || (h >= 1080);
            if (isAv1 || isQhdPlus || isHdPlus)
                wantsAutoProxy = true;
            break;
        }
        avformat_close_input(&fmt);
    }
    // NOTE: auto-proxy trigger moved past the track-index decision below
    // (search for "autoProxyMode" in this function). wantsAutoProxy is
    // the probe verdict consumed there.
    ClipInfo clip;
    clip.filePath = filePath;
    clip.displayName = QFileInfo(filePath).fileName();
    clip.duration = duration;
    // Every freshly-added clip gets a unique linkGroup shared by its video
    // and audio halves so the two stay locked together until the user
    // explicitly severs the sync via the clip's context menu.
    clip.linkGroup = allocateLinkGroup();

    const bool hdrIngestEnabled = hdringest::enabledFromEnv();
    const bool hdrTraceEnabled = hdringest::traceEnabledFromEnv();
    clipcolor::ColorMeta derivedColorMeta = clip.colorMeta;
    if (videoStreamFound && (hdrIngestEnabled || hdrTraceEnabled))
        derivedColorMeta = clipcolor::fromCodecParams(capturedPrimaries, capturedTrc,
                                                      capturedBitDepth,
                                                      capturedHasHdrMeta);
    if (hdrIngestEnabled && videoStreamFound)
        clip.colorMeta = derivedColorMeta;
    if (hdrTraceEnabled) {
        qInfo().noquote() << "[hdr-ingest] file=" << clip.displayName
                          << "ingest=" << (hdrIngestEnabled ? "on" : "off")
                          << "videoStreamFound=" << videoStreamFound
                          << "derived=" << clipcolor::describe(derivedColorMeta)
                          << "clip=" << clipcolor::describe(clip.colorMeta);
    }

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

    // Auto-proxy dispatch — runs after the track index is settled so that
    // MultiTrackOnly mode can gate on V2-or-later. wantsAutoProxy was
    // probed up at the top of this function (AV1 OR ≥2560×1440).
    if (wantsAutoProxy) {
        static const bool autoProxyDisabled =
            qEnvironmentVariableIntValue("VEDITOR_AUTO_PROXY_DISABLE") != 0;
        if (!autoProxyDisabled) {
            const auto mode = static_cast<AutoProxyMode>(
                prefs.value("autoProxyMode",
                            static_cast<int>(AutoProxyMode::MultiTrackOnly)).toInt());
            bool shouldTrigger = false;
            switch (mode) {
                case AutoProxyMode::Disabled:
                    break;
                case AutoProxyMode::MultiTrackOnly:
                    shouldTrigger = (videoTrackIdx >= 1);
                    break;
                case AutoProxyMode::Always:
                    shouldTrigger = true;
                    break;
            }
            if (shouldTrigger) {
                ProxyManager &pm = ProxyManager::instance();
                // Intentionally do NOT gate on pm.isProxyMode() here: proxy
                // mode controls *playback* substitution, but generating on
                // import is cheap (idempotent, background QProcess) and
                // pays off the moment the user toggles proxy mode on.
                if (pm.config().enabled && !pm.hasProxy(filePath)) {
                    qDebug() << "[auto-proxy] queuing proxy for heavy source"
                             << "file=" << filePath
                             << "mode=" << static_cast<int>(mode)
                             << "videoTrackIdx=" << videoTrackIdx;
                    pm.generateProxy(filePath);
                }
            }
        }
    }

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

    // Premiere 準拠の既定フィット: アスペクトの異なるクリップを explicit 出力
    // (SNS 縦動画プリセット等で確定したサイズ) のプロジェクトへ追加したとき、
    // 既定 (fit=none) だと renderLayer の IgnoreAspectRatio 充填で縦/横に
    // 引き伸ばされる。Premiere は決してアスペクトを歪めないため、ここで contain
    // (レターボックス) を既定にする。判定は render 時と同一述語 snsfit::shouldContain
    // をそのまま使う (アスペクト差 < 1e-4 のマッチクリップは false=フラグ未設定で
    // byte-identical、render SSOT は不変)。explicit 出力でないプロジェクトは従来どおり
    // none のまま (挙動不変)。cover やフィット解除へはクリップ右クリックで上書き可能。
    if (videoStreamFound && m_projectExplicitOutput
        && srcVideoW > 0 && srcVideoH > 0) {
        const QSize projOut(m_projectWidth, m_projectHeight);
        if (snsfit::shouldContain(true, projOut, QSize(srcVideoW, srcVideoH)))
            clip.fitContain = true;
    }

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
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
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
        remapTimelineCarrierAfterMutation(this, m_trackMatteEntries, snapBefore);
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
        saveUndoState("Split clip");
        updateInfoLabel();
    }
}

void Timeline::deleteSelectedClip()
{
    // RM-5: snapshot carrier keys BEFORE the delete so we can remap after.
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
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
        remapTimelineCarrierAfterMutation(this, m_trackMatteEntries, snapBefore);
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
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

bool Timeline::applyTrackingToOverlay(int overlayIndex, const EnhancedTextOverlay &updated)
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
    mgr.updateOverlay(overlayIndex, updated);
    track->setClips(clips);
    saveUndoState("Apply tracking to overlay");
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
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::setClipMotion(int trackIdx, int clipIdx,
                             const effectctrl::MotionState &motion)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return;
    auto *track = m_videoTracks[trackIdx];
    if (!track)
        return;

    QVector<ClipInfo> clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return;

    ClipInfo &clip = clips[clipIdx];
    clip.videoScale = qBound(0.0, motion.scale, 10.0);
    // videoDx/videoDy are stored as NORMALIZED fractions of canvas W/H
    // (same ±5.0 convention as setClipVideoTransform and all renderers).
    // All callers pass already-normalized values — clamp only.
    clip.videoDx = qBound(-5.0, motion.dx, 5.0);
    clip.videoDy = qBound(-5.0, motion.dy, 5.0);
    clip.rotation2DDegrees = qBound(-360.0, motion.rotation2DDeg, 360.0);
    clip.opacity = qBound(0.0, motion.opacity, 1.0);
    clip.is3DLayer = motion.is3DLayer;
    clip.layer3D.positionZ = motion.is3DLayer
        ? qBound(-10000.0, motion.posZ, 10000.0) : 0.0;
    clip.layer3D.rotationX = motion.is3DLayer
        ? qBound(-360.0, motion.rotX, 360.0) : 0.0;
    clip.layer3D.rotationY = motion.is3DLayer
        ? qBound(-360.0, motion.rotY, 360.0) : 0.0;
    clip.layer3D.rotationZ = motion.is3DLayer
        ? qBound(-360.0, motion.rotZ, 360.0) : 0.0;

    track->setClips(clips);
    emitSequenceChangedNow();
    emit positionChanged(m_playheadPos);
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
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    int insertAt = m_videoTrack->selectedClip() + 1;
    if (insertAt <= 0) insertAt = m_videoTrack->clipCount();
    const int maxIndex = qMin(m_videoTrack->clipCount(), m_audioTrack->clipCount());
    insertAt = qBound(0, insertAt, maxIndex);
    m_videoTrack->insertClip(insertAt, m_clipboard.value());
    m_audioTrack->insertClip(insertAt, m_clipboard.value());
    m_videoTrack->setSelectedClip(insertAt);
    m_audioTrack->setSelectedClip(insertAt);
    remapTimelineCarrierAfterMutation(this, m_trackMatteEntries, snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
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
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
    updateInfoLabel();
}

// PV-B SSOT: クリップ操作をプレビュー右クリックメニューと共有するため公開
// メソッドへ抽出。showClipContextMenu の各分岐と同一ロジック(挙動不変)。
void Timeline::applySnsFitToClip(TimelineTrack *track, int clipIndex,
                                 bool contain, bool cover, const QString &undoLabel)
{
    if (!track) return;
    QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size())
        return;
    ClipInfo &clip = clips[clipIndex];
    clip.fitContain = contain;
    clip.fitCover = cover;
    clip.videoScale = 1.0;
    clip.videoDx = 0.0;
    clip.videoDy = 0.0;
    clip.rotation2DDegrees = 0.0;
    track->setClips(clips);
    saveUndoState(undoLabel);
    emitSequenceChangedNow();
    emit positionChanged(m_playheadPos);
}

void Timeline::applySilenceCutToClip(TimelineTrack *track, int clipIndex)
{
    if (!track) return;
    QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size()) return;
    const ClipInfo &src = clips[clipIndex];

    QVector<float> samples;
    int sr = 0;
    if (!WaveformGenerator::decodeAudio(src.filePath, samples, sr) || samples.isEmpty() || sr <= 0) {
        QMessageBox::warning(nullptr, QStringLiteral("無音カット"),
                             QStringLiteral("音声のデコードに失敗しました。"));
        return;
    }

    const double srcOut = (src.outPoint > 0.0) ? src.outPoint : src.duration;
    const double totalSec = static_cast<double>(samples.size()) / sr;
    const double activeStart = std::max(src.inPoint, 0.0);
    const double activeEnd   = std::min(srcOut, totalSec);
    if (activeEnd <= activeStart) {
        QMessageBox::information(nullptr, QStringLiteral("無音カット"),
                                 QStringLiteral("有効な範囲がありません。"));
        return;
    }
    const int startSample = static_cast<int>(activeStart * sr);
    const int endSample   = static_cast<int>(activeEnd   * sr);
    QVector<float> activeSamples = samples.mid(startSample, endSample - startSample);

    QVector<silencecut::Segment> keeps =
        silencecut::detectKeepSegments(activeSamples, sr, silencecut::Config{});
    QVector<ClipInfo> subClips = silencecut::planKeepClips(src, keeps);

    if (subClips.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("無音カット"),
                                 QStringLiteral("カットする無音が見つかりませんでした。"));
        return;
    }
    if (subClips.size() == 1
        && qAbs(subClips[0].inPoint  - src.inPoint) < 0.01
        && qAbs(subClips[0].outPoint - srcOut)      < 0.01) {
        QMessageBox::information(nullptr, QStringLiteral("無音カット"),
                                 QStringLiteral("カットする無音がありません。"));
        return;
    }

    const QVector<silencecut::Segment> silences =
        silencecut::detectSilenceSegments(activeSamples, sr, silencecut::Config{});
    const int silenceCount = static_cast<int>(silences.size());
    const QString msg = QStringLiteral("%1 箇所の無音を除去し、%2 個のクリップに分割します。実行しますか?")
                            .arg(silenceCount)
                            .arg(subClips.size());
    if (QMessageBox::question(nullptr, QStringLiteral("無音カット"), msg)
            != QMessageBox::Yes)
        return;

    for (int i = 0; i < subClips.size(); ++i) {
        if (i > 0)
            subClips[i].leadInSec = 0.0;
        subClips[i].linkGroup = allocateLinkGroup();
    }

    QVector<ClipInfo> newClips = clips;
    newClips.removeAt(clipIndex);
    for (int i = 0; i < subClips.size(); ++i)
        newClips.insert(clipIndex + i, subClips[i]);

    track->setClips(newClips);
    saveUndoState(QStringLiteral("Silence auto-cut"));
    emitSequenceChangedNow();
    emit positionChanged(m_playheadPos);
}

void Timeline::applyBeatMarkersToClip(TimelineTrack *track, int clipIndex)
{
    if (!track) return;
    const QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size()) return;
    const ClipInfo &src = clips[clipIndex];

    QVector<float> samples;
    int sr = 0;
    if (!WaveformGenerator::decodeAudio(src.filePath, samples, sr) || samples.isEmpty() || sr <= 0) {
        QMessageBox::warning(nullptr, QStringLiteral("ビートマーカー"),
                             QStringLiteral("音声のデコードに失敗しました。"));
        return;
    }

    const double srcOut = (src.outPoint > 0.0) ? src.outPoint : src.duration;
    const double totalSec = static_cast<double>(samples.size()) / sr;
    const double activeStart = std::max(src.inPoint, 0.0);
    const double activeEnd   = std::min(srcOut, totalSec);
    if (activeEnd <= activeStart) {
        QMessageBox::information(nullptr, QStringLiteral("ビートマーカー"),
                                 QStringLiteral("有効な範囲がありません。"));
        return;
    }
    const int startSample = static_cast<int>(activeStart * sr);
    const int endSample   = static_cast<int>(activeEnd   * sr);
    const QVector<float> activeSamples = samples.mid(startSample, endSample - startSample);

    const beatdetect::Result beats =
        beatdetect::detectBeats(activeSamples, sr, beatdetect::Config{});
    if (beats.beatTimesSec.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("ビートマーカー"),
                                 QStringLiteral("ビートが検出されませんでした。"));
        return;
    }

    double clipStartSec = 0.0;
    for (int i = 0; i < clipIndex && i < clips.size(); ++i)
        clipStartSec += clips[i].leadInSec + clips[i].effectiveDuration();
    clipStartSec += src.leadInSec;
    const double speed = (src.speed > 0.0) ? src.speed : 1.0;

    const QString msg = QStringLiteral("%1 個のビート (推定 %2 BPM) をマーカーとして追加しますか?")
                            .arg(beats.beatTimesSec.size())
                            .arg(beats.bpm, 0, 'f', 1);
    if (QMessageBox::question(nullptr, QStringLiteral("ビートマーカー"), msg)
            != QMessageBox::Yes)
        return;

    const QColor beatColor(QStringLiteral("#39c0ff"));
    int added = 0;
    for (double b : beats.beatTimesSec) {
        const double sourceSec = activeStart + b;
        const double timelineSec = clipStartSec + (sourceSec - src.inPoint) / speed;
        if (timelineSec < 0.0)
            continue;
        const qint64 timelineUs = static_cast<qint64>(timelineSec * 1.0e6);
        addMarker(timelineUs, QStringLiteral("Beat %1").arg(added + 1), beatColor);
        ++added;
    }
    saveUndoState(QStringLiteral("Beat markers"));
    if (added > 0)
        emit positionChanged(m_playheadPos);
}

// 再生ヘッド直下の V1(最初の動画トラック)クリップを解決。見つかれば true。
bool Timeline::clipUnderPlayhead(TimelineTrack *&outTrack, int &outClipIndex) const
{
    outTrack = nullptr;
    outClipIndex = -1;
    if (m_videoTracks.isEmpty())
        return false;
    TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    if (!v1)
        return false;
    const QVector<ClipInfo> &clips = v1->clips();
    const double playSec = m_playheadPos;
    double start = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        const double clipStart = start + clips[i].leadInSec;
        const double clipEnd = clipStart + clips[i].effectiveDuration();
        if (playSec >= clipStart && playSec < clipEnd) {
            outTrack = v1;
            outClipIndex = i;
            return true;
        }
        start = clipEnd;
    }
    return false;
}

void Timeline::showClipContextMenu(TimelineTrack *track, int clipIndex, const QPoint &globalPos)
{
    if (!track || clipIndex < 0 || clipIndex >= track->clips().size()) return;

    // Make sure the right-clicked clip is the active selection — NLE users
    // expect "right click acts on the thing under the cursor".
    if (!track->isClipSelected(clipIndex))
        track->setSelectedClip(clipIndex);

    if (QSettings("VSimpleEditor", "Preferences")
            .value(rcpause::pauseOnRightClickKey(),
                   rcpause::kDefaultPauseOnRightClick).toBool()) {
        if (QWidget *topLevel = window()) {
            if (auto *player = topLevel->findChild<VideoPlayer *>())
                player->pause();
        }
    }

    // Audio-track context menu — visual transitions don't apply, so we
    // show a simplified menu with just the audio-relevant transitions
    // (FadeIn/Out + equal-power CrossDissolve) plus the standard
    // edit ops. AudioMixer reads leadIn/trailOut directly from the audio
    // clip so setting these here gives an audio-only fade with no video
    // mirror. Used for J-cut / L-cut workflows where audio fades earlier
    // or later than the video cut.
    if (m_audioTracks.contains(track)) {
        const ClipInfo &aClip = track->clips()[clipIndex];
        const int aLinkGroup = aClip.linkGroup;
        const bool aHasTrans = aClip.leadIn.type != TransitionType::None
                            || aClip.trailOut.type != TransitionType::None;
        QMenu aMenu;
        QAction *aCut = aMenu.addAction(QStringLiteral("カット"));
        QAction *aCopy = aMenu.addAction(QStringLiteral("コピー"));
        QAction *aDel = aMenu.addAction(QStringLiteral("削除"));
        aMenu.addSeparator();
        QAction *aUnlink = aMenu.addAction(QStringLiteral("同期を切る"));
        aUnlink->setEnabled(aLinkGroup > 0);
        aMenu.addSeparator();
        QAction *aNormalize = aMenu.addAction(QStringLiteral("ノーマライズ"));
        aMenu.addSeparator();
        QMenu *atMenu = aMenu.addMenu(QStringLiteral("音声トランジション"));
        QAction *aXdAct = atMenu->addAction(QStringLiteral("クロスフェード (1.0s)"));
        QAction *aFiAct = atMenu->addAction(QStringLiteral("フェードイン (0.5s)"));
        QAction *aFoAct = atMenu->addAction(QStringLiteral("フェードアウト (0.5s)"));
        QAction *aClearAct = nullptr;
        if (aHasTrans) {
            atMenu->addSeparator();
            aClearAct = atMenu->addAction(QStringLiteral("音声トランジション削除"));
        }
        QAction *aChosen = aMenu.exec(globalPos);
        if (!aChosen) return;
        // Helper: mutate this audio track only (no video mirror) so the
        // audio fade can outlive or precede the video cut for J/L-cuts.
        auto applyAudioOnly = [&](TransitionType type, double duration) {
            auto clips = track->clips();
            if (clipIndex >= clips.size()) return;
            Transition t;
            t.type = type;
            t.duration = duration;
            if (type == TransitionType::FadeIn) {
                clips[clipIndex].leadIn = t;
                if (clipIndex > 0) {
                    Transition mirror;
                    mirror.type = TransitionType::FadeOut;
                    mirror.duration = duration;
                    clips[clipIndex - 1].trailOut = mirror;
                }
            } else if (type == TransitionType::FadeOut) {
                clips[clipIndex].trailOut = t;
                if (clipIndex + 1 < clips.size()) {
                    Transition mirror;
                    mirror.type = TransitionType::FadeIn;
                    mirror.duration = duration;
                    clips[clipIndex + 1].leadIn = mirror;
                }
            } else { // CrossDissolve (audio-only equal-power crossfade)
                clips[clipIndex].trailOut = t;
                if (clipIndex + 1 < clips.size())
                    clips[clipIndex + 1].leadIn = t;
            }
            track->setClips(clips);
            saveUndoState(QString("Audio-only transition: %1")
                .arg(Transition::typeName(type)));
            scheduleEmitSequenceChanged();
        };
        if (aChosen == aCut) cutSelectedClip();
        else if (aChosen == aCopy) copySelectedClip();
        else if (aChosen == aDel) deleteSelectedClip();
        else if (aChosen == aUnlink) unlinkClipGroup(aLinkGroup);
        else if (aChosen == aNormalize) {
            const int trackIdx = m_audioTracks.indexOf(track);
            normalizeAudioClipPeak(trackIdx, clipIndex);
        }
        else if (aChosen == aXdAct) applyAudioOnly(TransitionType::CrossDissolve, 1.0);
        else if (aChosen == aFiAct) applyAudioOnly(TransitionType::FadeIn, 0.5);
        else if (aChosen == aFoAct) applyAudioOnly(TransitionType::FadeOut, 0.5);
        else if (aClearAct && aChosen == aClearAct) {
            auto clips = track->clips();
            if (clipIndex < clips.size()) {
                const bool hadLead = clips[clipIndex].leadIn.type != TransitionType::None;
                const bool hadTrail = clips[clipIndex].trailOut.type != TransitionType::None;
                if (hadLead && clipIndex > 0
                    && clips[clipIndex - 1].trailOut.type != TransitionType::None) {
                    clips[clipIndex - 1].trailOut = Transition{};
                }
                if (hadTrail && clipIndex + 1 < clips.size()
                    && clips[clipIndex + 1].leadIn.type != TransitionType::None) {
                    clips[clipIndex + 1].leadIn = Transition{};
                }
                clips[clipIndex].leadIn = Transition{};
                clips[clipIndex].trailOut = Transition{};
                track->setClips(clips);
                saveUndoState("Clear audio transitions");
                scheduleEmitSequenceChanged();
            }
        }
        return;
    }

    const ClipInfo &clipInfo = track->clips()[clipIndex];
    const int linkGroup = clipInfo.linkGroup;
    const bool hasTransition = clipInfo.leadIn.type != TransitionType::None
                            || clipInfo.trailOut.type != TransitionType::None;

    QMenu menu;
    QAction *cutAct = menu.addAction(QStringLiteral("カット"));
    QAction *copyAct = menu.addAction(QStringLiteral("コピー"));
    QAction *deleteAct = menu.addAction(QStringLiteral("削除"));
    QAction *silenceCutAct = menu.addAction(QStringLiteral("無音を自動カット..."));
    QAction *beatMarkerAct = menu.addAction(QStringLiteral("ビートでマーカー..."));
    menu.addSeparator();
    QAction *unlinkAct = menu.addAction(QStringLiteral("同期を切る"));
    unlinkAct->setEnabled(linkGroup > 0);
    QAction *relinkAct = menu.addAction(QStringLiteral("再同期"));
    relinkAct->setEnabled(linkGroup == 0);

    menu.addSeparator();
    QMenu *transitionMenu = menu.addMenu(QStringLiteral("トランジション"));
    QAction *xdAct = transitionMenu->addAction(QStringLiteral("クロスディゾルブ (1.0s)"));
    QAction *fdAct = transitionMenu->addAction(QStringLiteral("フィルムディゾルブ (1.0s)"));
    QAction *ddAct = transitionMenu->addAction(QStringLiteral("ディザディゾルブ (1.0s)"));
    QAction *blAct = transitionMenu->addAction(QStringLiteral("ブラーディゾルブ (1.0s)"));
    QAction *pxAct = transitionMenu->addAction(QStringLiteral("ピクセレート (1.0s)"));
    QAction *fiAct = transitionMenu->addAction(QStringLiteral("フェードイン (0.5s)"));
    QAction *foAct = transitionMenu->addAction(QStringLiteral("フェードアウト (0.5s)"));
    QAction *dbAct = transitionMenu->addAction(QStringLiteral("黒へディップ (1.0s)"));
    QAction *dwAct = transitionMenu->addAction(QStringLiteral("白へディップ (1.0s)"));
    transitionMenu->addSeparator();
    QMenu *wipeMenu = transitionMenu->addMenu(QStringLiteral("ワイプ (1.0s)"));
    QAction *wlAct = wipeMenu->addAction(QStringLiteral("左 → 右"));
    QAction *wrAct = wipeMenu->addAction(QStringLiteral("右 → 左"));
    QAction *wuAct = wipeMenu->addAction(QStringLiteral("上 → 下"));
    QAction *wdAct = wipeMenu->addAction(QStringLiteral("下 → 上"));
    QAction *cwAct  = wipeMenu->addAction(QStringLiteral("時計回り"));
    QAction *cwcAct = wipeMenu->addAction(QStringLiteral("反時計回り"));
    QAction *bhAct  = wipeMenu->addAction(QStringLiteral("バーンドア (水平・開く)"));
    QAction *bhcAct = wipeMenu->addAction(QStringLiteral("バーンドア (水平・閉じる)"));
    QAction *bvAct  = wipeMenu->addAction(QStringLiteral("バーンドア (垂直・開く)"));
    QAction *bvcAct = wipeMenu->addAction(QStringLiteral("バーンドア (垂直・閉じる)"));
    QMenu *slideMenu = transitionMenu->addMenu(QStringLiteral("スライド (1.0s)"));
    QAction *slAct = slideMenu->addAction(QStringLiteral("左へ"));
    QAction *srAct = slideMenu->addAction(QStringLiteral("右へ"));
    QAction *suAct = slideMenu->addAction(QStringLiteral("上へ"));
    QAction *sdAct = slideMenu->addAction(QStringLiteral("下へ"));
    QMenu *pushMenu = transitionMenu->addMenu(QStringLiteral("プッシュ (1.0s)"));
    QAction *plAct = pushMenu->addAction(QStringLiteral("左へ"));
    QAction *prAct = pushMenu->addAction(QStringLiteral("右へ"));
    QAction *puAct = pushMenu->addAction(QStringLiteral("上へ"));
    QAction *pdAct = pushMenu->addAction(QStringLiteral("下へ"));
    QMenu *irisMenu = transitionMenu->addMenu(QStringLiteral("アイリス (1.0s)"));
    QAction *irAct  = irisMenu->addAction(QStringLiteral("円・開く"));
    QAction *ircAct = irisMenu->addAction(QStringLiteral("円・閉じる"));
    QAction *ibAct  = irisMenu->addAction(QStringLiteral("矩形・開く"));
    QAction *ibcAct = irisMenu->addAction(QStringLiteral("矩形・閉じる"));
    QAction *czAct = transitionMenu->addAction(QStringLiteral("クロスズーム (1.0s)"));
    QMenu *spinMenu = transitionMenu->addMenu(QStringLiteral("スピン (1.0s)"));
    QAction *scwAct  = spinMenu->addAction(QStringLiteral("時計回り"));
    QAction *sccwAct = spinMenu->addAction(QStringLiteral("反時計回り"));
    QMenu *whipMenu = transitionMenu->addMenu(QStringLiteral("ウィップパン (0.5s)"));
    QAction *wplAct = whipMenu->addAction(QStringLiteral("左へ"));
    QAction *wprAct = whipMenu->addAction(QStringLiteral("右へ"));
    QAction *glAct  = transitionMenu->addAction(QStringLiteral("グリッチ (0.5s)"));
    QAction *llAct  = transitionMenu->addAction(QStringLiteral("ライトリーク (1.0s)"));
    QAction *lfAct  = transitionMenu->addAction(QStringLiteral("レンズフレア (1.0s)"));
    QAction *fbAct  = transitionMenu->addAction(QStringLiteral("フィルムバーン (1.0s)"));
    QAction *skAct  = transitionMenu->addAction(QStringLiteral("カメラシェイク (0.5s)"));
    QAction *chAct  = transitionMenu->addAction(QStringLiteral("チャンネルシフト (0.5s)"));
    QMenu *flipMenu = transitionMenu->addMenu(QStringLiteral("フリップ (1.0s)"));
    QAction *fhAct  = flipMenu->addAction(QStringLiteral("水平 (Y軸回転)"));
    QAction *fvAct  = flipMenu->addAction(QStringLiteral("垂直 (X軸回転)"));
    transitionMenu->addSeparator();
    // User-saved presets — populated dynamically from QSettings via
    // TransitionPresetStore. Empty submenu when nothing is saved.
    QMenu *presetMenu = transitionMenu->addMenu(QStringLiteral("プリセット"));
    QList<QPair<QAction *, TransitionPreset>> presetActs;
    {
        const auto loaded = TransitionPresetStore::loadAll();
        if (loaded.isEmpty()) {
            QAction *empty = presetMenu->addAction(QStringLiteral("(プリセット未登録)"));
            empty->setEnabled(false);
        } else {
            for (const auto &p : loaded) {
                const QString label = QStringLiteral("%1  (%2, %3s)")
                    .arg(p.name)
                    .arg(Transition::typeName(p.transition.type))
                    .arg(p.transition.duration, 0, 'f', 1);
                QAction *act = presetMenu->addAction(label);
                presetActs.append({ act, p });
            }
        }
    }
    QAction *transDialogAct = transitionMenu->addAction(QStringLiteral("カスタム..."));
    QAction *transClearAct = nullptr;
    if (hasTransition) {
        transitionMenu->addSeparator();
        transClearAct = transitionMenu->addAction(QStringLiteral("トランジションを削除"));
    }

    QAction *fxAct = menu.addAction(QStringLiteral("ビデオエフェクト..."));
    QAction *ccAct = menu.addAction(QStringLiteral("色補正 / グレーディング..."));
    QAction *parentAct = menu.addAction(QStringLiteral("ペアレント..."));
    QAction *nullAct = menu.addAction(QStringLiteral("ヌルオブジェクトを作成"));
    menu.addSeparator();
    // SNS 縦動画フィット (相互排他の3択): 幅フィット=レターボックスで全表示 /
    // 幅埋め=中央クロップで枠を歪みなく充填 / 解除=既定 (IgnoreAspectRatio で
    // 枠に伸ばす)。fitContain と fitCover は engine 側 (snsfit::maybeFit) で
    // cover 優先の相互排他なので、ここでも片方を立てたら他方は必ず倒す。
    QAction *snsFitAct = menu.addAction(QStringLiteral("SNS: 幅フィット中央(全表示・レターボックス)"));
    QAction *snsCoverAct = menu.addAction(QStringLiteral("SNS: 幅埋め(クロップ・歪みなし)"));
    QAction *snsFillAct = menu.addAction(QStringLiteral("SNS: フィット解除(全画面)"));

    QAction *chosen = menu.exec(globalPos);
    if (!chosen) return;
    if (chosen == cutAct) cutSelectedClip();
    else if (chosen == copyAct) copySelectedClip();
    else if (chosen == deleteAct) deleteSelectedClip();
    else if (chosen == silenceCutAct) applySilenceCutToClip(track, clipIndex);
    else if (chosen == beatMarkerAct) applyBeatMarkersToClip(track, clipIndex);
    else if (chosen == unlinkAct) unlinkClipGroup(linkGroup);
    else if (chosen == relinkAct) relinkClipAt(track, clipIndex);
    else if (chosen == snsFitAct) applySnsFitToClip(track, clipIndex, true, false, QStringLiteral("SNS width fit center"));
    else if (chosen == snsCoverAct) applySnsFitToClip(track, clipIndex, false, true, QStringLiteral("SNS width fill crop"));
    else if (chosen == snsFillAct) applySnsFitToClip(track, clipIndex, false, false, QStringLiteral("SNS restore fullscreen"));
    else if (chosen == xdAct) {
        Transition t;
        t.type = TransitionType::CrossDissolve;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == fdAct) {
        Transition t;
        t.type = TransitionType::FilmDissolve;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == ddAct) {
        Transition t;
        t.type = TransitionType::DitherDissolve;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == blAct) {
        Transition t;
        t.type = TransitionType::BlurDissolve;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == pxAct) {
        Transition t;
        t.type = TransitionType::Pixelate;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == fiAct) {
        Transition t;
        t.type = TransitionType::FadeIn;
        t.duration = 0.5;
        applyTransitionToSelected(t);
    }
    else if (chosen == foAct) {
        Transition t;
        t.type = TransitionType::FadeOut;
        t.duration = 0.5;
        applyTransitionToSelected(t);
    }
    else if (chosen == dbAct) {
        Transition t;
        t.type = TransitionType::DipToBlack;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == dwAct) {
        Transition t;
        t.type = TransitionType::DipToWhite;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == wlAct || chosen == wrAct || chosen == wuAct
             || chosen == wdAct || chosen == cwAct || chosen == cwcAct
             || chosen == bhAct || chosen == bhcAct
             || chosen == bvAct || chosen == bvcAct
             || chosen == slAct || chosen == srAct
             || chosen == suAct || chosen == sdAct
             || chosen == plAct || chosen == prAct
             || chosen == puAct || chosen == pdAct
             || chosen == czAct || chosen == scwAct || chosen == sccwAct
             || chosen == wplAct || chosen == wprAct || chosen == glAct
             || chosen == llAct || chosen == fhAct || chosen == fvAct
             || chosen == lfAct || chosen == fbAct
             || chosen == skAct || chosen == chAct
             || chosen == irAct || chosen == ircAct
             || chosen == ibAct || chosen == ibcAct) {
        Transition t;
        // Whip pan, glitch, camera shake, channel shift are punchy/short
        // by NLE convention; everyone else gets the 1.0s default we've
        // been using for context-menu presets. Custom dialog can override
        // either way.
        t.duration = (chosen == wplAct || chosen == wprAct || chosen == glAct
                      || chosen == skAct || chosen == chAct)
            ? 0.5 : 1.0;
        if (chosen == wlAct)      t.type = TransitionType::WipeLeft;
        else if (chosen == wrAct) t.type = TransitionType::WipeRight;
        else if (chosen == wuAct) t.type = TransitionType::WipeUp;
        else if (chosen == wdAct) t.type = TransitionType::WipeDown;
        else if (chosen == cwAct)  t.type = TransitionType::ClockWipe;
        else if (chosen == cwcAct) t.type = TransitionType::ClockWipeCCW;
        else if (chosen == bhAct)  t.type = TransitionType::BarnDoorHorizontal;
        else if (chosen == bhcAct) t.type = TransitionType::BarnDoorHClose;
        else if (chosen == bvAct)  t.type = TransitionType::BarnDoorVertical;
        else if (chosen == bvcAct) t.type = TransitionType::BarnDoorVClose;
        else if (chosen == slAct) t.type = TransitionType::SlideLeft;
        else if (chosen == srAct) t.type = TransitionType::SlideRight;
        else if (chosen == suAct) t.type = TransitionType::SlideUp;
        else if (chosen == sdAct) t.type = TransitionType::SlideDown;
        else if (chosen == plAct) t.type = TransitionType::PushLeft;
        else if (chosen == prAct) t.type = TransitionType::PushRight;
        else if (chosen == puAct) t.type = TransitionType::PushUp;
        else if (chosen == pdAct) t.type = TransitionType::PushDown;
        else if (chosen == czAct) t.type = TransitionType::CrossZoom;
        else if (chosen == scwAct)  t.type = TransitionType::SpinCW;
        else if (chosen == sccwAct) t.type = TransitionType::SpinCCW;
        else if (chosen == wplAct)  t.type = TransitionType::WhipPanLeft;
        else if (chosen == wprAct)  t.type = TransitionType::WhipPanRight;
        else if (chosen == glAct)   t.type = TransitionType::Glitch;
        else if (chosen == llAct)   t.type = TransitionType::LightLeak;
        else if (chosen == lfAct)   t.type = TransitionType::LensFlare;
        else if (chosen == fbAct)   t.type = TransitionType::FilmBurn;
        else if (chosen == skAct)   t.type = TransitionType::CameraShake;
        else if (chosen == chAct)   t.type = TransitionType::ColorChannelShift;
        else if (chosen == fhAct)   t.type = TransitionType::FlipHorizontal;
        else if (chosen == fvAct)   t.type = TransitionType::FlipVertical;
        else if (chosen == irAct)  t.type = TransitionType::IrisRound;
        else if (chosen == ircAct) t.type = TransitionType::IrisRoundClose;
        else if (chosen == ibAct)  t.type = TransitionType::IrisBox;
        else                       t.type = TransitionType::IrisBoxClose;
        applyTransitionToSelected(t);
    }
    else if (chosen == transDialogAct) emit transitionDialogRequested();
    else if (transClearAct && chosen == transClearAct) clearTransitionsOnSelected();
    else if (chosen == fxAct) emit videoEffectsDialogRequested();
    else if (chosen == ccAct) emit colorCorrectionRequested();
    else if (chosen == parentAct) emit clipParentDialogRequested();
    else if (chosen == nullAct) emit nullObjectRequested();
    else {
        // Preset dispatch — chosen action might be one of the dynamically
        // populated preset entries. Match by pointer since the labels
        // include the type/duration suffix and could collide with user
        // names that happen to match.
        for (const auto &pair : presetActs) {
            if (chosen != pair.first) continue;
            applyTransitionToSelected(pair.second.transition);
            break;
        }
    }
}

void Timeline::undo()
{
    undotrace::log("undo:enter");
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

qint64 Timeline::findSnapTarget(qint64 candidateUs, int *outScreenX) const
{
    const double candidateSec = static_cast<double>(candidateUs) / 1e6;
    const int candidateX = static_cast<int>(candidateSec * m_zoomLevel);

    struct Target {
        int x;
        int priority; // 1=clip-edge, 2=playhead, 4=t=0
        int distance;
    };
    QVector<Target> targets;

    auto collectClipEdges = [&](const QVector<TimelineTrack*> &tracks) {
        for (const auto *track : tracks) {
            if (!track) continue;
            int x = 0;
            const double pps = track->pixelsPerSecond();
            for (int i = 0; i < track->clipCount(); ++i) {
                const auto &clip = track->clips()[i];
                x += qMax(0, static_cast<int>(clip.leadInSec * pps));
                targets.append({x, 1, qAbs(candidateX - x)});
                const int cw = qMax(20, static_cast<int>(clip.effectiveDuration() * pps));
                targets.append({x + cw, 1, qAbs(candidateX - (x + cw))});
                x += cw;
            }
        }
    };
    collectClipEdges(m_videoTracks);
    collectClipEdges(m_audioTracks);

    // Playhead
    const int playheadX = static_cast<int>(m_playheadPos * m_zoomLevel);
    targets.append({playheadX, 2, qAbs(candidateX - playheadX)});

    // Time origin (t=0)
    targets.append({0, 4, qAbs(candidateX - 0)});

    // Find best: closest by pixel distance; on tie lower priority wins.
    Target best{-1, 999, INT_MAX};
    for (const auto &t : targets) {
        if (t.distance > TimelineTrack::SNAP_THRESHOLD) continue;
        if (t.distance < best.distance
            || (t.distance == best.distance && t.priority < best.priority)) {
            best = t;
        }
    }

    if (best.x >= 0) {
        *outScreenX = best.x;
        const double snappedSec = static_cast<double>(best.x) / m_zoomLevel;
        return static_cast<qint64>(snappedSec * 1e6);
    }
    *outScreenX = candidateX;
    return candidateUs;
}

void Timeline::triggerSnapLine(int screenX)
{
    m_snapLineX = screenX;
    m_snapLineFadeTimer.restart();
    // Repaint all tracks so the line appears on every row.
    for (auto *t : m_videoTracks)
        if (t) t->update();
    for (auto *t : m_audioTracks)
        if (t) t->update();
    // Schedule a clean-up repaint after the fade window.
    QTimer::singleShot(kSnapLineFadeMs, this, [this] {
        m_snapLineX = -1;
        for (auto *t : m_videoTracks) if (t) t->update();
        for (auto *t : m_audioTracks) if (t) t->update();
    });
}

int Timeline::snapLineX() const
{
    if (m_snapLineX < 0) return -1;
    if (m_snapLineFadeTimer.elapsed() >= kSnapLineFadeMs) return -1;
    return m_snapLineX;
}

int Timeline::snapLineAlpha() const
{
    if (m_snapLineX < 0) return 0;
    const qint64 elapsed = m_snapLineFadeTimer.elapsed();
    if (elapsed >= kSnapLineFadeMs) return 0;
    return 255 - static_cast<int>(255 * elapsed / kSnapLineFadeMs);
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
    if (m_markerLane)
        m_markerLane->setPixelsPerSecond(m_zoomLevel);

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

// Per-clip speed ramp
static const speedramp::SpeedRamp kIdentityRamp = speedramp::SpeedRamp::identity();

const speedramp::SpeedRamp &Timeline::speedRamp(int clipIndex) const
{
    if (clipIndex < 0 || !m_videoTrack) return kIdentityRamp;
    const auto &clips = m_videoTrack->clips();
    if (clipIndex >= clips.size()) return kIdentityRamp;
    return clips[clipIndex].speedRamp;
}

void Timeline::setSpeedRamp(int clipIndex, const speedramp::SpeedRamp &ramp)
{
    if (clipIndex < 0 || !m_videoTrack) return;
    auto clips = m_videoTrack->clips();
    if (clipIndex >= clips.size()) return;
    clips[clipIndex].speedRamp = ramp;
    m_videoTrack->setClips(clips);
    saveUndoState(QString("Set speed ramp clip %1").arg(clipIndex));
    scheduleEmitSequenceChanged();
}

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
    // Re-emit so AudioMixer picks up the new per-clip volume for the
    // matching PlaybackEntry on its next setSequence call.
    scheduleEmitSequenceChanged();
}

void Timeline::setClipPan(double pan)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    pan = qMax(-1.0, qMin(1.0, pan));
    auto audioClips = m_audioTrack->clips();
    if (sel < audioClips.size()) {
        audioClips[sel].pan = pan;
        m_audioTrack->setClips(audioClips);
    }
    saveUndoState(QString("Set pan %1").arg(pan, 0, 'f', 2));
    scheduleEmitSequenceChanged();
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

void Timeline::setClipLayerStyle(const LayerStyle &style)
{
    int sel = m_videoTrack->selectedClip();
    setClipLayerStyle(0, sel, style);
}

void Timeline::setClipLayerStyle(int trackIdx, int clipIdx, const LayerStyle &style)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return;
    auto *track = m_videoTracks[trackIdx];
    if (!track)
        return;

    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return;

    clips[clipIdx].layerStyle = style;
    track->setClips(clips);
    saveUndoState("Layer style");
    scheduleEmitSequenceChanged();
}

void Timeline::applyTransitionToSelected(const Transition &t)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    if (sel >= clips.size()) return;
    // Boundary transitions apply to BOTH sides of the cut so the user
    // doesn't need two manual operations and the badge appears on both
    // adjacent clips. Pairing rules:
    //   FadeIn  → this.leadIn = FadeIn; prev.trailOut = FadeOut (mirror)
    //   FadeOut → this.trailOut = FadeOut; next.leadIn = FadeIn (mirror)
    //   CrossDissolve / Wipe / Slide → both this.trailOut and next.leadIn
    //                                  set to the SAME type
    // Sequence-edge cases (no neighbour on the paired side) fall back to
    // single-side application — that's how a clip "fades in from black"
    // at the start of the timeline still works.
    if (t.type == TransitionType::FadeIn) {
        clips[sel].leadIn = t;
        if (sel > 0) {
            Transition mirror;
            mirror.type = TransitionType::FadeOut;
            mirror.duration = t.duration;
            mirror.easing = t.easing;
            clips[sel - 1].trailOut = mirror;
        }
    } else if (t.type == TransitionType::FadeOut) {
        clips[sel].trailOut = t;
        if (sel + 1 < clips.size()) {
            Transition mirror;
            mirror.type = TransitionType::FadeIn;
            mirror.duration = t.duration;
            mirror.easing = t.easing;
            clips[sel + 1].leadIn = mirror;
        }
    } else {
        clips[sel].trailOut = t;
        if (sel + 1 < clips.size())
            clips[sel + 1].leadIn = t;
    }
    m_videoTrack->setClips(clips);

    // Mirror to the linked audio track at the same index so AudioMixer
    // applies the matching equal-power crossfade automatically. Pro NLEs
    // (Premiere/DaVinci/FCP X) all default to linked V+A transitions —
    // setting one without the other always sounds wrong (audio just
    // jump-cuts under a video crossfade). Users who want video-only
    // transitions can break the link with the unlink command first.
    if (m_audioTrack) {
        auto aClips = m_audioTrack->clips();
        if (sel < aClips.size()) {
            if (t.type == TransitionType::FadeIn) {
                aClips[sel].leadIn = t;
                if (sel > 0) {
                    Transition mirror;
                    mirror.type = TransitionType::FadeOut;
                    mirror.duration = t.duration;
                    mirror.easing = t.easing;
                    aClips[sel - 1].trailOut = mirror;
                }
            } else if (t.type == TransitionType::FadeOut) {
                aClips[sel].trailOut = t;
                if (sel + 1 < aClips.size()) {
                    Transition mirror;
                    mirror.type = TransitionType::FadeIn;
                    mirror.duration = t.duration;
                    mirror.easing = t.easing;
                    aClips[sel + 1].leadIn = mirror;
                }
            } else {
                aClips[sel].trailOut = t;
                if (sel + 1 < aClips.size())
                    aClips[sel + 1].leadIn = t;
            }
            m_audioTrack->setClips(aClips);
        }
    }

    saveUndoState(QString("Add transition: %1").arg(Transition::typeName(t.type)));
    scheduleEmitSequenceChanged();

    // Handle-shortage advisory. Pure overlap transitions (CrossDissolve /
    // Wipe / Slide / Iris / Dip / Barn / ClockWipe) need source media past
    // the selected clip's outPoint and / or before the next clip's inPoint
    // depending on alignment. We mirror computePlaybackSequence's math
    // here so the message matches what the user will actually see.
    if (isOverlapTransition(t.type) && sel + 1 < clips.size()) {
        const auto &a = clips[sel];
        const auto &b = clips[sel + 1];
        const double aSpeed = (a.speed > 0.0) ? a.speed : 1.0;
        const double bSpeed = (b.speed > 0.0) ? b.speed : 1.0;
        const double aOutPoint = (a.outPoint > 0.0) ? a.outPoint : a.duration;
        const double aTrail = qMax(0.0, (a.duration - aOutPoint) / aSpeed);
        const double bLead  = qMax(0.0, b.inPoint / bSpeed);
        const double askedD = t.duration;
        double aExtend = 0.0, bRetract = 0.0;
        switch (t.alignment) {
            case TransitionAlignment::Start:
                aExtend  = qMin(askedD, aTrail);
                break;
            case TransitionAlignment::End:
                bRetract = qMin(askedD, bLead);
                break;
            case TransitionAlignment::Center:
                aExtend  = qMin(askedD * 0.5, aTrail);
                bRetract = qMin(askedD * 0.5, bLead);
                if (aExtend + bRetract < askedD) {
                    if (bRetract < askedD * 0.5) {
                        aExtend += qMin(askedD - aExtend - bRetract,
                                        aTrail - aExtend);
                    }
                    if (aExtend + bRetract < askedD && aExtend < askedD * 0.5) {
                        bRetract += qMin(askedD - aExtend - bRetract,
                                         bLead - bRetract);
                    }
                }
                break;
        }
        const double effectiveD = aExtend + bRetract;
        if (effectiveD + 0.005 < askedD) {
            emit transitionShortened(Transition::typeName(t.type),
                                     askedD, effectiveD);
        }
    }
}

void Timeline::clearTransitionsOnSelected()
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    if (sel >= clips.size()) return;
    const bool hadLead = clips[sel].leadIn.type != TransitionType::None;
    const bool hadTrail = clips[sel].trailOut.type != TransitionType::None;
    if (!hadLead && !hadTrail) return;
    // Mirror teardown — applyTransitionToSelected pairs leadIn/trailOut
    // across adjacent clips (FadeIn pairs with prev.trailOut=FadeOut,
    // FadeOut with next.leadIn=FadeIn, CrossDissolve/Wipe/Slide with the
    // same type on the other side). Clearing only this clip's slots
    // would leave the neighbour with a half-paired transition that fires
    // an audio fade or visual blend with nothing to blend against.
    if (hadLead && sel > 0
        && clips[sel - 1].trailOut.type != TransitionType::None) {
        clips[sel - 1].trailOut = Transition{};
    }
    if (hadTrail && sel + 1 < clips.size()
        && clips[sel + 1].leadIn.type != TransitionType::None) {
        clips[sel + 1].leadIn = Transition{};
    }
    clips[sel].leadIn = Transition{};
    clips[sel].trailOut = Transition{};
    m_videoTrack->setClips(clips);

    // Mirror teardown on the linked audio track. AudioMixer reads its
    // crossfade window from PlaybackEntry.leadIn/trailOut which is sourced
    // from the audio clips, so leaving them set here would keep the audio
    // fading even after the visual transition is gone.
    if (m_audioTrack) {
        auto aClips = m_audioTrack->clips();
        if (sel < aClips.size()) {
            if (hadLead && sel > 0
                && aClips[sel - 1].trailOut.type != TransitionType::None) {
                aClips[sel - 1].trailOut = Transition{};
            }
            if (hadTrail && sel + 1 < aClips.size()
                && aClips[sel + 1].leadIn.type != TransitionType::None) {
                aClips[sel + 1].leadIn = Transition{};
            }
            aClips[sel].leadIn = Transition{};
            aClips[sel].trailOut = Transition{};
            m_audioTrack->setClips(aClips);
        }
    }

    saveUndoState("Clear transitions");
    scheduleEmitSequenceChanged();
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

LayerStyle Timeline::clipLayerStyle() const
{
    int sel = m_videoTrack->selectedClip();
    return clipLayerStyle(0, sel);
}

LayerStyle Timeline::clipLayerStyle(int trackIdx, int clipIdx) const
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return {};
    const auto *track = m_videoTracks[trackIdx];
    if (!track)
        return {};
    const auto &clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return {};
    return clips[clipIdx].layerStyle;
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

int Timeline::selectedVideoClipIndex() const
{
    if (!m_videoTrack) return -1;
    return m_videoTrack->selectedClip();
}

void Timeline::insertClip3PointActive(double timelineStartSec, const ClipInfo &clip)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return;
    auto *track = m_videoTracks.first();
    track->insertClip3Point(timelineStartSec, clip);
    saveUndoState("3点編集: インサート");
}

void Timeline::overwriteClip3PointActive(double timelineStartSec, const ClipInfo &clip)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return;
    auto *track = m_videoTracks.first();
    track->overwriteClip3Point(timelineStartSec, clip);
    saveUndoState("3点編集: 上書き");
}

void Timeline::rippleDeleteTimeRangeActive(double startSec, double endSec)
{
    if (m_videoTracks.isEmpty())
        return;
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    // アクティブ動画トラック (無ければ先頭 V1) を選ぶ。
    TimelineTrack *track = nullptr;
    if (m_activeVideoTrackIndex >= 0
        && m_activeVideoTrackIndex < m_videoTracks.size())
        track = m_videoTracks[m_activeVideoTrackIndex];
    if (!track)
        track = m_videoTracks.first();
    if (!track)
        return;
    const bool changed = track->rippleDeleteTimeRange(startSec, endSec);
    // no-op だった場合 (範囲外/空) は Undo スナップショットを積まない。
    if (changed) {
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
        saveUndoState("リップル削除 (範囲)");
    }
}

bool Timeline::applyTrimActive(trimops::TrimType type, double deltaSec,
                               QString *errorOut)
{
    // アクティブ動画トラックの現在選択中クリップへトリムを適用する。
    // Roll は「選択クリップ = 編集点の左側クリップ」として扱うので、trimops::
    // applyTrim が clip[sel]/clip[sel+1] の編集点を動かす。選択が無ければ失敗。
    if (m_videoTracks.isEmpty()) {
        if (errorOut) *errorOut = QObject::tr("動画トラックがありません");
        return false;
    }
    TimelineTrack *track = nullptr;
    if (m_activeVideoTrackIndex >= 0
        && m_activeVideoTrackIndex < m_videoTracks.size()) {
        auto *candidate = m_videoTracks[m_activeVideoTrackIndex];
        const int candidateSel = candidate ? candidate->selectedClip() : -1;
        if (candidate && candidateSel >= 0 && candidateSel < candidate->clipCount())
            track = candidate;
    }
    if (!track) {
        for (int i = 0; i < m_videoTracks.size(); ++i) {
            auto *candidate = m_videoTracks[i];
            const int candidateSel = candidate ? candidate->selectedClip() : -1;
            if (candidate && candidateSel >= 0 && candidateSel < candidate->clipCount()) {
                track = candidate;
                m_activeVideoTrackIndex = i;
                break;
            }
        }
    }
    if (!track) {
        if (errorOut) *errorOut = QObject::tr("トリム対象のクリップが選択されていません");
        return false;
    }
    const int sel = track->selectedClip();
    if (sel < 0 || sel >= track->clipCount()) {
        if (errorOut) *errorOut = QObject::tr("トリム対象のクリップが選択されていません");
        return false;
    }
    if (!track->applyTrim(sel, type, deltaSec, errorOut))
        return false;
    saveUndoState("トリム");
    return true;
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

void Timeline::insertAudioClipAtPlayhead(const QString &wavPath, int trackIdx)
{
    // Probe duration via FFmpeg
    AVFormatContext *fmt = nullptr;
    double duration = 0.0;
    if (avformat_open_input(&fmt, wavPath.toUtf8().constData(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0)
            duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        avformat_close_input(&fmt);
    }

    ClipInfo clip;
    clip.filePath = wavPath;
    clip.displayName = QFileInfo(wavPath).fileName();
    clip.duration = duration;
    clip.linkGroup = allocateLinkGroup();

    // Determine target audio track: trackIdx is 1-based (2 = A2).
    // If the requested track doesn't exist, create it.
    int targetIndex = trackIdx - 1; // convert to 0-based
    if (targetIndex < 0 || targetIndex >= m_audioTracks.size()) {
        // Create tracks up to the requested one
        while (m_audioTracks.size() <= targetIndex) {
            addAudioTrack();
        }
    }

    TimelineTrack *target = m_audioTracks[targetIndex];
    if (!target) return;

    // Insert at the playhead position: find the clip index where playhead falls
    double playheadSec = m_playheadPos;
    double accumulatedTime = 0.0;
    int insertIdx = 0;
    const auto &clips = target->clips();
    for (int i = 0; i < clips.size(); ++i) {
        double clipEnd = accumulatedTime + clips[i].leadInSec + clips[i].effectiveDuration();
        if (playheadSec < clipEnd) {
            // Playhead is within or before this clip
            if (playheadSec < accumulatedTime + clips[i].leadInSec) {
                // In the lead-in gap before this clip
                insertIdx = i;
            } else {
                // Inside the clip — insert after it
                insertIdx = i + 1;
            }
            break;
        }
        accumulatedTime = clipEnd;
        insertIdx = i + 1;
    }

    // Set leadInSec so the clip starts at the playhead position
    clip.leadInSec = playheadSec - accumulatedTime;
    if (clip.leadInSec < 0.0) clip.leadInSec = 0.0;

    target->insertClip(insertIdx, clip);
    saveUndoState("Insert voice-over");
    updateInfoLabel();
    scheduleEmitSequenceChanged();
}

void Timeline::toggleMuteTrack(int audioTrackIndex)
{
    if (audioTrackIndex < 0 || audioTrackIndex >= m_audioTracks.size()) return;
    auto *track = m_audioTracks[audioTrackIndex];
    track->setMuted(!track->isMuted());
    // Re-emit so AudioMixer picks up the new audioMuted flag for every
    // entry on this track. Without this the mute toggle stayed silent
    // until the next clip edit triggered a sequence rebuild.
    scheduleEmitSequenceChanged();
    updateInfoLabel();
}

void Timeline::toggleSoloTrack(int audioTrackIndex)
{
    if (audioTrackIndex < 0 || audioTrackIndex >= m_audioTracks.size()) return;
    bool newSolo = !m_audioTracks[audioTrackIndex]->isSolo();
    // Clear all solo first, then set the target
    for (int i = 0; i < m_audioTracks.size(); ++i) {
        const bool wasSolo = m_audioTracks[i]->isSolo();
        const bool nowSolo = (i == audioTrackIndex && newSolo);
        if (wasSolo != nowSolo) {
            m_audioTracks[i]->setSolo(nowSolo);
            emit trackSoloChanged(i, nowSolo);
        }
    }
    // Solo state lives on the mixer (per-track effectiveGain), so the
    // PlaybackEntry-shaped schedule is unchanged — no audioSequenceChanged
    // re-emit needed. trackSoloChanged carries the only state the mixer
    // cares about and the track header UI updates from setSolo directly.
    updateInfoLabel();
}

void Timeline::normalizeAudioClipPeak(int trackIdx, int clipIdx)
{
    if (trackIdx < 0 || trackIdx >= m_audioTracks.size()) return;
    TimelineTrack *track = m_audioTracks[trackIdx];
    if (!track || clipIdx < 0 || clipIdx >= track->clips().size()) return;

    const ClipInfo clipSrc = track->clips()[clipIdx];
    ClipInfo clip = clipSrc;
    const QString filePath = clip.filePath;
    if (filePath.isEmpty()) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: ファイルを読めません."), 5000);
        return;
    }

    NormalizeDecoderCtx dec;
    if (!dec.open(filePath)) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: ファイルを読めません."), 5000);
        return;
    }

    const double fileInSec = clip.inPoint;
    const double fileOutSec = (clip.outPoint > 0.0) ? clip.outPoint : clip.duration;
    const double searchLenSec = fileOutSec - fileInSec;
    if (searchLenSec <= 0.0) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: 信号が検出されませんでした."), 5000);
        return;
    }

    const int channels = dec.codec->ch_layout.nb_channels;
    const int sampleRate = dec.codec->sample_rate;
    const int stream = dec.streamIdx();
    const double timebase = av_q2d(dec.fmt->streams[stream]->time_base);

    // Seek to in-point
    const int64_t seekTs = static_cast<int64_t>(fileInSec / timebase);
    av_seek_frame(dec.fmt, stream, seekTs, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec.codec);

    // Progress dialog for clips > 60 seconds (existing pattern from MainWindow)
    QProgressDialog *progress = nullptr;
    bool cancelled = false;
    if (searchLenSec > 60.0) {
        progress = new QProgressDialog(QStringLiteral("ノーマライズ中..."),
                                        QStringLiteral("キャンセル"), 0, 100, this);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(0);
        progress->setValue(0);
    }

    double maxSampleAbs = 0.0;
    int64_t lastPts = AV_NOPTS_VALUE;

    while (av_read_frame(dec.fmt, dec.pkt) >= 0) {
        if (dec.pkt->stream_index != stream) {
            av_packet_unref(dec.pkt);
            continue;
        }

        if (progress && progress->wasCanceled()) {
            cancelled = true;
            av_packet_unref(dec.pkt);
            break;
        }

        if (avcodec_send_packet(dec.codec, dec.pkt) < 0) {
            av_packet_unref(dec.pkt);
            continue;
        }

        while (avcodec_receive_frame(dec.codec, dec.frame) >= 0) {
            if (dec.frame->pts != AV_NOPTS_VALUE)
                lastPts = dec.frame->pts;
            if (lastPts == AV_NOPTS_VALUE) {
                av_frame_unref(dec.frame);
                continue;
            }

            const double frameTimeSec = static_cast<double>(lastPts) * timebase;
            const double frameEndSec =
                frameTimeSec + static_cast<double>(dec.frame->nb_samples) / sampleRate;

            // Entire frame before or after the clip window → skip
            if (frameEndSec <= fileInSec || frameTimeSec >= fileOutSec) {
                av_frame_unref(dec.frame);
                continue;
            }

            // Clip the sample range to the clip window
            int sStart = 0;
            int sEnd = dec.frame->nb_samples;
            if (frameTimeSec < fileInSec) {
                sStart = static_cast<int>((fileInSec - frameTimeSec) * sampleRate);
                if (sStart > dec.frame->nb_samples) sStart = dec.frame->nb_samples;
            }
            if (frameEndSec > fileOutSec) {
                sEnd = dec.frame->nb_samples
                     - static_cast<int>((frameEndSec - fileOutSec) * sampleRate);
                if (sEnd < sStart) sEnd = sStart;
            }

            // Walk every channel's samples and track max |sample|
            const auto sampleFmt = dec.codec->sample_fmt;
            const bool planar = av_sample_fmt_is_planar(sampleFmt) != 0;
            for (int ch = 0; ch < channels; ++ch) {
                for (int s = sStart; s < sEnd; ++s) {
                    const int idx = planar ? s : (s * channels + ch);
                    const uint8_t *buf = planar ? dec.frame->data[ch]
                                                : dec.frame->data[0];
                    if (!buf) break;
                    double val = 0.0;
                    switch (sampleFmt) {
                    case AV_SAMPLE_FMT_S16:
                    case AV_SAMPLE_FMT_S16P:
                        val = std::abs(static_cast<double>(
                            reinterpret_cast<const int16_t *>(buf)[idx]));
                        break;
                    case AV_SAMPLE_FMT_S32:
                    case AV_SAMPLE_FMT_S32P:
                        val = std::abs(static_cast<double>(
                            reinterpret_cast<const int32_t *>(buf)[idx]))
                            / 2147483648.0 * 32768.0;
                        break;
                    case AV_SAMPLE_FMT_FLT:
                    case AV_SAMPLE_FMT_FLTP:
                        val = std::abs(static_cast<double>(
                            reinterpret_cast<const float *>(buf)[idx]))
                            * 32768.0;
                        break;
                    case AV_SAMPLE_FMT_DBL:
                    case AV_SAMPLE_FMT_DBLP:
                        val = std::abs(
                            reinterpret_cast<const double *>(buf)[idx])
                            * 32768.0;
                        break;
                    default:
                        break;
                    }
                    if (val > maxSampleAbs) maxSampleAbs = val;
                }
            }

            av_frame_unref(dec.frame);

            if (progress && !cancelled) {
                const double pct = (frameTimeSec - fileInSec) / searchLenSec * 100.0;
                progress->setValue(static_cast<int>(qBound(0.0, pct, 100.0)));
                QCoreApplication::processEvents();
            }
        }
        av_packet_unref(dec.pkt);
    }

    if (progress) {
        progress->close();
        delete progress;
    }

    if (cancelled) {
        emit statusMessageRequested(QStringLiteral("ノーマライズがキャンセルされました."), 3000);
        return;
    }

    // Result: peak analysis
    if (maxSampleAbs < 1e-12) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: 信号が検出されませんでした."), 5000);
        return;
    }

    const double peakDb = 20.0 * std::log10(maxSampleAbs / 32768.0);
    if (peakDb < -90.0) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: 信号が検出されませんでした."), 5000);
        return;
    }

    double gainDb = -1.0 - peakDb;  // target peak = -1 dBFS
    gainDb = qBound(-24.0, gainDb, 12.0);  // clamp gain to [-24, +12]

    const double oldVol = clip.volume;
    const double gainLinear = std::pow(10.0, gainDb / 20.0);
    const double newVol = qBound(0.0, oldVol * gainLinear, 4.0);

    // Snapshot pre-mutation so Ctrl+Z restores the original volume.
    // Mirrors MainWindow::onMeterRequestNormalize ordering (saveState then
    // mutate). Must run before track->setClips below.
    saveUndoState(QStringLiteral("Normalize audio clip"));

    clip.volume = newVol;
    auto clips = track->clips();
    clips[clipIdx] = clip;
    track->setClips(clips);

    scheduleEmitSequenceChanged();

    const QString msg =
        QStringLiteral("A%1 クリップ %2 ノーマライズ: peak %3dB → gain %4→%5")
            .arg(trackIdx + 1)
            .arg(clipIdx + 1)
            .arg(peakDb, 0, 'f', 1)
            .arg(oldVol, 0, 'f', 2)
            .arg(clip.volume, 0, 'f', 2);
    emit statusMessageRequested(msg, 5000);
}

void Timeline::setPlayheadPosition(double seconds)
{
    m_playheadPos = qMax(0.0, seconds);
    syncPlayheadOverlay();
    // Chase mode: when playback or an external seek pushes the playhead
    // out of the central 50% comfort band, scroll the timeline so the
    // playhead lands back in the middle. Skipped while the user is
    // actively dragging — resolvePlayheadDragX handles that case with a
    // looser pin so a plain click in the outer band still lands the bar
    // exactly where the user clicked.
    if (!m_playheadDragging) ensurePlayheadVisible();
}

void Timeline::ensurePlayheadVisible()
{
    if (!m_scrollArea || !m_scrollArea->viewport() || !m_videoTrack) return;
    QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
    if (!hbar) return;
    const int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 0) return;
    const int playheadX = m_videoTrack->secondsToX(m_playheadPos);
    const int viewportX = playheadX - hbar->value();
    const int leftZone  = static_cast<int>(viewportW * 0.25);
    const int rightZone = static_cast<int>(viewportW * 0.75);
    if (viewportX >= leftZone && viewportX <= rightZone) return;
    // Re-centre at the 50% mark so the user has equal forward / backward
    // headroom after the chase scroll. Clamp so we never push past the
    // scroll rails (start of timeline at min, last-clip-end + trailing
    // pad at max).
    const int target = viewportW / 2;
    const int newScroll = qBound(hbar->minimum(),
                                 playheadX - target,
                                 hbar->maximum());
    if (newScroll != hbar->value()) hbar->setValue(newScroll);
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
    m_activeVideoTrackIndex = index < 0 ? -1 : 0;
    emit clipSelected(index);
    // V3 sprint — track-aware overload. m_videoTrack is m_videoTracks[0]
    // (V1) by construction, so this legacy single-track entry point always
    // refers to V1.
    emit clipSelectedOnTrack(index < 0 ? -1 : 0, index);
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
        // V3 sprint — track-aware overload. Resolve which video-track row
        // the click landed on; audio-track clicks emit trackIdx=-1 so the
        // edit target falls back to follow-active.
        int videoTrackIdx = m_videoTracks.indexOf(track);
        m_activeVideoTrackIndex = (index >= 0 && videoTrackIdx >= 0) ? videoTrackIdx : -1;
        emit clipSelectedOnTrack(index < 0 ? -1 : videoTrackIdx, index);
    });
    connect(track, &TimelineTrack::emptyAreaClicked, this, [this]() {
        clearAllSelections();
    });
    connect(track, &TimelineTrack::modified, this, &Timeline::onTrackModified);
    connect(track, &TimelineTrack::interactionCompleted, this,
        [this](const QString &description) {
            // Discrete user interaction finished — push an undo entry so
            // Ctrl+Z reverts THIS interaction. Without this, drag/trim/
            // transition-handle/envelope edits leave no undo record and a
            // later Ctrl+Z (e.g. from "Add transition") jumps past them
            // back to the prior explicit-action snapshot, which looks like
            // unrelated clips disappearing.
            saveUndoState(description);
        });
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
    // RM-5: remap Timeline's carrier when clips are reordered within a track.
    // clipMoved supplies (fromIndex, toIndex) after the mutation. Because
    // moveClip is a pure permutation we can derive the exact old→new index map
    // by direct arithmetic — no pre-mutation snapshot is needed:
    //   clip at `from` → `to`
    //   clips in [from+1..to] (if from<to) shift to [from..to-1]  (down by 1)
    //   clips in [to..from-1] (if from>to) shift to [to+1..from]  (up by 1)
    connect(track, &TimelineTrack::clipMoved, this,
        [this, track](int fromIdx, int toIdx) {
            if (m_trackMatteEntries.isEmpty() && m_clipParentEntries.isEmpty()) return;
            const int t = m_videoTracks.indexOf(track);
            if (t < 0) return;   // audio track — no video clip sidecars
            // Build old→new index map for track t only.
            QHash<QString, QString> oldToNew;
            oldToNew.insert(trackMatteClipKey(t, fromIdx),
                            trackMatteClipKey(t, toIdx));
            if (fromIdx < toIdx) {
                for (int i = fromIdx + 1; i <= toIdx; ++i)
                    oldToNew.insert(trackMatteClipKey(t, i),
                                    trackMatteClipKey(t, i - 1));
            } else {
                for (int i = toIdx; i < fromIdx; ++i)
                    oldToNew.insert(trackMatteClipKey(t, i),
                                    trackMatteClipKey(t, i + 1));
            }
            if (!m_trackMatteEntries.isEmpty()) {
                QHash<QString, TimelineTrackMatteEntry> rebuilt;
                rebuilt.reserve(m_trackMatteEntries.size());
                for (auto it = m_trackMatteEntries.cbegin();
                     it != m_trackMatteEntries.cend(); ++it) {
                    const QString newKey = oldToNew.value(it.key(), it.key());
                    TimelineTrackMatteEntry e = it.value();
                    e.matteSourceClipId = oldToNew.value(
                        e.matteSourceClipId, e.matteSourceClipId);
                    rebuilt.insert(newKey, e);
                }
                m_trackMatteEntries = rebuilt;
            }
            if (!m_clipParentEntries.isEmpty()) {
                QHash<QString, QString> rebuilt;
                rebuilt.reserve(m_clipParentEntries.size());
                for (auto it = m_clipParentEntries.cbegin();
                     it != m_clipParentEntries.cend(); ++it) {
                    const QString newChild = oldToNew.value(it.key(), it.key());
                    const QString newParent = oldToNew.value(it.value(), it.value());
                    if (newChild != newParent)
                        rebuilt.insert(newChild, newParent);
                }
                m_clipParentEntries = rebuilt;
            }
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
    // RM-5: snapshot before the cross-track remove+insert changes clip indices.
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);

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
            // RM-5: remap carrier after cross-track move changed clip indices.
            remapTimelineCarrierAfterMutation(this, m_trackMatteEntries, snapBefore);
            remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
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
    if (changed) {
        m_activeVideoTrackIndex = -1;
        emit clipSelected(-1);
        // V3 sprint — track-aware overload, deselect path.
        emit clipSelectedOnTrack(-1, -1);
    }
}

void Timeline::onTrackModified()
{
    // Any clip add/remove/move/trim/split bubbles up here. Auto-fit the zoom
    // first so the timeline widget never exceeds the viewport width (otherwise
    // long-form sequences hit the kMaxWidth hard cap and tail clips get
    // visually clipped). Then recompute the flat playback schedule.
    ensureSequenceFitsViewport();
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
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
                    // m_magnetArea sits above the V/A header rows and pairs
                    // with the right side's [time-ruler 22 + playhead-overlay
                    // 15] strip. Its height is locked to 37 px so V1's
                    // header stays vertically aligned with the V1 track —
                    // resizing it with m_trackHeight is what made the lock
                    // and mute icons drift away from their tracks.
                    if (hw == m_magnetArea)
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
        double rotation2DDegrees = 0.0;
        double opacity = 1.0;
        bool fitContain = false;
        bool fitCover = false;
        clipcolor::ColorMeta colorMeta;
        double volume = 1.0;
        double pan = 0.0;
        QVector<AudioGainPoint> volumeEnvelope;
        int clipIdx = -1;
        TransitionType leadInType = TransitionType::None;
        double leadInDuration = 0.0;
        TransitionAlignment leadInAlignment = TransitionAlignment::Center;
        TransitionEasing leadInEasing = TransitionEasing::Linear;
        TransitionType trailOutType = TransitionType::None;
        double trailOutDuration = 0.0;
        TransitionAlignment trailOutAlignment = TransitionAlignment::Center;
        TransitionEasing trailOutEasing = TransitionEasing::Linear;
        QVector<StabilizerKeyframe> stabilizerKeyframes;
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
                iv.rotation2DDegrees = c.rotation2DDegrees;
                iv.opacity = c.opacity;
                iv.fitContain = c.fitContain;
                iv.fitCover = c.fitCover;
                iv.colorMeta = c.colorMeta;
                iv.volume = c.volume;
                iv.pan = c.pan;
                iv.volumeEnvelope = c.volumeEnvelope;
                iv.clipIdx = ci;
                iv.leadInType = c.leadIn.type;
                iv.leadInDuration = c.leadIn.duration;
                iv.leadInAlignment = c.leadIn.alignment;
                iv.leadInEasing = c.leadIn.easing;
                iv.trailOutType = c.trailOut.type;
                iv.trailOutDuration = c.trailOut.duration;
                iv.trailOutAlignment = c.trailOut.alignment;
                iv.trailOutEasing = c.trailOut.easing;
                iv.stabilizerKeyframes = c.stabilizerKeyframes;
                ivs.append(iv);
                accum += clipDur;
            }
        }
        trackIntervals.append(ivs);
    }

    // Premiere-style overlap for boundary transitions. Center-at-Cut
    // alignment (Premiere default): the transition is split so half lives
    // in A's trail handle and half in B's lead handle. Each side may
    // borrow from the other when its own handle runs short, gracefully
    // degrading toward End-at-Cut (A no trail) or Start-at-Cut (B no lead)
    // before giving up. We only kick in when the two intervals are
    // adjacent on the timeline (no leadIn gap on B), since a deliberate
    // gap means the user wants a hard cut, not a transition.
    for (auto &trackIvs : trackIntervals) {
        for (int j = 1; j < trackIvs.size(); ++j) {
            Interval &a = trackIvs[j - 1];
            Interval &b = trackIvs[j];
            if (!isOverlapTransition(a.trailOutType)) continue;
            if (a.trailOutType != b.leadInType) continue;
            if (qAbs(a.timelineEnd - b.timelineStart) > 1e-3) continue;
            const double askedD = qMin(a.trailOutDuration, b.leadInDuration);
            if (askedD <= 0.0) continue;
            const double aSpeed = (a.speed > 0.0) ? a.speed : 1.0;
            const double bSpeed = (b.speed > 0.0) ? b.speed : 1.0;
            // A's trail handle = source frames past clipOut, divided by speed.
            // We grab the source duration from the owning ClipInfo since the
            // Interval struct does not carry it.
            double aTrailAvailable = 0.0;
            auto *aTrack = m_videoTracks.value(a.trackIdx, nullptr);
            if (aTrack && a.clipIdx >= 0 && a.clipIdx < aTrack->clips().size()) {
                const auto &ac = aTrack->clips()[a.clipIdx];
                aTrailAvailable = qMax(0.0, (ac.duration - a.clipOut) / aSpeed);
            }
            const double bLeadAvailable = qMax(0.0, b.clipIn / bSpeed);

            double aExtend = 0.0, bRetract = 0.0;
            switch (a.trailOutAlignment) {
                case TransitionAlignment::Start:
                    // Premiere "Start at Cut": entire transition AFTER cut.
                    // Consumes A's trail handle only; B stays put.
                    aExtend = qMin(askedD, aTrailAvailable);
                    break;
                case TransitionAlignment::End:
                    // Premiere "End at Cut": entire transition BEFORE cut.
                    // Consumes B's lead handle only; A stays put.
                    bRetract = qMin(askedD, bLeadAvailable);
                    break;
                case TransitionAlignment::Center:
                    // Premiere default: D/2 each side, with borrowing when
                    // one side runs short so the user gets the requested
                    // duration whenever physics allow.
                    aExtend  = qMin(askedD * 0.5, aTrailAvailable);
                    bRetract = qMin(askedD * 0.5, bLeadAvailable);
                    if (aExtend + bRetract < askedD) {
                        if (bRetract < askedD * 0.5) {
                            const double slack = qMin(askedD - aExtend - bRetract,
                                                      aTrailAvailable - aExtend);
                            if (slack > 0.0) aExtend += slack;
                        }
                        if (aExtend + bRetract < askedD && aExtend < askedD * 0.5) {
                            const double slack = qMin(askedD - aExtend - bRetract,
                                                      bLeadAvailable - bRetract);
                            if (slack > 0.0) bRetract += slack;
                        }
                    }
                    break;
            }
            const double effectiveD = aExtend + bRetract;
            if (effectiveD < 0.01) continue;

            a.clipOut       += aExtend * aSpeed;
            a.timelineEnd   += aExtend;
            b.timelineStart -= bRetract;
            b.clipIn        -= bRetract * bSpeed;
            a.trailOutDuration = effectiveD;
            b.leadInDuration   = effectiveD;
            qInfo() << "[SEQ] overlap pair:"
                    << Transition::typeName(a.trailOutType)
                    << "track=" << a.trackIdx
                    << "askedD=" << askedD
                    << "aTrail=" << aTrailAvailable << "bLead=" << bLeadAvailable
                    << "aExtend=" << aExtend << "bRetract=" << bRetract
                    << "effectiveD=" << effectiveD;
        }
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
        e.rotation2DDegrees = iv.rotation2DDegrees;
        e.opacity = iv.opacity;
        e.fitContain = iv.fitContain;
        e.fitCover = iv.fitCover;
        e.colorMeta = iv.colorMeta;
        e.volume = iv.volume;
        e.pan = iv.pan;
        e.volumeEnvelope = iv.volumeEnvelope;
        e.sourceClipIndex = iv.clipIdx;
        e.leadInType = iv.leadInType;
        e.leadInDuration = iv.leadInDuration;
        e.leadInEasing = iv.leadInEasing;
        e.trailOutType = iv.trailOutType;
        e.trailOutDuration = iv.trailOutDuration;
        e.trailOutEasing = iv.trailOutEasing;
        e.stabilizerKeyframes = iv.stabilizerKeyframes;
        // STAGE4B: carry this entry's track-matte assignment (if any) so the
        // live GPU compositor can apply it identically to the export path
        // (TimelineFrameRenderer.cpp:792-814). This is pure data plumbing —
        // no behavior changes until part 2 wires the GPU gate. The matte hash
        // is keyed by trackMatteClipKey(trackIdx,clipIdx), the same key space
        // export uses (renderClipId). Read m_trackMatteEntries directly: this
        // is a const member access (computePlaybackSequence is const) and the
        // hash is COW so no aliasing risk. Default-OFF (no matte entry) leaves
        // matteTypeOrdinal=0 / matteSourceClipId empty == today's behavior.
        {
            const QString matteKey = trackMatteClipKey(e.sourceTrack, e.sourceClipIndex);
            const auto mit = m_trackMatteEntries.constFind(matteKey);
            if (mit != m_trackMatteEntries.cend()) {
                e.matteTypeOrdinal = static_cast<int>(mit.value().matteType);
                e.matteSourceClipId = mit.value().matteSourceClipId;
            }
            const auto pit = m_clipParentEntries.constFind(matteKey);
            if (pit != m_clipParentEntries.cend() && pit.value() != matteKey)
                e.parentClipId = pit.value();
        }
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
    // Sum-mix every visible audio track. AudioMixer in VideoPlayer combines
    // each emitted entry into a single stereo output, so unlike the legacy
    // A1-wins gap-fill (which silently dropped overlapping clips) every
    // unmuted track contributes simultaneously. Track mute, per-clip
    // volume, and source identity all ride on the PlaybackEntry — the
    // mixer keys decoders by (filePath, clipIn, sourceTrack, sourceClipIndex)
    // to reuse open file contexts across re-emits.
    QVector<PlaybackEntry> result;
    if (m_audioTracks.isEmpty())
        return result;

    for (int t = 0; t < m_audioTracks.size(); ++t) {
        auto *track = m_audioTracks[t];
        if (!track || track->isHidden()) continue;
        const auto &clips = track->clips();
        const bool trackMuted = track->isMuted();
        double accum = 0.0;
        for (int ci = 0; ci < clips.size(); ++ci) {
            const auto &c = clips[ci];
            accum += qMax(0.0, c.leadInSec);
            const double clipDur = c.effectiveDuration();
            if (clipDur <= 0.0) continue;
            PlaybackEntry e;
            e.filePath = c.filePath;
            e.clipIn = c.inPoint;
            e.clipOut = (c.outPoint > 0.0) ? c.outPoint : c.duration;
            e.timelineStart = accum;
            e.timelineEnd = accum + clipDur;
            e.speed = (c.speed > 0.0) ? c.speed : 1.0;
            e.sourceTrack = t;
            e.audioMuted = trackMuted;
            e.volume = c.volume;
            e.pan = c.pan;
            e.volumeEnvelope = c.volumeEnvelope;
            e.sourceClipIndex = ci;
            e.leadInType = c.leadIn.type;
            e.leadInDuration = c.leadIn.duration;
            e.leadInEasing = c.leadIn.easing;
            e.trailOutType = c.trailOut.type;
            e.trailOutDuration = c.trailOut.duration;
            e.trailOutEasing = c.trailOut.easing;
            result.append(e);
            accum += clipDur;
        }
    }

    // Stable order: (timelineStart asc, sourceTrack asc) — purely cosmetic
    // for the mixer (entry lookup is by AudioTrackKey) but keeps debug logs
    // easy to follow.
    std::sort(result.begin(), result.end(),
              [](const PlaybackEntry &a, const PlaybackEntry &b) {
                  if (a.timelineStart != b.timelineStart)
                      return a.timelineStart < b.timelineStart;
                  return a.sourceTrack < b.sourceTrack;
              });
    return result;
}

void Timeline::refreshPlaybackSequence()
{
    // External hook for proxy generation / proxy mode toggle. The clip graph
    // hasn't changed, but the resolved playback paths have, so just re-emit
    // the same sequences we'd produce for any other rebuild trigger and let
    // MainWindow's getProxyPath translation in the sequenceChanged handler
    // pick up the now-Ready proxies.
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
}

void Timeline::saveUndoState(const QString &description)
{
    m_undoManager->saveState(currentState(), description);
}

void Timeline::setProjectOutputConfig(int width, int height, bool explicitOutput)
{
    m_projectWidth = width;
    m_projectHeight = height;
    m_projectExplicitOutput = explicitOutput;
}

void Timeline::setClipParentEntries(const QHash<QString, QString> &entries)
{
    m_clipParentEntries.clear();
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (!it.key().isEmpty() && !it.value().isEmpty() && it.key() != it.value())
            m_clipParentEntries.insert(it.key(), it.value());
    }
}

void Timeline::setClipParent(const QString& childKey, const QString& parentKey)
{
    if (childKey.isEmpty())
        return;
    if (parentKey.isEmpty() || childKey == parentKey) {
        m_clipParentEntries.remove(childKey);
        scheduleEmitSequenceChanged();
        return;
    }
    if (m_clipParentEntries.value(childKey) == parentKey)
        return;
    m_clipParentEntries.insert(childKey, parentKey);
    scheduleEmitSequenceChanged();
}

void Timeline::clearClipParent(const QString& childKey)
{
    if (childKey.isEmpty() || !m_clipParentEntries.contains(childKey))
        return;
    m_clipParentEntries.remove(childKey);
    scheduleEmitSequenceChanged();
}

TimelineState Timeline::currentState() const
{
    TimelineState state;
    state.videoTracks.reserve(m_videoTracks.size());
    for (const auto *t : m_videoTracks)
        state.videoTracks.append(t ? t->clips() : QVector<ClipInfo>{});
    state.audioTracks.reserve(m_audioTracks.size());
    for (const auto *t : m_audioTracks)
        state.audioTracks.append(t ? t->clips() : QVector<ClipInfo>{});
    state.selectedClip = m_videoTrack->selectedClip();

    for (int i = 0; i < m_videoTracks.size(); ++i) {
        if (!m_videoTracks[i]) continue;
        const int sel = m_videoTracks[i]->selectedClip();
        if (sel >= 0) {
            state.selectedVideoTrackIndex = i;
            state.selectedVideoClipIndex = sel;
            break;
        }
    }
    for (int i = 0; i < m_audioTracks.size(); ++i) {
        if (!m_audioTracks[i]) continue;
        const int sel = m_audioTracks[i]->selectedClip();
        if (sel >= 0) {
            state.selectedAudioTrackIndex = i;
            state.selectedAudioClipIndex = sel;
            break;
        }
    }

    state.playheadPos = m_playheadPos;
    state.clipParentEntries = m_clipParentEntries;

    if (m_audioMixer) {
        const int n = audioTrackCount();
        state.audioTrackGains.resize(n);
        for (int i = 0; i < n; ++i)
            state.audioTrackGains[i] = m_audioMixer->trackGain(i);
    }

    state.projectWidth = m_projectWidth;
    state.projectHeight = m_projectHeight;
    state.projectExplicitOutput = m_projectExplicitOutput;

    return state;
}

void Timeline::restoreState(const TimelineState &state)
{
    undotrace::log("restoreState:enter");
    // Make sure the editor has at least as many rows as the snapshot. We
    // only ADD here — never remove — because deleting a track widget
    // mid-undo invalidates pointers other UI code may already hold (the
    // undo path is reached from a menu callback, not a clean teardown).
    while (m_videoTracks.size() < state.videoTracks.size()) addVideoTrack();
    while (m_audioTracks.size() < state.audioTracks.size()) addAudioTrack();

    for (int i = 0; i < m_videoTracks.size(); ++i) {
        if (!m_videoTracks[i]) continue;
        const auto &clips = (i < state.videoTracks.size())
                            ? state.videoTracks[i]
                            : QVector<ClipInfo>{};
        m_videoTracks[i]->setClips(clips);
        m_videoTracks[i]->update();
    }
    for (int i = 0; i < m_audioTracks.size(); ++i) {
        if (!m_audioTracks[i]) continue;
        const auto &clips = (i < state.audioTracks.size())
                            ? state.audioTracks[i]
                            : QVector<ClipInfo>{};
        m_audioTracks[i]->setClips(clips);
        m_audioTracks[i]->update();
    }
    setClipParentEntries(state.clipParentEntries);

    // Clear every track's UI selection (via blockSignals so we don't emit
    // a cascade of intermediate selectionChanged signals from stale tracks).
    // Then restore the selection on the correct track, letting signals
    // through so wireTrackSelection can sync linked clips and emit
    // clipSelected/clipSelectedOnTrack.
    clearAllSelections();
    m_activeVideoTrackIndex = -1;

    bool videoSelSet = false;
    if (state.selectedVideoTrackIndex >= 0
        && state.selectedVideoTrackIndex < m_videoTracks.size()) {
        TimelineTrack *vt = m_videoTracks[state.selectedVideoTrackIndex];
        if (vt && state.selectedVideoClipIndex >= 0
            && state.selectedVideoClipIndex < vt->clipCount()) {
            vt->setSelectedClip(state.selectedVideoClipIndex);
            videoSelSet = true;
        }
    }
    // Fallback to legacy V1-relative selection for undo entries that
    // predate the V2 track-aware fields (selectedVideoTrackIndex == -1).
    if (!videoSelSet) {
        const bool vWas = m_videoTrack->blockSignals(true);
        m_videoTrack->setSelectedClip(state.selectedClip);
        m_videoTrack->blockSignals(vWas);
        emit clipSelected(state.selectedClip);
        m_activeVideoTrackIndex = state.selectedClip < 0 ? -1 : 0;
        emit clipSelectedOnTrack(state.selectedClip < 0 ? -1 : 0,
                                 state.selectedClip);
    }

    if (state.selectedAudioTrackIndex >= 0
        && state.selectedAudioTrackIndex < m_audioTracks.size()) {
        TimelineTrack *at = m_audioTracks[state.selectedAudioTrackIndex];
        if (at && state.selectedAudioClipIndex >= 0
            && state.selectedAudioClipIndex < at->clipCount()) {
            // Block signals so the audio track restore doesn't re-emit
            // clipSelected/clipSelectedOnTrack (the video path or legacy
            // fallback already handled those).
            const bool aWas = at->blockSignals(true);
            at->setSelectedClip(state.selectedAudioClipIndex);
            at->blockSignals(aWas);
        }
    } else if (state.selectedAudioTrackIndex < 0) {
        // Legacy fallback: old undo entries predating the V2 track-aware
        // fields restore selection on A1 the same as V1. If
        // selectedAudioTrackIndex >= 0 but the track no longer exists,
        // clear selection silently (no fallback).
        const bool aWas = m_audioTrack->blockSignals(true);
        m_audioTrack->setSelectedClip(state.selectedClip);
        m_audioTrack->blockSignals(aWas);
    }

    m_playheadPos = state.playheadPos;
    syncPlayheadOverlay();

    if (m_audioMixer && !state.audioTrackGains.isEmpty()) {
        const int n = qMin(state.audioTrackGains.size(), audioTrackCount());
        for (int i = 0; i < n; ++i)
            m_audioMixer->setTrackGain(i, state.audioTrackGains[i]);
    }

    // スナップショットに捕捉されたプロジェクト出力ジオメトリ(サイズ)を復元する。
    // SNS プリセットのリサイズを undo したとき、プロジェクトサイズも一緒に戻して
    // 縦伸びを断つ。projectWidth <= 0 のレガシー/空スナップショットはスキップ
    // (現在サイズを壊さない)。scheduleEmitSequenceChanged の前に行い、サイズ更新後の
    // シーケンス再構築でプレビューが新サイズで合成されるようにする。
    if (state.projectWidth > 0 && state.projectHeight > 0) {
        m_projectWidth = state.projectWidth;
        m_projectHeight = state.projectHeight;
        m_projectExplicitOutput = state.projectExplicitOutput;
        emit projectOutputConfigRestored(state.projectWidth, state.projectHeight,
                                         state.projectExplicitOutput);
    }

    // setClips bypasses the modified() signal path; trigger explicitly so the
    // VideoPlayer rebuilds its sequence after undo/redo.
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
    undotrace::log("restoreState:exit");
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
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
}

// --- Timeline markers (Premiere Pro / DaVinci Resolve parity) ---
// Markers are stored sorted by timelineUs so navigation is O(log N) and
// lane painting is left-to-right without per-paint sorting. Mutations emit
// markersChanged() so any future panel UI re-syncs without manual hooks.

namespace {
// Insert `m` into the sorted vector by timelineUs. Stable for equal stamps.
inline void insertMarkerSorted(QVector<Marker> &vec, const Marker &m)
{
    auto it = std::lower_bound(vec.begin(), vec.end(), m.timelineUs,
        [](const Marker &lhs, qint64 rhs) { return lhs.timelineUs < rhs; });
    vec.insert(it, m);
}
} // namespace

int Timeline::addMarker(qint64 timelineUs, const QString &label, QColor color)
{
    Marker m;
    m.id = m_nextMarkerId++;
    m.timelineUs = qMax<qint64>(0, timelineUs);
    m.label = label;
    m.color = color.isValid() ? color : QColor(QStringLiteral("#ff5050"));
    insertMarkerSorted(m_markersData, m);
    if (m_markerLane) m_markerLane->update();
    emit markersChanged();
    return m.id;
}

bool Timeline::removeMarker(int id)
{
    for (int i = 0; i < m_markersData.size(); ++i) {
        if (m_markersData[i].id == id) {
            m_markersData.removeAt(i);
            if (m_markerLane) m_markerLane->update();
            emit markersChanged();
            return true;
        }
    }
    return false;
}

bool Timeline::updateMarker(int id, const Marker &updated)
{
    for (int i = 0; i < m_markersData.size(); ++i) {
        if (m_markersData[i].id == id) {
            // Preserve the original id even if `updated.id` was 0 / unset
            // (callers shouldn't have to thread the id back through).
            Marker copy = updated;
            copy.id = id;
            copy.timelineUs = qMax<qint64>(0, copy.timelineUs);
            copy.durationUs = qMax<qint64>(0, copy.durationUs);
            if (!copy.color.isValid())
                copy.color = QColor(QStringLiteral("#ff5050"));
            // Re-sort if the time changed, otherwise just overwrite in place.
            if (copy.timelineUs == m_markersData[i].timelineUs) {
                m_markersData[i] = copy;
            } else {
                m_markersData.removeAt(i);
                insertMarkerSorted(m_markersData, copy);
            }
            if (m_markerLane) m_markerLane->update();
            emit markersChanged();
            return true;
        }
    }
    return false;
}

void Timeline::setMarkerDuration(int markerId, qint64 durationUs)
{
    // MK-1: clamp negatives to 0 (point marker). The span never moves the
    // marker's start, so no re-sort is needed — sort order is by timelineUs.
    const qint64 dur = qMax<qint64>(0, durationUs);
    for (int i = 0; i < m_markersData.size(); ++i) {
        if (m_markersData[i].id == markerId) {
            if (m_markersData[i].durationUs == dur)
                return;  // no change, avoid spurious repaint/signal
            m_markersData[i].durationUs = dur;
            if (m_markerLane) m_markerLane->update();
            emit markersChanged();
            return;
        }
    }
}

Marker Timeline::markerById(int id) const
{
    for (const auto &m : m_markersData)
        if (m.id == id) return m;
    return Marker{}; // default-constructed (id == -1) on miss
}

QVector<Marker> Timeline::markersInRange(qint64 startUs, qint64 endUs) const
{
    QVector<Marker> result;
    if (endUs < startUs) std::swap(startUs, endUs);
    // Markers are sorted; binary-search the lower bound for cheap range queries.
    auto lo = std::lower_bound(m_markersData.begin(), m_markersData.end(), startUs,
        [](const Marker &m, qint64 v) { return m.timelineUs < v; });
    auto hi = std::upper_bound(m_markersData.begin(), m_markersData.end(), endUs,
        [](qint64 v, const Marker &m) { return v < m.timelineUs; });
    for (auto it = lo; it != hi; ++it)
        result.append(*it);
    return result;
}

int Timeline::nextMarkerAfter(qint64 timelineUs) const
{
    auto it = std::upper_bound(m_markersData.begin(), m_markersData.end(), timelineUs,
        [](qint64 v, const Marker &m) { return v < m.timelineUs; });
    if (it == m_markersData.end()) return -1;
    return it->id;
}

int Timeline::prevMarkerBefore(qint64 timelineUs) const
{
    auto it = std::lower_bound(m_markersData.begin(), m_markersData.end(), timelineUs,
        [](const Marker &m, qint64 v) { return m.timelineUs < v; });
    if (it == m_markersData.begin()) return -1;
    --it;
    return it->id;
}

void Timeline::setMarkers(const QVector<Marker> &markers)
{
    m_markersData = markers;
    for (Marker &m : m_markersData) {
        m.timelineUs = qMax<qint64>(0, m.timelineUs);
        m.durationUs = qMax<qint64>(0, m.durationUs);
        if (!m.color.isValid())
            m.color = QColor(QStringLiteral("#ff5050"));
    }
    std::sort(m_markersData.begin(), m_markersData.end(),
        [](const Marker &a, const Marker &b) { return a.timelineUs < b.timelineUs; });
    // Restore the monotonic id counter so newly-added markers don't collide
    // with serialized ids from a loaded project.
    int maxId = 0;
    for (const auto &m : m_markersData) maxId = qMax(maxId, m.id);
    m_nextMarkerId = maxId + 1;
    if (m_markerLane) m_markerLane->update();
    emit markersChanged();
}

// --- Adjustment layers (Premiere/Photoshop parity) ---
// CRUD methods mirror the marker pattern above. Stored unsorted; the
// composeAdjustmentLayersAt() helper handles trackIndex stacking at
// render time. Id counter is monotonic and resets on setAdjustmentLayers().

int Timeline::addAdjustmentLayer(const AdjustmentLayer &layer)
{
    AdjustmentLayer copy = layer;
    copy.id = m_nextAdjustmentLayerId++;
    // Defensive: clamp negative timestamps and keep start <= end.
    copy.timelineStartUs = qMax<qint64>(0, copy.timelineStartUs);
    copy.timelineEndUs   = qMax<qint64>(copy.timelineStartUs, copy.timelineEndUs);
    m_adjustmentLayers.append(copy);
    emit adjustmentLayersChanged();
    return copy.id;
}

bool Timeline::removeAdjustmentLayer(int id)
{
    for (int i = 0; i < m_adjustmentLayers.size(); ++i) {
        if (m_adjustmentLayers[i].id == id) {
            m_adjustmentLayers.removeAt(i);
            emit adjustmentLayersChanged();
            return true;
        }
    }
    return false;
}

bool Timeline::updateAdjustmentLayer(int id, const AdjustmentLayer &layer)
{
    for (int i = 0; i < m_adjustmentLayers.size(); ++i) {
        if (m_adjustmentLayers[i].id == id) {
            // Preserve the original id even if `layer.id` was 0 / unset.
            AdjustmentLayer copy = layer;
            copy.id = id;
            copy.timelineStartUs = qMax<qint64>(0, copy.timelineStartUs);
            copy.timelineEndUs   = qMax<qint64>(copy.timelineStartUs, copy.timelineEndUs);
            m_adjustmentLayers[i] = copy;
            emit adjustmentLayersChanged();
            return true;
        }
    }
    return false;
}

AdjustmentLayer Timeline::adjustmentLayerById(int id) const
{
    for (const auto &l : m_adjustmentLayers)
        if (l.id == id) return l;
    return AdjustmentLayer{}; // default-constructed (id == -1) on miss
}

void Timeline::setAdjustmentLayers(const QVector<AdjustmentLayer> &layers)
{
    m_adjustmentLayers = layers;
    // Restore the monotonic id counter so newly-added layers don't collide
    // with serialized ids from a loaded project.
    int maxId = 0;
    for (const auto &l : m_adjustmentLayers) maxId = qMax(maxId, l.id);
    m_nextAdjustmentLayerId = maxId + 1;
    emit adjustmentLayersChanged();
}

void Timeline::onPlayheadAutoScrollTick()
{
    // Drives auto-scroll while the user drags the playhead bar with the
    // cursor held in the outer 15% of the viewport. The bar visually pins
    // at the 15% / 85% boundary; the timeline scrolls under it, advancing
    // m_playheadPos so the user keeps scrubbing without losing sight of
    // the bar.
    if (!m_playheadDragging) {
        if (m_playheadAutoScrollTimer)
            m_playheadAutoScrollTimer->stop();
        return;
    }
    if (!m_scrollArea || !m_scrollArea->viewport() || !m_videoTrack)
        return;
    QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
    if (!hbar)
        return;

    const int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 0)
        return;
    const int scrollX = hbar->value();
    const int leftZone = static_cast<int>(viewportW * 0.25);
    const int rightZone = static_cast<int>(viewportW * 0.75);

    int delta = 0;
    if (m_playheadDragViewportX < leftZone) {
        const int overshoot = leftZone - m_playheadDragViewportX;
        delta = -qBound(2, overshoot / 2, 80);
    } else if (m_playheadDragViewportX > rightZone) {
        const int overshoot = m_playheadDragViewportX - rightZone;
        delta = qBound(2, overshoot / 2, 80);
    } else {
        return; // cursor inside the central 70% free zone — no auto-scroll
    }

    const int newScrollX = qBound(hbar->minimum(), scrollX + delta, hbar->maximum());
    if (newScrollX == scrollX)
        return; // already at the rail; nothing more to scroll

    hbar->setValue(newScrollX);

    const int boundary = (delta < 0) ? leftZone : rightZone;
    const int barX = newScrollX + boundary;
    m_playheadPos = m_videoTrack->xToSeconds(barX);
    if (m_playheadOverlay)
        m_playheadOverlay->setPlayheadX(barX);
    emit scrubPositionChanged(m_playheadPos);
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

// US-INT-4: V1-only writer. Mutates the underlying TimelineTrack clip
// vector via clips()/setClips() so the change participates in the normal
// invalidation/repaint flow.
void Timeline::setClipStabilizerKeyframes(int clipIndex,
                                          const QVector<StabilizerKeyframe> &kfs)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size())
        return;
    clips[clipIndex].stabilizerKeyframes = kfs;
    track->setClips(clips);
    track->update();
    scheduleEmitSequenceChanged();
}
