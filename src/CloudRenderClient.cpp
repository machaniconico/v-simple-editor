#include "CloudRenderClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

namespace cloudrender {

namespace {

QString providerKey(Provider provider)
{
    switch (provider) {
    case Provider::AwsBatch: return QStringLiteral("awsbatch");
    case Provider::GcpRun:   return QStringLiteral("gcprun");
    case Provider::Generic:  return QStringLiteral("generic");
    }
    return QStringLiteral("generic");
}

QString providerName(Provider provider)
{
    switch (provider) {
    case Provider::AwsBatch: return QStringLiteral("AWS Batch");
    case Provider::GcpRun:   return QStringLiteral("GCP Run");
    case Provider::Generic:  return QStringLiteral("Generic");
    }
    return QStringLiteral("Generic");
}

QString replyErrorString(QNetworkReply* reply)
{
    if (!reply) {
        return QStringLiteral("reply is null");
    }

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString error = reply->errorString();
    if (error.isEmpty()) {
        error = QStringLiteral("unknown network error");
    }
    return QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(error);
}

QStringList splitCommand(const QString& commandLine)
{
    const QString trimmed = commandLine.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    return QProcess::splitCommand(trimmed);
}

QJsonArray toJsonArray(const QStringList& parts)
{
    QJsonArray arr;
    for (const QString& part : parts) {
        arr.append(part);
    }
    return arr;
}

QJsonArray environmentForJob(const RenderJob& job)
{
    QJsonArray env;
    env.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("INPUT_URL")},
        {QStringLiteral("value"), job.inputUrl}
    });
    env.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("OUTPUT_URL")},
        {QStringLiteral("value"), job.outputUrl}
    });
    env.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("FFMPEG_ARGS")},
        {QStringLiteral("value"), job.ffmpegArgs}
    });
    return env;
}

QJsonObject genericJobObject(const RenderJob& job)
{
    return QJsonObject{
        {QStringLiteral("jobId"), job.jobId},
        {QStringLiteral("inputUrl"), job.inputUrl},
        {QStringLiteral("outputUrl"), job.outputUrl},
        {QStringLiteral("ffmpegArgs"), job.ffmpegArgs}
    };
}

QString queryValueOrDefault(const ProviderConfig& config,
                            const QString& key,
                            const QString& fallback)
{
    const QUrl url = QUrl::fromUserInput(config.endpointUrl);
    const QUrlQuery query(url);
    const QString value = query.queryItemValue(key);
    return value.isEmpty() ? fallback : value;
}

QJsonObject firstJobObject(const QJsonObject& root)
{
    if (root.value(QStringLiteral("jobs")).isArray()) {
        const QJsonArray jobs = root.value(QStringLiteral("jobs")).toArray();
        if (!jobs.isEmpty() && jobs.first().isObject()) {
            return jobs.first().toObject();
        }
    }
    if (root.value(QStringLiteral("job")).isObject()) {
        return root.value(QStringLiteral("job")).toObject();
    }
    if (root.value(QStringLiteral("result")).isObject()) {
        return root.value(QStringLiteral("result")).toObject();
    }
    return root;
}

QString stringValue(const QJsonObject& object, const QStringList& keys)
{
    for (const QString& key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isString()) {
            const QString text = value.toString().trimmed();
            if (!text.isEmpty()) {
                return text;
            }
        }
    }
    return QString();
}

int intValue(const QJsonObject& object, const QStringList& keys, int fallback = -1)
{
    for (const QString& key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isDouble()) {
            return qBound(0, value.toInt(), 100);
        }
        if (value.isString()) {
            bool ok = false;
            const int percent = value.toString().trimmed().toInt(&ok);
            if (ok) {
                return qBound(0, percent, 100);
            }
        }
    }
    return fallback;
}

