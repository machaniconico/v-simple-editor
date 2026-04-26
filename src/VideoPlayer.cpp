#include "VideoPlayer.h"
#include <QSet>
#include "GLPreview.h"
#include <QSettings>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QUrl>
#include <QtGlobal>
#include <QDebug>
#include <QPointer>
#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QPen>
#include <QFontMetrics>
#include <cmath>
#include <limits>

namespace {

QString formatTimestamp(int64_t positionUs)
{
    const int totalSeconds = static_cast<int>(qMax<int64_t>(0, positionUs / AV_TIME_BASE));
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;

    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }

    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

}

VideoPlayer::VideoPlayer(QWidget *parent)
    : QWidget(parent)
{
    qInfo() << "VideoPlayer::ctor";
    setupUI();
    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setSingleShot(true);
    connect(m_playbackTimer, &QTimer::timeout, this, &VideoPlayer::handlePlaybackTick);

    m_seekTimer = new QTimer(this);
    m_seekTimer->setSingleShot(true);
    m_seekTimer->setInterval(15);
    connect(m_seekTimer, &QTimer::timeout, this, &VideoPlayer::performPendingSeek);

    m_mixer = new AudioMixer(this);
    connect(m_mixer, &AudioMixer::decoderError, this,
            [](const QString &msg) { qWarning() << "AudioMixer:" << msg; });
}

VideoPlayer::~VideoPlayer()
{
    qInfo() << "VideoPlayer::dtor";
    // Stop any in-flight timers before decoder teardown so a queued
    // handlePlaybackTick doesn't fire on a half-destroyed object.
    if (m_playbackTimer) m_playbackTimer->stop();
    if (m_seekTimer)     m_seekTimer->stop();
    resetDecoder();
    // Tear down V2+ decoder pool after the legacy V1 decoder is gone, then
    // release the shared HW device context. The pool decoders only hold
    // av_buffer_ref'd handles to m_sharedPoolHwDeviceCtx, so freeing them
    // first decrements the refcount to 1 (held by VideoPlayer itself).
    clearAllPoolDecoders();
    if (m_sharedPoolHwDeviceCtx)
        av_buffer_unref(&m_sharedPoolHwDeviceCtx);
    m_sharedPoolHwPixFmt = AV_PIX_FMT_NONE;
}

void VideoPlayer::setupUI()
{
    auto *layout = new QVBoxLayout(this);

    auto *displayStack = new QStackedWidget(this);

    m_videoDisplay = new QLabel(this);
    m_videoDisplay->setAlignment(Qt::AlignCenter);
    m_videoDisplay->setMinimumSize(480, 270);
    m_videoDisplay->setText("Drop a video file or use File > Open");
    m_videoDisplay->setStyleSheet("background-color: #1a1a1a; color: #888; font-size: 16px;");

    m_glPreview = new GLPreview(this);
    m_glPreview->setMinimumSize(384, 216);
    connect(m_glPreview, &GLPreview::textRectRequested,
            this, &VideoPlayer::textRectRequested);
    connect(m_glPreview, &GLPreview::textInlineCommitted,
            this, &VideoPlayer::textInlineCommitted);
    connect(m_glPreview, &GLPreview::textOverlayEditCommitted,
            this, &VideoPlayer::textOverlayEditCommitted);
    connect(m_glPreview, &GLPreview::textOverlayEditStarted,
            this, [this](int idx) { setHiddenTextOverlayIndex(idx); });
    connect(m_glPreview, &GLPreview::textOverlayEditEnded,
            this, [this]() { setHiddenTextOverlayIndex(-1); });
    connect(m_glPreview, &GLPreview::textOverlayRectChanged,
            this, &VideoPlayer::textOverlayRectChanged);
    connect(m_glPreview, &GLPreview::videoSourceTransformChanged,
            this, [this](double scale, double dx, double dy) {
                // Map back to the current entry's source clip so MainWindow
                // can persist the new transform to the right ClipInfo.
                if (m_activeEntry < 0 || m_activeEntry >= m_sequence.size())
                    return;
                const auto &entry = m_sequence[m_activeEntry];
                emit videoSourceTransformChanged(entry.sourceTrack, entry.sourceClipIndex,
                                                 scale, dx, dy);
            });

    displayStack->addWidget(m_videoDisplay); // index 0: software
    displayStack->addWidget(m_glPreview);    // index 1: GL
    displayStack->setCurrentIndex(m_useGL ? 1 : 0);

    layout->addWidget(displayStack, 1);

    auto *controls = new QHBoxLayout();

    m_proxyButton = new QPushButton(this);
    // Frame step: |◀ and ▶|
    m_stepBackButton = new QPushButton(QString::fromUtf8("\xE2\x8F\xAE"), this); // ⏮
    m_stepFwdButton  = new QPushButton(QString::fromUtf8("\xE2\x8F\xAD"), this); // ⏭
    // Unicode media controls: ▶ U+25B6 / ⏸ U+23F8 / ⏹ U+23F9
    // m_playButton doubles as pause: updatePlayButton() flips its glyph
    // and tooltip based on m_playing, and the click is wired to togglePlay()
    // so a single press swaps the state instead of needing two buttons.
    m_playButton = new QPushButton(QString::fromUtf8("\xE2\x96\xB6"), this);
    m_stopButton = new QPushButton(QString::fromUtf8("\xE2\x8F\xB9"), this);
    m_seekBar = new QSlider(Qt::Horizontal, this);
    m_timeLabel = new QLabel("00:00 / 00:00", this);

    const QString mediaBtnStyle =
        "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
        "  border-radius: 4px; font-size: 16px; padding: 0; }"
        "QPushButton:hover { background-color: #555; }"
        "QPushButton:pressed { background-color: #666; }"
        "QPushButton:disabled { color: #777; background-color: #383838; }";
    m_playButton->setFixedSize(40, 32);
    m_stopButton->setFixedSize(40, 32);
    m_playButton->setStyleSheet(mediaBtnStyle);
    m_stopButton->setStyleSheet(mediaBtnStyle);
    m_playButton->setToolTip(QStringLiteral("再生"));
    m_stopButton->setToolTip(QStringLiteral("停止"));
    m_stepBackButton->setFixedSize(40, 32);
    m_stepFwdButton->setFixedSize(40, 32);
    m_stepBackButton->setStyleSheet(mediaBtnStyle);
    m_stepFwdButton->setStyleSheet(mediaBtnStyle);
    m_stepBackButton->setToolTip(QStringLiteral("1フレーム戻る (←)"));
    m_stepFwdButton->setToolTip(QStringLiteral("1フレーム進む (→)"));
    connect(m_stepBackButton, &QPushButton::clicked, this, &VideoPlayer::stepBackward);
    connect(m_stepFwdButton,  &QPushButton::clicked, this, &VideoPlayer::stepForward);

    {
        QSettings prefs("VSimpleEditor", "Preferences");
        int saved = prefs.value("proxyDivisor", 1).toInt();
        if (saved != 1 && saved != 2 && saved != 4 && saved != 8) saved = 1;
        m_proxyDivisor = saved;
    }
    auto proxyLabel = [](int d) {
        switch (d) {
        case 2: return QStringLiteral("1/2");
        case 4: return QStringLiteral("1/4");
        case 8: return QStringLiteral("1/8");
        default: return QStringLiteral("Full");
        }
    };
    m_proxyButton->setText(proxyLabel(m_proxyDivisor));
    m_proxyButton->setFixedSize(56, 32);
    m_proxyButton->setStyleSheet(mediaBtnStyle);
    m_proxyButton->setToolTip(QStringLiteral("プロキシ設定 (再生用プロキシ ON/OFF と プレビュー解像度)"));
    // Forward to MainWindow's proxy settings dialog so this seekbar-left
    // affordance and the toolbar's "プロキシモード切替" / "プロキシ生成..."
    // entries all drive the same configuration surface.
    connect(m_proxyButton, &QPushButton::clicked, this, &VideoPlayer::proxySettingsRequested);
    m_timeLabel->setFixedWidth(120);
    m_seekBar->setRange(0, 0);
    m_seekBar->setTracking(false);

    controls->addWidget(m_proxyButton);
    controls->addWidget(m_stepBackButton);
    controls->addWidget(m_playButton);
    controls->addWidget(m_stopButton);
    controls->addWidget(m_stepFwdButton);
    controls->addWidget(m_seekBar);
    controls->addWidget(m_timeLabel);

    layout->addLayout(controls);

    connect(m_playButton, &QPushButton::clicked, this, &VideoPlayer::togglePlay);
    connect(m_stopButton, &QPushButton::clicked, this, &VideoPlayer::stop);
    connect(m_seekBar, &QSlider::sliderMoved, this, [this](int pos) {
        m_lastDragMs = pos;
        previewSeek(pos);
    });
    connect(m_seekBar, &QSlider::sliderReleased, this, [this]() {
        if (m_lastDragMs >= 0)
            seek(m_lastDragMs);
    });
    connect(m_seekBar, &QSlider::valueChanged, this, [this](int value) {
        if (m_lastDragMs >= 0) {
            m_lastDragMs = -1;
            return;
        }
        if (!m_seekBar->isSliderDown())
            seek(value);
    });
}

void VideoPlayer::setProxyDivisor(int divisor)
{
    static const QSet<int> allowed = {1, 2, 4, 8};
    if (!allowed.contains(divisor) || divisor == m_proxyDivisor)
        return;
    m_proxyDivisor = divisor;
    if (m_proxyButton) {
        const QString text = (divisor == 1) ? QStringLiteral("Full")
                            : QStringLiteral("1/%1").arg(divisor);
        m_proxyButton->setText(text);
    }
    QSettings("VSimpleEditor", "Preferences").setValue("proxyDivisor", m_proxyDivisor);
    if (!m_lastSourceFrame.isNull())
        displayFrame(m_lastSourceFrame);
}

void VideoPlayer::loadFile(const QString &filePath)
{
    qInfo() << "VideoPlayer::loadFile BEGIN" << filePath;
    // Audio is now driven by the independent A-track schedule via
    // setAudioSequence -> AudioMixer. loadFile only reloads the FFmpeg
    // video decoder. No audio touching here — audio is owned by the mixer
    // and is independent of which video file the FFmpeg decoder is on, since
    // the audio schedule owns the audio channel the old 200ms mute lockout
    // is no longer applicable.
    resetDecoder();
    qInfo() << "  resetDecoder done";
    m_loadedFilePath = filePath;

    const QByteArray pathUtf8 = filePath.toUtf8();
    if (avformat_open_input(&m_formatCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        qWarning() << "avformat_open_input failed for" << filePath;
        m_videoDisplay->setText("Failed to open file");
        return;
    }
    qInfo() << "  avformat_open_input ok";

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        qWarning() << "avformat_find_stream_info failed";
        resetDecoder();
        m_videoDisplay->setText("Failed to read stream info");
        return;
    }
    qInfo() << "  avformat_find_stream_info ok, nb_streams=" << m_formatCtx->nb_streams;

    m_videoStreamIndex = -1;
    for (unsigned i = 0; i < m_formatCtx->nb_streams; i++) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (m_videoStreamIndex < 0) {
        resetDecoder();
        m_videoDisplay->setText("No video stream found");
        return;
    }

    auto *codecpar = m_formatCtx->streams[m_videoStreamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        resetDecoder();
        m_videoDisplay->setText("Unsupported codec");
        return;
    }

    m_hdrInfo = {};
    m_hdrInfo.primaries = codecpar->color_primaries;
    m_hdrInfo.trc = codecpar->color_trc;
    m_hdrInfo.colorspace = codecpar->color_space;
    m_hdrInfo.bitDepth = std::max(8, av_get_bits_per_pixel(av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(codecpar->format))) / 3);
    m_hdrInfo.isHdr = (codecpar->color_trc == AVCOL_TRC_SMPTE2084
                       || codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67);

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx || avcodec_parameters_to_context(m_codecCtx, codecpar) < 0) {
        resetDecoder();
        m_videoDisplay->setText("Failed to initialize codec");
        return;
    }

    m_hwPixFmt = AV_PIX_FMT_NONE;
    if (av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                m_hwPixFmt = cfg->pix_fmt;
                break;
            }
        }
    }

    if (m_hwPixFmt != AV_PIX_FMT_NONE && m_hwDeviceCtx) {
        m_codecCtx->opaque = this;
        m_codecCtx->get_format = &VideoPlayer::getHwFormatCallback;
        m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        qInfo() << "  HW decode enabled via D3D11VA";
    } else {
        if (m_hwDeviceCtx) {
            av_buffer_unref(&m_hwDeviceCtx);
            m_hwDeviceCtx = nullptr;
        }
        qInfo() << "  HW decode unavailable, using software decoding";
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        resetDecoder();
        m_videoDisplay->setText("Failed to open codec");
        return;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    if (!m_packet || !m_frame || !m_swFrame) {
        resetDecoder();
        m_videoDisplay->setText("Failed to allocate decode buffers");
        return;
    }
    qInfo() << "  packet/frame allocated";

    if (m_formatCtx->duration > 0) {
        m_durationUs = m_formatCtx->duration;
    } else {
        AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
        if (stream->duration > 0)
            m_durationUs = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    }

    m_frameDurationUs = streamFrameDurationUs();
    m_displayAspectRatio = streamDisplayAspectRatio();
    qInfo() << "  duration=" << m_durationUs << "frameDur=" << m_frameDurationUs
            << "aspect=" << m_displayAspectRatio;
    if (m_glPreview)
        m_glPreview->setDisplayAspectRatio(m_displayAspectRatio);
    if (!m_suppressUiUpdates) {
        m_seekBar->setRange(0, sliderPositionForUs(m_durationUs));
        emit durationChanged(static_cast<double>(m_durationUs) / AV_TIME_BASE);
    }

    qInfo() << "  entering seekInternal(0)";
    if (!seekInternal(0, true, true)) {
        qWarning() << "seekInternal(0) failed";
        resetDecoder();
        m_videoDisplay->setText("Failed to decode first frame");
        return;
    }
    qInfo() << "  seekInternal(0) ok";

    // NOTE: AudioMixer owns the audio path now. loadFile only touches the
    // video decoder; audio entry switching, mute, and seek are all driven
    // by setAudioSequence -> m_mixer.

    // If a sequence is active, restore its slider range so the seekbar shows
    // the full timeline rather than just this file's duration.
    if (sequenceActive()) {
        applySequenceSliderRange();
        emit durationChanged(static_cast<double>(m_sequenceDurationUs) / AV_TIME_BASE);
    }

    updatePositionUi();
    qInfo() << "VideoPlayer::loadFile END";
}

