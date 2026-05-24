#pragma once

#include <QObject>
#include <QThread>
#include "ExportDialog.h"
#include "Timeline.h"

class SmartReframe;
class SubtitleTrackRenderer;

// ===========================================================================
// LEGACY — bypasses the SSOT edit graph; do not use for new code.
//
// Exporter::doExport is a CPU-only ffmpeg transcode that applies a hard-coded
// subset of effects via applyEffectStack (~11 effect types) and SKIPS the
// graph entirely for the 10-bit/HDR/ProRes path (see Exporter.cpp tenBitPath).
// It does NOT reproduce the GLPreview composite.
//
// Production video export goes RenderQueue -> tlrender::renderFrameAt (S8),
// which renders the FULL preview edit graph pixel-for-pixel. As of S12 every
// UI-reachable export entry point (File->Export Ctrl+E, Mobile Export, Batch
// Export, Render Queue) routes through RenderQueue; Exporter is retained only
// for ABI/source compatibility and is no longer wired to any UI action.
//
// See progress.txt "### S12 single-path audit".
// ===========================================================================

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
