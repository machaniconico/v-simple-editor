#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QDateTime>
#include <QJsonObject>
#include <QMutex>

#include "AcesColor.h"  // AC: ACES カラーマネジメント パイプライン (production export 配線)

class QThread;
class QProcess;
class Timeline;

enum class RenderJobStatus {
    Pending,
    Rendering,
    Completed,
    Failed,
    Cancelled
};

// Flat-field RenderJob — Premiere Media Encoder / Resolve Deliver style.
// `uuid` is a stable QString id for the spec API (RenderQueueDialog uses it);
// the legacy `id` int field is preserved so existing MainWindow code keeps
// compiling unchanged.
struct RenderJob {
    int id = 0;                  // legacy int id (preserved for MainWindow)
    QString uuid;                // spec API id (QString)
    QString name;
    QString projectFilePath;
    QString outputPath;

    // Preset description + flat encoder params (Render Queue dialog uses these).
    QString preset;              // e.g. "1080p H.264 / 50 Mbps"
    int width = 1920;
    int height = 1080;
    QString codec = "h264";      // "h264" | "hevc" | "av1" | "prores"
    int bitrateBps = 50000000;
    qint64 startUs = 0;          // timeline range start
    qint64 endUs = 0;            // timeline range end (0 = whole timeline)
    int passes = 1;              // 1 or 2 (2-pass VBR)

    // Live state.
    RenderJobStatus status = RenderJobStatus::Pending;  // legacy enum (MainWindow reads this)
    int progressPercent = 0;
    QString error;

    // Legacy fields (still populated for backwards compatibility).
    QJsonObject exportConfig;
    double loudnessGainDb = 0.0;
    QString dolbyVisionXml;
    int progress = 0;
    QDateTime startTime;
    QDateTime endTime;
    QString errorMessage;

    // S8 (NLE-parity): additive, NON-persisted in-memory render seam. When a
    // caller already holds a live edit-graph Timeline (the parity selftest, or
    // any in-process exporter), it can point this at it so RenderQueue renders
    // THAT timeline through tlrender::renderFrameAt directly. Mirrors the exact
    // additive pattern ClipInfo::lutFilePath / ClipInfo::maskSystem use
    // (Timeline.h:108-138): saveQueue/loadQueue and the JSON round-trip never
    // touch it. When null (every production / dialog caller — RenderQueueDialog
    // leaves it unset), RenderQueue loads `projectFilePath` into a Timeline via
    // the genuine ProjectFile::load + Timeline::restoreFromProject path. NOT
    // owned by RenderJob; the caller guarantees the pointee outlives the job.
    Timeline *timeline = nullptr;

    // Convenience accessor — returns the spec's lowercase status string.
    QString statusString() const {
        switch (status) {
        case RenderJobStatus::Pending:   return QStringLiteral("pending");
        case RenderJobStatus::Rendering: return QStringLiteral("running");
        case RenderJobStatus::Completed: return QStringLiteral("completed");
        case RenderJobStatus::Failed:    return QStringLiteral("failed");
        case RenderJobStatus::Cancelled: return QStringLiteral("cancelled");
        }
        return QStringLiteral("pending");
    }
};

// Built-in preset descriptor — RenderQueueDialog populates the catalogue
// from RenderQueue::availablePresets().
struct RenderPreset {
    QString name;
    int width;
    int height;
    QString codec;       // "h264" | "hevc" | "av1" | "prores"
    int bitrateBps;
    QString container;   // "mp4" | "mov" | "mkv" | "webm"
};

class RenderQueue : public QObject
{
    Q_OBJECT

public:
    explicit RenderQueue(QObject *parent = nullptr);
    ~RenderQueue();

    // Spec API — preferred for new code (uuid-keyed).
    void addJob(const RenderJob &job);
    void removeJob(const QString &uuid);
    void clear();
    QVector<RenderJob> jobs() const { return m_jobs; }
    bool isRunning() const { return m_running; }
    void start();   // process pending jobs sequentially
    void stop();    // cancel current and stop queue

    // AC: production export 経路 (RenderQueue→tlrender::renderFrameAt) に適用する
    // ACES カラーマネジメント パイプラインを設定する。MainWindow の SSOT
    // m_acesPipeline を start() 前に push する想定 (UI スレッドから呼ばれる)。
    // enabled=false (既定) のときレンダーワーカーは一切適用せずビット同一。
    void setAcesPipeline(const aces::AcesPipeline &p);
    void setLoudnessGainDb(double gainDb);

    static QStringList buildLoudnessAudioFilterArgs(double gainDb);

    static QVector<RenderPreset> availablePresets();
    static RenderJob jobFromPreset(const RenderPreset &preset,
                                   const QString &outputPath,
                                   qint64 startUs = 0,
                                   qint64 endUs = 0);

    // Legacy API — kept so existing callers compile unchanged.
    int addJob(const QString &name, const QString &projectFilePath,
               const QString &outputPath, const QJsonObject &exportConfig);
    void removeJob(int id);
    void clearCompleted();
    void clearAll();

    void startQueue();
    void pauseQueue();
    void resumeQueue();
    void cancelCurrent();

    const RenderJob *currentJob() const;
    int pendingCount() const;
    int completedCount() const;

    void moveJobUp(int id);
    void moveJobDown(int id);
    void moveJobUpUuid(const QString &uuid);
    void moveJobDownUuid(const QString &uuid);

    int totalEstimatedTime() const;

