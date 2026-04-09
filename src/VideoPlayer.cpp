#include "VideoPlayer.h"
#include "GLPreview.h"
#include <QTime>
#include <QStackedWidget>

VideoPlayer::VideoPlayer(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
}

VideoPlayer::~VideoPlayer()
{
    if (m_codecCtx)
        avcodec_free_context(&m_codecCtx);
    if (m_formatCtx)
        avformat_close_input(&m_formatCtx);
}

void VideoPlayer::setupUI()
{
    auto *layout = new QVBoxLayout(this);

    auto *displayStack = new QStackedWidget(this);

    m_videoDisplay = new QLabel(this);
    m_videoDisplay->setAlignment(Qt::AlignCenter);
    m_videoDisplay->setMinimumSize(640, 360);
    m_videoDisplay->setText("Drop a video file or use File > Open");
    m_videoDisplay->setStyleSheet("background-color: #1a1a1a; color: #888; font-size: 16px;");

    m_glPreview = new GLPreview(this);
    m_glPreview->setMinimumSize(640, 360);

    displayStack->addWidget(m_videoDisplay); // index 0: software
    displayStack->addWidget(m_glPreview);    // index 1: GL
    displayStack->setCurrentIndex(m_useGL ? 1 : 0);

    layout->addWidget(displayStack, 1);

    auto *controls = new QHBoxLayout();

    m_playButton = new QPushButton("Play", this);
    m_stopButton = new QPushButton("Stop", this);
    m_seekBar = new QSlider(Qt::Horizontal, this);
    m_timeLabel = new QLabel("00:00 / 00:00", this);

    m_playButton->setFixedWidth(60);
    m_stopButton->setFixedWidth(60);
    m_timeLabel->setFixedWidth(120);
    m_seekBar->setRange(0, 0);

    controls->addWidget(m_playButton);
    controls->addWidget(m_stopButton);
    controls->addWidget(m_seekBar);
    controls->addWidget(m_timeLabel);

    layout->addLayout(controls);

    connect(m_playButton, &QPushButton::clicked, this, [this]() {
        if (m_playing) pause(); else play();
    });
    connect(m_stopButton, &QPushButton::clicked, this, &VideoPlayer::stop);
    connect(m_seekBar, &QSlider::sliderMoved, this, &VideoPlayer::seek);
}

