#include "Timeline.h"
#include "UndoManager.h"
#include <optional>
#include <QWheelEvent>

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
    m_dragging = true;
    m_playheadX = event->pos().x();
    emit playheadMoved(m_playheadX);
    update();
}

void PlayheadOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) { m_playheadX = qMax(0, event->pos().x()); emit playheadMoved(m_playheadX); update(); }
}

void PlayheadOverlay::mouseReleaseEvent(QMouseEvent *) { m_dragging = false; }

// --- TimelineTrack ---

TimelineTrack::TimelineTrack(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(CLIP_HEIGHT);
    setMaximumHeight(CLIP_HEIGHT);
    setMouseTracking(true);
    setAcceptDrops(true);
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
    if (m_selectedClip >= m_clips.size()) m_selectedClip = m_clips.size() - 1;
    updateMinimumWidth(); update(); emit selectionChanged(m_selectedClip); emit modified();
}

void TimelineTrack::moveClip(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_clips.size()) return;
    if (toIndex < 0 || toIndex >= m_clips.size()) return;
    if (fromIndex == toIndex) return;
    ClipInfo clip = m_clips[fromIndex];
    m_clips.removeAt(fromIndex);
    m_clips.insert(toIndex, clip);
    m_selectedClip = toIndex;
    updateMinimumWidth(); update(); emit clipMoved(fromIndex, toIndex); emit modified();
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

void TimelineTrack::setClips(const QVector<ClipInfo> &clips) { m_clips = clips; updateMinimumWidth(); update(); }
void TimelineTrack::setSelectedClip(int index) { m_selectedClip = index; update(); emit selectionChanged(index); }

void TimelineTrack::setPixelsPerSecond(int pps)
{
    m_pixelsPerSecond = qMax(2, qMin(100, pps));
    updateMinimumWidth();
    update();
}

int TimelineTrack::clipAtX(int x) const
{
    int cx = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        int w = qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
        if (x >= cx && x < cx + w) return i;
        cx += w;
    }
    return -1;
}

int TimelineTrack::clipStartX(int index) const
{
    int x = 0;
    for (int i = 0; i < index && i < m_clips.size(); ++i)
        x += qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
    return x;
}

double TimelineTrack::xToSeconds(int x) const { return static_cast<double>(x) / m_pixelsPerSecond; }
int TimelineTrack::secondsToX(double seconds) const { return static_cast<int>(seconds * m_pixelsPerSecond); }

int TimelineTrack::snapToEdge(int x) const
{
    if (!m_snapEnabled) return x;
    int cx = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        if (qAbs(x - cx) <= SNAP_THRESHOLD) return cx;
        cx += qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
        if (qAbs(x - cx) <= SNAP_THRESHOLD) return cx;
    }
    return x;
}

void TimelineTrack::updateMinimumWidth()
{
    int totalWidth = 0;
    for (const auto &c : m_clips)
        totalWidth += qMax(20, static_cast<int>(c.effectiveDuration() * m_pixelsPerSecond));
    setMinimumWidth(totalWidth + 100);
}