void VideoPlayer::setSequence(const QVector<PlaybackEntry> &entries)
{
    qInfo() << "VideoPlayer::setSequence count=" << entries.size();

    // Topmost-track-wins via single-decoder pipeline: take Timeline's
    // sequence as-is. Timeline::computePlaybackSequence sorts by
    // (timelineStart asc, sourceTrack asc), so findActiveEntryAt naturally
    // hits V1 first wherever V1 exists; in V1-empty gaps only V2 (or V3,
    // ...) matches — that's what lets seeks into V2-only regions load V2's
    // source instead of bouncing back to V1's head. The Stage 2 multi-layer
    // compositor will replace this with per-sourceTrack LayerDecoders.
    int64_t totalUs = 0;
    for (const auto &e : entries) {
        const int64_t entryEndUs = static_cast<int64_t>(e.timelineEnd * AV_TIME_BASE);
        if (entryEndUs > totalUs) totalUs = entryEndUs;
    }

    m_sequence = entries;
    m_sequenceDurationUs = totalUs;

    // Phase 1d pool reconciliation. For every active V2+ TrackDecoder, drop
    // it to the eviction grace pool when the (filePath, clipIn, sourceTrack)
    // key disappears from the new sequence. The grace TTL keeps lastFrameRgb
    // available for a few ticks after eviction so harvestOverlayLayer can
    // fall back to it while the replacement decoder catches up. The slot
    // manager release mirrors the qHash-based packing used in
    // acquireDecoderForClip.
    {
        QSet<TrackKey> newKeys;
        for (const auto &e : entries) {
            // Include every entry (V1 included). V1 enters the pool when
            // it isn't m_activeEntry — Scenario E: V2 starts earlier than
            // V1 so m_sequence sorts V2 first; the legacy decoder rides
            // V2 while V1 has to come from the pool to paint on top.
            // Excluding V1 here would force-evict its pool decoder on
            // every Timeline edit and the user sees a stutter every
            // drag/trim commit.
            newKeys.insert(TrackKey{e.filePath, qRound64(e.clipIn * 1000.0),
                                    e.sourceTrack, e.sourceClipIndex});
        }
        for (auto it = m_trackDecoders.begin(); it != m_trackDecoders.end(); ) {
            const TrackKey &k = it.key();
            if (newKeys.contains(k)) {
                ++it;
                continue;
            }
            TrackDecoder *d = it.value();
            const int slotClipId = static_cast<int>(qHash(k));
            m_slotManager.releaseSlot(slotClipId);
            m_slotIdToKey.remove(slotClipId);
            it = m_trackDecoders.erase(it);
            if (d) {
                d->graceTtlTicks = 4;
                m_evictionGracePool.append(d);
            }
        }
    }

    if (entries.isEmpty()) {
        // Timeline emptied (all clips deleted). Pause and clear the slider so
        // the player doesn't keep ticking against a stale file. We don't tear
        // down the decoder — preview path may still be using it.
        m_activeEntry = -1;
        m_timelinePositionUs = 0;
        if (m_playing) pause();
        m_seekBar->setRange(0, 0);
        emit durationChanged(0.0);
        // US-T35 clear any OBS-style transform so a fresh import starts at
        // identity instead of inheriting the last clip's scale/offset.
        if (m_glPreview)
            m_glPreview->resetVideoSourceTransform();
        // Drop the cached compositor base on Timeline-empty too.
        // Otherwise a re-import after clearing keeps the previous V1 frame
        // around and the first compositor tick on the new clip flashes
        // the stale picture (paired with resetDecoder's clear).
        m_lastV1RawFrame = QImage();
        return;
    }

    // Slider range follows the sequence total.
    applySequenceSliderRange();
    emit durationChanged(static_cast<double>(m_sequenceDurationUs) / AV_TIME_BASE);

    // Clamp current timeline position into the new sequence and pick an
    // active entry. Default to 0 if we have no prior position (e.g. first add).
    int64_t clamped = qBound<int64_t>(0, m_timelinePositionUs, m_sequenceDurationUs);
    int desiredIdx = findActiveEntryAt(clamped);
    if (desiredIdx < 0) {
        desiredIdx = 0;
        clamped = static_cast<int64_t>(m_sequence.first().timelineStart * AV_TIME_BASE);
    }

    const auto &target = m_sequence[desiredIdx];
    const bool needFileSwitch = (target.filePath != m_loadedFilePath) || !m_formatCtx;
    // Idempotency: if the structurally identical entry is already active and
    // the file is already loaded, skip the seek entirely so back-to-back
    // sequenceChanged emissions (e.g. video track + audio track both
    // emitting modified() during a single addClip) don't repeatedly disturb
    // the mixer and cause audible artifacts.
    const bool entryStructurallyChanged = needFileSwitch || (m_activeEntry != desiredIdx);

    // Snapshot playback state BEFORE loadFile+resetDecoder tear down the
    // playback timer, so we can resurrect it after the file swap completes.
    const bool wasPlaying = m_playing;
    qInfo() << "VideoPlayer::setSequence flow"
            << "wasPlaying=" << wasPlaying
            << "needFileSwitch=" << needFileSwitch
            << "entryStructurallyChanged=" << entryStructurallyChanged
            << "desiredIdx=" << desiredIdx
            << "m_activeEntry(before)=" << m_activeEntry;

    if (needFileSwitch) {
        loadFile(target.filePath);  // resets decoder, clears m_playing
        if (wasPlaying)
            m_playing = true;
    }

    m_activeEntry = desiredIdx;
    m_timelinePositionUs = clamped;

    if (entryStructurallyChanged) {
        const int64_t localUs = entryLocalPositionUs(desiredIdx, clamped);
        seekInternal(localUs, true, true);
    }

    // If a sequenceChanged arrived mid-playback (e.g. a linked-clip drag was
    // released while playing), loadFile+resetDecoder halted the playback
    // timer. Resurrect it here — without this the player sits in a zombie
    // state where m_playing=true but nothing actually ticks, which looks
    // like the pause button stopped responding.
    if (wasPlaying && needFileSwitch) {
        qInfo() << "VideoPlayer::setSequence resurrecting playback timer";
        scheduleNextFrame();
    }
    // Audio-side resume is handled by setAudioSequence (called separately
    // via MainWindow's audioSequenceChanged wiring) and by the per-tick
    // Mixer's master clock + decode thread.

    // Keep the play/pause button enabled states in sync with m_playing.
    // Without this, the button may show the wrong enabled state after we
    // restore m_playing post-loadFile — and the user experiences a "dead"
    // pause button even though the click is wired.
    updatePlayButton();

    updatePositionUi();
}

void VideoPlayer::setAudioSequence(const QVector<PlaybackEntry> &entries)
{
    qInfo() << "VideoPlayer::setAudioSequence count=" << entries.size();
    const bool hadEntries = m_audioSequenceHadEntries;
    m_audioSequenceHadEntries = !entries.isEmpty();
    if (!m_mixer) return;
    m_mixer->setSequence(entries);
    if (entries.isEmpty()) {
        m_mixer->stop();
        return;
    }
    // Don't seek on every re-emit: AudioMixer::setSequence already flushes
    // rings + sets seekPending for any entry whose timeline range moved,
    // and an unconditional seek here would close+reopen the QAudioSink on
    // every volume slider drag or mute toggle (audible click). Only seek
    // when transitioning empty->non-empty so the mixer lands on the
    // current playhead the first time it gets a schedule.
    if (!hadEntries) {
        m_mixer->seekTo(m_timelinePositionUs);
    }
    if (m_playing && m_playbackSpeed >= 0.0) {
        m_mixer->play();
    } else {
        m_mixer->pause();
    }
}

int VideoPlayer::findActiveEntryAt(int64_t timelineUs) const
{
    if (m_sequence.isEmpty()) return -1;
    const double tSec = static_cast<double>(timelineUs) / AV_TIME_BASE;
    for (int i = 0; i < m_sequence.size(); ++i) {
        const auto &e = m_sequence[i];
        if (tSec >= e.timelineStart && tSec < e.timelineEnd)
            return i;
    }
    // At or past the very end → last entry.
    if (tSec >= m_sequence.last().timelineEnd - 1e-6)
        return m_sequence.size() - 1;
    // Before the first → first.
    if (tSec < m_sequence.first().timelineStart)
        return 0;
    return -1;
}

QVector<int> VideoPlayer::findActiveEntriesAt(int64_t timelineUs) const
{
    QVector<int> result;
    if (m_sequence.isEmpty()) return result;
    const double tSec = static_cast<double>(timelineUs) / AV_TIME_BASE;
    for (int i = 0; i < m_sequence.size(); ++i) {
        const auto &e = m_sequence[i];
        if (tSec >= e.timelineStart && tSec < e.timelineEnd)
            result.append(i);
    }
    return result;
}

int64_t VideoPlayer::entryLocalPositionUs(int entryIdx, int64_t timelineUs) const
{
    if (entryIdx < 0 || entryIdx >= m_sequence.size()) return 0;
    const auto &e = m_sequence[entryIdx];
    const double tSec = static_cast<double>(timelineUs) / AV_TIME_BASE;
    const double offsetIntoEntry = qMax(0.0, tSec - e.timelineStart);
    const double speed = (e.speed > 0.0) ? e.speed : 1.0;
    const double localSec = e.clipIn + offsetIntoEntry * speed;
    return static_cast<int64_t>(localSec * AV_TIME_BASE);
}

int64_t VideoPlayer::fileLocalToTimelineUs(int entryIdx, int64_t fileLocalUs) const
{
    if (entryIdx < 0 || entryIdx >= m_sequence.size()) return 0;
    const auto &e = m_sequence[entryIdx];
    const double fileLocalSec = static_cast<double>(fileLocalUs) / AV_TIME_BASE;
    const double speed = (e.speed > 0.0) ? e.speed : 1.0;
    const double offsetIntoEntry = qMax(0.0, (fileLocalSec - e.clipIn) / speed);
    const double timelineSec = e.timelineStart + offsetIntoEntry;
    return static_cast<int64_t>(timelineSec * AV_TIME_BASE);
}

void VideoPlayer::applySequenceSliderRange()
{
    const int64_t sliderMaxMs = qMin<int64_t>(m_sequenceDurationUs / 1000,
                                              std::numeric_limits<int>::max());
    m_seekBar->setRange(0, static_cast<int>(sliderMaxMs));
}

int VideoPlayer::sliderTimelinePosition(int64_t timelineUs) const
{
    const int64_t ms = qMax<int64_t>(0, timelineUs / 1000);
    return static_cast<int>(qMin<int64_t>(ms, std::numeric_limits<int>::max()));
}

