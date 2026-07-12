#include "YoutubeUploadManager.h"

#include <QFileInfo>

namespace youtube {
namespace manager {

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

Manager::Manager(youtube::oauth::AuthClient* oauth, QObject* parent)
    : QObject(parent)
    , m_oauth(oauth)
    , m_uploadClient(new youtube::upload::Client(this))
{
    // upload::Client の signal を全て自身の slot に配線する。
    connect(m_uploadClient, &youtube::upload::Client::sessionInitiated,
            this, &Manager::onSessionInitiated);
    connect(m_uploadClient, &youtube::upload::Client::sessionError,
            this, &Manager::onSessionError);
    connect(m_uploadClient, &youtube::upload::Client::chunkUploaded,
            this, &Manager::onChunkUploaded);
    connect(m_uploadClient, &youtube::upload::Client::chunkError,
            this, &Manager::onChunkError);
    connect(m_uploadClient, &youtube::upload::Client::completed,
            this, &Manager::onCompleted);

    // oauth::AuthClient は外部所有。non-null のときのみ refresh 経路を配線。
    if (m_oauth) {
        connect(m_oauth, &youtube::oauth::AuthClient::tokensReceived,
                this, &Manager::onTokensReceived);
        connect(m_oauth, &youtube::oauth::AuthClient::authError,
                this, &Manager::onAuthError);
    }
}

Manager::~Manager() = default;

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

QString Manager::addJob(const QString& filePath,
                        const youtube::upload::UploadMetadata& metadata)
{
    Job job;
    job.id       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.filePath = filePath;
    job.metadata = metadata;
    job.state    = State::Queued;

    QFileInfo fi(filePath);
    job.totalSize = fi.exists() ? fi.size() : 0;

    m_jobs.insert(job.id, job);
    emit jobAdded(job.id);

    // 不正ファイル (存在しない / 0 byte) は即時 Failed。
    if (!fi.exists() || job.totalSize <= 0) {
        Job& stored = m_jobs[job.id];
        stored.state        = State::Failed;
        stored.errorMessage = QStringLiteral("file not found or empty: %1").arg(filePath);
        emit jobStateChanged(job.id, State::Failed);
        emit jobFailed(job.id, stored.errorMessage);
        return job.id;
    }

    m_queue.enqueue(job.id);

    if (m_currentJobId.isEmpty()) {
        processNext();
    }
    return job.id;
}

void Manager::pause(const QString& jobId)
{
    if (!m_jobs.contains(jobId)) return;

    Job& job = m_jobs[jobId];
    if (job.state == State::Completed || job.state == State::Failed) {
        return; // 既に終端状態
    }

    job.state = State::Paused;
    emit jobStateChanged(jobId, State::Paused);

    if (m_currentJobId == jobId) {
        // active job を pause: 進行中 chunk を中断し idle に戻す。
        // upload::Client は中断 API を持たないので、reply は捨てて current を解放。
        // (再開時は queryStatus → newOffset から再 upload)
        m_currentJobId.clear();
        // キューには戻さない (resume() で戻る)。
        // 次の job を進めて並列性を保つ。
        processNext();
    } else {
        // キューから外す (resume で再 enqueue)
        QQueue<QString> filtered;
        while (!m_queue.isEmpty()) {
            const QString id = m_queue.dequeue();
            if (id != jobId) filtered.enqueue(id);
        }
        m_queue = filtered;
    }
}

void Manager::resume(const QString& jobId)
{
    if (!m_jobs.contains(jobId)) return;

    Job& job = m_jobs[jobId];
    if (job.state != State::Paused) return;

    job.state        = State::Queued;
    job.retryAttempt = 0;
    emit jobStateChanged(jobId, State::Queued);

    m_queue.enqueue(jobId);

    if (m_currentJobId.isEmpty()) {
        processNext();
    }
}

void Manager::cancel(const QString& jobId)
{
    if (!m_jobs.contains(jobId)) return;

    Job& job = m_jobs[jobId];
    if (job.state == State::Completed || job.state == State::Failed) {
        return;
    }

    job.state        = State::Failed;
    job.errorMessage = QStringLiteral("cancelled");
    emit jobStateChanged(jobId, State::Failed);
    emit jobFailed(jobId, job.errorMessage);

    if (m_currentJobId == jobId) {
        m_currentJobId.clear();
        processNext();
    } else {
        QQueue<QString> filtered;
        while (!m_queue.isEmpty()) {
            const QString id = m_queue.dequeue();
            if (id != jobId) filtered.enqueue(id);
        }
        m_queue = filtered;
    }
}

State Manager::jobState(const QString& jobId) const
{
    auto it = m_jobs.constFind(jobId);
    if (it == m_jobs.constEnd()) return State::Failed;
    return it->state;
}

int Manager::jobProgress(const QString& jobId) const
{
    auto it = m_jobs.constFind(jobId);
    if (it == m_jobs.constEnd()) return 0;
    if (it->totalSize <= 0) return 0;
    const qint64 pct = (it->uploadedBytes * 100) / it->totalSize;
    if (pct < 0)   return 0;
    if (pct > 100) return 100;
    return static_cast<int>(pct);
}

Job Manager::jobSnapshot(const QString& jobId) const
{
    auto it = m_jobs.constFind(jobId);
    if (it == m_jobs.constEnd()) return Job{};
    return *it;
}

// ---------------------------------------------------------------------------
// internal state machine
// ---------------------------------------------------------------------------

void Manager::processNext()
{
    if (!m_currentJobId.isEmpty()) return;
    if (m_queue.isEmpty())         return;

    // Paused/Failed/Completed で取り残されている entry はスキップ
    while (!m_queue.isEmpty()) {
        const QString id = m_queue.dequeue();
        if (!m_jobs.contains(id)) continue;
        const State s = m_jobs[id].state;
        if (s != State::Queued) continue;
        m_currentJobId = id;
        break;
    }

    if (m_currentJobId.isEmpty()) return;

    Job& job = m_jobs[m_currentJobId];
    job.uploadedBytes = 0;
    job.retryAttempt  = 0;
    job.sessionUri.clear();

    setState(m_currentJobId, State::Authorizing);

    // OAuth token: refresh を試みる。refresh 不要 (or refresh_token 無し) なら
    // refreshIfExpired は false を返すので、その場で initiate に進む。
    bool needWait = false;
    if (m_oauth) {
        needWait = m_oauth->refreshIfExpired();
    }

    if (!needWait) {
        // 即時 initiate へ。
        setState(m_currentJobId, State::Initiating);
        if (!m_oauth || !m_oauth->currentToken().isValid()) {
            failCurrent(QStringLiteral("no valid OAuth token"));
            return;
        }
        m_uploadClient->initiateSession(m_oauth->currentToken(),
                                        job.metadata,
                                        job.totalSize);
    }
    // needWait=true の場合は onTokensReceived/onAuthError を待つ。
}

void Manager::setState(const QString& jobId, State newState)
{
    if (!m_jobs.contains(jobId)) return;
    m_jobs[jobId].state = newState;
    emit jobStateChanged(jobId, newState);
}

void Manager::emitProgress(const QString& jobId)
{
    emit jobProgressChanged(jobId, jobProgress(jobId));
}

void Manager::pushNextChunk()
{
    if (m_currentJobId.isEmpty()) return;
    if (!m_jobs.contains(m_currentJobId)) return;

    Job& job = m_jobs[m_currentJobId];
    if (job.state != State::Uploading) return;
    if (job.uploadedBytes >= job.totalSize) return;

    QFile f(job.filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        failCurrent(QStringLiteral("cannot open file: %1").arg(job.filePath));
        return;
    }
    if (!f.seek(job.uploadedBytes)) {
        failCurrent(QStringLiteral("cannot seek to offset %1").arg(job.uploadedBytes));
        return;
    }
    const qint64 remaining = job.totalSize - job.uploadedBytes;
    const qint64 want      = remaining < kChunkSize ? remaining : kChunkSize;
    const QByteArray chunk = f.read(want);
    f.close();

    if (chunk.isEmpty()) {
        failCurrent(QStringLiteral("read 0 bytes at offset %1").arg(job.uploadedBytes));
        return;
    }

    m_uploadClient->uploadChunk(job.sessionUri,
                                chunk,
                                job.uploadedBytes,
                                job.totalSize);
}

void Manager::failCurrent(const QString& reason)
{
    if (m_currentJobId.isEmpty()) return;
    const QString id = m_currentJobId;
    if (m_jobs.contains(id)) {
        m_jobs[id].state        = State::Failed;
        m_jobs[id].errorMessage = reason;
    }
    emit jobStateChanged(id, State::Failed);
    emit jobFailed(id, reason);

    m_currentJobId.clear();
    processNext();
}

void Manager::finishCurrentAndAdvance()
{
    m_currentJobId.clear();
    processNext();
}

// ---------------------------------------------------------------------------
// upload::Client signal handlers
// ---------------------------------------------------------------------------

void Manager::onSessionInitiated(const QString& sessionUri)
{
    if (m_currentJobId.isEmpty()) return;
    if (!m_jobs.contains(m_currentJobId)) return;

    Job& job = m_jobs[m_currentJobId];
    if (job.state != State::Initiating) return;

    job.sessionUri    = sessionUri;
    job.uploadedBytes = 0;
    job.retryAttempt  = 0;

    setState(m_currentJobId, State::Uploading);
    emitProgress(m_currentJobId);
    pushNextChunk();
}

void Manager::onSessionError(const QString& reason)
{
    if (m_currentJobId.isEmpty()) return;
    failCurrent(QStringLiteral("session error: %1").arg(reason));
}

void Manager::onChunkUploaded(qint64 newOffset)
{
    if (m_currentJobId.isEmpty()) return;
    if (!m_jobs.contains(m_currentJobId)) return;

    Job& job = m_jobs[m_currentJobId];
    if (job.state != State::Uploading) return;

    job.uploadedBytes = newOffset;
    job.retryAttempt  = 0;
    emitProgress(m_currentJobId);

    if (newOffset >= job.totalSize) {
        // 完了は upload::Client::completed で通知される (200/201 経路)。
        // 308 で newOffset == totalSize というのは仕様上稀だが念のため pushNextChunk しない。
        return;
    }
    pushNextChunk();
}

void Manager::onChunkError(const QString& reason)
{
    if (m_currentJobId.isEmpty()) return;
    if (!m_jobs.contains(m_currentJobId)) return;

    Job& job = m_jobs[m_currentJobId];
    if (job.state != State::Uploading) return;

    job.retryAttempt++;
    if (job.retryAttempt > kMaxRetry) {
        failCurrent(QStringLiteral("chunk error after %1 retries: %2")
                        .arg(kMaxRetry).arg(reason));
        return;
    }

    // exponential backoff: 2^attempt 秒 = 1 / 2 / 4 / 8 ...
    const int delayMs = (1 << job.retryAttempt) * 1000;
    QTimer::singleShot(delayMs, this, &Manager::retryCurrentChunk);
}

void Manager::onCompleted(const QString& videoId)
{
    if (m_currentJobId.isEmpty()) return;
    if (!m_jobs.contains(m_currentJobId)) return;

    const QString id = m_currentJobId;
    Job& job = m_jobs[id];
    if (job.state != State::Uploading) return;

    job.uploadedBytes = job.totalSize;
    job.state         = State::Completed;
    emit jobStateChanged(id, State::Completed);
    emit jobProgressChanged(id, 100);
    emit jobCompleted(id, videoId);

    finishCurrentAndAdvance();
}

void Manager::retryCurrentChunk()
{
    if (m_currentJobId.isEmpty()) return;
    if (!m_jobs.contains(m_currentJobId)) return;
    if (m_jobs[m_currentJobId].state != State::Uploading) return;
    pushNextChunk();
}

// ---------------------------------------------------------------------------
// oauth::AuthClient signal handlers
// ---------------------------------------------------------------------------

void Manager::onTokensReceived(const youtube::oauth::Token& token)
{
    if (m_currentJobId.isEmpty()) return;
    if (!m_jobs.contains(m_currentJobId)) return;

    Job& job = m_jobs[m_currentJobId];
    if (job.state != State::Authorizing) return;

    if (!token.isValid()) {
        failCurrent(QStringLiteral("invalid token after refresh"));
        return;
    }

    setState(m_currentJobId, State::Initiating);
    m_uploadClient->initiateSession(token, job.metadata, job.totalSize);
}

void Manager::onAuthError(const QString& reason)
{
    if (m_currentJobId.isEmpty()) return;
    if (!m_jobs.contains(m_currentJobId)) return;
    if (m_jobs[m_currentJobId].state != State::Authorizing) return;

    failCurrent(QStringLiteral("auth error: %1").arg(reason));
}

} // namespace manager
} // namespace youtube