void TimelineTrack::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    int x = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        int clipWidth = qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
        QRect clipRect(x, 0, clipWidth, CLIP_HEIGHT);
        QColor color = (i % 2 == 0) ? QColor(0x44, 0x88, 0xCC) : QColor(0x44, 0xAA, 0x88);
        if (i == m_selectedClip) color = color.lighter(140);
        if (m_dragMode == DragMode::MoveClip && i == m_dropTargetIndex) {
            painter.setPen(QPen(Qt::yellow, 3));
            painter.drawLine(x, 0, x, CLIP_HEIGHT);
        }
        painter.fillRect(clipRect, color);
        if (i == m_selectedClip) {
            painter.setPen(QPen(Qt::yellow, 2));
            painter.drawRect(clipRect.adjusted(1, 1, -1, -1));
            painter.fillRect(QRect(x, 0, TRIM_HANDLE_WIDTH, CLIP_HEIGHT), QColor(255, 255, 255, 80));
            painter.fillRect(QRect(x + clipWidth - TRIM_HANDLE_WIDTH, 0, TRIM_HANDLE_WIDTH, CLIP_HEIGHT), QColor(255, 255, 255, 80));
        } else {
            painter.setPen(QPen(Qt::white, 1));
            painter.drawRect(clipRect);
        }
        // Draw waveform if available
        if (!m_clips[i].waveform.isEmpty()) {
            painter.setPen(QPen(QColor(100, 200, 150, 180), 1));
            const auto &wf = m_clips[i].waveform;
            int wfStart = 0;
            int wfEnd = wf.peaks.size();
            double clipDurSec = m_clips[i].effectiveDuration();
            int peaksForClip = static_cast<int>(clipDurSec * wf.peaksPerSecond);
            if (peaksForClip > 0 && clipWidth > 2) {
                int midY = CLIP_HEIGHT / 2;
                int maxAmp = CLIP_HEIGHT / 2 - 4;
                for (int px = 0; px < clipWidth; ++px) {
                    int peakIdx = px * peaksForClip / clipWidth;
                    if (peakIdx >= wf.peaks.size()) break;
                    float amp = wf.peaks[peakIdx];
                    int h = static_cast<int>(amp * maxAmp);
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
}

void TimelineTrack::mousePressEvent(QMouseEvent *event)
{
    int clickedClip = clipAtX(event->pos().x());
    if (event->button() == Qt::LeftButton && clickedClip >= 0) {
        setSelectedClip(clickedClip);
        emit clipClicked(clickedClip);
        int cx = clipStartX(clickedClip);
        int clipWidth = qMax(20, static_cast<int>(m_clips[clickedClip].effectiveDuration() * m_pixelsPerSecond));
        int localX = event->pos().x() - cx;
        if (localX <= TRIM_HANDLE_WIDTH) {
            m_dragMode = DragMode::TrimLeft; m_dragClipIndex = clickedClip;
            m_dragStartX = event->pos().x(); m_dragOriginalValue = m_clips[clickedClip].inPoint;
        } else if (localX >= clipWidth - TRIM_HANDLE_WIDTH) {
            m_dragMode = DragMode::TrimRight; m_dragClipIndex = clickedClip;
            m_dragStartX = event->pos().x();
            m_dragOriginalValue = m_clips[clickedClip].outPoint > 0 ? m_clips[clickedClip].outPoint : m_clips[clickedClip].duration;
        } else {
            m_dragMode = DragMode::MoveClip; m_dragClipIndex = clickedClip;
            m_dragStartX = event->pos().x(); m_dropTargetIndex = -1;
        }
    } else if (clickedClip < 0) {
        setSelectedClip(-1);
    }
}

void TimelineTrack::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragMode == DragMode::MoveClip && m_dragClipIndex >= 0) {
        int target = clipAtX(event->pos().x());
        if (target < 0) target = m_clips.size() - 1;
        m_dropTargetIndex = target;
        setCursor(Qt::ClosedHandCursor); update(); return;
    }
    if (m_dragMode == DragMode::TrimLeft || m_dragMode == DragMode::TrimRight) {
        int snappedX = snapToEdge(event->pos().x());
        int dx = snappedX - m_dragStartX;
        double deltaSec = static_cast<double>(dx) / m_pixelsPerSecond;
        ClipInfo &clip = m_clips[m_dragClipIndex];
        if (m_dragMode == DragMode::TrimLeft) {
            double newIn = qMax(0.0, m_dragOriginalValue + deltaSec);
            double maxIn = (clip.outPoint > 0 ? clip.outPoint : clip.duration) - 0.1;
            clip.inPoint = qMin(newIn, maxIn);
        } else {
            double newOut = qMin(clip.duration, m_dragOriginalValue + deltaSec);
            clip.outPoint = qMax(newOut, clip.inPoint + 0.1);
        }
        updateMinimumWidth(); update(); return;
    }
    int hover = clipAtX(event->pos().x());
    if (hover >= 0 && hover == m_selectedClip) {
        int cx = clipStartX(hover);
        int cw = qMax(20, static_cast<int>(m_clips[hover].effectiveDuration() * m_pixelsPerSecond));
        int lx = event->pos().x() - cx;
        setCursor((lx <= TRIM_HANDLE_WIDTH || lx >= cw - TRIM_HANDLE_WIDTH) ? Qt::SizeHorCursor : Qt::OpenHandCursor);
    } else { setCursor(Qt::ArrowCursor); }
}