bool VideoPlayer::seekToTimelineUs(int64_t timelineUs, bool precise)
{
    if (m_sequence.isEmpty()) return false;
    timelineUs = qBound<int64_t>(0, timelineUs, m_sequenceDurationUs);
    int idx = findActiveEntryAt(timelineUs);
    if (idx < 0) idx = 0;
    if (idx >= m_sequence.size()) return false;

    const auto &e = m_sequence[idx];
    const bool needSwitch = (e.filePath != m_loadedFilePath) || !m_formatCtx;
    // Freeze UI updates across the loadFile → seek chain so the slider
    // doesn't flash back to 0 while the intermediate seekInternal(0) runs
    // inside loadFile. We explicitly call updatePositionUi at the end.
    const bool prevSuppress = m_suppressUiUpdates;
    m_suppressUiUpdates = needSwitch;
    const bool wasPlayingOuter = m_playing;
    if (needSwitch) {
        loadFile(e.filePath); // reloads the video decoder; audio routes through the mixer
        if (wasPlayingOuter)
            m_playing = true;
    }

    m_activeEntry = idx;
    m_timelinePositionUs = timelineUs;
    const int64_t localUs = entryLocalPositionUs(idx, timelineUs);
    const bool ok = seekInternal(localUs, true, precise);
    // Push the seeked-to clip's own transform so cross-clip seeks don't
    // inherit the previously-active clip's OBS-style scale/offset.
    if (m_glPreview)
        m_glPreview->setVideoSourceTransform(e.videoScale, e.videoDx, e.videoDy);
    m_suppressUiUpdates = prevSuppress;

    // resetDecoder (called from loadFile when needSwitch) ran updatePlayButton
    // with m_playing=false. Resurrect the video playback timer.
    if (wasPlayingOuter && needSwitch) {
        scheduleNextFrame();
    }
    // Audio side stays in step via the mixer's master clock — a seek
    // updates m_writeCursorUs and lazily re-seeks every active entry.
    if (m_mixer) m_mixer->seekTo(timelineUs);
    updatePlayButton();

    updatePositionUi();
    return ok;
}

bool VideoPlayer::advanceToEntry(int newEntryIdx)
{
    if (newEntryIdx < 0 || newEntryIdx >= m_sequence.size())
        return false;

    const auto &next = m_sequence[newEntryIdx];
    qInfo() << "VideoPlayer::advanceToEntry idx=" << newEntryIdx
            << "file=" << next.filePath
            << "timelineStart=" << next.timelineStart
            << "clipIn=" << next.clipIn;

    const bool wasPlaying = m_playing;
    const bool needSwitch = (next.filePath != m_loadedFilePath);
    // Suppress intermediate UI updates across the loadFile → seek chain so the
    // slider doesn't flash back to 0 while resetDecoder/seekInternal(0) inside
    // loadFile temporarily clear the slider range.
    const bool prevSuppress = m_suppressUiUpdates;
    m_suppressUiUpdates = needSwitch;
    if (needSwitch) {
        loadFile(next.filePath); // loadFile only reloads the video decoder; audio is driven by setAudioSequence
    }

    m_activeEntry = newEntryIdx;
    // US-T35 apply the new entry's per-clip video source transform to the
    // GL preview so each clip keeps its own scale/offset.
    if (m_glPreview) {
        m_glPreview->setVideoSourceTransform(next.videoScale, next.videoDx, next.videoDy);
    }
    m_timelinePositionUs = static_cast<int64_t>(next.timelineStart * AV_TIME_BASE);
    const int64_t startLocalUs = static_cast<int64_t>(next.clipIn * AV_TIME_BASE);
    if (!seekInternal(startLocalUs, true, true)) {
        m_suppressUiUpdates = prevSuppress;
        return false;
    }

    // Audio is driven by the AudioMixer's master clock now — re-anchor it
    // so the new entry's local position is reached on the next refill tick.
    m_timelinePositionUs = static_cast<int64_t>(next.timelineStart * AV_TIME_BASE);
    if (m_mixer) m_mixer->seekTo(m_timelinePositionUs);

    if (wasPlaying)
        m_playing = true;
    m_suppressUiUpdates = prevSuppress;
    // resetDecoder (called from loadFile) ran updatePlayButton with
    // m_playing=false and disabled the pause button. After we restore
    // m_playing above, sync the button enabled state again — otherwise
    // the user sees the pause button as un-clickable across every clip
    // boundary that crosses a file switch.
    updatePlayButton();
    updatePositionUi();
    return true;
}

void VideoPlayer::setCanvasSize(int width, int height)
{
    m_canvasWidth = width;
    m_canvasHeight = height;
    double ar = static_cast<double>(width) / height;
    m_videoDisplay->setMinimumSize(
        qMin(640, width / 2),
        qMin(360, height / 2));
    if (m_currentFrameImage.isNull()) {
        QString orientation = (ar > 1.0) ? "Landscape" : (ar < 1.0) ? "Portrait" : "Square";
        m_videoDisplay->setText(QString("%1x%2 %3\nDrop a video file or use File > Open")
            .arg(width).arg(height).arg(orientation));
    } else {
        refreshDisplayedFrame();
    }
}

void VideoPlayer::play()
{
    if (!m_formatCtx || !m_codecCtx)
        return;

    if (m_playing)
        return;

    m_playing = true;
    updatePlayButton();
    emit stateChanged(true);
    scheduleNextFrame();

    // Kick the AudioMixer into play. If the audio schedule is empty the
    // mixer already stopped itself; otherwise this resumes from the
    // current master-clock position. Reverse playback mutes audio in
    // setPlaybackSpeed.
    if (m_mixer && m_playbackSpeed >= 0.0)
        m_mixer->play();
}

void VideoPlayer::pause()
{
    if (m_playbackTimer)
        m_playbackTimer->stop();

    m_playing = false;
    updatePlayButton();
    emit stateChanged(false);

    if (m_mixer)
        m_mixer->pause();
}

void VideoPlayer::stepForward()
{
    pause();
    const int64_t step = m_frameDurationUs > 0 ? m_frameDurationUs : AV_TIME_BASE / 30;
    const int64_t target = qMin(m_durationUs > 0 ? m_durationUs - 1 : INT64_MAX,
                                m_currentPositionUs + step);
    seekInternal(target, /*displayFrame=*/true, /*precise=*/true);
}

void VideoPlayer::stepBackward()
{
    pause();
    const int64_t step = m_frameDurationUs > 0 ? m_frameDurationUs : AV_TIME_BASE / 30;
    const int64_t target = qMax<int64_t>(0, m_currentPositionUs - step);
    seekInternal(target, /*displayFrame=*/true, /*precise=*/true);
}

void VideoPlayer::stop()
{
    pause();
    seekInternal(0, true, true);
    if (m_mixer) {
        m_mixer->stop();
        m_mixer->seekTo(0);
    }
}

void VideoPlayer::seek(int positionMs)
{
    // In sequence mode positionMs is interpreted as TIMELINE ms (the slider
    // and Timeline both speak timeline coordinates). In legacy single-file
    // mode it's the file-local ms — performPendingSeek picks the right path.
    if (!sequenceActive() && (!m_formatCtx || !m_codecCtx))
        return;

    m_pendingSeekMs = qMax(0, positionMs);
    m_pendingSeekPrecise = true;
    if (m_playbackTimer)
        m_playbackTimer->stop();

    if (!m_seekInProgress && m_seekTimer && !m_seekTimer->isActive())
        m_seekTimer->start();
}

void VideoPlayer::previewSeek(int positionMs)
{
    if (!sequenceActive() && (!m_formatCtx || !m_codecCtx))
        return;

    m_pendingSeekMs = qMax(0, positionMs);
    if (m_playbackTimer)
        m_playbackTimer->stop();

    if (!m_seekInProgress && m_seekTimer && !m_seekTimer->isActive())
        m_seekTimer->start();
}

void VideoPlayer::setPlaybackSpeed(double speed)
{
    if (qFuzzyIsNull(speed))
        speed = 1.0;

    const bool wasReverse = (m_playbackSpeed < 0.0);
    const double absSpeed = qBound(0.25, std::abs(speed), 16.0);
    m_playbackSpeed = (speed < 0.0) ? -absSpeed : absSpeed;
    emit playbackSpeedChanged(m_playbackSpeed);

    if (m_playing)
        scheduleNextFrame();

    if (m_mixer) {
        if (m_playbackSpeed < 0.0) {
            // Reverse playback: silence the mixer. We don't yet support
            // reverse audio — pause until forward playback resumes.
            m_mixer->pause();
        } else {
            // Re-anchor the mixer if we're coming out of reverse: the
            // video timeline cursor moved backward while the mixer was
            // paused, so resuming without a seek would play audio from
            // the pre-reverse position.
            if (wasReverse) {
                m_mixer->seekTo(m_timelinePositionUs);
            }
            if (m_playing) {
                m_mixer->play();
            }
        }
    }
}

void VideoPlayer::speedUp()
{
    if (!m_playing) {
        setPlaybackSpeed(1.0);
        play();
        return;
    }

    if (m_playbackSpeed < 0.0)
        setPlaybackSpeed(1.0);
    else
        setPlaybackSpeed(qMin(16.0, m_playbackSpeed * 2.0));
}

void VideoPlayer::speedDown()
{
    if (!m_playing) {
        setPlaybackSpeed(-1.0);
        play();
        return;
    }

    if (m_playbackSpeed > 0.0)
        setPlaybackSpeed(-1.0);
    else
        setPlaybackSpeed(qMax(-16.0, m_playbackSpeed * 2.0));
}

void VideoPlayer::togglePlay()
{
    if (m_playing) {
        pause();
    } else {
        setPlaybackSpeed(1.0);
        play();
    }
}

void VideoPlayer::updatePlayButton()
{
    // The play button doubles as pause: flip its glyph and tooltip
    // depending on the current state. Stop stays always enabled.
    if (m_playButton) {
        m_playButton->setText(m_playing
            ? QString::fromUtf8("\xE2\x8F\xB8")    // ⏸ pause
            : QString::fromUtf8("\xE2\x96\xB6")); // ▶ play
        m_playButton->setToolTip(m_playing
            ? QStringLiteral("一時停止")
            : QStringLiteral("再生"));
        m_playButton->setEnabled(true);
    }
    if (m_stopButton) m_stopButton->setEnabled(true);
}

