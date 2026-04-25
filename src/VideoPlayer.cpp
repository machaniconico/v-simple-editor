#include "VideoPlayer.h"
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

    m_audioPlayer = new QMediaPlayer(this);
    m_audioOut = new QAudioOutput(this);
    m_audioOut->setVolume(1.0);
    m_audioPlayer->setAudioOutput(m_audioOut);
    connect(m_audioPlayer, &QMediaPlayer::errorOccurred, this,
            [](QMediaPlayer::Error err, const QString &msg) {
                if (err != QMediaPlayer::NoError)
                    qWarning() << "QMediaPlayer error:" << err << msg;
            });
    // Qt MediaFoundation setSource is async — apply deferred position/play
    // when the new source finishes loading. Without this the audio side-player
    // swallows the setPosition+play() issued immediately after setSource and
    // the user hears silence after a file-switching seek or tick boundary.
    connect(m_audioPlayer, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                // Drop pending state if the load failed or the source was
                // cleared — otherwise a broken file would leave the yield
                // guard in applyAudioEntryAtTimeline permanently stuck.
                if (status == QMediaPlayer::InvalidMedia
                    || status == QMediaPlayer::NoMedia) {
                    m_pendingAudioPositionMs = -1;
                    m_pendingAudioPlay = false;
                    return;
                }
                if (status != QMediaPlayer::LoadedMedia
                    && status != QMediaPlayer::BufferedMedia) {
                    return;
                }
                if (m_pendingAudioPositionMs >= 0) {
                    m_audioPlayer->setPosition(m_pendingAudioPositionMs);
                    m_pendingAudioPositionMs = -1;
                }
                if (m_pendingAudioPlay && m_playing && m_playbackSpeed >= 0.0) {
                    m_audioPlayer->play();
                }
                m_pendingAudioPlay = false;
            });
}

VideoPlayer::~VideoPlayer()
{
    qInfo() << "VideoPlayer::dtor";
    // Stop any in-flight timers before decoder teardown so a queued
    // handlePlaybackTick doesn't fire on a half-destroyed object.
    if (m_playbackTimer) m_playbackTimer->stop();
    if (m_seekTimer)     m_seekTimer->stop();
    resetDecoder();
}

