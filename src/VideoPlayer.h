#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QHBoxLayout>
#include <QVBoxLayout>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

class VideoPlayer : public QWidget
{
    Q_OBJECT

public:
    explicit VideoPlayer(QWidget *parent = nullptr);
    ~VideoPlayer();

    void loadFile(const QString &filePath);
    void setCanvasSize(int width, int height);

public slots:
    void play();
    void pause();
    void stop();
    void seek(int position);
    void setPlaybackSpeed(double speed);
    void speedUp();    // L key
    void speedDown();  // J key
    void togglePlay(); // K key

signals:
    void positionChanged(int position);
    void durationChanged(int duration);
    void stateChanged(bool playing);
    void playbackSpeedChanged(double speed);

private:
    void setupUI();
    void updatePlayButton();
    void displayFrame(const QImage &image);

    QLabel *m_videoDisplay;
    QPushButton *m_playButton;
    QPushButton *m_stopButton;
    QSlider *m_seekBar;
    QLabel *m_timeLabel;

    AVFormatContext *m_formatCtx = nullptr;
    AVCodecContext *m_codecCtx = nullptr;
    int m_videoStreamIndex = -1;
    bool m_playing = false;
    int64_t m_duration = 0;
    int m_canvasWidth = 1920;
    int m_canvasHeight = 1080;
    double m_playbackSpeed = 1.0;
};