void VideoPlayer::displayFrame(const QImage &image)
{
    m_lastSourceFrame = image;
    const QImage composed = composeFrameWithOverlays(image);
    m_currentFrameImage = composed;
    if (m_useGL && m_glPreview) {
        m_glPreview->setDisplayAspectRatio(effectiveDisplayAspectRatio());
        int hdrTransfer = 0;
        if (m_hdrInfo.isHdr) {
            if (m_hdrInfo.trc == AVCOL_TRC_SMPTE2084) hdrTransfer = 1;
            else if (m_hdrInfo.trc == AVCOL_TRC_ARIB_STD_B67) hdrTransfer = 2;
        }
        m_glPreview->setHdrTransfer(hdrTransfer);
        m_glPreview->displayFrame(composed);
    } else {
        const QSize targetSize = fittedDisplaySize(m_videoDisplay->size());
        const QPixmap pixmap = QPixmap::fromImage(composed);
        m_videoDisplay->setPixmap(pixmap.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
}

void VideoPlayer::setTextOverlays(const QVector<EnhancedTextOverlay> &overlays)
{
    m_textOverlays = overlays;
    // Drop the hidden-during-edit index if the overlay list shrank under it.
    if (m_hiddenTextOverlayIndex >= m_textOverlays.size())
        m_hiddenTextOverlayIndex = -1;
    refreshTextOverlayHits();
    refreshDisplayedFrame();
}

void VideoPlayer::refreshTextOverlayHits()
{
    if (!m_glPreview)
        return;
    QVector<GLPreview::TextOverlayHit> hits;
    hits.reserve(m_textOverlays.size());
    for (int i = 0; i < m_textOverlays.size(); ++i) {
        const auto &ov = m_textOverlays[i];
        if (!ov.visible || ov.text.isEmpty())
            continue;
        // Skip overlays whose rect dimensions are zero — they're auto-sized
        // at paint time and don't have a stable hit area until the frame
        // is rendered. Click-to-edit works for explicit-size overlays only.
        if (ov.width <= 0.0 || ov.height <= 0.0)
            continue;
        GLPreview::TextOverlayHit hit;
        hit.index = i;
        hit.text = ov.text;
        hit.normalizedRect = QRectF(ov.x, ov.y, ov.width, ov.height);
        hits.append(hit);
    }
    m_glPreview->setTextOverlayHitList(hits);
}

QImage VideoPlayer::composeFrameWithOverlays(const QImage &source) const
{
    if (source.isNull())
        return source;

    const bool applyPreviewFx = !m_previewEffects.isEmpty()
                                && (m_previewEffectsLive || !m_playing);

    // Preview proxy: only shrink during playback, keep paused frames full-res.
    const int proxy = (applyPreviewFx && m_playing) ? qMax(1, m_proxyDivisor) : 1;
    auto runProxy = [proxy, this](const QImage &img) {
        if (proxy <= 1)
            return VideoEffectProcessor::applyEffectStack(img, {}, m_previewEffects);
        const QImage small = img.scaled(img.width() / proxy, img.height() / proxy,
                                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QImage processed = VideoEffectProcessor::applyEffectStack(
            small, {}, m_previewEffects);
        return processed.scaled(img.width(), img.height(),
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    };

    if (m_textOverlays.isEmpty()) {
        if (!applyPreviewFx) return source;
        return runProxy(source);
    }

    const double nowSec = static_cast<double>(
        sequenceActive() ? m_timelinePositionUs : m_currentPositionUs) / AV_TIME_BASE;

    QImage composed = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&composed);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int W = composed.width();
    const int H = composed.height();
    // US-T32 WYSIWYG: the inline text tool draws at the literal pointSize
    // in widget coordinates. When compose bakes overlays into the source
    // image at (W,H) and GLPreview later scales it to the current letterbox
    // for display, the display scale is letterboxH/H. To land at the same
    // visible pointSize we inverse-scale by H/letterboxH so the committed
    // text matches the inline input size 1:1 on screen.
    double letterboxH = static_cast<double>(H);
    if (m_glPreview) {
        const QRectF lb = m_glPreview->letterboxRect();
        if (lb.height() > 0.0 && std::isfinite(lb.height()))
            letterboxH = lb.height();
    }
    const double fontScale = (letterboxH > 0.0)
        ? (static_cast<double>(H) / letterboxH)
        : 1.0;
    for (int i = 0; i < m_textOverlays.size(); ++i) {
        if (i == m_hiddenTextOverlayIndex) continue;
        const auto &ov = m_textOverlays[i];
        if (!ov.visible || ov.text.isEmpty()) continue;
        const double start = ov.startTime;
        const double end   = (ov.endTime > 0.0) ? ov.endTime : 1e18;
        if (nowSec < start || nowSec >= end) continue;

        QFont font = ov.font;
        font.setPointSizeF(qMax(1.0, font.pointSizeF() * fontScale));
        p.setFont(font);

        const QFontMetrics fm(font);
        int boxW = qMax(1, static_cast<int>(ov.width  * W));
        int boxH = qMax(1, static_cast<int>(ov.height * H));
        if (ov.width <= 0.0 || ov.height <= 0.0) {
            const QSize textSize = fm.boundingRect(ov.text).size();
            boxW = textSize.width() + 16;
            boxH = textSize.height() + 8;
        }
        const int cx = static_cast<int>(ov.x * W);
        const int cy = static_cast<int>(ov.y * H);
        QRect box(cx - boxW / 2, cy - boxH / 2, boxW, boxH);

        if (ov.backgroundColor.alpha() > 0)
            p.fillRect(box, ov.backgroundColor);

        // Match QPainter::drawText(rect, AlignCenter|TextWordWrap, text) so
        // the committed render lands at the exact same position as the inline
        // text tool (widget-space drawText uses fm.boundingRect(rect,flags)
        // internally; horizontalAdvance + fm.height() drifts by 1-2 px on
        // certain glyph runs because horizontalAdvance includes right-side
        // bearing while the visual glyph rect doesn't).
        const QRect textRect = fm.boundingRect(
            box, Qt::AlignCenter | Qt::TextWordWrap, ov.text);
        const int baselineX = textRect.left();
        const int baselineY = textRect.top() + fm.ascent();
        QPainterPath path;
        path.addText(baselineX, baselineY, font, ov.text);

        if (ov.outlineWidth > 0 && ov.outlineColor.alpha() > 0) {
            QPen outline(ov.outlineColor);
            outline.setWidthF(ov.outlineWidth * fontScale);
            outline.setJoinStyle(Qt::RoundJoin);
            outline.setCapStyle(Qt::RoundCap);
            p.strokePath(path, outline);
        }

        if (ov.gradientEnabled) {
            const QRectF bb = path.boundingRect();
            const QPointF center = bb.center();

            // Build the effective stop list. Prefer multi-stop gradientStops;
            // fall back to the legacy 2-stop (+ midpoint) on empty.
            QVector<GradientStop> stops;
            if (ov.gradientStops.size() >= 2) {
                stops = ov.gradientStops;
            } else {
                const QColor a = ov.gradientReverse ? ov.gradientEnd   : ov.gradientStart;
                const QColor b = ov.gradientReverse ? ov.gradientStart : ov.gradientEnd;
                GradientStop s0, s1;
                s0.position = 0.0; s0.color = a; s0.opacity = a.alphaF();
                s1.position = 1.0; s1.color = b; s1.opacity = b.alphaF();
                stops = { s0, s1 };
                const double midT = qBound(0.01, ov.gradientMidpoint / 100.0, 0.99);
                GradientStop sm;
                sm.position = midT;
                sm.color = QColor(
                    (a.red()   + b.red())   / 2,
                    (a.green() + b.green()) / 2,
                    (a.blue()  + b.blue())  / 2,
                    (a.alpha() + b.alpha()) / 2);
                sm.opacity = 0.5 * (a.alphaF() + b.alphaF());
                stops.insert(1, sm);
            }
            if (ov.gradientReverse && !ov.gradientStops.isEmpty()) {
                for (auto &s : stops) s.position = 1.0 - s.position;
                std::sort(stops.begin(), stops.end(),
                          [](const GradientStop &a, const GradientStop &b){ return a.position < b.position; });
            }
            auto applyStops = [&](QGradient &g) {
                for (const auto &s : stops) {
                    QColor c = s.color;
                    c.setAlphaF(qBound(0.0, s.opacity, 1.0));
                    g.setColorAt(qBound(0.0, s.position, 1.0), c);
                }
            };

            if (ov.gradientType == 1) {
                // Radial: center the gradient on the text bbox, radius = half diagonal
                const double r = 0.5 * std::hypot(bb.width(), bb.height());
                QRadialGradient grad(center, r);
                applyStops(grad);
                p.fillPath(path, grad);
            } else {
                // Linear: project bbox onto angle so colors hit bbox edges exactly
                const double rad = qDegreesToRadians(ov.gradientAngle);
                const double dx = std::cos(rad);
                const double dy = std::sin(rad);
                const double halfSpan = 0.5 * (std::abs(bb.width() * dx) + std::abs(bb.height() * dy));
                const QPointF offset(dx * halfSpan, dy * halfSpan);
                QLinearGradient grad(center - offset, center + offset);
                applyStops(grad);
                p.fillPath(path, grad);
            }
        } else {
            p.fillPath(path, ov.color);
        }
    }
    p.end();
    if (applyPreviewFx)
        return runProxy(composed);
    return composed;
}

void VideoPlayer::setHiddenTextOverlayIndex(int index)
{
    if (m_hiddenTextOverlayIndex == index) return;
    m_hiddenTextOverlayIndex = index;
    if (!m_lastSourceFrame.isNull())
        displayFrame(m_lastSourceFrame);
}

void VideoPlayer::setColorCorrection(const ColorCorrection &cc)
{
    if (m_glPreview)
        m_glPreview->setColorCorrection(cc);
}

void VideoPlayer::setPreviewEffects(const QVector<VideoEffect> &effects, bool live)
{
    m_previewEffectsLive = live;

    const bool gpuEnabled = QSettings("VSimpleEditor", "Preferences")
                                .value("gpuEffectsEnabled", true).toBool()
                            && m_useGL && m_glPreview;

    QVector<VideoEffect> gpu;
    QVector<VideoEffect> cpu;
    for (const VideoEffect &e : effects) {
        const bool gpuCapable =
            e.type == VideoEffectType::Blur      ||
            e.type == VideoEffectType::Noise     ||
            e.type == VideoEffectType::Sepia     ||
            e.type == VideoEffectType::Grayscale ||
            e.type == VideoEffectType::Invert    ||
            e.type == VideoEffectType::Vignette;
        if (gpuEnabled && gpuCapable) gpu.append(e);
        else                          cpu.append(e);
    }

    m_previewEffects = cpu;
    if (m_glPreview)
        m_glPreview->setVideoEffects(gpu);

    if (!m_lastSourceFrame.isNull())
        displayFrame(m_lastSourceFrame);
}

void VideoPlayer::setGLAcceleration(bool enabled)
{
    m_useGL = enabled;
    auto *stack = qobject_cast<QStackedWidget*>(m_videoDisplay->parentWidget());
    if (stack)
        stack->setCurrentIndex(enabled ? 1 : 0);
    refreshDisplayedFrame();
}

void VideoPlayer::setTextToolActive(bool active)
{
    if (m_textToolActive == active)
        return;
    m_textToolActive = active;
    // Forward the mode to GLPreview (owns the drag capture + I-beam cursor)
    // and mirror the cursor on the QLabel fallback for non-GL software mode.
    if (m_glPreview)
        m_glPreview->setTextToolActive(active);
    if (m_videoDisplay) {
        if (active)
            m_videoDisplay->setCursor(Qt::IBeamCursor);
        else
            m_videoDisplay->unsetCursor();
    }
}

void VideoPlayer::clearTextToolRect()
{
    if (m_glPreview)
        m_glPreview->clearTextToolRect();
}

void VideoPlayer::setTextToolStyle(const QFont &font, const QColor &color)
{
    if (m_glPreview)
        m_glPreview->setTextToolStyle(font, color);
}

void VideoPlayer::setSnapStrength(double pixels)
{
    if (m_glPreview)
        m_glPreview->setSnapStrength(pixels);
}

bool VideoPlayer::isTextToolEditing() const
{
    return m_glPreview && m_glPreview->isTextToolEditing();
}

QString VideoPlayer::currentTextToolInputText() const
{
    return m_glPreview ? m_glPreview->currentTextToolInputText() : QString();
}

void VideoPlayer::commitCurrentTextToolEdit()
{
    if (m_glPreview)
        m_glPreview->commitCurrentTextToolEdit();
}

void VideoPlayer::resetDecoder()
{
    if (m_playbackTimer)
        m_playbackTimer->stop();
    if (m_seekTimer)
        m_seekTimer->stop();

    // Audio lifecycle belongs to AudioMixer now — do NOT touch the mixer
    // here. resetDecoder is called from loadFile() on every video entry
    // boundary; the mixer keeps its own decoder pool keyed by AudioTrackKey
    // and stays in step via setAudioSequence + the master clock.

    m_playing = false;
    m_videoStreamIndex = -1;
    m_hdrInfo = {};
    m_durationUs = 0;
    m_currentPositionUs = 0;
    m_frameDurationUs = 0;
    m_displayAspectRatio = 0.0;
    m_currentFrameImage = QImage();
    m_pendingSeekMs = -1;
    m_pendingSeekPrecise = false;
    m_seekInProgress = false;

    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_swFrame)
        av_frame_free(&m_swFrame);
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_codecCtx)
        avcodec_free_context(&m_codecCtx);
    if (m_formatCtx)
        avformat_close_input(&m_formatCtx);
    if (m_hwDeviceCtx)
        av_buffer_unref(&m_hwDeviceCtx);
    m_hwPixFmt = AV_PIX_FMT_NONE;

    // Drop the cached compositor base. Otherwise the next tick after a
    // clip switch (advanceToEntry → loadFile → resetDecoder → fresh
    // decode) hands the compositor the previous clip's V1 frame as the
    // canvas base, and the user briefly sees the old clip behind any
    // V2+ overlays.
    m_lastV1RawFrame = QImage();

    updatePlayButton();
    if (!m_suppressUiUpdates)
        m_seekBar->setRange(0, 0);
    if (m_glPreview)
        m_glPreview->setDisplayAspectRatio(0.0);
    updatePositionUi();
}

void VideoPlayer::scheduleNextFrame()
{
    if (!m_playing || !m_playbackTimer)
        return;

    const double absSpeed = qMax(0.25, std::abs(m_playbackSpeed));
    const int64_t baseFrameUs = (m_frameDurationUs > 0) ? m_frameDurationUs : (AV_TIME_BASE / 30);
    const int64_t frameIntervalUs = static_cast<int64_t>(baseFrameUs / absSpeed);
    const int frameIntervalMs = qMax(1, static_cast<int>(frameIntervalUs / 1000));
    int intervalMs = frameIntervalMs;

    // Audio-paced scheduling: pace the next frame against the AudioMixer's
    // audible clock (master clock minus OS-buffered samples) so video
    // tracks the playhead the user actually hears. The mixer publishes its
    // clock in TIMELINE microseconds (independent of which entry it's
    // currently sourcing from), so the J-cut/L-cut filePath-guard the old
    // QMediaPlayer path needed is gone — every active video entry can pace
    // off the same single audio clock.
    if (m_mixer && m_mixer->isPlaying() && m_playbackSpeed >= 0.0
        && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
        const int64_t audioTlUs = m_mixer->audibleClockUs();
        const int64_t videoAheadUs = m_timelinePositionUs - audioTlUs;
        const int64_t waitUs = static_cast<int64_t>(videoAheadUs / absSpeed);
        // Floor the interval at half-frame so catchup mode doesn't peg the
        // timer at 1 ms — every tick already runs up to 7 frame decodes
        // (correctVideoDriftAgainstAudioClock + decodeNextFrame), so a 1 ms
        // tick rate saturates one CPU thread without giving the decoder room
        // to actually catch up. Cap at 5 s to keep the UI responsive even
        // if the audio clock has drifted absurdly far behind the video
        // cursor (also avoids signed overflow on the int cast).
        const int floorMs = qMax(1, frameIntervalMs / 2);
        constexpr int64_t kCeilingMs = 5000;
        intervalMs = (waitUs <= 0)
            ? floorMs
            : static_cast<int>(qBound<int64_t>(floorMs, waitUs / 1000, kCeilingMs));
    }
    m_playbackTimer->start(intervalMs);
}

int VideoPlayer::correctVideoDriftAgainstAudioClock()
{
    // Match the AudioMixer master clock against the video timeline cursor.
    // Reverse playback or a missing video entry short-circuits to keep the
    // legacy single-file path untouched. With the mixer publishing
    // timeline-unified microseconds, J-cut/L-cut no longer needs a filePath
    // guard — every active entry shares one clock source.
    if (!m_mixer || m_playbackSpeed < 0.0)
        return 0;
    if (!m_mixer->isPlaying())
        return 0;
    if (m_activeEntry < 0 || m_activeEntry >= m_sequence.size())
        return 0;

    const int64_t audioTlUs = m_mixer->audibleClockUs();
    const int64_t frameUs = (m_frameDurationUs > 0) ? m_frameDurationUs : (AV_TIME_BASE / 30);
    // Threshold > 1.5 frames avoids skipping on natural scheduler jitter.
    const int64_t catchupThresholdUs = (frameUs * 3) / 2;
    const int maxSkips = 6; // bounded so one tick can't block the UI

    int skipped = 0;
    while (skipped < maxSkips) {
        if (audioTlUs - m_timelinePositionUs <= catchupThresholdUs)
            break;
        // Skip-decode WITHOUT displaying — handlePlaybackTick calls
        // decodeNextFrame(true) right after this returns, so the frame the
        // user sees is always the freshest, never a stale skipped one.
        if (!decodeNextFrame(false))
            break;
        ++skipped;
    }
    return skipped;
}

void VideoPlayer::performPendingSeek()
{
    if (m_seekInProgress || m_pendingSeekMs < 0)
        return;

    const bool seqMode = sequenceActive();
    if (!seqMode && (!m_formatCtx || !m_codecCtx)) {
        m_pendingSeekMs = -1;
        return;
    }

    m_seekInProgress = true;

    const bool wasPlaying = m_playing;
    const int requestedMs = m_pendingSeekMs;
    const bool precise = m_pendingSeekPrecise;
    m_pendingSeekMs = -1;
    m_pendingSeekPrecise = false;

    bool seekOk;
    if (seqMode) {
        const int64_t timelineUs = static_cast<int64_t>(requestedMs) * 1000;
        // Preview seeks (non-precise / drag) MUST NOT switch files. Each
        // file switch reloads the FFmpeg video decoder and the user
        // hears the old audio briefly overlapping with the new — perceived
        // as "double playback". Refuse cross-file preview seeks; the final
        // committed seek (sliderReleased / valueChanged) handles the switch.
        if (!precise) {
            const int64_t clampedTimelineUs = qBound<int64_t>(0, timelineUs, m_sequenceDurationUs);
            const int idx = findActiveEntryAt(clampedTimelineUs);
            if (idx >= 0 && idx < m_sequence.size()
                && m_sequence[idx].filePath != m_loadedFilePath
                && m_formatCtx) {
                qInfo() << "VideoPlayer: skipping preview file switch idx=" << idx;
                seekOk = false;
            } else {
                seekOk = seekToTimelineUs(timelineUs, precise);
            }
        } else {
            seekOk = seekToTimelineUs(timelineUs, precise);
        }
    } else {
        const int64_t targetUs = static_cast<int64_t>(requestedMs) * 1000;
        seekOk = seekInternal(targetUs, true, precise);
    }

    m_seekInProgress = false;
    // After a seek completes, V2+ pool decoders may still hold lastFrameRgb
    // and currentPositionUs from before the seek. Ask the next playback tick
    // to clear firstFrameDecoded on every active pool decoder so
    // harvestOverlayLayer drops into its catch-up loop and lands on the
    // correct file-local position for the new playhead.
    m_postSeekResyncRequested = true;

    if (m_pendingSeekMs >= 0) {
        if (m_seekTimer)
            m_seekTimer->start();
        return;
    }

    // NOTE: audio is owned by AudioMixer's master clock — seekToTimelineUs
    // calls m_mixer->seekTo with the timeline position. Do NOT push a
    // setPosition equivalent here; the previous shim used VIDEO file-local
    // coordinates as an AUDIO player position which caused drift after seek.

    if (seekOk && wasPlaying)
        scheduleNextFrame();
}

void VideoPlayer::updatePositionUi()
{
    if (m_suppressUiUpdates) return;
    int64_t displayUs;
    int64_t totalUs;
    if (sequenceActive()) {
        // Reproject the current file-local position into timeline coordinates
        // so the slider, time label and positionChanged signal all speak the
        // same timeline-space the Timeline widget uses.
        if (m_activeEntry >= 0 && m_activeEntry < m_sequence.size())
            m_timelinePositionUs = fileLocalToTimelineUs(m_activeEntry, m_currentPositionUs);
        displayUs = m_timelinePositionUs;
        totalUs = m_sequenceDurationUs;
    } else {
        displayUs = m_currentPositionUs;
        totalUs = m_durationUs;
    }

    const int sliderValue = qMin(sliderTimelinePosition(displayUs), m_seekBar->maximum());
    const QSignalBlocker blocker(m_seekBar);
    if (!m_seekBar->isSliderDown())
        m_seekBar->setValue(sliderValue);
    m_timeLabel->setText(QString("%1 / %2")
        .arg(formatTimestamp(displayUs))
        .arg(formatTimestamp(totalUs)));
    emit positionChanged(static_cast<double>(displayUs) / AV_TIME_BASE);
}

bool VideoPlayer::seekInternal(int64_t positionUs, bool displayFrame, bool precise)
{
    if (!m_formatCtx || !m_codecCtx || m_videoStreamIndex < 0)
        return false;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    const int64_t targetUs = qMax<int64_t>(0, (m_durationUs > 0) ? qMin(positionUs, m_durationUs) : positionUs);
    const int64_t targetTimestamp = streamTimestampForPosition(targetUs);

    if (av_seek_frame(m_formatCtx, m_videoStreamIndex, targetTimestamp, AVSEEK_FLAG_BACKWARD) < 0)
        return false;

    avcodec_flush_buffers(m_codecCtx);
    m_currentPositionUs = targetUs;

    if (!displayFrame) {
        updatePositionUi();
        return true;
    }

    bool foundFrame = false;
    while (decodeNextFrame(false)) {
        foundFrame = true;
        if (!precise || m_currentPositionUs >= targetUs)
            break;
    }

    if (foundFrame && m_frame) {
        AVFrame *displayable = ensureSwFrame(m_frame);
        if (!displayable)
            return false;
        const QImage image = frameToImage(displayable);
        if (image.isNull())
            return false;
        this->displayFrame(image);
        m_currentPositionUs = targetUs;
        updatePositionUi();
        return true;
    }

    updatePositionUi();
    return false;
}

bool VideoPlayer::decodeNextFrame(bool displayFrame)
{
    if (!m_formatCtx || !m_codecCtx || !m_packet || !m_frame)
        return false;

    const auto receiveFrame = [this, displayFrame]() -> bool {
        const int receiveResult = avcodec_receive_frame(m_codecCtx, m_frame);
        if (receiveResult == 0)
            return presentDecodedFrame(m_frame, displayFrame);
        return false;
    };

    if (receiveFrame())
        return true;

    while (av_read_frame(m_formatCtx, m_packet) >= 0) {
        if (m_packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(m_packet);
            continue;
        }

        const int sendResult = avcodec_send_packet(m_codecCtx, m_packet);
        av_packet_unref(m_packet);
        if (sendResult < 0)
            continue;

        if (receiveFrame())
            return true;
    }

    if (avcodec_send_packet(m_codecCtx, nullptr) >= 0 && receiveFrame())
        return true;

    return false;
}

bool VideoPlayer::presentDecodedFrame(AVFrame *frame, bool displayFrameRequested)
{
    int64_t positionUs = m_currentPositionUs;
    const int64_t bestEffortTimestamp =
        (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : frame->pts;

    if (bestEffortTimestamp != AV_NOPTS_VALUE) {
        positionUs = positionFromStreamTimestamp(bestEffortTimestamp);
    } else if (m_frameDurationUs > 0) {
        positionUs += m_frameDurationUs;
    }

    positionUs = qMax<int64_t>(0, positionUs);
    if (m_durationUs > 0)
        positionUs = qMin(positionUs, m_durationUs);
    m_currentPositionUs = positionUs;

    if (displayFrameRequested) {
        AVFrame *displayable = ensureSwFrame(frame);
        if (!displayable)
            return false;
        const QImage image = frameToImage(displayable);
        if (image.isNull())
            return false;
        // Cache the raw V1 source BEFORE the display gate so the Phase 1d
        // compositor in handlePlaybackTick has the latest frame to paint
        // overlays on, even when we defer the displayFrame call below.
        // m_lastV1RawFrame is the dedicated compositor base — it stays a
        // pristine V1 frame regardless of what displayFrame writes back
        // into m_lastSourceFrame later in the pipeline.
        m_lastV1RawFrame = image;
        m_lastSourceFrame = image;
        if (!m_deferDisplayThisTick)
            displayFrame(image);
        updatePositionUi();
    }

    return true;
}

enum AVPixelFormat VideoPlayer::getHwFormatCallback(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts)
{
    auto *self = static_cast<VideoPlayer*>(ctx->opaque);
    if (!self || self->m_hwPixFmt == AV_PIX_FMT_NONE)
        return pixFmts[0];
    for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->m_hwPixFmt)
            return *p;
    }
    qWarning() << "HW pixel format not offered by decoder, falling back to SW";
    return pixFmts[0];
}

