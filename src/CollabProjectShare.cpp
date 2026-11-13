#include "CollabProjectShare.h"
#include "JsonNumberClamp.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>

namespace collab::share {

// ---------------------------------------------------------------------------
// exportProject
// ---------------------------------------------------------------------------
bool exportProject(const ProjectData &project,
                   const collab::CommentTrack &commentTrack,
                   const QString &outPath)
{
    // Serialise the project using the existing ProjectFile helper.
    const QString projectJson = ProjectFile::toJsonString(project);
    QJsonDocument projectDoc = QJsonDocument::fromJson(projectJson.toUtf8());
    if (projectDoc.isNull())
        return false;

    QJsonObject root;
    root[QStringLiteral("project")]  = projectDoc.object();
    root[QStringLiteral("comments")] = commentTrack.toJson();
    root[QStringLiteral("history")]  = QJsonArray{};

    QJsonObject meta;
    meta[QStringLiteral("exportedAt")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    meta[QStringLiteral("exporter")] = QStringLiteral("v-simple-editor");
    root[QStringLiteral("meta")] = meta;

    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

// ---------------------------------------------------------------------------
// importProject
// ---------------------------------------------------------------------------
ImportResult importProject(const QString &inPath)
{
    ImportResult result;

    QFile f(inPath);
    if (!f.exists()) {
        result.warning = QStringLiteral("File not found: ") + inPath;
        return result;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        result.warning = QStringLiteral("Cannot open file for reading: ") + inPath;
        return result;
    }

    const QByteArray bytes = f.readAll();
    QJsonParseError parseError;
    QJsonDocument doc = jsonclamp::parseWithClampFallback(bytes, &parseError);
    if (doc.isNull()) {
        result.warning = QStringLiteral("JSON parse error: ") + parseError.errorString();
        return result;
    }

    const QJsonObject root = doc.object();

    if (!root.contains(QStringLiteral("project"))) {
        result.warning = QStringLiteral("Missing 'project' key in import file.");
        return result;
    }

    // Re-serialise the nested project object and parse it via ProjectFile.
    const QJsonObject projectObj = root[QStringLiteral("project")].toObject();
    const QString projectJsonStr =
        QString::fromUtf8(QJsonDocument(projectObj).toJson(QJsonDocument::Compact));

    ProjectData pd;
    if (!ProjectFile::fromJsonString(projectJsonStr, pd)) {
        result.warning = QStringLiteral("Failed to deserialise project data.");
        return result;
    }
    result.project = pd;

    // Comments are optional — missing key just yields a default-constructed track.
    if (root.contains(QStringLiteral("comments"))) {
        result.commentTrack =
            collab::CommentTrack::fromJson(root[QStringLiteral("comments")].toObject());
    }

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// diffProjects
// ---------------------------------------------------------------------------
QList<DiffEntry> diffProjects(const ProjectData &a, const ProjectData &b)
{
    QList<DiffEntry> entries;

    // --- video tracks count ---
    const int aVTracks = a.videoTracks.size();
    const int bVTracks = b.videoTracks.size();
    if (aVTracks != bVTracks) {
        DiffEntry e;
        e.path        = QStringLiteral("videoTracks");
        e.description = QString("Video tracks: %1 → %2").arg(aVTracks).arg(bVTracks);
        e.type        = (bVTracks > aVTracks) ? DiffEntry::Type::Added
                      : (bVTracks < aVTracks) ? DiffEntry::Type::Removed
                                              : DiffEntry::Type::Modified;
        entries.append(e);
    }

    // --- audio tracks count ---
    const int aATracks = a.audioTracks.size();
    const int bATracks = b.audioTracks.size();
    if (aATracks != bATracks) {
        DiffEntry e;
        e.path        = QStringLiteral("audioTracks");
        e.description = QString("Audio tracks: %1 → %2").arg(aATracks).arg(bATracks);
        e.type        = (bATracks > aATracks) ? DiffEntry::Type::Added
                      : (bATracks < aATracks) ? DiffEntry::Type::Removed
                                              : DiffEntry::Type::Modified;
        entries.append(e);
    }

    // --- total clips count ---
    auto countClips = [](const QVector<QVector<ClipInfo>> &tracks) {
        int total = 0;
        for (const auto &t : tracks) total += t.size();
        return total;
    };
    const int aClips = countClips(a.videoTracks) + countClips(a.audioTracks);
    const int bClips = countClips(b.videoTracks) + countClips(b.audioTracks);
    if (aClips != bClips) {
        DiffEntry e;
        e.path        = QStringLiteral("clips");
        e.description = QString("Clips: %1 → %2").arg(aClips).arg(bClips);
        e.type        = (bClips > aClips) ? DiffEntry::Type::Added
                      : (bClips < aClips) ? DiffEntry::Type::Removed
                                          : DiffEntry::Type::Modified;
        entries.append(e);
    }

    // --- total effects count (VideoEffect per ClipInfo) ---
    auto countEffects = [](const QVector<QVector<ClipInfo>> &tracks) {
        int total = 0;
        for (const auto &t : tracks)
            for (const auto &c : t)
                total += c.effects.size();
        return total;
    };
    const int aEffects = countEffects(a.videoTracks) + countEffects(a.audioTracks);
    const int bEffects = countEffects(b.videoTracks) + countEffects(b.audioTracks);
    if (aEffects != bEffects) {
        DiffEntry e;
        e.path        = QStringLiteral("effects");
        e.description = QString("Effects: %1 → %2").arg(aEffects).arg(bEffects);
        e.type        = (bEffects > aEffects) ? DiffEntry::Type::Added
                      : (bEffects < aEffects) ? DiffEntry::Type::Removed
                                              : DiffEntry::Type::Modified;
        entries.append(e);
    }

    return entries;
}

} // namespace collab::share