void TimelineTrack::mouseReleaseEvent(QMouseEvent *)
{
    if (m_dragMode == DragMode::MoveClip && m_dragClipIndex >= 0 && m_dropTargetIndex >= 0)
        if (m_dragClipIndex != m_dropTargetIndex) moveClip(m_dragClipIndex, m_dropTargetIndex);
    if (m_dragMode == DragMode::TrimLeft || m_dragMode == DragMode::TrimRight) emit modified();
    m_dragMode = DragMode::None; m_dragClipIndex = -1; m_dropTargetIndex = -1;
    setCursor(Qt::ArrowCursor); update();
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

    m_infoLabel = new QLabel("Timeline", this);
    m_infoLabel->setStyleSheet("font-weight: bold; color: #ccc;");
    layout->addWidget(m_infoLabel);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setStyleSheet("background-color: #2a2a2a;");

    auto *tracksContainer = new QWidget();
    auto *tracksOuterLayout = new QVBoxLayout(tracksContainer);
    tracksOuterLayout->setContentsMargins(0, 0, 0, 0);
    tracksOuterLayout->setSpacing(0);

    auto *playheadOverlay = new PlayheadOverlay(tracksContainer);
    playheadOverlay->setFixedHeight(15);
    playheadOverlay->setStyleSheet("background-color: #222;");
    connect(playheadOverlay, &PlayheadOverlay::playheadMoved, this, [this](int x) {
        m_playheadPos = m_videoTrack->xToSeconds(x);
        emit positionChanged(m_playheadPos);
    });
    tracksOuterLayout->addWidget(playheadOverlay);

    m_tracksWidget = new QWidget();
    m_tracksLayout = new QVBoxLayout(m_tracksWidget);
    m_tracksLayout->setSpacing(2);

    // Create initial V1 and A1
    m_videoTrack = new TimelineTrack(this);
    m_audioTrack = new TimelineTrack(this);
    m_videoTracks.append(m_videoTrack);
    m_audioTracks.append(m_audioTrack);

    auto *v1Label = new QLabel("V1");
    v1Label->setStyleSheet("color: #4488CC; font-size: 11px;");
    m_tracksLayout->addWidget(v1Label);
    m_tracksLayout->addWidget(m_videoTrack);

    auto *a1Label = new QLabel("A1");
    a1Label->setStyleSheet("color: #44AA88; font-size: 11px;");
    m_tracksLayout->addWidget(a1Label);
    m_tracksLayout->addWidget(m_audioTrack);

    m_tracksLayout->addStretch();

    tracksOuterLayout->addWidget(m_tracksWidget);
    m_scrollArea->setWidget(tracksContainer);
    layout->addWidget(m_scrollArea);
    setStyleSheet("background-color: #333;");

    connect(m_videoTrack, &TimelineTrack::clipClicked, this, &Timeline::onTrackClipClicked);
    connect(m_videoTrack, &TimelineTrack::selectionChanged, this, [this](int index) {
        m_audioTrack->setSelectedClip(index);
        emit clipSelected(index);
    });
    connect(m_videoTrack, &TimelineTrack::modified, this, &Timeline::onTrackModified);
    connect(m_audioTrack, &TimelineTrack::modified, this, &Timeline::onTrackModified);
}

void Timeline::addVideoTrack()
{
    int num = m_videoTracks.size() + 1;
    auto *label = new QLabel(QString("V%1").arg(num));
    label->setStyleSheet("color: #4488CC; font-size: 11px;");
    auto *track = new TimelineTrack(this);
    track->setPixelsPerSecond(m_zoomLevel);
    track->setSnapEnabled(snapEnabled());

    // Insert before audio tracks
    int insertIdx = m_videoTracks.size() * 2; // each track has label+widget
    m_tracksLayout->insertWidget(insertIdx, label);
    m_tracksLayout->insertWidget(insertIdx + 1, track);

    m_videoTracks.append(track);
    connect(track, &TimelineTrack::modified, this, &Timeline::onTrackModified);
    updateInfoLabel();
}

void Timeline::addAudioTrack()
{
    int num = m_audioTracks.size() + 1;
    auto *label = new QLabel(QString("A%1").arg(num));
    label->setStyleSheet("color: #44AA88; font-size: 11px;");
    auto *track = new TimelineTrack(this);
    track->setPixelsPerSecond(m_zoomLevel);
    track->setSnapEnabled(snapEnabled());

    // Insert before stretch
    int insertIdx = m_tracksLayout->count() - 1; // before stretch
    m_tracksLayout->insertWidget(insertIdx, label);
    m_tracksLayout->insertWidget(insertIdx + 1, track);

    m_audioTracks.append(track);
    connect(track, &TimelineTrack::modified, this, &Timeline::onTrackModified);
    updateInfoLabel();
}

