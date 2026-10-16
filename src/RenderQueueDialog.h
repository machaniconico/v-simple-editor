#pragma once

// RenderQueueDialog
// -----------------
// Premiere "Media Encoder" / Resolve "Deliver" page parity. A modeless
// queue UI that lists pending / running / completed export jobs as a
// table, lets the user add/remove jobs, and Start/Stop the run.
//
// The dialog owns nothing media-related — all real work happens in
// RenderQueue (which spawns ffmpeg out-of-process). The dialog only
// observes RenderQueue::jobsChanged / jobProgressUuid /
// jobCompletedUuid signals to keep the table in sync.
//
// Public method `queue()` exposes the underlying RenderQueue so callers
// can pre-add jobs (e.g. wire from the Export menu).

#include <QDialog>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>

#include "RenderQueue.h"

class QTableWidget;
class QPushButton;
class QLabel;
class QComboBox;
class QLineEdit;
class QSpinBox;
class Timeline;

class RenderQueueDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RenderQueueDialog(QWidget *parent = nullptr);
    ~RenderQueueDialog() override;

    // Returns the underlying queue (caller may pre-populate before show()).
    RenderQueue *queue() const { return m_queue; }

    // Optional: reuse an externally-owned queue. The dialog takes ownership
    // of signal connections only; lifetime stays with the caller. MainWindow
    // passes ensureRenderQueue() so dialog jobs share the GUI / MCP queue (and
    // its export notification); "停止" then stops that whole queue. The private
    // queue made in the constructor stays the default for a standalone dialog.
    void setQueue(RenderQueue *queue);

    // US-1125: on a shared queue the queue-wide defaults may belong to another
    // job, so the dialog states the project's ACES pipeline and the loudness
    // gain (non-zero only, like RenderQueue::addJob) on every job it adds.
    void setAcesProvider(std::function<aces::AcesPipeline()> provider) { m_acesProvider = std::move(provider); }
    void setLoudnessGainProvider(std::function<double()> provider) { m_loudnessGainProvider = std::move(provider); }

    // Default timeline range used by the "Add Job" sub-dialog when the
    // caller doesn't supply one. 0..0 means "whole timeline".
    void setDefaultTimelineRange(qint64 startUs, qint64 endUs);

    // RM-1.4: the live edit-graph Timeline used for queue entries whose
    // source path is left blank (the "(現在開いているプロジェクト)" /
    // "current project" case). Without it, a blank-source job has an
    // empty projectFilePath → RenderQueue::resolveTimeline returns
    // nullptr → the entire render (and every track matte) is silently
    // dropped. The caller (MainWindow) must keep this Timeline's matte
    // carrier synced before the dialog submits jobs. Not owned.
    void setLiveTimeline(Timeline *timeline) { m_liveTimeline = timeline; }

private slots:
    void onAddJobClicked();
    void onRemoveJobClicked();
    void onStartClicked();
    void onStopClicked();
    void onJobsChanged();
    void onJobProgress(QString uuid, int percent);
    void onJobCompleted(QString uuid, bool success, QString error);
    void onAllCompleted();

private:
    void buildUi();
    void rebuildTable();
    void updateButtons();
    int  rowForUuid(const QString &uuid) const;
    QString uuidForRow(int row) const;

    RenderQueue       *m_queue          = nullptr;
    bool               m_ownsQueue      = true;
    QSet<QString>      m_ownJobUuids;   // jobs this dialog added; 削除 only touches these
    qint64             m_defaultStartUs = 0;
    qint64             m_defaultEndUs   = 0;
    Timeline          *m_liveTimeline   = nullptr;   // RM-1.4, not owned
    std::function<aces::AcesPipeline()> m_acesProvider;
    std::function<double()>             m_loudnessGainProvider;

    QTableWidget      *m_table          = nullptr;
    QPushButton       *m_addBtn         = nullptr;
    QPushButton       *m_removeBtn      = nullptr;
    QPushButton       *m_startBtn       = nullptr;
    QPushButton       *m_stopBtn        = nullptr;
    QPushButton       *m_closeBtn       = nullptr;
    QLabel            *m_statusLabel    = nullptr;
};

// Lightweight modal sub-dialog for "Add Job". Defined in the .cpp because
// it has no consumers outside the queue dialog.
class AddRenderJobDialog;