JobStatus statusFromString(const QString& rawStatus)
{
    const QString status = rawStatus.trimmed().toUpper();
    if (status == QStringLiteral("DONE") ||
        status == QStringLiteral("COMPLETED") ||
        status == QStringLiteral("COMPLETE") ||
        status == QStringLiteral("SUCCEEDED") ||
        status == QStringLiteral("SUCCESS") ||
        status == QStringLiteral("FINISHED")) {
        return JobStatus::Done;
    }

    if (status == QStringLiteral("FAILED") ||
        status == QStringLiteral("ERROR") ||
        status == QStringLiteral("CANCELLED") ||
        status == QStringLiteral("CANCELED") ||
        status == QStringLiteral("ABORTED") ||
        status == QStringLiteral("TIMEOUT")) {
        return JobStatus::Failed;
    }

    if (status == QStringLiteral("RUNNING") ||
        status == QStringLiteral("ACTIVE") ||
        status == QStringLiteral("PROCESSING") ||
        status == QStringLiteral("EXECUTING") ||
        status == QStringLiteral("IN_PROGRESS")) {
        return JobStatus::Running;
    }

    return JobStatus::Queued;
}

QString extractStatusString(const QJsonObject& root,
                            const QJsonObject& primary,
                            Provider provider)
{
    QString status = stringValue(primary, {
        QStringLiteral("status"),
        QStringLiteral("state"),
        QStringLiteral("phase")
    });
    if (!status.isEmpty()) {
        return status;
    }

    if (root.value(QStringLiteral("status")).isString()) {
        return root.value(QStringLiteral("status")).toString();
    }
    if (root.value(QStringLiteral("state")).isString()) {
        return root.value(QStringLiteral("state")).toString();
    }

    if (provider == Provider::GcpRun && root.value(QStringLiteral("status")).isObject()) {
        const QJsonObject statusObject = root.value(QStringLiteral("status")).toObject();
        const QString condition = stringValue(statusObject, {
            QStringLiteral("state"),
            QStringLiteral("phase")
        });
        if (!condition.isEmpty()) {
            return condition;
        }
    }

    return QStringLiteral("queued");
}

int extractProgress(const QJsonObject& root,
                    const QJsonObject& primary,
                    JobStatus status,
                    int fallback)
{
    int progress = intValue(primary, {
        QStringLiteral("progress"),
        QStringLiteral("percent"),
        QStringLiteral("completionPercent")
    });
    if (progress >= 0) {
        return progress;
    }

    progress = intValue(root, {
        QStringLiteral("progress"),
        QStringLiteral("percent"),
        QStringLiteral("completionPercent")
    });
    if (progress >= 0) {
        return progress;
    }

    if (status == JobStatus::Done) {
        return 100;
    }
    if (status == JobStatus::Running) {
        return qMax(fallback, 1);
    }
    if (status == JobStatus::Failed) {
        return fallback;
    }
    return 0;
}

QString extractOutputUrl(const QJsonObject& root, const QJsonObject& primary)
{
    QString outputUrl = stringValue(primary, {
        QStringLiteral("outputUrl"),
        QStringLiteral("resultUrl"),
        QStringLiteral("destination")
    });
    if (!outputUrl.isEmpty()) {
        return outputUrl;
    }

    outputUrl = stringValue(root, {
        QStringLiteral("outputUrl"),
        QStringLiteral("resultUrl"),
        QStringLiteral("destination")
    });
    return outputUrl;
}

QString extractFailureReason(const QJsonObject& root, const QJsonObject& primary)
{
    QString message = stringValue(primary, {
        QStringLiteral("error"),
        QStringLiteral("message"),
        QStringLiteral("statusReason"),
        QStringLiteral("reason")
    });
    if (!message.isEmpty()) {
        return message;
    }

    message = stringValue(root, {
        QStringLiteral("error"),
        QStringLiteral("message"),
        QStringLiteral("statusReason"),
        QStringLiteral("reason")
    });
    return message;
}

} // namespace

Client::Client(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

Client::~Client() = default;

void Client::setProviderConfig(const ProviderConfig& config)
{
    m_config = config;
}

QUrl Client::buildEndpointUrl(const QString& relativePath) const
{
    QUrl url = QUrl::fromUserInput(m_config.endpointUrl.trimmed());
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }
    path += relativePath;
    url.setPath(path);
    return url;
}

void Client::applyAuthHeaders(QNetworkRequest& request, const ProviderConfig& config) const
{
    if (config.apiKey.trimmed().isEmpty()) {
        return;
    }

    const QByteArray apiKey = config.apiKey.trimmed().toUtf8();
    request.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey);
    request.setRawHeader("X-Api-Key", apiKey);
}