AVFrame *VideoPlayer::ensureSwFrame(AVFrame *frame)
{
    if (!frame)
        return nullptr;
    if (m_hwPixFmt == AV_PIX_FMT_NONE || frame->format != m_hwPixFmt)
        return frame;
    if (!m_swFrame)
        return nullptr;

    av_frame_unref(m_swFrame);
    if (av_hwframe_transfer_data(m_swFrame, frame, 0) < 0) {
        qWarning() << "av_hwframe_transfer_data failed";
        return nullptr;
    }
    m_swFrame->pts = frame->pts;
    m_swFrame->best_effort_timestamp = frame->best_effort_timestamp;
    return m_swFrame;
}

QImage VideoPlayer::frameToImage(const AVFrame *frame)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        qWarning() << "frameToImage: invalid frame";
        return {};
    }

    const bool hdr = m_hdrInfo.isHdr;
    const AVPixelFormat dstPixFmt = hdr ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB24;
    const QImage::Format qFmt = hdr ? QImage::Format_RGBA64 : QImage::Format_RGB888;

    m_swsCtx = sws_getCachedContext(
        m_swsCtx,
        frame->width,
        frame->height,
        static_cast<AVPixelFormat>(frame->format),
        frame->width,
        frame->height,
        dstPixFmt,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!m_swsCtx) {
        qWarning() << "frameToImage: sws_getCachedContext failed";
        return {};
    }

    QImage image(frame->width, frame->height, qFmt);
    if (image.isNull()) {
        qWarning() << "frameToImage: QImage alloc failed" << frame->width << "x" << frame->height;
        return {};
    }

    uint8_t *dest[4] = { image.bits(), nullptr, nullptr, nullptr };
    int destLinesize[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
    sws_scale(m_swsCtx, frame->data, frame->linesize, 0, frame->height, dest, destLinesize);
    return image;
}

