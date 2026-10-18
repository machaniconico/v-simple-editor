#include "FcpxmlExporter.h"
#include "Timeline.h"

#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QUrl>
#include <QSet>
#include <QtMath>
#include <QHash>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fcpx::xml {

// ---------------------------------------------------------------------------
// Helper: format seconds as rational time string "numerator/fps s"
// ---------------------------------------------------------------------------

static QString fmtTime(double secs, int fps)
{
    return QString::number(qRound(secs * fps)) + "/" + QString::number(fps) + "s";
}

static QString xmlEsc(QString s)
{
    return s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
}

// ---------------------------------------------------------------------------
// buildXml
// ---------------------------------------------------------------------------

QString buildXml(const QVector<ClipEntry> &clips, const ExporterConfig &config)
{
    const int fps = config.fps;

    // Compute total timeline duration
    double totalDuration = 0.0;
    for (const ClipEntry &c : clips) {
        const double end = c.offset + c.duration;
        if (end > totalDuration)
            totalDuration = end;
    }

    // Build ordered unique-file list and id map in one pass (clips order preserved)
    QVector<const ClipEntry *> uniqueClips;
    QMap<QString, int> assetIdMap;  // filePath -> asset id (1-based)
    {
        int idx = 1;
        for (const ClipEntry &c : clips) {
            if (!assetIdMap.contains(c.filePath)) {
                assetIdMap.insert(c.filePath, idx);
                uniqueClips.append(&c);
                ++idx;
            }
        }
    }

    QString out;
    QTextStream s(&out);
    s.setEncoding(QStringConverter::Utf8);

    // XML declaration + DOCTYPE
    s << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s << "<!DOCTYPE fcpxml>\n";
    s << "<fcpxml version=\"1.10\">\n";

    // --- resources ---
    s << "  <resources>\n";

    // Format resource (r0)
    s << "    <format"
      << " id=\"r0\""
      << " name=\"FFVideoFormat1080p30\""
      << " frameDuration=\"" << config.frameDuration << "\""
      << " width=\""         << config.width         << "\""
      << " height=\""        << config.height        << "\""
      << "/>\n";

    // Asset resources — one per unique file, ids r1..rN
    for (const ClipEntry *c : uniqueClips) {
        const int assetIdx = assetIdMap.value(c->filePath);
        const QString clipName = c->name.isEmpty()
                               ? QFileInfo(c->filePath).baseName()
                               : c->name;
        const QString srcUrl = QUrl::fromLocalFile(c->filePath).toString();

        s << "    <asset"
          << " id=\"r"      << assetIdx          << "\""
          << " name=\""     << xmlEsc(clipName)  << "\""
          << " src=\""      << xmlEsc(srcUrl)    << "\""
          << " start=\"0s\""
          << " duration=\"" << fmtTime(c->duration, fps) << "\""
          << " hasVideo=\"1\""
          << " format=\"r0\""
          << "/>\n";
    }

    s << "  </resources>\n";

    // --- library / event / project / sequence / spine ---
    s << "  <library>\n";
    s << "    <event name=\"" << xmlEsc(config.projectName) << " Event\">\n";
    s << "      <project name=\"" << xmlEsc(config.projectName) << "\">\n";
    s << "        <sequence"
      << " format=\"r0\""
      << " duration=\""  << fmtTime(totalDuration, fps) << "\""
      << " tcStart=\"0s\""
      << " tcFormat=\"NDF\""
      << ">\n";
    s << "          <spine>\n";

    // Lane > 0 clips become children: of their anchor, else of the spine clip
    // covering their start, else of a gap in the hole they start in. The spine
    // itself (lane 0 clips and gaps) never overlaps.
    constexpr double kEps = 1e-9;
    QHash<QString, int> spineByUid;
    QVector<int> spineIdx;
    for (int i = 0; i < clips.size(); ++i) {
        if (clips[i].lane != 0)
            continue;
        spineIdx.append(i);
        if (!clips[i].uid.isEmpty() && !spineByUid.contains(clips[i].uid))
            spineByUid.insert(clips[i].uid, i);
    }
    QHash<int, QVector<int>> childrenOf;  // spine clip index -> lane clip indices
    struct Gap {
        double holeEnd = 0.0;  // next spine clip offset, or infinity after the last
        double offset = 0.0;
        double end = 0.0;
        QVector<int> children;
    };
    QVector<Gap> gaps;
    for (int i = 0; i < clips.size(); ++i) {
        const ClipEntry &c = clips[i];
        if (c.lane == 0)
            continue;
        int parent = -1;
        const int anchor = c.anchorUid.isEmpty() ? -1 : spineByUid.value(c.anchorUid, -1);
        if (anchor >= 0)
            parent = anchor;
        double holeEnd = std::numeric_limits<double>::infinity();
        for (int j : spineIdx) {
            const ClipEntry &p = clips[j];
            if (parent < 0 && p.offset <= c.offset + kEps && c.offset < p.offset + p.duration - kEps)
                parent = j;
            if (p.offset > c.offset + kEps && p.offset < holeEnd)
                holeEnd = p.offset;
        }
        if (parent >= 0) {
            childrenOf[parent].append(i);
            continue;
        }
        int g = 0;
        while (g < gaps.size() && gaps[g].holeEnd != holeEnd)
            ++g;
        if (g == gaps.size()) {
            gaps.append(Gap{holeEnd, c.offset, 0.0, {}});
        }
        Gap &gap = gaps[g];
        gap.offset = qMin(gap.offset, c.offset);
        gap.end = qMax(gap.end, c.offset + c.duration);
        gap.children.append(i);
    }
    for (Gap &gap : gaps) {
        if (std::isfinite(gap.holeEnd))
            gap.end = gap.holeEnd;
    }

    auto clipName = [](const ClipEntry &c) {
        return c.name.isEmpty() ? QFileInfo(c.filePath).baseName() : c.name;
    };
    auto writeClip = [&](const QString &indent, int index, const QString &offsetStr,
                         bool lane, bool open) {
        const ClipEntry &c = clips[index];
        s << indent << "<asset-clip"
          << " ref=\"r"     << assetIdMap.value(c.filePath, 1) << "\"";
        if (lane)
            s << " lane=\"" << c.lane << "\"";
        s << " offset=\""   << offsetStr                        << "\""
          << " duration=\"" << fmtTime(c.duration, fps)         << "\""
          << " start=\""    << fmtTime(c.startInSource, fps)    << "\""
          << " name=\""     << xmlEsc(clipName(c))              << "\""
          << (open ? ">\n" : "/>\n");
    };

    struct SpineItem { double offset; int gap; int clip; };
    QVector<SpineItem> items;
    for (int i : spineIdx)
        items.append({clips[i].offset, -1, i});
    for (int g = 0; g < gaps.size(); ++g)
        items.append({gaps[g].offset, g, -1});
    if (!gaps.isEmpty()) {
        std::stable_sort(items.begin(), items.end(),
                         [](const SpineItem &a, const SpineItem &b) { return a.offset < b.offset; });
    }

    for (const SpineItem &item : items) {
        if (item.gap >= 0) {
            const Gap &gap = gaps[item.gap];
            s << "            <gap"
              << " name=\"Gap\""
              << " offset=\""   << fmtTime(gap.offset, fps)             << "\""
              << " duration=\"" << fmtTime(gap.end - gap.offset, fps)   << "\""
              << " start=\"0s\""
              << ">\n";
            for (int child : gap.children)
                writeClip(QStringLiteral("              "), child,
                          fmtTime(clips[child].offset - gap.offset, fps), true, false);
            s << "            </gap>\n";
            continue;
        }
        const ClipEntry &c = clips[item.clip];
        const QVector<int> kids = childrenOf.value(item.clip);
        const QString indent = QStringLiteral("            ");
        writeClip(indent, item.clip, fmtTime(c.offset, fps), false, !kids.isEmpty());
        if (kids.isEmpty())
            continue;
        for (int child : kids)
            writeClip(indent + QStringLiteral("  "), child,
                      fmtTime(c.startInSource + (clips[child].offset - c.offset), fps), true, false);
        s << indent << "</asset-clip>\n";
    }

    s << "          </spine>\n";
    s << "        </sequence>\n";
    s << "      </project>\n";
    s << "    </event>\n";
    s << "  </library>\n";
    s << "</fcpxml>\n";

    return out;
}

