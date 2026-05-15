#include "VimeoUploadManager.h"

#include <QUuid>

namespace vimeo {
namespace manager {

Manager::Manager(vimeo::oauth::AuthClient *oauth, QObject *parent)
    : QObject(parent)
    , m_oauth(oauth)
{
}

QString Manager::addJob(const QString &filePath,
                        const QString &title,
                        const QString &description,
                        const QString &privacy)
{
    JobInfo info;
    info.jobId       = QUuid::createUuid().toString();
    info.filePath    = filePath;
    info.title       = title;
    info.description = description;
    info.privacy     = privacy;
    info.state       = JobState::Idle;

    m_jobs.append(info);
    emit jobStateChanged(info.jobId, JobState::Idle);

    startNextQueued();
    return info.jobId;
}

void Manager::cancelJob(const QString &jobId)
{
    for (JobInfo &info : m_jobs) {
        if (info.jobId != jobId)
            continue;

        if (m_clients.contains(jobId)) {
            vimeo::upload::UploadClient *client = m_clients.take(jobId);
            client->cancel();
            delete client;
        }

        info.state     = JobState::Failed;
        info.lastError = QStringLiteral("cancelled");
        emit jobStateChanged(jobId, JobState::Failed);
        emit jobFinished(jobId, false, QStringLiteral("cancelled"));
        break;
    }
}

void Manager::retryJob(const QString &jobId)
{
    for (JobInfo &info : m_jobs) {
        if (info.jobId != jobId)
            continue;

        // Remove any stale client
        if (m_clients.contains(jobId)) {
            delete m_clients.take(jobId);
        }

        info.state          = JobState::Idle;
        info.progressPercent = 0;
        info.lastError.clear();
        emit jobStateChanged(jobId, JobState::Idle);

        startNextQueued();
        break;
    }
}

QList<JobInfo> Manager::activeJobs() const
{
    return m_jobs;
}

JobInfo Manager::job(const QString &jobId) const
{
    for (const JobInfo &info : m_jobs) {
        if (info.jobId == jobId)
            return info;
    }
    return JobInfo{};
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void Manager::startNextQueued()
{
    // Count currently uploading jobs
    int uploading = 0;
    for (const JobInfo &info : m_jobs) {
        if (info.state == JobState::Uploading || info.state == JobState::Authenticating)
            ++uploading;
    }

    if (uploading >= m_concurrencyLimit)
        return;

    // Find next idle job
    for (JobInfo &info : m_jobs) {
        if (info.state != JobState::Idle)
            continue;

        info.state = JobState::Uploading;
        emit jobStateChanged(info.jobId, JobState::Uploading);

        const QString jobId = info.jobId;

        // Build UploadJob
        vimeo::upload::UploadJob uploadJob;
        uploadJob.filePath    = info.filePath;
        uploadJob.title       = info.title;
        uploadJob.description = info.description;
        uploadJob.privacy     = info.privacy;

        // Determine access token
        QString token;
        if (!m_oauth.isNull())
            token = m_oauth->accessToken();

        auto *client = new vimeo::upload::UploadClient(
            m_oauth.isNull() ? vimeo::oauth::VimeoOAuthConfig::defaultConfig()
                             : m_oauth->config(),
            this);

        if (!token.isEmpty())
            client->setAccessToken(token);

        // Connect progress
        connect(client, &vimeo::upload::UploadClient::uploadProgress,
                this, [this, jobId](qint64 sent, qint64 total) {
                    onClientProgress(jobId, sent, total);
                });

        // Connect finished
        connect(client, &vimeo::upload::UploadClient::uploadFinished,
                this, [this, jobId](const QString &videoUri) {
                    onClientFinished(jobId, videoUri);
                });

        // Connect failed
        connect(client, &vimeo::upload::UploadClient::uploadFailed,
                this, [this, jobId](const QString &err) {
                    onClientFailed(jobId, err);
                });

        m_clients.insert(jobId, client);
        client->startUpload(uploadJob);

        ++uploading;
        if (uploading >= m_concurrencyLimit)
            break;
    }
}

void Manager::onClientProgress(const QString &jobId, qint64 sent, qint64 total)
{
    int percent = (total > 0) ? static_cast<int>(sent * 100 / total) : 0;

    for (JobInfo &info : m_jobs) {
        if (info.jobId == jobId) {
            info.progressPercent = percent;
            break;
        }
    }

    emit jobProgress(jobId, percent);
}

void Manager::onClientFinished(const QString &jobId, const QString &videoUri)
{
    for (JobInfo &info : m_jobs) {
        if (info.jobId == jobId) {
            info.state           = JobState::Complete;
            info.progressPercent = 100;
            break;
        }
    }

    if (m_clients.contains(jobId)) {
        delete m_clients.take(jobId);
    }

    emit jobStateChanged(jobId, JobState::Complete);
    emit jobFinished(jobId, true, videoUri);

    startNextQueued();
}

void Manager::onClientFailed(const QString &jobId, const QString &err)
{
    for (JobInfo &info : m_jobs) {
        if (info.jobId == jobId) {
            info.state     = JobState::Failed;
            info.lastError = err;
            break;
        }
    }

    if (m_clients.contains(jobId)) {
        delete m_clients.take(jobId);
    }

    emit jobStateChanged(jobId, JobState::Failed);
    emit jobFinished(jobId, false, err);

    startNextQueued();
}

} // namespace manager
} // namespace vimeo