void Timeline::addClip(const QString &filePath)
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
    // Generate waveform async
    auto *wfGen = new WaveformGenerator(this);
    connect(wfGen, &WaveformGenerator::waveformReady, this,
        [this, wfGen](const QString &path, const WaveformData &data) {
            // Find and update clip with matching path
            auto vClips = m_videoTrack->clips();
            for (int i = 0; i < vClips.size(); ++i) {
                if (vClips[i].filePath == path && vClips[i].waveform.isEmpty()) {
                    vClips[i].waveform = data;
                    m_videoTrack->setClips(vClips);
                    break;
                }
            }
            wfGen->deleteLater();
        });
    wfGen->generateAsync(filePath);

    m_videoTrack->addClip(clip);
    m_audioTrack->addClip(clip);
    saveUndoState("Add clip");
    updateInfoLabel();
}

void Timeline::splitAtPlayhead()
{
    const auto &clips = m_videoTrack->clips();
    double accum = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        double clipDur = clips[i].effectiveDuration();
        if (m_playheadPos >= accum && m_playheadPos < accum + clipDur) {
            double localPos = m_playheadPos - accum;
            m_videoTrack->splitClipAt(i, localPos);
            m_audioTrack->splitClipAt(i, localPos);
            saveUndoState("Split clip");
            updateInfoLabel();
            return;
        }
        accum += clipDur;
    }
}

void Timeline::deleteSelectedClip()
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    m_videoTrack->removeClip(sel);
    m_audioTrack->removeClip(sel);
    saveUndoState("Delete clip");
    updateInfoLabel();
}

void Timeline::rippleDeleteSelectedClip() { deleteSelectedClip(); }
bool Timeline::hasSelection() const { return m_videoTrack->selectedClip() >= 0; }

void Timeline::copySelectedClip()
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return;
    m_clipboard = m_videoTrack->clips()[sel];
}

void Timeline::pasteClip()
{
    if (!m_clipboard.has_value()) return;
    int insertAt = m_videoTrack->selectedClip() + 1;
    if (insertAt <= 0) insertAt = m_videoTrack->clipCount();
    m_videoTrack->insertClip(insertAt, m_clipboard.value());
    m_audioTrack->insertClip(insertAt, m_clipboard.value());
    m_videoTrack->setSelectedClip(insertAt);
    m_audioTrack->setSelectedClip(insertAt);
    saveUndoState("Paste clip");
    updateInfoLabel();
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

bool Timeline::snapEnabled() const { return m_videoTrack->snapEnabled(); }

// Zoom
void Timeline::zoomIn() { setZoomLevel(m_zoomLevel + 5); }
void Timeline::zoomOut() { setZoomLevel(qMax(2, m_zoomLevel - 5)); }

void Timeline::setZoomLevel(int pixelsPerSecond)
{
    m_zoomLevel = qMax(2, qMin(100, pixelsPerSecond));
    for (auto *t : m_videoTracks) t->setPixelsPerSecond(m_zoomLevel);
    for (auto *t : m_audioTracks) t->setPixelsPerSecond(m_zoomLevel);
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

void Timeline::setPlayheadPosition(double seconds) { m_playheadPos = seconds; }

double Timeline::totalDuration() const
{
    double total = 0.0;
    for (const auto &clip : m_videoTrack->clips()) total += clip.effectiveDuration();
    return total;
}

void Timeline::onTrackClipClicked(int index)
{
    m_audioTrack->setSelectedClip(index);
    emit clipSelected(index);
}

void Timeline::onTrackModified() {}

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
    m_videoTrack->setSelectedClip(state.selectedClip);
    m_audioTrack->setSelectedClip(state.selectedClip);
    m_playheadPos = state.playheadPos;
    emit clipSelected(state.selectedClip);
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
    saveUndoState("Load project");
    updateInfoLabel();
}

void Timeline::updateInfoLabel()
{
    QString info = QString("Timeline - %1 clip(s) | Zoom %2x | %3")
        .arg(m_videoTrack->clipCount())
        .arg(m_zoomLevel)
        .arg(snapEnabled() ? "Snap ON" : "Snap OFF");
    if (m_videoTracks.size() > 1 || m_audioTracks.size() > 1)
        info += QString(" | V%1 A%2").arg(m_videoTracks.size()).arg(m_audioTracks.size());
    if (hasMarkedRange())
        info += QString(" | I/O: %1s-%2s").arg(m_markIn, 0, 'f', 1).arg(m_markOut, 0, 'f', 1);
    m_infoLabel->setText(info);
}