int64_t VideoPlayer::streamFrameDurationUs() const
{
    if (!m_formatCtx || m_videoStreamIndex < 0)
        return AV_TIME_BASE / 30;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    AVRational frameRate = stream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0)
        frameRate = stream->r_frame_rate;
    if (frameRate.num > 0 && frameRate.den > 0)
        return qMax<int64_t>(1, av_rescale_q(1, av_inv_q(frameRate), AV_TIME_BASE_Q));

    return AV_TIME_BASE / 30;
}

int64_t VideoPlayer::streamTimestampForPosition(int64_t positionUs) const
{
    if (!m_formatCtx || m_videoStreamIndex < 0)
        return positionUs;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    int64_t timestamp = av_rescale_q(positionUs, AV_TIME_BASE_Q, stream->time_base);
    if (stream->start_time != AV_NOPTS_VALUE)
        timestamp += stream->start_time;
    return timestamp;
}

int64_t VideoPlayer::positionFromStreamTimestamp(int64_t timestamp) const
{
    if (!m_formatCtx || m_videoStreamIndex < 0)
        return timestamp;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    if (stream->start_time != AV_NOPTS_VALUE)
        timestamp -= stream->start_time;
    return av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q);
}

int VideoPlayer::sliderPositionForUs(int64_t positionUs) const
{
    const int64_t positionMs = qMax<int64_t>(0, positionUs / 1000);
    return static_cast<int>(qMin<int64_t>(positionMs, std::numeric_limits<int>::max()));
}

double VideoPlayer::streamDisplayAspectRatio() const
{
    if (!m_formatCtx || !m_codecCtx || m_videoStreamIndex < 0 || m_codecCtx->height <= 0)
        return 0.0;

    AVStream *stream = m_formatCtx->streams[m_videoStreamIndex];
    AVRational sampleAspect = av_guess_sample_aspect_ratio(m_formatCtx, stream, nullptr);
    if (sampleAspect.num <= 0 || sampleAspect.den <= 0)
        sampleAspect = stream->sample_aspect_ratio;
    if (sampleAspect.num <= 0 || sampleAspect.den <= 0)
        sampleAspect = m_codecCtx->sample_aspect_ratio;

    double aspectRatio = static_cast<double>(m_codecCtx->width) / m_codecCtx->height;
    if (sampleAspect.num > 0 && sampleAspect.den > 0)
        aspectRatio *= av_q2d(sampleAspect);

    return (aspectRatio > 0.0 && std::isfinite(aspectRatio)) ? aspectRatio : 0.0;
}

double VideoPlayer::effectiveDisplayAspectRatio() const
{
    if (m_displayAspectRatio > 0.0 && std::isfinite(m_displayAspectRatio))
        return m_displayAspectRatio;

    if (!m_currentFrameImage.isNull() && m_currentFrameImage.height() > 0)
        return static_cast<double>(m_currentFrameImage.width()) / m_currentFrameImage.height();

    return 0.0;
}

QSize VideoPlayer::fittedDisplaySize(const QSize &bounds) const
{
    if (!bounds.isValid())
        return QSize(1, 1);

    const double aspectRatio = effectiveDisplayAspectRatio();
    if (!(aspectRatio > 0.0) || !std::isfinite(aspectRatio))
        return bounds;

    int targetWidth = bounds.width();
    int targetHeight = qRound(targetWidth / aspectRatio);
    if (targetHeight > bounds.height()) {
        targetHeight = bounds.height();
        targetWidth = qRound(targetHeight * aspectRatio);
    }

    return QSize(qMax(1, targetWidth), qMax(1, targetHeight));
}

void VideoPlayer::refreshDisplayedFrame()
{
    // Use the raw source cache, NOT m_currentFrameImage — the latter is
    // already composited, so re-running composeFrameWithOverlays on it
    // would burn the overlays in twice (visible as duplicated text when
    // an existing overlay is selected and setTextOverlays re-pushes).
    if (m_lastSourceFrame.isNull())
        return;
    displayFrame(m_lastSourceFrame);
}

void VideoPlayer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    refreshDisplayedFrame();
}

void VideoPlayer::handlePlaybackTick()
{
    if (!m_playing)
        return;

    // Feed the slot manager's distance-from-playhead eviction heuristic.
    // Without this every slot looks equidistant from a stale playhead at
    // 0, so eviction degenerates to "drop the slot with the largest
    // clipStartSec" regardless of what's actually playing.
    m_slotManager.setPlayheadPosition(
        static_cast<double>(m_timelinePositionUs) / static_cast<double>(AV_TIME_BASE));

    // Sweep the eviction grace pool first so any decoder whose graceTtl
    // dropped to 0 last tick is freed before we touch the pool again. Safe
    // here because no decode loop has started this tick.
    tickEvictionGracePool();

    // Post-seek resync: clear firstFrameDecoded on every V2+ pool decoder
    // so harvestOverlayLayer drops into its long catch-up path on the very
    // next call, instead of trusting stale lastFrameRgb from before the
    // seek. Cheap — sets a bool, doesn't free anything.
    if (m_postSeekResyncRequested) {
        for (auto it = m_trackDecoders.begin(); it != m_trackDecoders.end(); ++it) {
            if (it.value())
                it.value()->firstFrameDecoded = false;
        }
        m_postSeekResyncRequested = false;
    }

    // Decide upfront whether the V1 frame for this tick will be composited
    // with V2+ overlays. When yes, presentDecodedFrame caches the V1 frame
    // into m_lastSourceFrame but skips displayFrame so the compositor step
    // below can blend the overlays before pushing the final image.
    const bool willComposite = m_playbackSpeed >= 0.0
                               && sequenceActive()
                               && !m_seekInProgress
                               && hasOverlayActive(findActiveEntriesAt(m_timelinePositionUs));
    m_deferDisplayThisTick = willComposite;

    // When leaving the compositor path, restore the active entry's
    // OBS-style transform on the GL viewport. The compositor resets the
    // viewport to identity (1,0,0) so the canvas-final composite isn't
    // transformed again — but a non-composite tick ships the raw frame
    // straight to GL, so the viewport has to carry the entry's
    // scale/dx/dy or the user's pan/zoom snaps off the moment the
    // overlay ends.
    if (m_lastTickWasComposite && !willComposite && m_glPreview
        && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
        const auto &v1e = m_sequence[m_activeEntry];
        m_glPreview->setVideoSourceTransform(v1e.videoScale, v1e.videoDx, v1e.videoDy);
    }
    m_lastTickWasComposite = willComposite;

    bool advanced = false;
    if (m_playbackSpeed >= 0.0) {
        // Drift correction runs BEFORE the display decode so the frame the
        // user sees this tick is always the freshest one, not the tail of a
        // skip-decode batch. Helper short-circuits on J-cut/L-cut.
        correctVideoDriftAgainstAudioClock();
        advanced = decodeNextFrame(true);
    } else {
        const int64_t stepUs = qMax<int64_t>(1, m_frameDurationUs);
        const int64_t targetUs = qMax<int64_t>(0, m_currentPositionUs - stepUs);
        advanced = seekInternal(targetUs, true, true);
    }
    // Always clear the flag so any subsequent displayFrame call (boundary
    // jump, seekInternal in reverse, etc.) is unguarded.
    m_deferDisplayThisTick = false;

    // Compositor step: paint V2+ overlays on top of the cached V1 frame and
    // push to the GLPreview. Only runs forward, in sequence mode, when not
    // in a preview seek. V1-only timelines short-circuit via hasOverlayActive
    // (overlays.isEmpty -> displayFrame is bypassed entirely on the legacy
    // path because m_deferDisplayThisTick was false above).
    if (advanced && willComposite && !m_lastV1RawFrame.isNull()) {
        const QVector<int> activeIdxs = findActiveEntriesAt(m_timelinePositionUs);
        // GL viewport's setVideoSourceTransform is normally driven by the
        // active entry's videoScale/Dx/Dy in advanceToEntry. When we hand
        // GL a composed canvas-final image we have to (a) bake the
        // entry's transform into the canvas via a base layer, and (b)
        // reset GL viewport to identity so it doesn't apply the same
        // transform a second time on top of the already-baked composite.
        // Build the layer list. The legacy decoder owns whichever entry is
        // currently m_activeEntry (sorted V1-first, so usually V1 — but in
        // a V1-gap V2-active section it can be V2). All other active
        // entries come from the pool via harvestOverlayLayer.
        QVector<DecodedLayer> layers;
        layers.reserve(activeIdxs.size());
        bool overlayPresent = false;
        for (int idx : activeIdxs) {
            if (idx < 0 || idx >= m_sequence.size())
                continue;
            const auto &e = m_sequence[idx];
            DecodedLayer layer;
            if (idx == m_activeEntry) {
                if (m_lastV1RawFrame.isNull())
                    continue;
                layer.rgb = m_lastV1RawFrame;
                layer.isFresh = true;
            } else {
                if (!harvestOverlayLayer(e, idx, &layer))
                    continue;
            }
            layer.opacity = e.opacity;
            layer.videoScale = e.videoScale;
            layer.videoDx = e.videoDx;
            layer.videoDy = e.videoDy;
            layer.sourceTrack = e.sourceTrack;
            layer.sequenceIdx = idx;
            layers.append(layer);
            if (e.sourceTrack > 0)
                overlayPresent = true;
        }
        // V1-wins paint order: higher sourceTrack = back, V1 (sourceTrack 0)
        // = front. Sort DESCENDING by sourceTrack so V_max paints first into
        // the canvas and V1 paints last on top — V1 covers V2 wherever V1
        // has opaque pixels, V2 fills V1's gaps and shows through V1's
        // per-clip opacity.
        std::sort(layers.begin(), layers.end(),
                  [](const DecodedLayer &a, const DecodedLayer &b) {
                      return a.sourceTrack > b.sourceTrack;
                  });
        if (overlayPresent) {
            // Reuse a canvas-sized scratch buffer so we don't allocate
            // ~8MB (1080p ARGB) every tick. Re-allocates only when the
            // canvas size or format changes; otherwise we just refill it
            // with black. composeMultiTrackFrame still promotes through
            // convertToFormat, but starting with a matching format keeps
            // that to a no-op detach.
            if (m_canvasBase.size() != QSize(m_canvasWidth, m_canvasHeight)
                || m_canvasBase.format() != QImage::Format_ARGB32_Premultiplied) {
                m_canvasBase = QImage(m_canvasWidth, m_canvasHeight,
                                      QImage::Format_ARGB32_Premultiplied);
            }
            if (m_canvasBase.isNull()) {
                // Allocation failed (extreme — e.g. canvas not yet known).
                // Fall back to legacy V1-only display, including the
                // viewport restore so V1 keeps its own scale/dx/dy
                // instead of staying on the compositor's identity reset.
                if (m_glPreview && m_activeEntry >= 0
                    && m_activeEntry < m_sequence.size()) {
                    const auto &v1e = m_sequence[m_activeEntry];
                    m_glPreview->setVideoSourceTransform(
                        v1e.videoScale, v1e.videoDx, v1e.videoDy);
                }
                displayFrame(m_lastV1RawFrame);
            } else {
                m_canvasBase.fill(Qt::black);
                const QImage composed = composeMultiTrackFrame(m_canvasBase, layers);
                if (m_glPreview)
                    m_glPreview->setVideoSourceTransform(1.0, 0.0, 0.0);
                displayFrame(composed);
            }
        } else {
            // Every overlay bailed (decoder open failed, slot exhausted).
            // Fall back to the V1-only display, and restore V1's transform
            // on the GL viewport — composite branch sets it to identity to
            // keep the baked transform from being applied twice, but we're
            // back on the raw V1 frame now so V1 has to carry its own
            // transform again or it snaps to identity.
            if (m_glPreview && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()) {
                const auto &v1e = m_sequence[m_activeEntry];
                m_glPreview->setVideoSourceTransform(v1e.videoScale, v1e.videoDx, v1e.videoDy);
            }
            displayFrame(m_lastV1RawFrame);
        }
    }

    // A/V sync: video owns m_currentPositionUs via the FFmpeg decoder;
    // AudioMixer publishes its own master clock. Coupling is audio-as-master
    // via correctVideoDriftAgainstAudioClock + audio-paced scheduleNextFrame.
    // The mixer's clock is timeline-unified, so J-cut/L-cut paths no longer
    // need to fall back to independent clocks.

    // Sequence mode: detect when we've crossed the active entry's outPoint or
    // run off the end of the file, and switch to the next entry.
    if (sequenceActive() && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()
        && m_playbackSpeed >= 0.0) {
        const auto &active = m_sequence[m_activeEntry];
        const int64_t entryEndLocalUs = static_cast<int64_t>(active.clipOut * AV_TIME_BASE);
        const bool reachedEntryEnd = (m_currentPositionUs >= entryEndLocalUs);
        const bool decodeStopped = !advanced;

        if (reachedEntryEnd || decodeStopped) {
            const int nextIdx = m_activeEntry + 1;
            if (nextIdx < m_sequence.size()) {
                if (advanceToEntry(nextIdx)) {
                    updatePositionUi();
                    scheduleNextFrame();
                    return;
                }
            } else {
                // End of sequence.
                m_timelinePositionUs = m_sequenceDurationUs;
                m_currentPositionUs = entryEndLocalUs;
                updatePositionUi();
                pause();
                if (m_mixer) m_mixer->stop();
                return;
            }
        }
    }

    if (!advanced) {
        if (m_playbackSpeed >= 0.0 && m_durationUs > 0) {
            m_currentPositionUs = m_durationUs;
            updatePositionUi();
        }
        pause();
        if (m_mixer)
            m_mixer->stop();
        return;
    }

    // Audio is paced internally by AudioMixer's master clock — no per-tick
    // reposition is needed during natural playback. Cross-entry boundaries
    // and seeks are handled separately via m_mixer->seekTo from the
    // seekToTimelineUs / advanceToEntry call sites.

    scheduleNextFrame();
}

