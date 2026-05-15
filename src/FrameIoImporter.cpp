#include "FrameIoImporter.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUuid>
#include <cmath>

namespace frameio {
namespace importer {

FrameIoImporter::FrameIoImporter(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void FrameIoImporter::fetchComments(const QString &assetId,
                                    const ImportConfig &config)
{
    const QString urlStr =
        QStringLiteral("%1/assets/%2/comments").arg(config.baseUrl, assetId);

    const QUrl requestUrl(urlStr);
    QNetworkRequest request(requestUrl);
    request.setRawHeader(
        "Authorization",
        QStringLiteral("Bearer %1").arg(config.apiToken).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));

    emit importProgress(0);

    QNetworkReply *reply = m_nam->get(request);

    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit importFailed(reply->errorString());
            return;
        }

        emit importProgress(80);

        const QByteArray data = reply->readAll();
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

        if (parseError.error != QJsonParseError::NoError) {
            emit importFailed(
                QStringLiteral("JSON parse error: %1").arg(parseError.errorString()));
            return;
        }

        QJsonArray arr;
        if (doc.isArray()) {
            arr = doc.array();
        } else if (doc.isObject()) {
            // Some Frame.io endpoints wrap in {"data": [...]}
            arr = doc.object().value(QStringLiteral("data")).toArray();
        }

        const collab::CommentTrack track = parseFrameIoJson(arr);

        emit importProgress(100);
        emit importFinished(track);
    });
}

collab::CommentTrack FrameIoImporter::parseFrameIoJson(const QJsonArray &arr,
                                                       double fps)
{
    collab::CommentTrack track;
    track.trackId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    track.version = 0;

    for (const QJsonValue &val : arr) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();

        collab::Comment comment;

        // id
        comment.id = obj.value(QStringLiteral("id")).toString();
        if (comment.id.isEmpty()) {
            comment.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }

        // body
        comment.body = obj.value(QStringLiteral("body")).toString();

        // timestamp (seconds) → timecodeMs (milliseconds)
        const double timestampSec =
            obj.value(QStringLiteral("timestamp")).toDouble(0.0);
        comment.timecodeMs = static_cast<qint64>(std::round(timestampSec * 1000.0));

        // author.name → authorId
        const QJsonObject authorObj =
            obj.value(QStringLiteral("author")).toObject();
        comment.authorId = authorObj.value(QStringLiteral("name")).toString();
        if (comment.authorId.isEmpty()) {
            comment.authorId = QStringLiteral("unknown");
        }

        // parentId (empty = top-level)
        comment.parentId = obj.value(QStringLiteral("parent_id")).toString();

        // timestampMs: use current UTC epoch ms (Frame.io has inserted_at field)
        const QString insertedAt =
            obj.value(QStringLiteral("inserted_at")).toString();
        if (!insertedAt.isEmpty()) {
            const QDateTime dt =
                QDateTime::fromString(insertedAt, Qt::ISODateWithMs);
            comment.timestampMs = dt.isValid() ? dt.toMSecsSinceEpoch() : 0LL;
        } else {
            comment.timestampMs = 0LL;
        }

        comment.status = collab::Status::Open;

        track.comments.append(comment);
    }

    return track;
}

} // namespace importer
} // namespace frameio
