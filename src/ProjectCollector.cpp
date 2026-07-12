#include "ProjectCollector.h"
#include "ProjectFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QThread>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static constexpr qint64 kChunkSize = 1024 * 1024;  // 1 MiB copy buffer

// Resolve a unique destination basename inside destMediaDir for srcPath.
// seenSrcToBasename / seenBasenameToSrc persist across calls so the same
// source always maps to the same destination, and distinct sources with
// clashing basenames get _2, _3, ... suffixes.
static QString resolveDestBasename(const QString& srcPath,
                                   QHash<QString, QString>& seenSrcToBasename,
                                   QHash<QString, QString>& seenBasenameToSrc,
                                   const QString& destMediaDir)
{
    auto it = seenSrcToBasename.find(srcPath);
    if (it != seenSrcToBasename.end())
        return it.value();

    QFileInfo fi(srcPath);
    const QString base = fi.completeBaseName();
    const QString ext  = fi.suffix().isEmpty()
                             ? QString()
                             : QStringLiteral(".") + fi.suffix();
    QString candidate = base + ext;

    int counter = 2;
    while (seenBasenameToSrc.contains(candidate) ||
           QFile::exists(destMediaDir + QLatin1Char('/') + candidate)) {
        candidate = base + QStringLiteral("_%1").arg(counter) + ext;
        ++counter;
    }

    seenSrcToBasename.insert(srcPath, candidate);
    seenBasenameToSrc.insert(candidate, srcPath);
    return candidate;
}

// ---------------------------------------------------------------------------
// PathSlot — file-scope struct, safe for QVector + range-for under MSVC
// ---------------------------------------------------------------------------

struct PathSlot {
    QString* field;    // pointer into a mutable ProjectData field
    QString  srcPath;  // the absolute source path at collection time
};