// ---- Per-clip decoder pool ---------------------------------------------------
//
// The active sequence entry (m_activeEntry) is owned by the legacy decoder
// (m_formatCtx/m_codecCtx/...). Every other active entry — V1 included when
// the legacy decoder is sitting on V2 because V2 starts earlier on the
// timeline — comes through this pool. The pool keeps overlay clips' own
// AVFormatContext / AVCodecContext open across short cuts without re-opening
// the file every time the playhead crosses a clip boundary, while
// DecoderSlotManager bounds the number of concurrent HW-decoder sessions on
// the GPU.
//
// Lifecycle invariant (US-2 contract):
//   - Acquire returns a TrackDecoder for the requested entry; refresh path
//     for already-open clips, eviction-with-grace path otherwise.
//   - Eviction does NOT immediately tear down the displaced decoder; it
//     drops to m_evictionGracePool with graceTtlTicks=4 so a stale
//     reference held inside decodeNextFrame / presentDecodedFrame survives
//     long enough to finish its current frame.
//   - tickEvictionGracePool runs from a deferred / next-tick context only
//     (US-3 wiring) — never from inside the decode loop.

VideoPlayer::TrackDecoder *VideoPlayer::acquireDecoderForClip(const PlaybackEntry &entry)
{
    // The active sequence entry (m_activeEntry) is owned by the legacy
    // decoder; the layer-construction loop in handlePlaybackTick gates
    // that case before calling here. Every other active entry — including
    // V1 entries that aren't the active one (Scenario: V2 starts earlier
    // than V1 so m_sequence sorts V2 first; m_activeEntry tracks V2 even
    // while V1 overlaps) — goes through the pool so V1 can still paint
    // last in V1-wins ordering.
    if (entry.filePath.isEmpty())
        return nullptr;

    const TrackKey key{entry.filePath, qRound64(entry.clipIn * 1000.0),
                       entry.sourceTrack, entry.sourceClipIndex};

    // DecoderSlotManager indexes by a single int. Hash the TrackKey down to
    // a 32-bit slot id — qHash gives us a path/clipIn-stable identifier
    // that survives clip reorders (unlike the previous (track<<16 | index)
    // packing which shifted with sourceClipIndex). Hash collisions are
    // possible but vanishingly rare for the small handful of overlay clips
    // we run concurrently; the worst case is one decoder being evicted a
    // little earlier than ideal.
    const int slotClipId = static_cast<int>(qHash(key));

    DecoderSlotManager::SlotRequest req;
    req.clipId = slotClipId;
    // Slot manager treats trackIdx==0 as "the protected V1 slot — never
    // evict". Pool entries are by construction NOT m_activeEntry (the
    // layer-construction loop gates that case), so a V1 entry that lands
    // in the pool is just another overlay layer, not the main timeline
    // video. Map V1's 0 → 1 so the protection logic doesn't pin a pool
    // slot indefinitely; the legacy decoder still owns the primary V1
    // instance when V1 IS m_activeEntry.
    req.trackIdx = (entry.sourceTrack == 0) ? 1 : entry.sourceTrack;
    req.clipStartSec = entry.timelineStart;

    int evictedClipId = -1;
    if (!m_slotManager.requestSlot(req, &evictedClipId)) {
        // Every slot is held by V1 clips — caller must skip rendering this
        // V2+ layer this tick. Should be rare since V1 is single-decoder.
        return nullptr;
    }

    if (evictedClipId != -1) {
        // Look the evicted slot up in the reverse map instead of
        // re-hashing every TrackKey and comparing the hash — re-hash
        // compare evicts the wrong decoder on any hash collision (same
        // int packing for two different keys).
        auto kit = m_slotIdToKey.find(evictedClipId);
        if (kit != m_slotIdToKey.end()) {
            const TrackKey evictedKey = kit.value();
            m_slotIdToKey.erase(kit);
            auto dit = m_trackDecoders.find(evictedKey);
            if (dit != m_trackDecoders.end()) {
                TrackDecoder *evicted = dit.value();
                m_trackDecoders.erase(dit);
                if (evicted) {
                    evicted->graceTtlTicks = 4;
                    m_evictionGracePool.append(evicted);
                }
            }
        }
    }

    // Refresh path: slot manager refreshed an existing entry. Return the
    // already-open decoder unchanged.
    if (auto it = m_trackDecoders.find(key); it != m_trackDecoders.end())
        return it.value();

    // No existing decoder for this (file, clipIn, track) — open one.
    TrackDecoder *fresh = openTrackDecoder(entry);
    if (!fresh) {
        // Open failed: free the slot we just claimed so a future retry can
        // succeed. Pool stays consistent.
        m_slotManager.releaseSlot(slotClipId);
        return nullptr;
    }
    m_trackDecoders.insert(key, fresh);
    m_slotIdToKey.insert(slotClipId, key);
    return fresh;
}

void VideoPlayer::releaseDecoderForClip(const TrackKey &key)
{
    auto it = m_trackDecoders.find(key);
    if (it == m_trackDecoders.end())
        return;
    TrackDecoder *d = it.value();
    m_trackDecoders.erase(it);

    const int slotClipId = static_cast<int>(qHash(key));
    m_slotManager.releaseSlot(slotClipId);
    m_slotIdToKey.remove(slotClipId);

    // Synchronous teardown is OK here: callers reach this path from
    // setSequence reconciliation or explicit clip removal — never from
    // inside decodeNextFrame / presentDecodedFrame.
    if (d) {
        closeTrackDecoder(d);
        delete d;
    }
}

void VideoPlayer::clearAllPoolDecoders()
{
    for (auto it = m_trackDecoders.begin(); it != m_trackDecoders.end(); ++it) {
        TrackDecoder *d = it.value();
        if (!d)
            continue;
        closeTrackDecoder(d);
        delete d;
    }
    m_trackDecoders.clear();
    m_slotIdToKey.clear();

    for (TrackDecoder *d : m_evictionGracePool) {
        if (!d)
            continue;
        closeTrackDecoder(d);
        delete d;
    }
    m_evictionGracePool.clear();

    m_slotManager.clear();
}

void VideoPlayer::tickEvictionGracePool()
{
    // Walk in reverse so removal indices stay valid.
    for (int i = m_evictionGracePool.size() - 1; i >= 0; --i) {
        TrackDecoder *d = m_evictionGracePool[i];
        if (!d) {
            m_evictionGracePool.removeAt(i);
            continue;
        }
        if (d->graceTtlTicks > 0)
            --d->graceTtlTicks;
        if (d->graceTtlTicks <= 0) {
            closeTrackDecoder(d);
            delete d;
            m_evictionGracePool.removeAt(i);
        }
    }
}

