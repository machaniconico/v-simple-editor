#include "UncProbe.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QSemaphore>
#include <QThreadPool>
#include <QUrl>
#include <atomic>
#include <memory>
#include <vector>

namespace uncprobe {
namespace {
// Shared by the caller and every task through shared_ptr: a probe that wakes after the
// deadline writes into this heap object, never into the caller's stack or the ImportResult.
struct ProbeShared {
    explicit ProbeShared(int sentinels, int roots)
        : slot(new std::atomic<int>[sentinels]()), rootDone(new std::atomic<int>[roots]()) {}
    QSemaphore finished;
    std::unique_ptr<std::atomic<int>[]> slot;     // per sentinel: 0 unknown, 1 file exists, 2 file missing
    std::unique_ptr<std::atomic<int>[]> rootDone; // per root: 1 once its slots are final
};
constexpr int kExists = 1;
constexpr int kMissing = 2;

struct RootJob {
    QString root;
    std::vector<int> sentinels;
    QStringList files; // normalised path per sentinel
};
} // namespace

std::optional<QString> normaliseUnc(const QString &path)
{
    QString n;
    if (path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl url(path);
        if (url.host().isEmpty() || url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
            return std::nullopt;
        n = url.toLocalFile();
    } else {
        n = path;
    }
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (n.startsWith(QLatin1String("//?/UNC/"), Qt::CaseInsensitive)) n = QLatin1String("//") + n.mid(8);
    if (!n.startsWith(QLatin1String("//")) || n.startsWith(QLatin1String("//./"))
        || n.startsWith(QLatin1String("//?/")) || n.size() < 3 || n.at(2) == QLatin1Char('/'))
        return std::nullopt;
    n.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return n;
}

QString shareRoot(const QString &path)
{
    const QString n = normaliseUnc(path).value_or(path);
    const QStringList parts = n.mid(2).split(QLatin1Char('\\'));
    QString root = QStringLiteral("\\\\") + parts.value(0);
    if (parts.size() > 1 && !parts[1].isEmpty()) root += QLatin1Char('\\') + parts[1];
    return root.toLower();
}

int relinkOfflineSources(fcpx::xml::ImportResult &result, ProbeFn probe, int deadlineMs, int maxConcurrent)
{
    QElapsedTimer timer;
    timer.start();
    if (result.offlineSources.isEmpty()) return 0;
    if (!probe) {
#ifdef Q_OS_WIN
        probe = [](const QString &p) { return QFileInfo(p).exists(); };
#else
        probe = [](const QString &) { return false; };
#endif
    }
    // Group the sentinels by share root; device paths and other non-UNC forms are never probed.
    QStringList sentinels, originals, normalised;
    QHash<QString, int> rootIndex;
    std::vector<RootJob> jobs;
    for (auto it = result.offlineSources.cbegin(); it != result.offlineSources.cend(); ++it) {
        const auto norm = normaliseUnc(it.value());
        if (!norm) continue;
        const int s = int(sentinels.size());
        sentinels << it.key(); originals << it.value(); normalised << *norm;
        const QString root = shareRoot(*norm);
        auto found = rootIndex.find(root);
        if (found == rootIndex.end()) {
            found = rootIndex.insert(root, int(jobs.size()));
            jobs.push_back(RootJob{root, {}, {}});
        }
        jobs[size_t(found.value())].sentinels.push_back(s);
        jobs[size_t(found.value())].files << *norm;
    }
    if (jobs.empty()) return 0;

    const int rootCount = int(jobs.size());
    auto shared = std::make_shared<ProbeShared>(int(sentinels.size()), rootCount);
    // One pool per call and never deleted: an unanswered QFileInfo can neither block the
    // application's exit nor starve a later call (a singleton would).
    auto *pool = new QThreadPool;
    pool->setExpiryTimeout(1000);
    pool->setMaxThreadCount(qMax(1, maxConcurrent));
    for (int r = 0; r < rootCount; ++r) {
        pool->start([shared, probe, job = jobs[size_t(r)], r]() {
            if (probe(job.root)) {
                QHash<QString, bool> known;
                for (size_t i = 0; i < job.sentinels.size(); ++i) {
                    const QString &file = job.files[int(i)];
                    auto k = known.find(file);
                    if (k == known.end()) k = known.insert(file, probe(file));
                    shared->slot[job.sentinels[i]].store(k.value() ? kExists : kMissing);
                }
                shared->rootDone[r].store(1);
            }
            shared->finished.release();
        });
    }
    shared->finished.tryAcquire(rootCount, int(qMax<qint64>(0, deadlineMs - timer.elapsed())));

    // Only roots that finished inside the deadline are collected; the rest stay sentinels.
    QHash<QString, QString> rewrite; // sentinel -> path to write
    int relinked = 0;
    for (const RootJob &job : jobs) {
        const int r = int(&job - jobs.data());
        if (!shared->rootDone[r].load()) continue;
        for (size_t i = 0; i < job.sentinels.size(); ++i) {
            const int s = job.sentinels[i];
            const int state = shared->slot[s].load();
            if (state == 0) continue;
            const QString &sentinel = sentinels[s];
            rewrite.insert(sentinel, normalised[s]);
            result.offlineSources.remove(sentinel);
            const QString oldLine = QStringLiteral("オフライン素材 (ネットワークパスは確認しません): %1 → %2").arg(originals[s], sentinel);
            QString newLine;
            if (state == kExists) {
                newLine = QStringLiteral("再リンク: %1").arg(normalised[s]);
                ++result.resolvedFiles;
                --result.unresolvedFiles;
                ++relinked;
            } else {
                newLine = QStringLiteral("見つかりません: %1 (共有は応答)").arg(normalised[s]);
            }
            for (QString &w : result.warnings)
                if (w == oldLine) w = newLine;
        }
    }
    if (rewrite.isEmpty()) return relinked;
    auto rewriteTracks = [&rewrite](QVector<QVector<ClipInfo>> &tracks) {
        for (auto &track : tracks)
            for (ClipInfo &c : track) {
                const auto hit = rewrite.constFind(c.filePath);
                if (hit != rewrite.constEnd()) c.filePath = hit.value();
            }
    };
    rewriteTracks(result.videoTracks);
    rewriteTracks(result.audioTracks);
    for (TimelineSequence &sq : result.sequences) {
        rewriteTracks(sq.videoTracks);
        rewriteTracks(sq.audioTracks);
    }
    return relinked;
}
} // namespace uncprobe