    bool saveQueue(const QString &filePath) const;
    bool loadQueue(const QString &filePath);

signals:
    // Spec signals (QString uuid keyed).
    void jobsChanged();
    void jobProgressUuid(QString uuid, int percent);
    void jobCompletedUuid(QString uuid, bool success, QString error);
    void allCompleted();

    // Legacy signals (still emitted alongside the spec signals).
    void jobStarted(int id);
    void jobProgress(int id, int percent);
    void jobCompleted(int id);
    void jobFailed(int id, const QString &error);
    void queueFinished();
    void queueProgress(int overallPercent);

private:
    void startNextJob();
    // S8/US-MF-5: the SSOT render-pipe. Renders every output frame of `job`
    // through tlrender::renderFrameAt and feeds flattened RGB24 frames into
    // libavcore::FrameEncoder in-process. Runs on a worker QThread so the
    // queue stays responsive; emits jobProgress per frame and finishes via
    // finishCurrentJob().
    //
    // US-MF-6 / US-B3-7: startRenderPipe is a 3-way dispatcher.
    //   1) 8-bit jobs (H.264 / H.265 / ProRes / AV1) → in-process
    //      libavcore::FrameEncoder path (unchanged from US-MF-5).
    //   2) 10-bit HDR (HDR10 / HLG) AND the loaded avcodec DLL exposes a
    //      10-bit HEVC encoder (libavcore::tenBitHevcEncoderAvailable
    //      returns true) → in-process FrameEncoder path with
    //      videoCodecName overridden to libavcore::firstTenBitHevcEncoder()
    //      and isHdr10/isHlg + hdrMaster* populated. FrameEncoder writes
    //      a genuine 10-bit HEVC yuv420p10le stream with BT.2020/PQ|HLG
    //      colour signalling.
    //   3) 10-bit HDR but the DLL has no 10-bit HEVC encoder (the bundled
    //      avcodec-62.dll: libx264/libx265 absent, h264_mf/hevc_mf 8-bit
    //      only) → startRenderPipeSubprocess fallback (ffmpeg.exe QProcess
    //      encode, rawvideo rgb24 stdin → libx265 yuv420p10le + the genuine
    //      BT.2020/PQ|HLG HDR metadata). Drop-in replacing the bundled DLL
    //      with a libx265-enabled build flips HDR jobs to the in-process
    //      path automatically — no recompile needed.
    void startRenderPipe(int jobIndex);
    // US-MF-6 / US-B3-7: ffmpeg.exe subprocess encode for 10-bit HDR
    // (HDR10 / HLG) jobs — reached only when no 10-bit HEVC encoder is
    // available in the loaded avcodec DLL. A worker QThread renders every
    // frame via tlrender::renderFrameAt and streams flattened rgb24 to
    // ffmpeg's stdin; ffmpeg encodes libx265 main10 yuv420p10le with the
    // HDR10/HLG colour signalling + master-display / MaxCLL params (sourced
    // from libavcore::hdr10MasterDisplayString — the SSOT shared with the
    // in-process FrameEncoder), muxing the source audio. Finishes via
    // finishCurrentJob().
    void startRenderPipeSubprocess(int jobIndex);
    void finishCurrentJob(bool success, const QString &errorMsg);
    int findJobIndex(int id) const;
    int findJobIndexByUuid(const QString &uuid) const;
    static QString mapCodecToEncoderName(const QString &codec);
    static QString defaultContainerFor(const QString &codec);
    // US-MF-6 / US-B3-7: locate the ffmpeg.exe binary for the HDR subprocess
    // fallback branch. PATH first, then app dir, %LOCALAPPDATA%\Microsoft\
    // WinGet\Links, C:\ffmpeg\bin, and the macOS/Linux install dirs.
    static QString findFFmpegBinary();
    // Resolve the Timeline a job renders: the in-memory job.timeline seam if
    // set, otherwise ProjectFile::load(projectFilePath) -> a freshly built
    // Timeline via Timeline::restoreFromProject. `*ownedOut` receives a
    // heap Timeline the caller must delete (only when loaded from file);
    // null when job.timeline was used.
    static Timeline *resolveTimeline(const RenderJob &job, Timeline **ownedOut);

    QVector<RenderJob> m_jobs;
    int m_nextId = 1;
    int m_currentJobIndex = -1;
    bool m_running = false;
    bool m_paused = false;

    // S8 render-pipe worker state. m_renderThread runs the synchronous
    // frame-render + in-process encode loop; m_cancelRequested is the cross-thread
    // cancellation flag (mirrors VideoStabilizer::m_cancelled).
    QThread *m_renderThread = nullptr;
    bool m_cancelRequested = false;

    // US-MF-6: ffmpeg.exe subprocess handle for the 10-bit HDR (HDR10/HLG)
    // render branch. Only the HDR subprocess path sets this; the in-process
    // FrameEncoder path leaves it null. m_processMutex guards it so
    // cancelCurrent() / ~RenderQueue() can kill the encoder from the queue
    // thread while the worker thread owns it.
    QMutex m_processMutex;
    QProcess *m_process = nullptr;

    // AC: production export 経路に適用する ACES パイプライン (SSOT は MainWindow
    // の m_acesPipeline)。setAcesPipeline は UI スレッドから start() 前に呼ばれ、
    // レンダーワーカーはフレームループ開始前に 1 度だけスナップショットを取る。
    // 設定とスナップショット取得を m_acesMutex で保護する (Exporter の前例に倣う)。
    mutable QMutex m_acesMutex;
    aces::AcesPipeline m_acesPipeline;

    double m_loudnessGainDb = 0.0;
};