VideoPlayer::TrackDecoder *VideoPlayer::openTrackDecoder(const PlaybackEntry &entry)
{
    auto *d = new TrackDecoder();
    d->sourceClipIndex = entry.sourceClipIndex;
    d->sourceTrack = entry.sourceTrack;
    d->filePath = entry.filePath;
    d->clipIn = entry.clipIn;

    const QByteArray pathUtf8 = entry.filePath.toUtf8();
    if (avformat_open_input(&d->formatCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        qWarning() << "openTrackDecoder: avformat_open_input failed for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }
    if (avformat_find_stream_info(d->formatCtx, nullptr) < 0) {
        qWarning() << "openTrackDecoder: avformat_find_stream_info failed for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    d->videoStreamIndex = av_find_best_stream(d->formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->videoStreamIndex < 0) {
        qWarning() << "openTrackDecoder: no video stream in" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    auto *codecpar = d->formatCtx->streams[d->videoStreamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        qWarning() << "openTrackDecoder: unsupported codec for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    d->codecCtx = avcodec_alloc_context3(codec);
    if (!d->codecCtx || avcodec_parameters_to_context(d->codecCtx, codecpar) < 0) {
        qWarning() << "openTrackDecoder: codec context alloc failed for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    // Lazily create the shared HW device context the first time a pool
    // decoder actually needs HW. Subsequent decoders just bump its refcount.
    // V1's m_hwDeviceCtx is intentionally separate so any V2 HW failure
    // never destabilises V1's decoder.
    d->hwPixFmt = AV_PIX_FMT_NONE;
    if (!m_sharedPoolHwDeviceCtx) {
        AVBufferRef *fresh = nullptr;
        if (av_hwdevice_ctx_create(&fresh, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
            m_sharedPoolHwDeviceCtx = fresh;
        }
    }
    if (m_sharedPoolHwDeviceCtx) {
        // Find the codec-specific HW pixel format for D3D11VA.
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                d->hwPixFmt = cfg->pix_fmt;
                break;
            }
        }
        if (d->hwPixFmt != AV_PIX_FMT_NONE) {
            // av_buffer_ref can fail (allocator OOM). Fall back to SW decode
            // instead of trusting a null hw_device_ctx — the codec would
            // otherwise crash inside avcodec_open2 / avcodec_send_packet.
            AVBufferRef *hwRef = av_buffer_ref(m_sharedPoolHwDeviceCtx);
            if (!hwRef) {
                qWarning() << "openTrackDecoder: av_buffer_ref failed, falling back to SW";
                d->hwPixFmt = AV_PIX_FMT_NONE;
            } else {
                d->codecCtx->opaque = d;
                d->codecCtx->get_format = &VideoPlayer::poolGetHwFormatCallback;
                d->codecCtx->hw_device_ctx = hwRef;
            }
        }
    }
    // Software fallback path: leave hwPixFmt = AV_PIX_FMT_NONE, no get_format
    // hook, no hw_device_ctx. Decoder will emit SW frames directly.

    if (avcodec_open2(d->codecCtx, codec, nullptr) < 0) {
        qWarning() << "openTrackDecoder: avcodec_open2 failed for" << entry.filePath;
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->swFrame = av_frame_alloc();
    if (!d->packet || !d->frame || !d->swFrame) {
        qWarning() << "openTrackDecoder: frame/packet alloc failed";
        closeTrackDecoder(d);
        delete d;
        return nullptr;
    }

    // Duration / frame timing. Mirror loadFile's resolution order so the
    // numbers downstream code expects line up.
    if (d->formatCtx->duration > 0) {
        d->durationUs = d->formatCtx->duration;
    } else {
        AVStream *stream = d->formatCtx->streams[d->videoStreamIndex];
        if (stream->duration > 0)
            d->durationUs = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    }

    AVStream *vs = d->formatCtx->streams[d->videoStreamIndex];
    AVRational fr = vs->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = vs->r_frame_rate;
    if (fr.num > 0 && fr.den > 0)
        d->frameDurationUs = av_rescale_q(1, AVRational{fr.den, fr.num}, AV_TIME_BASE_Q);
    if (d->frameDurationUs <= 0)
        d->frameDurationUs = AV_TIME_BASE / 30;

    if (vs->codecpar->width > 0 && vs->codecpar->height > 0) {
        AVRational sar = vs->sample_aspect_ratio;
        double sarVal = (sar.num > 0 && sar.den > 0)
            ? static_cast<double>(sar.num) / sar.den : 1.0;
        d->displayAspect = (static_cast<double>(vs->codecpar->width) * sarVal)
            / static_cast<double>(vs->codecpar->height);
    }

    // Seek to the entry's clipIn so the first decoded frame is meaningful.
    // Pool decoders are demand-decoded by US-3 wiring, but seeding the seek
    // here means the first decoded packet won't be from t=0 of the source
    // file when clipIn > 0.
    const int64_t clipInUs = static_cast<int64_t>(entry.clipIn * AV_TIME_BASE);
    if (clipInUs > 0) {
        av_seek_frame(d->formatCtx, -1, clipInUs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(d->codecCtx);
    }
    d->currentPositionUs = clipInUs;

    // performPendingSeek() may have already set m_postSeekResyncRequested
    // before this fresh decoder existed. The flag is cleared on the next
    // handlePlaybackTick — any decoder opened in that window would
    // otherwise miss the firstFrameDecoded reset and skip its catch-up
    // loop on the very first harvest. Mirror the flag here so the new
    // decoder is treated like every other pool decoder.
    if (m_postSeekResyncRequested) {
        d->firstFrameDecoded = false;
    }

    return d;
}

void VideoPlayer::closeTrackDecoder(TrackDecoder *d)
{
    if (!d)
        return;
    // sws first since it doesn't depend on codec lifetime.
    if (d->swsCtx) {
        sws_freeContext(d->swsCtx);
        d->swsCtx = nullptr;
    }
    if (d->frame)
        av_frame_free(&d->frame);
    if (d->swFrame)
        av_frame_free(&d->swFrame);
    if (d->packet)
        av_packet_free(&d->packet);
    if (d->codecCtx)
        avcodec_free_context(&d->codecCtx);
    if (d->formatCtx)
        avformat_close_input(&d->formatCtx);
    // m_sharedPoolHwDeviceCtx is intentionally NOT touched here. Each pool
    // decoder holds its own av_buffer_ref'd handle through codecCtx; freeing
    // the codec context above already drops that ref. The shared device
    // itself stays alive until ~VideoPlayer.
    d->hwPixFmt = AV_PIX_FMT_NONE;
    d->videoStreamIndex = -1;
    d->firstFrameDecoded = false;
    d->lastFrameRgb = QImage();
    d->lastFramePresentedTimelineUs = -1;
    // Caller owns the heap allocation — closeTrackDecoder only does state
    // cleanup so it can be called from openTrackDecoder's failure path
    // before the pointer is even handed to the caller.
}

enum AVPixelFormat VideoPlayer::poolGetHwFormatCallback(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts)
{
    auto *d = static_cast<TrackDecoder*>(ctx->opaque);
    if (!d || d->hwPixFmt == AV_PIX_FMT_NONE)
        return pixFmts[0];
    for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == d->hwPixFmt)
            return *p;
    }
    qWarning() << "poolGetHwFormatCallback: HW pixel format not offered, falling back to SW";
    return pixFmts[0];
}

// ---- Phase 1d software compositor ------------------------------------------
//
// composeMultiTrackFrame paints overlay layers (V2+) on top of the V1 base
// using QPainter::CompositionMode_SourceOver against an ARGB32_Premultiplied
// canvas. Each layer's videoScale/videoDx/videoDy follows the OBS-style
// transform convention used elsewhere in the player: dx/dy in canvas-relative
// 0..1 units, scale around the canvas center.
//
// V1-only timeline regression safety: when overlayLayers is empty we return
// v1Frame unchanged so the legacy displayFrame path (composeFrameWithOverlays
// for text + GLPreview push) sees the exact same QImage it always saw.

QImage VideoPlayer::composeMultiTrackFrame(const QImage &v1Frame,
                                           const QVector<DecodedLayer> &overlayLayers) const
{
    if (v1Frame.isNull() || overlayLayers.isEmpty())
        return v1Frame;

    // Write canvas-relative dst rects directly (no translate/scale stack)
    // so the geometry matches GLPreview's setVideoSourceTransform 1:1.
    // dx/dy are normalized canvas units (0..1); positive dy moves the
    // layer DOWN visually. QImage Y is top-down so a larger cy paints
    // further down the canvas — and in paintGL positive dy decreases
    // viewportY, which on an OpenGL Y-up framebuffer puts the rendered
    // region nearer the bottom of the screen. Both pipelines therefore
    // agree on the user-facing dy direction with `cy = h/2 + dy * h`.
    // We fill the entire canvas rect with the (scaled) layer image so a
    // 100% opaque base layer paints over the canvas just like GL does.
    QImage composed = v1Frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&composed);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const QSize canvas(composed.width(), composed.height());
    for (const DecodedLayer &L : overlayLayers) {
        if (L.rgb.isNull() || L.opacity <= 0.001)
            continue;
        const double w  = canvas.width()  * L.videoScale;
        const double h  = canvas.height() * L.videoScale;
        const double cx = canvas.width()  * 0.5 + L.videoDx * canvas.width();
        const double cy = canvas.height() * 0.5 + L.videoDy * canvas.height();
        const QRectF dst(cx - w * 0.5, cy - h * 0.5, w, h);
        p.setOpacity(qBound(0.0, L.opacity, 1.0));
        p.drawImage(dst, L.rgb);
    }
    p.end();
    return composed;
}

// decodePoolFrame is decodeNextFrame's V2+ twin: same FFmpeg loop logic but
// runs against a TrackDecoder* instead of the legacy m_formatCtx/m_codecCtx
// pair. It does NOT call displayFrame — the compositor in handlePlaybackTick
// owns the display step. On success the pool decoder's lastFrameRgb /
// currentPositionUs / firstFrameDecoded are updated; on failure (EOF) the
// caller falls back to the previous lastFrameRgb so the overlay stays sticky
// instead of going black at clip ends.
bool VideoPlayer::decodePoolFrame(TrackDecoder *d)
{
    if (!d || !d->formatCtx || !d->codecCtx || !d->packet || !d->frame)
        return false;

    const auto receiveFrame = [this, d]() -> bool {
        // avcodec_receive_frame doesn't always reset the frame before
        // writing — leftover side-data buffers from a prior frame can
        // leak the HW-frame ref it overwrites. Explicit unref makes the
        // contract symmetric with av_read_frame / av_packet_unref.
        av_frame_unref(d->frame);
        const int receiveResult = avcodec_receive_frame(d->codecCtx, d->frame);
        if (receiveResult != 0)
            return false;

        // Compute file-local position in microseconds. We do NOT commit
        // it to d->currentPositionUs here — see the bottom of the lambda.
        // The commit is gated on sws_scale success so any failure path
        // between here and there leaves currentPositionUs at its pre-call
        // value; otherwise a HW-transfer or sws_scale hiccup advances the
        // anchor past the real frame and harvest's drift gate stops being
        // able to detect that the decoder is stuck.
        AVStream *stream = d->formatCtx->streams[d->videoStreamIndex];
        const int64_t bestEffortTimestamp =
            (d->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? d->frame->best_effort_timestamp
                : d->frame->pts;
        int64_t positionUs = d->currentPositionUs;
        if (bestEffortTimestamp != AV_NOPTS_VALUE) {
            int64_t ts = bestEffortTimestamp;
            if (stream->start_time != AV_NOPTS_VALUE)
                ts -= stream->start_time;
            positionUs = av_rescale_q(ts, stream->time_base, AV_TIME_BASE_Q);
        } else if (d->frameDurationUs > 0) {
            positionUs += d->frameDurationUs;
        }
        positionUs = qMax<int64_t>(0, positionUs);
        if (d->durationUs > 0)
            positionUs = qMin(positionUs, d->durationUs);

        // HW->SW transfer if the decoder is on D3D11VA.
        AVFrame *displayable = d->frame;
        if (d->hwPixFmt != AV_PIX_FMT_NONE && d->frame->format == d->hwPixFmt) {
            if (!d->swFrame)
                return false;
            av_frame_unref(d->swFrame);
            if (av_hwframe_transfer_data(d->swFrame, d->frame, 0) < 0) {
                qWarning() << "decodePoolFrame: av_hwframe_transfer_data failed";
                return false;
            }
            d->swFrame->pts = d->frame->pts;
            d->swFrame->best_effort_timestamp = d->frame->best_effort_timestamp;
            displayable = d->swFrame;
        }

        if (!displayable || displayable->width <= 0 || displayable->height <= 0)
            return false;

        // sws_scale into a QImage. Pool stays SDR for the MVP — HDR overlay
        // mixing isn't supported yet (V1 still drives the HDR transfer
        // metadata). RGB24 is fine for SourceOver compositing because we
        // promote to ARGB32_Premultiplied in composeMultiTrackFrame anyway.
        d->swsCtx = sws_getCachedContext(
            d->swsCtx,
            displayable->width, displayable->height,
            static_cast<AVPixelFormat>(displayable->format),
            displayable->width, displayable->height,
            AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!d->swsCtx)
            return false;
        QImage image(displayable->width, displayable->height, QImage::Format_RGB888);
        if (image.isNull())
            return false;
        uint8_t *dest[4]      = { image.bits(), nullptr, nullptr, nullptr };
        int      destLines[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
        sws_scale(d->swsCtx, displayable->data, displayable->linesize, 0,
                  displayable->height, dest, destLines);

        // Commit position + frame only after the full pipeline succeeded.
        d->currentPositionUs = positionUs;
        d->lastFrameRgb = image;
        d->firstFrameDecoded = true;
        return true;
    };

    if (receiveFrame())
        return true;

    while (av_read_frame(d->formatCtx, d->packet) >= 0) {
        if (d->packet->stream_index != d->videoStreamIndex) {
            av_packet_unref(d->packet);
            continue;
        }
        const int sendResult = avcodec_send_packet(d->codecCtx, d->packet);
        av_packet_unref(d->packet);
        if (sendResult < 0)
            continue;
        if (receiveFrame())
            return true;
    }

    if (avcodec_send_packet(d->codecCtx, nullptr) >= 0 && receiveFrame())
        return true;

    return false;
}

bool VideoPlayer::harvestOverlayLayer(const PlaybackEntry &e, int seqIdx, DecodedLayer *out)
{
    if (!out)
        return false;

    TrackDecoder *d = acquireDecoderForClip(e);
    if (!d)
        return false;

    const int64_t expectedFileLocalUs = entryLocalPositionUs(seqIdx, m_timelinePositionUs);
    const int64_t halfFrame = qMax<int64_t>(1, d->frameDurationUs / 2);
    const int64_t drift = expectedFileLocalUs - d->currentPositionUs;

    // Case B: large drift (rewind / non-1x speed mismatch / post-seek). Re-seek.
    if (d->firstFrameDecoded
        && (drift < -halfFrame * 2 || drift > d->frameDurationUs * 4)) {
        const int64_t clipInUs  = static_cast<int64_t>(e.clipIn  * AV_TIME_BASE);
        const int64_t clipOutUs = static_cast<int64_t>(e.clipOut * AV_TIME_BASE);
        const int64_t targetUs = qBound<int64_t>(clipInUs, expectedFileLocalUs, clipOutUs);
        av_seek_frame(d->formatCtx, -1, targetUs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(d->codecCtx);
        d->currentPositionUs = targetUs;
    }

    int catchupCap = d->firstFrameDecoded ? 4 : 8;
    bool ok = false;
    while (catchupCap-- > 0) {
        ok = decodePoolFrame(d);
        if (!ok)
            break;
        if (d->currentPositionUs + halfFrame >= expectedFileLocalUs)
            break;
    }

    if (!d->lastFrameRgb.isNull()) {
        out->rgb     = d->lastFrameRgb;
        out->isFresh = ok;
    } else {
        // Fresh decoder with no successful decode yet — try the eviction
        // grace pool for the same identity. Match the TrackKey contract
        // exactly: sourceTrack + sourceClipIndex + qRound64(clipIn*1000)
        // + filePath. Using qFuzzyCompare on raw doubles here would
        // re-introduce the hash/equality skew that breaks QHash's contract.
        const qint64 eClipMs = qRound64(e.clipIn * 1000.0);
        for (TrackDecoder *g : m_evictionGracePool) {
            if (!g)
                continue;
            if (g->sourceTrack == e.sourceTrack
                && g->sourceClipIndex == e.sourceClipIndex
                && qRound64(g->clipIn * 1000.0) == eClipMs
                && g->filePath == e.filePath
                && !g->lastFrameRgb.isNull()) {
                out->rgb     = g->lastFrameRgb;
                out->isFresh = false;
                break;
            }
        }
        if (out->rgb.isNull())
            return false;
    }

    out->opacity     = e.opacity;
    out->videoScale  = e.videoScale;
    out->videoDx     = e.videoDx;
    out->videoDy     = e.videoDy;
    out->sourceTrack = e.sourceTrack;
    out->sequenceIdx = seqIdx;
    return true;
}

bool VideoPlayer::hasOverlayActive(const QVector<int> &activeIdxs) const
{
    for (int idx : activeIdxs) {
        if (idx >= 0 && idx < m_sequence.size() && m_sequence[idx].sourceTrack > 0)
            return true;
    }
    return false;
}
