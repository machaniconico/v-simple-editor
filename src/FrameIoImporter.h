#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonArray>
#include <QString>
#include <QPointer>

#include "CollaborationModel.h"

namespace frameio {
namespace importer {

struct ImportConfig {
    QString apiToken;
    QString projectId;
    QString baseUrl = QStringLiteral("https://api.frame.io/v3");
};

class FrameIoImporter : public QObject {
    Q_OBJECT

public:
    explicit FrameIoImporter(QObject *parent = nullptr);

    void fetchComments(const QString &assetId, const ImportConfig &config);

    static collab::CommentTrack parseFrameIoJson(const QJsonArray &arr,
                                                 double fps = 30.0);

signals:
    void importProgress(int percent);
    void importFinished(const collab::CommentTrack &track);
    void importFailed(const QString &error);

private:
    QNetworkAccessManager *m_nam = nullptr;
};

} // namespace importer
} // namespace frameio