void VideoPlayer::loadFile(const QString &filePath)
{
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }

    if (avformat_open_input(&m_formatCtx, filePath.toUtf8().constData(), nullptr, nullptr) != 0) {
        m_videoDisplay->setText("Failed to open file");
        return;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        m_videoDisplay->setText("Failed to read stream info");
        return;
    }

    m_videoStreamIndex = -1;
    for (unsigned i = 0; i < m_formatCtx->nb_streams; i++) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (m_videoStreamIndex < 0) {
        m_videoDisplay->setText("No video stream found");
        return;
    }

    auto *codecpar = m_formatCtx->streams[m_videoStreamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        m_videoDisplay->setText("Unsupported codec");
        return;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, codecpar);
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        m_videoDisplay->setText("Failed to open codec");
        return;
    }

    m_duration = m_formatCtx->duration / AV_TIME_BASE;
    m_seekBar->setRange(0, static_cast<int>(m_duration));
    emit durationChanged(static_cast<int>(m_duration));

    // Decode and display first frame
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    while (av_read_frame(m_formatCtx, packet) >= 0) {
        if (packet->stream_index == m_videoStreamIndex) {
            if (avcodec_send_packet(m_codecCtx, packet) == 0) {
                if (avcodec_receive_frame(m_codecCtx, frame) == 0) {
                    SwsContext *swsCtx = sws_getContext(
                        frame->width, frame->height, m_codecCtx->pix_fmt,
                        frame->width, frame->height, AV_PIX_FMT_RGB24,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);

                    QImage image(frame->width, frame->height, QImage::Format_RGB888);
                    uint8_t *dest[1] = { image.bits() };
                    int destLinesize[1] = { static_cast<int>(image.bytesPerLine()) };
                    sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height, dest, destLinesize);
                    sws_freeContext(swsCtx);

                    displayFrame(image);
                    av_packet_unref(packet);
                    break;
                }
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    QTime total(0, 0);
    total = total.addSecs(static_cast<int>(m_duration));
    m_timeLabel->setText("00:00 / " + total.toString("mm:ss"));
}

void VideoPlayer::setCanvasSize(int width, int height)
{
    m_canvasWidth = width;
    m_canvasHeight = height;
    double ar = static_cast<double>(width) / height;
    // Adjust display aspect ratio
    int displayH = m_videoDisplay->width() / ar;
    m_videoDisplay->setMinimumSize(
        qMin(640, width / 2),
        qMin(360, height / 2));
    QString orientation = (ar > 1.0) ? "Landscape" : (ar < 1.0) ? "Portrait" : "Square";
    m_videoDisplay->setText(QString("%1x%2 %3\nDrop a video file or use File > Open")
        .arg(width).arg(height).arg(orientation));
}

void VideoPlayer::play()
{
    m_playing = true;
    updatePlayButton();
    emit stateChanged(true);
    // TODO: Start decode/render timer loop
}

void VideoPlayer::pause()
{
    m_playing = false;
    updatePlayButton();
    emit stateChanged(false);
}

void VideoPlayer::stop()
{
    m_playing = false;
    updatePlayButton();
    m_seekBar->setValue(0);
    emit stateChanged(false);
    emit positionChanged(0);
}

void VideoPlayer::seek(int position)
{
    if (!m_formatCtx) return;
    int64_t timestamp = static_cast<int64_t>(position) * AV_TIME_BASE;
    av_seek_frame(m_formatCtx, -1, timestamp, AVSEEK_FLAG_BACKWARD);
    emit positionChanged(position);
}

void VideoPlayer::setPlaybackSpeed(double speed)
{
    m_playbackSpeed = qMax(0.25, qMin(16.0, speed));
    emit playbackSpeedChanged(m_playbackSpeed);
}

void VideoPlayer::speedUp()
{
    // L key: increase speed (1x -> 2x -> 4x -> 8x -> 16x), or if paused/reverse start forward
    if (!m_playing) { play(); m_playbackSpeed = 1.0; }
    else if (m_playbackSpeed < 0) m_playbackSpeed = 1.0;
    else if (m_playbackSpeed < 16.0) m_playbackSpeed *= 2.0;
    setPlaybackSpeed(m_playbackSpeed);
}

void VideoPlayer::speedDown()
{
    // J key: decrease/reverse speed (1x -> -1x -> -2x -> -4x -> -8x -> -16x)
    if (!m_playing) { m_playbackSpeed = -1.0; play(); }
    else if (m_playbackSpeed > 0) m_playbackSpeed = -1.0;
    else if (m_playbackSpeed > -16.0) m_playbackSpeed *= 2.0;
    setPlaybackSpeed(m_playbackSpeed);
}

void VideoPlayer::togglePlay()
{
    // K key: toggle play/pause, reset speed to 1x
    if (m_playing) {
        pause();
    } else {
        m_playbackSpeed = 1.0;
        play();
    }
    setPlaybackSpeed(m_playbackSpeed);
}

void VideoPlayer::updatePlayButton()
{
    m_playButton->setText(m_playing ? "Pause" : "Play");
}

void VideoPlayer::displayFrame(const QImage &image)
{
    if (m_useGL && m_glPreview) {
        m_glPreview->displayFrame(image);
    } else {
        QPixmap pixmap = QPixmap::fromImage(image);
        m_videoDisplay->setPixmap(pixmap.scaled(m_videoDisplay->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void VideoPlayer::setColorCorrection(const ColorCorrection &cc)
{
    if (m_glPreview)
        m_glPreview->setColorCorrection(cc);
}

void VideoPlayer::setGLAcceleration(bool enabled)
{
    m_useGL = enabled;
    auto *stack = qobject_cast<QStackedWidget*>(m_videoDisplay->parentWidget());
    if (stack)
        stack->setCurrentIndex(enabled ? 1 : 0);
}
