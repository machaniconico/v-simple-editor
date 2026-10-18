#include "FcpxmlImport.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QStringView>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>

namespace fcpx::xml {
namespace {
// Nonnegative, reduced rational times. Checked arithmetic keeps all placement
// calculations exact until the final conversion into Timeline's seconds.
struct Time {
    qint64 n = 0, d = 1;
    double seconds() const { return double(n) / double(d); }
};
bool time(const QString &value, Time &out)
{
    if (!value.endsWith('s')) return false;
    const auto parts = value.chopped(1).split('/');
    if (parts.size() > 2) return false;
    bool a = false, b = true;
    const qint64 n = parts[0].toLongLong(&a);
    const qint64 d = parts.size() == 2 ? parts[1].toLongLong(&b) : 1;
    if (!a || !b || n < 0 || d <= 0) return false;
    const qint64 g = std::gcd(n, d);
    out = {n / g, d / g};
    return true;
}
bool combine(Time a, Time b, bool subtract, Time &out)
{
    const qint64 g = std::gcd(a.d, b.d), x = b.d / g, y = a.d / g;
    const auto max = std::numeric_limits<qint64>::max();
    if (a.n > max / x || b.n > max / y || a.d > max / x) return false;
    const qint64 left = a.n * x, right = b.n * y;
    if (subtract ? left < right : left > max - right) return false;
    out = {subtract ? left - right : left + right, a.d * x};
    const qint64 divisor = std::gcd(out.n, out.d);
    out.n /= divisor; out.d /= divisor;
    return true;
}
bool lessTime(Time a, Time b)
{
    bool reverse = false;
    for (;;) {
        if (a.n / a.d != b.n / b.d)
            return reverse ? a.n / a.d > b.n / b.d : a.n / a.d < b.n / b.d;
        a.n %= a.d; b.n %= b.d;
        if (!a.n || !b.n) return reverse ? a.n > b.n : a.n < b.n;
        std::swap(a.n, a.d); std::swap(b.n, b.d); reverse = !reverse;
    }
}
// hasAudio: a compound clip whose sequence holds audio clips.
struct Asset { QString path, name, format; Time start, duration; bool audioOnly = false, hasAudio = false; };
struct Entry {
    QString ref, name, uid;
    int lane = 0, parentIndex = -1;
    bool refClip = false; // ref-clip: ref names a media (compound clip)
    Time offset, duration, start;
    Time handleEnd; // pre-trim source end (start + duration) of an overlap-trimmed clip
    bool underGap = false;
    bool audioElement = false;
    bool compoundAudioCopy = false; // audio-track reference of a mixed ref-clip
};
// A spine-level transition remembers its neighbouring spine clips (entry
// indexes, -1 when the neighbour is missing or a gap) until placement is known.
struct SpineTransition { QString name; Time duration; int prev = -1, next = -1; qint64 line = 0; };
bool isMediaClipTag(QStringView tag)
{
    return tag == QLatin1String("asset-clip") || tag == QLatin1String("video")
        || tag == QLatin1String("audio");
}
bool isContainerTag(QStringView tag)
{
    return tag == QLatin1String("clip") || tag == QLatin1String("sync-clip");
}
// A media's <sequence> rewritten as a standalone document for parseDocument.
struct Compound { QString name, document; };
// Shared by the document and its compound clips: resources, the media ids
// being read (outermost first), the nesting level of the document being read
// (0 = the root), the deepest level a kept reference reached each media at,
// and the sequences read so far.
struct Nesting {
    QMap<QString, Asset> assets;
    QMap<QString, QXmlStreamAttributes> formats;
    QMap<QString, Compound> media;
    QSet<QString> built, found, missing;
    QStringList chain;
    int level = 0;
    QMap<QString, int> depths;
    QVector<TimelineSequence> sequences;
};
constexpr int kMaxCompoundDepth = 8;
}
// originOut / contentEndOut receive the sequence tcStart and the end of its
// placed clips (relative to tcStart) for a compound clip's asset range.
static ImportResult parseDocument(const QString &xml, const QString &directory, Nesting &nest,
                                  Time *originOut, Time *contentEndOut)
{
    ImportResult result;
    QXmlStreamReader reader(xml);
    QMap<QString, Asset> &assets = nest.assets;
    auto isAudioEntry = [&](const Entry &e) { return e.audioElement || assets.value(e.ref).audioOnly; };
    QMap<QString, QXmlStreamAttributes> &formats = nest.formats;
    QVector<Entry> entries;
    int assetClipCount = 0;
    QStringList stack;
    Time origin;
    QString sequenceFormat;
    bool root = false, sequence = false, spine = false;
    int gapDepth = 0;
    QVector<SpineTransition> transitions;
    int lastSpineRoot = -1;
    bool awaitingNext = false;
    auto warnSkip = [&] {
        result.warnings.append(QStringLiteral("未対応の要素を読み飛ばしました: %1 (行 %2)")
            .arg(reader.name().toString()).arg(reader.lineNumber()));
        reader.skipCurrentElement();
    };
    auto readTime = [&](const QXmlStreamAttributes &a, const QString &key, Time &v, bool required = false) {
        if (!a.hasAttribute(key) && !required) return true;
        if (time(a.value(key).toString(), v)) return true;
        reader.raiseError(QStringLiteral("不正な時刻: %1").arg(key)); return false;
    };
    // XML times remain nonnegative. Reorder subtraction for a J cut so its
    // relative position may be negative without storing a negative rational.
    auto placeConnected = [&](Entry &e, const Time &parentOffset, const Time &parentStart) {
        Time absoluteTimelineSec, trimTimelineSec;
        bool beforeZero = false;
        if (!lessTime(e.offset, parentStart)) {
            Time relativeTimelineSec;
            if (!combine(e.offset, parentStart, true, relativeTimelineSec)
                || !combine(parentOffset, relativeTimelineSec, false, absoluteTimelineSec)) return false;
        } else {
            Time relativeTimelineSec;
            if (!combine(parentStart, e.offset, true, relativeTimelineSec)) return false;
            if (!combine(parentOffset, relativeTimelineSec, true, absoluteTimelineSec)) {
                if (!combine(relativeTimelineSec, parentOffset, true, trimTimelineSec)) return false;
                beforeZero = true;
            }
        }
        if (beforeZero) {
            if (!combine(trimTimelineSec, origin, false, trimTimelineSec)) return false;
            absoluteTimelineSec = origin;
        } else if (lessTime(absoluteTimelineSec, origin)) {
            if (!combine(origin, absoluteTimelineSec, true, trimTimelineSec)) return false;
            absoluteTimelineSec = origin;
        }
        if (trimTimelineSec.n) {
            Time sourceStartSec, remainingLocalSec;
            if (!combine(e.start, trimTimelineSec, false, sourceStartSec)
                || !combine(e.duration, trimTimelineSec, true, remainingLocalSec)
                || !remainingLocalSec.n) return false;
            e.start = sourceStartSec;
            e.duration = remainingLocalSec;
            result.warnings.append(QStringLiteral("接続クリップをタイムライン 0 で切り詰めました: %1 / %2")
                .arg(e.name, e.ref));
        }
        e.offset = absoluteTimelineSec;
        return true;
    };
    std::function<void()> warnDescendants;
    warnDescendants = [&] {
        while (reader.readNextStartElement()) {
            if (isMediaClipTag(reader.name())) {
                const auto a = reader.attributes();
                ++assetClipCount;
                result.warnings.append(QStringLiteral("親クリップが不正なため接続クリップを読み飛ばしました: %1 / %2")
                    .arg(a.value("name").toString(), a.value("ref").toString()));
            }
            warnDescendants();
        }
    };
    // A ref-clip reads its media sequence once (inner sequences first in
    // nest.sequences), at the deepest level a kept reference reaches the media
    // at, so no reference path through it passes kMaxCompoundDepth (the
    // renderer drops a nested sequence beyond it). Every reference is checked
    // for a cycle and for depth, even to a media already read; only a kept
    // reference records its level. False: skipped with a warning. An undefined
    // media falls through to the missing-asset warning.
    auto keepCompound = [&](const QString &id) {
        if (!nest.media.contains(id)) return true;
        QString skipped;
        if (nest.chain.contains(id)) {
            skipped = QStringLiteral("compound clip の循環参照を読み飛ばしました: %1").arg(id);
        } else if (nest.level >= kMaxCompoundDepth) {
            skipped = QStringLiteral("compound clip の入れ子が深さ %1 を超えたため読み飛ばしました: %2")
                .arg(kMaxCompoundDepth).arg(id);
        } else {
            nest.depths.insert(id, qMax(nest.depths.value(id, nest.level), nest.level));
            if (nest.built.contains(id)) return true;
            const Compound compound = nest.media.value(id);
            Time innerOrigin, innerEnd;
            const int outerLevel = nest.level;
            nest.chain.append(id);
            nest.level = nest.depths.value(id) + 1;
            const ImportResult inner = parseDocument(compound.document, directory, nest, &innerOrigin, &innerEnd);
            nest.level = outerLevel;
            nest.chain.removeLast();
            const QStringList existing = result.warnings;
            for (const QString &warning : inner.warnings)
                if (!existing.contains(warning)) result.warnings.append(warning);
            if (inner.error.isEmpty()) {
                auto hasClips = [](const QVector<QVector<ClipInfo>> &tracks) {
                    return std::any_of(tracks.cbegin(), tracks.cend(), [](const auto &t) { return !t.isEmpty(); });
                };
                TimelineSequence sequence;
                sequence.id = id;
                sequence.name = compound.name.isEmpty() ? id : compound.name;
                sequence.videoTracks = inner.videoTracks;
                sequence.audioTracks = inner.audioTracks;
                if (sequence.videoTracks.isEmpty()) sequence.videoTracks.append(QVector<ClipInfo>{});
                if (sequence.audioTracks.isEmpty()) sequence.audioTracks.append(QVector<ClipInfo>{});
                nest.sequences.append(sequence);
                Asset asset;
                asset.name = sequence.name;
                asset.start = innerOrigin;
                asset.duration = innerEnd;
                asset.hasAudio = hasClips(inner.audioTracks);
                asset.audioOnly = !hasClips(inner.videoTracks) && asset.hasAudio;
                assets.insert(id, asset);
                nest.built.insert(id);
                return true;
            }
            skipped = QStringLiteral("compound clip を読み込めないため読み飛ばしました: %1 (%2)").arg(id, inner.error);
        }
        result.warnings.append(skipped);
        return false;
    };
    std::function<void(const Time *, const Time *, int, int, const QXmlStreamAttributes *)> readSpine;
    readSpine = [&](const Time *parentOffset, const Time *parentStart, int parentIndex,
                    int depth, const QXmlStreamAttributes *fallback) {
        const QStringView tag = reader.name();
        if (isContainerTag(tag)) {
            const auto container = reader.attributes();
            Entry frame;
            bool valid = true;
            if (container.hasAttribute("offset"))
                valid = time(container.value("offset").toString(), frame.offset);
            if (container.hasAttribute("start"))
                valid &= time(container.value("start").toString(), frame.start);
            if (parentOffset && valid)
                valid = placeConnected(frame, *parentOffset, *parentStart);
            if (!valid) {
                result.warnings.append(QStringLiteral("クリップの値が不正です (接続位置): %1 / %2")
                    .arg(container.value("name").toString(), container.value("ref").toString()));
                warnDescendants();
                return;
            }
            QVector<int> directChildren;
            while (reader.readNextStartElement()) {
                const QStringView childTag = reader.name();
                if (isMediaClipTag(childTag)) {
                    const int index = entries.size();
                    readSpine(&frame.offset, &frame.start, parentIndex, depth + 1, &container);
                    if (entries.size() > index) directChildren.append(index);
                } else warnSkip();
            }
            if (directChildren.isEmpty()) return;
            int primary = directChildren.first();
            for (int index : directChildren) {
                if (assets.contains(entries[index].ref) && !isAudioEntry(entries[index])) {
                    primary = index;
                    break;
                }
            }
            if (parentIndex < 0 && depth == 0)
                entries[primary].uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            for (int index : directChildren) {
                if (index != primary) entries[index].parentIndex = primary;
            }
            return;
        }
        if (tag == QLatin1String("gap")) {
            const auto a = reader.attributes();
            Entry anchor;
            bool valid = true;
            if (a.hasAttribute("offset")) valid = time(a.value("offset").toString(), anchor.offset);
            if (a.hasAttribute("start")) valid &= time(a.value("start").toString(), anchor.start);
            if (parentOffset && valid) valid = placeConnected(anchor, *parentOffset, *parentStart);
            if (!valid) {
                result.warnings.append(QStringLiteral("gap の接続位置が不正です: %1")
                    .arg(a.value("name").toString()));
                warnDescendants();
                return;
            }
            ++gapDepth;
            while (reader.readNextStartElement())
                readSpine(&anchor.offset, &anchor.start, parentIndex, depth + 1, nullptr);
            --gapDepth;
            return;
        }
        const bool refClip = tag == QLatin1String("ref-clip");
        if (refClip && !keepCompound(reader.attributes().value("ref").toString())) { warnDescendants(); return; }
        if (!refClip && !isMediaClipTag(tag)) { warnSkip(); return; }
        ++assetClipCount;
        const auto a = reader.attributes();
        const bool explicitOffset = a.hasAttribute("offset");
        auto attribute = [&](const QString &key) {
            return a.hasAttribute(key) ? a.value(key).toString()
                : fallback && !(explicitOffset && key == QLatin1String("start"))
                    ? fallback->value(key).toString() : QString();
        };
        auto hasAttribute = [&](const QString &key) {
            return a.hasAttribute(key) || (fallback && !(explicitOffset && key == QLatin1String("start"))
                && fallback->hasAttribute(key));
        };
        Entry e; e.ref = attribute(QStringLiteral("ref")); e.name = attribute(QStringLiteral("name"));
        e.parentIndex = parentIndex;
        e.underGap = gapDepth > 0;
        e.audioElement = tag == QLatin1String("audio");
        e.refClip = refClip;
        bool laneOk = true;
        if (hasAttribute(QStringLiteral("lane"))) e.lane = attribute(QStringLiteral("lane")).toInt(&laneOk);
        QString invalid;
        if (!laneOk) invalid = QStringLiteral("lane");
        for (const auto &key : {QStringLiteral("offset"), QStringLiteral("duration"), QStringLiteral("start")}) {
            Time *value = key == QStringLiteral("offset") ? &e.offset
                : key == QStringLiteral("duration") ? &e.duration : &e.start;
            if ((hasAttribute(key) || key == QStringLiteral("duration"))
                && !time(attribute(key), *value)) invalid = key;
        }
        if (fallback && !explicitOffset) e.offset = *parentOffset;
        if (parentOffset && !(fallback && !explicitOffset) && invalid.isEmpty()) {
            if (!placeConnected(e, *parentOffset, *parentStart))
                invalid = QStringLiteral("接続位置");
        }
        if (!invalid.isEmpty()) {
            result.warnings.append(QStringLiteral("クリップの値が不正です (%1): %2 / %3")
                .arg(invalid, e.name, e.ref));
            warnDescendants();
            return;
        }
        const int entryIndex = entries.size();
        if (parentIndex < 0 && depth == 0)
            e.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entries.append(e);
        while (reader.readNextStartElement()) {
            readSpine(&e.offset, &e.start, entryIndex, depth + 1, nullptr);
        }
    };
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isEndElement()) { if (!stack.isEmpty()) stack.removeLast(); continue; }
        if (!reader.isStartElement()) continue;
        const QString tag = reader.name().toString(), parent = stack.value(stack.size() - 1);
        const auto a = reader.attributes();
        if (stack.isEmpty()) {
            if (root || tag != QStringLiteral("fcpxml")) { reader.raiseError(QStringLiteral("FCPXML ではありません。")); break; }
            root = true;
        } else if (tag == "resources" && parent == "fcpxml") {
        } else if (tag == "format" && parent == "resources") {
            formats.insert(a.value("id").toString(), a);
            reader.skipCurrentElement(); continue;
        } else if (tag == "asset" && parent == "resources") {
            Asset asset;
            asset.path = a.value("src").toString(); asset.name = a.value("name").toString();
            asset.format = a.value("format").toString();
            asset.audioOnly = a.value("hasVideo") == QLatin1String("0")
                || (!a.hasAttribute("hasVideo") && a.value("hasAudio") == QLatin1String("1"));
            if (!readTime(a, QStringLiteral("start"), asset.start)
                || !readTime(a, QStringLiteral("duration"), asset.duration)) break;
            const QString id = a.value("id").toString();
            if (id.isEmpty() || assets.contains(id) || nest.media.contains(id)) { reader.raiseError(QStringLiteral("素材 ID が不正です。")); break; }
            while (reader.readNextStartElement()) {
                if (reader.name() == QLatin1String("media-rep") && reader.attributes().value("kind") == QLatin1String("original-media")) {
                    asset.path = reader.attributes().value("src").toString(); reader.skipCurrentElement();
                } else warnSkip();
            }
            assets.insert(id, asset); continue;
        } else if (tag == "media" && parent == "resources") {
            const QString id = a.value("id").toString();
            if (id.isEmpty() || assets.contains(id) || nest.media.contains(id)) { reader.raiseError(QStringLiteral("素材 ID が不正です。")); break; }
            Compound compound;
            compound.name = a.value("name").toString();
            while (reader.readNextStartElement()) {
                if (reader.name() != QLatin1String("sequence") || !compound.document.isEmpty()) { warnSkip(); continue; }
                QXmlStreamWriter writer(&compound.document);
                writer.writeStartElement(QStringLiteral("fcpxml"));
                writer.writeStartElement(QStringLiteral("project"));
                writer.writeAttribute(QStringLiteral("name"), compound.name);
                for (int depth = 0; !reader.hasError(); reader.readNext()) {
                    writer.writeCurrentToken(reader);
                    if (reader.isStartElement()) ++depth;
                    else if (reader.isEndElement() && --depth == 0) break;
                }
                writer.writeEndElement();
                writer.writeEndElement();
            }
            nest.media.insert(id, compound); continue;
        } else if ((tag == "library" && parent == "fcpxml")
                   || (tag == "event" && (parent == "library" || parent == "fcpxml"))) {
        } else if (tag == "project" && (parent == "event" || parent == "fcpxml")) {
            if (sequence) { warnSkip(); continue; }
            result.title = a.value("name").toString();
        } else if (tag == "sequence" && parent == "project" && !sequence) {
            sequence = true; sequenceFormat = a.value("format").toString();
            if (!readTime(a, QStringLiteral("tcStart"), origin)) break;
        } else if (tag == "spine" && parent == "sequence" && !spine) {
            spine = true;
        } else if (parent == "spine" && tag == "transition") {
            SpineTransition transition;
            transition.name = a.value("name").toString();
            transition.line = reader.lineNumber();
            if (!time(a.value("duration").toString(), transition.duration) || !transition.duration.n) {
                warnSkip(); continue;
            }
            transition.prev = lastSpineRoot;
            transitions.append(transition);
            awaitingNext = true;
            reader.skipCurrentElement(); continue;
        } else if (parent == "spine" && (tag == "asset-clip" || tag == "gap" || tag == "ref-clip"
                   || isContainerTag(QStringView(tag)))) {
            const int firstEntry = entries.size();
            readSpine(nullptr, nullptr, -1, 0, nullptr);
            int root = -1;
            if (tag != "gap") {
                for (int i = firstEntry; i < entries.size() && root < 0; ++i)
                    if (!entries[i].uid.isEmpty()) root = i;
                if (awaitingNext) transitions.last().next = root;
            }
            awaitingNext = false;
            lastSpineRoot = root;
            continue;
        } else { warnSkip(); continue; }
        stack.append(tag);
    }
    if (reader.hasError()) result.error = QStringLiteral("FCPXML 読み込みエラー (行 %1): %2").arg(reader.lineNumber()).arg(reader.errorString());
    else if (!root || !sequence || !spine) result.error = QStringLiteral("読み込めるシーケンスがありません。");
    if (!result.error.isEmpty()) return result;
    // Format metadata is interpreted but cannot replace global project settings
    // when importing into a new sequence of an existing project.
    QSet<QString> usedFormats;
    if (!sequenceFormat.isEmpty()) usedFormats.insert(sequenceFormat);
    for (const auto &e : entries) if (assets.contains(e.ref) && !assets[e.ref].format.isEmpty()) usedFormats.insert(assets[e.ref].format);
    for (const auto &id : usedFormats) {
        const auto f = formats.value(id);
        Time frame;
        if (f.isEmpty() || !time(f.value("frameDuration").toString(), frame) || frame.n == 0)
            result.warnings.append(QStringLiteral("未解決または不正なフォーマット: %1").arg(id));
        else result.warnings.append(QStringLiteral("素材フォーマット %1: %2×%3、フレーム時間 %4。プロジェクトの映像設定を維持します。")
            .arg(id, f.value("width").toString(), f.value("height").toString(), f.value("frameDuration").toString()));
    }
    QMap<int, QVector<Entry>> videoLanes, audioLanes;
    QVector<bool> hasConnectedChildren(entries.size(), false);
    for (const auto &e : entries)
        if (e.parentIndex >= 0) hasConnectedChildren[e.parentIndex] = true;
    // A container may list its audio child before the video chosen as primary.
    // Validate the parent chain before emitting clips in XML order.
    enum Rejection { Valid, InvalidParent, MissingAsset, InvalidRange };
    QVector<int> rejection(entries.size(), -1);
    std::function<int(int)> checkEntry = [&](int i) -> int {
        if (rejection[i] >= 0) return rejection[i];
        const Entry &e = entries[i];
        const int parentRejection = e.parentIndex >= 0 ? checkEntry(e.parentIndex) : Valid;
        if (parentRejection == InvalidParent || parentRejection == InvalidRange)
            return rejection[i] = InvalidParent;
        if (!assets.contains(e.ref) || e.refClip != nest.built.contains(e.ref)) return rejection[i] = MissingAsset;
        if (hasConnectedChildren[i]) {
            Time timelineOffset, sourceStart, sourceEnd, timelineEnd;
            const Asset asset = assets.value(e.ref);
            if (!e.duration.n || !combine(e.offset, origin, true, timelineOffset)
                || !combine(e.start, asset.start, true, sourceStart)
                || !combine(sourceStart, e.duration, false, sourceEnd)
                || !combine(timelineOffset, e.duration, false, timelineEnd))
                return rejection[i] = InvalidRange;
        }
        return rejection[i] = Valid;
    };
    // FCP may deliver a transition as clips overlapping by its length. The
    // sequential model cannot hold an overlap, so both clips are trimmed to the
    // midpoint of the overlap; the trimmed media stays available as handle.
    // A pair is trimmed when both clips survive validation, share a kind and a lane.
    // Placement may still put them on different tracks (the overlap with an earlier
    // clip promotes one of them); such a pair loses its transition, so its trim is
    // restored to the untrimmed entries and the import is rebuilt without it.
    // Same time-range predicate importLanes applies; the trim must not move a clip
    // that is dropped there, nor let the trim validate a clip that was invalid.
    auto rangeValid = [&](const Entry &e, Time &offset, Time &start, Time &out, Time &nextEnd) {
        const Asset asset = assets.value(e.ref);
        return e.duration.n && combine(e.offset, origin, true, offset)
            && combine(e.start, asset.start, true, start)
            && combine(start, e.duration, false, out)
            && combine(offset, e.duration, false, nextEnd);
    };
    const auto untrimmed = entries;
    QSet<int> rejectedTrims;
    QSet<QString> found, missing, v1Uids;
    const int warningsBase = result.warnings.size();
    Time contentEnd;
    auto locate = [&](int entryIndex, const QVector<ClipInfo> *&track) -> ClipInfo * {
        if (entryIndex < 0 || entries[entryIndex].uid.isEmpty()) return nullptr;
        for (auto *tracks : {&result.videoTracks, &result.audioTracks})
            for (auto &candidate : *tracks)
                for (auto &c : candidate)
                    if (c.uid == entries[entryIndex].uid) { track = &candidate; return &c; }
        return nullptr;
    };
    for (;;) {
    entries = untrimmed;
    QVector<int> trimmedTransitions;
    rejection.fill(-1);
    videoLanes.clear();
    audioLanes.clear();
    found.clear(); missing.clear(); v1Uids.clear();
    result.warnings.resize(warningsBase);
    contentEnd = Time{};
    for (int ti = 0; ti < transitions.size(); ++ti) {
        const auto &transition = transitions[ti];
        if (rejectedTrims.contains(ti) || transition.prev < 0 || transition.next < 0) continue;
        Entry &a = entries[transition.prev], &b = entries[transition.next];
        if (checkEntry(transition.prev) != Valid || checkEntry(transition.next) != Valid
            || isAudioEntry(a) != isAudioEntry(b) || a.lane != b.lane) continue;
        Time r0, r1, r2, r3;
        if (!rangeValid(a, r0, r1, r2, r3) || !rangeValid(b, r0, r1, r2, r3)) continue;
        Time endA, overlap, half, aDuration, bOffset, bStart, bDuration, aHandleEnd;
        if (!combine(a.offset, a.duration, false, endA) || !lessTime(b.offset, endA)
            || !combine(endA, b.offset, true, overlap)
            || overlap.d > std::numeric_limits<qint64>::max() / 2) continue;
        half = {overlap.n, overlap.d * 2};
        const qint64 g = std::gcd(half.n, half.d);
        half.n /= g; half.d /= g;
        if (!combine(a.duration, half, true, aDuration) || !aDuration.n
            || !combine(b.offset, half, false, bOffset) || !combine(b.start, half, false, bStart)
            || !combine(b.duration, half, true, bDuration) || !bDuration.n
            || !combine(a.start, a.duration, false, aHandleEnd)) continue;
        a.handleEnd = aHandleEnd;
        a.duration = aDuration;
        b.offset = bOffset; b.start = bStart; b.duration = bDuration;
        trimmedTransitions.append(ti);
    }
    for (int i = 0; i < entries.size(); ++i) {
        const auto &e = entries[i];
        switch (checkEntry(i)) {
        case InvalidParent:
            result.warnings.append(QStringLiteral("親クリップが不正なため接続クリップを読み飛ばしました: %1 / %2")
                .arg(e.name, e.ref));
            continue;
        case MissingAsset:
            result.warnings.append(QStringLiteral("未定義の素材参照を読み飛ばしました: %1").arg(e.ref));
            continue;
        case InvalidRange:
            result.warnings.append(QStringLiteral("lane %1 の時刻範囲が不正なクリップを読み飛ばしました: %2 / %3")
                .arg(e.lane).arg(e.name, e.ref));
            continue;
        default:
            break;
        }
        (isAudioEntry(e) ? audioLanes : videoLanes)[e.lane].append(e);
        // A compound with video and audio clips is referenced from a video and
        // an audio track, like Timeline::addSequenceClip, so its nested audio
        // plays. The audio copy carries no uid (Timeline assigns its own) and
        // keeps the parent, so a connected copy anchors with its video.
        if (e.refClip && !isAudioEntry(e) && assets.value(e.ref).hasAudio) {
            Entry audioCopy = e;
            audioCopy.uid.clear();
            audioCopy.compoundAudioCopy = true;
            audioLanes[e.lane].append(audioCopy);
        }
    }
    result.videoTracks.clear();
    result.audioTracks.clear();
    auto importLanes = [&](const QMap<int, QVector<Entry>> &lanes, QVector<QVector<ClipInfo>> &tracks) {
        auto laneKeys = lanes.keys();
        std::sort(laneKeys.begin(), laneKeys.end(), [](int a, int b) {
            if (a == 0 || b == 0) return a == 0 && b != 0;
            const qint64 absA = a < 0 ? -qint64(a) : qint64(a);
            const qint64 absB = b < 0 ? -qint64(b) : qint64(b);
            if (absA != absB) return absA < absB;
            return a > b;
        });
        for (int lane : laneKeys) {
            auto clips = lanes.value(lane);
            // Stable ordering via Euclidean continued fractions avoids overflowing
            // cross-products and does not round rational offsets to seconds.
            std::stable_sort(clips.begin(), clips.end(), [](const Entry &a, const Entry &b) {
                return lessTime(a.offset, b.offset);
            });
            QVector<QVector<ClipInfo>> laneTracks;
            QVector<Time> laneEnds;
            for (const auto &e : clips) {
                const auto asset = assets.value(e.ref);
                Time offset, gap, start, out, nextEnd;
                if (!rangeValid(e, offset, start, out, nextEnd)) {
                    result.warnings.append(QStringLiteral("lane %1 の時刻範囲が不正なクリップを読み飛ばしました: %2 / %3")
                        .arg(lane).arg(e.name, e.ref));
                    continue;
                }
                int trackIndex = 0;
                while (trackIndex < laneEnds.size() && lessTime(offset, laneEnds[trackIndex]))
                    ++trackIndex;
                if (trackIndex == laneTracks.size()) {
                    laneTracks.append(QVector<ClipInfo>{});
                    laneEnds.append(Time{});
                }
                if (!combine(offset, laneEnds[trackIndex], true, gap)) {
                    result.warnings.append(QStringLiteral("lane %1 の時刻範囲が不正なクリップを読み飛ばしました: %2 / %3")
                        .arg(lane).arg(e.name, e.ref));
                    continue;
                }
                QString path;
                if (e.refClip) {
                    path = timeline_nesting::sequenceClipFilePath(e.ref);
                } else {
                    const QUrl url(asset.path);
                    path = url.isLocalFile() ? url.toLocalFile() : asset.path;
                    if (path.isEmpty()) path = QStringLiteral("offline-%1").arg(e.ref);
                    if (QDir::isRelativePath(path)) path = QDir(directory).absoluteFilePath(path);
                    if (QFileInfo(path).isFile()) found.insert(path);
                    else if (!missing.contains(path)) {
                        missing.insert(path);
                        if (!nest.missing.contains(path))
                            result.warnings.append(QStringLiteral("オフライン素材: %1").arg(path));
                    }
                }
                ClipInfo c; c.filePath = path; c.displayName = e.name.isEmpty() ? asset.name : e.name;
                if (e.refClip) c.sequenceRefId = e.ref;
                c.inPoint = start.seconds();
                c.outPoint = out.seconds();
                c.duration = asset.duration.n > 0
                    ? qMax(c.outPoint, asset.duration.seconds()) : c.outPoint;
                Time trimmedEnd;
                if (e.handleEnd.n > 0 && combine(e.handleEnd, asset.start, true, trimmedEnd))
                    c.duration = qMax(c.duration, trimmedEnd.seconds());
                c.leadInSec = gap.seconds();
                c.uid = e.uid;
                const bool childOnV1 = &tracks == &result.videoTracks
                    && tracks.isEmpty() && trackIndex == 0;
                if (e.parentIndex >= 0 && !e.underGap) {
                    int rootIndex = e.parentIndex;
                    while (entries[rootIndex].parentIndex >= 0)
                        rootIndex = entries[rootIndex].parentIndex;
                    const Entry &ancestor = entries[rootIndex];
                    if (!ancestor.uid.isEmpty() && assets.contains(ancestor.ref)) {
                        // The detach warnings come once per compound, from its video reference.
                        if (isAudioEntry(ancestor)) {
                            if (!e.compoundAudioCopy)
                                result.warnings.append(QStringLiteral("主ストーリーラインが音声クリップのため接続を解除しました: %1")
                                    .arg(c.displayName));
                        } else if (!v1Uids.contains(ancestor.uid) || childOnV1 || e.lane == 0) {
                            if (!e.compoundAudioCopy)
                                result.warnings.append(QStringLiteral("接続先の主ストーリーラインのクリップが V1 に置かれないため接続を解除しました: %1")
                                    .arg(c.displayName));
                        } else {
                            c.anchorUid = ancestor.uid;
                            Time delta;
                            if (!lessTime(e.offset, ancestor.offset))
                                c.anchorOffsetSec = combine(e.offset, ancestor.offset, true, delta)
                                    ? delta.seconds() : e.offset.seconds() - ancestor.offset.seconds();
                            else
                                c.anchorOffsetSec = combine(ancestor.offset, e.offset, true, delta)
                                    ? -delta.seconds() : e.offset.seconds() - ancestor.offset.seconds();
                        }
                    }
                }
                if (childOnV1 && !c.uid.isEmpty())
                    v1Uids.insert(c.uid);
                laneTracks[trackIndex].append(c);
                laneEnds[trackIndex] = nextEnd;
                if (lessTime(contentEnd, nextEnd)) contentEnd = nextEnd;
            }
            for (const auto &track : laneTracks) {
                if (track.isEmpty()) continue;
                const int targetTrack = tracks.size() + 1;
                if (lane < 0 && &tracks == &result.videoTracks)
                    result.warnings.append(QStringLiteral("lane %1 の映像は FCP では主ストーリーラインの下に合成されます。V%2 に置きました。")
                        .arg(lane).arg(targetTrack));
                tracks.append(track);
            }
        }
    };
    importLanes(videoLanes, result.videoTracks);
    importLanes(audioLanes, result.audioTracks);
    bool rebuild = false;
    for (int ti : trimmedTransitions) {
        const QVector<ClipInfo> *trackA = nullptr, *trackB = nullptr;
        const auto &transition = transitions[ti];
        if (!locate(transition.prev, trackA) || !locate(transition.next, trackB) || trackA == trackB) continue;
        rejectedTrims.insert(ti);
        rebuild = true;
    }
    if (!rebuild) break;
    }
    if (originOut) *originOut = origin;
    if (contentEndOut) *contentEndOut = contentEnd;
    // Attach spine transitions to the clip edges, like applyTransitionToSelected.
    auto twice = [](Time t) { Time out; return combine(t, t, false, out) ? out : t; };
    for (const auto &transition : transitions) {
        const QVector<ClipInfo> *trackA = nullptr, *trackB = nullptr;
        ClipInfo *clipA = locate(transition.prev, trackA), *clipB = locate(transition.next, trackB);
        Time length = transition.duration;
        Transition applied;
        if (transition.prev >= 0 && transition.next >= 0) {
            if (!clipA || !clipB) continue;
            const Entry &a = entries[transition.prev], &b = entries[transition.next];
            Time endA;
            if (trackA != trackB || !combine(a.offset, a.duration, false, endA)
                || lessTime(endA, b.offset) || lessTime(b.offset, endA)) {
                result.warnings.append(QStringLiteral("未対応の要素を読み飛ばしました: transition (行 %1)")
                    .arg(transition.line));
                continue;
            }
            // Center takes half of the length from each clip's handle.
            const Asset asset = assets.value(a.ref);
            Time sourceStart, sourceEnd, handleA, handleB;
            // The A-side handle is what the emitted ClipInfo keeps beyond its out point.
            Time handleEnd = asset.duration, trimmedEnd;
            if (a.handleEnd.n > 0 && combine(a.handleEnd, asset.start, true, trimmedEnd)
                && lessTime(handleEnd, trimmedEnd)) handleEnd = trimmedEnd;
            if (!combine(a.start, asset.start, true, sourceStart)
                || !combine(sourceStart, a.duration, false, sourceEnd)
                || !handleEnd.n || !combine(handleEnd, sourceEnd, true, handleA)) handleA = Time{};
            if (lessTime(twice(handleA), length)) length = twice(handleA);
            if (!combine(b.start, assets.value(b.ref).start, true, handleB)) handleB = Time{};
            if (lessTime(twice(handleB), length)) length = twice(handleB);
            if (lessTime(length, transition.duration)) {
                result.warnings.append(QStringLiteral("transition %1 のハンドルが不足したため長さを %2 秒に縮めました")
                    .arg(transition.name.isEmpty() ? QStringLiteral("Cross Dissolve") : transition.name,
                         QString::number(length.seconds(), 'g', 6)));
                if (!length.n) continue;
            }
            applied.type = TransitionType::CrossDissolve;
            applied.duration = length.seconds();
            clipA->trailOut = applied;
            clipB->leadIn = applied;
        } else if (transition.next >= 0 && clipB) {
            if (lessTime(entries[transition.next].duration, length)) length = entries[transition.next].duration;
            applied.type = TransitionType::FadeIn;
            applied.duration = length.seconds();
            clipB->leadIn = applied;
        } else if (transition.prev >= 0 && clipA) {
            if (lessTime(entries[transition.prev].duration, length)) length = entries[transition.prev].duration;
            applied.type = TransitionType::FadeOut;
            applied.duration = length.seconds();
            clipA->trailOut = applied;
        } else continue;
        if (!transition.name.isEmpty() && transition.name != QLatin1String("Cross Dissolve"))
            result.warnings.append(QStringLiteral("transition の名前 %1 は未対応のため Cross Dissolve に置き換えました")
                .arg(transition.name));
    }
    nest.found += found; nest.missing += missing;
    result.resolvedFiles = nest.found.size(); result.unresolvedFiles = nest.missing.size();
    if (result.videoTracks.isEmpty() && result.audioTracks.isEmpty()) {
        if (assetClipCount == 0) result.warnings.append(QStringLiteral("読み込めるクリップがありません。"));
        else result.error = QStringLiteral("読み込めるクリップがありません。");
        return result;
    }
    if (result.videoTracks.isEmpty()) result.videoTracks.append(QVector<ClipInfo>{});
    if (result.audioTracks.isEmpty()) result.audioTracks.append(QVector<ClipInfo>{});
    return result;
}
ImportResult parseXml(const QString &xml, const QString &directory)
{
    // Each pass reads every media at the deepest level the passes so far kept
    // a reference to it at, so its sequence does not depend on document order
    // and, once a pass records no deeper level, every reference path stays
    // within kMaxCompoundDepth. The passes end: a recorded level only grows
    // (seeded from the previous pass, raised by qMax), it stays below
    // kMaxCompoundDepth (a reference at that level is skipped, not recorded),
    // and the media ids are finite.
    QMap<QString, int> depths;
    for (;;) {
        Nesting nest;
        nest.depths = depths;
        ImportResult result = parseDocument(xml, directory, nest, nullptr, nullptr);
        if (nest.depths == depths) {
            result.sequences = nest.sequences;
            return result;
        }
        depths = nest.depths;
    }
}
ImportResult readFcpxml(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) { ImportResult r; r.error = QStringLiteral("FCPXML を開けません: %1").arg(filePath); return r; }
    auto result = parseXml(QString::fromUtf8(file.readAll()), QFileInfo(filePath).absolutePath());
    if (result.title.trimmed().isEmpty()) result.title = QFileInfo(filePath).completeBaseName();
    return result;
}
bool applyImport(Timeline &timeline, ImportResult &result, QStringList *warnings)
{
    if (warnings) warnings->append(result.warnings);
    if (result.title.trimmed().isEmpty()) return false;
    // Timeline.cpp's kDefaultSequenceId: the first addSequence on a timeline
    // without a sequence model also materializes it, so it is not a new id.
    const QString defaultId = QStringLiteral("main");
    const QString activeId = timeline.activeSequenceId().isEmpty() ? defaultId : timeline.activeSequenceId();
    QMap<QString, QString> renamed; // media id -> id given by addSequence
    QSet<QString> added;
    // Each clip is rewritten once, from the ids it was parsed with.
    auto rewrite = [&](QVector<QVector<ClipInfo>> &tracks) {
        for (auto &track : tracks)
            for (auto &clip : track)
                if (renamed.contains(clip.sequenceRefId)) {
                    clip.sequenceRefId = renamed.value(clip.sequenceRefId);
                    clip.filePath = timeline_nesting::sequenceClipFilePath(clip.sequenceRefId);
                }
    };
    auto rollback = [&] {
        if (added.isEmpty()) return false;
        QVector<TimelineSequence> kept;
        for (const auto &sequence : timeline.sequences())
            if (!added.contains(sequence.id)) kept.append(sequence);
        timeline.setSequences(kept, activeId);
        return false;
    };
    for (auto &sequence : result.sequences) {
        rewrite(sequence.videoTracks);
        rewrite(sequence.audioTracks);
        QSet<QString> before;
        for (const auto &existing : timeline.sequences()) before.insert(existing.id);
        timeline.addSequence(sequence);
        QStringList fresh;
        for (const auto &existing : timeline.sequences())
            if (!before.contains(existing.id) && existing.id != defaultId) fresh.append(existing.id);
        if (fresh.size() != 1) {
            if (warnings) warnings->append(QStringLiteral("入れ子シーケンスを追加できません: %1").arg(sequence.id));
            return rollback();
        }
        renamed.insert(sequence.id, fresh.first());
        added.insert(fresh.first());
        sequence.id = fresh.first();
    }
    rewrite(result.videoTracks);
    rewrite(result.audioTracks);
    if (!timeline.createSequence(result.title, result.videoTracks, result.audioTracks)) return rollback();
    return true;
}
} // namespace fcpx::xml