// Collect all media path pathSlots from a ProjectData copy.
static QVector<PathSlot> collectSlots(ProjectData& data)
{
    QVector<PathSlot> pathSlots;

    for (int ti = 0; ti < data.videoTracks.size(); ++ti) {
        QVector<ClipInfo>& track = data.videoTracks[ti];
        for (int ci = 0; ci < track.size(); ++ci) {
            ClipInfo& clip = track[ci];
            if (!clip.filePath.isEmpty())
                pathSlots.append({&clip.filePath, clip.filePath});
        }
    }

    for (int ti = 0; ti < data.audioTracks.size(); ++ti) {
        QVector<ClipInfo>& track = data.audioTracks[ti];
        for (int ci = 0; ci < track.size(); ++ci) {
            ClipInfo& clip = track[ci];
            if (!clip.filePath.isEmpty())
                pathSlots.append({&clip.filePath, clip.filePath});
        }
    }

    for (int i = 0; i < data.particleClipEntries.size(); ++i) {
        ParticleClipEntry& pce = data.particleClipEntries[i];
        if (!pce.clipFilePath.isEmpty())
            pathSlots.append({&pce.clipFilePath, pce.clipFilePath});
    }

    // OverlayItem with type == "image": path stored in `text` field.
    for (int i = 0; i < data.overlays.size(); ++i) {
        OverlayItem& overlay = data.overlays[i];
        if (overlay.type == QLatin1String("image") && !overlay.text.isEmpty())
            pathSlots.append({&overlay.text, overlay.text});
    }

    return pathSlots;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

class CollectorThread : public QThread {
public:
    CollectorThread(ProjectCollector* collector,
                    ProjectData dataCopy,
                    QString destDir,
                    QString projectFileName)
        : QThread(nullptr)
        , m_collector(collector)
        , m_dataCopy(std::move(dataCopy))
        , m_destDir(std::move(destDir))
        , m_projectFileName(std::move(projectFileName))
    {}

protected:
    void run() override {
        m_collector->doCollect(std::move(m_dataCopy),
                               std::move(m_destDir),
                               std::move(m_projectFileName));
    }

private:
    ProjectCollector* m_collector;
    ProjectData       m_dataCopy;
    QString           m_destDir;
    QString           m_projectFileName;
};

// ---------------------------------------------------------------------------
// ProjectCollector
// ---------------------------------------------------------------------------

ProjectCollector::ProjectCollector(QObject* parent)
    : QObject(parent)
{}

ProjectCollector::~ProjectCollector()
{
    cancel();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void ProjectCollector::collect(const ProjectData& data,
                               const QString& destDir,
                               const QString& projectFileName)
{
    m_warnings.clear();
    m_bytesCopied = 0;
    m_cancelled   = false;

    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }

    // Pass a *copy* — the caller's const ref is never touched.
    auto* t = new CollectorThread(this, data, destDir, projectFileName);
    m_thread = t;
    t->start();
}

void ProjectCollector::cancel()
{
    m_cancelled = true;
}

QStringList ProjectCollector::warnings() const
{
    return m_warnings;
}

qint64 ProjectCollector::bytesCopied() const
{
    return m_bytesCopied;
}

// ---------------------------------------------------------------------------
// doCollect — runs on the worker thread
// ---------------------------------------------------------------------------

void ProjectCollector::doCollect(ProjectData dataCopy,
                                 QString destDir,
                                 QString projectFileName)
{
    // 1. Create destDir/media/
    const QString mediaSubdir = destDir + QStringLiteral("/media");
    if (!QDir().mkpath(mediaSubdir)) {
        emit finished(false, QStringLiteral("failed to create dest dir: ") + mediaSubdir);
        return;
    }

    QHash<QString, QString> srcToBasename;
    QHash<QString, QString> basenameToSrc;

    // 2. Collect all path pathSlots from the data copy.
    QVector<PathSlot> pathSlots = collectSlots(dataCopy);

    // 3. Compute total bytes for progress reporting (best-effort).
    qint64 totalBytes = 0;
    for (int i = 0; i < pathSlots.size(); ++i) {
        QFileInfo fi(pathSlots[i].srcPath);
        if (fi.exists())
            totalBytes += fi.size();
    }

    // 4. Copy files and rewrite paths in dataCopy.
    int    copiedFiles = 0;
    qint64 doneSoFar   = 0;

    for (int i = 0; i < pathSlots.size(); ++i) {
        if (m_cancelled) {
            emit finished(false, QStringLiteral("cancelled"));
            return;
        }

        PathSlot& ps = pathSlots[i];
        const QString& src = ps.srcPath;
        QFileInfo srcInfo(src);

        if (src.isEmpty() || !srcInfo.exists()) {
            if (!src.isEmpty())
                m_warnings.append(QStringLiteral("missing: ") + src);
            continue;
        }

        const QString basename = resolveDestBasename(src, srcToBasename,
                                                     basenameToSrc, mediaSubdir);
        const QString destPath = QDir(mediaSubdir).absoluteFilePath(basename);

        if (!QFile::exists(destPath)) {
            QFile srcFile(src);
            if (!srcFile.open(QIODevice::ReadOnly)) {
                emit finished(false,
                              QStringLiteral("IO error: cannot open ") + src
                              + QStringLiteral(": ") + srcFile.errorString());
                return;
            }
            QFile dstFile(destPath);
            if (!dstFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                emit finished(false,
                              QStringLiteral("IO error: cannot write ") + destPath
                              + QStringLiteral(": ") + dstFile.errorString());
                return;
            }

            while (!srcFile.atEnd()) {
                if (m_cancelled) {
                    dstFile.close();
                    QFile::remove(destPath);
                    emit finished(false, QStringLiteral("cancelled"));
                    return;
                }

                const QByteArray chunk = srcFile.read(kChunkSize);
                if (chunk.isEmpty() && srcFile.error() != QFile::NoError) {
                    dstFile.close();
                    QFile::remove(destPath);
                    emit finished(false,
                                  QStringLiteral("IO error reading ") + src
                                  + QStringLiteral(": ") + srcFile.errorString());
                    return;
                }

                const qint64 written = dstFile.write(chunk);
                if (written != chunk.size()) {
                    emit finished(false,
                                  QStringLiteral("IO error writing ") + destPath
                                  + QStringLiteral(": ") + dstFile.errorString());
                    return;
                }

                m_bytesCopied += written;
                doneSoFar     += written;

                if (totalBytes > 0) {
                    const int pct = static_cast<int>(doneSoFar * 100 / totalBytes);
                    emit progressChanged(pct);
                }
            }
            ++copiedFiles;
        }

        // The loader opens filePath as-is, so avoid CWD-dependent collected projects.
        *ps.field = QDir::cleanPath(destPath);
    }

    if (m_cancelled) {
        emit finished(false, QStringLiteral("cancelled"));
        return;
    }

    // 5. Save the modified copy.
    const QString outPath = destDir + QLatin1Char('/') + projectFileName;
    if (!ProjectFile::save(outPath, dataCopy)) {
        emit finished(false,
                      QStringLiteral("failed to save project file: ") + outPath);
        return;
    }

    emit progressChanged(100);
    emit finished(true,
                  QStringLiteral("Collected %1 files (%2 warnings, %3 bytes)")
                      .arg(copiedFiles)
                      .arg(m_warnings.size())
                      .arg(m_bytesCopied));
}
