#pragma once

#include <QObject>
#include <QThread>
#include <atomic>
#include <optional>
#include "ExportDialog.h"
#include "PremiereXmlExporter.h"
#include "TimecodeBurnIn.h"
#include "Timeline.h"

class SmartReframe;
class SubtitleTrackRenderer;
class QImage;

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

namespace exporterframe {

enum class RgbConversionPath {
    LegacyFixedYuv420P,
    EncoderPixelFormat
};

// TC disabled must retain the pre-burn-in fixed-YUV420P conversion byte path.
// Keeping this decision pure makes the no-op compatibility rule testable.
RgbConversionPath selectRgbConversionPath(bool timecodeBurnInEnabled) noexcept;

// TC-enabled RGB24 -> encoder-frame conversion. The caller owns an allocated
// outputFrame; its format, dimensions, and colour metadata are preserved and
// define the swscale target.
bool convertRgbImageToFrame(const QImage &image, AVFrame *outputFrame,
                            bool configureColorMatrix);

} // namespace exporterframe

namespace exportertimecode {

// Absolute timeline time for a frame in the legacy sequential exporter.
double timelineSeconds(double processedDuration, double leadInSec,
                       double sourceOffsetSec, double speed) noexcept;

} // namespace exportertimecode

class Exporter : public QObject
{
    Q_OBJECT

public:
    explicit Exporter(QObject *parent = nullptr);

    void startExport(const ExportConfig &config, const QVector<ClipInfo> &clips);
    void cancel();

    void setSmartReframe(SmartReframe *reframe);
    void setSubtitleRenderer(SubtitleTrackRenderer *renderer);
    void setTimecodeBurnIn(const TimecodeBurnInSettings &settings);
    void setLoudnessGainDb(double gainDb);

    // Premiere Pro XML (FCP7) export dispatcher.
    // Converts clips to PremiereHighlight list and calls PremiereXmlExporter::generateCombinedXml.
    // Returns true on success; false on failure (caller should show QMessageBox::warning).
    static bool exportAsPremiereXml(const QVector<ClipInfo> &clips,
                                    const ExportConfig &config,
                                    const QString &outputPath,
                                    const QString &projectName = QStringLiteral("v-simple-editor Project"));

signals:
    void progressChanged(int percent);
    void exportFinished(bool success, const QString &message);

private:
    void doExport(const ExportConfig &config, const QVector<ClipInfo> &clips,
                  const std::optional<TimecodeBurnInSettings> &timecodeBurnIn);
    bool openInputFile(const QString &path, AVFormatContext **fmtCtx, AVCodecContext **decCtx, int *streamIndex);
    bool transcodeClip(const ClipInfo &clip, AVFormatContext *outFmt, AVCodecContext *encCtx,
                       AVStream *outStream, SwsContext *swsCtx, int64_t &pts);

    std::atomic_bool m_cancelled = false;
    QThread *m_thread = nullptr;

    SmartReframe *m_smartReframe = nullptr;
    SubtitleTrackRenderer *m_subtitleRenderer = nullptr;
    std::optional<TimecodeBurnInSettings> m_timecodeBurnIn;
    double m_loudnessGainDb = 0.0;
};