QByteArray Client::buildSubmitBody(const RenderJob& job, const ProviderConfig& config) const
{
    switch (config.provider) {
    case Provider::AwsBatch: {
        QJsonObject parameters{
            {QStringLiteral("inputUrl"), job.inputUrl},
            {QStringLiteral("outputUrl"), job.outputUrl},
            {QStringLiteral("ffmpegArgs"), job.ffmpegArgs}
        };

        QJsonArray command = toJsonArray(splitCommand(job.ffmpegArgs));
        if (command.isEmpty()) {
            command.append(QStringLiteral("ffmpeg"));
        }

        QJsonObject containerOverrides{
            {QStringLiteral("command"), command},
            {QStringLiteral("environment"), environmentForJob(job)}
        };

        QJsonObject root{
            {QStringLiteral("jobName"), QStringLiteral("ffmpeg-%1").arg(job.jobId)},
            {QStringLiteral("jobQueue"),
             queryValueOrDefault(config,
                                 QStringLiteral("jobQueue"),
                                 QStringLiteral("cloudrender-ffmpeg-queue"))},
            {QStringLiteral("jobDefinition"),
             queryValueOrDefault(config,
                                 QStringLiteral("jobDefinition"),
                                 QStringLiteral("cloudrender-ffmpeg-job"))},
            {QStringLiteral("parameters"), parameters},
            {QStringLiteral("containerOverrides"), containerOverrides},
            {QStringLiteral("tags"), QJsonObject{
                {QStringLiteral("jobId"), job.jobId},
                {QStringLiteral("provider"), providerKey(config.provider)}
            }}
        };
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    case Provider::GcpRun: {
        QJsonObject containerOverride{
            {QStringLiteral("args"), toJsonArray(splitCommand(job.ffmpegArgs))},
            {QStringLiteral("env"), environmentForJob(job)}
        };

        QJsonObject overrides{
            {QStringLiteral("containerOverrides"), QJsonArray{containerOverride}}
        };

        QJsonObject root = genericJobObject(job);
        root.insert(QStringLiteral("overrides"), overrides);
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    case Provider::Generic: {
        const QJsonObject root = genericJobObject(job);
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }
    }

    return QJsonDocument(genericJobObject(job)).toJson(QJsonDocument::Compact);
}

void Client::updateCachedJob(const RenderJob& job)
{
    m_jobs.insert(job.jobId, job);
}

QString Client::submitJob(const RenderJob& job)
{
    RenderJob pendingJob = job;
    if (pendingJob.jobId.trimmed().isEmpty()) {
        pendingJob.jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    pendingJob.status = JobStatus::Queued;
    pendingJob.progress = 0;
    updateCachedJob(pendingJob);

    if (m_config.endpointUrl.trimmed().isEmpty()) {
        emit jobFailed(pendingJob.jobId, QStringLiteral("endpoint URL is empty"));
        return pendingJob.jobId;
    }

    QNetworkRequest request(buildEndpointUrl(QStringLiteral("submit")));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json; charset=UTF-8"));
    applyAuthHeaders(request, m_config);

    const QByteArray body = buildSubmitBody(pendingJob, m_config);
    QNetworkReply* reply = m_network->post(request, body);
    if (!reply) {
        emit jobFailed(pendingJob.jobId, QStringLiteral("failed to dispatch submit request"));
        return pendingJob.jobId;
    }

    m_submitReplies.insert(reply, SubmitContext{m_config, pendingJob});
    connect(reply, &QNetworkReply::finished, this, &Client::onSubmitReplyFinished);
    return pendingJob.jobId;
}

void Client::pollJob(const QString& jobId)
{
    if (jobId.trimmed().isEmpty()) {
        return;
    }
    if (m_config.endpointUrl.trimmed().isEmpty()) {
        emit jobFailed(jobId, QStringLiteral("endpoint URL is empty"));
        return;
    }

    const QString encodedJobId = QString::fromLatin1(QUrl::toPercentEncoding(jobId));
    QNetworkRequest request(buildEndpointUrl(QStringLiteral("status/%1").arg(encodedJobId)));
    applyAuthHeaders(request, m_config);

    QNetworkReply* reply = m_network->get(request);
    if (!reply) {
        emit jobFailed(jobId, QStringLiteral("failed to dispatch poll request"));
        return;
    }

    m_pollReplies.insert(reply, PollContext{m_config, jobId});
    connect(reply, &QNetworkReply::finished, this, &Client::onPollReplyFinished);
}

void Client::cancelJob(const QString& jobId)
{
    if (jobId.trimmed().isEmpty()) {
        return;
    }
    if (m_config.endpointUrl.trimmed().isEmpty()) {
        emit jobFailed(jobId, QStringLiteral("endpoint URL is empty"));
        return;
    }

    const QString encodedJobId = QString::fromLatin1(QUrl::toPercentEncoding(jobId));
    QNetworkRequest request(buildEndpointUrl(QStringLiteral("cancel/%1").arg(encodedJobId)));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json; charset=UTF-8"));
    applyAuthHeaders(request, m_config);

    const QJsonObject body{
        {QStringLiteral("jobId"), jobId},
        {QStringLiteral("provider"), providerKey(m_config.provider)}
    };

    QNetworkReply* reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!reply) {
        emit jobFailed(jobId, QStringLiteral("failed to dispatch cancel request"));
        return;
    }

    m_cancelReplies.insert(reply, CancelContext{m_config, jobId});
    connect(reply, &QNetworkReply::finished, this, &Client::onCancelReplyFinished);
}

void Client::onSubmitReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || !m_submitReplies.contains(reply)) {
        return;
    }

    const SubmitContext context = m_submitReplies.take(reply);
    reply->deleteLater();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        emit jobFailed(context.job.jobId, replyErrorString(reply));
        return;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        emit jobFailed(context.job.jobId,
                       QStringLiteral("%1 submit failed with HTTP %2")
                           .arg(providerName(context.config.provider))
                           .arg(httpStatus));
        return;
    }

    emit jobSubmitted(context.job.jobId);
}

