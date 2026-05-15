#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkRequest;
class QNetworkReply;
class QUrl;

namespace cloudrender {

enum class Provider {
    AwsBatch,
    GcpRun,
    Generic
};

enum class JobStatus {
    Queued,
    Running,
    Done,
    Failed
};

struct ProviderConfig {
    Provider provider = Provider::Generic;
    QString  endpointUrl;
    QString  apiKey;
};

struct RenderJob {
    QString   jobId;
    QString   inputUrl;
    QString   outputUrl;
    QString   ffmpegArgs;
    JobStatus status   = JobStatus::Queued;
    int       progress = 0;
};

class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(QObject* parent = nullptr);
    ~Client() override;

    void setProviderConfig(const ProviderConfig& config);
    const ProviderConfig& providerConfig() const { return m_config; }

    QString submitJob(const RenderJob& job);
    void pollJob(const QString& jobId);
    void cancelJob(const QString& jobId);

signals:
    void jobSubmitted(const QString& jobId);
    void jobProgress(const QString& jobId, int percent);
    void jobCompleted(const QString& jobId, const QString& outputUrl);
    void jobFailed(const QString& jobId, const QString& error);

private slots:
    void onSubmitReplyFinished();
    void onPollReplyFinished();
    void onCancelReplyFinished();

private:
    struct SubmitContext {
        ProviderConfig config;
        RenderJob      job;
    };

    struct PollContext {
        ProviderConfig config;
        QString        jobId;
    };

    struct CancelContext {
        ProviderConfig config;
        QString        jobId;
    };

    QUrl buildEndpointUrl(const QString& relativePath) const;
    void applyAuthHeaders(QNetworkRequest& request, const ProviderConfig& config) const;
    QByteArray buildSubmitBody(const RenderJob& job, const ProviderConfig& config) const;
    void updateCachedJob(const RenderJob& job);

    ProviderConfig m_config;
    QPointer<QNetworkAccessManager> m_network;
    QHash<QString, RenderJob> m_jobs;
    QHash<QNetworkReply*, SubmitContext> m_submitReplies;
    QHash<QNetworkReply*, PollContext> m_pollReplies;
    QHash<QNetworkReply*, CancelContext> m_cancelReplies;
};

} // namespace cloudrender
