#pragma once

#include <QObject>
#include <QThread>
#include "ExportDialog.h"
#include "Timeline.h"

class SmartReframe;
class SubtitleTrackRenderer;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

class Exporter : public QObject
{
    Q_OBJECT

public:
    explicit Exporter(QObject *parent = nullptr);

    void startExport(const ExportConfig &config, const QVector<ClipInfo> &clips);
    void cancel();

    void setSmartReframe(SmartReframe *reframe);
    void setSubtitleRenderer(SubtitleTrackRenderer *renderer);
    void setLoudnessGainDb(double gainDb);

signals:
    void progressChanged(int percent);
    void exportFinished(bool success, const QString &message);

private:
    void doExport(const ExportConfig &config, const QVector<ClipInfo> &clips);
    bool openInputFile(const QString &path, AVFormatContext **fmtCtx, AVCodecContext **decCtx, int *streamIndex);
    bool transcodeClip(const ClipInfo &clip, AVFormatContext *outFmt, AVCodecContext *encCtx,
                       AVStream *outStream, SwsContext *swsCtx, int64_t &pts);

    bool m_cancelled = false;
    QThread *m_thread = nullptr;

    SmartReframe *m_smartReframe = nullptr;
    SubtitleTrackRenderer *m_subtitleRenderer = nullptr;
    double m_loudnessGainDb = 0.0;
};
