#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include "VimeoOAuth.h"
#include "VimeoUploadClient.h"

namespace vimeo {
namespace manager {

enum class JobState {
    Idle,
    Authenticating,
    Uploading,
    Complete,
    Failed
};

struct JobInfo {
    QString jobId;
    QString filePath;
    QString title;
    QString description;
    QString privacy       = QStringLiteral("unlisted");
    JobState state        = JobState::Idle;
    int progressPercent   = 0;
    QString lastError;
};

// ---------------------------------------------------------------------------
// Manager — Sprint 20 US-VIM-2
// Manages a queue of Vimeo upload jobs with a concurrency limit.
// ---------------------------------------------------------------------------

class Manager : public QObject {
    Q_OBJECT
public:
    explicit Manager(vimeo::oauth::AuthClient *oauth,
                     QObject *parent = nullptr);
    ~Manager() override = default;

    QString addJob(const QString &filePath,
                   const QString &title,
                   const QString &description,
                   const QString &privacy = QStringLiteral("unlisted"));

    void cancelJob(const QString &jobId);
    void retryJob(const QString &jobId);

    QList<JobInfo> activeJobs() const;
    JobInfo job(const QString &jobId) const;

signals:
    void jobStateChanged(const QString &jobId, JobState state);
    void jobProgress(const QString &jobId, int percent);
    void jobFinished(const QString &jobId, bool success, const QString &message);

private slots:
    void onClientProgress(const QString &jobId, qint64 sent, qint64 total);
    void onClientFinished(const QString &jobId, const QString &videoUri);
    void onClientFailed(const QString &jobId, const QString &err);

private:
    void startNextQueued();

    QPointer<vimeo::oauth::AuthClient>           m_oauth;
    QList<JobInfo>                               m_jobs;
    QHash<QString, vimeo::upload::UploadClient*> m_clients;
    int                                          m_concurrencyLimit = 3;
};

} // namespace manager
} // namespace vimeo
