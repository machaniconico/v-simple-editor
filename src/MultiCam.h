#pragma once

// MultiCam
// --------
// Two co-existing data tracks for multi-camera editing:
//
//   1) MultiCamSession (legacy / advanced):
//      A QObject session model that owns CameraSource entries, audio
//      auto-sync hooks, generated EditSegment lists, etc. This is the
//      richer model retained for the planned timeline-integration pass.
//
//   2) MultiCamProject (this story — STANDALONE):
//      A pure-data Premiere/Resolve-style "edit decision list" produced
//      by MultiCamDialog. The dialog lets the user load 2..4 angles,
//      drop switch markers (cuts) at the playhead, and emits a
//      MultiCamProject for the caller. The dialog never touches the
//      Timeline, AudioMixer, or any decoder — Timeline integration is
//      deferred to a follow-up consolidation pass.
//
// Both shapes co-exist; MultiCamProject does not derive from or depend
// on MultiCamSession.

#include <QObject>
#include <QVector>
#include <QString>
#include <QImage>
#include <QJsonObject>
#include "MultiCamSync.h"

class QJsonArray;

struct CameraSource {
    QString filePath;
    QString label;        // "Camera 1", "Camera 2", etc.
    double syncOffset = 0.0; // seconds offset for sync (positive = delayed)
    double duration = 0.0;
    bool isActive = true;
};

struct CameraCut {
    int cameraIndex;      // which camera is active
    double startTime;     // timeline time
    double endTime;
};

class MultiCamSession : public QObject
{
    Q_OBJECT

public:
    explicit MultiCamSession(QObject *parent = nullptr);

    // Source management
    void addSource(const QString &filePath, const QString &label = QString());
    void removeSource(int index);
    int sourceCount() const { return m_sources.size(); }
    const QVector<CameraSource> &sources() const { return m_sources; }

    // Sync
    void setSyncOffset(int sourceIndex, double offset);
    multicam::AudioSyncReport autoSyncByAudio();

    // Cutting
    void switchToCamera(int cameraIndex, double time);
    void addCut(int cameraIndex, double startTime, double endTime);
    void removeCut(int index);
    const QVector<CameraCut> &cuts() const { return m_cuts; }

    // Get active camera at a given time
    int activeCameraAt(double time) const;

    // Multi-view grid layout
    int gridColumns() const;
    int gridRows() const;

    // Generate final edit list (for export)
    struct EditSegment {
        int cameraIndex;
        double sourceStart; // accounting for sync offset
        double sourceEnd;
        double timelineStart;
        double timelineEnd;
    };
    QVector<EditSegment> generateEditList() const;

    // Total duration (max of all sources considering offsets)
    double totalDuration() const;

signals:
    void sourcesChanged();
    void cutsChanged();
    void syncCompleted();

private:
    void sortCuts();

    QVector<CameraSource> m_sources;
    QVector<CameraCut> m_cuts;
};

// ---------------------------------------------------------------------------
// MultiCamProject — pure-data EDL (this story).
//
// id is a stable per-angle integer (not the index into angles[]). The dialog
// allocates ids via running max+1; persistence preserves them so that
// switches can keep referring to a specific angle even after the user
// reorders or deletes adjacent angles.
// ---------------------------------------------------------------------------

struct MultiCamAngle {
    int id = 0;                       // stable id
    QString sourcePath;               // video file path
    qint64 syncOffsetUs = 0;          // offset from "master sync time"
    QString label;                    // human-friendly ("Cam 1", "Cam B")
};

struct MultiCamSwitch {
    qint64 timelineUs = 0;            // when to switch
    int activeAngleId = 0;            // which angle becomes active
};

struct MultiCamProject {
    QVector<MultiCamAngle>  angles;     // 2..4 entries (validated by dialog)
    QVector<MultiCamSwitch> switches;   // sorted by timelineUs ascending
    int                     defaultAngleId = 0;

    // JSON I/O. Round-trip stable: fromJson(toJson()) preserves all fields
    // including id ordering.
    QJsonObject toJson() const;
    static MultiCamProject fromJson(const QJsonObject &);

    // Active angle at a given timeline time. Returns the activeAngleId of
    // the latest switch with timelineUs <= timelineUs. If no switch sits
    // before timelineUs, falls back to defaultAngleId. switches must be
    // sorted by timelineUs ascending (the dialog enforces this on insert).
    int activeAngleAt(qint64 timelineUs) const;
};

// Forward declaration for callers that only need the dialog's pointer type.
class MultiCamDialog;
