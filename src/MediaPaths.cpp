#include "MediaPaths.h"

#include "ClipGeometry.h"
#include "ProjectFile.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <utility>

namespace mediapaths {

PathSlot::PathSlot(QString *directField, QString slotCategory)
    : field(directField)
    , category(std::move(slotCategory))
{
}

PathSlot::PathSlot(QString value, QString slotCategory,
                   std::function<void(const QString &)> writer)
    : category(std::move(slotCategory))
    , m_ownedField(new QString(std::move(value)))
    , m_writer(std::move(writer))
{
    field = m_ownedField.data();
}

QString PathSlot::value() const
{
    return field ? *field : QString{};
}

bool PathSlot::setValue(const QString &newValue)
{
    if (!field)
        return false;
    if (m_writer)
        m_writer(newValue);
    *field = newValue;
    return true;
}

namespace {

void appendClipSlots(QVector<PathSlot> &pathSlots,
                     QVector<QVector<ClipInfo>> &tracks,
                     const QString &kind)
{
    for (auto &track : tracks) {
        for (ClipInfo &clip : track) {
            if (!clip.filePath.isEmpty())
                pathSlots.append(PathSlot(&clip.filePath, kind + QStringLiteral(".filePath")));
            if (!clip.lutFilePath.isEmpty())
                pathSlots.append(PathSlot(&clip.lutFilePath, kind + QStringLiteral(".lutFilePath")));
        }
    }
}

void updateNestedField(ProjectData *data, int parentEntryIndex,
                       int sequenceIndex, const QString &trackKey,
                       int trackIndex, int clipIndex,
                       const QString &fieldKey, const QString &newValue)
{
    if (!data || parentEntryIndex < 0
        || parentEntryIndex >= data->clipParentEntries.size()) {
        return;
    }

    ClipParentEntry &entry = data->clipParentEntries[parentEntryIndex];
    QJsonObject store = timeline_nesting::decodeSequenceStoreObject(entry.parentClipId);
    QJsonArray sequences = store.value(QStringLiteral("sequences")).toArray();
    if (sequenceIndex < 0 || sequenceIndex >= sequences.size())
        return;
    QJsonObject sequence = sequences.at(sequenceIndex).toObject();
    QJsonArray tracks = sequence.value(trackKey).toArray();
    if (trackIndex < 0 || trackIndex >= tracks.size())
        return;
    QJsonArray clips = tracks.at(trackIndex).toArray();
    if (clipIndex < 0 || clipIndex >= clips.size())
        return;

    QJsonObject clip = clips.at(clipIndex).toObject();
    clip.insert(fieldKey, newValue);
    clips.replace(clipIndex, clip);
    tracks.replace(trackIndex, clips);
    sequence.insert(trackKey, tracks);
    sequences.replace(sequenceIndex, sequence);
    store.insert(QStringLiteral("sequences"), sequences);
    entry.parentClipId = timeline_nesting::encodeSequenceStoreObject(store);
}

void appendNestedSlots(QVector<PathSlot> &pathSlots, ProjectData &data)
{
    const QString storeKey = timeline_nesting::sequenceStoreParentKey();
    for (int parentIndex = 0; parentIndex < data.clipParentEntries.size(); ++parentIndex) {
        const ClipParentEntry &entry = data.clipParentEntries.at(parentIndex);
        if (entry.clipId != storeKey)
            continue;

        const QJsonObject store =
            timeline_nesting::decodeSequenceStoreObject(entry.parentClipId);
        const QJsonArray sequences = store.value(QStringLiteral("sequences")).toArray();
        for (int sequenceIndex = 0; sequenceIndex < sequences.size(); ++sequenceIndex) {
            const QJsonObject sequence = sequences.at(sequenceIndex).toObject();
            const auto appendTrackArray = [&](const QString &trackKey,
                                              const QString &kind) {
                const QJsonArray tracks = sequence.value(trackKey).toArray();
                for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
                    const QJsonArray clips = tracks.at(trackIndex).toArray();
                    for (int clipIndex = 0; clipIndex < clips.size(); ++clipIndex) {
                        const QJsonObject clip = clips.at(clipIndex).toObject();
                        const auto appendField = [&](const QString &fieldKey) {
                            const QString value = clip.value(fieldKey).toString();
                            if (value.isEmpty())
                                return;
                            pathSlots.append(PathSlot(
                                value,
                                QStringLiteral("nested.") + kind
                                    + QLatin1Char('.') + fieldKey,
                                [&data, parentIndex, sequenceIndex, trackKey,
                                 trackIndex, clipIndex, fieldKey](const QString &replacement) {
                                    updateNestedField(&data, parentIndex, sequenceIndex,
                                                      trackKey, trackIndex, clipIndex,
                                                      fieldKey, replacement);
                                }));
                        };
                        appendField(QStringLiteral("filePath"));
                        appendField(QStringLiteral("lutFilePath"));
                    }
                }
            };
            appendTrackArray(QStringLiteral("videoTracks"), QStringLiteral("video"));
            appendTrackArray(QStringLiteral("audioTracks"), QStringLiteral("audio"));
        }
    }
}

bool isMissingFilesystemPath(const QString &path)
{
    if (clipgeom::isNullObjectFilePath(path)
        || timeline_nesting::isSequenceClipFilePath(path)) {
        return false;
    }
    const QFileInfo info(path);
    return !info.exists() || !info.isFile();
}

} // namespace

QVector<PathSlot> enumeratePathSlots(ProjectData &data)
{
    QVector<PathSlot> pathSlots;
    appendClipSlots(pathSlots, data.videoTracks, QStringLiteral("video"));
    appendClipSlots(pathSlots, data.audioTracks, QStringLiteral("audio"));

    for (ParticleClipEntry &entry : data.particleClipEntries) {
        if (!entry.clipFilePath.isEmpty()) {
            pathSlots.append(PathSlot(&entry.clipFilePath,
                                      QStringLiteral("particle.clipFilePath")));
        }
    }
    for (OverlayItem &overlay : data.overlays) {
        if (overlay.type == QLatin1String("image") && !overlay.text.isEmpty())
            pathSlots.append(PathSlot(&overlay.text, QStringLiteral("overlay.image")));
    }

    appendNestedSlots(pathSlots, data);
    return pathSlots;
}

QStringList missingFiles(const ProjectData &data)
{
    ProjectData copy = data;
    const QVector<PathSlot> pathSlots = enumeratePathSlots(copy);
    QStringList missing;
    QSet<QString> seen;
    for (const PathSlot &slot : pathSlots) {
        const QString path = slot.value();
        if (!isMissingFilesystemPath(path) || seen.contains(path))
            continue;
        seen.insert(path);
        missing.append(path);
    }
    return missing;
}

int replacePaths(ProjectData &data,
                 const QHash<QString, QString> &oldToNew)
{
    if (oldToNew.isEmpty())
        return 0;
    QVector<PathSlot> pathSlots = enumeratePathSlots(data);
    int replaced = 0;
    for (PathSlot &slot : pathSlots) {
        const auto it = oldToNew.constFind(slot.value());
        if (it == oldToNew.cend() || it.value().isEmpty())
            continue;
        if (slot.setValue(it.value()))
            ++replaced;
    }
    return replaced;
}

} // namespace mediapaths