void VideoPlayer::setupUI()
{
    auto *layout = new QVBoxLayout(this);

    auto *displayStack = new QStackedWidget(this);

    m_videoDisplay = new QLabel(this);
    m_videoDisplay->setAlignment(Qt::AlignCenter);
    m_videoDisplay->setMinimumSize(800, 450);
    m_videoDisplay->setText("Drop a video file or use File > Open");
    m_videoDisplay->setStyleSheet("background-color: #1a1a1a; color: #888; font-size: 16px;");

    m_glPreview = new GLPreview(this);
    m_glPreview->setMinimumSize(640, 360);
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
    m_playButton = new QPushButton(QString::fromUtf8("\xE2\x96\xB6"), this);
    m_pauseButton = new QPushButton(QString::fromUtf8("\xE2\x8F\xB8"), this);
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
    m_pauseButton->setFixedSize(40, 32);
    m_stopButton->setFixedSize(40, 32);
    m_playButton->setStyleSheet(mediaBtnStyle);
    m_pauseButton->setStyleSheet(mediaBtnStyle);
    m_stopButton->setStyleSheet(mediaBtnStyle);
    m_playButton->setToolTip(QStringLiteral("再生"));
    m_pauseButton->setToolTip(QStringLiteral("一時停止"));
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
    m_proxyButton->setToolTip(QStringLiteral("プレビュー プロキシ解像度 (CPUエフェクト再生時に適用)"));
    connect(m_proxyButton, &QPushButton::clicked, this, [this, proxyLabel]() {
        const int order[] = {1, 2, 4, 8};
        int idx = 0;
        for (int i = 0; i < 4; ++i) if (order[i] == m_proxyDivisor) { idx = i; break; }
        m_proxyDivisor = order[(idx + 1) % 4];
        m_proxyButton->setText(proxyLabel(m_proxyDivisor));
        QSettings("VSimpleEditor", "Preferences").setValue("proxyDivisor", m_proxyDivisor);
        if (!m_lastSourceFrame.isNull())
            displayFrame(m_lastSourceFrame);
    });
    m_timeLabel->setFixedWidth(120);
    m_seekBar->setRange(0, 0);
    m_seekBar->setTracking(false);

    controls->addWidget(m_proxyButton);
    controls->addWidget(m_stepBackButton);
    controls->addWidget(m_playButton);
    controls->addWidget(m_pauseButton);
    controls->addWidget(m_stopButton);
    controls->addWidget(m_stepFwdButton);
    controls->addWidget(m_seekBar);
    controls->addWidget(m_timeLabel);

    layout->addLayout(controls);

    connect(m_playButton, &QPushButton::clicked, this, &VideoPlayer::play);
    connect(m_pauseButton, &QPushButton::clicked, this, &VideoPlayer::pause);
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

void VideoPlayer::loadFile(const QString &filePath)
{
    qInfo() << "VideoPlayer::loadFile BEGIN" << filePath;
    // Audio is now driven by the independent A-track schedule via
    // setAudioSequence/applyAudioEntryAtTimeline. loadFile only reloads the
    // FFmpeg video decoder. No audio side-player touching here — that was
    // needed back when m_audioPlayer was bound to the video file, but since
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

    // NOTE: The audio side-player is now driven by setAudioSequence /
    // applyAudioEntryAtTimeline from the A-track schedule, NOT by the video
    // file currently decoded by this VideoPlayer. We deliberately do not
    // bind m_audioPlayer to filePath here — audio and video are independent
    // timelines so unlinked clips can play a J-cut / L-cut.
    if (m_audioOut)
        m_audioOut->setMuted(false);

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
    // the file is already loaded, skip the seek/setSource entirely so back-to-
    // back sequenceChanged emissions (e.g. video track + audio track both
    // emitting modified() during a single addClip) don't repeatedly disturb
    // the QMediaPlayer side-player and cause audible double playback.
    const bool entryStructurallyChanged = needFileSwitch || (m_activeEntry != desiredIdx);

    // Snapshot playback state BEFORE loadFile+resetDecoder tear down the
    // playback timer and the audio side-player, so we can resurrect them
    // after the file swap completes.
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
    // Audio mute may have changed even when the entry is otherwise unchanged
    // (e.g. user toggled the M button on the active track) — always reapply.
    applyActiveEntryAudioMute();

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
    // applyAudioEntryAtTimeline call inside handlePlaybackTick.

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
    m_audioSequence = entries;
    if (entries.isEmpty()) {
        if (m_audioPlayer) {
            m_audioPlayer->stop();
            m_audioPlayer->setSource(QUrl());
        }
        m_activeAudioEntry = -1;
        m_audioLoadedFilePath.clear();
        return;
    }
    // Pick the entry for the current timeline position and let the
    // filePath comparison decide whether setSource is actually needed.
    // Forcing setSource unconditionally races with Qt MediaFoundation's async
    // source load on Windows: the player accepts setPosition/play() before the
    // new source is ready and the audio ends up silent ("同期を切って配置を
    // ずらすと音が消える"). The filePath check still picks up real file swaps
    // (e.g. A2 fills A1's dragged gap) because the entry at the current time
    // will have a different filePath in that case.
    applyAudioEntryAtTimeline(m_timelinePositionUs, /*forceSourceReload=*/false, /*forceReposition=*/true);
    applyActiveEntryAudioMute();
}

int VideoPlayer::findActiveAudioEntryAt(int64_t timelineUs) const
{
    if (m_audioSequence.isEmpty()) return -1;
    const double tSec = static_cast<double>(timelineUs) / AV_TIME_BASE;
    for (int i = 0; i < m_audioSequence.size(); ++i) {
        const auto &e = m_audioSequence[i];
        if (tSec >= e.timelineStart && tSec < e.timelineEnd)
            return i;
    }
    return -1;
}

void VideoPlayer::applyAudioEntryAtTimeline(int64_t timelineUs, bool forceSourceReload, bool forceReposition)
{
    if (!m_audioPlayer) return;
    if (m_audioSequence.isEmpty()) {
        if (!m_audioLoadedFilePath.isEmpty()) {
            m_audioPlayer->stop();
            m_audioPlayer->setSource(QUrl());
            m_audioLoadedFilePath.clear();
        }
        m_activeAudioEntry = -1;
        return;
    }
    int idx = findActiveAudioEntryAt(timelineUs);
    if (idx < 0) {
        // In a gap — pause without clearing the source. Do NOT force a
        // setSource on the next re-entry: Qt Media Foundation's setSource
        // is async and ignores the immediately-following setPosition, which
        // causes the audio to replay from 0 ("先頭からの音が流れる"). Let
        // setPosition handle re-entry cleanly. Since the audio schedule now
        // covers A1+A2+... with V1-wins resolution, full A-side gaps are
        // rare — they only occur where no audio track has any clip.
        if (m_audioPlayer->playbackState() == QMediaPlayer::PlayingState)
            m_audioPlayer->pause();
        m_activeAudioEntry = -1;
        return;
    }
    const auto &target = m_audioSequence[idx];
    // INVARIANT: m_audioLoadedFilePath MUST always reflect the player's
    // current source. Never clear the source without also clearing this
    // cache, and vice versa — otherwise needSwitch short-circuits and the
    // player sits on a stale/empty source.
    const bool needSwitch = forceSourceReload || (target.filePath != m_audioLoadedFilePath);
    const double tSec = static_cast<double>(timelineUs) / AV_TIME_BASE;
    const double entryLocalSec = target.clipIn + (tSec - target.timelineStart);
    const qint64 posMs = qMax<qint64>(0, static_cast<qint64>(entryLocalSec * 1000.0));
    if (needSwitch) {
        // Source switch — stash position/play and let mediaStatusChanged
        // apply them after Qt MF finishes the async load. Issuing setPosition
        // + play() immediately here races with the async loader and leaves
        // the side-player silent on Windows.
        m_pendingAudioPositionMs = posMs;
        m_pendingAudioPlay = (m_playing && m_playbackSpeed >= 0.0);
        m_audioPlayer->setSource(QUrl::fromLocalFile(target.filePath));
        m_audioLoadedFilePath = target.filePath;
        m_activeAudioEntry = idx;
        return;
    }
    // forceReposition is set from seek / boundary-jump call sites so a seek
    // landing inside the SAME audio entry still updates the player position.
    // Without it, seek-within-entry silently left the QMediaPlayer clock at
    // its old position — which caused the "audio drops out after seek" report.
    if (idx != m_activeAudioEntry || forceReposition) {
        m_audioPlayer->setPosition(posMs);
    }
    m_activeAudioEntry = idx;
    // Don't fight the pending-load path — if a setSource is still in flight
    // the mediaStatusChanged handler owns the play() call.
    if (m_pendingAudioPositionMs >= 0 || m_pendingAudioPlay) {
        return;
    }
    if (m_playing && m_playbackSpeed >= 0.0
        && m_audioPlayer->playbackState() != QMediaPlayer::PlayingState) {
        m_audioPlayer->play();
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

void VideoPlayer::applyActiveEntryAudioMute()
{
    // With the audio schedule owning the audio channel, mute state is
    // derived from the active AUDIO entry (m_audioSequence[m_activeAudioEntry]),
    // not the video entry. A track mute toggle flows through audioMuted on
    // the PlaybackEntry and lands here.
    if (!m_audioOut) return;
    bool muted = false;
    if (audioSequenceActive() && m_activeAudioEntry >= 0
        && m_activeAudioEntry < m_audioSequence.size()) {
        muted = m_audioSequence[m_activeAudioEntry].audioMuted;
    } else if (!audioSequenceActive()) {
        // No audio schedule at all — silence the side-player.
        muted = true;
    }
    // Reverse playback already mutes audio elsewhere; don't fight that here.
    if (m_playbackSpeed < 0.0)
        muted = true;
    m_audioOut->setMuted(muted);
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
        loadFile(e.filePath); // sets m_audioPlayer source as part of init
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
    applyActiveEntryAudioMute();
    m_suppressUiUpdates = prevSuppress;

    // resetDecoder (called from loadFile when needSwitch) ran updatePlayButton
    // with m_playing=false. Resurrect the video playback timer.
    if (wasPlayingOuter && needSwitch) {
        scheduleNextFrame();
    }
    // Audio stays in step via the audio schedule path. forceReposition=true
    // because this is a user-initiated seek — we WANT to update the audio
    // player position even when the entry index didn't change.
    applyAudioEntryAtTimeline(timelineUs, /*forceSourceReload=*/false, /*forceReposition=*/true);
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

    // Audio is driven by the independent A-track schedule — no direct
    // m_audioPlayer ops here any more. Re-apply the audio schedule entry for
    // the new timeline position so the A-side stays in step.
    m_timelinePositionUs = static_cast<int64_t>(next.timelineStart * AV_TIME_BASE);
    // Hard entry jump — always reposition the audio player so it starts at
    // the new entry's local position even if the audio schedule stayed on
    // the same entry index.
    applyAudioEntryAtTimeline(m_timelinePositionUs, /*forceSourceReload=*/false, /*forceReposition=*/true);
    applyActiveEntryAudioMute();

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

    // Kick the audio side-player via the independent audio schedule. If no
    // audio schedule is set (legacy single-file mode or the user has no A
    // clips), applyAudioEntryAtTimeline short-circuits and we stay silent —
    // which is the correct new behaviour now that audio is owned by A-track.
    // forceReposition=true so audio is aligned with the current playhead
    // before resuming (otherwise a previous pause-then-seek can leave the
    // player reading from a stale QMediaPlayer position).
    applyAudioEntryAtTimeline(m_timelinePositionUs, /*forceSourceReload=*/false, /*forceReposition=*/true);
    if (m_audioOut)
        m_audioOut->setMuted(false);
    applyActiveEntryAudioMute();
}

void VideoPlayer::pause()
{
    if (m_playbackTimer)
        m_playbackTimer->stop();

    m_playing = false;
    updatePlayButton();
    emit stateChanged(false);

    if (m_audioPlayer)
        m_audioPlayer->pause();
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
    if (m_audioPlayer) {
        m_audioPlayer->stop();
        m_audioPlayer->setPosition(0);
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

    const double absSpeed = qBound(0.25, std::abs(speed), 16.0);
    m_playbackSpeed = (speed < 0.0) ? -absSpeed : absSpeed;
    emit playbackSpeedChanged(m_playbackSpeed);

    if (m_playing)
        scheduleNextFrame();

    if (m_audioPlayer) {
        if (m_playbackSpeed < 0.0) {
            if (m_audioOut)
                m_audioOut->setMuted(true);
            m_audioPlayer->pause();
        } else {
            if (m_audioOut)
                m_audioOut->setMuted(false);
            m_audioPlayer->setPlaybackRate(absSpeed);
            if (m_playing && m_audioPlayer->source().isValid())
                m_audioPlayer->play();
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
    // With separate Play / Pause / Stop buttons, the icons no longer toggle.
    // Instead enable/disable the buttons that don't apply to the current state
    // so the user gets a visual hint about what's actionable.
    if (m_playButton)  m_playButton->setEnabled(!m_playing);
    if (m_pauseButton) m_pauseButton->setEnabled(m_playing);
    if (m_stopButton)  m_stopButton->setEnabled(true);
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

    // Audio lifecycle belongs to the A-track schedule now — do NOT touch
    // m_audioPlayer here. resetDecoder is called from loadFile() on every
    // video entry boundary, and wiping the audio source at those boundaries
    // desynchronises it from m_audioLoadedFilePath: the next
    // applyAudioEntryAtTimeline sees target.filePath == m_audioLoadedFilePath,
    // skips setSource, and the player stays on the empty source → silence
    // inside the shifted A1 range ("A1 の上で音が出ない"). The audio
    // side-player is cleaned up via setAudioSequence(empty) and in the
    // destructor when the QObject parent tears it down.

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
    int intervalMs = qMax(1, static_cast<int>(frameIntervalUs / 1000));

    // Audio-paced scheduling: pace the next frame against the audio clock
    // when the audio side-player is reading the same file as the active
    // video entry. The naive "frameIntervalUs from now" interval accumulates
    // decode latency into progressive drift on long clips, and also lets
    // video run ahead of audio when QMediaPlayer's async setSource starts
    // late. J-cut / L-cut falls through to the naive interval via the
    // filePath guard so unlinked-clip playback is untouched.
    if (m_audioPlayer && m_playbackSpeed >= 0.0
        && m_audioPlayer->playbackState() == QMediaPlayer::PlayingState
        && m_pendingAudioPositionMs < 0 && !m_pendingAudioPlay
        && m_activeEntry >= 0 && m_activeEntry < m_sequence.size()
        && m_activeAudioEntry >= 0 && m_activeAudioEntry < m_audioSequence.size()
        && m_sequence[m_activeEntry].filePath == m_audioSequence[m_activeAudioEntry].filePath) {
        const int64_t audioPosUs = static_cast<int64_t>(m_audioPlayer->position()) * 1000;
        const int64_t videoAheadUs = m_currentPositionUs - audioPosUs;
        const int64_t waitUs = static_cast<int64_t>(videoAheadUs / absSpeed);
        // waitUs <= 0: video is behind audio — fire next tick ASAP so
        // correctVideoDriftAgainstAudioClock can catch up. waitUs > 0: video
        // is ahead — wait so audio can catch up.
        intervalMs = (waitUs <= 0) ? 1 : qMax(1, static_cast<int>(waitUs / 1000));
    }
    m_playbackTimer->start(intervalMs);
}

int VideoPlayer::correctVideoDriftAgainstAudioClock()
{
    // Same guards as the audio-paced branch in scheduleNextFrame — the two
    // layers activate/deactivate in lockstep. Reverse playback, a pending
    // async setSource, a J-cut/L-cut pair, or missing entries all short-
    // circuit so unlinked-clip playback is untouched.
    if (!m_audioPlayer || m_playbackSpeed < 0.0)
        return 0;
    if (m_audioPlayer->playbackState() != QMediaPlayer::PlayingState)
        return 0;
    if (m_pendingAudioPositionMs >= 0 || m_pendingAudioPlay)
        return 0;
    if (m_activeEntry < 0 || m_activeEntry >= m_sequence.size())
        return 0;
    if (m_activeAudioEntry < 0 || m_activeAudioEntry >= m_audioSequence.size())
        return 0;
    if (m_sequence[m_activeEntry].filePath != m_audioSequence[m_activeAudioEntry].filePath)
        return 0;

    const int64_t audioPosUs = static_cast<int64_t>(m_audioPlayer->position()) * 1000;
    const int64_t frameUs = (m_frameDurationUs > 0) ? m_frameDurationUs : (AV_TIME_BASE / 30);
    // Threshold > 1.5 frames avoids skipping on natural scheduler jitter.
    const int64_t catchupThresholdUs = (frameUs * 3) / 2;
    const int maxSkips = 6; // bounded so one tick can't block the UI

    int skipped = 0;
    while (skipped < maxSkips) {
        if (audioPosUs - m_currentPositionUs <= catchupThresholdUs)
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
        // file switch tears down the QMediaPlayer side-player and the user
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

    if (m_pendingSeekMs >= 0) {
        if (m_seekTimer)
            m_seekTimer->start();
        return;
    }

    // NOTE: audio side is already repositioned by applyAudioEntryAtTimeline
    // inside seekToTimelineUs (forceReposition=true). The old direct
    // m_audioPlayer->setPosition(m_currentPositionUs / 1000) call here was a
    // legacy single-file-mode shim — in sequence mode it used VIDEO
    // file-local coordinates to set the AUDIO player position, which was
    // wrong and caused audible drift after seek. Do not reintroduce it.

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

    // A/V sync: INDEPENDENT schedules by default (video owns
    // m_currentPositionUs via the FFmpeg decoder; audio owns the QMediaPlayer
    // clock via the A-track schedule), coupled to the audio clock as master
    // when the audio side-player and active video entry resolve to the same
    // file. The coupling is implemented by correctVideoDriftAgainstAudioClock
    // above and by audio-paced scheduleNextFrame. J-cut / L-cut falls back
    // to fully independent clocks so unlinked clip playback keeps working.

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
                if (m_audioPlayer) m_audioPlayer->stop();
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
        if (m_audioPlayer)
            m_audioPlayer->stop();
        return;
    }

    // Keep the audio side aligned with the current timeline position —
    // cheap when nothing changed, switches source when we've crossed an
    // audio entry boundary. forceReposition=false: natural tick flow, don't
    // spam setPosition every frame.
    applyAudioEntryAtTimeline(m_timelinePositionUs, /*forceSourceReload=*/false, /*forceReposition=*/false);

    scheduleNextFrame();
}
