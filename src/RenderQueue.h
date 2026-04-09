#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QDateTime>
#include <QJsonObject>
#include <QProcess>

enum class RenderJobStatus {
    Pending,
    Rendering,
    Completed,
    Failed,
    Cancelled
};

struct RenderJob {
    int id = 0;
    QString name;
    QString projectFilePath;
    QString outputPath;
    QJsonObject exportConfig;
    RenderJobStatus status = RenderJobStatus::Pending;
    int progress = 0;
    QDateTime startTime;
    QDateTime endTime;
    QString errorMessage;
};

class RenderQueue : public QObject
{
    Q_OBJECT

public:
    explicit RenderQueue(QObject *parent = nullptr);
    ~RenderQueue();

    int addJob(const QString &name, const QString &projectFilePath,
               const QString &outputPath, const QJsonObject &exportConfig);
    void removeJob(int id);
    void clearCompleted();
    void clearAll();

    void startQueue();
    void pauseQueue();
    void resumeQueue();
    void cancelCurrent();

    QVector<RenderJob> jobs() const { return m_jobs; }
    const RenderJob *currentJob() const;
    bool isRunning() const { return m_running; }
    int pendingCount() const;
    int completedCount() const;

    void moveJobUp(int id);
    void moveJobDown(int id);

    int totalEstimatedTime() const;

    bool saveQueue(const QString &filePath) const;
    bool loadQueue(const QString &filePath);

signals:
    void jobStarted(int id);
    void jobProgress(int id, int percent);
    void jobCompleted(int id);
    void jobFailed(int id, const QString &error);
    void queueFinished();
    void queueProgress(int overallPercent);

private:
    void startNextJob();
    QStringList buildFFmpegArgs(const RenderJob &job) const;
    void parseFFmpegOutput(const QString &line);
    int findJobIndex(int id) const;

    QVector<RenderJob> m_jobs;
    QProcess *m_process = nullptr;
    int m_nextId = 1;
    int m_currentJobIndex = -1;
    bool m_running = false;
    bool m_paused = false;
    double m_currentDuration = 0.0;  // total duration in seconds for progress parsing
};