void Client::onPollReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || !m_pollReplies.contains(reply)) {
        return;
    }

    const PollContext context = m_pollReplies.take(reply);
    const QByteArray payload = reply->readAll();
    reply->deleteLater();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        emit jobFailed(context.jobId, replyErrorString(reply));
        return;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        emit jobFailed(context.jobId,
                       QStringLiteral("%1 poll failed with HTTP %2")
                           .arg(providerName(context.config.provider))
                           .arg(httpStatus));
        return;
    }

    RenderJob job = m_jobs.value(context.jobId);
    if (job.jobId.isEmpty()) {
        job.jobId = context.jobId;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonObject primary = firstJobObject(root);
        const QString statusText = extractStatusString(root, primary, context.config.provider);
        job.status = statusFromString(statusText);
        job.progress = extractProgress(root, primary, job.status, job.progress);

        const QString outputUrl = extractOutputUrl(root, primary);
        if (!outputUrl.isEmpty()) {
            job.outputUrl = outputUrl;
        }
        updateCachedJob(job);

        if (job.status == JobStatus::Done) {
            emit jobProgress(job.jobId, 100);
            emit jobCompleted(job.jobId, job.outputUrl);
            return;
        }

        if (job.status == JobStatus::Failed) {
            QString reason = extractFailureReason(root, primary);
            if (reason.isEmpty()) {
                reason = QStringLiteral("cloud render job failed");
            }
            emit jobFailed(job.jobId, reason);
            return;
        }

        emit jobProgress(job.jobId, job.progress);
        return;
    }

    emit jobProgress(job.jobId, job.progress);
}

void Client::onCancelReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || !m_cancelReplies.contains(reply)) {
        return;
    }

    const CancelContext context = m_cancelReplies.take(reply);
    reply->deleteLater();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        emit jobFailed(context.jobId, replyErrorString(reply));
        return;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        emit jobFailed(context.jobId,
                       QStringLiteral("%1 cancel failed with HTTP %2")
                           .arg(providerName(context.config.provider))
                           .arg(httpStatus));
        return;
    }

    RenderJob job = m_jobs.value(context.jobId);
    if (job.jobId.isEmpty()) {
        job.jobId = context.jobId;
    }
    job.status = JobStatus::Failed;
    updateCachedJob(job);
    emit jobFailed(context.jobId, QStringLiteral("cancelled"));
}

} // namespace cloudrender