// ---------------------------------------------------------------------------
// exportProject
// ---------------------------------------------------------------------------

bool exportProject(const QString &outPath, const QVector<ClipEntry> &clips,
                   const ExporterConfig &config)
{
    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << buildXml(clips, config);
    return true;
}

} // namespace fcpx::xml

// ---------------------------------------------------------------------------
// buildExportClips
// ---------------------------------------------------------------------------

namespace fcpx {

QVector<xml::ClipEntry> buildExportClips(const Timeline &timeline)
{
    QVector<xml::ClipEntry> entries;

    const auto &tracks = timeline.videoTracks();
    for (int trackIdx = 0; trackIdx < tracks.size(); ++trackIdx) {
        const auto *track = tracks[trackIdx];
        if (!track)
            continue;

        double cursorSec = 0.0;
        const auto &clips = track->clips();
        for (const ClipInfo &clip : clips) {
            cursorSec += clip.leadInSec;

            const double durationSec = qMax(0.0, clip.effectiveDuration());
            if (clip.filePath.isEmpty() || durationSec <= 0.0) {
                cursorSec += durationSec;
                continue;
            }

            xml::ClipEntry entry;
            entry.filePath = clip.filePath;
            entry.offset = qMax(0.0, cursorSec);
            entry.duration = durationSec;
            entry.startInSource = qMax(0.0, clip.inPoint);
            entry.name = clip.displayName.isEmpty()
                ? QFileInfo(clip.filePath).completeBaseName()
                : clip.displayName;
            entry.lane = trackIdx;
            if (trackIdx == 0)
                entry.uid = clip.uid;
            else
                entry.anchorUid = clip.anchorUid;
            entries.append(entry);

            cursorSec += durationSec;
        }
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const xml::ClipEntry &lhs, const xml::ClipEntry &rhs) {
                         if (lhs.offset < rhs.offset)
                             return true;
                         if (lhs.offset > rhs.offset)
                             return false;
                         return lhs.name < rhs.name;
                     });
    return entries;
}

} // namespace fcpx
