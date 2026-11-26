#include "DuplicateFrameDetector.h"
#include "ClipGeometry.h"
#include "ExternalGenerator.h"
#include "Timeline.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace dupframes {

namespace {

QString canonicalMediaKey(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};
    if (path.startsWith(QStringLiteral("veditor://"), Qt::CaseInsensitive))
        return {};
    if (clipgeom::isNullObjectFilePath(path) || extgen::isExternalGeneratorFilePath(path))
        return {};
    QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    clean = clean.toLower();
#endif
    return clean;
}

void collectTrackRefs(const QVector<TimelineTrack *> &tracks,
                      bool isAudio,
                      QHash<QString, QVector<ClipRef>> &bySource,
                      QHash<QString, QString> &displayPathByKey)
{
    for (int t = 0; t < tracks.size(); ++t) {
        const TimelineTrack *tr = tracks[t];
        if (!tr) continue;
        const auto clips = tr->clips();
        double cursor = 0.0;
        for (int c = 0; c < clips.size(); ++c) {
            const ClipInfo &ci = clips[c];
            cursor += qMax(0.0, ci.leadInSec);
            const double dur = qMax(0.0, ci.effectiveDuration());
            const double tlStart = cursor;
            const double tlEnd = cursor + dur;
            cursor = tlEnd;

            if (!ci.visible || ci.isAdjustment || ci.isSequenceReference() || !ci.shapes.isEmpty())
                continue;
            const QString key = canonicalMediaKey(ci.filePath);
            if (key.isEmpty())
                continue;

            const double srcIn = qMax(0.0, ci.inPoint);
            const double srcOut = (ci.outPoint > srcIn) ? ci.outPoint : ci.duration;
            if (srcOut <= srcIn)
                continue;

            ClipRef ref;
            ref.isAudio = isAudio;
            ref.trackIdx = t;
            ref.clipIdx = c;
            ref.clipUid = ci.uid;
            ref.displayName = ci.displayName.isEmpty() ? QFileInfo(ci.filePath).fileName() : ci.displayName;
            ref.timelineStart = tlStart;
            ref.timelineEnd = tlEnd;
            ref.sourceIn = srcIn;
            ref.sourceOut = srcOut;
            bySource[key].append(ref);
            if (!displayPathByKey.contains(key))
                displayPathByKey.insert(key, ci.filePath);
        }
    }
}

} // namespace

QColor colorForGroup(int groupId)
{
    static const QColor kPalette[] = {
        QColor(239, 83, 80),   // Red
        QColor(66, 165, 245),  // Blue
        QColor(102, 187, 106), // Green
        QColor(255, 167, 38),  // Orange
        QColor(171, 71, 188),  // Purple
        QColor(38, 198, 218),  // Cyan
        QColor(236, 64, 122),  // Pink
        QColor(212, 225, 87),  // Lime
    };
    const int n = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
    const int idx = ((groupId > 0 ? groupId - 1 : 0) % n + n) % n;
    return kPalette[idx];
}

AnalysisResult analyzeTimeline(const Timeline *timeline,
                               bool includeVideo,
                               bool includeAudio,
                               double minOverlapSec)
{
    AnalysisResult res;
    if (!timeline)
        return res;

    QHash<QString, QVector<ClipRef>> bySource;
    QHash<QString, QString> displayPathByKey;
    if (includeVideo)
        collectTrackRefs(timeline->videoTracks(), false, bySource, displayPathByKey);
    if (includeAudio)
        collectTrackRefs(timeline->audioTracks(), true, bySource, displayPathByKey);

    QStringList keys = bySource.keys();
    std::sort(keys.begin(), keys.end());

    QSet<QString> affectedKeys;
    int nextGroup = 1;
    const double eps = qMax(1e-6, minOverlapSec);

    for (const QString &key : keys) {
        const auto &refs = bySource[key];
        if (refs.size() < 2)
            continue;

        bool sourceHasOverlap = false;
        int groupId = 0;

        for (int i = 0; i < refs.size(); ++i) {
            for (int j = i + 1; j < refs.size(); ++j) {
                // Do not flag linked V+A pair (one video, one audio) at the same timeline position
                if (refs[i].isAudio != refs[j].isAudio)
                    continue;
                const double ovIn = qMax(refs[i].sourceIn, refs[j].sourceIn);
                const double ovOut = qMin(refs[i].sourceOut, refs[j].sourceOut);
                if (ovOut - ovIn < eps)
                    continue;
                if (!sourceHasOverlap) {
                    sourceHasOverlap = true;
                    groupId = nextGroup++;
                    ++res.distinctSources;
                }
                DuplicateOverlap ov;
                ov.sourcePath = displayPathByKey.value(key, key);
                ov.groupId = groupId;
                ov.groupColor = colorForGroup(groupId);
                ov.overlapSourceIn = ovIn;
                ov.overlapSourceOut = ovOut;
                ov.first = refs[i];
                ov.second = refs[j];
                res.overlaps.append(ov);

                affectedKeys.insert(QStringLiteral("%1:%2:%3")
                    .arg(refs[i].isAudio ? 1 : 0).arg(refs[i].trackIdx).arg(refs[i].clipIdx));
                affectedKeys.insert(QStringLiteral("%1:%2:%3")
                    .arg(refs[j].isAudio ? 1 : 0).arg(refs[j].trackIdx).arg(refs[j].clipIdx));
            }
        }
    }

    res.affectedClipCount = affectedKeys.size();
    return res;
}

} // namespace dupframes
