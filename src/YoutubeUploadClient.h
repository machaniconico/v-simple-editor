#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QPointer>

#include "YoutubeOAuth.h"

class QNetworkAccessManager;
class QNetworkReply;

// ---------------------------------------------------------------------------
// namespace youtube::upload — Sprint 17 US-YT-2
// YouTube Data API v3 Resumable Upload クライアント。
// 仕様: https://developers.google.com/youtube/v3/guides/using_resumable_upload_protocol
//
// 典型フロー:
//   1) initiateSession(token, metadata, fileSize)
//        → POST /upload/youtube/v3/videos?uploadType=resumable
//        → 200 OK + Location: <sessionUri>  → sessionInitiated(sessionUri)
//   2) uploadChunk(sessionUri, chunkBytes, offset, totalSize)
//        → PUT sessionUri  Content-Range: bytes <offset>-<end>/<totalSize>
//        → 308 Resume Incomplete  → chunkUploaded(newOffset)
//        → 200/201 OK             → completed(videoId)
//   3) queryStatus(sessionUri, totalSize)  (中断後の再開)
//        → PUT sessionUri (empty body)  Content-Range: bytes */<totalSize>
//        → 308 + Range: bytes=0-<lastByte>  → statusKnown(nextOffset)
//
// 設計方針:
//   - OAuth 認証層 (youtube::oauth::Token) を引数で受け取る疎結合。
//   - 内部 QNetworkAccessManager を 1 つ持ち、async signal で結果通知。
//   - 例外は投げず、必ずエラー signal で通知する。
// ---------------------------------------------------------------------------

namespace youtube {
namespace upload {

// アップロード対象動画のメタデータ。snippet + status の最小構成。
struct UploadMetadata {
    QString title;
    QString description;
    QStringList tags;
    QString privacy   = QStringLiteral("private"); // private / unlisted / public
    int categoryId    = 22;                        // YouTube category id (22 = People & Blogs)
};

// resumable session の状態スナップショット (UI 進捗表示用)。
struct UploadSession {
    QString sessionUri;
    qint64 totalSize     = 0;
    qint64 uploadedBytes = 0;
};

class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(QObject* parent = nullptr);
    ~Client() override;

    // POST /upload/youtube/v3/videos?uploadType=resumable&part=snippet,status
    // 成功: sessionInitiated(sessionUri)、失敗: sessionError(reason)。
    void initiateSession(const youtube::oauth::Token& token,
                         const UploadMetadata& metadata,
                         qint64 fileSize);

    // PUT sessionUri に chunk を送信。Content-Range: bytes offset-(offset+len-1)/totalSize。
    // 308 Resume Incomplete → chunkUploaded(newOffset)
    // 200/201 OK            → completed(videoId)
    // それ以外/network err  → chunkError(reason)
    void uploadChunk(const QString& sessionUri,
                     const QByteArray& chunk,
                     qint64 offset,
                     qint64 totalSize);

    // 中断 session の現在状態問い合わせ。Content-Range: bytes */totalSize で空 PUT。
    // 308 + Range: bytes=0-<lastByte> → statusKnown(lastByte+1)
    // 200/201 OK                       → statusKnown(totalSize)  (= 完了済み)
    void queryStatus(const QString& sessionUri, qint64 totalSize);

    // Content-Range header 値の純関数 builder (テスト容易性 + 内部使用)。
    // chunkLen <= 0 の場合は "bytes */<totalSize>" を返す (status query 用)。
    static QByteArray buildContentRange(qint64 offset, qint64 chunkLen, qint64 totalSize);

    // Range: bytes=0-<lastByte> 形式から lastByte を取り出す。失敗時 -1。
    static qint64 parseRangeHeaderEnd(const QByteArray& rangeHeaderValue);

    // テスト用 base URL override (空 → 既存 https://www.googleapis.com を使う)。
    // thread-safe ではない: selftest 起動時に setApiBaseUrl 後、production 経路では使わない想定。
    static void setApiBaseUrl(const QString& url);
    static QString apiBaseUrl();

signals:
    // initiateSession 成功時。sessionUri を以降の uploadChunk / queryStatus に渡す。
    void sessionInitiated(const QString& sessionUri);
    void sessionError(const QString& reason);

    // uploadChunk: 308 で次に送るべきバイトオフセットを通知。
    void chunkUploaded(qint64 newOffset);
    void chunkError(const QString& reason);

    // uploadChunk: 200/201 でアップロード完了。videoId は response JSON の "id"。
    void completed(const QString& videoId);

    // queryStatus 結果。中断後の再送オフセットとして使う。
    void statusKnown(qint64 nextOffset);
    void statusError(const QString& reason);

private slots:
    void onInitiateReplyFinished();
    void onChunkReplyFinished();
    void onStatusReplyFinished();

private:
    QPointer<QNetworkAccessManager> m_nam;

    // 進行中 reply (一度に 1 つのみサポート)。複数並列が必要なら呼び出し側で別 Client を作る。
    QPointer<QNetworkReply> m_initiateReply;
    QPointer<QNetworkReply> m_chunkReply;
    QPointer<QNetworkReply> m_statusReply;

    // chunk reply context (offset/length は finish 時に必要)
    qint64 m_chunkOffset    = 0;
    qint64 m_chunkLen       = 0;
    qint64 m_chunkTotalSize = 0;

    // status reply context
    qint64 m_statusTotalSize = 0;
};

} // namespace upload
} // namespace youtube
