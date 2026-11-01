#include "RenderInPlace.h"
#include "Timeline.h"
#include "TimelineFrameRenderer.h"
#include "AudioKeyframes.h"
#include "CaptionOverlayBuilder.h"
#include "CaptionTrack.h"
#include "AudioClipEditor.h"
#include "VideoPlayer.h"
#include "SilenceCut.h"
#include "BeatDetect.h"
#include "ClipGeometry.h"
#include "MediaPaths.h"
#include "clipanim/ClipAnim.h"
#include "ThreePointEdit.h"
#include "TrimOps.h"
#include "TrackMatteKey.h"
#include "ProxyManager.h"
#include "UndoTrace.h"
#include "UndoManager.h"
#include "AudioMixer.h"
#include "DynamicZoom.h"
#include "OverlayDialogs.h"
#include "ProjectFile.h"
#include "WaveformGenerator.h"
#include "MusicRemixDialog.h"
#include "ShapeModifierDialog.h"
#include "MusicRemix.h"
#include "DialogueLevelerDialog.h"
#include "DialogueLeveler.h"
#include "color/ClipColor.h"
#include "playback/HdrIngestProbe.h"
#include "playback/hdringest_flag.h"
#include "playback/PixFmtDepth.h"
#include "playback/SnsFit.h"
#include "util/RcPause.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <utility>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QRubberBand>
#include <QSet>
#include <QThread>
#include <QtMath>
#include <QDir>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QUuid>
#include <QMessageBox>
#include <QMutexLocker>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QStringList>

namespace veditor {
bool parentingEnabledFromEnv(); // TimelineFrameRenderer.cpp
}

// STAGE4B: PlaybackEntry::matteTypeOrdinal stores TrackMatteType as a plain int
// (PlaybackTypes.h must stay MaskSystem.h-free). This is the SSOT populate site
// and already sees TrackMatteType (via Timeline.h -> MaskSystem.h), so we pin
// the ordinal correspondence here. If TrackMatteType is ever reordered, these
// fire at compile time before any silent matte mis-application can occur.
static_assert(static_cast<int>(TrackMatteType::None) == 0,
              "PlaybackEntry::matteTypeOrdinal 0 must mean TrackMatteType::None");
static_assert(static_cast<int>(TrackMatteType::AlphaMatte) == 1,
              "PlaybackEntry::matteTypeOrdinal 1 must mean TrackMatteType::AlphaMatte");
static_assert(static_cast<int>(TrackMatteType::AlphaInvertedMatte) == 2,
              "PlaybackEntry::matteTypeOrdinal 2 must mean TrackMatteType::AlphaInvertedMatte");
static_assert(static_cast<int>(TrackMatteType::LumaMatte) == 3,
              "PlaybackEntry::matteTypeOrdinal 3 must mean TrackMatteType::LumaMatte");
static_assert(static_cast<int>(TrackMatteType::LumaInvertedMatte) == 4,
              "PlaybackEntry::matteTypeOrdinal 4 must mean TrackMatteType::LumaInvertedMatte");

double ClipInfo::sourceSecondAtLocalTime(double localSec) const
{
    const double sourceOut = (outPoint > 0.0) ? outPoint : duration;
    if (sourceOut <= inPoint)
        return inPoint;

    const double boundedLocal = qMax(0.0, localSec);
    double sourceSecond = inPoint;
    if (!timeRemapCurve.keys.isEmpty()) {
        sourceSecond += qMax(0.0, timeRemapCurve.srcTimeAt(boundedLocal));
    } else {
        const double uniformSpeed = (speed > 0.0) ? speed : 1.0;
        if (speedRamp.isIdentity()) {
            sourceSecond += boundedLocal * uniformSpeed;
        } else {
            const qint64 scaledTimelineUs = qRound64(
                boundedLocal * uniformSpeed * 1'000'000.0);
            sourceSecond += static_cast<double>(
                speedRamp.timelineToSourceUs(scaledTimelineUs)) / 1'000'000.0;
        }
    }

    sourceSecond = qBound(inPoint, sourceSecond, sourceOut);
    if (reversed)
        sourceSecond = inPoint + sourceOut - sourceSecond;
    return qBound(inPoint, sourceSecond, sourceOut);
}

double ClipInfo::localSecondAtSourceTime(double sourceSec) const
{
    const double sourceOut = (outPoint > 0.0) ? outPoint : duration;
    if (sourceOut <= inPoint)
        return 0.0;

    double foldedSource = qBound(inPoint, sourceSec, sourceOut);
    if (reversed)
        foldedSource = inPoint + sourceOut - foldedSource;
    const double targetOffset = qBound(0.0, foldedSource - inPoint,
                                       sourceOut - inPoint);

    if (!timeRemapCurve.keys.isEmpty()) {
        const auto &keys = timeRemapCurve.keys;
        if (keys.size() == 1)
            return qMax(0.0, keys.first().outTime);
        for (int i = 1; i < keys.size(); ++i) {
            const auto &left = keys[i - 1];
            const auto &right = keys[i];
            const double lo = qMin(left.srcTime, right.srcTime);
            const double hi = qMax(left.srcTime, right.srcTime);
            if (targetOffset < lo || targetOffset > hi)
                continue;
            const double span = right.srcTime - left.srcTime;
            if (qFuzzyIsNull(span))
                return qMax(0.0, left.outTime);
            const double fraction = (targetOffset - left.srcTime) / span;
            return qMax(0.0, left.outTime
                                 + fraction * (right.outTime - left.outTime));
        }
        const auto nearest = std::min_element(
            keys.cbegin(), keys.cend(), [targetOffset](const auto &a, const auto &b) {
                return qAbs(a.srcTime - targetOffset) < qAbs(b.srcTime - targetOffset);
            });
        return nearest == keys.cend() ? 0.0 : qMax(0.0, nearest->outTime);
    }

    const double uniformSpeed = (speed > 0.0) ? speed : 1.0;
    if (speedRamp.isIdentity())
        return targetOffset / uniformSpeed;
    const qint64 sourceOffsetUs = qRound64(targetOffset * 1'000'000.0);
    return qMax(0.0, static_cast<double>(
        speedRamp.sourceToTimelineUs(sourceOffsetUs))
        / uniformSpeed / 1'000'000.0);
}

bool ClipInfo::sourceTimeRunsBackwardAtLocalTime(double localSec) const
{
    const double localDuration = qMax(0.0, effectiveDuration());
    const double boundedLocal = qBound(0.0, localSec, localDuration);
    const double currentSource = sourceSecondAtLocalTime(boundedLocal);
    constexpr double probeSec = 0.000001;
    constexpr double sourceEpsilon = 1e-12;

    if (boundedLocal < localDuration) {
        const double nextLocal = qMin(localDuration, boundedLocal + probeSec);
        const double delta = sourceSecondAtLocalTime(nextLocal) - currentSource;
        if (qAbs(delta) > sourceEpsilon)
            return delta < 0.0;
    }
    if (boundedLocal > 0.0) {
        const double previousLocal = qMax(0.0, boundedLocal - probeSec);
        const double delta = currentSource - sourceSecondAtLocalTime(previousLocal);
        if (qAbs(delta) > sourceEpsilon)
            return delta < 0.0;
    }
    return false;
}

namespace audioxfade {

namespace {

bool validAudioFadeDuration(double durationSec, QString *errorOut)
{
    if (!std::isfinite(durationSec) || durationSec <= 0.0) {
        if (errorOut)
            *errorOut = QStringLiteral("フェード時間は 0 より大きい有限値で指定してください");
        return false;
    }
    return true;
}

} // namespace

bool applyCrossfade(QVector<ClipInfo> &clips, int clipIndexA,
                    double durationSec, QString *errorOut)
{
    if (!validAudioFadeDuration(durationSec, errorOut))
        return false;
    if (clipIndexA < 0 || clipIndexA + 1 >= clips.size()) {
        if (errorOut)
            *errorOut = QStringLiteral("クロスフェードには隣接する A/B クリップが必要です");
        return false;
    }

    Transition transition;
    transition.type = TransitionType::CrossDissolve;
    transition.duration = durationSec;
    clips[clipIndexA].trailOut = transition;
    clips[clipIndexA + 1].leadIn = transition;
    return true;
}

bool applyFade(QVector<ClipInfo> &clips, int clipIndex,
               AudioFadeEdge edge, double durationSec, QString *errorOut)
{
    if (!validAudioFadeDuration(durationSec, errorOut))
        return false;
    if (clipIndex < 0 || clipIndex >= clips.size()) {
        if (errorOut)
            *errorOut = QStringLiteral("音声クリップの index が範囲外です");
        return false;
    }

    Transition transition;
    transition.type = edge == AudioFadeEdge::In
        ? TransitionType::FadeIn : TransitionType::FadeOut;
    transition.duration = durationSec;
    if (edge == AudioFadeEdge::In)
        clips[clipIndex].leadIn = transition;
    else
        clips[clipIndex].trailOut = transition;
    return true;
}

} // namespace audioxfade

namespace {
// Transition badge geometry. The badge width grows with duration so the
// user gets live visual feedback while dragging the resize handle (a 6 px
// band on the inward edge). The 18 px Y-band defines the hit-test area
// for both the press handler and the hover-cursor logic.
constexpr int kTransBadgeMinW    = 40;
constexpr int kTransBadgeMaxW    = 160;
constexpr int kTransBadgeHandleW = 6;
constexpr int kTransBadgeYTop    = 3;
constexpr int kTransBadgeH       = 18;
constexpr int kTransBadgeYBot    = kTransBadgeYTop + kTransBadgeH;

inline int transBadgeWidth(double durSec, double pps, int clipWidth) {
    const int desired = static_cast<int>(durSec * pps);
    return qBound(kTransBadgeMinW, desired,
                  qMin(kTransBadgeMaxW, clipWidth - 4));
}

// Volume-envelope (rubber-band) overlay state. Process-wide because the
// MainWindow menu toggle flips every audio row at once. Default OFF — the
// envelope is only drawn / hit-tested when the user opts in, so existing
// audio clip interaction (drag, trim, transition badge) is unchanged.
bool g_envelopeEditMode = false;
constexpr int kEnvelopePointRadiusPx = 5;
constexpr double kEnvelopeMaxGain = 2.0;     // matches ClipInfo::volume cap
constexpr double kEnvelopeHitRadiusPx = 5.0; // mouse hit radius for points
constexpr qint64 kDefaultAdjustmentLayerDurationUs = 5LL * 1000000LL;
constexpr int kAdjustmentLayerMinPaintWidthPx = 42;

// Map a clip-local time + gain to widget pixel coords. The clipRect's top
// row is gain=2.0 (max), the bottom row is gain=0.0; gain=1.0 sits at the
// vertical centre. Time is laid out at the track pps.
inline double envelopeGainToY(double gain, int rowHeight) {
    const double clamped = qBound(0.0, gain, kEnvelopeMaxGain);
    return rowHeight * (1.0 - clamped / kEnvelopeMaxGain);
}
inline double envelopeYToGain(double y, int rowHeight) {
    if (rowHeight <= 0) return 1.0;
    const double t = qBound(0.0, 1.0 - y / static_cast<double>(rowHeight), 1.0);
    return t * kEnvelopeMaxGain;
}

int adjustmentLayerTrackIndex(const Timeline *timeline, const TimelineTrack *track)
{
    if (!timeline || !track || track->isAudioTrack())
        return -1;
    return timeline->videoTracks().indexOf(const_cast<TimelineTrack *>(track));
}

void paintAdjustmentLayersForTrack(QPainter &painter,
                                   const Timeline *timeline,
                                   const TimelineTrack *track,
                                   const QRect &visibleRect,
                                   double pixelsPerSecond,
                                   int rowHeight)
{
    if (!timeline || !track || pixelsPerSecond <= 0.0 || rowHeight <= 0)
        return;
    const int trackIndex = adjustmentLayerTrackIndex(timeline, track);
    if (trackIndex < 0)
        return;

    const QVector<AdjustmentLayer> &layers = timeline->adjustmentLayers();
    if (layers.isEmpty())
        return;

    const int bandTop = qBound(3, rowHeight / 5, 10);
    const int bandHeight = qMax(18, qMin(32, rowHeight - bandTop - 6));
    const QColor fill(174, 126, 255, 190);
    const QColor stroke(232, 215, 255, 230);
    const QColor accent(255, 219, 95, 235);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (const AdjustmentLayer &layer : layers) {
        if (layer.trackIndex != trackIndex || layer.timelineEndUs <= layer.timelineStartUs)
            continue;

        const double startSec = static_cast<double>(layer.timelineStartUs) / 1.0e6;
        const double endSec = static_cast<double>(layer.timelineEndUs) / 1.0e6;
        const int startX = qMax(0, static_cast<int>(startSec * pixelsPerSecond));
        const int endX = qMax(startX + 1, static_cast<int>(endSec * pixelsPerSecond));
        if (endX < visibleRect.left() || startX > visibleRect.right())
            continue;

        QRect rect(startX, bandTop,
                   qMax(kAdjustmentLayerMinPaintWidthPx, endX - startX),
                   bandHeight);
        painter.setPen(QPen(stroke, 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(rect.adjusted(0, 0, -1, -1), 4, 4);

        const int iconLeft = rect.left() + 7;
        const int iconCenterY = rect.center().y();
        painter.setPen(QPen(accent, 2));
        painter.drawLine(iconLeft, iconCenterY, iconLeft + 12, iconCenterY);
        painter.drawLine(iconLeft + 6, iconCenterY - 6, iconLeft + 6, iconCenterY + 6);
        painter.setBrush(accent);
        painter.drawEllipse(QPointF(iconLeft + 6, iconCenterY), 3.0, 3.0);

        QRect textRect = rect.adjusted(25, 0, -6, 0);
        if (textRect.width() > 8) {
            painter.setPen(Qt::white);
            painter.setFont(QFont(QStringLiteral("Arial"), 8, QFont::Bold));
            const QString baseName = layer.name.isEmpty()
                ? QStringLiteral("Adjustment Layer")
                : layer.name;
            const QString label = QStringLiteral("ADJ  %1").arg(baseName);
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                             painter.fontMetrics().elidedText(label, Qt::ElideRight,
                                                              textRect.width()));
        }
    }

    painter.restore();
}

AdjustmentLayer makeAdjustmentLayer(double startSec, double endSec, int trackIndex)
{
    AdjustmentLayer layer;
    const double safeStartSec = qMax(0.0, startSec);
    const double safeEndSec = qMax(safeStartSec, endSec);
    layer.timelineStartUs = static_cast<qint64>(safeStartSec * 1.0e6);
    layer.timelineEndUs = static_cast<qint64>(safeEndSec * 1.0e6);
    if (layer.timelineEndUs <= layer.timelineStartUs)
        layer.timelineEndUs = layer.timelineStartUs + kDefaultAdjustmentLayerDurationUs;
    layer.trackIndex = qMax(0, trackIndex);
    layer.name = QStringLiteral("Adjustment Layer");
    return layer;
}

QString audioChannelModeMenuLabel(AudioChannelMode mode)
{
    switch (mode) {
    case AudioChannelMode::Stereo:
        return QStringLiteral("Stereo");
    case AudioChannelMode::FillLeft:
        return QStringLiteral("Fill Left (L→LR)");
    case AudioChannelMode::FillRight:
        return QStringLiteral("Fill Right (R→LR)");
    case AudioChannelMode::Swap:
        return QStringLiteral("LRスワップ");
    case AudioChannelMode::Mono:
        return QStringLiteral("モノ化");
    }
    return QStringLiteral("Stereo");
}

QAction *addAudioChannelModeAction(QMenu *menu,
                                   AudioChannelMode current,
                                   AudioChannelMode mode)
{
    QAction *act = menu->addAction(audioChannelModeMenuLabel(mode));
    act->setCheckable(true);
    act->setChecked(current == mode);
    return act;
}

QAction *addClipLabelAction(QMenu *menu, ClipLabel current, ClipLabel label)
{
    QAction *act = menu->addAction(clipLabelName(label));
    act->setCheckable(true);
    act->setChecked(current == label);
    return act;
}
} // namespace

QColor clipLabelColor(ClipLabel label)
{
    switch (label) {
    case ClipLabel::Red:    return QColor(0xD9, 0x4B, 0x4B);
    case ClipLabel::Orange: return QColor(0xE5, 0x8A, 0x3A);
    case ClipLabel::Yellow: return QColor(0xD7, 0xB8, 0x4A);
    case ClipLabel::Green:  return QColor(0x53, 0xA8, 0x5B);
    case ClipLabel::Cyan:   return QColor(0x36, 0xA9, 0xB7);
    case ClipLabel::Blue:   return QColor(0x4B, 0x78, 0xD1);
    case ClipLabel::Purple: return QColor(0x8A, 0x5A, 0xBD);
    case ClipLabel::Pink:   return QColor(0xD6, 0x5B, 0x9E);
    case ClipLabel::None:   return QColor();
    }
    return QColor();
}

QString clipLabelName(ClipLabel label)
{
    switch (label) {
    case ClipLabel::None:   return QStringLiteral("なし");
    case ClipLabel::Red:    return QStringLiteral("赤");
    case ClipLabel::Orange: return QStringLiteral("オレンジ");
    case ClipLabel::Yellow: return QStringLiteral("黄");
    case ClipLabel::Green:  return QStringLiteral("緑");
    case ClipLabel::Cyan:   return QStringLiteral("シアン");
    case ClipLabel::Blue:   return QStringLiteral("青");
    case ClipLabel::Purple: return QStringLiteral("紫");
    case ClipLabel::Pink:   return QStringLiteral("ピンク");
    }
    return QStringLiteral("なし");
}

ClipLabel clipLabelFromString(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("red"))
        return ClipLabel::Red;
    if (normalized == QStringLiteral("orange"))
        return ClipLabel::Orange;
    if (normalized == QStringLiteral("yellow"))
        return ClipLabel::Yellow;
    if (normalized == QStringLiteral("green"))
        return ClipLabel::Green;
    if (normalized == QStringLiteral("cyan"))
        return ClipLabel::Cyan;
    if (normalized == QStringLiteral("blue"))
        return ClipLabel::Blue;
    if (normalized == QStringLiteral("purple"))
        return ClipLabel::Purple;
    if (normalized == QStringLiteral("pink"))
        return ClipLabel::Pink;
    return ClipLabel::None;
}

QString clipLabelToString(ClipLabel label)
{
    switch (label) {
    case ClipLabel::None:   return QStringLiteral("none");
    case ClipLabel::Red:    return QStringLiteral("red");
    case ClipLabel::Orange: return QStringLiteral("orange");
    case ClipLabel::Yellow: return QStringLiteral("yellow");
    case ClipLabel::Green:  return QStringLiteral("green");
    case ClipLabel::Cyan:   return QStringLiteral("cyan");
    case ClipLabel::Blue:   return QStringLiteral("blue");
    case ClipLabel::Purple: return QStringLiteral("purple");
    case ClipLabel::Pink:   return QStringLiteral("pink");
    }
    return QStringLiteral("none");
}

namespace timeline_nesting {
namespace {
constexpr char kSequenceClipPrefix[] = "veditor://sequence/";
constexpr char kSequenceStoreParentKey[] = "__veditor_sequence_store__";
}

QString sequenceClipFilePath(const QString &sequenceId)
{
    return QString::fromLatin1(kSequenceClipPrefix) + sequenceId;
}

bool isSequenceClipFilePath(const QString &filePath)
{
    return filePath.startsWith(QLatin1String(kSequenceClipPrefix));
}

QString sequenceIdFromClipFilePath(const QString &filePath)
{
    return isSequenceClipFilePath(filePath)
        ? filePath.mid(int(sizeof(kSequenceClipPrefix) - 1))
        : QString();
}

QString sequenceStoreParentKey()
{
    return QString::fromLatin1(kSequenceStoreParentKey);
}

QString encodeSequenceStoreObject(const QJsonObject &store)
{
    if (store.isEmpty())
        return {};
    const QByteArray json = QJsonDocument(store).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(json.toBase64());
}

QJsonObject decodeSequenceStoreObject(const QString &encoded)
{
    if (encoded.isEmpty())
        return {};
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromBase64(encoded.toLatin1()));
    if (doc.isNull())
        doc = QJsonDocument::fromJson(encoded.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject{};
}
} // namespace timeline_nesting

namespace {
constexpr int kSequenceStoreVersion = 1;
constexpr char kDefaultSequenceId[] = "main";

QString fallbackSequenceId(int index)
{
    return index == 0
        ? QString::fromLatin1(kDefaultSequenceId)
        : QStringLiteral("sequence-%1").arg(index + 1);
}

bool hasSequenceId(const QVector<TimelineSequence> &sequences, const QString &id)
{
    return std::any_of(sequences.cbegin(), sequences.cend(),
                       [&id](const TimelineSequence &s) { return s.id == id; });
}

QString uniqueSequenceId(const QVector<TimelineSequence> &sequences,
                         const QString &preferred,
                         int index)
{
    QString base = preferred.trimmed();
    if (base.isEmpty())
        base = fallbackSequenceId(index);
    QString candidate = base;
    int suffix = 2;
    while (hasSequenceId(sequences, candidate))
        candidate = QStringLiteral("%1-%2").arg(base).arg(suffix++);
    return candidate;
}

QString resolveSequenceRefId(const ClipInfo &clip)
{
    if (!clip.sequenceRefId.isEmpty())
        return clip.sequenceRefId;
    return timeline_nesting::sequenceIdFromClipFilePath(clip.filePath);
}

struct NestedSequenceIntervalMapping {
    double timelineStart = 0.0;
    double timelineEnd = 0.0;
    bool runsBackward = false;
};

NestedSequenceIntervalMapping mapNestedSequenceInterval(
    const ClipInfo &parentClip,
    double parentTimelineStart,
    double sourceIn,
    double overlapStart,
    double overlapEnd,
    double parentSpeed,
    bool reverseCompositionActive)
{
    // Preserve the legacy flattening arithmetic exactly while reverse is OFF.
    // Once either side participates in reverse, resolve the parent's authored
    // time-remap as well: a descending non-reversed parent composed with a
    // reversed child runs forward and must keep video/audio on the same nested
    // interval. The renderer enters the same shared mapper only when this
    // reverse-composition flag is active, keeping reverse-OFF projects on the
    // exact legacy path.
    if (!reverseCompositionActive) {
        return {
            parentTimelineStart + (overlapStart - sourceIn) / parentSpeed,
            parentTimelineStart + (overlapEnd - sourceIn) / parentSpeed,
            false
        };
    }

    // A one-key time-remap is a hold. Its inverse is not a point-to-point
    // function: every output time maps to the same source time. Inverting both
    // source boundaries therefore returns the same key time and collapses the
    // flattened preview entry to zero length, while the recursive renderer
    // correctly keeps drawing the hold. Preserve the authored half-open
    // timeline interval for this degenerate inverse so preview and export both
    // retain a live [start, end) entry.
    if (parentClip.timeRemapCurve.keys.size() == 1) {
        return {
            parentTimelineStart + (overlapStart - sourceIn) / parentSpeed,
            parentTimelineStart + (overlapEnd - sourceIn) / parentSpeed,
            false
        };
    }

    const double localAtSourceStart =
        parentClip.localSecondAtSourceTime(overlapStart);
    const double localAtSourceEnd =
        parentClip.localSecondAtSourceTime(overlapEnd);
    return {
        parentTimelineStart + qMin(localAtSourceStart, localAtSourceEnd),
        parentTimelineStart + qMax(localAtSourceStart, localAtSourceEnd),
        localAtSourceEnd + 1e-9 < localAtSourceStart
    };
}

bool sequenceContainsReversedClip(
    const QVector<TimelineSequence> &sequences,
    const QString &sequenceId,
    bool audio,
    QVector<QString> &visited,
    int depth = 0)
{
    if (sequenceId.isEmpty() || depth > 8
        || visited.contains(sequenceId)) {
        return false;
    }
    const TimelineSequence *sequence = nullptr;
    for (const TimelineSequence &candidate : sequences) {
        if (candidate.id == sequenceId) {
            sequence = &candidate;
            break;
        }
    }
    if (!sequence)
        return false;

    visited.append(sequenceId);
    const QVector<QVector<ClipInfo>> &tracks = audio
        ? sequence->audioTracks : sequence->videoTracks;
    for (const QVector<ClipInfo> &track : tracks) {
        for (const ClipInfo &child : track) {
            if (child.reversed) {
                visited.removeLast();
                return true;
            }
            if (!child.isSequenceReference())
                continue;
            if (sequenceContainsReversedClip(
                    sequences, resolveSequenceRefId(child), audio,
                    visited, depth + 1)) {
                visited.removeLast();
                return true;
            }
        }
    }
    visited.removeLast();
    return false;
}

void upsertSequence(QVector<TimelineSequence> &sequences,
                    const TimelineSequence &sequence)
{
    if (sequence.id.isEmpty())
        return;
    for (TimelineSequence &existing : sequences) {
        if (existing.id == sequence.id) {
            existing = sequence;
            return;
        }
    }
    sequences.append(sequence);
}

QVector<TimelineSequence> normalizeSequences(const QVector<TimelineSequence> &input,
                                             QString *activeSequenceId)
{
    QVector<TimelineSequence> normalized;
    normalized.reserve(input.size());
    for (int i = 0; i < input.size(); ++i) {
        TimelineSequence seq = input[i];
        seq.id = uniqueSequenceId(normalized, seq.id, i);
        if (seq.name.trimmed().isEmpty())
            seq.name = QStringLiteral("Sequence %1").arg(i + 1);
        normalized.append(seq);
    }

    if (normalized.isEmpty()) {
        if (activeSequenceId)
            activeSequenceId->clear();
        return normalized;
    }

    if (activeSequenceId) {
        if (!hasSequenceId(normalized, *activeSequenceId))
            *activeSequenceId = normalized.first().id;
    }
    return normalized;
}

QJsonObject sequenceToStoreJson(const TimelineSequence &sequence)
{
    ProjectData data;
    data.videoTracks = sequence.videoTracks;
    data.audioTracks = sequence.audioTracks;
    data.generatedCaptionOverlays = sequence.generatedCaptionOverlays;

    QJsonObject project =
        QJsonDocument::fromJson(ProjectFile::toJsonString(data).toUtf8()).object();

    QJsonObject obj;
    obj[QStringLiteral("id")] = sequence.id;
    obj[QStringLiteral("name")] = sequence.name;
    obj[QStringLiteral("videoTracks")] = project.value(QStringLiteral("videoTracks")).toArray();
    obj[QStringLiteral("audioTracks")] = project.value(QStringLiteral("audioTracks")).toArray();
    obj[QStringLiteral("generatedCaptionOverlays")] =
        project.value(QStringLiteral("generatedCaptionOverlays")).toArray();
    return obj;
}

TimelineSequence sequenceFromStoreJson(const QJsonObject &obj)
{
    TimelineSequence sequence;
    sequence.id = obj.value(QStringLiteral("id")).toString();
    sequence.name = obj.value(QStringLiteral("name")).toString();

    QJsonObject project;
    project[QStringLiteral("version")] = 2;
    project[QStringLiteral("config")] = QJsonObject{};
    project[QStringLiteral("videoTracks")] = obj.value(QStringLiteral("videoTracks")).toArray();
    project[QStringLiteral("audioTracks")] = obj.value(QStringLiteral("audioTracks")).toArray();
    project[QStringLiteral("generatedCaptionOverlays")] =
        obj.value(QStringLiteral("generatedCaptionOverlays")).toArray();
    project[QStringLiteral("playheadPos")] = 0.0;
    project[QStringLiteral("markIn")] = -1.0;
    project[QStringLiteral("markOut")] = -1.0;
    project[QStringLiteral("zoomLevel")] = 10;

    ProjectData data;
    if (ProjectFile::fromJsonString(QString::fromUtf8(
            QJsonDocument(project).toJson(QJsonDocument::Compact)), data)) {
        sequence.videoTracks = data.videoTracks;
        sequence.audioTracks = data.audioTracks;
        sequence.generatedCaptionOverlays = data.generatedCaptionOverlays;
    }
    return sequence;
}

QJsonObject sequenceStoreToJson(const QVector<TimelineSequence> &sequences,
                                const QString &activeSequenceId)
{
    if (sequences.isEmpty())
        return {};

    QJsonArray arr;
    for (const TimelineSequence &sequence : sequences)
        arr.append(sequenceToStoreJson(sequence));

    QJsonObject store;
    store[QStringLiteral("version")] = kSequenceStoreVersion;
    store[QStringLiteral("activeSequenceId")] = activeSequenceId;
    store[QStringLiteral("sequences")] = arr;
    return store;
}

bool sequencesFromStoreJson(const QJsonObject &store,
                            QVector<TimelineSequence> *sequencesOut,
                            QString *activeSequenceIdOut)
{
    const QJsonArray arr = store.value(QStringLiteral("sequences")).toArray();
    if (arr.isEmpty())
        return false;

    QVector<TimelineSequence> sequences;
    sequences.reserve(arr.size());
    for (const QJsonValue &value : arr) {
        TimelineSequence sequence = sequenceFromStoreJson(value.toObject());
        if (!sequence.id.isEmpty())
            sequences.append(sequence);
    }
    QString active = store.value(QStringLiteral("activeSequenceId")).toString();
    sequences = normalizeSequences(sequences, &active);
    if (sequences.isEmpty())
        return false;

    if (sequencesOut)
        *sequencesOut = sequences;
    if (activeSequenceIdOut)
        *activeSequenceIdOut = active;
    return true;
}

constexpr char kBreadcrumbBarObjectName[] = "TimelineBreadcrumbBar";
constexpr char kBreadcrumbPathProperty[] = "_veditor_breadcrumb_path";

QString sequenceNameForBreadcrumb(const Timeline *timeline, const QString &sequenceId)
{
    if (!timeline)
        return sequenceId;
    for (const TimelineSequence &sequence : timeline->sequences()) {
        if (sequence.id == sequenceId) {
            if (!sequence.name.trimmed().isEmpty())
                return sequence.name;
            break;
        }
    }
    if (sequenceId == QLatin1String(kDefaultSequenceId))
        return QStringLiteral("Main");
    return sequenceId;
}

QStringList breadcrumbPathForTimeline(const Timeline *timeline)
{
    return timeline
        ? timeline->property(kBreadcrumbPathProperty).toStringList()
        : QStringList{};
}

void setBreadcrumbPathForTimeline(Timeline *timeline, const QStringList &path)
{
    if (timeline)
        timeline->setProperty(kBreadcrumbPathProperty, path);
}

void rebuildTimelineBreadcrumbBar(Timeline *timeline)
{
    if (!timeline)
        return;
    QWidget *bar = timeline->findChild<QWidget *>(
        QString::fromLatin1(kBreadcrumbBarObjectName));
    if (!bar || !bar->layout())
        return;

    QString active = timeline->activeSequenceId();
    QStringList path = breadcrumbPathForTimeline(timeline);
    if (active.isEmpty()) {
        path.clear();
    } else if (path.isEmpty()) {
        path.append(active);
    } else if (path.last() != active) {
        const int existing = path.indexOf(active);
        if (existing >= 0)
            path = path.mid(0, existing + 1);
        else
            path.append(active);
    }
    setBreadcrumbPathForTimeline(timeline, path);

    QLayout *layout = bar->layout();
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            delete widget;
        delete item;
    }

    bar->setVisible(path.size() > 1);
    if (path.size() <= 1)
        return;

    for (int i = 0; i < path.size(); ++i) {
        if (i > 0) {
            auto *sep = new QLabel(QStringLiteral(">"), bar);
            sep->setStyleSheet(QStringLiteral("color: #888; padding: 0 2px;"));
            layout->addWidget(sep);
        }

        const QString sequenceId = path[i];
        auto *button = new QPushButton(sequenceNameForBreadcrumb(timeline, sequenceId), bar);
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(
            QStringLiteral("QPushButton { background: transparent; color: #d8e6ff; border: none; padding: 1px 4px; }"
                           "QPushButton:hover { color: white; text-decoration: underline; }"));
        button->setEnabled(i + 1 < path.size());
        QObject::connect(button, &QPushButton::clicked, timeline,
                         [timeline, sequenceId, i]() {
            if (!timeline || !timeline->setActiveSequence(sequenceId))
                return;
            QStringList newPath = breadcrumbPathForTimeline(timeline);
            if (newPath.size() > i + 1)
                newPath = newPath.mid(0, i + 1);
            setBreadcrumbPathForTimeline(timeline, newPath);
            rebuildTimelineBreadcrumbBar(timeline);
        });
        layout->addWidget(button);
    }
    layout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
}
} // namespace

bool applyColorMatchLutToSelectedTimelineClip(Timeline *timeline,
                                              const QString &lutPath,
                                              double lutIntensity,
                                              QString *errorMessage)
{
    auto fail = [&](const QString &message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };

    if (!timeline)
        return fail(QStringLiteral("Timeline is not available."));
    if (lutPath.isEmpty() || !QFileInfo::exists(lutPath))
        return fail(QStringLiteral("LUT file does not exist."));

    const QVector<TimelineTrack *> &tracks = timeline->videoTracks();
    if (tracks.isEmpty())
        return fail(QStringLiteral("No video track is available."));

    const TimelineState state = timeline->currentState();
    int trackIdx = state.selectedVideoTrackIndex;
    int clipIdx = state.selectedVideoClipIndex;

    if (trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx]
        || clipIdx < 0 || clipIdx >= tracks[trackIdx]->clipCount()) {
        trackIdx = -1;
        clipIdx = -1;
        for (int i = 0; i < tracks.size(); ++i) {
            TimelineTrack *track = tracks[i];
            if (!track)
                continue;
            const int selected = track->selectedClip();
            if (selected >= 0 && selected < track->clipCount()) {
                trackIdx = i;
                clipIdx = selected;
                break;
            }
        }
    }

    if (trackIdx < 0 || clipIdx < 0)
        return fail(QStringLiteral("No video clip is selected."));

    TimelineTrack *track = tracks[trackIdx];
    QVector<ClipInfo> clips = track->clips();
    if (clipIdx >= clips.size())
        return fail(QStringLiteral("Selected clip is no longer available."));

    const double intensity = qBound(0.0, lutIntensity, 1.0);
    ClipInfo &clip = clips[clipIdx];
    if (clip.lutFilePath == lutPath && qFuzzyCompare(clip.lutIntensity + 1.0, intensity + 1.0))
        return true;

    clip.lutFilePath = lutPath;
    clip.lutIntensity = intensity;
    track->setClips(clips);

    if (timeline->undoManager())
        timeline->saveUndoState(QStringLiteral("Apply color match LUT"));
    timeline->refreshPlaybackSequence();
    return true;
}

QString exportAudioChannelPanFilterForMode(AudioChannelMode mode)
{
    switch (mode) {
    case AudioChannelMode::Stereo:
        return {};
    case AudioChannelMode::FillLeft:
        return QStringLiteral("pan=stereo|c0=c0|c1=c0");
    case AudioChannelMode::FillRight:
        return QStringLiteral("pan=stereo|c0=c1|c1=c1");
    case AudioChannelMode::Swap:
        return QStringLiteral("pan=stereo|c0=c1|c1=c0");
    case AudioChannelMode::Mono:
        return QStringLiteral("pan=stereo|c0=0.5*c0+0.5*c1|c1=0.5*c0+0.5*c1");
    }
    return {};
}

QString buildExportAudioMixEntryPreFxFilterChain(int inputIndex,
    const QString &clipIn, const QString &clipOut, AudioChannelMode mode,
    bool reversed, double speed)
{
    QStringList filters;
    filters << QStringLiteral("atrim=start=%1:end=%2")
                   .arg(clipIn, clipOut);
    if (reversed) {
        filters << QStringLiteral("areverse");
        // Reversed preview consumes its buffered PCM at the clip's uniform
        // speed. Mirror that order for export: reverse first, then change
        // duration. Split factors to remain compatible with FFmpeg versions
        // whose atempo range is 0.5..2.0.
        double remainingSpeed = std::isfinite(speed) && speed > 0.0
            ? speed : 1.0;
        while (remainingSpeed < 0.5) {
            filters << QStringLiteral("atempo=0.5");
            remainingSpeed /= 0.5;
        }
        while (remainingSpeed > 2.0) {
            filters << QStringLiteral("atempo=2");
            remainingSpeed /= 2.0;
        }
        if (remainingSpeed != 1.0) {
            filters << QStringLiteral("atempo=%1").arg(
                QString::number(remainingSpeed, 'g', 15));
        }
    }
    filters << QStringLiteral("asetpts=PTS-STARTPTS")
            << QStringLiteral("aresample=48000")
            << QStringLiteral("aformat=sample_fmts=fltp:channel_layouts=stereo");

    const QString panFilter = exportAudioChannelPanFilterForMode(mode);
    if (!panFilter.isEmpty())
        filters << panFilter;

    return QStringLiteral("[%1:a]%2[prefx%1]").arg(inputIndex)
        .arg(filters.join(QLatin1Char(',')));
}

QString buildExportAudioMixEntryPostFxFilterChain(int inputIndex,
    int delayMs, const QString &volumeExpression, double clipDuration,
    TransitionType leadInType, double leadInDuration,
    TransitionType trailOutType, double trailOutDuration)
{
    QStringList filters;
    filters << QStringLiteral("volume='%1':eval=frame").arg(volumeExpression);

    // Audio-only fades use the same equal-power characteristic as
    // AudioMixer's sqrt gain. FFmpeg's qsin curve is the matching export
    // implementation. Keep these filters after areverse/atempo so their
    // durations are expressed in timeline-time. Defaults add nothing, which
    // preserves the legacy chain byte-for-byte for clips without fades.
    const bool leadFade = (leadInType == TransitionType::FadeIn
                           || leadInType == TransitionType::CrossDissolve)
        && std::isfinite(leadInDuration) && leadInDuration > 0.0;
    const bool trailFade = (trailOutType == TransitionType::FadeOut
                            || trailOutType == TransitionType::CrossDissolve)
        && std::isfinite(trailOutDuration) && trailOutDuration > 0.0;
    if (leadFade) {
        filters << QStringLiteral("afade=t=in:curve=qsin:d=%1")
                       .arg(QString::number(leadInDuration, 'g', 15));
    }
    if (trailFade) {
        const double start = qMax(0.0, clipDuration - trailOutDuration);
        filters << QStringLiteral("afade=t=out:curve=qsin:st=%1:d=%2")
                       .arg(QString::number(start, 'g', 15),
                            QString::number(trailOutDuration, 'g', 15));
    }

    if (delayMs > 0)
        filters << QStringLiteral("adelay=%1:all=1").arg(delayMs);

    return QStringLiteral("[%1:a]%2[a%1]").arg(inputIndex)
        .arg(filters.join(QLatin1Char(',')));
}

QString buildExportAudioMixEntryFilterChain(int inputIndex,
                                            const QString &clipIn,
                                            const QString &clipOut,
                                            int delayMs,
                                            const QString &volumeExpression,
                                            AudioChannelMode mode,
                                            bool reversed,
                                            double speed,
                                            TransitionType leadInType,
                                            double leadInDuration,
                                            TransitionType trailOutType,
                                            double trailOutDuration)
{
    QString pre = buildExportAudioMixEntryPreFxFilterChain(
        inputIndex, clipIn, clipOut, mode, reversed, speed);
    QString post = buildExportAudioMixEntryPostFxFilterChain(inputIndex,
        delayMs, volumeExpression,
        std::isfinite(speed) && speed > 0.0
            ? qMax(0.0, (clipOut.toDouble() - clipIn.toDouble()) / speed)
            : qMax(0.0, clipOut.toDouble() - clipIn.toDouble()),
        leadInType, leadInDuration, trailOutType, trailOutDuration);
    // Join at the DSP boundary without changing the legacy graph text.
    pre.chop(QStringLiteral("[prefx%1]").arg(inputIndex).size());
    post.remove(0, QStringLiteral("[%1:a]").arg(inputIndex).size());
    return pre + QLatin1Char(',') + post;
}

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

// RAII helper for one-shot FFmpeg audio decoding used by per-clip PCM peak
// analysis (normalizeAudioClipPeak). Mirrors AudioMixer::openEntry's pattern
// so we don't introduce a separate FFmpeg open path.
struct NormalizeDecoderCtx {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;

    bool open(const QString &path) {
        if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0)
            return false;
        const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (idx < 0) return false;
        AVCodecParameters *par = fmt->streams[idx]->codecpar;
        const AVCodec *c = avcodec_find_decoder(par->codec_id);
        if (!c) return false;
        codec = avcodec_alloc_context3(c);
        if (!codec) return false;
        if (avcodec_parameters_to_context(codec, par) < 0) return false;
        if (avcodec_open2(codec, c, nullptr) < 0) return false;
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        if (!pkt || !frame) return false;
        m_streamIdx = idx;
        return true;
    }

    ~NormalizeDecoderCtx() {
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
    }

    int streamIdx() const { return m_streamIdx; }
private:
    int m_streamIdx = -1;
};

// Definitions for the static envelope-mode getters declared in Timeline.h.
// Implemented out-of-line so the anonymous-namespace flag stays a single
// translation-unit-local symbol.
void TimelineTrack::setEnvelopeEditMode(bool on) { g_envelopeEditMode = on; }
bool TimelineTrack::envelopeEditMode() { return g_envelopeEditMode; }

// Small strip drawn above the V/A tracks that visualizes each text
// overlay's time range as a rounded rectangle with the (truncated) overlay
// text. The strip is rebuilt from Timeline's clip[0].textManager every
// time refreshTextStrip() runs. Each bar's left/right edge is draggable
// to adjust the corresponding overlay's startTime / endTime — the Timeline
// listens to textOverlayTimeChanged and writes the new values back through
// updateTextOverlayTime.
class TextStripWidget : public QWidget {
public:
    using TimeChangeCallback = std::function<bool(int, double, double)>;
    using TimeEditCallback = std::function<void()>;
    using RowHeightChangeCallback = std::function<void(int)>;
    explicit TextStripWidget(QWidget *parent = nullptr) : QWidget(parent) {
        // Caller sets the height via setFixedHeight to match Timeline's
        // m_trackHeight so the text row looks like any V/A row.
        setStyleSheet("background-color: #2b2b2b;");
        setMouseTracking(true);
        setObjectName(QStringLiteral("TimelineTextStrip"));
        setAccessibleName(QStringLiteral("Timeline text overlays"));
    }
    void setOverlays(const QVector<EnhancedTextOverlay> &overlays, double clipDurationSec) {
        m_overlays = overlays;
        m_clipDuration = clipDurationSec;
        rebuildRowLayout();
        update();
    }
    void setPixelsPerSecond(double pps) { m_pps = pps; update(); }
    // Baseline height used when there is only one overlay (or zero) —
    // Timeline sets this to m_trackHeight so the T row matches V1/A1.
    void setSingleRowHeight(int h) {
        m_singleRowHeight = qMax(20, h);
        rebuildRowLayout();
        update();
    }
    // Called with (overlayIndex, newStartTime, newEndTime) whenever the
    // user drags one of an overlay bar's edge handles. A function
    // callback avoids Q_OBJECT + MOC plumbing for this inline widget.
    void setTimeChangeCallback(TimeChangeCallback cb) { m_timeChangeCb = std::move(cb); }
    void setTimeEditStartedCallback(TimeEditCallback cb) {
        m_timeEditStartedCb = std::move(cb);
    }
    void setTimeEditFinishedCallback(TimeEditCallback cb) {
        m_timeEditFinishedCb = std::move(cb);
    }
    // Called with the new row height (pixels) while the user drags the
    // row's bottom edge. Timeline resizes the strip AND its paired header.
    void setRowHeightChangeCallback(RowHeightChangeCallback cb) { m_rowHeightCb = std::move(cb); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#2b2b2b"));
        p.setPen(QColor("#555"));
        p.drawLine(0, height() - 1, width(), height() - 1);
        if (m_overlays.isEmpty() || m_pps <= 0.0)
            return;
        for (int i = 0; i < m_overlays.size(); ++i) {
            QRect r = rectForOverlay(i);
            if (r.width() <= 0) continue;
            p.setBrush(QColor(90, 130, 210, 200));
            p.setPen(QColor(140, 170, 230));
            p.drawRoundedRect(r, 3, 3);
            // Edge trim handles (brighter when hovering or dragging).
            const bool leftHot  = (m_hoverIdx == i && m_hoverEdge == -1) || (m_dragIdx == i && m_dragEdge == -1);
            const bool rightHot = (m_hoverIdx == i && m_hoverEdge == 1)  || (m_dragIdx == i && m_dragEdge == 1);
            p.fillRect(QRect(r.left(), r.top(), 3, r.height()),
                       leftHot ? QColor(255, 220, 120, 230) : QColor(255, 255, 255, 120));
            p.fillRect(QRect(r.right() - 2, r.top(), 3, r.height()),
                       rightHot ? QColor(255, 220, 120, 230) : QColor(255, 255, 255, 120));
            const auto &ov = m_overlays[i];
            // T1, T2, T3 row label — shown at the left of every overlay bar
            // so the user can immediately identify which text row this is.
            const QString rowLabel = QString("T%1").arg(m_overlayRowIdx[i] + 1);
            p.setPen(QColor(255, 220, 120));
            QFont lf = p.font();
            lf.setPointSize(qBound(8, qRound(r.height() * 0.45), 14));
            lf.setBold(true);
            p.setFont(lf);
            const QFontMetrics lfm(lf);
            const int labelW = lfm.horizontalAdvance(rowLabel) + 6;
            p.drawText(r.adjusted(4, 0, 0, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, rowLabel);
            if (!ov.text.isEmpty() && r.width() > 14 + labelW) {
                p.setPen(Qt::white);
                QFont f = p.font();
                const int pt = qBound(8, qRound(r.height() * 0.42), 14);
                f.setPointSize(pt);
                f.setBold(false);
                p.setFont(f);
                const QString elided = QFontMetrics(f)
                    .elidedText(ov.text, Qt::ElideRight, r.width() - labelW - 6);
                p.drawText(r.adjusted(4 + labelW, 0, -4, 0),
                           Qt::AlignVCenter | Qt::AlignLeft, elided);
            }
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        // Row-height drag has priority — it doesn't depend on m_pps.
        if (m_rowHeightDragging) {
            const int newH = qBound(20, m_rowHeightDragStartH + (event->pos().y() - m_rowHeightDragStartY), 240);
            if (m_rowHeightCb)
                m_rowHeightCb(newH);
            return;
        }
        if (m_pps <= 0.0) {
            setCursor(Qt::ArrowCursor);
            return;
        }
        if (m_dragIdx >= 0 && m_dragIdx < m_overlays.size()) {
            const double tSec = qMax(0.0, event->pos().x() / m_pps);
            const EnhancedTextOverlay previous = m_overlays.at(m_dragIdx);
            auto &ov = m_overlays[m_dragIdx];
            if (m_dragEdge == -1) {
                ov.startTime = qMin(tSec, ov.endTime - 0.1);
            } else if (m_dragEdge == 1) {
                ov.endTime = qMax(tSec, ov.startTime + 0.1);
            } else {
                // Body drag: translate the whole overlay preserving its
                // duration. The anchor is the press offset inside the bar
                // so the bar stays glued to the cursor.
                const double dur = qMax(0.1, m_dragOriginalEnd - m_dragOriginalStart);
                double newStart = tSec - m_dragAnchorOffsetSec;
                if (newStart < 0.0) newStart = 0.0;
                ov.startTime = newStart;
                ov.endTime = newStart + dur;
            }
            // Re-bin-pack so overlays that now overlap move to a new row.
            rebuildRowLayout();
            const double requestedStart = ov.startTime;
            const double requestedEnd = ov.endTime;
            if (m_timeChangeCb
                && !m_timeChangeCb(m_dragIdx, requestedStart, requestedEnd)) {
                // Timeline rejected the candidate (for example because it
                // would cross a neighbouring generated caption).  Restore
                // the widget model immediately so paint/hit-testing cannot
                // drift away from the canonical timeline state.
                m_overlays[m_dragIdx] = previous;
                rebuildRowLayout();
            }
            update();
            return;
        }
        // Bottom-edge hover (last 4 px) toggles the row-resize cursor so
        // the user sees the vertical drag affordance even with no overlays.
        if (event->pos().y() >= height() - 4) {
            setCursor(Qt::SizeVerCursor);
            return;
        }
        // Hover feedback — wider edge hit (7 px) so edges are easier to
        // grab, and SizeAllCursor over the body so the user sees it is
        // draggable as a whole.
        int hoverIdx = -1;
        int hoverEdge = 0;
        bool hoverBody = false;
        for (int i = 0; i < m_overlays.size(); ++i) {
            const QRect r = rectForOverlay(i);
            if (!r.contains(event->pos()))
                continue;
            if (qAbs(event->pos().x() - r.left()) <= 7) {
                hoverIdx = i; hoverEdge = -1; break;
            }
            if (qAbs(event->pos().x() - r.right()) <= 7) {
                hoverIdx = i; hoverEdge = 1; break;
            }
            hoverIdx = i; hoverEdge = 0; hoverBody = true; break;
        }
        if (hoverIdx != m_hoverIdx || hoverEdge != m_hoverEdge) {
            m_hoverIdx = hoverIdx;
            m_hoverEdge = hoverEdge;
            update();
        }
        if (hoverIdx < 0)
            setCursor(Qt::ArrowCursor);
        else if (hoverBody)
            setCursor(Qt::SizeAllCursor);
        else
            setCursor(Qt::SizeHorCursor);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) return;
        // Row-resize grip — bottom 4 px — starts a vertical drag that
        // changes THIS row's height independently of V/A tracks.
        if (event->pos().y() >= height() - 4) {
            m_rowHeightDragging = true;
            m_rowHeightDragStartY = event->pos().y();
            m_rowHeightDragStartH = height();
            setCursor(Qt::SizeVerCursor);
            return;
        }
        if (m_pps <= 0.0)
            return;
        for (int i = 0; i < m_overlays.size(); ++i) {
            const QRect r = rectForOverlay(i);
            if (!r.contains(event->pos())) continue;
            if (qAbs(event->pos().x() - r.left()) <= 7) {
                if (m_timeEditStartedCb)
                    m_timeEditStartedCb();
                m_dragIdx = i; m_dragEdge = -1;
                return;
            }
            if (qAbs(event->pos().x() - r.right()) <= 7) {
                if (m_timeEditStartedCb)
                    m_timeEditStartedCb();
                m_dragIdx = i; m_dragEdge = 1;
                return;
            }
            // Body drag — capture anchor so the bar slides with the cursor
            // without snapping its leading edge to the click point.
            if (m_timeEditStartedCb)
                m_timeEditStartedCb();
            m_dragIdx = i;
            m_dragEdge = 0;
            m_dragOriginalStart = m_overlays[i].startTime;
            m_dragOriginalEnd   = (m_overlays[i].endTime > 0.0)
                                    ? m_overlays[i].endTime
                                    : qMax(m_dragOriginalStart + 1.0, m_clipDuration);
            m_dragAnchorOffsetSec = (event->pos().x() / m_pps) - m_dragOriginalStart;
            return;
        }
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        const bool finishedTimingDrag = m_dragIdx >= 0;
        m_dragIdx = -1;
        m_dragEdge = 0;
        m_rowHeightDragging = false;
        if (finishedTimingDrag && m_timeEditFinishedCb)
            m_timeEditFinishedCb();
        update();
    }
private:
    QRect rectForOverlay(int i) const {
        if (i < 0 || i >= m_overlays.size()) return QRect();
        const auto &ov = m_overlays[i];
        const double start = qMax(0.0, ov.startTime);
        const double end   = (ov.endTime > 0.0) ? ov.endTime
                                                : qMax(start + 1.0, m_clipDuration);
        if (end <= start) return QRect();
        const int x = static_cast<int>(start * m_pps);
        const int w = qMax(4, static_cast<int>((end - start) * m_pps));
        // Row assignment from bin-packing: overlapping overlays get stacked
        // on separate sub-rows so they don't visually overlap.
        const int rows = qMax(1, m_rowCount);
        const int slotH = qMax(8, (height() - 4) / rows);
        const int row = (i < m_overlayRowIdx.size()) ? m_overlayRowIdx[i] : 0;
        const int y = 2 + row * slotH;
        return QRect(x, y, w, slotH - 1);
    }
    void rebuildRowLayout() {
        m_overlayRowIdx.clear();
        m_overlayRowIdx.resize(m_overlays.size());

        const QString generatedTag = CaptionOverlayBuilder::generatedTemplateName();
        int ordinaryRowCount = 0;
        QVector<int> generatedIndices;
        generatedIndices.reserve(m_overlays.size());
        for (int i = 0; i < m_overlays.size(); ++i) {
            if (m_overlays[i].templateName == generatedTag) {
                generatedIndices.append(i);
            } else {
                // Preserve the existing one-row-per-title behavior for
                // ordinary authored overlays.
                m_overlayRowIdx[i] = ordinaryRowCount++;
            }
        }

        // Single-word captions are normally sequential.  Interval-color only
        // the reserved generated set so hundreds of words share one row,
        // while genuinely overlapping captions receive additional rows.
        std::sort(generatedIndices.begin(), generatedIndices.end(),
                  [this](int lhs, int rhs) {
                      if (m_overlays[lhs].startTime != m_overlays[rhs].startTime)
                          return m_overlays[lhs].startTime < m_overlays[rhs].startTime;
                      return m_overlays[lhs].endTime < m_overlays[rhs].endTime;
                  });
        QVector<double> generatedRowEnds;
        for (int index : std::as_const(generatedIndices)) {
            const auto &overlay = m_overlays[index];
            const double start = qMax(0.0, overlay.startTime);
            const double end = overlay.endTime > start
                ? overlay.endTime
                : qMax(start + 0.1, m_clipDuration);
            int generatedRow = -1;
            for (int row = 0; row < generatedRowEnds.size(); ++row) {
                if (generatedRowEnds[row] <= start) {
                    generatedRow = row;
                    break;
                }
            }
            if (generatedRow < 0) {
                generatedRow = generatedRowEnds.size();
                generatedRowEnds.append(end);
            } else {
                generatedRowEnds[generatedRow] = end;
            }
            m_overlayRowIdx[index] = ordinaryRowCount + generatedRow;
        }

        m_rowCount = qMax(1, ordinaryRowCount + generatedRowEnds.size());
        const int target = qMax(m_singleRowHeight, m_rowCount * kRowSlotHeight + 4);
        if (target != height() && m_rowHeightCb)
            m_rowHeightCb(target);
    }
    QVector<EnhancedTextOverlay> m_overlays;
    QVector<int> m_overlayRowIdx;  // row assignment per overlay
    int m_rowCount = 1;
    int m_singleRowHeight = 50;  // baseline when no overlays (matches V1 row)
    static constexpr int kRowSlotHeight = 26;
    double m_clipDuration = 0.0;
    double m_pps = 50.0;
    int m_dragIdx = -1;
    int m_dragEdge = 0;   // -1 = left edge, 1 = right edge, 0 = body (move)
    double m_dragOriginalStart = 0.0;
    double m_dragOriginalEnd = 0.0;
    double m_dragAnchorOffsetSec = 0.0;
    int m_hoverIdx = -1;
    int m_hoverEdge = 0;
    TimeChangeCallback m_timeChangeCb;
    TimeEditCallback m_timeEditStartedCb;
    TimeEditCallback m_timeEditFinishedCb;
    RowHeightChangeCallback m_rowHeightCb;
    bool m_rowHeightDragging = false;
    int m_rowHeightDragStartY = 0;
    int m_rowHeightDragStartH = 0;
};
#include <optional>
#include <functional>
#include <cmath>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QWheelEvent>
#include <QDebug>
#include <QCoreApplication>
#include <QPair>
#include <QTimer>
#include <QDrag>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QPixmap>
#include <QPainter>
#include <QMenu>
#include <QSettings>
#include <QAction>
#include <QToolTip>
#include <QHelpEvent>
#include <QHash>
#include <QProgressDialog>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// --- PlayheadOverlay ---

PlayheadOverlay::PlayheadOverlay(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
}

void PlayheadOverlay::setPlayheadX(int x) { m_playheadX = x; update(); }

void PlayheadOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(0xFF, 0x44, 0x44), 2));
    painter.drawLine(m_playheadX, 0, m_playheadX, height());
    QPolygon tri;
    tri << QPoint(m_playheadX - 6, 0) << QPoint(m_playheadX + 6, 0) << QPoint(m_playheadX, 10);
    painter.setBrush(QColor(0xFF, 0x44, 0x44));
    painter.drawPolygon(tri);
}

void PlayheadOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_dragging = true;
    m_playheadX = qBound(0, event->pos().x(), width());
    grabMouse();
    emit playheadMoved(m_playheadX);
    event->accept();
    update();
}

void PlayheadOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    m_playheadX = qBound(0, event->pos().x(), width());
    emit playheadMoved(m_playheadX);
    event->accept();
    update();
}

void PlayheadOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    m_dragging = false;
    releaseMouse();
    m_playheadX = qBound(0, event->pos().x(), width());
    emit playheadReleased(m_playheadX);
    event->accept();
    update();
}

// --- TimeRuler ---

TimeRuler::TimeRuler(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(22);
    setStyleSheet("background-color: #2d2d2d;");
    setMouseTracking(true);
    setCursor(Qt::SizeHorCursor);
    setToolTip("Drag horizontally to zoom timeline (right=in, left=out)");
}

void TimeRuler::setPixelsPerSecond(double pps)
{
    pps = qBound(0.02, pps, 200.0);
    if (qFuzzyCompare(pps, m_pixelsPerSecond))
        return;
    m_pixelsPerSecond = pps;
    update();
}

void TimeRuler::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x2d, 0x2d, 0x2d));
    painter.setPen(QPen(QColor(0xaa, 0xaa, 0xaa), 1));

    const int w = width();
    const int h = height();

    int majorIntervalSec = 1;
    if (m_pixelsPerSecond < 0.2) majorIntervalSec = 3600;       // 1h marks for 4h+ clips
    else if (m_pixelsPerSecond < 0.5) majorIntervalSec = 1800;  // 30 min
    else if (m_pixelsPerSecond < 1.0) majorIntervalSec = 600;   // 10 min
    else if (m_pixelsPerSecond < 2.0) majorIntervalSec = 300;
    else if (m_pixelsPerSecond < 3.0) majorIntervalSec = 120;
    else if (m_pixelsPerSecond < 4.0) majorIntervalSec = 60;
    else if (m_pixelsPerSecond < 8.0) majorIntervalSec = 30;
    else if (m_pixelsPerSecond < 16.0) majorIntervalSec = 10;
    else if (m_pixelsPerSecond < 40.0) majorIntervalSec = 5;
    else if (m_pixelsPerSecond < 80.0) majorIntervalSec = 2;

    const int majorPx = qMax(1, static_cast<int>(majorIntervalSec * m_pixelsPerSecond));
    if (majorPx <= 0) return;

    const int minorPerMajor = 5;
    const int minorPx = qMax(1, majorPx / minorPerMajor);

    QFont font = painter.font();
    font.setPointSize(8);
    painter.setFont(font);

    for (int x = 0; x < w; x += minorPx) {
        const bool isMajor = (x % majorPx) == 0;
        if (isMajor) {
            painter.setPen(QPen(QColor(0xdd, 0xdd, 0xdd), 1));
            painter.drawLine(x, h - 8, x, h);
            const int totalSec = static_cast<int>(x / m_pixelsPerSecond);
            const int hours = totalSec / 3600;
            const int mins = (totalSec % 3600) / 60;
            const int secs = totalSec % 60;
            QString label = (hours > 0)
                ? QString("%1:%2:%3").arg(hours).arg(mins, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'))
                : QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
            painter.drawText(QRect(x + 2, 0, 60, h - 8), Qt::AlignLeft | Qt::AlignVCenter, label);
        } else {
            painter.setPen(QPen(QColor(0x77, 0x77, 0x77), 1));
            painter.drawLine(x, h - 4, x, h);
        }
    }

    painter.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
    painter.drawLine(0, h - 1, w, h - 1);
}

void TimeRuler::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    // Use global (screen) X, not widget-local X. While dragging, setZoomLevel
    // resizes the ruler and the scroll area may auto-scroll, which would
    // shift the ruler's widget-relative origin under the cursor and cause the
    // drag delta to spiral — zoom→resize→shift→bigger delta→more zoom, runaway.
    m_dragStartX = event->globalPosition().toPoint().x();
    m_dragStartPps = m_pixelsPerSecond;
    grabMouse();
    emit zoomDragStarted();
    event->accept();
}

void TimeRuler::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int dx = event->globalPosition().toPoint().x() - m_dragStartX;
    // Multiplicative drag: each pixel multiplies zoom by ~2%. Works uniformly
    // from 0.02 pps (20h clip fits) up to 200 pps (frame-precise).
    const double factor = std::pow(1.02, static_cast<double>(dx));
    const double newPps = qBound(0.02, m_dragStartPps * factor, 200.0);
    if (!qFuzzyCompare(newPps, m_pixelsPerSecond)) {
        m_pixelsPerSecond = newPps;
        emit zoomChanged(newPps);
        update();
    }
    event->accept();
}

void TimeRuler::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    releaseMouse();
    emit zoomDragEnded();
    event->accept();
}

// --- MarkerLane ---
// 16px-tall strip painted above the time ruler. Reads markers directly from
// the owning Timeline so there's no per-marker QObject and no manual
// add/remove syncing — every paint reflects the current QVector<Marker>.

MarkerLane::MarkerLane(Timeline *timeline, QWidget *parent)
    : QWidget(parent), m_timeline(timeline)
{
    setFixedHeight(kLaneHeight);
    setStyleSheet("background-color: #232323;");
    setMouseTracking(true);
    // Enable hover tooltips via QHelpEvent without per-marker setToolTip.
    setAttribute(Qt::WA_Hover, true);
}

void MarkerLane::setPixelsPerSecond(double pps)
{
    if (qFuzzyCompare(pps, m_pixelsPerSecond))
        return;
    m_pixelsPerSecond = qBound(0.02, pps, 200.0);
    update();
}

void MarkerLane::setScrollOffset(int offsetX)
{
    if (offsetX == m_scrollOffset)
        return;
    m_scrollOffset = offsetX;
    update();
}

int MarkerLane::markerScreenX(qint64 timelineUs) const
{
    // contentX = timelineUs / 1e6 * pps. Subtract scroll offset so the lane
    // pans together with the tracks below.
    const double seconds = static_cast<double>(timelineUs) / 1'000'000.0;
    const int contentX = static_cast<int>(seconds * m_pixelsPerSecond);
    return contentX - m_scrollOffset;
}

int MarkerLane::hitTestMarker(const QPoint &pos) const
{
    if (!m_timeline) return -1;
    const int half = kTriangleSize / 2 + kHitPad;
    // Reverse iterate so a topmost (rightmost-painted) marker wins on overlap.
    const auto &markers = m_timeline->markers();
    for (int i = markers.size() - 1; i >= 0; --i) {
        const int x = markerScreenX(markers[i].timelineUs);
        if (qAbs(pos.x() - x) <= half && pos.y() <= kLaneHeight)
            return markers[i].id;
    }
    return -1;
}

void MarkerLane::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x23, 0x23, 0x23));
    // Bottom hairline so the lane visually sits as a tray above the ruler.
    painter.setPen(QPen(QColor(0x3a, 0x3a, 0x3a), 1));
    painter.drawLine(0, height() - 1, width(), height() - 1);

    if (!m_timeline) return;
    const auto &markers = m_timeline->markers();
    if (markers.isEmpty()) return;

    painter.setRenderHint(QPainter::Antialiasing, true);
    const int half = kTriangleSize / 2;
    const int top = 2;                  // small top gap so triangle base
    const int tipY = top + kTriangleSize; // hangs cleanly off the ruler line.

    // MK-1: span (duration) markers first, so the triangle/label below
    // paints on top of the bar. A point marker (durationUs == 0) draws no
    // bar — it stays byte-identical to the legacy single-triangle look.
    for (const auto &m : markers) {
        if (m.durationUs <= 0) continue;
        const int x0 = markerScreenX(m.timelineUs);
        const int x1 = markerScreenX(m.timelineUs + m.durationUs);
        // Cull bars fully off either edge.
        if (x1 <= 0 || x0 >= width()) continue;
        const int barLeft  = qMax(0, x0);
        const int barRight = qMin(width(), x1);
        const int barW = qMax(1, barRight - barLeft);
        const QColor base = m.color.isValid() ? m.color : QColor("#ff5050");
        QColor barFill = base;
        barFill.setAlpha(80);  // half-transparent so the ruler shows through
        // Fill the full lane height minus the bottom hairline.
        painter.setPen(Qt::NoPen);
        painter.setBrush(barFill);
        painter.drawRect(QRect(barLeft, 0, barW, height() - 1));
    }

    for (const auto &m : markers) {
        const int x = markerScreenX(m.timelineUs);
        // Cull off-screen triangles cheaply — important for the
        // 100-marker spec acceptance.
        if (x + half < 0 || x - half > width()) continue;

        QPolygonF tri;
        tri << QPointF(x - half, top)
            << QPointF(x + half, top)
            << QPointF(x,        tipY);
        const QColor fill = m.color.isValid() ? m.color : QColor("#ff5050");
        painter.setBrush(fill);
        painter.setPen(QPen(fill.darker(140), 1));
        painter.drawPolygon(tri);
    }
}

void MarkerLane::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int id = hitTestMarker(event->pos());
    if (id >= 0) {
        emit markerClicked(id);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

bool MarkerLane::event(QEvent *event)
{
    // Hover tooltip — show label + H:MM:SS.mmm time. Computed lazily so
    // there's no per-marker setToolTip overhead even with hundreds of
    // markers.
    if (event->type() == QEvent::ToolTip && m_timeline) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        const int id = hitTestMarker(helpEvent->pos());
        if (id >= 0) {
            const Marker mk = m_timeline->markerById(id);
            const qint64 us = mk.timelineUs;
            const qint64 totalMs = us / 1000;
            const int hours = static_cast<int>(totalMs / 3'600'000);
            const int mins  = static_cast<int>((totalMs / 60'000) % 60);
            const int secs  = static_cast<int>((totalMs / 1000) % 60);
            const int millis = static_cast<int>(totalMs % 1000);
            const QString timeStr = QString("%1:%2:%3.%4")
                .arg(hours)
                .arg(mins,   2, 10, QChar('0'))
                .arg(secs,   2, 10, QChar('0'))
                .arg(millis, 3, 10, QChar('0'));
            const QString text = mk.label.isEmpty()
                ? timeStr
                : (mk.label + QChar('\n') + timeStr);
            QToolTip::showText(helpEvent->globalPos(), text, this);
            return true;
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QWidget::event(event);
}

// --- TimelineTrack ---

TimelineTrack::TimelineTrack(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(m_rowHeight);
    setMaximumHeight(m_rowHeight);
    setMouseTracking(true);
    setAcceptDrops(true);
}

void TimelineTrack::setRowHeight(int h)
{
    h = qBound(20, h, 300);
    if (h == m_rowHeight) return;
    m_rowHeight = h;
    setMinimumHeight(m_rowHeight);
    setMaximumHeight(m_rowHeight);
    update();
}

void TimelineTrack::ensureUid(ClipInfo &clip)
{
    if (clip.uid.isEmpty()) clip.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QVector<ClipInfo> &TimelineTrack::mutableClips()
{
    if (m_timeline) m_timeline->noteClipsChanged();
    return m_clips;
}

void TimelineTrack::addClip(const ClipInfo &clip)
{
    ClipInfo added = clip;
    ensureUid(added);
    mutableClips().append(added); updateMinimumWidth(); update(); emit modified();
}
void TimelineTrack::insertClip(int index, const ClipInfo &clip)
{
    if (index < 0 || index > m_clips.size()) index = m_clips.size();
    ClipInfo inserted = clip;
    ensureUid(inserted);
    mutableClips().insert(index, inserted); updateMinimumWidth(); update(); emit modified();
}

void TimelineTrack::removeClip(int index)
{
    if (index < 0 || index >= m_clips.size()) return;
    mutableClips().removeAt(index);
    QList<int> newSel;
    for (int s : m_selectedClips) {
        if (s < index) newSel.append(s);
        else if (s > index) newSel.append(s - 1);
    }
    m_selectedClips = newSel;
    const int primary = m_selectedClips.isEmpty() ? -1 : m_selectedClips.last();
    updateMinimumWidth(); update(); emit selectionChanged(primary, false); emit modified();
}

void TimelineTrack::moveClip(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_clips.size()) return;
    if (toIndex < 0 || toIndex >= m_clips.size()) return;
    if (fromIndex == toIndex) return;
    ClipInfo clip = m_clips[fromIndex];
    mutableClips().removeAt(fromIndex);
    mutableClips().insert(toIndex, clip);
    m_selectedClips = {toIndex};
    updateMinimumWidth(); update();
    emit selectionChanged(toIndex, false);
    emit clipMoved(fromIndex, toIndex); emit modified();
}

namespace {

// US-1501 consumed-interval split (rule (73)) of a clip whose source position
// is not linear in local time. Each piece covers local [a, b] of the clip and
// keeps exactly the source it consumes there: with g(t) the source consumed
// before the reverse fold (in [0, out - in]) and [mn, mx] its range on the
// piece, the piece is [in + mn, in + mx] (reversed [out - mx, out - mn]) at
// speed' = (mx - mn) / (b - a), so both halves map every local second to the
// source of the unsplit clip and keep the lengths L / D - L. A piece whose
// source does not move (mx - mn < 1 us * speed) keeps the speed and holds F
// with a one-key remap; a ramp piece whose speeds leave [0.1, 10] becomes a
// time-remap curve. Audio tracks never get a remap (AudioMixer does not read
// it): there a held or out-of-range forward piece plays its source 1:1 from
// min(in + mn, out - len * speed), and an out-of-range reversed piece plays
// the mirrored range at speed'.
bool splitClipByConsumedSource(const ClipInfo &clip, double localSec, bool audioTrack,
                               ClipInfo *left, ClipInfo *right)
{
    constexpr double kSplitEps = 1e-6;
    const double inSec = clip.inPoint;
    const double outSec = (clip.outPoint > 0.0) ? clip.outPoint : clip.duration;
    const double totalSec = clip.effectiveDuration();
    if (!(outSec > inSec) || !std::isfinite(totalSec) || localSec <= kSplitEps
        || localSec >= totalSec - kSplitEps) {
        return false;
    }
    const double speed = (clip.speed > 0.0) ? clip.speed : 1.0;
    const double rangeSec = outSec - inSec;
    const bool useRemap = !audioTrack && !clip.timeRemapCurve.keys.isEmpty();
    ClipInfo mapping = clip;
    if (!useRemap)
        mapping.timeRemapCurve = timeremap::TimeRemapCurve{};
    const auto consumedSec = [&](double t) {
        const double sourceSec = mapping.sourceSecondAtLocalTime(t);
        return qBound(0.0, clip.reversed ? outSec - sourceSec : sourceSec - inSec, rangeSec);
    };

    const auto makePiece = [&](double fromSec, double toSec, ClipInfo *out) {
        const double lenSec = toSec - fromSec;
        double mn = qMin(consumedSec(fromSec), consumedSec(toSec));
        double mx = qMax(consumedSec(fromSec), consumedSec(toSec));
        if (useRemap) {
            for (const timeremap::TimeRemapKey &key : clip.timeRemapCurve.keys) {
                if (key.outTime <= fromSec || key.outTime >= toSec)
                    continue;
                const double keySec = qBound(0.0, key.srcTime, rangeSec);
                mn = qMin(mn, keySec);
                mx = qMax(mx, keySec);
            }
        }
        const double heldSourceSec = mapping.sourceSecondAtLocalTime(fromSec);

        ClipInfo piece = clip;
        piece.speedRamp = speedramp::SpeedRamp::identity();
        piece.timeRemapCurve = timeremap::TimeRemapCurve{};
        const bool held = mx - mn < 1e-6 * speed;
        bool plainOneToOne = false;
        if (!held) {
            piece.speed = (mx - mn) / lenSec;
            piece.inPoint = clip.reversed ? outSec - mx : inSec + mn;
            piece.outPoint = clip.reversed ? outSec - mn : inSec + mx;
            if (useRemap) {
                piece.speedRamp = clip.speedRamp;
                piece.timeRemapCurve = clip.timeRemapCurve.sliced(fromSec, toSec, mn);
            } else if (!clip.speedRamp.sliced(qRound64(fromSec * speed * 1'000'000.0),
                                              qRound64(toSec * speed * 1'000'000.0),
                                              piece.speed / speed, speed / piece.speed,
                                              &piece.speedRamp)) {
                if (audioTrack) {
                    plainOneToOne = !clip.reversed;
                } else {
                    timeremap::TimeRemapCurve curve;
                    curve.blendMode = timeremap::FrameBlendMode::NearestFrame;
                    curve.sourceFps = 0.0;
                    const QVector<speedramp::SpeedRamp::CurvePoint> points =
                        clip.speedRamp.approximateSourceCurve(fromSec, toSec, speed, rangeSec);
                    for (const speedramp::SpeedRamp::CurvePoint &point : points)
                        curve.keys.append({point.localSec, point.sourceSec - mn});
                    piece.timeRemapCurve = curve;
                }
            }
        }
        if (held || plainOneToOne) {
            const double spanSec = lenSec * speed;
            piece.speed = clip.speed;
            piece.speedRamp = speedramp::SpeedRamp::identity();
            piece.timeRemapCurve = timeremap::TimeRemapCurve{};
            if (clip.reversed) {
                piece.outPoint = qMax(outSec - mn, inSec + spanSec);
                piece.inPoint = piece.outPoint - spanSec;
            } else {
                piece.inPoint = qMin(inSec + mn, outSec - spanSec);
                piece.outPoint = piece.inPoint + spanSec;
            }
            if (held && !audioTrack) {
                timeremap::TimeRemapCurve hold;
                hold.blendMode = useRemap ? clip.timeRemapCurve.blendMode
                                          : timeremap::FrameBlendMode::NearestFrame;
                hold.sourceFps = useRemap ? clip.timeRemapCurve.sourceFps : 0.0;
                hold.addKey(0.0, clip.reversed ? piece.outPoint - heldSourceSec
                                               : heldSourceSec - piece.inPoint);
                piece.timeRemapCurve = hold;
            }
        }
        if (!(piece.inPoint < piece.outPoint))
            return false;
        *out = piece;
        return true;
    };
    return makePiece(0.0, localSec, left) && makePiece(localSec, totalSec, right);
}

// US-1807: effect keyframe tracks are named by effect index,
// "effect.<i>.<rest>" (parsed as EffectControlsPanel's parseEffectTrackName).
bool parseSplitEffectTrackName(const QString &trackName, int *effectIndex, QString *rest)
{
    static const QString prefix = QStringLiteral("effect.");
    if (!trackName.startsWith(prefix)) return false;
    const int indexStart = prefix.size();
    const int dotPos = trackName.indexOf(QLatin1Char('.'), indexStart);
    if (dotPos <= indexStart) return false;
    bool ok = false;
    const int parsedIndex = trackName.mid(indexStart, dotPos - indexStart).toInt(&ok);
    if (!ok || parsedIndex < 0) return false;
    *effectIndex = parsedIndex;
    *rest = trackName.mid(dotPos + 1);
    return true;
}

// Drops the tracks of the removed effect and moves the tracks of every later
// effect down by one index, as the Effect Controls delete path does.
void removeSplitEffectTracks(KeyframeManager &keyframes, int removedIndex)
{
    QStringList removedNames;
    QVector<QPair<int, QString>> laterTracks;
    for (const KeyframeTrack &track : keyframes.tracks()) {
        int index = -1;
        QString rest;
        if (!parseSplitEffectTrackName(track.propertyName(), &index, &rest)) continue;
        if (index == removedIndex) removedNames.append(track.propertyName());
        else if (index > removedIndex) laterTracks.append({index, rest});
    }
    for (const QString &name : removedNames) keyframes.removeTrack(name);
    std::stable_sort(laterTracks.begin(), laterTracks.end(),
                     [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                         return a.first < b.first;
                     });
    for (const QPair<int, QString> &ref : laterTracks) {
        keyframes.renameTrack(QStringLiteral("effect.%1.%2").arg(ref.first).arg(ref.second),
                              QStringLiteral("effect.%1.%2").arg(ref.first - 1).arg(ref.second));
    }
    QVector<StringKeyframeTrack> &stringTracks = keyframes.stringTracks();
    for (int s = stringTracks.size() - 1; s >= 0; --s) {
        int index = -1;
        QString rest;
        if (!parseSplitEffectTrackName(stringTracks[s].propertyName(), &index, &rest)) continue;
        if (index == removedIndex) {
            stringTracks.removeAt(s);
        } else if (index > removedIndex) {
            StringKeyframeTrack renamed(QStringLiteral("effect.%1.%2").arg(index - 1).arg(rest),
                                        stringTracks[s].defaultValue());
            for (const StringKeyframePoint &point : stringTracks[s].keyframes())
                renamed.addKeyframe(point.time, point.value);
            stringTracks[s] = renamed;
        }
    }
}

} // namespace

void TimelineTrack::splitClipAt(int index, double localSeconds, bool notify)
{
    if (index < 0 || index >= m_clips.size()) return;
    ClipInfo &original = mutableClips()[index];
    double effectiveStart = original.inPoint;
    double effectiveEnd = (original.outPoint > 0.0) ? original.outPoint : original.duration;
    // US-1501: speed-ramped / time-remapped clips are cut by the consumed
    // source (splitClipByConsumedSource). Audio tracks follow the ramp only
    // where AudioMixer reads it (atempo or reversed) and never the remap.
    const bool hasRamp = !original.speedRamp.isIdentity();
    const bool hasRemap = !original.timeRemapCurve.keys.isEmpty();
    const bool consumedRule = m_isAudioTrack
        ? hasRamp && (original.atempoEnabled || original.reversed)
        : hasRamp || hasRemap;
    ClipInfo secondHalf = original;
    if (consumedRule) {
        ClipInfo firstHalf = original;
        if (!splitClipByConsumedSource(original, localSeconds, m_isAudioTrack,
                                       &firstHalf, &secondHalf)) {
            return;
        }
        original = firstHalf;
    } else {
        // A reversed clip with a uniform speed plays its source from out down
        // to in, so local second L sits at source out - L * speed: the left
        // half keeps [split, out] and the right half takes [in, split]. Only
        // uniform clips reach here (an audio clip's ramp without atempo, or
        // its remap, is not played; the remap is dropped below).
        const bool reversedUniform = original.reversed;
        double splitPoint = reversedUniform ? effectiveEnd - localSeconds * original.speed
                                            : effectiveStart + localSeconds * original.speed;
        constexpr double kSplitEps = 1e-6;
        if (splitPoint <= effectiveStart + kSplitEps || splitPoint >= effectiveEnd - kSplitEps) return;
        secondHalf.inPoint = reversedUniform ? effectiveStart : splitPoint;
        secondHalf.outPoint = reversedUniform ? splitPoint : effectiveEnd;
        if (reversedUniform) {
            original.inPoint = splitPoint;
            original.outPoint = effectiveEnd;
        } else {
            original.outPoint = splitPoint;
        }
        if (hasRemap) {
            original.timeRemapCurve = timeremap::TimeRemapCurve{};
            secondHalf.timeRemapCurve = timeremap::TimeRemapCurve{};
        }
    }
    secondHalf.leadInSec = 0.0;
    // The cut creates a hard edge between the halves: the right half's left
    // edge is no longer the original's leadIn, and the left half's right edge
    // is no longer the original's trailOut. Reset those so neither transition
    // is duplicated onto both halves.
    secondHalf.leadIn = Transition{};
    // US-1807: both halves keep the 3D text and the brush. Everything the right
    // half reads in clip-local seconds moves by the cut (timeline seconds, not
    // the source-second splitPoint) so it plays as it did before the split.
    // Every split path comes through here.
    const double cutLocalSec = localSeconds;
    // Keyframes: shift every key, keep the negative ones (valueAt interpolates
    // from them) and write back in ascending index order so the sort that
    // setKeyframePoint / setKeyframeTime run keeps the order.
    for (KeyframeTrack &track : secondHalf.keyframes.tracks()) {
        for (int k = 0; k < track.count(); ++k) {
            KeyframePoint point = track.keyframes()[k];
            point.time -= cutLocalSec;
            track.setKeyframePoint(k, point);
        }
    }
    for (StringKeyframeTrack &track : secondHalf.keyframes.stringTracks()) {
        for (int k = 0; k < track.count(); ++k)
            track.setKeyframeTime(k, track.keyframes()[k].time - cutLocalSec);
    }
    // Effect windows (>= 0 is a limit): a window that ended before the cut
    // leaves the right half; endSec at the cut stays (effectActiveAt includes
    // endSec); a start before the cut becomes 0.
    for (int e = secondHalf.effects.size() - 1; e >= 0; --e) {
        VideoEffect &fx = secondHalf.effects[e];
        if (fx.endSec >= 0.0) {
            if (fx.endSec - cutLocalSec < 0.0) {
                secondHalf.effects.removeAt(e);
                // Its tracks go too, and the later effects' tracks follow
                // their effects down one index (the key times are already
                // shifted above, so they move with the renamed tracks).
                removeSplitEffectTracks(secondHalf.keyframes, e);
                continue;
            }
            fx.endSec -= cutLocalSec;
        }
        if (fx.startSec >= 0.0)
            fx.startSec = std::max(0.0, fx.startSec - cutLocalSec);
    }
    // Volume envelope: shift, drop the points before the cut and start the
    // right half at the gain the original had at the cut (evaluated on a
    // time-sorted copy) unless a point already sits at the cut (within 1e-9 s).
    if (!secondHalf.volumeEnvelope.isEmpty()) {
        constexpr double kEnvelopeCutEps = 1e-9;
        QVector<AudioGainPoint> sortedEnvelope = secondHalf.volumeEnvelope;
        std::stable_sort(sortedEnvelope.begin(), sortedEnvelope.end(),
                         [](const AudioGainPoint &a, const AudioGainPoint &b) {
                             return a.time < b.time;
                         });
        const double gainAtCut = audio::evaluateVolumeEnvelope(
            sortedEnvelope, cutLocalSec, secondHalf.volume);
        QVector<AudioGainPoint> shifted;
        bool hasPointAtCut = false;
        for (AudioGainPoint point : secondHalf.volumeEnvelope) {
            point.time -= cutLocalSec;
            if (point.time < -kEnvelopeCutEps) continue;
            if (std::abs(point.time) <= kEnvelopeCutEps) {
                point.time = 0.0;
                hasPointAtCut = true;
            }
            shifted.append(point);
        }
        if (!hasPointAtCut) shifted.prepend(AudioGainPoint{0.0, gainAtCut});
        secondHalf.volumeEnvelope = shifted;
    }
    // 3D text: the animation time offset accumulates over repeated splits.
    if (!secondHalf.text3DConfig.isEmpty()) {
        const double previousOffset =
            secondHalf.text3DConfig.value(QStringLiteral("timeOffsetSec")).toDouble(0.0);
        secondHalf.text3DConfig[QStringLiteral("timeOffsetSec")] =
            std::isfinite(previousOffset) ? previousOffset + cutLocalSec : cutLocalSec;
    }
    // The right half is a new clip; the left half keeps the original's uid.
    secondHalf.uid.clear();
    ensureUid(secondHalf);
    original.trailOut = Transition{};
    const QString leftUid = original.uid;
    mutableClips().insert(index + 1, secondHalf);
    emit clipSplit(leftUid, secondHalf.uid, localSeconds);
    if (notify) {
        updateMinimumWidth(); update(); emit modified();
    }
}

bool TimelineTrack::applyTrim(int clipIndex, trimops::TrimType type,
                              double deltaSec, QString *errorOut)
{
    // 純粋エンジン trimops:: が境界判定と in-place 変更を担う。成功時のみ
    // split/insert と同じ後処理 (幅再計算 + 再描画 + modified 発火) を行う。
    // 失敗時は m_clips が一切変更されないので何もせず false を返す。
    if (!trimops::applyTrim(mutableClips(), clipIndex, type, deltaSec, errorOut))
        return false;
    updateMinimumWidth(); update(); emit modified();
    return true;
}

void TimelineTrack::applyDragMove(int clipIdx, double leadIn, double nextLeadIn)
{
    if (clipIdx < 0 || clipIdx >= m_clips.size()) return;
    mutableClips()[clipIdx].leadInSec = qMax(0.0, leadIn);
    if (nextLeadIn >= 0.0 && clipIdx + 1 < m_clips.size())
        mutableClips()[clipIdx + 1].leadInSec = qMax(0.0, nextLeadIn);
    updateMinimumWidth();
    update();
}

void TimelineTrack::setLinkedTrimBounds(double lo, double hi)
{
    m_linkedTrimLo = lo;
    m_linkedTrimHi = hi;
    m_linkedTrimActive = true;
}

void TimelineTrack::rejectLinkedTrim()
{
    m_linkedTrimRejected = true;
    m_linkedTrimActive = false;
}

void TimelineTrack::applyLinkedTrim(int clipIdx, bool leftEdge, double edgePoint, double gapLeadIn,
                                    double nextLeadIn)
{
    if (clipIdx < 0 || clipIdx >= m_clips.size()) return;
    ClipInfo &clip = mutableClips()[clipIdx];
    if (leftEdge) {
        clip.inPoint = edgePoint;
        clip.leadInSec = qMax(0.0, gapLeadIn);
        if (nextLeadIn >= 0.0 && clipIdx + 1 < m_clips.size())
            mutableClips()[clipIdx + 1].leadInSec = qMax(0.0, nextLeadIn);
    } else {
        clip.outPoint = edgePoint;
        if (gapLeadIn >= 0.0 && clipIdx + 1 < m_clips.size())
            mutableClips()[clipIdx + 1].leadInSec = qMax(0.0, gapLeadIn);
    }
    updateMinimumWidth();
    update();
}

void TimelineTrack::removeClipPreservingDownstream(int index, bool notify)
{
    if (index < 0 || index >= m_clips.size()) return;
    const double absorbed = m_clips[index].leadInSec + m_clips[index].effectiveDuration();
    mutableClips().removeAt(index);
    // Push the vacated time into the next clip's leadInSec so everything
    // downstream stays at the same absolute timeline position.
    if (index < m_clips.size())
        mutableClips()[index].leadInSec += absorbed;
    QList<int> newSel;
    for (int s : m_selectedClips) {
        if (s < index) newSel.append(s);
        else if (s > index) newSel.append(s - 1);
    }
    m_selectedClips = newSel;
    const int primary = m_selectedClips.isEmpty() ? -1 : m_selectedClips.last();
    if (notify) {
        updateMinimumWidth(); update();
        emit selectionChanged(primary, false);
        emit modified();
    }
}

void TimelineTrack::insertClipPreservingDownstream(int index, const ClipInfo &clip, double leadInSec)
{
    if (index < 0) index = 0;
    if (index > m_clips.size()) index = m_clips.size();
    ClipInfo newClip = clip;
    ensureUid(newClip);
    newClip.leadInSec = qMax(0.0, leadInSec);
    const double consumed = newClip.leadInSec + newClip.effectiveDuration();
    mutableClips().insert(index, newClip);
    // Selected clips at or after the insert point moved one slot right (the
    // mirror of removeClipPreservingDownstream); the selection follows them.
    bool selectionMoved = false;
    for (int &s : m_selectedClips)
        if (s >= index) { ++s; selectionMoved = true; }
    // The existing clip that now sits at index+1 used to start at
    //   (prevEnd + oldLeadIn)
    // and must continue to start at that same absolute time. After the
    // insertion, prevEnd effectively advanced by `consumed`, so we shave
    // `consumed` off its leadInSec. If we'd go negative the plan was bad.
    if (index + 1 < m_clips.size()) {
        double &nextLead = mutableClips()[index + 1].leadInSec;
        nextLead -= consumed;
        if (nextLead < 0) nextLead = 0;
    }
    updateMinimumWidth(); update();
    // Not selectionChanged: that starts the linked selection sync mid-mutation.
    if (selectionMoved) emit selectionShifted(selectedClip());
    emit modified();
}

void TimelineTrack::insertClip3Point(double timelineStartSec, const ClipInfo &clip)
{
    // ripple insert: T を跨ぐ位置に割り込み、後続クリップを右へ押し出す。
    // 配置 index と先行ギャップ leadIn は純粋エンジンに委ねる。
    double leadIn = 0.0;
    const int index = threepoint::insertIndexForTime(m_clips, timelineStartSec, &leadIn);
    insertClipPreservingDownstream(index, clip, leadIn);
    // insertClipPreservingDownstream が update() / emit modified() を済ませている。
}

void TimelineTrack::overwriteClip3Point(double timelineStartSec, const ClipInfo &clip)
{
    const threepoint::OverwritePlan plan =
        threepoint::planOverwrite(m_clips, timelineStartSec, clip.effectiveDuration());
    if (!plan.valid)
        return;
    // SM-1 notes の実行順序を厳守する:
    //   (1) tail を先に分割 (head 分割の +1 ズレが tail index に波及しないように)
    if (plan.splitTailIndex >= 0)
        splitClipAt(plan.splitTailIndex, plan.splitTailLocalSec);
    //   (2) head を分割
    if (plan.splitHeadIndex >= 0)
        splitClipAt(plan.splitHeadIndex, plan.splitHeadLocalSec);
    //   (3) [T,T+L) に完全に収まる既存クリップ群を、 downstream の絶対位置を
    //       保ったまま末尾側から削除 (前から消すと後続インデックスがズレる)。
    for (int i = plan.removeCount - 1; i >= 0; --i)
        removeClipPreservingDownstream(plan.removeFromIndex + i);
    //   (4) 新クリップを T 開始に配置 (先行ギャップ insertLeadInSec)。
    insertClipPreservingDownstream(plan.insertIndex, clip, plan.insertLeadInSec);
    // 各プリミティブが update() / emit modified() を内包しているので追加処理は不要。
}

bool TimelineTrack::rippleDeleteTimeRange(double startSec, double endSec, bool notify)
{
    // TB-3: タイムラインの [startSec, endSec) を削除し、後続クリップを左へ詰める。
    // 既存の split/remove プリミティブだけで構成し、時間↔leadInSec モデルは
    // effectiveDuration() / leadInSec の積み上げ (insertClip3Point などと同じ規約)
    // に従う。境界は安全に no-op。
    constexpr double kEps = 1e-6;
    if (m_clips.isEmpty())
        return false;
    if (endSec - startSec <= kEps)   // 空 / 反転 / 微小範囲
        return false;
    if (startSec < 0.0) startSec = 0.0;
    if (endSec - startSec <= kEps)
        return false;

    bool changed = false;

    // (1) 範囲境界で跨ぐクリップを分割する。overwriteClip3Point と同じく、
    //     末尾境界 (endSec) を先に分割してから先頭境界 (startSec) を分割すると、
    //     先頭分割で生じる +1 インデックスズレが末尾側に波及しない。
    //     splitClipAt は clip-local 秒を取るので、各クリップのタイムライン開始時刻を
    //     leadInSec 積み上げで求め、ローカル秒へ換算する。
    auto splitAtTimelineSec = [&](double t) {
        double cursor = 0.0;
        for (int i = 0; i < m_clips.size(); ++i) {
            const double clipStart = cursor + m_clips[i].leadInSec;
            const double clipEnd = clipStart + m_clips[i].effectiveDuration();
            if (t > clipStart + kEps && t < clipEnd - kEps) {
                const int beforeCount = m_clips.size();
                splitClipAt(i, t - clipStart, notify);   // clip-local 秒
                if (m_clips.size() != beforeCount)
                    changed = true;
                return;
            }
            cursor = clipEnd;
        }
    };
    splitAtTimelineSec(endSec);
    splitAtTimelineSec(startSec);

    // (2) [startSec, endSec) に完全に収まるクリップ群を末尾側から削除する。
    //     removeClipPreservingDownstream は削除クリップの footprint を次クリップの
    //     leadInSec に押し込むため、この時点では後続の絶対位置は保たれる
    //     (= 削除区間が空ギャップとして残る)。後ろから消すのは前から消すと
    //     後続インデックスがズレるため。
    QVector<int> toRemove;
    {
        double cursor = 0.0;
        for (int i = 0; i < m_clips.size(); ++i) {
            const double clipStart = cursor + m_clips[i].leadInSec;
            const double clipEnd = clipStart + m_clips[i].effectiveDuration();
            if (clipStart >= startSec - kEps && clipEnd <= endSec + kEps)
                toRemove.append(i);
            cursor = clipEnd;
        }
    }
    if (!toRemove.isEmpty())
        changed = true;
    for (int j = toRemove.size() - 1; j >= 0; --j)
        removeClipPreservingDownstream(toRemove[j], notify);

    // (3) リップル: 削除区間ぶん後続クリップを左へ詰める。削除区間の直後に
    //     位置するクリップ (タイムライン開始が startSec 以降の最初のクリップ) の
    //     leadInSec から削除長を引く。removeClipPreservingDownstream が空きを
    //     leadInSec に積んでいるので、ここで引くと後続全体が左シフトする。
    const double deletedLen = endSec - startSec;
    double cursor = 0.0;
    for (int i = 0; i < m_clips.size(); ++i) {
        const double clipStart = cursor + m_clips[i].leadInSec;
        if (clipStart >= startSec - kEps) {
            double &lead = mutableClips()[i].leadInSec;
            const double newLead = qMax(0.0, lead - deletedLen);
            if (qAbs(newLead - lead) > kEps) {
                lead = newLead;
                changed = true;
            }
            break;
        }
        cursor = clipStart + m_clips[i].effectiveDuration();
    }

    if (!changed)
        return false;
    updateMinimumWidth(); update();
    if (notify)
        emit modified();
    return true;
}

TimelineTrack::DropPlan TimelineTrack::planDrop(double dropTime, double clipDuration,
                                                const QSet<int> *ignoreIndexes) const
{
    DropPlan plan{};
    if (dropTime < 0.0) dropTime = 0.0;
    double cursor = 0.0; // end time of the previous clip (i.e. start of the gap before clip[i])
    // US-1403: an ignored clip leaves its span to the gap; clipsEnd is the end of
    // the previous clip, ignored or not, and slot the index clip[i] has without
    // the ignored ones (both equal cursor / i when nothing is ignored).
    double clipsEnd = 0.0;
    int slot = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        const double clipStart = clipsEnd + m_clips[i].leadInSec;
        const double clipEnd = clipStart + m_clips[i].effectiveDuration();
        clipsEnd = clipEnd;
        if (ignoreIndexes && ignoreIndexes->contains(i))
            continue;
        // Does the dropped clip fit entirely within the gap before clip[i]?
        if (dropTime + clipDuration <= clipStart + 1e-6) {
            plan.insertIdx = slot;
            plan.newLeadIn = qMax(0.0, dropTime - cursor);
            plan.nextLeadInDelta = plan.newLeadIn + clipDuration;
            plan.valid = true;
            return plan;
        }
        // Overlap with clip[i] — no valid slot here.
        if (dropTime < clipEnd - 1e-6)
            return plan; // invalid
        cursor = clipEnd;
        ++slot;
    }
    // After the last clip: always valid, just appends.
    plan.insertIdx = slot;
    plan.newLeadIn = qMax(0.0, dropTime - cursor);
    plan.nextLeadInDelta = 0.0;
    plan.valid = true;
    return plan;
}

namespace {
// US-1405: set by Timeline::restoreFromProject while it loads the video tracks
// top-down, so a uid repeated on a later video track is repaired too.
QSet<QString> *g_loadVideoSeenUids = nullptr;
} // namespace

void TimelineTrack::setClips(const QVector<ClipInfo> &clips)
{
    if (m_timeline) m_timeline->noteClipsChanged();
    m_clips = clips;
    // Every clip leaving setClips has a uid; a uid repeated within the track
    // (or, while a project loads, on an earlier video track) keeps it on the
    // earlier clip and gives the later one a fresh uid.
    QSet<QString> localSeen;
    QSet<QString> &seen = g_loadVideoSeenUids ? *g_loadVideoSeenUids : localSeen;
    for (ClipInfo &clip : m_clips) {
        if (!clip.uid.isEmpty() && seen.contains(clip.uid)) {
            clip.uid.clear();
            ++m_duplicateUidRepairCount;
        }
        ensureUid(clip);
        seen.insert(clip.uid);
    }
    updateMinimumWidth(); update();
}
void TimelineTrack::setSelectedClip(int index) {
    QList<int> newSel;
    if (index >= 0) newSel.append(index);
    if (m_selectedClips == newSel) return;
    m_selectedClips = newSel;
    update();
    emit selectionChanged(index, false);
}

void TimelineTrack::toggleClipSelection(int index) {
    if (index < 0) return;
    if (m_selectedClips.contains(index)) {
        m_selectedClips.removeAll(index);
    } else {
        m_selectedClips.append(index);
    }
    update();
    const int primary = m_selectedClips.isEmpty() ? -1 : m_selectedClips.last();
    emit selectionChanged(primary, true);
}

void TimelineTrack::clearClipSelection() {
    if (m_selectedClips.isEmpty()) return;
    m_selectedClips.clear();
    update();
    emit selectionChanged(-1, false);
}

void TimelineTrack::moveSelectedClipsGroup(int targetIndex)
{
    if (m_selectedClips.size() < 2) return;
    if (targetIndex < 0 || targetIndex > m_clips.size()) return;

    QList<int> sortedSel = m_selectedClips;
    std::sort(sortedSel.begin(), sortedSel.end());

    // Bail if any source index is invalid.
    for (int idx : sortedSel)
        if (idx < 0 || idx >= m_clips.size()) return;

    // Extract moved clips in order.
    QVector<ClipInfo> movedClips;
    movedClips.reserve(sortedSel.size());
    for (int idx : sortedSel) movedClips.append(m_clips[idx]);

    // Compute insert position relative to the list with sources removed.
    int adjustedTarget = targetIndex;
    for (int idx : sortedSel)
        if (idx < targetIndex) adjustedTarget--;

    // Remove sources from highest to lowest so indices stay valid.
    for (int i = sortedSel.size() - 1; i >= 0; --i)
        mutableClips().removeAt(sortedSel[i]);

    adjustedTarget = qBound(0, adjustedTarget, m_clips.size());

    for (int i = 0; i < movedClips.size(); ++i)
        mutableClips().insert(adjustedTarget + i, movedClips[i]);

    // New selection is the consecutive range of inserted clips.
    QList<int> newSel;
    for (int i = 0; i < movedClips.size(); ++i) newSel.append(adjustedTarget + i);
    m_selectedClips = newSel;

    updateMinimumWidth(); update();
    emit selectionChanged(newSel.last(), false);
    emit clipMoved(sortedSel.first(), adjustedTarget);
    emit modified();
}

void TimelineTrack::setPixelsPerSecond(double pps)
{
    m_pixelsPerSecond = qBound(0.02, pps, 200.0);
    updateMinimumWidth();
    update();
}

int TimelineTrack::clipAtX(int x) const
{
    int cx = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        cx += qMax(0, static_cast<int>(m_clips[i].leadInSec * m_pixelsPerSecond));
        int w = qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
        if (x >= cx && x < cx + w) return i;
        cx += w;
    }
    return -1;
}

int TimelineTrack::clipStartX(int index) const
{
    int x = 0;
    for (int i = 0; i < index && i < m_clips.size(); ++i) {
        x += qMax(0, static_cast<int>(m_clips[i].leadInSec * m_pixelsPerSecond));
        x += qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
    }
    if (index >= 0 && index < m_clips.size())
        x += qMax(0, static_cast<int>(m_clips[index].leadInSec * m_pixelsPerSecond));
    return x;
}

double TimelineTrack::xToSeconds(int x) const { return static_cast<double>(x) / m_pixelsPerSecond; }
int TimelineTrack::secondsToX(double seconds) const { return static_cast<int>(seconds * m_pixelsPerSecond); }

int TimelineTrack::snapToEdge(int x) const
{
    if (!m_snapEnabled) return x;
    int cx = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        cx += qMax(0, static_cast<int>(m_clips[i].leadInSec * m_pixelsPerSecond));
        if (qAbs(x - cx) <= SNAP_THRESHOLD) return cx;
        cx += qMax(20, static_cast<int>(m_clips[i].effectiveDuration() * m_pixelsPerSecond));
        if (qAbs(x - cx) <= SNAP_THRESHOLD) return cx;
    }
    return x;
}

void TimelineTrack::updateMinimumWidth()
{
    qint64 totalWidth = 0;
    for (const auto &c : m_clips) {
        totalWidth += qMax<qint64>(0, static_cast<qint64>(c.leadInSec * m_pixelsPerSecond));
        qint64 w = qMax<qint64>(20,
            static_cast<qint64>(c.effectiveDuration() * m_pixelsPerSecond));
        totalWidth += w;
    }
    if (m_timeline && !m_isAudioTrack) {
        const int trackIndex = adjustmentLayerTrackIndex(m_timeline, this);
        if (trackIndex >= 0) {
            for (const AdjustmentLayer &layer : m_timeline->adjustmentLayers()) {
                if (layer.trackIndex != trackIndex || layer.timelineEndUs <= 0)
                    continue;
                const double endSec = static_cast<double>(layer.timelineEndUs) / 1.0e6;
                totalWidth = qMax(totalWidth,
                                  static_cast<qint64>(endSec * m_pixelsPerSecond));
            }
        }
    }
    // Qt widget width is int; cap to a sane upper bound. Large widgets break
    // backing-store allocation and painter clipping, but we need to allow
    // multi-clip sequences to span well past the original ~5h cap. Timeline's
    // ensureSequenceFitsViewport() auto-zooms-out when content exceeds the
    // viewport so the user normally never hits this hard cap.
    constexpr qint64 kMaxWidth = 2000000;
    totalWidth = qMin(totalWidth, kMaxWidth);
    // Trailing pad past the last clip's end: large enough to comfortably
    // place the playhead a few seconds past the longest clip end at typical
    // zoom levels (~12 s at default 50 pps). Old value (+100 px) was just
    // ~2 s and made post-clip scrubbing feel cramped.
    constexpr int kTrailingPadPx = 600;
    setMinimumWidth(static_cast<int>(totalWidth) + kTrailingPadPx);
}

int TimelineTrack::filmstripTileCount(int width, int thumbnailWidth)
{
    return width > 0 && thumbnailWidth > 0 ? 1 + (width - 1) / thumbnailWidth : 0;
}

int TimelineTrack::filmstripImageIndex(double sourceSeconds, double mediaDuration, int count)
{
    if (count <= 1 || !std::isfinite(sourceSeconds) || !std::isfinite(mediaDuration)
        || mediaDuration <= 0.0) return 0;
    return int(qBound(0.0, std::floor(sourceSeconds / mediaDuration * count + 0.5),
                      double(count - 1)));
}

QVector<TimelineTrack::FilmstripTile> TimelineTrack::filmstripTilesForTest(int clipIndex) const
{
    if (!m_timeline || !m_timeline->filmstripEnabled()) return {};
    return m_filmstripTiles.value(clipIndex);
}

void TimelineTrack::paintFilmstrip(QPainter &painter, int clipIndex, const QRect &clipRect,
                                  const QRect &visibleRect)
{
    ++m_filmstripBranchCount;
    auto &tiles = m_filmstripTiles[clipIndex];
    tiles.clear();
    const auto &clip = m_clips[clipIndex];
    if (m_isAudioTrack || clip.filePath.isEmpty() || !clip.sequenceRefId.isEmpty()
        || !clip.shapes.isEmpty() || !clipRect.intersects(visibleRect)) return;
    auto *cache = m_timeline->filmstripCache();
    if (!cache) return;
    // Same key, count and size as MediaPoolDock; pending/cached requests coalesce.
    // Probe the full source duration: a clip may end before the source does.
    cache->request(clip.filePath, clip.filePath, 0.0, 8);
    const auto frames = cache->frames(clip.filePath);
    if (frames.isEmpty()) return;
    const QRect body = clipRect.adjusted(0, 4, 0, -4);
    const int tileWidth = qMax(1, qRound(body.height() * 16.0 / 9.0));
    const int count = filmstripTileCount(body.width(), tileWidth);
    if (body.height() <= 0) return;
    const int first = qMax(0, (visibleRect.left() - body.left()) / tileWidth);
    const int last = qMin(count, (visibleRect.right() - body.left()) / tileWidth + 1);
    for (int i = first; i < last; ++i) {
        const QRect rect = QRect(body.left() + i * tileWidth, body.top(), tileWidth,
                                 body.height()).intersected(body);
        double fraction = double(rect.center().x() - body.left()) / body.width();
        if (clip.reversed) fraction = 1.0 - fraction;
        const double end = clip.outPoint > clip.inPoint ? clip.outPoint : clip.duration;
        const double sourceSeconds = clip.inPoint + fraction * qMax(0.0, end - clip.inPoint);
        const int index = filmstripImageIndex(sourceSeconds, cache->duration(clip.filePath),
                                              int(frames.size()));
        tiles.append({rect, index});
        const auto &image = frames[index];
        painter.drawImage(rect, image, QRectF(0, 0,
            image.width() * double(rect.width()) / tileWidth, image.height()));
        ++m_filmstripDrawCount;
    }
}

void Timeline::setSkimmingEnabled(bool enabled)
{
    if (m_skimmingEnabled == enabled) return;
    m_skimmingEnabled = enabled;
    if (!enabled) clearSkim();
    if (enabled && !m_filmstripCache) setFilmstripCache(new ThumbnailCache(this));
}

void Timeline::setSkimmingPlaybackActive(bool playing)
{
    m_skimmingPlaybackActive = playing;
    if (playing) clearSkim();
}

void Timeline::clearSkim()
{
    m_skimTrack.clear();
    if (!m_skimVisible) return;
    m_skimVisible = false;
    emit skimPreviewCleared();
}

void Timeline::hoverSkim(TimelineTrack *track, const QPoint &pos)
{
    // OFF must not even query the shared thumbnail cache.
    if (!m_skimmingEnabled) return;
    if (m_skimmingPlaybackActive || !track || track->isAudioTrack()
        || !track->rect().contains(pos)) { clearSkim(); return; }
    const int index = track->clipAtX(pos.x());
    if (index < 0) { clearSkim(); return; }
    const auto &clip = track->clips()[index];
    if (clip.filePath.isEmpty() || !clip.sequenceRefId.isEmpty()
        || !clip.shapes.isEmpty() || clipgeom::isNullObjectFilePath(clip.filePath)) {
        clearSkim();
        return;
    }
    auto *cache = filmstripCache();
    if (!cache) { clearSkim(); return; }
    m_skimTrack = track;
    m_skimPos = pos;
    cache->request(clip.filePath, clip.filePath, 0.0, 8);
    const auto frames = cache->frames(clip.filePath);
    if (frames.isEmpty()) {
        if (m_skimVisible) {
            m_skimVisible = false;
            emit skimPreviewCleared();
        }
        return;
    }
    const int width = qMax(20, static_cast<int>(clip.effectiveDuration()
                                               * track->pixelsPerSecond()));
    const int frame = ThumbnailCache::skimIndex(pos.x() - track->clipStartX(index),
                                                width, int(frames.size()));
    if (frames[frame].isNull()) { clearSkim(); return; }
    m_skimVisible = true;
    emit skimPreviewRequested(frames[frame]);
}

void TimelineTrack::leaveEvent(QEvent *event)
{
    if (m_timeline && m_timeline->skimmingEnabled()) m_timeline->clearSkim();
    QWidget::leaveEvent(event);
}

void Timeline::setFilmstripCache(ThumbnailCache *cache)
{
    if (m_filmstripCache == cache) return;
    clearSkim();
    disconnect(m_filmstripReadyConnection);
    m_filmstripCache = cache;
    if (cache) {
        m_filmstripReadyConnection = connect(cache, &ThumbnailCache::ready, this,
            [this](const QString &key, const QVector<QImage> &) {
                if (m_skimmingEnabled && m_skimTrack) {
                    const int index = m_skimTrack->clipAtX(m_skimPos.x());
                    if (index >= 0 && m_skimTrack->clips()[index].filePath == key)
                        hoverSkim(m_skimTrack.data(), m_skimPos);
                }
                if (!m_filmstripEnabled) return;
                for (auto *track : m_videoTracks)
                    for (const auto &clip : track->clips())
                        if (clip.filePath == key) { track->update(); break; }
            });
    }
    for (auto *track : m_videoTracks) track->update();
}

void Timeline::setFilmstripEnabled(bool enabled)
{
    if (m_filmstripEnabled == enabled) return;
    m_filmstripEnabled = enabled;
    if (enabled && !m_filmstripCache) setFilmstripCache(new ThumbnailCache(this));
    for (auto *track : m_videoTracks) track->update();
}

void TimelineTrack::paintEvent(QPaintEvent *event)
{
    ++m_paintCount;
    static int paintCount = 0;
    if (++paintCount <= 5) {
        qInfo() << "TimelineTrack::paintEvent #" << paintCount
                << "clips=" << m_clips.size() << "pps=" << m_pixelsPerSecond;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect visibleRect = event ? event->rect() : QRect(0, 0, width(), height());
    QVector<QPair<QRect, QColor>> labelBars;
    int x = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        // Leading gap (leadInSec) before this clip, created by left-trim so the
        // clip's right edge stays anchored. Skip drawing over it — the dark
        // track background shows through.
        x += qMax(0, static_cast<int>(m_clips[i].leadInSec * m_pixelsPerSecond));
        // Cap the drawn width so pathological long clips don't explode paint
        // (raised in lockstep with updateMinimumWidth's kMaxWidth so multi-clip
        // sequences place subsequent clips at the correct x position).
        int clipWidth = qMax(20, static_cast<int>(
            qMin<double>(2000000.0,
                         m_clips[i].effectiveDuration() * m_pixelsPerSecond)));
        QRect clipRect(x, 0, clipWidth, m_rowHeight);
        // Clip body colour reflects edit STATE, not list position: a clip
        // that's just moved must keep the same colour as before the move.
        // Plain clips are blue; clips with a color correction or any video
        // effect applied switch to green so the user can see at a glance
        // which clips are processed. The previous index-parity colouring
        // (i % 2) made every move flip the colour, which read as "the clip
        // mutated" even though only its position changed.
        const bool hasEffects = !m_clips[i].colorCorrection.isDefault()
                             || !m_clips[i].effects.isEmpty();
        QColor color = hasEffects ? QColor(0x44, 0xAA, 0x88)
                                  : QColor(0x44, 0x88, 0xCC);
        if (m_clips[i].label != ClipLabel::None)
            color = clipLabelColor(m_clips[i].label);
        const bool isSelected = m_selectedClips.contains(i);
        if (isSelected) color = color.lighter(140);
        if (m_dragMode == DragMode::MoveClip && i == m_dropTargetIndex) {
            painter.setPen(QPen(Qt::yellow, 3));
            painter.drawLine(x, 0, x, m_rowHeight);
        }
        painter.fillRect(clipRect, color);
        if (m_timeline && m_timeline->filmstripEnabled())
            paintFilmstrip(painter, i, clipRect, visibleRect);
        if (!m_clips[i].visible && !clipgeom::isNullObjectFilePath(m_clips[i].filePath)) {
            painter.fillRect(clipRect, QColor(220, 220, 220, 140));
            painter.fillRect(clipRect, QBrush(QColor(70, 70, 70, 140), Qt::BDiagPattern));
        }
        if (m_clips[i].label != ClipLabel::None) {
            labelBars.append(qMakePair(
                QRect(x, clipRect.bottom() - 2, clipWidth, 3),
                clipLabelColor(m_clips[i].label)));
        }
        // Premiere-style effect indicator: a 3 px purple bar along the top
        // edge of any clip with a non-default color correction OR any video
        // effect applied. The bar spans the full clip width so the user can
        // immediately see the affected time range.
        if (hasEffects) {
            const QRect fxBar(x, 0, clipWidth, 3);
            painter.fillRect(fxBar, QColor(170, 100, 230, 230));
        }
        // Trim handle indicators — visible on all clips so users can discover
        // the edge-drag-to-trim affordance without having to hunt for it.
        const QColor trimColor = isSelected ? QColor(255, 200, 60, 220)
                                            : QColor(255, 255, 255, 120);
        painter.fillRect(QRect(x, 0, TRIM_HANDLE_WIDTH, m_rowHeight), trimColor);
        painter.fillRect(QRect(x + clipWidth - TRIM_HANDLE_WIDTH, 0, TRIM_HANDLE_WIDTH, m_rowHeight), trimColor);
        if (isSelected) {
            painter.setPen(QPen(Qt::yellow, 2));
            painter.drawRect(clipRect.adjusted(1, 1, -1, -1));
        } else {
            painter.setPen(QPen(Qt::white, 1));
            painter.drawRect(clipRect);
        }
        // Draw waveform if available — only for the visible x range to avoid
        // drawing 100k+ lines for very long clips.
        if (!m_clips[i].waveform.isEmpty()) {
            painter.setPen(QPen(QColor(100, 200, 150, 180), 1));
            const auto &wf = m_clips[i].waveform;
            const int wfPeakCount = wf.peaks.size();
            if (wfPeakCount > 0 && clipWidth > 2) {
                const int midY = m_rowHeight / 2;
                const int maxAmp = m_rowHeight / 2 - 4;

                // Intersect with visible rect to skip drawing offscreen pixels.
                const int pxStart = qMax(0, visibleRect.left() - x);
                const int pxEnd   = qMin(clipWidth, visibleRect.right() - x + 1);

                for (int px = pxStart; px < pxEnd; ++px) {
                    // Map px (0..clipWidth-1) linearly to peaks (0..wfPeakCount-1).
                    qint64 peakIdx = static_cast<qint64>(px) * wfPeakCount / clipWidth;
                    if (peakIdx < 0 || peakIdx >= wfPeakCount) continue;
                    const float amp = wf.peaks[static_cast<int>(peakIdx)];
                    const int h = static_cast<int>(amp * maxAmp);
                    painter.drawLine(x + px, midY - h, x + px, midY + h);
                }
            }
        }

        // Label with speed indicator
        painter.setPen(Qt::white);
        QRect textRect = clipRect.adjusted(8, 4, -8, -4);
        double dur = m_clips[i].effectiveDuration();
        int mins = static_cast<int>(dur) / 60;
        int secs = static_cast<int>(dur) % 60;
        const bool offlineClip = m_timeline && !m_clips[i].filePath.isEmpty()
            && m_timeline->isPathOffline(m_clips[i].filePath);
        QString label = m_clips[i].reversed
            ? QStringLiteral("◀ ") + m_clips[i].displayName
            : m_clips[i].displayName;
        if (offlineClip)
            label += QStringLiteral(" (オフライン)");
        if (m_clips[i].speed != 1.0)
            label += QString(" [%1x]").arg(m_clips[i].speed, 0, 'f', 1);
        label += QString(" %1:%2").arg(mins, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(label, Qt::ElideRight, textRect.width()));
        if (offlineClip && clipWidth > 40) {
            // US-1408: small top-left pill, drawn like the connection badge.
            painter.save();
            QFont badgeFont = painter.font();
            badgeFont.setPixelSize(9);
            painter.setFont(badgeFont);
            const QRect badge(x + TRIM_HANDLE_WIDTH + 2, kTransBadgeYBot + 2, 42, 11);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0xb0, 0x20, 0x20, 230));
            painter.drawRoundedRect(badge, 3, 3);
            painter.setPen(QColor(0xff, 0xe0, 0xe0));
            painter.drawText(badge, Qt::AlignCenter, QStringLiteral("オフライン"));
            painter.restore();
        }

        // Transition badge. Pill at the affected corner — top-left for
        // leadIn (FadeIn), top-right for trailOut (FadeOut / CrossDissolve
        // / Wipe / Slide). Coloured per type so the user can tell them
        // apart at a glance, with abbrev + duration text. Geometry constants
        // and the duration-tracking width come from the file-scope helpers
        // so paint, hit-test, and hover all agree.
        auto transitionAbbrev = [](TransitionType type) -> QString {
            switch (type) {
                case TransitionType::None:               return "";
                case TransitionType::FadeIn:             return "Fi";
                case TransitionType::FadeOut:            return "Fo";
                case TransitionType::CrossDissolve:      return "X";
                case TransitionType::WipeLeft:           return "WL";
                case TransitionType::WipeRight:          return "WR";
                case TransitionType::WipeUp:             return "WU";
                case TransitionType::WipeDown:           return "WD";
                case TransitionType::SlideLeft:          return "SL";
                case TransitionType::SlideRight:         return "SR";
                case TransitionType::SlideUp:            return "SU";
                case TransitionType::SlideDown:          return "SD";
                case TransitionType::DipToBlack:         return "DB";
                case TransitionType::DipToWhite:         return "DW";
                case TransitionType::IrisRound:          return "IR";
                case TransitionType::IrisBox:            return "IB";
                case TransitionType::ClockWipe:          return "CW";
                case TransitionType::BarnDoorHorizontal: return "BH";
                case TransitionType::BarnDoorVertical:   return "BV";
                case TransitionType::PushLeft:           return "PL";
                case TransitionType::PushRight:          return "PR";
                case TransitionType::PushUp:             return "PU";
                case TransitionType::PushDown:           return "PD";
                case TransitionType::CrossZoom:          return "CZ";
                case TransitionType::FilmDissolve:       return "Fd";
                case TransitionType::SpinCW:             return "S+";
                case TransitionType::SpinCCW:            return "S-";
                case TransitionType::DitherDissolve:     return "Dt";
                case TransitionType::IrisRoundClose:     return "ir";
                case TransitionType::IrisBoxClose:       return "ib";
                case TransitionType::BarnDoorHClose:     return "bH";
                case TransitionType::BarnDoorVClose:     return "bV";
                case TransitionType::ClockWipeCCW:       return "cw";
                case TransitionType::WhipPanLeft:        return "<<";
                case TransitionType::WhipPanRight:       return ">>";
                case TransitionType::Glitch:             return "G!";
                case TransitionType::MorphCut:           return "MC";
                case TransitionType::LightLeak:          return "LL";
                case TransitionType::FlipHorizontal:     return "FH";
                case TransitionType::FlipVertical:       return "FV";
                case TransitionType::LensFlare:          return "Lf";
                case TransitionType::FilmBurn:           return "FB";
                case TransitionType::Pixelate:           return "Px";
                case TransitionType::BlurDissolve:       return "Bl";
                case TransitionType::CameraShake:        return "Sk";
                case TransitionType::ColorChannelShift:  return "Ch";
            }
            return "";
        };
        // Single colour for every transition type. The badge abbreviation
        // (Fi / X / WL / ...) already encodes the type, so the colour's job
        // is to mark the badge as a *transition* and keep it visually
        // distinct from the effect indicators (purple bar / green clip
        // body). Amber sits in the warm half of the wheel where neither of
        // those reside, so transitions read as their own category.
        auto transitionColor = [](TransitionType type) -> QColor {
            Q_UNUSED(type);
            return QColor(255, 200, 80);
        };
        auto paintTransitionBadge = [&](const Transition &t, int badgeX, bool handleOnRight) {
            if (t.type == TransitionType::None) return;
            const int badgeW = transBadgeWidth(t.duration, m_pixelsPerSecond, clipWidth);
            if (badgeW < 12) return;
            const QColor base = transitionColor(t.type);
            const QRect badge(badgeX, kTransBadgeYTop, badgeW, kTransBadgeH);

            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setBrush(base);
            painter.setPen(QPen(base.darker(160), 1));
            painter.drawRoundedRect(badge, 4, 4);
            painter.setRenderHint(QPainter::Antialiasing, false);

            // Drag handle indicator. Two short vertical lines on the inward
            // edge of the badge; the actual hit-test zone is 6 px wide.
            const int handleX = handleOnRight ? badge.right() - 4 : badge.left() + 1;
            painter.setPen(QPen(base.darker(220), 1));
            painter.drawLine(handleX,     badge.top() + 3, handleX,     badge.bottom() - 3);
            painter.drawLine(handleX + 2, badge.top() + 3, handleX + 2, badge.bottom() - 3);

            // Two-tier label: full ("X 1.0s") if it fits, otherwise just
            // the duration ("1.0s") so the user can always read seconds.
            // The type is still encoded in the badge color when the abbrev
            // is dropped, and hover gives a tooltip with the full name.
            painter.setPen(QPen(QColor(20, 10, 0), 1));
            painter.setFont(QFont("Arial", 8, QFont::Bold));
            const QString full = QString("%1 %2s")
                .arg(transitionAbbrev(t.type))
                .arg(t.duration, 0, 'f', 1);
            const QString compact = QString("%1s").arg(t.duration, 0, 'f', 1);
            const QString tiny = QString::number(qRound(t.duration)) + "s";
            const auto fm = painter.fontMetrics();
            const int avail = badgeW - 8;
            QString shown = (fm.horizontalAdvance(full) <= avail) ? full
                          : (fm.horizontalAdvance(compact) <= avail) ? compact
                          : (fm.horizontalAdvance(tiny) <= avail) ? tiny
                          : fm.elidedText(compact, Qt::ElideRight, avail);
            painter.drawText(badge.adjusted(4, 0, -4, 0),
                             Qt::AlignCenter, shown);
        };
        if (m_clips[i].leadIn.type != TransitionType::None) {
            // leadIn handle sits on the badge's right edge — drag right to
            // grow into the clip.
            paintTransitionBadge(m_clips[i].leadIn, x + 2, /*handleOnRight*/ true);
        }
        if (m_clips[i].trailOut.type != TransitionType::None) {
            const int badgeW = transBadgeWidth(m_clips[i].trailOut.duration,
                                               m_pixelsPerSecond, clipWidth);
            // trailOut handle sits on the badge's left edge — drag left to
            // grow toward the cut.
            paintTransitionBadge(m_clips[i].trailOut,
                                 x + clipWidth - badgeW - 2,
                                 /*handleOnRight*/ false);
        }

        // Volume-envelope (rubber band) overlay. Drawn on top of the
        // waveform so the line is always legible. Active only on audio
        // rows when the user has flipped the global edit-mode toggle.
        if (m_isAudioTrack && g_envelopeEditMode) {
            const auto &env = m_clips[i].volumeEnvelope;
            const double effDur = m_clips[i].effectiveDuration();
            if (effDur > 0.0) {
                if (env.isEmpty()) {
                    // No automation yet — show a dashed reference line at
                    // the static volume so the user can see where a click
                    // would land.
                    const double y = envelopeGainToY(m_clips[i].volume, m_rowHeight);
                    painter.setPen(QPen(QColor(255, 215, 60, 230), 2, Qt::DashLine));
                    painter.drawLine(QPointF(x, y), QPointF(x + clipWidth, y));
                } else {
                    // Polyline through the envelope points, with implicit
                    // endpoints clamped to the first / last gain so the line
                    // spans the whole clip even when only interior points
                    // are authored.
                    QVector<QPointF> pts;
                    pts.reserve(env.size() + 2);
                    pts.append(QPointF(x,
                                       envelopeGainToY(env.first().gain, m_rowHeight)));
                    for (const auto &p : env) {
                        const double frac = qBound(0.0, p.time / effDur, 1.0);
                        pts.append(QPointF(x + frac * clipWidth,
                                           envelopeGainToY(p.gain, m_rowHeight)));
                    }
                    pts.append(QPointF(x + clipWidth,
                                       envelopeGainToY(env.last().gain, m_rowHeight)));

                    painter.setPen(QPen(QColor(255, 215, 60, 230), 2));
                    for (int k = 0; k + 1 < pts.size(); ++k)
                        painter.drawLine(pts[k], pts[k + 1]);

                    // Solid white circles for each authored point. The two
                    // synthetic endpoints (indices 0 and last) are not drawn
                    // because they aren't editable.
                    painter.setBrush(QColor(255, 255, 255));
                    painter.setPen(QPen(QColor(40, 30, 0), 1));
                    for (int k = 1; k + 1 < pts.size(); ++k) {
                        painter.drawEllipse(pts[k],
                                            kEnvelopePointRadiusPx,
                                            kEnvelopePointRadiusPx);
                    }
                }
            }
        }
        // Shared link identity also covers imported AV pairs; no new project state.
        if (m_clips[i].linkGroup > 0) {
            const QColor groupColor = QColor::fromHsv((m_clips[i].linkGroup % 36) * 10, 180, 245);
            painter.fillRect(QRect(clipRect.left() + 3, clipRect.top() + 3, 10, 4), groupColor);
        }

        x += clipWidth;
    }

    paintAdjustmentLayersForTrack(painter, m_timeline, this, visibleRect,
                                  m_pixelsPerSecond, m_rowHeight);

    // Cross-track drop preview. Cyan bar = valid drop, red bar = overlap.
    if (m_dropIndicatorX >= 0) {
        const QColor color = m_dropIndicatorValid
            ? QColor(100, 220, 255, 200)
            : QColor(255, 80, 80, 200);
        painter.setPen(QPen(color, 3));
        painter.drawLine(m_dropIndicatorX, 0, m_dropIndicatorX, m_rowHeight);
        if (!m_dropIndicatorValid) {
            // Crosshatched band so the invalid state reads at a glance even
            // on top of waveform noise.
            painter.fillRect(m_dropIndicatorX - 2, 0, 4, m_rowHeight,
                             QColor(255, 80, 80, 90));
        }
    }

    // Visual indication of hidden / muted state — overlay the whole track.
    if (m_hidden) {
        painter.fillRect(rect(), QColor(0, 0, 0, 140));
        painter.setPen(QPen(QColor(180, 180, 180), 1, Qt::DashLine));
        painter.drawLine(0, 0, width(), height());
    }
    if (m_muted) {
        painter.fillRect(rect(), QColor(120, 30, 30, 60));
    }

    // Resize handle hint at the bottom edge of the track. A faint horizontal
    // bar tells the user the row height can be dragged here.
    painter.fillRect(0, height() - 2, width(), 2, QColor(70, 70, 70));

    // Labelled clips reclaim their own bottom 3 px after the shared resize
    // affordance is painted. With no labels this loop is empty, preserving the
    // exact legacy paint sequence and pixels.
    for (const auto &labelBar : std::as_const(labelBars))
        painter.fillRect(labelBar.first, labelBar.second);

    // Snap line visual feedback — yellow vertical line fading over 200 ms.
    if (m_timeline) {
        const int sx = m_timeline->snapLineX();
        if (sx >= 0) {
            const int alpha = m_timeline->snapLineAlpha();
            painter.setPen(QPen(QColor(255, 220, 0, alpha), 1));
            painter.drawLine(sx, 0, sx, m_rowHeight);
        }
    }
}

void TimelineTrack::mousePressEvent(QMouseEvent *event)
{
    if (m_timeline && m_timeline->skimmingEnabled()) m_timeline->clearSkim();
    if (m_locked) { event->accept(); return; }
    if (event->button() == Qt::LeftButton
        && event->pos().y() >= m_rowHeight - RESIZE_HANDLE_HEIGHT) {
        m_resizingHeight = true;
        m_resizeStartY = event->globalPosition().toPoint().y();
        m_resizeStartHeight = m_rowHeight;
        setCursor(Qt::SizeVerCursor); grabMouse();
        event->accept(); return;
    }
    const int clipIndex = clipAtX(event->pos().x());
    if (event->button() == Qt::LeftButton && clipIndex < 0) {
        m_marqueeCandidate = true;
        m_marqueeActive = false;
        m_marqueeAdditive = event->modifiers().testFlag(Qt::ShiftModifier);
        m_marqueeClearOnClick = !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier));
        m_marqueeStartGlobal = event->globalPosition().toPoint();
        event->accept();
        return;
    }
    if (tryHitEnvelopeKeyframe(event, clipIndex)) return;
    const bool additive = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (event->button() == Qt::RightButton) {
        if (clipIndex >= 0)
            emit clipContextMenuRequested(clipIndex, event->globalPosition().toPoint());
        else
            emit gapContextMenuRequested(qMax(0.0, xToSeconds(event->pos().x())),
                                         event->globalPosition().toPoint());
        event->accept(); return;
    }
    QRectF clipRect;
    if (clipIndex >= 0) {
        const int cx = clipStartX(clipIndex);
        const int cw = qMax(20, static_cast<int>(
            m_clips[clipIndex].effectiveDuration() * m_pixelsPerSecond));
        clipRect = QRectF(cx, 0, cw, m_rowHeight);
        if (event->button() == Qt::LeftButton && !additive
            && tryHitTransitionDragHandle(event, clipIndex, clipRect))
            return;
    }
    if (event->button() == Qt::LeftButton && !additive)
        emit seekRequested(qMax(0.0, xToSeconds(event->pos().x())));
    if (event->button() == Qt::LeftButton && clipIndex >= 0) {
        if (additive) {
            toggleClipSelection(clipIndex); emit clipClicked(clipIndex); return;
        }
        const bool partOfMulti = m_selectedClips.size() > 1
                                  && m_selectedClips.contains(clipIndex);
        if (!partOfMulti) setSelectedClip(clipIndex);
        emit clipClicked(clipIndex);
        if (tryHitTransitionBadge(event, clipIndex)) return;
        if (tryHitTrimEdge(event, clipIndex, clipRect)) return;
        handleBodyClick(event, clipIndex);
    } else if (event->button() == Qt::LeftButton && !additive) {
        setSelectedClip(-1); emit emptyAreaClicked();
    }
}

bool TimelineTrack::tryHitEnvelopeKeyframe(QMouseEvent *ev, int clipIndex)
{
    if (!g_envelopeEditMode || !m_isAudioTrack)
        return false;
    if (ev->button() != Qt::LeftButton && ev->button() != Qt::RightButton)
        return false;
    if (clipIndex < 0 || clipIndex >= m_clips.size())
        return false;
    ClipInfo &clip = mutableClips()[clipIndex];
    const double effDur = clip.effectiveDuration();
    if (effDur <= 0.0) return false;
    const QPoint pos = ev->pos();
    const int clipStartXpx = clipStartX(clipIndex);
    const int clipWidth = qMax(20, static_cast<int>(
        qMin<double>(2000000.0, effDur * m_pixelsPerSecond)));
    const double localPx = pos.x() - clipStartXpx;
    if (localPx < 0.0 || localPx > clipWidth) return false;
    int hitPt = -1;
    for (int k = 0; k < clip.volumeEnvelope.size(); ++k) {
        const auto &pt = clip.volumeEnvelope[k];
        const double frac = qBound(0.0, pt.time / effDur, 1.0);
        const double px = frac * clipWidth;
        const double py = envelopeGainToY(pt.gain, m_rowHeight);
        const double dx = localPx - px;
        const double dy = pos.y() - py;
        if (dx * dx + dy * dy <= kEnvelopeHitRadiusPx * kEnvelopeHitRadiusPx) {
            hitPt = k;
            break;
        }
    }
    const bool altHeld = ev->modifiers() & Qt::AltModifier;
    if (ev->button() == Qt::RightButton) {
        if (hitPt >= 0) {
            clip.volumeEnvelope.remove(hitPt);
            update();
            emit modified();
            ev->accept();
            return true;
        }
        return false;
    }
    if (hitPt >= 0) {
        m_envelopeDragClipIdx = clipIndex;
        m_envelopeDragPointIdx = hitPt;
        ev->accept();
        return true;
    }
    if (altHeld) {
        const bool inBadgeBand = pos.y() >= kTransBadgeYTop
                                  && pos.y() <= kTransBadgeYBot;
        const bool clipHasTrans = clip.leadIn.type != TransitionType::None
                                   || clip.trailOut.type != TransitionType::None;
        if (!(inBadgeBand && clipHasTrans)) {
            AudioGainPoint np;
            np.time = qBound(0.0,
                (localPx / clipWidth) * effDur, effDur);
            np.gain = envelopeYToGain(pos.y(), m_rowHeight);
            int insertAt = 0;
            while (insertAt < clip.volumeEnvelope.size()
                   && clip.volumeEnvelope[insertAt].time <= np.time)
                ++insertAt;
            clip.volumeEnvelope.insert(insertAt, np);
            m_envelopeDragClipIdx = clipIndex;
            m_envelopeDragPointIdx = insertAt;
            update();
            emit modified();
            ev->accept();
            return true;
        }
    }
    return false;
}

bool TimelineTrack::tryHitTransitionDragHandle(QMouseEvent *ev, int clipIndex, const QRectF &clipRect)
{
    if (ev->button() != Qt::LeftButton) return false;
    const int cy = ev->pos().y();
    if (cy < kTransBadgeYTop || cy > kTransBadgeYBot) return false;
    if (clipIndex < 0 || clipIndex >= m_clips.size()) return false;
    const int cx = static_cast<int>(clipRect.x());
    const int cw = static_cast<int>(clipRect.width());
    const auto &clip = m_clips[clipIndex];
    if (clip.leadIn.type != TransitionType::None && clip.leadIn.duration > 0.0) {
        const int badgeX = cx + 2;
        const int badgeW = transBadgeWidth(clip.leadIn.duration, m_pixelsPerSecond, cw);
        const int handleX = badgeX + badgeW - kTransBadgeHandleW;
        if (ev->pos().x() >= handleX && ev->pos().x() <= handleX + kTransBadgeHandleW) {
            setSelectedClip(clipIndex);
            emit clipClicked(clipIndex);
            m_dragMode = DragMode::TransitionLeadInResize;
            m_dragClipIndex = clipIndex;
            m_dragStartX = ev->pos().x();
            m_dragOriginalTransitionDuration = clip.leadIn.duration;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return true;
        }
    }
    if (clip.trailOut.type != TransitionType::None && clip.trailOut.duration > 0.0) {
        const int badgeW = transBadgeWidth(clip.trailOut.duration, m_pixelsPerSecond, cw);
        const int badgeX = cx + cw - badgeW - 2;
        if (ev->pos().x() >= badgeX && ev->pos().x() <= badgeX + kTransBadgeHandleW) {
            setSelectedClip(clipIndex);
            emit clipClicked(clipIndex);
            m_dragMode = DragMode::TransitionTrailOutResize;
            m_dragClipIndex = clipIndex;
            m_dragStartX = ev->pos().x();
            m_dragOriginalTransitionDuration = clip.trailOut.duration;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return true;
        }
    }
    return false;
}

bool TimelineTrack::tryHitTransitionBadge(QMouseEvent *ev, int clipIndex)
{
    if (ev->button() != Qt::LeftButton) return false;
    if (clipIndex < 0 || clipIndex >= m_clips.size()) return false;
    const int cy = ev->pos().y();
    if (cy < kTransBadgeYTop || cy > kTransBadgeYBot) return false;
    const auto &clip = m_clips[clipIndex];
    const int cx = clipStartX(clipIndex);
    const int cw = qMax(20, static_cast<int>(
        clip.effectiveDuration() * m_pixelsPerSecond));
    const int px = ev->pos().x();
    if (clip.leadIn.type != TransitionType::None && clip.leadIn.duration > 0.0) {
        const int badgeW = transBadgeWidth(clip.leadIn.duration, m_pixelsPerSecond, cw);
        const int badgeX = cx + 2;
        // Body region of the leadIn badge (between badge start and the
        // resize handle). Swallow the click so it doesn't fall through to
        // trim-edge or body-click handling — clicking a transition badge
        // should never select / drag the clip body.
        if (px >= badgeX && px < badgeX + badgeW - kTransBadgeHandleW) {
            ev->accept();
            return true;
        }
    }
    if (clip.trailOut.type != TransitionType::None && clip.trailOut.duration > 0.0) {
        const int badgeW = transBadgeWidth(clip.trailOut.duration, m_pixelsPerSecond, cw);
        const int badgeX = cx + cw - badgeW - 2;
        if (px >= badgeX + kTransBadgeHandleW && px < badgeX + badgeW) {
            ev->accept();
            return true;
        }
    }
    return false;
}

bool TimelineTrack::tryHitTrimEdge(QMouseEvent *ev, int clipIndex, const QRectF &clipRect)
{
    if (ev->button() != Qt::LeftButton) return false;
    if (clipIndex < 0 || clipIndex >= m_clips.size()) return false;
    const int cx = static_cast<int>(clipRect.x());
    const int clipWidth = static_cast<int>(clipRect.width());
    const int localX = ev->pos().x() - cx;
    if (localX <= TRIM_HANDLE_WIDTH) {
        m_dragMode = DragMode::TrimLeft;
        m_dragClipIndex = clipIndex;
        m_dragStartX = ev->pos().x();
        m_dragOriginalValue = m_clips[clipIndex].inPoint;
        m_dragOriginalLeadIn = m_clips[clipIndex].leadInSec;
        m_trimSoloLink = ev->modifiers().testFlag(Qt::AltModifier);
        m_linkedTrimActive = false;
        m_linkedTrimRejected = false;
        if (!m_trimSoloLink && m_clips[clipIndex].linkGroup > 0)
            emit linkedTrimStarted(clipIndex, true);
        if (m_linkedTrimRejected) {
            // A locked partner makes the linked trim ineligible: swallow the
            // press without starting a trim (no edit, no undo entry).
            m_linkedTrimRejected = false;
            m_dragMode = DragMode::None;
            m_dragClipIndex = -1;
        }
        return true;
    }
    if (localX >= clipWidth - TRIM_HANDLE_WIDTH) {
        m_dragMode = DragMode::TrimRight;
        m_dragClipIndex = clipIndex;
        m_dragStartX = ev->pos().x();
        m_dragOriginalValue = m_clips[clipIndex].outPoint > 0
            ? m_clips[clipIndex].outPoint : m_clips[clipIndex].duration;
        m_trimSoloLink = ev->modifiers().testFlag(Qt::AltModifier);
        m_linkedTrimActive = false;
        m_linkedTrimRejected = false;
        if (!m_trimSoloLink && m_clips[clipIndex].linkGroup > 0)
            emit linkedTrimStarted(clipIndex, false);
        if (m_linkedTrimRejected) {
            // A locked partner makes the linked trim ineligible: swallow the
            // press without starting a trim (no edit, no undo entry).
            m_linkedTrimRejected = false;
            m_dragMode = DragMode::None;
            m_dragClipIndex = -1;
        }
        return true;
    }
    return false;
}

void TimelineTrack::handleBodyClick(QMouseEvent *ev, int clipIndex)
{
    if (ev->button() != Qt::LeftButton) return;
    if (clipIndex < 0 || clipIndex >= m_clips.size()) return;
    m_dragMode = DragMode::MoveClip;
    m_dragClipIndex = clipIndex;
    m_dragStartX = ev->pos().x();
    m_dragOriginalLeadIn = m_clips[clipIndex].leadInSec;
    m_dragOriginalClipStartX = clipStartX(clipIndex);
    m_dragOriginalLeadInNext = (clipIndex + 1 < m_clips.size())
        ? m_clips[clipIndex + 1].leadInSec : -1.0;
    m_dragRawDeltaSec = 0.0;
    m_dropTargetIndex = -1;
    // Pre-collect snap targets at drag start for O(log n) per-tick lookups.
    if (m_snapEnabled && m_timeline) {
        const qint64 playheadUs = static_cast<qint64>(
            m_timeline->playheadPosition() * 1'000'000.0);
        m_timeline->snapEngine().collectFromTimeline(
            *m_timeline, playheadUs, m_timeline->markerManager());
    }
    emit linkedDragStarted(clipIndex);
    if (m_timeline) m_timeline->updateConnectionOverlay();
}

void TimelineTrack::mouseMoveEvent(QMouseEvent *event)
{
    if (m_timeline) {
        if (event->buttons() == Qt::NoButton) m_timeline->hoverSkim(this, event->pos());
        else if (m_timeline->skimmingEnabled()) m_timeline->clearSkim();
    }
    if (m_marqueeCandidate) {
        const QPoint end = event->globalPosition().toPoint();
        if ((end - m_marqueeStartGlobal).manhattanLength() >= 4)
            m_marqueeActive = true;
        if (m_marqueeActive)
            emit marqueeDragged(m_marqueeStartGlobal, end, m_marqueeAdditive, false);
        event->accept();
        return;
    }
    if (m_resizingHeight) {
        const int globalY = event->globalPosition().toPoint().y();
        const int delta = globalY - m_resizeStartY;
        const int newH = qBound(20, m_resizeStartHeight + delta, 300);
        if (newH != m_rowHeight) {
            setRowHeight(newH);
            emit rowHeightChanged(newH);
        }
        event->accept();
        return;
    }
    if (m_envelopeDragClipIdx >= 0 && m_envelopeDragPointIdx >= 0
        && m_envelopeDragClipIdx < m_clips.size()) {
        ClipInfo &clip = mutableClips()[m_envelopeDragClipIdx];
        if (m_envelopeDragPointIdx < clip.volumeEnvelope.size()) {
            const double effDur = clip.effectiveDuration();
            if (effDur > 0.0) {
                const int clipStartXpx = clipStartX(m_envelopeDragClipIdx);
                const int clipWidth = qMax(20, static_cast<int>(
                    qMin<double>(2000000.0, effDur * m_pixelsPerSecond)));
                const double localPx = qBound(0.0,
                    static_cast<double>(event->pos().x() - clipStartXpx),
                    static_cast<double>(clipWidth));
                double newTime = qBound(0.0, (localPx / clipWidth) * effDur, effDur);
                // Clamp to neighbours: cannot cross prev/next keyframe.
                // Find the closest points on either side (excluding the one
                // being dragged) and enforce a 1 ms minimum gap.
                double prevTime = 0.0;
                double nextTime = effDur;
                for (int k = 0; k < clip.volumeEnvelope.size(); ++k) {
                    if (k == m_envelopeDragPointIdx) continue;
                    const double t = clip.volumeEnvelope[k].time;
                    if (t <= newTime && t > prevTime) prevTime = t;
                    if (t >= newTime && t < nextTime) nextTime = t;
                }
                if (prevTime > 0.0) prevTime += 0.001; // 1 ms after prev
                if (nextTime < effDur) nextTime -= 0.001; // 1 ms before next
                clip.volumeEnvelope[m_envelopeDragPointIdx].time =
                    qBound(prevTime, newTime, nextTime);
                clip.volumeEnvelope[m_envelopeDragPointIdx].gain =
                    envelopeYToGain(event->pos().y(), m_rowHeight);
                update();
            }
        }
        event->accept();
        return;
    }
    if (m_dragMode == DragMode::MoveClip && m_dragClipIndex >= 0 && m_dragClipIndex < m_clips.size()) {
        const int y = event->pos().y();
        // If the user has pulled the cursor far enough vertically out of our
        // row, promote the in-track leadInSec drag into a full Qt drag so
        // other tracks can accept it as a drop. Revert the leadInSec we
        // tentatively applied so canceling the drag leaves the clip alone.
        if (promotesCrossTrackDrag(y)) {
            // Snapshot the data we need before mutating the drag state.
            const int srcIdx = m_dragClipIndex;
            mutableClips()[srcIdx].leadInSec = m_dragOriginalLeadIn;
            if (m_dragOriginalLeadInNext >= 0.0 && srcIdx + 1 < m_clips.size())
                mutableClips()[srcIdx + 1].leadInSec = m_dragOriginalLeadInNext;
            const int grabOffsetPx = m_dragStartX - clipStartX(srcIdx);

            // Ask Timeline to roll back any linked partners it already
            // shifted — otherwise they'd be stuck at the interim positions
            // when the QDrag starts.
            emit linkedDragCancelled();

            // End the in-track drag BEFORE starting QDrag — drag->exec() runs
            // a nested event loop and we don't want a stale MoveClip state.
            m_dragMode = DragMode::None;
            m_dragClipIndex = -1;
            m_dragOriginalLeadInNext = -1.0;
            setCursor(Qt::ArrowCursor);
            updateMinimumWidth();
            update();
            if (m_timeline) m_timeline->updateConnectionOverlay();

            // Build MIME payload: "srcIdx:grabOffsetPx".
            QByteArray payload;
            payload += QByteArray::number(srcIdx);
            payload += ':';
            payload += QByteArray::number(grabOffsetPx);
            auto *mime = new QMimeData();
            mime->setData("application/x-vse-clip", payload);

            auto *drag = new QDrag(this);
            drag->setMimeData(mime);
            // Ghost pixmap so the user sees what they're dragging.
            const int ghostW = qMax(20, static_cast<int>(
                m_clips[srcIdx].effectiveDuration() * m_pixelsPerSecond));
            QPixmap ghost(ghostW, m_rowHeight);
            ghost.fill(QColor(68, 136, 204, 180));
            QPainter gp(&ghost);
            gp.setPen(QPen(QColor(255, 255, 255, 220), 1));
            gp.drawRect(0, 0, ghostW - 1, m_rowHeight - 1);
            gp.drawText(ghost.rect().adjusted(4, 0, -4, 0),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        m_clips[srcIdx].displayName);
            gp.end();
            drag->setPixmap(ghost);
            drag->setHotSpot(QPoint(grabOffsetPx, m_rowHeight / 2));

            drag->exec(Qt::MoveAction);
            return;
        }

        // Positional drag: shift leadInSec + compensate the next clip so
        // downstream clips don't slide. Linked clips take a different path
        // — Timeline applies a globally-clamped delta to every partner
        // (including this one) so the V/A pair stays locked together even
        // when each track has different clamp room.
        const int dx = event->pos().x() - m_dragStartX;
        int adjustedDx = dx;

        // Snap: adjust delta so the clip's left edge magnetises to the
        // nearest target (other clip edges, playhead, t=0, markers).
        if (m_snapEnabled && m_timeline) {
            const int candidateX = m_dragOriginalClipStartX + dx;
            const double candidateSec = static_cast<double>(candidateX) / m_pixelsPerSecond;
            const qint64 candidateUs = static_cast<qint64>(candidateSec * 1e6);
            const qint64 toleranceUs = static_cast<qint64>(
                SNAP_THRESHOLD / m_pixelsPerSecond * 1e6);
            const auto match = m_timeline->snapEngine().findMatch(candidateUs, toleranceUs);
            if (match.has_value()) {
                const double snappedSec = static_cast<double>(match->timeUs) / 1e6;
                const int snappedX = static_cast<int>(snappedSec * m_pixelsPerSecond);
                adjustedDx = snappedX - m_dragOriginalClipStartX;
                m_timeline->triggerSnapLine(snappedX);
            }
        }

        const double rawDeltaSec = static_cast<double>(adjustedDx) / m_pixelsPerSecond;
        m_dragRawDeltaSec = rawDeltaSec;
        const int linkGroup = m_clips[m_dragClipIndex].linkGroup;
        if (linkGroup > 0) {
            emit linkedDragDelta(m_dragClipIndex, rawDeltaSec);
            setCursor(Qt::ClosedHandCursor);
            if (m_timeline) m_timeline->updateConnectionOverlay();
            return;
        }

        double deltaSec = rawDeltaSec;
        if (deltaSec < -m_dragOriginalLeadIn)
            deltaSec = -m_dragOriginalLeadIn;
        if (m_dragOriginalLeadInNext >= 0.0 && deltaSec > m_dragOriginalLeadInNext)
            deltaSec = m_dragOriginalLeadInNext;
        mutableClips()[m_dragClipIndex].leadInSec = m_dragOriginalLeadIn + deltaSec;
        if (m_dragOriginalLeadInNext >= 0.0
            && m_dragClipIndex + 1 < m_clips.size()) {
            mutableClips()[m_dragClipIndex + 1].leadInSec =
                m_dragOriginalLeadInNext - deltaSec;
        }
        setCursor(Qt::ClosedHandCursor);
        updateMinimumWidth();
        update();
        if (m_timeline) m_timeline->updateConnectionOverlay();
        return;
    }
    if (m_dragMode == DragMode::TransitionLeadInResize
        || m_dragMode == DragMode::TransitionTrailOutResize) {
        if (m_dragClipIndex < 0 || m_dragClipIndex >= m_clips.size()) {
            m_dragMode = DragMode::None;
            m_dragClipIndex = -1;
            setCursor(Qt::ArrowCursor);
            return;
        }
        ClipInfo &clip = mutableClips()[m_dragClipIndex];
        const double maxDur = qMax(0.1, clip.effectiveDuration());
        const int dx = event->pos().x() - m_dragStartX;
        const double deltaSec = static_cast<double>(dx) / m_pixelsPerSecond;
        if (m_dragMode == DragMode::TransitionLeadInResize) {
            // Drag right grows leadIn, drag left shrinks it.
            clip.leadIn.duration =
                qBound(0.1, m_dragOriginalTransitionDuration + deltaSec, maxDur);
        } else {
            // trailOut handle is on the badge's left edge — drag left grows
            // it (deltaSec negative), drag right shrinks it.
            clip.trailOut.duration =
                qBound(0.1, m_dragOriginalTransitionDuration - deltaSec, maxDur);
        }
        setCursor(Qt::SizeHorCursor);
        update();
        return;
    }
    if (m_dragMode == DragMode::TrimLeft || m_dragMode == DragMode::TrimRight) {
        if (m_dragClipIndex < 0 || m_dragClipIndex >= m_clips.size()) {
            if (m_linkedTrimActive) {
                m_linkedTrimActive = false;
                emit linkedTrimCancelled();
            }
            m_dragMode = DragMode::None;
            m_dragClipIndex = -1;
            setCursor(Qt::ArrowCursor);
            return;
        }
        int snappedX = snapToEdge(event->pos().x());
        int dx = snappedX - m_dragStartX;
        double deltaSec = static_cast<double>(dx) / m_pixelsPerSecond;
        if (m_linkedTrimActive) {
            // Linked trim: [lo, hi] already intersects every member's room,
            // so Timeline applies this one delta to all of them unclamped.
            const double clamped = qBound(m_linkedTrimLo, deltaSec, m_linkedTrimHi);
            emit linkedTrimDelta(m_dragClipIndex, m_dragMode == DragMode::TrimLeft, clamped);
            updateMinimumWidth(); update(); return;
        }
        ClipInfo &clip = mutableClips()[m_dragClipIndex];
        // deltaSec is a TIMELINE delta; inPoint/outPoint live in SOURCE time.
        // For a speed-scaled clip the source moves speed× as fast as the
        // timeline, so convert timeline→source with *speed (and back with
        // /speed for leadInSec, which is a timeline quantity).
        const double speed = qMax(0.001, clip.speed);
        if (m_dragMode == DragMode::TrimLeft) {
            double newIn = qMax(0.0, m_dragOriginalValue + deltaSec * speed);
            double maxIn = (clip.outPoint > 0 ? clip.outPoint : clip.duration) - 0.1;
            clip.inPoint = qMin(newIn, maxIn);
            // Keep the clip's right edge anchored by shifting its leadInSec by
            // the same timeline amount the inPoint changed. inPoint+ → leadInSec+
            // (clip slides right), inPoint− → leadInSec− (clip slides left,
            // clamped at 0 so it can't cross its neighbor).
            // US-1305: in storyline mode V1 keeps the clip's start instead (as
            // RippleIn does), so the length change ripples the clips after it.
            const double timelineDelta = (clip.inPoint - m_dragOriginalValue) / speed;
            const bool storylineV1 = m_timeline && m_timeline->storylineMode()
                && m_timeline->videoTracks().value(0, nullptr) == this;
            clip.leadInSec = storylineV1 ? m_dragOriginalLeadIn
                                         : qMax(0.0, m_dragOriginalLeadIn + timelineDelta);
        } else {
            double newOut = qMin(clip.duration, m_dragOriginalValue + deltaSec * speed);
            clip.outPoint = qMax(newOut, clip.inPoint + 0.1);
        }
        updateMinimumWidth(); update(); return;
    }
    // Hover near the bottom edge → vertical resize cursor hint.
    if (event->pos().y() >= m_rowHeight - RESIZE_HANDLE_HEIGHT) {
        setCursor(Qt::SizeVerCursor);
        return;
    }

    int hover = clipAtX(event->pos().x());
    // Transition badge hover takes priority — only fires inside the badge
    // Y-band so ordinary clip-body hover still routes to OpenHand / SizeHor.
    // Two zones: handle (6 px inward edge) → SizeHorCursor; badge body →
    // tooltip with full transition name + duration so the user can read
    // them even when the badge is too narrow to display the label.
    if (hover >= 0) {
        const int cy = event->pos().y();
        if (cy >= kTransBadgeYTop && cy <= kTransBadgeYBot) {
            const int cx = clipStartX(hover);
            const int cw = qMax(20, static_cast<int>(
                m_clips[hover].effectiveDuration() * m_pixelsPerSecond));
            const auto &c = m_clips[hover];
            const int px = event->pos().x();
            auto tipFor = [](const Transition &t) {
                return QString("%1 — %2 s")
                    .arg(Transition::typeName(t.type))
                    .arg(t.duration, 0, 'f', 2);
            };
            if (c.leadIn.type != TransitionType::None && c.leadIn.duration > 0.0) {
                const int badgeX = cx + 2;
                const int badgeW = transBadgeWidth(c.leadIn.duration,
                                                   m_pixelsPerSecond, cw);
                const int handleX = badgeX + badgeW - kTransBadgeHandleW;
                if (px >= handleX && px <= handleX + kTransBadgeHandleW) {
                    setCursor(Qt::SizeHorCursor);
                    QToolTip::showText(event->globalPosition().toPoint(),
                                       tipFor(c.leadIn), this);
                    return;
                }
                if (px >= badgeX && px < badgeX + badgeW) {
                    QToolTip::showText(event->globalPosition().toPoint(),
                                       tipFor(c.leadIn), this);
                }
            }
            if (c.trailOut.type != TransitionType::None && c.trailOut.duration > 0.0) {
                const int badgeW = transBadgeWidth(c.trailOut.duration,
                                                   m_pixelsPerSecond, cw);
                const int badgeX = cx + cw - badgeW - 2;
                if (px >= badgeX && px <= badgeX + kTransBadgeHandleW) {
                    setCursor(Qt::SizeHorCursor);
                    QToolTip::showText(event->globalPosition().toPoint(),
                                       tipFor(c.trailOut), this);
                    return;
                }
                if (px >= badgeX && px < badgeX + badgeW) {
                    QToolTip::showText(event->globalPosition().toPoint(),
                                       tipFor(c.trailOut), this);
                }
            }
        }
    }
    if (hover >= 0 && m_selectedClips.contains(hover)) {
        int cx = clipStartX(hover);
        int cw = qMax(20, static_cast<int>(m_clips[hover].effectiveDuration() * m_pixelsPerSecond));
        int lx = event->pos().x() - cx;
        setCursor((lx <= TRIM_HANDLE_WIDTH || lx >= cw - TRIM_HANDLE_WIDTH) ? Qt::SizeHorCursor : Qt::OpenHandCursor);
    } else { setCursor(Qt::ArrowCursor); }
}

void TimelineTrack::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_marqueeCandidate && event && event->button() == Qt::LeftButton) {
        m_marqueeCandidate = false;
        if (m_marqueeActive) {
            m_marqueeActive = false;
            emit marqueeDragged(m_marqueeStartGlobal, event->globalPosition().toPoint(),
                                m_marqueeAdditive, true);
        } else if (m_marqueeClearOnClick) {
            setSelectedClip(-1);
            emit emptyAreaClicked();
            emit seekRequested(qMax(0.0, xToSeconds(mapFromGlobal(m_marqueeStartGlobal).x())));
        }
        event->accept();
        return;
    }
    if (m_resizingHeight) {
        m_resizingHeight = false;
        releaseMouse();
        setCursor(Qt::ArrowCursor);
        emit rowHeightChanged(m_rowHeight);
        if (event) event->accept();
        return;
    }
    if (m_envelopeDragClipIdx >= 0 && m_envelopeDragPointIdx >= 0
        && m_envelopeDragClipIdx < m_clips.size()) {
        ClipInfo &clip = mutableClips()[m_envelopeDragClipIdx];
        if (m_envelopeDragPointIdx < clip.volumeEnvelope.size()) {
            // The dragged point may have crossed neighbours during the move
            // — re-sort so the AudioMixer evaluator's monotonic-time
            // assumption holds.
            std::sort(clip.volumeEnvelope.begin(), clip.volumeEnvelope.end(),
                      [](const AudioGainPoint &a, const AudioGainPoint &b) {
                          return a.time < b.time;
                      });
        }
        m_envelopeDragClipIdx = -1;
        m_envelopeDragPointIdx = -1;
        update();
        emit modified();
        emit interactionCompleted(QStringLiteral("Move volume keyframe"));
        if (event) event->accept();
        return;
    }
    if (m_dragMode == DragMode::MoveClip
        && m_dragClipIndex >= 0 && m_dragClipIndex < m_clips.size()) {
        if (m_timeline && m_timeline->storylineMode()
            && m_timeline->videoTracks().value(0, nullptr) == this) {
            // US-1203: in storyline mode a V1 move is a reorder: back to the
            // pre-drag positions (linked partners too), then take the clip out
            // and insert it where it was dropped; V1 stays gap-free.
            applyDragMove(m_dragClipIndex, m_dragOriginalLeadIn, m_dragOriginalLeadInNext);
            emit linkedDragCancelled();
            if (m_timeline->storylineMoveClip(this, m_dragClipIndex, m_dragRawDeltaSec)) {
                emit modified();
                emit interactionCompleted(QStringLiteral("Move clip"));
            }
        } else if (!qFuzzyCompare(m_clips[m_dragClipIndex].leadInSec, m_dragOriginalLeadIn)) {
            // Fire modified() so Timeline saves an undo snapshot and the playback
            // schedule rebuilds with the new leadInSec gaps.
            emit modified();
            emit interactionCompleted(QStringLiteral("Move clip"));
        }
    }
    if (m_dragMode == DragMode::TrimLeft || m_dragMode == DragMode::TrimRight) {
        if (m_linkedTrimActive) {
            m_linkedTrimActive = false;
            emit linkedTrimFinished();
        }
        emit modified();
        emit interactionCompleted(QStringLiteral("Trim clip"));
    }
    if ((m_dragMode == DragMode::TransitionLeadInResize
         || m_dragMode == DragMode::TransitionTrailOutResize)
        && m_dragClipIndex >= 0 && m_dragClipIndex < m_clips.size()) {
        const ClipInfo &clip = m_clips[m_dragClipIndex];
        const double newDur = (m_dragMode == DragMode::TransitionLeadInResize)
            ? clip.leadIn.duration
            : clip.trailOut.duration;
        // Threshold guards against an unintended click-without-drag from
        // emitting modified() and pushing an empty undo entry.
        if (qAbs(newDur - m_dragOriginalTransitionDuration) > 0.01) {
            emit modified();
            emit interactionCompleted(QStringLiteral("Resize transition"));
        }
    }
    m_dragMode = DragMode::None; m_dragClipIndex = -1; m_dropTargetIndex = -1;
    m_dragOriginalLeadInNext = -1.0;
    m_dragOriginalTransitionDuration = 0.0;
    setCursor(Qt::ArrowCursor); update();
    if (m_timeline) m_timeline->updateConnectionOverlay();
}

// --- Cross-track drag & drop ---

static int parseDropPayload(const QByteArray &payload, int &grabOffsetPx)
{
    const int sep = payload.indexOf(':');
    if (sep <= 0) return -1;
    bool okIdx = false, okOff = false;
    const int srcIdx = payload.left(sep).toInt(&okIdx);
    grabOffsetPx = payload.mid(sep + 1).toInt(&okOff);
    if (!okIdx || !okOff) return -1;
    return srcIdx;
}

static TimelineTrack *s_selftestDropSource = nullptr;

void TimelineTrack::setSelftestDropSource(TimelineTrack *src)
{
    s_selftestDropSource = src;
}

static TimelineTrack *dropSourceTrack(const QDropEvent *event)
{
    auto *src = qobject_cast<TimelineTrack *>(event->source());
    return src ? src : s_selftestDropSource;
}

// US-1202: a storyline drop handles exactly one local, non-project file. Anything
// else is refused so the drag falls through to MainWindow::dropEvent as in HEAD.
static bool isSingleLocalMediaDrop(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls()) return false;
    const QList<QUrl> urls = mime->urls();
    if (urls.size() != 1 || urls.first().toLocalFile().isEmpty()) return false;
    return QFileInfo(urls.first().toLocalFile()).suffix().compare(
               QStringLiteral("veditor"), Qt::CaseInsensitive) != 0;
}

bool TimelineTrack::acceptsMediaDrop() const
{
    // US-1304: V1 takes a media drop too, as a storyline insert at the drop point.
    return m_timeline && m_timeline->storylineMode() && !m_locked;
}

bool TimelineTrack::moveDragInfo(int *clipIndex, double *deltaSec) const
{
    if (m_dragMode != DragMode::MoveClip || m_dragClipIndex < 0
        || m_dragClipIndex >= m_clips.size())
        return false;
    if (clipIndex) *clipIndex = m_dragClipIndex;
    if (deltaSec) *deltaSec = m_clips[m_dragClipIndex].leadInSec - m_dragOriginalLeadIn;
    return true;
}

void TimelineTrack::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat("application/x-vse-clip")
        || event->mimeData()->hasFormat("application/x-vse-effect-library")
        || (event->mimeData()->hasUrls() && acceptsMediaDrop()
            && isSingleLocalMediaDrop(event->mimeData())))
        event->acceptProposedAction();
    else
        event->ignore();
}

void TimelineTrack::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        if (acceptsMediaDrop() && isSingleLocalMediaDrop(event->mimeData()))
            event->acceptProposedAction();
        else
            event->ignore();
        return;
    }
    if (event->mimeData()->hasFormat("application/x-vse-effect-library")) {
        if (m_locked) {
            event->ignore();
            return;
        }
        event->acceptProposedAction();
        return;
    }
    if (!event->mimeData()->hasFormat("application/x-vse-clip")) {
        event->ignore();
        return;
    }
    TimelineTrack *src = dropSourceTrack(event);
    if (!src || src == this) {
        // Same-track drags are handled by the leadInSec path — nothing to do here.
        event->ignore();
        m_dropIndicatorX = -1;
        update();
        return;
    }
    int grabOffsetPx = 0;
    const int srcIdx = parseDropPayload(
        event->mimeData()->data("application/x-vse-clip"), grabOffsetPx);
    if (srcIdx < 0 || srcIdx >= src->m_clips.size()) {
        event->ignore();
        return;
    }
    const int dropX = event->position().toPoint().x() - grabOffsetPx;
    const double dropTime = qMax(0.0, dropX / m_pixelsPerSecond);
    // US-1203: in storyline mode acceptClipDrop inserts a drop on V1, so the
    // indicator shows a valid drop at the insert point.
    if (m_timeline && m_timeline->storylineMode() && m_timeline->videoTracks().value(0, nullptr) == this) {
        const int insertIdx = threepoint::insertIndexForTime(m_clips, dropTime, nullptr);
        double insertSec = 0.0;
        for (int i = 0; i < insertIdx; ++i)
            insertSec += m_clips[i].leadInSec + m_clips[i].effectiveDuration();
        m_dropIndicatorX = qMax(0, static_cast<int>(insertSec * m_pixelsPerSecond));
        m_dropIndicatorValid = true;
        event->acceptProposedAction();
        update();
        return;
    }
    m_dropIndicatorX = qMax(0, dropX);
    m_dropIndicatorValid = dropEligible(src->m_clips[srcIdx], dropTime, src, false);
    event->acceptProposedAction();
    update();
}

void TimelineTrack::dragLeaveEvent(QDragLeaveEvent *)
{
    if (m_dropIndicatorX != -1) {
        m_dropIndicatorX = -1;
        m_dropIndicatorValid = false;
        update();
    }
}

void TimelineTrack::dropEvent(QDropEvent *event)
{
    // US-1202: with storyline mode on a media file dropped on V2+ / A is
    // imported at the drop time and connected to the V1 clip under it. Otherwise
    // the event is ignored and MainWindow::dropEvent handles it as before.
    if (event->mimeData()->hasUrls()) {
        if (!acceptsMediaDrop() || !isSingleLocalMediaDrop(event->mimeData())) {
            event->ignore();
            return;
        }
        const double dropTimeSec = qMax(0.0, event->position().toPoint().x() / m_pixelsPerSecond);
        QString err;
        const bool placed = m_timeline->dropMediaOnTrack(
            this, event->mimeData()->urls().first().toLocalFile(), dropTimeSec, &err);
        if (placed) {
            event->acceptProposedAction();
        } else {
            event->ignore();
            if (!err.isEmpty()) emit m_timeline->statusMessageRequested(err, 4000);
        }
        return;
    }
    if (m_locked) {
        event->ignore();
        return;
    }
    const int oldIndicator = m_dropIndicatorX;
    m_dropIndicatorX = -1;
    m_dropIndicatorValid = false;
    if (oldIndicator >= 0) update();

    if (event->mimeData()->hasFormat("application/x-vse-effect-library")) {
        const int clipIndex = clipAtX(event->position().toPoint().x());
        const QString entryId = QString::fromUtf8(
            event->mimeData()->data("application/x-vse-effect-library"));
        if (entryId.isEmpty()) {
            event->ignore();
            return;
        }
        emit effectDropped(entryId, clipIndex);
        event->acceptProposedAction();
        return;
    }

    if (acceptClipDrop(event->mimeData(), event->position().toPoint(), dropSourceTrack(event)))
        event->acceptProposedAction();
    else
        event->ignore();
}

bool TimelineTrack::dropEligible(const ClipInfo &clip, double dropTime, TimelineTrack *src,
                                 bool toNewTrack, QSet<int> *groupIndexesOut) const
{
    if (groupIndexesOut) groupIndexesOut->clear();
    const double clipDur = clip.effectiveDuration();
    if (planDrop(dropTime, clipDur).valid)
        return true;
    // US-1403: a slot held only by members of the dropped clip's own link group.
    if (clip.linkGroup <= 0 || !m_timeline || toNewTrack)
        return false;
    QSet<int> groupIndexes;
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips[i].linkGroup == clip.linkGroup) groupIndexes.insert(i);
    if (groupIndexes.isEmpty() || !planDrop(dropTime, clipDur, &groupIndexes).valid)
        return false;
    const auto &tracks = m_isAudioTrack ? m_timeline->audioTracks() : m_timeline->videoTracks();
    const int destIdx = tracks.indexOf(const_cast<TimelineTrack *>(this));
    const bool intoStoryline = m_timeline->storylineMode() && m_isAudioTrack && destIdx == 0;
    // Every member sits on this track, so they share one target.
    if (destIdx < 0
        || Timeline::sameKindLinkTarget(tracks, destIdx, destIdx, tracks.indexOf(src), intoStoryline) == destIdx)
        return false;
    if (groupIndexesOut) *groupIndexesOut = groupIndexes;
    return true;
}

bool TimelineTrack::acceptClipDrop(const QMimeData *mime, const QPoint &pos,
                                   TimelineTrack *src, bool toNewTrack)
{
    if (!mime || !mime->hasFormat("application/x-vse-clip"))
        return false;
    if (!src || src == this)
        return false;
    int grabOffsetPx = 0;
    const int srcIdx = parseDropPayload(mime->data("application/x-vse-clip"), grabOffsetPx);
    if (srcIdx < 0 || srcIdx >= src->m_clips.size())
        return false;
    const ClipInfo clipData = src->m_clips[srcIdx];
    const double clipDur = clipData.effectiveDuration();
    const int dropX = pos.x() - grabOffsetPx;
    const double dropTime = qMax(0.0, dropX / m_pixelsPerSecond);
    // US-1203: in storyline mode a drop on V1 is an insert (V1 only, gap-free)
    // instead of a placement in a free slot.
    const bool storylineInsert = m_timeline && m_timeline->storylineMode() && !toNewTrack
        && m_timeline->videoTracks().value(0, nullptr) == this;
    DropPlan plan = storylineInsert ? DropPlan{} : planDrop(dropTime, clipDur);
    // US-1403: a slot held only by members of the dropped clip's own link group
    // takes the drop when those members can move with the group, by the rule
    // handleCrossTrackLinkedDrop applies to them, to another unlocked track;
    // otherwise the drop is refused with nothing changed (dropEligible).
    QVector<DetachedClip> sameGroup;
    if (!storylineInsert) {
        QSet<int> groupIndexes;
        if (!dropEligible(clipData, dropTime, src, toNewTrack, &groupIndexes))
            return false;
        if (!groupIndexes.isEmpty()) {
            for (int i = 0; i < m_clips.size(); ++i)
                if (groupIndexes.contains(i)) sameGroup.append({i, m_clips[i]});
        }
    }

    // Order matters: remove from source first (while our indices are still
    // valid — source and this track have separate m_clips, so removing from
    // source never perturbs this track's indices, but doing it first also
    // matches the "move, not copy" semantics if src == this ever slips
    // through in the future).
    // A linked drop may create partner tracks through addAudioTrack(false) /
    // addVideoTrack(false), which skip the external-state capture; take it before
    // the carrier remap so the undo snapshot keeps the live matte keys. A
    // new-track drop already took it in dropClipOnNewTrack, before the track
    // existed; a second capture here would record the added track.
    if (clipData.linkGroup > 0 && m_timeline && !toNewTrack)
        m_timeline->captureExternalTrackStateForCompoundEdit();
    // US-1403: the carriers are remapped once, by uid, from this snapshot taken
    // before any clip moves; the members leave this track first, then the slot
    // is planned again on what is left.
    TrackClipSnapshot sameGroupBefore;
    if (!sameGroup.isEmpty()) {
        sameGroupBefore = snapshotTrackClips(m_timeline);
        for (int k = sameGroup.size() - 1; k >= 0; --k)
            removeClipPreservingDownstream(sameGroup[k].index);
        plan = planDrop(dropTime, clipDur);
    }
    src->removeClipPreservingDownstream(srcIdx);
    // US-1203: a clip taken out of V1 in storyline mode closes its gap.
    if (m_timeline && m_timeline->storylineMode() && m_timeline->videoTracks().value(0, nullptr) == src)
        m_timeline->closeGapsOnTrack(src);
    int insertIdx = plan.insertIdx;
    double partnerSec = dropTime;
    if (storylineInsert)
        insertIdx = m_timeline->storylineInsertClip(this, dropTime, clipData, &partnerSec);
    else
        insertClipPreservingDownstream(plan.insertIdx, clipData, plan.newLeadIn);

    // Every cross-track move changes clip keys on both tracks; Timeline remaps
    // the carriers before any linked-partner move below.
    if (sameGroup.isEmpty())
        emit clipMovedAcrossTracks(src, srcIdx, insertIdx);

    // Notify Timeline to move linked partners (V↔A) to the corresponding track.
    // Pass the absolute drop time so Timeline can place the partner at the same position
    // (a storyline insert: the inserted clip's start).
    const QString droppedUid = m_clips.value(insertIdx).uid;
    if (!sameGroup.isEmpty()) {
        emit sameGroupCrossTrackDropped(insertIdx, clipData.linkGroup, partnerSec, src, sameGroupBefore,
                                        sameGroup);
    } else if (clipData.linkGroup > 0) {
        emit crossTrackDropped(insertIdx, clipData.linkGroup, partnerSec, src);
    }
    // US-1202: with storyline mode on, a clip landing on V2+ / A connects to the
    // V1 clip under it; the drop's single undo point below carries it.
    // US-1203: an audio clip dropped onto A1 may have been realigned to its
    // picture's V1 start (or evacuated), so it is found again by uid.
    if (m_timeline && m_timeline->storylineMode()) {
        int droppedIdx = -1;
        for (int i = 0; droppedIdx < 0 && i < m_clips.size(); ++i)
            if (m_clips[i].uid == droppedUid) droppedIdx = i;
        m_timeline->autoConnectDroppedClip(this, droppedIdx);
    }

    // One drop = one undo point, taken after the partner move, track creation
    // and carrier remaps above (drag->exec in mouseMoveEvent returns without
    // an interactionCompleted of its own).
    emit interactionCompleted(toNewTrack ? QStringLiteral("クリップを新しいトラックへ移動")
                                         : QStringLiteral("クリップをトラック間移動"));
    return true;
}

bool TimelineTrack::promotesCrossTrackDrag(int y) const
{
    if (y < -CROSS_TRACK_DRAG_THRESHOLD || y >= height() + CROSS_TRACK_DRAG_THRESHOLD)
        return true;
    if (!m_timeline)
        return false;
    const auto &tracks = m_isAudioTrack ? m_timeline->audioTracks() : m_timeline->videoTracks();
    if (tracks.isEmpty() || tracks.last() != this)
        return false;
    const QWidget *band = m_timeline->findChild<QWidget *>(
        m_isAudioTrack ? QStringLiteral("newAudioTrackDropZone")
                       : QStringLiteral("newVideoTrackDropZone"));
    const QWidget *tracksWidget = parentWidget();
    if (!band || !tracksWidget || !tracksWidget->isAncestorOf(band))
        return false;
    const int cursorY = mapToParent(QPoint(0, y)).y();
    const int bandTop = band->mapTo(tracksWidget, QPoint(0, 0)).y();
    return cursorY >= bandTop && cursorY < bandTop + band->height();
}

// US-1117: thin band at the end of the video block (above the V/A separator)
// and at the end of the audio block. A timeline clip of the band's kind
// dropped here creates V(n+1) / A(n+1) and moves there; media-bin drops and a
// clip of the other kind are ignored. No shortcut. US-1407: with storyline mode
// on, a single local media file dropped here is imported on the new track.
static constexpr int kNewTrackDropZoneHeight = 10;

class TimelineNewTrackDropZone : public QWidget
{
public:
    TimelineNewTrackDropZone(Timeline *timeline, bool audio, QWidget *parent)
        : QWidget(parent), m_timeline(timeline), m_audio(audio)
    {
        setObjectName(audio ? QStringLiteral("newAudioTrackDropZone")
                            : QStringLiteral("newVideoTrackDropZone"));
        setFixedHeight(kNewTrackDropZoneHeight);
        setAcceptDrops(true);
        setToolTip(audio ? QStringLiteral("音声クリップをここへドロップすると新しい音声トラックへ移動")
                         : QStringLiteral("映像クリップをここへドロップすると新しい映像トラックへ移動"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), m_hover ? QColor(0x6a, 0xa0, 0xff) : QColor(0x2a, 0x2a, 0x2a));
    }
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!accepts(event)) {
            event->ignore();
            return;
        }
        event->acceptProposedAction();
        setHover(true);
    }
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (accepts(event))
            event->acceptProposedAction();
        else
            event->ignore();
    }
    void dragLeaveEvent(QDragLeaveEvent *) override { setHover(false); }
    void dropEvent(QDropEvent *event) override
    {
        setHover(false);
        if (event->mimeData()->hasUrls() && !event->mimeData()->hasFormat("application/x-vse-clip")) {
            QString err;
            const double dropTimeSec = m_timeline
                ? qMax(0.0, event->position().toPoint().x() / m_timeline->m_zoomLevel) : 0.0;
            if (m_timeline && isSingleLocalMediaDrop(event->mimeData())
                && m_timeline->dropMediaOnNewTrack(
                    m_audio, event->mimeData()->urls().first().toLocalFile(), dropTimeSec, &err)) {
                event->acceptProposedAction();
            } else {
                event->ignore();
                if (m_timeline && !err.isEmpty())
                    emit m_timeline->statusMessageRequested(err, 4000);
            }
            return;
        }
        if (m_timeline && m_timeline->dropClipOnNewTrack(m_audio, event->mimeData(),
                                                          event->position().toPoint(),
                                                          dropSourceTrack(event)))
            event->acceptProposedAction();
        else
            event->ignore();
    }

private:
    bool accepts(const QDropEvent *event) const
    {
        // US-1407: a single local media file, storyline mode on only.
        if (m_timeline && m_timeline->storylineMode()
            && !event->mimeData()->hasFormat("application/x-vse-clip")
            && isSingleLocalMediaDrop(event->mimeData()))
            return true;
        auto *src = qobject_cast<TimelineTrack *>(event->source());
        return event->mimeData()->hasFormat("application/x-vse-clip") && src
            && src->isAudioTrack() == m_audio;
    }
    void setHover(bool on)
    {
        if (m_hover == on) return;
        m_hover = on;
        update();
    }

    Timeline *m_timeline = nullptr;
    bool m_audio = false;
    bool m_hover = false;
};

bool Timeline::dropClipOnNewTrack(bool audio, const QMimeData *mime, const QPoint &pos,
                                  TimelineTrack *src)
{
    if (!mime || !mime->hasFormat("application/x-vse-clip") || !src
        || src->isAudioTrack() != audio)
        return false;
    int grabOffsetPx = 0;
    const int srcIdx = parseDropPayload(mime->data("application/x-vse-clip"), grabOffsetPx);
    if (srcIdx < 0 || srcIdx >= src->clipCount())
        return false;
    // addVideoTrack(false) / addAudioTrack(false) skip the external-state
    // capture; take it before the track exists so the drop's one undo point
    // (acceptClipDrop's interactionCompleted) also removes the new track.
    captureExternalTrackStateForCompoundEdit();
    if (audio)
        addAudioTrack(false);
    else
        addVideoTrack(false);
    const auto &tracks = audio ? m_audioTracks : m_videoTracks;
    if (tracks.last()->acceptClipDrop(mime, pos, src, true))
        return true;
    removeTrackInternal(audio, int(tracks.size()) - 1);
    return false;
}

// US-1202: transparent child of the tracks container that draws the connection
// lines / badges and the V1 drag follow preview (Timeline::paintConnections).
// It never takes mouse or drag events and keeps itself on top of the track rows.
class TimelineConnectionOverlay : public QWidget
{
public:
    TimelineConnectionOverlay(Timeline *timeline, QWidget *parent)
        : QWidget(parent), m_timeline(timeline)
    {
        setObjectName(QStringLiteral("timelineConnectionOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        parent->installEventFilter(this);
        setGeometry(parent->rect());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        m_timeline->paintConnections(painter);
    }
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (obj == parentWidget()) {
            if (event->type() == QEvent::Resize)
                setGeometry(parentWidget()->rect());
            else if (event->type() == QEvent::ChildAdded)
                QTimer::singleShot(0, this, [this] { raise(); });
        }
        return false;
    }

private:
    Timeline *m_timeline = nullptr;
};

// --- Timeline ---

Timeline::Timeline(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("mainTimeline"));
    m_undoManager = new UndoManager(this);
    // Coalesce sequenceChanged storm: drag/scrub events used to emit on
    // every mouse-move (~30 Hz), each driving AudioMixer::seekTo +
    // QAudioSink stop/restart synchronously on the main thread. The 50 ms
    // single-shot timer collapses bursts to one emission per quiescent
    // window without delaying user-visible feedback (≤ 1 frame at 20 fps).
    m_emitSequenceTimer = new QTimer(this);
    m_emitSequenceTimer->setSingleShot(true);
    m_emitSequenceTimer->setInterval(50);
    connect(m_emitSequenceTimer, &QTimer::timeout, this, &Timeline::emitSequenceChangedNow);
    // US-1408: undo / redo and sequence switches judge only the paths the offline
    // cache has not seen yet (no populateProjectData on these paths).
    connect(this, &Timeline::trackStateRestored, this, &Timeline::refreshOfflinePathsForUnknown);
    connect(this, &Timeline::sequencesChanged, this, &Timeline::refreshOfflinePathsForUnknown);
    setupUI();
    saveUndoState("Initial state");
}

void Timeline::scheduleEmitSequenceChanged()
{
    if (m_emitSequenceTimer)
        m_emitSequenceTimer->start();
}

void Timeline::emitSequenceChangedNow()
{
    // Direct emit — never call scheduleEmitSequenceChanged() here, that
    // would restart the debounce timer and recurse.
    undotrace::log("emitSeqNow:enter");
    emit sequenceChanged(computePlaybackSequence());
    emit audioSequenceChanged(computeAudioPlaybackSequence());
    updateConnectionOverlay();
    undotrace::log("emitSeqNow:exit");
}

void Timeline::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *infoRow = new QHBoxLayout();
    m_infoLabel = new QLabel("Timeline", this);
    m_infoLabel->setStyleSheet("font-weight: bold; color: #ccc;");
    infoRow->addWidget(m_infoLabel);

    auto *breadcrumbBar = new QWidget(this);
    breadcrumbBar->setObjectName(QString::fromLatin1(kBreadcrumbBarObjectName));
    breadcrumbBar->setVisible(false);
    breadcrumbBar->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *breadcrumbLayout = new QHBoxLayout(breadcrumbBar);
    breadcrumbLayout->setContentsMargins(4, 0, 4, 0);
    breadcrumbLayout->setSpacing(2);
    infoRow->addWidget(breadcrumbBar, 1);

    // Track row-height controls. The minus/plus buttons let the user resize
    // every track in 10 px steps (clamped to 30..200) for dense or expanded
    // multi-track layouts.
    const QString sizeBtnStyle =
        "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
        "  border-radius: 3px; font-size: 13px; padding: 0 6px; }"
        "QPushButton:hover { background-color: #555; }";
    auto *rowHLabel = new QLabel(QStringLiteral("行高"), this);
    rowHLabel->setStyleSheet("color: #999; font-size: 11px;");
    auto *rowMinus = new QPushButton(QString::fromUtf8("\xE2\x88\x92"), this); // −
    auto *rowPlus  = new QPushButton(QString::fromUtf8("\x2B"), this);          // +
    rowMinus->setFixedSize(26, 22);
    rowPlus->setFixedSize(26, 22);
    rowMinus->setStyleSheet(sizeBtnStyle);
    rowPlus->setStyleSheet(sizeBtnStyle);
    rowMinus->setToolTip(QStringLiteral("行を低く"));
    rowPlus->setToolTip(QStringLiteral("行を高く"));
    connect(rowMinus, &QPushButton::clicked, this, &Timeline::decreaseTrackHeight);
    connect(rowPlus,  &QPushButton::clicked, this, &Timeline::increaseTrackHeight);
    infoRow->addWidget(rowHLabel);
    infoRow->addWidget(rowMinus);
    infoRow->addWidget(rowPlus);

    layout->addLayout(infoRow);
    rebuildTimelineBreadcrumbBar(this);

    // Horizontal split: [frozen header column | scrollable tracks area].
    // Track headers (mute/hide buttons + label) live in the header column on
    // the left and stay put while the user scrolls the tracks horizontally.
    auto *contentArea = new QWidget(this);
    auto *contentHbox = new QHBoxLayout(contentArea);
    contentHbox->setContentsMargins(0, 0, 0, 0);
    contentHbox->setSpacing(0);

    m_headerColumn = new QWidget(contentArea);
    m_headerColumn->setFixedWidth(kHeaderColumnWidth);
    m_headerColumn->setStyleSheet("background-color: #252525;");
    m_headerLayout = new QVBoxLayout(m_headerColumn);
    m_headerLayout->setContentsMargins(0, 0, 0, 0);
    m_headerLayout->setSpacing(2);
    // Magnet (snap) toggle in the header column's top area. Fixed height
    // must equal the total of all fixed-height widgets that sit above the
    // first track row on the right side: marker-lane (16 px) + time-ruler
    // (22 px) + playhead-overlay (15 px) = 53 px. The header column's
    // QVBoxLayout spacing (2 px) fires once after this widget, so we
    // subtract 2 px so the V/A separator on the left lines up with the
    // V/A separator on the right (otherwise the header column drifts 2 px
    // below the track column at the V/A boundary).
    auto *magnetArea = new QWidget(m_headerColumn);
    m_magnetArea = magnetArea;
    // 16 = MarkerLane::kLaneHeight (private, can't reference directly)
    // 22 = TimeRuler fixed height, 15 = PlayheadOverlay fixed height
    // -2 = compensates for the 2 px QVBoxLayout spacing that fires once
    //      after this widget before the first track header row.
    magnetArea->setFixedHeight(16 + 22 + 15 - 2);
    magnetArea->setStyleSheet("background-color: #1f1f1f;");
    auto *magnetRow = new QHBoxLayout(magnetArea);
    magnetRow->setContentsMargins(6, 2, 6, 2);
    magnetRow->setSpacing(0);
    auto *magnetBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\xA7\xB2"), magnetArea); // 🧲
    magnetBtn->setCheckable(true);
    magnetBtn->setChecked(snapEnabled());
    magnetBtn->setFixedSize(32, 26);
    magnetBtn->setToolTip(QStringLiteral("マグネット / スナップ 切替"));
    magnetBtn->setStyleSheet(
        "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
        "  border-radius: 3px; font-size: 14px; padding: 0; }"
        "QPushButton:hover { background-color: #555; }"
        "QPushButton:checked { background-color: #3a7f4a; color: white;"
        "  border: 1px solid #6cbd7a; }");
    connect(magnetBtn, &QPushButton::toggled, this, [this](bool on) {
        setSnapEnabled(on);
    });
    magnetRow->addWidget(magnetBtn);
    magnetRow->addStretch();
    m_headerLayout->addWidget(magnetArea);

    m_scrollArea = new QScrollArea(contentArea);
    m_scrollArea->setObjectName(QStringLiteral("timelineScrollArea"));
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setStyleSheet("background-color: #2a2a2a;");
    // Horizontal scrollbar removed — its role overlaps with the player's seek
    // bar above. Auto-fit zoom makes content fit the viewport in most cases;
    // the scroll position is still controllable via wheel / drag.
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // AlwaysOn (not AsNeeded) so the bar stays visible while the timeline
    // pane is shrunk via the splitter — every track stays reachable.
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    auto *tracksContainer = new QWidget();
    auto *tracksOuterLayout = new QVBoxLayout(tracksContainer);
    tracksOuterLayout->setContentsMargins(0, 0, 0, 0);
    tracksOuterLayout->setSpacing(0);

    // Marker lane sits ABOVE the time ruler so colored triangles align with
    // ruler ticks and hover tooltips don't overlap track widgets. The lane
    // mirrors m_zoomLevel + scrollX so triangle positions track ruler ticks.
    m_markerLane = new MarkerLane(this, tracksContainer);
    m_markerLane->setPixelsPerSecond(m_zoomLevel);
    connect(m_markerLane, &MarkerLane::markerClicked,
            this, &Timeline::markerClicked);
    tracksOuterLayout->addWidget(m_markerLane);

    m_timeRuler = new TimeRuler(tracksContainer);
    m_timeRuler->setPixelsPerSecond(m_zoomLevel);
    connect(m_timeRuler, &TimeRuler::zoomChanged, this, &Timeline::setZoomLevel);
    connect(m_timeRuler, &TimeRuler::zoomDragStarted, this, &Timeline::captureZoomAnchor);
    connect(m_timeRuler, &TimeRuler::zoomDragEnded, this, &Timeline::clearZoomAnchor);
    tracksOuterLayout->addWidget(m_timeRuler);

    m_playheadOverlay = new PlayheadOverlay(tracksContainer);
    m_playheadOverlay->setFixedHeight(15);
    m_playheadOverlay->setStyleSheet("background-color: #222;");
    // Helper that clamps a candidate playhead content-x into the central 50%
    // of the visible viewport. Returns the bar position to draw and updates
    // m_playheadDragViewportX as a side effect (used by the auto-scroll timer).
    // Tighter than the older 70% so the timeline reveals more upcoming
    // content before the playhead leaves the comfort zone.
    auto resolvePlayheadDragX = [this](int contentX) -> int {
        int barX = contentX;
        if (m_scrollArea && m_scrollArea->viewport()) {
            QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
            if (hbar) {
                const int viewportW = m_scrollArea->viewport()->width();
                const int scrollX = hbar->value();
                const int viewportX = contentX - scrollX;
                const int leftZone = static_cast<int>(viewportW * 0.25);
                const int rightZone = static_cast<int>(viewportW * 0.75);
                if (viewportW > 0) {
                    // Only pin the bar at the 15% / 85% boundary if the
                    // timeline can actually keep scrolling in that direction.
                    // When the scrollbar is already at its rail (timeline
                    // start at min, last-clip-end + trailing pad at max),
                    // pinning would leave the start (content x = 0) and the
                    // end-of-content unreachable — the user could never drag
                    // the bar to position 0 or past the last clip.
                    if (viewportX < leftZone && scrollX > hbar->minimum())
                        barX = scrollX + leftZone;
                    else if (viewportX > rightZone && scrollX < hbar->maximum())
                        barX = scrollX + rightZone;
                }
                m_playheadDragViewportX = viewportX;
            }
        }
        return barX;
    };

    connect(m_playheadOverlay, &PlayheadOverlay::playheadMoved, this, [this, resolvePlayheadDragX](int x) {
        // First event of a press-drag cycle: place the bar exactly where the
        // user clicked (no clamp, no scroll) so a plain click on a position
        // in the outer 15% still lands the playhead there. Auto-scroll only
        // engages on subsequent moves.
        const bool firstEvent = !m_playheadDragging;
        m_playheadDragging = true;
        if (firstEvent) {
            m_playheadDragMoved = false;
            m_playheadPos = m_videoTrack->xToSeconds(x);
            if (m_playheadOverlay)
                m_playheadOverlay->setPlayheadX(x);
            if (m_scrollArea && m_scrollArea->horizontalScrollBar())
                m_playheadDragViewportX = x - m_scrollArea->horizontalScrollBar()->value();
            emit scrubPositionChanged(m_playheadPos);
            return;
        }
        m_playheadDragMoved = true;
        const int barX = resolvePlayheadDragX(x);
        m_playheadPos = m_videoTrack->xToSeconds(barX);
        if (m_playheadOverlay)
            m_playheadOverlay->setPlayheadX(barX);
        if (m_playheadAutoScrollTimer && !m_playheadAutoScrollTimer->isActive())
            m_playheadAutoScrollTimer->start();
        emit scrubPositionChanged(m_playheadPos);
    });
    connect(m_playheadOverlay, &PlayheadOverlay::playheadReleased, this, [this, resolvePlayheadDragX](int x) {
        const bool useClamp = m_playheadDragMoved;
        m_playheadDragging = false;
        m_playheadDragMoved = false;
        if (m_playheadAutoScrollTimer)
            m_playheadAutoScrollTimer->stop();
        // Pure click (no drag) lands the bar exactly where the user clicked
        // — even inside the outer 15% scroll zone — so a single click in any
        // visible area is a precise scrub. A release after an actual drag,
        // however, must keep the clamped position so the bar matches what
        // the user saw mid-drag instead of snapping to the raw cursor x.
        const int barX = useClamp ? resolvePlayheadDragX(x) : x;
        m_playheadPos = m_videoTrack->xToSeconds(barX);
        if (m_playheadOverlay)
            m_playheadOverlay->setPlayheadX(barX);
        emit positionChanged(m_playheadPos);
    });

    if (!m_playheadAutoScrollTimer) {
        m_playheadAutoScrollTimer = new QTimer(this);
        m_playheadAutoScrollTimer->setInterval(16); // ~60 Hz while drag-scrolling
        connect(m_playheadAutoScrollTimer, &QTimer::timeout,
                this, &Timeline::onPlayheadAutoScrollTick);
    }
    tracksOuterLayout->addWidget(m_playheadOverlay);

    m_tracksWidget = new QWidget();
    m_tracksLayout = new QVBoxLayout(m_tracksWidget);
    m_tracksLayout->setContentsMargins(0, 0, 0, 0);
    m_tracksLayout->setSpacing(2);
    m_connectionOverlay = new TimelineConnectionOverlay(this, m_tracksWidget);

    // Create initial V1 and A1.
    m_videoTrack = new TimelineTrack(this);
    m_audioTrack = new TimelineTrack(this);
    m_audioTrack->setIsAudioTrack(true);
    m_videoTrack->setTimeline(this);
    m_audioTrack->setTimeline(this);
    m_videoTracks.append(m_videoTrack);
    m_audioTracks.append(m_audioTrack);

    // Text strip lives inside the V group — above V1 so it's visually part
    // of the video category. Matching header spacer is added to the header
    // column at the same logical row so V1 stays aligned.
    m_textStrip = new TextStripWidget(m_tracksWidget);
    m_textStrip->setFixedHeight(m_trackHeight);
    m_textStrip->setSingleRowHeight(m_trackHeight);
    m_textStrip->setPixelsPerSecond(m_zoomLevel);
    m_textStrip->setTimeEditStartedCallback([this]() {
        beginTextOverlayTimeEdit();
    });
    m_textStrip->setTimeChangeCallback([this](int idx, double start, double end) {
        const bool changed = updateTextOverlayTime(idx, start, end, false);
        if (changed) {
            const QVector<EnhancedTextOverlay> applied = timelineTextOverlays();
            if (idx >= 0 && idx < applied.size()) {
                emit textOverlayTimeChanged(
                    idx, applied.at(idx).startTime, applied.at(idx).endTime);
            }
        }
        return changed;
    });
    m_textStrip->setTimeEditFinishedCallback([this]() {
        finishTextOverlayTimeEdit();
    });
    // Row-resize drag: resize ONLY the text row (m_textStrip + paired
    // header), independent of V/A track height, so the user can make the
    // T1 row taller without affecting the rest of the timeline. Once the
    // user drags, setTrackHeight stops overwriting this row's height.
    m_textStrip->setRowHeightChangeCallback([this](int newH) {
        if (m_textStrip)
            m_textStrip->setFixedHeight(newH);
        if (m_textStripHeader)
            m_textStripHeader->setFixedHeight(newH);
        m_textStripCustomHeight = newH;
    });
    m_textStripHeader = new QWidget(m_headerColumn);
    m_textStripHeader->setFixedHeight(m_trackHeight);
    // Belt-and-suspenders sync: whatever resize path actually changes the
    // textStrip's height (callback, layout invalidation, refreshTextStrip
    // re-entry, etc.), mirror it onto the header column's matching row so
    // the V1 lock/mute icons never drift away from the V1 clip block. The
    // explicit setFixedHeight calls in the row-height callback should keep
    // these in sync, but the resize-event filter is the empirical fallback
    // for any path that bypasses the callback.
    {
        struct HeightFollower : public QObject {
            QPointer<QWidget> follower;
            bool eventFilter(QObject *obj, QEvent *e) override {
                if (e->type() == QEvent::Resize) {
                    if (auto *src = qobject_cast<QWidget*>(obj)) {
                        if (follower && follower->height() != src->height())
                            follower->setFixedHeight(src->height());
                    }
                }
                return false;
            }
        };
        auto *follower = new HeightFollower;
        follower->follower = m_textStripHeader;
        follower->setParent(m_textStrip);
        m_textStrip->installEventFilter(follower);
    }
    m_textStripHeader->setStyleSheet(
        "background-color: #353535; color: #ddd; border-right: 1px solid #1a1a1a;");
    auto *thLayout = new QHBoxLayout(m_textStripHeader);
    thLayout->setContentsMargins(6, 0, 6, 0);
    auto *thLabel = new QLabel("T1", m_textStripHeader);
    thLabel->setStyleSheet("color: #ddd; font-size: 11px; font-weight: bold;");
    thLayout->addWidget(thLabel);
    thLayout->addStretch();
    m_headerLayout->addWidget(m_textStripHeader);
    m_tracksLayout->addWidget(m_textStrip);

    m_headerLayout->addWidget(createTrackHeader(m_videoTrack, "V1", false));
    m_tracksLayout->addWidget(m_videoTrack);

    // Boundary row between the video and audio categories: the new-video-
    // track drop band (US-1117) over a 3 px bright bar so the V/A split is
    // immediately visible. Band and bar share one row widget, mirrored in the
    // header column, so the row ordering stays aligned, A rows keep layout
    // index videoTrackCount() + 2 + i and the band stays at the end of the
    // video block whatever addVideoTrack / remapTrackIndices insert.
    auto *vaBoundary = new QWidget(m_tracksWidget);
    vaBoundary->setFixedHeight(kNewTrackDropZoneHeight + 3);
    auto *vaBoundaryLayout = new QVBoxLayout(vaBoundary);
    vaBoundaryLayout->setContentsMargins(0, 0, 0, 0);
    vaBoundaryLayout->setSpacing(0);
    vaBoundaryLayout->addWidget(new TimelineNewTrackDropZone(this, false, vaBoundary));
    m_vaSeparator = new QWidget(vaBoundary);
    m_vaSeparator->setFixedHeight(3);
    m_vaSeparator->setStyleSheet("background-color: #6aa0ff;");
    vaBoundaryLayout->addWidget(m_vaSeparator);
    m_vaBoundaryHeader = new QWidget(m_headerColumn);
    m_vaBoundaryHeader->setFixedHeight(kNewTrackDropZoneHeight + 3);
    auto *vaBoundaryHeaderLayout = new QVBoxLayout(m_vaBoundaryHeader);
    vaBoundaryHeaderLayout->setContentsMargins(0, 0, 0, 0);
    vaBoundaryHeaderLayout->setSpacing(0);
    auto *videoDropZoneSpacer = new QWidget(m_vaBoundaryHeader);
    videoDropZoneSpacer->setFixedHeight(kNewTrackDropZoneHeight);
    vaBoundaryHeaderLayout->addWidget(videoDropZoneSpacer);
    m_vaSeparatorHeader = new QWidget(m_vaBoundaryHeader);
    m_vaSeparatorHeader->setFixedHeight(3);
    m_vaSeparatorHeader->setStyleSheet("background-color: #6aa0ff;");
    vaBoundaryHeaderLayout->addWidget(m_vaSeparatorHeader);
    m_headerLayout->addWidget(m_vaBoundaryHeader);
    m_tracksLayout->addWidget(vaBoundary);

    m_headerLayout->addWidget(createTrackHeader(m_audioTrack, "A1", true));
    m_tracksLayout->addWidget(m_audioTrack);

    // New-audio-track drop band (US-1117) at the end of the audio block, just
    // before the trailing stretch (addAudioTrack inserts in front of it), with
    // a same-height spacer in the header column.
    m_newAudioTrackDropZoneHeader = new QWidget(m_headerColumn);
    m_newAudioTrackDropZoneHeader->setFixedHeight(kNewTrackDropZoneHeight);
    m_headerLayout->addWidget(m_newAudioTrackDropZoneHeader);
    m_tracksLayout->addWidget(new TimelineNewTrackDropZone(this, true, m_tracksWidget));

    m_tracksLayout->addStretch();
    m_headerLayout->addStretch();

    tracksOuterLayout->addWidget(m_tracksWidget);
    m_scrollArea->setWidget(tracksContainer);

    // Keep the marker lane's content-x offset in sync with horizontal
    // scrolling so triangles stay aligned with their underlying clips
    // while the user pans. Cheap signal — only fires on scrollbar value
    // changes, not on every paint.
    if (m_markerLane) {
        if (QScrollBar *hbar = m_scrollArea->horizontalScrollBar()) {
            connect(hbar, &QScrollBar::valueChanged,
                    m_markerLane, &MarkerLane::setScrollOffset);
        }
    }

    // Install event filters so clicks on empty areas (below tracks, outer
    // container, scroll viewport) deselect all clips like clicks on a track's
    // empty area do.
    m_tracksWidget->installEventFilter(this);
    tracksContainer->installEventFilter(this);
    m_scrollArea->viewport()->installEventFilter(this);

    // Wrap the header column in its own QScrollArea so the V/A header
    // rows scroll vertically in lockstep with the right-side track scroll.
    // Without this, scrolling the tracks down leaves the header labels
    // pinned at the top while the clip rows slide upward — the lock /
    // mute icons drift away from the clips they label.
    auto *headerScrollArea = new QScrollArea(contentArea);
    headerScrollArea->setObjectName(QStringLiteral("timelineHeaderScrollArea"));
    headerScrollArea->setWidgetResizable(true);
    headerScrollArea->setFrameShape(QFrame::NoFrame);
    headerScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    headerScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    headerScrollArea->setFixedWidth(kHeaderColumnWidth);
    headerScrollArea->setStyleSheet("background-color: #252525;");
    headerScrollArea->setWidget(m_headerColumn);

    // Sync vertical scroll: when the user scrolls the tracks side, the
    // header column must follow so V1's icons stay next to the V1 row.
    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            headerScrollArea->verticalScrollBar(), &QScrollBar::setValue);

    contentHbox->addWidget(headerScrollArea);
    contentHbox->addWidget(m_scrollArea, 1);

    layout->addWidget(contentArea);
    setStyleSheet("background-color: #333;");

    // Floor for the splitter so the preview can grow vertically; tracks that
    // overflow this height stay reachable via the always-on scroll bar above.
    setMinimumHeight(150);

    wireTrackSelection(m_videoTrack);
    wireTrackSelection(m_audioTrack);

    auto seekToTrackClick = [this](double seconds) {
        m_playheadPos = qMax(0.0, seconds);
        syncPlayheadOverlay();
        emit positionChanged(m_playheadPos);
    };
    connect(m_videoTrack, &TimelineTrack::seekRequested, this, seekToTrackClick);
    connect(m_audioTrack, &TimelineTrack::seekRequested, this, seekToTrackClick);

    syncPlayheadOverlay();
}

QWidget *Timeline::createTrackHeader(TimelineTrack *track, const QString &name, bool isAudioRow)
{
    auto *w = new QWidget();
    w->setFixedHeight(m_trackHeight); // mirrors current row height
    w->setStyleSheet("QWidget { background-color: #303030; border-right: 1px solid #444; }");

    auto *hbox = new QHBoxLayout(w);
    hbox->setContentsMargins(4, 4, 4, 4);
    hbox->setSpacing(3);

    // Edit lock button (both track types). 🔓 unlocked / 🔒 locked.
    // When locked, mousePressEvent/dropEvent on the track early-return so
    // drag/trim/split/drop edits are blocked — playback is unaffected.
    auto *lockBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x93"), w); // 🔓
    lockBtn->setObjectName(QStringLiteral("timelineTrackLockButton"));
    lockBtn->setFixedSize(28, 28);
    lockBtn->setCheckable(true);
    lockBtn->setToolTip(QStringLiteral("編集ロック"));
    lockBtn->setStyleSheet(
        "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
        "  border-radius: 3px; font-size: 14px; padding: 0; }"
        "QPushButton:hover { background-color: #555; }"
        "QPushButton:checked { background-color: #cc8; color: #222; border: 1px solid #ffd; }");

    // Audio rows get mute/solo toggles; video rows get a hide toggle.
    QPushButton *muteBtn = nullptr;
    QPushButton *soloBtn = nullptr;
    QPushButton *hideBtn = nullptr;
    if (isAudioRow) {
        muteBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x8A"), w); // 🔊
        muteBtn->setObjectName(QStringLiteral("timelineTrackMuteButton"));
        muteBtn->setFixedSize(28, 28);
        muteBtn->setCheckable(true);
        muteBtn->setToolTip(QStringLiteral("ミュート (audio)"));
        muteBtn->setStyleSheet(
            "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
            "  border-radius: 3px; font-size: 14px; padding: 0; }"
            "QPushButton:hover { background-color: #555; }"
            "QPushButton:checked { background-color: #c44; color: white; border: 1px solid #f88; }");

        soloBtn = new QPushButton(QStringLiteral("S"), w);
        soloBtn->setObjectName(QStringLiteral("timelineTrackSoloButton"));
        soloBtn->setFixedSize(28, 28);
        soloBtn->setCheckable(true);
        soloBtn->setToolTip(QStringLiteral("ソロ（音声）"));
        soloBtn->setStyleSheet(
            "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
            "  border-radius: 3px; font-size: 13px; font-weight: bold; padding: 0; }"
            "QPushButton:hover { background-color: #555; }"
            "QPushButton:checked { background-color: #ca3; color: #222; border: 1px solid #fd6; }");
    } else {
        hideBtn = new QPushButton(QString::fromUtf8("\xE2\x97\x89"), w); // ◉
        hideBtn->setObjectName(QStringLiteral("timelineTrackHideButton"));
        hideBtn->setFixedSize(28, 28);
        hideBtn->setCheckable(true);
        hideBtn->setToolTip(QStringLiteral("非表示 (hide video)"));
        hideBtn->setStyleSheet(
            "QPushButton { background-color: #444; color: #ddd; border: 1px solid #666;"
            "  border-radius: 3px; font-size: 16px; padding: 0; }"
            "QPushButton:hover { background-color: #555; }"
            "QPushButton:checked { background-color: #666; color: #888; border: 1px solid #999; }");
    }

    auto *stripe = new QWidget(w);
    stripe->setObjectName(QStringLiteral("timelineTrackColorStripe"));
    stripe->setFixedWidth(6);
    hbox->setContentsMargins(0, 4, 4, 4);
    hbox->addWidget(stripe);
    auto *label = new QLabel(name, w);
    label->setObjectName(QStringLiteral("timelineTrackName"));
    label->setProperty("defaultName", name);
    label->setStyleSheet(isAudioRow
        ? "QLabel { background: transparent; border: none; color: #44AA88; font-weight: bold; font-size: 12px; }"
        : "QLabel { background: transparent; border: none; color: #4488CC; font-weight: bold; font-size: 12px; }");

    hbox->addWidget(lockBtn);
    if (muteBtn) hbox->addWidget(muteBtn);
    if (soloBtn) hbox->addWidget(soloBtn);
    if (hideBtn) hbox->addWidget(hideBtn);
    hbox->addWidget(label, 1);

    QPointer<TimelineTrack> trackPtr(track);
    QPointer<QPushButton> lockBtnPtr(lockBtn);
    QPointer<QPushButton> muteBtnPtr(muteBtn);
    QPointer<QPushButton> soloBtnPtr(soloBtn);
    QPointer<QPushButton> hideBtnPtr(hideBtn);
    QPointer<QWidget> headerPtr(w);

    // When the user drags the bottom edge of THIS track, resize the matching
    // header on the left so the row stays aligned.
    connect(track, &TimelineTrack::rowHeightChanged, this, [headerPtr](int newH) {
        if (headerPtr) headerPtr->setFixedHeight(newH);
    });

    connect(lockBtn, &QPushButton::toggled, this, [trackPtr, lockBtnPtr](bool checked) {
        if (!trackPtr) return;
        qInfo() << "Timeline: lock toggled =" << checked << "track=" << trackPtr.data();
        trackPtr->setLocked(checked);
        if (lockBtnPtr) {
            lockBtnPtr->setText(checked
                ? QString::fromUtf8("\xF0\x9F\x94\x92")    // 🔒
                : QString::fromUtf8("\xF0\x9F\x94\x93")); // 🔓
        }
    });

    if (muteBtn) {
        connect(muteBtn, &QPushButton::toggled, this, [this, trackPtr, muteBtnPtr](bool checked) {
            if (!trackPtr) return;
            qInfo() << "Timeline: mute toggled =" << checked << "track=" << trackPtr.data();
            trackPtr->setMuted(checked);
            if (muteBtnPtr) {
                muteBtnPtr->setText(checked
                    ? QString::fromUtf8("\xF0\x9F\x94\x87")    // 🔇
                    : QString::fromUtf8("\xF0\x9F\x94\x8A")); // 🔊
            }
            // Re-emit sequences so VideoPlayer picks up the audioMuted flag.
            scheduleEmitSequenceChanged();
            scheduleEmitSequenceChanged();
        });
    }
    if (soloBtn) {
        connect(soloBtn, &QPushButton::toggled, this,
                [this, trackPtr, soloBtnPtr](bool checked) {
            if (!trackPtr)
                return;
            qInfo() << "Timeline: solo toggled =" << checked
                    << "track=" << trackPtr.data();
            const int trackIndex = m_audioTracks.indexOf(trackPtr.data());
            if (trackIndex >= 0)
                toggleSoloTrack(trackIndex);
            if (soloBtnPtr)
                soloBtnPtr->setChecked(trackPtr->isSolo());
        });
    }
    if (hideBtn) {
        connect(hideBtn, &QPushButton::toggled, this, [this, trackPtr, hideBtnPtr](bool checked) {
            if (!trackPtr) return;
            qInfo() << "Timeline: hide toggled =" << checked << "track=" << trackPtr.data();
            trackPtr->setHidden(checked);
            if (hideBtnPtr) {
                hideBtnPtr->setText(checked
                    ? QString::fromUtf8("\xE2\x8A\x98")    // ⊘
                    : QString::fromUtf8("\xE2\x97\x89")); // ◉
            }
            scheduleEmitSequenceChanged();
            scheduleEmitSequenceChanged();
        });
    }

    w->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(w, &QWidget::customContextMenuRequested, this,
            [this, w, trackPtr, isAudioRow](const QPoint &pos) {
        if (!trackPtr) return;
        const int idx = isAudioRow ? m_audioTracks.indexOf(trackPtr.data())
                                   : m_videoTracks.indexOf(trackPtr.data());
        if (idx < 0) return;
        QMenu menu(w);
        auto *rename = menu.addAction(QStringLiteral("名前を変更…"));
        QMenu *colors = menu.addMenu(QStringLiteral("色"));
        for (int i = int(ClipLabel::None); i <= int(ClipLabel::Pink); ++i) {
            const QColor color = clipLabelColor(ClipLabel(i));
            auto *action = colors->addAction(clipLabelName(ClipLabel(i)));
            action->setCheckable(true);
            action->setChecked(trackPtr->color == color);
            connect(action, &QAction::triggered, this, [this, isAudioRow, idx, color] {
                setTrackColor(isAudioRow, idx, color);
            });
        }
        menu.addSeparator();
        auto *remove = menu.addAction(QStringLiteral("トラックを削除"));
        auto *up = menu.addAction(QStringLiteral("上へ移動"));
        auto *down = menu.addAction(QStringLiteral("下へ移動"));
        const int count = isAudioRow ? audioTrackCount() : videoTrackCount();
        remove->setEnabled(count > 1 && !trackPtr->isLocked());
        up->setEnabled(idx > 0);
        down->setEnabled(idx + 1 < count);
        connect(remove, &QAction::triggered, this, [this, isAudioRow, trackPtr] {
            if (trackPtr) requestRemoveTrack(isAudioRow,
                (isAudioRow ? m_audioTracks : m_videoTracks).indexOf(trackPtr.data()));
        });
        auto move = [this, isAudioRow, trackPtr](int offset) {
            if (!trackPtr) return;
            const int index = (isAudioRow ? m_audioTracks : m_videoTracks).indexOf(trackPtr.data());
            moveTrack(isAudioRow, index, index + offset);
        };
        connect(up, &QAction::triggered, this, [move] { move(-1); });
        connect(down, &QAction::triggered, this, [move] { move(1); });
        if (menu.exec(w->mapToGlobal(pos)) == rename && trackPtr) {
            bool ok = false;
            const QString value = QInputDialog::getText(this, QStringLiteral("名前を変更"),
                QStringLiteral("トラック名（空欄で既定名）"), QLineEdit::Normal,
                trackPtr->customName, &ok);
            if (ok) setTrackCustomName(isAudioRow, idx, value);
        }
    });
    m_trackHeaders.insert(track, w);
    syncTrackHeaderFlags(track);

    return w;
}

void Timeline::syncTrackHeaderFlags(TimelineTrack *track)
{
    QWidget *header = m_trackHeaders.value(track, nullptr);
    if (!track || !header)
        return;

    if (auto *label = header->findChild<QLabel *>(QStringLiteral("timelineTrackName")))
        label->setText(track->customName.isEmpty()
            ? label->property("defaultName").toString() : track->customName);
    if (auto *stripe = header->findChild<QWidget *>(QStringLiteral("timelineTrackColorStripe"))) {
        stripe->setStyleSheet(QStringLiteral("background: %1; border: none;")
            .arg(track->color.isValid() ? track->color.name() : QStringLiteral("transparent")));
    }

    auto syncButton = [header](const QString &objectName, bool checked,
                               const QString &onText, const QString &offText) {
        auto *button = header->findChild<QPushButton *>(objectName,
                                                        Qt::FindDirectChildrenOnly);
        if (!button)
            return;
        const bool wasBlocked = button->blockSignals(true);
        button->setChecked(checked);
        button->setText(checked ? onText : offText);
        button->blockSignals(wasBlocked);
    };

    syncButton(QStringLiteral("timelineTrackLockButton"), track->isLocked(),
               QString::fromUtf8("\xF0\x9F\x94\x92"), // 🔒
               QString::fromUtf8("\xF0\x9F\x94\x93")); // 🔓
    syncButton(QStringLiteral("timelineTrackMuteButton"), track->isMuted(),
               QString::fromUtf8("\xF0\x9F\x94\x87"), // 🔇
               QString::fromUtf8("\xF0\x9F\x94\x8A")); // 🔊
    syncButton(QStringLiteral("timelineTrackSoloButton"), track->isSolo(),
               QStringLiteral("S"), QStringLiteral("S"));
    syncButton(QStringLiteral("timelineTrackHideButton"), track->isHidden(),
               QString::fromUtf8("\xE2\x8A\x98"), // ⊘
               QString::fromUtf8("\xE2\x97\x89")); // ◉
}

void Timeline::addVideoTrack(bool recordUndo)
{
    if (recordUndo) captureExternalTrackState();
    int num = m_videoTracks.size() + 1;
    auto *track = new TimelineTrack(this);
    track->setPixelsPerSecond(m_zoomLevel);
    track->setSnapEnabled(snapEnabled());
    track->setRowHeight(m_trackHeight);
    track->setTimeline(this);

    // Young number on top: V1 stays at index 1 in the tracks layout
    // (textStrip occupies index 0), V2 inserts directly after V1, V3 after
    // V2, and so on. Header column has magnetArea at 0 and textStripHeader
    // at 1, so V1 header is at 2 and V2 header belongs at 3.
    const int trackIdx = m_videoTracks.size();
    m_tracksLayout->insertWidget(trackIdx + 1, track);
    m_headerLayout->insertWidget(trackIdx + 2,
        createTrackHeader(track, QString("V%1").arg(num), false));

    m_videoTracks.append(track);
    m_trackStructureRevision = ++m_nextTrackStructureRevision;
    noteClipsChanged();
    wireTrackSelection(track);
    updateInfoLabel();
    if (recordUndo) saveUndoState(QStringLiteral("トラックを追加"));
}

bool Timeline::insertVfxFootageAtPlayhead(const ClipInfo &clip,
                                          int *trackIndex,
                                          int *clipIndex)
{
    if (trackIndex)
        *trackIndex = -1;
    if (clipIndex)
        *clipIndex = -1;
    if (clip.filePath.isEmpty() || clip.effectiveDuration() <= 0.0)
        return false;

    // A footage overlay belongs above the base V1. If the project has no
    // tracks yet, create V1 as the eventual base row and V2 as the target.
    if (m_videoTracks.isEmpty())
        addVideoTrack(false);

    const double dropTime = qMax(0.0, m_playheadPos);
    int targetTrack = -1;
    TimelineTrack::DropPlan targetPlan;
    for (int t = 1; t < m_videoTracks.size(); ++t) {
        TimelineTrack *candidate = m_videoTracks.at(t);
        if (!candidate || candidate->isLocked())
            continue;
        const TimelineTrack::DropPlan plan = candidate->planDrop(
            dropTime, clip.effectiveDuration());
        if (plan.valid) {
            targetTrack = t;
            targetPlan = plan;
            break;
        }
    }

    if (targetTrack < 0) {
        addVideoTrack(false);
        targetTrack = static_cast<int>(m_videoTracks.size()) - 1;
        TimelineTrack *candidate = m_videoTracks.value(targetTrack, nullptr);
        if (!candidate)
            return false;
        targetPlan = candidate->planDrop(dropTime, clip.effectiveDuration());
        if (!targetPlan.valid)
            return false;
    }

    TimelineTrack *target = m_videoTracks.value(targetTrack, nullptr);
    if (!target)
        return false;
    target->insertClipPreservingDownstream(targetPlan.insertIdx, clip,
                                           targetPlan.newLeadIn);
    if (trackIndex)
        *trackIndex = targetTrack;
    if (clipIndex)
        *clipIndex = targetPlan.insertIdx;
    saveUndoState(QStringLiteral("Add VFX footage"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::addAudioTrack(bool recordUndo)
{
    if (recordUndo) captureExternalTrackState();
    int num = m_audioTracks.size() + 1;
    auto *track = new TimelineTrack(this);
    track->setIsAudioTrack(true);
    track->setPixelsPerSecond(m_zoomLevel);
    track->setSnapEnabled(snapEnabled());
    track->setRowHeight(m_trackHeight);
    track->setTimeline(this);

    // Insert at the end of the audio block (just before the new-audio-track
    // drop band and the trailing stretch of each layout; the header column
    // mirrors the band with a spacer). Both layouts end with band + stretch
    // so count()-2 is the right insert position for either.
    const int trackInsertIdx = m_tracksLayout->count() - 2;
    const int headerInsertIdx = m_headerLayout->count() - 2;
    m_tracksLayout->insertWidget(trackInsertIdx, track);
    m_headerLayout->insertWidget(headerInsertIdx,
        createTrackHeader(track, QString("A%1").arg(num), true));

    m_audioTracks.append(track);
    m_trackStructureRevision = ++m_nextTrackStructureRevision;
    noteClipsChanged();
    wireTrackSelection(track);
    updateInfoLabel();
    if (recordUndo) saveUndoState(QStringLiteral("トラックを追加"));
}

bool Timeline::requestRemoveTrack(bool audio, int index)
{
    QPointer<TimelineTrack> track = trackAt(audio, index);
    if (!track || track->isLocked()
        || (audio ? audioTrackCount() : videoTrackCount()) <= 1) return false;
    const int clipCount = track->clips().size();
    if (clipCount > 0 && QMessageBox::question(this, QStringLiteral("トラックを削除"),
            QStringLiteral("%1 個のクリップも削除されます。トラックを削除しますか？").arg(clipCount),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    if (!track) return false;
    return removeTrack(audio, (audio ? m_audioTracks : m_videoTracks).indexOf(track.data()));
}

namespace {

constexpr double kConnectionEps = 1e-6;

QVector<double> clipStartSecs(const QVector<ClipInfo> &clips)
{
    QVector<double> starts;
    starts.reserve(clips.size());
    double cursor = 0.0;
    for (const ClipInfo &clip : clips) {
        cursor += clip.leadInSec;
        starts.append(cursor);
        cursor += clip.effectiveDuration();
    }
    return starts;
}

QString connectionBaselineKey(bool audio, int track, const QString &uid)
{
    return (audio ? QStringLiteral("a:") : QStringLiteral("v:"))
        + QString::number(track) + QLatin1Char(':') + uid;
}

// uid -> the clip-local spans [start, end) that ripple / overwrite ranges
// (sorted, non-overlapping) remove from each clip of `track`, a middle span
// included (its pieces get new uids, so they are recorded by the clip's uid).
QHash<QString, QVector<QPair<double, double>>> rangeRemovedSpans(
    const TimelineTrack *track, const QVector<QPair<double, double>> &ranges)
{
    QHash<QString, QVector<QPair<double, double>>> spans;
    if (!track)
        return spans;
    double cursor = 0.0;
    for (const ClipInfo &clip : track->clips()) {
        const double start = cursor + clip.leadInSec;
        const double end = start + clip.effectiveDuration();
        cursor = end;
        if (clip.uid.isEmpty())
            continue;
        for (const auto &range : ranges) {
            const double from = qMax(range.first, start);
            const double to = qMin(range.second, end);
            if (to - from > kConnectionEps)
                spans[clip.uid].append({from - start, to - start});
        }
    }
    return spans;
}

// US-1203: storyline placement takes linked audio out and puts it back.
// removeClipPreservingDownstream drops a removed clip's selection and
// insertClipPreservingDownstream now shifts selected indices at/after the insert
// point, but the selection is still kept as uids per track and reassigned
// afterwards: removeClipPreservingDownstream drops the removed clip's own
// selection and placement must not run the linked-selection sync.
using TrackSelectionUids = QHash<TimelineTrack *, QStringList>;

TrackSelectionUids selectedUidsByTrack(const QVector<TimelineTrack *> &tracks)
{
    TrackSelectionUids selected;
    for (TimelineTrack *track : tracks) {
        if (!track) continue;
        for (int index : track->selectedClips())
            if (index >= 0 && index < track->clipCount())
                selected[track].append(track->clips().at(index).uid);
    }
    return selected;
}

void reselectByUid(const TrackSelectionUids &selected)
{
    for (auto it = selected.cbegin(); it != selected.cend(); ++it) {
        TimelineTrack *track = it.key();
        QList<int> indices;
        for (const QString &uid : it.value()) {
            for (int i = 0; i < track->clipCount(); ++i) {
                if (track->clips().at(i).uid != uid || indices.contains(i)) continue;
                indices.append(i);
                break;
            }
        }
        track->restoreClipSelection(indices);
    }
}

} // namespace

bool Timeline::removeTrack(bool audio, int index, QString *err)
{
    if (err) err->clear();
    const auto &tracks = audio ? m_audioTracks : m_videoTracks;
    if (index < 0 || index >= tracks.size()) {
        if (err) *err = QStringLiteral("トラック番号が範囲外です");
        return false;
    }
    if (tracks.size() <= 1 || tracks[index]->isLocked()) {
        if (err) *err = QStringLiteral("最後のトラックまたはロック中のトラックは削除できません");
        return false;
    }
    if (!audio && index == 0 && hasConnectedClips()) {
        // US-1214: the primary storyline stays V1 while connections exist.
        const QString message = QStringLiteral("接続クリップがあるため V1 は削除できません");
        if (err) *err = message;
        emit statusMessageRequested(message, 5000);
        return false;
    }
    captureExternalTrackState();
    if (!audio && index == 0) {
        // Anchors live only on V1.
        for (const ClipInfo &clip : tracks[index]->clips())
            m_pendingDeletedUids[clip.uid].append({0.0, clip.effectiveDuration()});
    }
    removeTrackInternal(audio, index);
    saveUndoState(QStringLiteral("トラックを削除"));
    return true;
}

void Timeline::removeTrackInternal(bool audio, int index)
{
    const auto &tracks = audio ? m_audioTracks : m_videoTracks;
    QVector<int> oldToNew(tracks.size());
    for (int i = 0; i < oldToNew.size(); ++i)
        oldToNew[i] = i == index ? -1 : (i > index ? i - 1 : i);
    remapTrackIndices(audio, oldToNew);
    emit trackIndicesRemapped(audio, oldToNew);
}

bool Timeline::moveTrack(bool audio, int from, int to, QString *err)
{
    if (err) err->clear();
    const auto &tracks = audio ? m_audioTracks : m_videoTracks;
    if (from < 0 || from >= tracks.size() || to < 0 || to >= tracks.size()) {
        if (err) *err = QStringLiteral("トラック番号が範囲外です");
        return false;
    }
    if (from == to) return true;
    if (!audio && (from == 0 || to == 0) && hasConnectedClips()) {
        // US-1214: the primary storyline stays V1 while connections exist.
        const QString message = QStringLiteral("接続クリップがあるため V1 は移動できません");
        if (err) *err = message;
        emit statusMessageRequested(message, 5000);
        return false;
    }
    captureExternalTrackState();
    QVector<int> oldToNew(tracks.size());
    for (int i = 0; i < oldToNew.size(); ++i) {
        oldToNew[i] = i;
        if (i == from) oldToNew[i] = to;
        else if (from < to && i > from && i <= to) oldToNew[i] = i - 1;
        else if (from > to && i >= to && i < from) oldToNew[i] = i + 1;
    }
    remapTrackIndices(audio, oldToNew);
    emit trackIndicesRemapped(audio, oldToNew);
    saveUndoState(QStringLiteral("トラックを移動"));
    return true;
}

void Timeline::remapTrackIndices(bool audio, const QVector<int> &oldToNew)
{
    m_trackStructureRevision = ++m_nextTrackStructureRevision;
    noteClipsChanged();
    // Track identity is the widget during this operation. Clip order within
    // each widget is unchanged; clip-order survivor remapping must not run.
    auto &tracks = audio ? m_audioTracks : m_videoTracks;
    const auto before = tracks;
    int count = 0;
    for (int index : oldToNew) if (index >= 0) ++count;
    tracks = QVector<TimelineTrack *>(count, nullptr);
    m_linkedDragPartners.clear();
    m_linkedTrimPartners.clear();
    auto detach = [](QVBoxLayout *layout, QWidget *widget) {
        const int index = layout->indexOf(widget);
        if (index >= 0) delete layout->takeAt(index);
    };
    for (int i = 0; i < before.size(); ++i) {
        auto *track = before[i];
        auto *header = m_trackHeaders.value(track, nullptr);
        detach(m_tracksLayout, track);
        if (header) detach(m_headerLayout, header);
        if (oldToNew[i] < 0) {
            m_trackHeaders.remove(track);
            if (header) { header->hide(); header->deleteLater(); }
            track->hide();
            track->disconnect();
            track->deleteLater();
        } else {
            tracks[oldToNew[i]] = track;
        }
    }
    m_videoTrack = m_videoTracks.first();
    m_audioTrack = m_audioTracks.first();
    for (int i = 0; i < tracks.size(); ++i) {
        auto *track = tracks[i];
        auto *header = m_trackHeaders.value(track, nullptr);
        // Rows: text strip, V1..Vn, V/A boundary (new-video-track band +
        // separator, one row), A1..Am, new-audio-track band, stretch.
        const int row = audio ? int(m_videoTracks.size()) + 2 + i : 1 + i;
        m_tracksLayout->insertWidget(row, track);
        if (header) {
            m_headerLayout->insertWidget(row + 1, header);
            if (auto *label = header->findChild<QLabel *>(QStringLiteral("timelineTrackName")))
                label->setProperty("defaultName", QStringLiteral("%1%2")
                    .arg(audio ? QStringLiteral("A") : QStringLiteral("V")).arg(i + 1));
            syncTrackHeaderFlags(track);
        }
    }
    int &active = audio ? m_activeAudioTrackIndex : m_activeVideoTrackIndex;
    active = oldToNew.value(active, -1);
    // Selection is owned by surviving widgets and follows their new row.
    if (!audio) {
        auto remapKey = [&oldToNew](const QString &key) -> QString {
            const int colon = key.indexOf(QLatin1Char(':'));
            bool ok = false;
            const int oldTrack = key.left(colon).toInt(&ok);
            if (colon < 0 || !ok) return key;
            const int newTrack = oldToNew.value(oldTrack, -1);
            return newTrack < 0 ? QString() : QString::number(newTrack) + key.mid(colon);
        };
        const auto currentMattes = trackMatteEntries();
        QHash<QString, TimelineTrackMatteEntry> mattes;
        for (auto it = currentMattes.cbegin(); it != currentMattes.cend(); ++it) {
            const QString key = remapKey(it.key());
            auto entry = it.value();
            const QString source = remapKey(entry.matteSourceClipId);
            if (key.isEmpty() || (!entry.matteSourceClipId.isEmpty() && source.isEmpty())) continue;
            entry.matteSourceClipId = source;
            mattes.insert(key, entry);
        }
        commitTrackMatteEntries(std::move(mattes));
        const auto rotoBefore = rotoEntries();
        QHash<QString, RotoClipEntry> rotos;
        rotos.reserve(rotoBefore.size());
        for (auto it = rotoBefore.cbegin(); it != rotoBefore.cend(); ++it) {
            const QString key = remapKey(it.key());
            if (!key.isEmpty()) rotos.insert(key, it.value());
        }
        commitRotoEntries(std::move(rotos), true);
        QHash<QString, QString> parents;
        for (auto it = m_clipParentEntries.cbegin(); it != m_clipParentEntries.cend(); ++it) {
            const QString child = remapKey(it.key()), parent = remapKey(it.value());
            if (!child.isEmpty() && !parent.isEmpty()) parents.insert(child, parent);
        }
        m_clipParentEntries = parents;
    }
    // Only the active sequence is updated; inactive sequence arrays are untouched.
    syncActiveSequenceFromCurrentTracks();
    const auto *selected = m_videoTracks.value(m_activeVideoTrackIndex, nullptr);
    emit clipSelected(selected ? selected->selectedClip() : -1);
    emit clipSelectedOnTrack(m_activeVideoTrackIndex, selected ? selected->selectedClip() : -1);
    updateInfoLabel();
    refreshTextStrip();
    scheduleEmitSequenceChanged();
}

void Timeline::repaintAudioTracks()
{
    for (auto *t : m_audioTracks)
        if (t) t->update();
}

void Timeline::applyDuckingFromTrack(int voiceTrackIdx,
                                     double duckGain,
                                     double attackSec,
                                     double releaseSec)
{
    if (voiceTrackIdx < 0 || voiceTrackIdx >= m_audioTracks.size()) return;
    auto *voice = m_audioTracks[voiceTrackIdx];
    if (!voice) return;

    // Build the voice timeline footprint as a list of (start, end) seconds.
    // Each clip lays out sequentially as leadInSec gap + effectiveDuration.
    QVector<QPair<double, double>> voiceRanges;
    {
        double accum = 0.0;
        for (const auto &c : voice->clips()) {
            accum += c.leadInSec;
            const double dur = c.effectiveDuration();
            if (dur > 0.0) voiceRanges.append({accum, accum + dur});
            accum += dur;
        }
    }
    if (voiceRanges.isEmpty()) return;

    duckGain = qBound(0.0, duckGain, 2.0);
    attackSec = qMax(0.0, attackSec);
    releaseSec = qMax(0.0, releaseSec);

    bool anyChanged = false;
    for (int t = 0; t < m_audioTracks.size(); ++t) {
        if (t == voiceTrackIdx) continue;
        auto *bgm = m_audioTracks[t];
        if (!bgm) continue;

        auto bgmClips = bgm->clips();
        bool trackChanged = false;
        double bgmAccum = 0.0;
        for (auto &bc : bgmClips) {
            bgmAccum += bc.leadInSec;
            const double bgmStart = bgmAccum;
            const double bgmEnd = bgmAccum + bc.effectiveDuration();
            const double staticGain = bc.volume;
            QVector<AudioGainPoint> env;

            for (const auto &vr : voiceRanges) {
                const double duckStart = vr.first - attackSec;
                const double duckEnd   = vr.second + releaseSec;
                if (duckEnd <= bgmStart || duckStart >= bgmEnd) continue;

                // Clip the four envelope anchors to the BGM clip's local
                // time window (0..effectiveDuration). Any anchor that lands
                // outside the clip is clamped to the boundary.
                const double bgmDur = bgmEnd - bgmStart;
                const double localAttackStart = qBound(0.0,
                    duckStart - bgmStart, bgmDur);
                const double localVoiceStart  = qBound(0.0,
                    vr.first  - bgmStart, bgmDur);
                const double localVoiceEnd    = qBound(0.0,
                    vr.second - bgmStart, bgmDur);
                const double localReleaseEnd  = qBound(0.0,
                    duckEnd   - bgmStart, bgmDur);

                env.append({localAttackStart, staticGain});
                env.append({localVoiceStart,  duckGain});
                env.append({localVoiceEnd,    duckGain});
                env.append({localReleaseEnd,  staticGain});
            }

            if (!env.isEmpty()) {
                std::sort(env.begin(), env.end(),
                          [](const AudioGainPoint &a, const AudioGainPoint &b) {
                              return a.time < b.time;
                          });
                if (bc.volumeEnvelope.isEmpty()) {
                    bc.volumeEnvelope = env;
                } else {
                    // Merge duck envelope with existing user envelope:
                    // for each duck point, insert it taking MIN gain so
                    // user boost points still allow ducking down but don't
                    // override a manual fade.
                    QVector<AudioGainPoint> merged = bc.volumeEnvelope;
                    std::sort(merged.begin(), merged.end(),
                              [](const AudioGainPoint &a, const AudioGainPoint &b) {
                                  return a.time < b.time;
                              });
                    const int origEnvSize = merged.size();
                    for (const auto &dp : env) {
                        // Find the existing gain at this time by linear
                        // interpolation between adjacent ORIGINAL points
                        // (search only within the pre-existing envelope).
                        double existingGain = dp.gain;
                        if (origEnvSize == 0) {
                            existingGain = 1.0;
                        } else if (origEnvSize == 1) {
                            existingGain = merged[0].gain;
                        } else {
                            for (int i = 0; i < origEnvSize; ++i) {
                                if (merged[i].time >= dp.time) {
                                    if (i == 0) {
                                        existingGain = merged[0].gain;
                                    } else {
                                        const double t0 = merged[i - 1].time;
                                        const double t1 = merged[i].time;
                                        const double g0 = merged[i - 1].gain;
                                        const double g1 = merged[i].gain;
                                        if (t1 - t0 > 0.0) {
                                            const double ratio = (dp.time - t0) / (t1 - t0);
                                            existingGain = g0 + ratio * (g1 - g0);
                                        } else {
                                            existingGain = g0;
                                        }
                                    }
                                    break;
                                }
                                if (i == origEnvSize - 1) {
                                    existingGain = merged[i].gain;
                                }
                            }
                        }
                        merged.append({dp.time, qMin(existingGain, dp.gain)});
                    }
                    std::sort(merged.begin(), merged.end(),
                              [](const AudioGainPoint &a, const AudioGainPoint &b) {
                                  return a.time < b.time;
                              });
                    // De-duplicate points within 1 ms
                    QVector<AudioGainPoint> deduped;
                    for (const auto &p : merged) {
                        if (!deduped.isEmpty() && qAbs(p.time - deduped.last().time) <= 0.001) {
                            // Take the min gain at this time cluster
                            deduped.last().gain = qMin(deduped.last().gain, p.gain);
                        } else {
                            deduped.append(p);
                        }
                    }
                    bc.volumeEnvelope = deduped;
                }
                trackChanged = true;
            }
            bgmAccum += bc.effectiveDuration();
        }
        if (trackChanged) {
            bgm->setClips(bgmClips);
            anyChanged = true;
        }
    }

    if (anyChanged) {
        saveUndoState(QString("BGM auto-duck (voice = A%1)").arg(voiceTrackIdx + 1));
        scheduleEmitSequenceChanged();
        repaintAudioTracks();
    }
}

namespace {

struct TimelineMediaProbe {
    bool openedOk = false;
    bool videoStreamFound = false;
    bool audioStreamFound = false;
    bool wantsAutoProxy = false;
    double durationSec = 0.0;
    double frameRate = 0.0;
    int videoWidth = 0;
    int videoHeight = 0;
    int primaries = 0;
    int transferCharacteristic = 0;
    int bitDepth = 8;
    bool hasHdrMetadata = false;
    bool hasAlpha = false;
};

// import_media とクリップ置き換えで共有する同期 probe。ストリーム有無・尺・
// 映像メタデータを 1 回の avformat open から取得し、呼び出し側が同じ判定を使う。
TimelineMediaProbe probeTimelineMedia(const QString &filePath)
{
    TimelineMediaProbe result;
    AVFormatContext *formatContext = nullptr;
    if (avformat_open_input(&formatContext, filePath.toUtf8().constData(),
                            nullptr, nullptr) != 0) {
        return result;
    }

    result.openedOk = true;
    const bool streamInfoAvailable =
        avformat_find_stream_info(formatContext, nullptr) >= 0;
    if (streamInfoAvailable && formatContext->duration > 0) {
        result.durationSec = static_cast<double>(formatContext->duration)
            / AV_TIME_BASE;
    }

    for (unsigned i = 0; i < formatContext->nb_streams; ++i) {
        const AVStream *stream = formatContext->streams[i];
        if (!stream || !stream->codecpar)
            continue;
        if (streamInfoAvailable && formatContext->duration <= 0
            && stream->duration > 0) {
            const double streamDurationSec =
                stream->duration * av_q2d(stream->time_base);
            if (std::isfinite(streamDurationSec) && streamDurationSec > 0.0)
                result.durationSec = qMax(result.durationSec, streamDurationSec);
        }
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            result.audioStreamFound = true;
            continue;
        }
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO
            || result.videoStreamFound) {
            continue;
        }

        result.videoStreamFound = true;
        const AVRational rate = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
            ? stream->avg_frame_rate : stream->r_frame_rate;
        if (rate.num > 0 && rate.den > 0) result.frameRate = av_q2d(rate);
        result.videoWidth = stream->codecpar->width;
        result.videoHeight = stream->codecpar->height;
        const hdringest::ColorInputs colorInputs =
            hdringest::captureColorInputs(stream->codecpar);
        result.primaries = colorInputs.primaries;
        result.transferCharacteristic = colorInputs.trc;
        result.bitDepth = colorInputs.bitDepth;
        result.hasHdrMetadata = colorInputs.hasHdrMeta;

        const AVPixFmtDescriptor *pixelDescriptor = av_pix_fmt_desc_get(
            static_cast<AVPixelFormat>(stream->codecpar->format));
        const bool alphaByPixelFormat = pixelDescriptor
            && (pixelDescriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
        const bool alphaByCodec = stream->codecpar->codec_id == AV_CODEC_ID_QTRLE
            || stream->codecpar->codec_id == AV_CODEC_ID_PNG;
        result.hasAlpha = alphaByPixelFormat || alphaByCodec;

        const bool isAv1 = stream->codecpar->codec_id == AV_CODEC_ID_AV1;
        const bool isQhdPlus = result.videoWidth >= 2560
            || result.videoHeight >= 1440;
        const bool isHdPlus = result.videoWidth >= 1920
            || result.videoHeight >= 1080;
        result.wantsAutoProxy = isAv1 || isQhdPlus || isHdPlus;
    }

    avformat_close_input(&formatContext);
    return result;
}

} // namespace

bool Timeline::importMedia(const QString &filePath,
                           int requestedTrackIndex,
                           double requestedStartSec,
                           MediaImportResult *result,
                           QString *err,
                           ImportMediaKind kind,
                           const QString &anchorUid)
{
    if (result)
        *result = MediaImportResult{};

    const auto fail = [err](const QString &message) {
        if (err)
            *err = message;
        return false;
    };
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile())
        return fail(QStringLiteral("ファイルが見つかりません: %1").arg(filePath));
    if (requestedTrackIndex < -1)
        return fail(QStringLiteral("trackIndex must be non-negative"));
    if (!std::isfinite(requestedStartSec) || requestedStartSec < -1.0) {
        return fail(QStringLiteral("startSec must be non-negative"));
    }
    // US-1202: a connected import needs exactly one V1 clip with this uid.
    double anchorStartSec = 0.0;
    if (!anchorUid.isEmpty()) {
        int anchorMatches = 0;
        if (const TimelineTrack *v1 = m_videoTracks.value(0, nullptr)) {
            const auto starts = clipStartSecs(v1->clips());
            for (int i = 0; i < v1->clipCount(); ++i) {
                if (v1->clips().at(i).uid != anchorUid) continue;
                if (anchorMatches++ == 0) anchorStartSec = starts[i];
            }
        }
        if (anchorMatches != 1)
            return fail(QStringLiteral("接続先の V1 クリップが一意ではありません"));
    }

    // Phase 1e Win #11 — auto-proxy for heavy video sources. AV1 SW/HW
    // decode runs 4–10x heavier than H.264 (no SIMD-friendly inverse
    // transforms in libdav1d; the D3D11VA path still pays full-res
    // av_hwframe_transfer_data per tick), and 1440p+ sources stress
    // GPU↔CPU bandwidth in multi-track playback regardless of codec.
    // The architect/tracer/critic audit traced user-reported multi-second
    // freezes to AV1 1440p × 2 multi-track with no proxy entries
    // registered — getProxyPath returned the original path so V2/V3
    // pool decoders opened the full-res AV1 source directly. ProxyManager
    // already supports background h264 proxy generation; the gap was that
    // Timeline::addClip never enqueued it. Trigger generation here on
    // import so the existing sequenceChanged → getProxyPath wiring picks
    // up the proxy as soon as it lands. Kept opt-out via
    // VEDITOR_AUTO_PROXY_DISABLE=1 in case a user wants to skip the
    // first-import encode wait, and gated on resolution/codec to avoid
    // generating proxies for clips that already play smoothly.
    const TimelineMediaProbe mediaProbe = probeTimelineMedia(filePath);
    const double duration = mediaProbe.durationSec;
    const bool wantsAutoProxy = mediaProbe.wantsAutoProxy;
    const bool videoStreamFound = mediaProbe.videoStreamFound;
    const bool audioStreamFound = mediaProbe.audioStreamFound;
    const bool openedOk = mediaProbe.openedOk;
    const int capturedPrimaries = mediaProbe.primaries;
    const int capturedTrc = mediaProbe.transferCharacteristic;
    const int capturedBitDepth = mediaProbe.bitDepth;
    const bool capturedHasHdrMeta = mediaProbe.hasHdrMetadata;
    const bool capturedHasAlpha = mediaProbe.hasAlpha;
    // ソース映像の素のピクセル寸法 (アスペクト既定フィット判定用)。0 = 未取得。
    const int srcVideoW = mediaProbe.videoWidth;
    const int srcVideoH = mediaProbe.videoHeight;
    // MCP (Auto / VideoOnly / AudioOnly) は開けないファイルを拒否して 0 秒クリップを
    // 作らない。GUI の LinkedPair は従来どおり (既存のセルフテストが仮ファイルを
    // addClip で置く前提を保つ)。
    if (kind != ImportMediaKind::LinkedPair
        && (!openedOk || (!videoStreamFound && !audioStreamFound))) {
        return fail(QStringLiteral("メディアとして開けません (動画/音声ストリームがありません): %1")
                        .arg(filePath));
    }
    if (kind == ImportMediaKind::VideoOnly && !videoStreamFound)
        return fail(QStringLiteral("映像ストリームがありません: %1").arg(filePath));
    if (kind == ImportMediaKind::AudioOnly && !audioStreamFound)
        return fail(QStringLiteral("音声ストリームがありません: %1").arg(filePath));
    const bool placeVideo = kind == ImportMediaKind::LinkedPair
        || kind == ImportMediaKind::VideoOnly
        || (kind == ImportMediaKind::Auto && videoStreamFound);
    const bool placeAudio = kind == ImportMediaKind::LinkedPair
        || kind == ImportMediaKind::AudioOnly
        || (kind == ImportMediaKind::Auto && audioStreamFound);
    // NOTE: auto-proxy trigger moved past the track-index decision below
    // (search for "autoProxyMode" in this function). wantsAutoProxy is
    // the probe verdict consumed there.
    ClipInfo clip;
    clip.filePath = filePath;
    clip.displayName = QFileInfo(filePath).fileName();
    clip.duration = duration;
    // Every freshly-added clip gets a unique linkGroup shared by its video
    // and audio halves so the two stay locked together until the user
    // explicitly severs the sync via the clip's context menu.
    clip.linkGroup = allocateLinkGroup();

    const bool hdrIngestEnabled = hdringest::enabledFromEnv();
    const bool hdrTraceEnabled = hdringest::traceEnabledFromEnv();
    clipcolor::ColorMeta derivedColorMeta = clip.colorMeta;
    if (videoStreamFound && (hdrIngestEnabled || hdrTraceEnabled))
        derivedColorMeta = clipcolor::fromCodecParams(capturedPrimaries, capturedTrc,
                                                      capturedBitDepth,
                                                      capturedHasHdrMeta);
    if (hdrIngestEnabled && videoStreamFound)
        clip.colorMeta = derivedColorMeta;
    if (hdrTraceEnabled) {
        qInfo().noquote() << "[hdr-ingest] file=" << clip.displayName
                          << "ingest=" << (hdrIngestEnabled ? "on" : "off")
                          << "videoStreamFound=" << videoStreamFound
                          << "derived=" << clipcolor::describe(derivedColorMeta)
                          << "clip=" << clipcolor::describe(clip.colorMeta);
    }

    // Import placement policy — user chooses between parallel stacking
    // (V1→V2→V3...) and append-to-first-track (V1/A1 continuous sequence).
    // Toggled from MainWindow's 環境設定 submenu.
    QSettings prefs("VSimpleEditor", "Preferences");
    const ImportPlacement placement = static_cast<ImportPlacement>(
        prefs.value("importPlacement", static_cast<int>(ImportPlacement::ParallelTrack)).toInt());

    // US-1306: decide the target indices first, validate against the tracks
    // that already exist (lock + planDrop), and only then create tracks. A
    // new track is empty and unlocked, so it can never fail validation; every
    // failing return therefore happens before any track is created.
    const QString lockedMessage = QStringLiteral("ロック中のトラックには配置できません");
    int videoTrackIdx = -1;
    int audioTrackIdx = -1;
    bool placementFixed = false;
    if (requestedTrackIndex >= 0) {
        // MCP は指定されたトラック番号をそのまま V/A のペアに使う。
        // 片方だけ増やすとリンク済み素材が孤立するため、必要なら両方を同時に作る
        // (片側だけ置く kind ではその側だけ)。
        videoTrackIdx = requestedTrackIndex;
        audioTrackIdx = requestedTrackIndex;
        placementFixed = true;
    } else if (placement == ImportPlacement::AppendToFirstTrack) {
        // Extend the existing sequence: always V1/A1, appending after
        // whatever clips already live there. Create V1/A1 if the project
        // starts with zero tracks.
        videoTrackIdx = 0;
        audioTrackIdx = 0;
        placementFixed = true;
    } else {
        // Premiere-style stacking: each drop lands on the first EMPTY and
        // unlocked video track (creating V<n>/A<n> as needed). First drop
        // fills V1/A1, second fills V2/A2, and so on.
        const auto firstFreeTrack = [](const QVector<TimelineTrack *> &tracks) {
            for (int t = 0; t < tracks.size(); ++t) {
                if (tracks[t] && tracks[t]->clipCount() == 0 && !tracks[t]->isLocked())
                    return t;
            }
            return static_cast<int>(tracks.size());
        };
        if (placeVideo)
            videoTrackIdx = firstFreeTrack(m_videoTracks);
        if (placeAudio)
            audioTrackIdx = firstFreeTrack(m_audioTracks);
    }
    // US-1304: with storyline mode on, a video landing on V1 is a storyline insert
    // at the drop point; its audio goes to the first free unlocked audio track.
    const bool storylineInsert = m_storylineMode && placeVideo && videoTrackIdx == 0
        && anchorUid.isEmpty();
    if (placementFixed) {
        // 配置先が固定のモード (明示 index / AppendToFirstTrack) はロック中なら拒否する。
        const TimelineTrack *lockedV = placeVideo ? m_videoTracks.value(videoTrackIdx, nullptr) : nullptr;
        const TimelineTrack *lockedA = placeAudio && !storylineInsert
            ? m_audioTracks.value(audioTrackIdx, nullptr) : nullptr;
        if ((lockedV && lockedV->isLocked()) || (lockedA && lockedA->isLocked()))
            return fail(lockedMessage);
    }

    auto trackEnd = [](const TimelineTrack *track) {
        double end = 0.0;
        if (!track)
            return end;
        for (const ClipInfo &existing : track->clips())
            end += qMax(0.0, existing.leadInSec) + existing.effectiveDuration();
        return end;
    };
    const TimelineTrack *existingVideo = placeVideo ? m_videoTracks.value(videoTrackIdx, nullptr) : nullptr;
    const TimelineTrack *existingAudio = placeAudio ? m_audioTracks.value(audioTrackIdx, nullptr) : nullptr;

    // 指定位置は空きへ正確に入れ、既存クリップと重なる要求は MCP から
    // 黙って丸めない。startSec 省略時だけ対象トラック末尾へ追記する。
    const double videoDropTime = requestedStartSec >= 0.0
        ? requestedStartSec : trackEnd(existingVideo);
    // MCP の指定トラックでは V/A を同じ linkGroup の一組として扱うため、
    // startSec 省略時も音声だけ別の末尾へ置かず video の開始時刻を共有する。
    // GUI の従来の自動トラック選択では、元の V/A 各トラック末尾規則を維持する。
    const double audioDropTime = (requestedTrackIndex >= 0 && placeVideo)
        ? videoDropTime
        : (requestedStartSec >= 0.0 ? requestedStartSec : trackEnd(existingAudio));
    const double importDuration = qMax(0.0, clip.effectiveDuration());
    if (!storylineInsert && existingVideo && !existingVideo->planDrop(videoDropTime, importDuration).valid)
        return fail(QStringLiteral("指定位置にクリップを配置できません"));
    if (!storylineInsert && existingAudio && !existingAudio->planDrop(audioDropTime, importDuration).valid)
        return fail(QStringLiteral("指定位置にクリップを配置できません"));
    // The audio slot is settled before anything changes, so a failure needs no
    // rollback. The V1 insert closes the gaps in front of it: the landing time is
    // the summed length of the clips before the insert index, not their start
    // times (which still include those gaps).
    TimelineTrack *storylineAudioTrack = nullptr;
    TimelineTrack::DropPlan storylineAudioPlan;
    double landedSec = 0.0;
    if (storylineInsert) {
        if (const TimelineTrack *v1 = m_videoTracks.value(0, nullptr)) {
            const int index = threepoint::insertIndexForTime(v1->clips(), videoDropTime, nullptr);
            for (int i = 0; i < index; ++i)
                landedSec += v1->clips().at(i).effectiveDuration();
        }
        if (placeAudio) {
            storylineAudioTrack = storylineAudioSlot(0, clip, landedSec, &storylineAudioPlan);
            if (!storylineAudioTrack)
                return fail(QStringLiteral("音声を置ける空きトラックがありません"));
            audioTrackIdx = m_audioTracks.indexOf(storylineAudioTrack);
        }
    }

    // addVideoTrack(false) / addAudioTrack(false) below skip the capture; take
    // it before any track is created so undoing the import restores live state.
    captureExternalTrackState();
    while (placeVideo && m_videoTracks.size() <= videoTrackIdx)
        addVideoTrack(false);
    while (placeAudio && !storylineInsert && m_audioTracks.size() <= audioTrackIdx)
        addAudioTrack(false);

    qInfo() << "Timeline::addClip routing video→V" << (videoTrackIdx + 1)
            << "audio→A" << (audioTrackIdx + 1)
            << "file=" << filePath;

    TimelineTrack *videoTrack = placeVideo ? m_videoTracks.value(videoTrackIdx, nullptr) : nullptr;
    TimelineTrack *audioTrack = placeAudio ? m_audioTracks.value(audioTrackIdx, nullptr) : nullptr;
    if ((placeVideo && !videoTrack) || (placeAudio && !audioTrack))
        return fail(QStringLiteral("取り込み先のトラックを準備できません"));
    TimelineTrack::DropPlan videoPlan;
    TimelineTrack::DropPlan audioPlan;
    if (placeVideo && !storylineInsert) {
        videoPlan = videoTrack->planDrop(videoDropTime, importDuration);
        if (!videoPlan.valid)
            return fail(QStringLiteral("指定位置にクリップを配置できません"));
    }
    if (placeAudio && storylineInsert) {
        audioPlan = storylineAudioPlan;
    } else if (placeAudio) {
        audioPlan = audioTrack->planDrop(audioDropTime, importDuration);
        if (!audioPlan.valid)
            return fail(QStringLiteral("指定位置にクリップを配置できません"));
    }

    // Auto-proxy dispatch — runs after the track index is settled so that
    // MultiTrackOnly mode can gate on V2-or-later. wantsAutoProxy was
    // probed up at the top of this function (AV1 OR ≥2560×1440).
    if (wantsAutoProxy && !capturedHasAlpha) {
        static const bool autoProxyDisabled =
            qEnvironmentVariableIntValue("VEDITOR_AUTO_PROXY_DISABLE") != 0;
        if (!autoProxyDisabled) {
            const auto mode = static_cast<AutoProxyMode>(
                prefs.value("autoProxyMode",
                            static_cast<int>(AutoProxyMode::MultiTrackOnly)).toInt());
            bool shouldTrigger = false;
            switch (mode) {
                case AutoProxyMode::Disabled:
                    break;
                case AutoProxyMode::MultiTrackOnly:
                    shouldTrigger = (videoTrackIdx >= 1);
                    break;
                case AutoProxyMode::Always:
                    shouldTrigger = true;
                    break;
            }
            if (shouldTrigger) {
                ProxyManager &pm = ProxyManager::instance();
                // Intentionally do NOT gate on pm.isProxyMode() here: proxy
                // mode controls *playback* substitution, but generating on
                // import is cheap (idempotent, background QProcess) and
                // pays off the moment the user toggles proxy mode on.
                if (pm.config().enabled && !pm.hasProxy(filePath)) {
                    qDebug() << "[auto-proxy] queuing proxy for heavy source"
                             << "file=" << filePath
                             << "mode=" << static_cast<int>(mode)
                             << "videoTrackIdx=" << videoTrackIdx;
                    pm.generateProxy(filePath);
                }
            }
        }
    }

    // Generate waveform async; apply to the AUDIO track that received the clip.
    auto *wfGen = new WaveformGenerator(this);
    QPointer<Timeline> self(this);
    const int waveAudioIdx = audioTrackIdx;
    connect(wfGen, &WaveformGenerator::waveformReady, this,
        [self, wfGen, waveAudioIdx](const QString &path, const WaveformData &data) {
            qInfo() << "Timeline::waveformReady slot peaks=" << data.peaks.size()
                    << "dur=" << data.duration << "audioIdx=" << waveAudioIdx;
            if (self && waveAudioIdx >= 0 && waveAudioIdx < self->m_audioTracks.size()) {
                auto *track = self->m_audioTracks[waveAudioIdx];
                if (track) {
                    auto clips = track->clips();
                    for (int i = 0; i < clips.size(); ++i) {
                        if (clips[i].filePath == path && clips[i].waveform.isEmpty()) {
                            clips[i].waveform = data;
                            track->setClips(clips);
                            break;
                        }
                    }
                }
            }
            wfGen->deleteLater();
        });
    wfGen->generateAsync(filePath);

    // Premiere 準拠の既定フィット: アスペクトの異なるクリップを explicit 出力
    // (SNS 縦動画プリセット等で確定したサイズ) のプロジェクトへ追加したとき、
    // 既定 (fit=none) だと renderLayer の IgnoreAspectRatio 充填で縦/横に
    // 引き伸ばされる。Premiere は決してアスペクトを歪めないため、ここで contain
    // (レターボックス) を既定にする。判定は render 時と同一述語 snsfit::shouldContain
    // をそのまま使う (アスペクト差 < 1e-4 のマッチクリップは false=フラグ未設定で
    // byte-identical、render SSOT は不変)。explicit 出力でないプロジェクトは従来どおり
    // none のまま (挙動不変)。cover やフィット解除へはクリップ右クリックで上書き可能。
    if (videoStreamFound && m_projectExplicitOutput
        && srcVideoW > 0 && srcVideoH > 0) {
        const QSize projOut(m_projectWidth, m_projectHeight);
        if (snsfit::shouldContain(true, projOut, QSize(srcVideoW, srcVideoH)))
            clip.fitContain = true;
    }

    // 取り込みも他の MCP 編集と同じく、snapshot を変更の直前に作る。
    // US-1320: the import's key remap and the reconcile's (saveUndoState) leave as one emit.
    std::optional<ClipKeyRemapBatch> keyRemapBatch(std::in_place, this);
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    ClipInfo videoClip = clip;
    ClipInfo audioClip = clip;
    if (!anchorUid.isEmpty()) {
        videoClip.anchorUid = audioClip.anchorUid = anchorUid;
        videoClip.anchorOffsetSec = videoDropTime - anchorStartSec;
        audioClip.anchorOffsetSec = audioDropTime - anchorStartSec;
    }
    double landedActualSec = landedSec;
    if (storylineInsert) {
        double actualSec = 0.0;
        videoPlan.insertIdx = storylineInsertClip(videoTrack, videoDropTime, videoClip, &actualSec);
        landedActualSec = actualSec;
        if (std::abs(actualSec - landedSec) > kConnectionEps) {
            qWarning() << "Timeline::importMedia storyline landing mismatch planned=" << landedSec
                       << "actual=" << actualSec;
            appendConnectionWarning(QStringLiteral("storyline landing mismatch planned=%1 actual=%2")
                                        .arg(landedSec).arg(actualSec));
            Q_ASSERT_X(false, "Timeline::importMedia", "storyline landing mismatch");
        }
    } else if (placeVideo) {
        videoTrack->insertClipPreservingDownstream(videoPlan.insertIdx, videoClip,
                                                   videoPlan.newLeadIn);
    }
    if (placeAudio) {
        audioTrack->insertClipPreservingDownstream(audioPlan.insertIdx, audioClip,
                                                   audioPlan.newLeadIn);
    }
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState("Add clip");
    keyRemapBatch.reset();
    if (result) {
        result->videoTrackIndex = placeVideo ? videoTrackIdx : -1;
        result->videoClipIndex = placeVideo ? videoPlan.insertIdx : -1;
        result->audioTrackIndex = placeAudio ? audioTrackIdx : -1;
        result->audioClipIndex = placeAudio ? audioPlan.insertIdx : -1;
        result->videoStartSec = storylineInsert ? landedActualSec : videoDropTime;
        result->audioStartSec = storylineInsert ? landedActualSec : audioDropTime;
        result->durationSec = importDuration;
    }
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    emit mediaImported();
    return true;
}

int Timeline::allocateLinkGroup()
{
    QSet<int> used;
    for (const auto *track : m_videoTracks)
        for (const auto &clip : track->clips()) used.insert(clip.linkGroup);
    for (const auto *track : m_audioTracks)
        for (const auto &clip : track->clips()) used.insert(clip.linkGroup);
    int group = 0;
    do { group = m_nextLinkGroup++; } while (used.contains(group));
    return group;
}

void Timeline::addClip(const QString &filePath)
{
    // GUI の従来経路も MCP と同じ取り込み実装を使い、配置・リンク・Undo が
    // 別々に進化しないようにする。取り込みに失敗した時は理由をステータスバーに 5 秒
    // 表示する(ルール (60)、非モーダル)。
    QString err;
    if (!importMedia(filePath, -1, -1.0, nullptr, &err) && !err.isEmpty())
        emit statusMessageRequested(err, 5000);
}

TimelineTrack *Timeline::primaryErgoTrack() const
{
    auto selected = [](TimelineTrack *track) {
        return track && track->selectedClip() >= 0 && track->selectedClip() < track->clipCount();
    };
    auto *video = m_videoTracks.value(m_activeVideoTrackIndex, nullptr);
    if (selected(video)) return video;
    auto *audio = m_audioTracks.value(m_activeAudioTrackIndex, nullptr);
    if (selected(audio)) return audio;
    for (auto *track : m_videoTracks) if (selected(track)) return track;
    for (auto *track : m_audioTracks) if (selected(track)) return track;
    return nullptr;
}

void Timeline::moveSelectedClipToPlayhead(bool tail)
{
    auto *track = primaryErgoTrack();
    if (!track) {
        emit statusMessageRequested(QStringLiteral("移動するクリップを選択してください。"), 3000);
        return;
    }
    const int index = track->selectedClip();
    const bool audio = track->isAudioTrack();
    const int row = audio ? m_audioTracks.indexOf(track) : m_videoTracks.indexOf(track);
    const double requested = qMax(0.0, m_playheadPos
        - (tail ? track->clips()[index].effectiveDuration() : 0.0));
    double target = requested;
    QVector<double> attempted;
    // Collision candidates do not mutate or save undo. Only the successful
    // move commits, including linked audio and the existing carrier remapping.
    for (int attempt = 0; attempt < 64; ++attempt) {
        attempted.append(target);
        double settled = target;
        QString error;
        if (!moveClipByIndex(audio, row, index, target, &settled, &error)) {
            emit statusMessageRequested(QStringLiteral("クリップを移動できませんでした。ロック状態を確認してください。"), 3000);
            return;
        }
        if (qAbs(settled - target) <= 1e-6) {
            if (qAbs(settled - requested) > 1e-6)
                emit statusMessageRequested(QStringLiteral("移動先を %1 秒に調整しました。")
                                                .arg(settled, 0, 'f', 3), 3000);
            return;
        }
        bool repeated = false;
        for (double previous : attempted)
            if (qAbs(previous - settled) <= 1e-6) repeated = true;
        if (repeated) break;
        target = settled;
    }
    emit statusMessageRequested(QStringLiteral("リンクしたクリップを配置できる移動先がありません。"), 3000);
}

void Timeline::selectClipsWithSameLabel()
{
    auto *track = primaryErgoTrack();
    if (!track) {
        emit statusMessageRequested(QStringLiteral("基準となるクリップを選択してください。"), 3000);
        return;
    }
    const ClipLabel label = track->clips()[track->selectedClip()].label;
    if (label == ClipLabel::None) {
        emit statusMessageRequested(QStringLiteral("ラベルの無いクリップは対象外です。"), 3000);
        return;
    }
    selectClipsForErgo(0, label);
}

void Timeline::selectAllClips()
{
    selectClipsForErgo(0);
}

void Timeline::selectClipsFromPlayhead(bool forward)
{
    selectClipsForErgo(forward ? 1 : -1);
}

void Timeline::selectClipsForErgo(int direction, ClipLabel label)
{
    // Block linked-selection propagation: eligibility is per clip and track.
    int primary = -1;
    m_activeVideoTrackIndex = -1;
    m_activeAudioTrackIndex = -1;
    auto selectTrack = [&](TimelineTrack *track, int videoIndex) {
        if (!track) return;
        const QSignalBlocker blocker(track);
        track->clearClipSelection();
        if (track->isLocked() || track->isHidden()) return;
        double cursor = 0.0;
        for (int i = 0; i < track->clipCount(); ++i) {
            const ClipInfo &clip = track->clips()[i];
            const double start = cursor + clip.leadInSec;
            const double end = start + clip.effectiveDuration();
            if ((label == ClipLabel::None || clip.label == label)
                && (direction == 0 || (direction > 0 && start >= m_playheadPos)
                    || (direction < 0 && end <= m_playheadPos))) {
                track->toggleClipSelection(i);
                if (primary < 0 || videoIndex >= 0) {
                    primary = i;
                    m_activeVideoTrackIndex = videoIndex;
                    m_activeAudioTrackIndex = videoIndex < 0 ? m_audioTracks.indexOf(track) : -1;
                }
            }
            cursor = end;
        }
    };
    for (int i = 0; i < m_videoTracks.size(); ++i) selectTrack(m_videoTracks[i], i);
    for (auto *track : m_audioTracks) selectTrack(track, -1);
    emit clipSelected(primary);
    emit clipSelectedOnTrack(m_activeVideoTrackIndex, primary);
    if (primary < 0)
        emit statusMessageRequested(QStringLiteral("選択できるクリップがありません。"), 3000);
}

void Timeline::bladeAllTracksAtPlayhead()
{
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QHash<int, int> oldGroupToNewGroup;
    // Loaded projects may contain groups beyond the allocator's current value.
    QSet<int> usedGroups;
    auto collectGroups = [&](TimelineTrack *track) {
        if (!track) return;
        for (const auto &clip : track->clips())
            if (clip.linkGroup > 0) usedGroups.insert(clip.linkGroup);
    };
    for (auto *track : m_videoTracks) collectGroups(track);
    for (auto *track : m_audioTracks) collectGroups(track);
    bool anySplit = false;
    auto splitTrack = [&](TimelineTrack *track) {
        if (!track || track->isLocked()) return;
        const auto clips = track->clips();
        double cursor = 0.0;
        for (int i = 0; i < clips.size(); ++i) {
            const double start = cursor + clips[i].leadInSec;
            const double end = start + clips[i].effectiveDuration();
            if (m_playheadPos > start && m_playheadPos < end) {
                track->splitClipAt(i, m_playheadPos - start, false);
                if (track->clipCount() == clips.size()) return;
                auto updated = track->clips();
                const int oldGroup = updated[i + 1].linkGroup;
                if (oldGroup > 0) {
                    auto it = oldGroupToNewGroup.find(oldGroup);
                    if (it == oldGroupToNewGroup.end()) {
                        int newGroup = allocateLinkGroup();
                        while (usedGroups.contains(newGroup)) newGroup = allocateLinkGroup();
                        usedGroups.insert(newGroup);
                        it = oldGroupToNewGroup.insert(oldGroup, newGroup);
                    }
                    updated[i + 1].linkGroup = it.value();
                }
                track->setClips(updated);
                // Keep selected downstream clips attached to their new indices.
                const auto selected = track->selectedClips();
                const QSignalBlocker blocker(track);
                track->clearClipSelection();
                for (int index : selected)
                    track->toggleClipSelection(index > i ? index + 1 : index);
                anySplit = true;
                return;
            }
            cursor = end;
        }
    };
    for (auto *track : m_videoTracks) splitTrack(track);
    for (auto *track : m_audioTracks) splitTrack(track);
    if (!anySplit) {
        emit statusMessageRequested(QStringLiteral("再生ヘッド位置に分割できるクリップがありません。"), 3000);
        return;
    }
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("全トラックを再生ヘッドで分割"));
    // Refresh edit targets without triggering linked-selection propagation.
    int primary = -1;
    int videoTrackIndex = -1;
    if (m_activeVideoTrackIndex >= 0 && m_activeVideoTrackIndex < m_videoTracks.size()
        && m_videoTracks[m_activeVideoTrackIndex]) {
        videoTrackIndex = m_activeVideoTrackIndex;
        primary = m_videoTracks[videoTrackIndex]->selectedClip();
    } else {
        for (auto *track : m_audioTracks) {
            if (track && track->selectedClip() >= 0) {
                primary = track->selectedClip();
                break;
            }
        }
    }
    emit clipSelected(primary);
    emit clipSelectedOnTrack(videoTrackIndex, primary);
    onTrackModified();
    updateInfoLabel();
    emit positionChanged(m_playheadPos);
}

void Timeline::splitAtPlayhead()
{
    // Split only the clips the user has explicitly selected (on any track),
    // and only when the playhead lies inside that clip. This matches how
    // NLEs behave with a multi-selection: if you selected one clip you get
    // one cut; if nothing is selected, nothing happens.
    //
    // The right half of every cut gets a FRESH linkGroup so post-split
    // halves are treated as independent clips. A single shared map keeps
    // V_right and A_right in the same new group — the linked V/A pair
    // stays linked after the split, just with a new identity separate from
    // the left half.
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    bool anySplit = false;
    QHash<int, int> oldGroupToNewGroup;
    auto splitTrack = [&](TimelineTrack *track) {
        if (!track) return;
        const QList<int> selected = track->selectedClips();
        if (selected.isEmpty()) return;
        QVector<ClipInfo> clips = track->clips();
        double accum = 0.0;
        for (int i = 0; i < clips.size(); ++i) {
            accum += clips[i].leadInSec;
            const double clipDur = clips[i].effectiveDuration();
            const double clipEnd = accum + clipDur;
            if (selected.contains(i)
                && m_playheadPos > accum + 0.05
                && m_playheadPos < clipEnd - 0.05) {
                track->splitClipAt(i, m_playheadPos - accum);
                auto updated = track->clips();
                if (i + 1 < updated.size()) {
                    const int oldGroup = updated[i + 1].linkGroup;
                    if (oldGroup > 0) {
                        auto it = oldGroupToNewGroup.find(oldGroup);
                        if (it == oldGroupToNewGroup.end())
                            it = oldGroupToNewGroup.insert(oldGroup, allocateLinkGroup());
                        updated[i + 1].linkGroup = it.value();
                        track->setClips(updated);
                    }
                }
                anySplit = true;
                return;
            }
            accum = clipEnd;
        }
    };
    for (auto *t : m_videoTracks) splitTrack(t);
    for (auto *t : m_audioTracks) splitTrack(t);
    if (anySplit) {
        remapTrackMatteLocked(snapBefore);
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
        saveUndoState("Split clip");
        updateInfoLabel();
    }
}

bool Timeline::freezeFrameAtPlayhead(TimelineTrack *track, int clipIndex)
{
    if (!track) {
        if (!clipUnderPlayhead(track, clipIndex))
            return false;
    }
    if (!track || m_videoTracks.indexOf(track) < 0 || track->isLocked())
        return false;
    if (clipIndex < 0 || clipIndex >= track->clips().size())
        return false;

    constexpr double kEps = 1e-6;
    const QVector<ClipInfo> beforeClips = track->clips();
    double cursor = 0.0;
    double clipStart = 0.0;
    double clipEnd = 0.0;
    bool found = false;
    for (int i = 0; i < beforeClips.size(); ++i) {
        const ClipInfo &clip = beforeClips[i];
        const double start = cursor + qMax(0.0, clip.leadInSec);
        const double end = start + clip.effectiveDuration();
        if (i == clipIndex) {
            clipStart = start;
            clipEnd = end;
            found = true;
            break;
        }
        cursor = end;
    }
    if (!found || clipEnd - clipStart <= kEps)
        return false;
    if (m_playheadPos < clipStart - kEps || m_playheadPos >= clipEnd - kEps)
        return false;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    int holdIndex = clipIndex;
    const double localSeconds = qBound(0.0, m_playheadPos - clipStart, clipEnd - clipStart);
    // US-1501: hold the source frame under the playhead (F), whatever piece
    // of the source the right half was cut to.
    const double heldSourceSec = beforeClips[clipIndex].sourceSecondAtLocalTime(
        localSeconds > kEps ? localSeconds : 0.0);
    if (localSeconds > kEps) {
        const int beforeCount = track->clipCount();
        track->splitClipAt(clipIndex, localSeconds, false);
        if (track->clipCount() != beforeCount + 1)
            return false;
        holdIndex = clipIndex + 1;
    }

    QVector<ClipInfo> updated = track->clips();
    if (holdIndex < 0 || holdIndex >= updated.size())
        return false;

    timeremap::TimeRemapCurve holdCurve;
    holdCurve.blendMode = timeremap::FrameBlendMode::NearestFrame;
    const bool hadTimeRemap = updated[holdIndex].hasTimeRemap();
    holdCurve.sourceFps = hadTimeRemap && updated[holdIndex].timeRemapCurve.sourceFps > 0.0
        ? updated[holdIndex].timeRemapCurve.sourceFps
        : 0.0;
    const ClipInfo &held = updated[holdIndex];
    const double heldOutSec = (held.outPoint > 0.0) ? held.outPoint : held.duration;
    holdCurve.addKey(0.0, held.reversed ? heldOutSec - heldSourceSec
                                        : heldSourceSec - held.inPoint);
    updated[holdIndex].timeRemapCurve = holdCurve;
    track->setClips(updated);
    track->setSelectedClip(holdIndex);

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("Freeze frame"));
    ensureSequenceFitsViewport();
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
    updateInfoLabel();
    emit positionChanged(m_playheadPos);
    return true;
}

QVector<Timeline::GroupClipTarget> Timeline::selectedGroupClipTargets() const
{
    QVector<GroupClipTarget> result;
    for (bool audio : {false, true}) {
        const auto &tracks = audio ? m_audioTracks : m_videoTracks;
        for (int t = 0; t < tracks.size(); ++t)
            for (int c : tracks[t]->selectedClips()) result.append({audio, t, c});
    }
    return result;
}

QVector<Timeline::GroupClipTarget> Timeline::selectedArrangementTargets() const
{
    QVector<GroupClipTarget> result;
    for (const auto &target : selectedGroupClipTargets()) {
        if (target.audio) continue;
        const auto *track = m_videoTracks[target.trackIndex];
        if (target.clipIndex < 0 || target.clipIndex >= track->clipCount()) continue;
        if (!clipgeom::isNullObjectFilePath(track->clips()[target.clipIndex].filePath))
            result.append(target);
    }
    return result;
}

bool Timeline::canArrangeSelectedLayers(LayerArrangement arrangement) const
{
    const auto targets = selectedArrangementTargets();
    const bool distribute = arrangement == LayerArrangement::DistributeHorizontal
                         || arrangement == LayerArrangement::DistributeVertical;
    if (targets.size() < (distribute ? 3 : 1)
        || m_projectWidth <= 0 || m_projectHeight <= 0) return false;
    for (const auto &target : targets)
        if (m_videoTracks[target.trackIndex]->isLocked()) return false;
    return true;
}

bool Timeline::arrangeSelectedLayers(LayerArrangement arrangement)
{
    if (!canArrangeSelectedLayers(arrangement)) return false;
    const bool horizontal = arrangement == LayerArrangement::Left
                         || arrangement == LayerArrangement::HorizontalCenter
                         || arrangement == LayerArrangement::Right
                         || arrangement == LayerArrangement::DistributeHorizontal;
    const bool distribute = arrangement == LayerArrangement::DistributeHorizontal
                         || arrangement == LayerArrangement::DistributeVertical;
    const QSize canvas(m_projectWidth, m_projectHeight);
    const double extent = horizontal ? canvas.width() : canvas.height();
    // Evaluate what renderFrameAt draws at the playhead: position keyframes via
    // clipanim::effectiveTransformAt, then the parent chain via composeParented
    // (same guards as TimelineFrameRenderer's parent resolve).
    const bool parenting = veditor::parentingEnabledFromEnv();
    const auto clipStart = [&](int trackIdx, int clipIdx) {
        const auto &clips = m_videoTracks[trackIdx]->clips();
        double cursor = 0.0;
        for (int i = 0; i < clipIdx; ++i)
            cursor += clips[i].leadInSec + clips[i].effectiveDuration();
        return cursor + clips[clipIdx].leadInSec;
    };
    const auto clampedLocalSecondsAt = [&](int trackIdx, int clipIdx) {
        const auto &clip = m_videoTracks[trackIdx]->clips()[clipIdx];
        return qBound(0.0, m_playheadPos - clipStart(trackIdx, clipIdx),
                      clip.effectiveDuration());
    };
    // US-805: the parent is active exactly when the renderer composites it as a
    // layer (tlrender::parentLayerAt: base track, hidden track, disabled /
    // adjustment clip, overlap selection and transition trail included) and its
    // media is statically decodable; otherwise the child renders raw. The
    // parent's transform is evaluated at the clip-local seconds the gate returns.
    const qint64 playheadUsec = qRound64(m_playheadPos * 1'000'000.0);
    // US-805: clip-local seconds the renderer evaluates a selected clip at
    // (unclamped; inside a borrowed transition interval it runs past the
    // nominal end / before the nominal start). Falls back to the clamped
    // nominal time where the gate has no hit (V1, outside the clip's layer).
    const auto renderLocalSecondsAt = [&](int trackIdx, int clipIdx) {
        const auto hit = tlrender::parentLayerAt(this, playheadUsec, trackIdx, clipIdx);
        return hit ? hit->clipLocalSeconds : clampedLocalSecondsAt(trackIdx, clipIdx);
    };
    const auto activeParentLayer = [&](const QString &key, int *trackIdx, int *clipIdx,
                                       double *localSec) {
        const QStringList parts = key.split(QLatin1Char(':'));
        if (parts.size() != 2) return false;
        bool okTrack = false, okClip = false;
        const int t = parts[0].toInt(&okTrack), c = parts[1].toInt(&okClip);
        if (!okTrack || !okClip) return false;
        const auto hit = tlrender::parentLayerAt(this, playheadUsec, t, c);
        if (!hit || !hit->decodable) return false;
        *trackIdx = t;
        *clipIdx = c;
        *localSec = hit->clipLocalSeconds;
        return true;
    };
    // 0 = no parent, 1 = parented (*parentWorld set), -1 = broken chain (renders raw).
    std::function<int(int, int, int, QVector<QString> &, clipgeom::ClipTransform *)> resolveParent;
    const auto resolveWorld = [&](int trackIdx, int clipIdx, double localSec, int depth,
                                  QVector<QString> &stack, clipgeom::ClipTransform *world) {
        const auto &clip = m_videoTracks[trackIdx]->clips()[clipIdx];
        *world = clipanim::effectiveTransformAt(clip, localSec);
        if (depth > 8) return true;
        clipgeom::ClipTransform parentWorld;
        const int parented = resolveParent(trackIdx, clipIdx, depth, stack, &parentWorld);
        if (parented < 0) return false;
        if (parented > 0) *world = clipgeom::composeParented(*world, parentWorld, canvas);
        return true;
    };
    resolveParent = [&](int trackIdx, int clipIdx, int depth, QVector<QString> &stack,
                        clipgeom::ClipTransform *parentWorld) {
        if (!parenting) return 0;
        const QString key = trackMatteClipKey(trackIdx, clipIdx);
        const QString parentKey = m_clipParentEntries.value(key);
        if (parentKey.isEmpty()) return 0;
        int parentTrack = -1, parentClip = -1;
        double parentLocalSec = 0.0;
        if (!activeParentLayer(parentKey, &parentTrack, &parentClip, &parentLocalSec)
            || parentKey == key || stack.contains(key) || stack.contains(parentKey))
            return -1;
        stack.append(key);
        const bool ok = resolveWorld(parentTrack, parentClip, parentLocalSec, depth + 1, stack,
                                     parentWorld);
        stack.removeLast();
        return ok ? 1 : -1;
    };
    struct Item {
        GroupClipTarget target; double start = 0.0; double length = 0.0;
        double renderLocalSec = 0.0; ClipInfo original; double goal = 0.0;
        bool wrote = false;
    };
    const auto clipOf = [&](const Item &item) {
        return m_videoTracks[item.target.trackIndex]->clips()[item.target.clipIndex];
    };
    // Bounds as renderFrameAt draws the clip at the playhead (the rendering SSOT,
    // including its center pivot, scale and rotation) under the current parent chain.
    const auto boundsOf = [&](const Item &item, const ClipInfo &clip) {
        clipgeom::ClipTransform transform = clipanim::effectiveTransformAt(clip, item.renderLocalSec);
        clipgeom::ClipTransform parentWorld;
        QVector<QString> stack;
        if (resolveParent(item.target.trackIndex, item.target.clipIndex, 0, stack,
                          &parentWorld) > 0)
            transform = clipgeom::composeParented(transform, parentWorld, canvas);
        return clipgeom::resolveTransform(transform, canvas)
            .mapRect(QRectF(QPointF(0, 0), QSizeF(canvas)));
    };
    // The aligned point of a clip's rendered bounds: left / center / right (top /
    // middle / bottom). Distribution places left (top) edges.
    const double anchor = arrangement == LayerArrangement::HorizontalCenter
                       || arrangement == LayerArrangement::VerticalCenter ? 0.5
                        : arrangement == LayerArrangement::Right
                       || arrangement == LayerArrangement::Bottom ? 1.0 : 0.0;
    const auto edgeOf = [&](const QRectF &bounds) {
        return horizontal ? bounds.left() + anchor * bounds.width()
                          : bounds.top() + anchor * bounds.height();
    };
    QVector<Item> items;
    for (const auto &target : selectedArrangementTargets()) {
        Item item;
        item.target = target;
        item.renderLocalSec = renderLocalSecondsAt(target.trackIndex, target.clipIndex);
        item.original = clipOf(item);
        const QRectF bounds = boundsOf(item, item.original);
        item.start = horizontal ? bounds.left() : bounds.top();
        item.length = horizontal ? bounds.width() : bounds.height();
        if (!std::isfinite(item.start) || !std::isfinite(item.length)) return false;
        // Alignment reference is the canvas (0 / center / extent), so it never
        // moves with a clip's own width change.
        item.goal = anchor * extent;
        items.append(item);
    }
    // Distribution reference: the outermost clips in spatial order keep their
    // original rendered (world) position, and the middle ones get equal gaps
    // between rendered bounds. An endpoint is still solved like every other clip,
    // so when a selected ancestor moves, the endpoint is compensated back to its
    // world position (same parent-first rule as alignment). layout() derives the
    // middle goals from the current widths; gapSpread() is max gap - min gap.
    const auto currentBounds = [&]() {
        QVector<QRectF> bounds;
        for (const auto &item : items) bounds.append(boundsOf(item, clipOf(item)));
        return bounds;
    };
    const auto lo = [&](const QRectF &b) { return horizontal ? b.left() : b.top(); };
    const auto len = [&](const QRectF &b) { return horizontal ? b.width() : b.height(); };
    const auto layout = [&]() {
        const QVector<QRectF> bounds = currentBounds();
        double totalLength = 0.0;
        for (const auto &b : bounds) totalLength += len(b);
        const double gap = (lo(bounds.last()) + len(bounds.last()) - lo(bounds.first())
                          - totalLength) / (items.size() - 1);
        double next = lo(bounds.first()) + len(bounds.first()) + gap;
        for (int i = 1; i + 1 < items.size(); ++i) {
            items[i].goal = next;
            next += len(bounds[i]) + gap;
        }
    };
    const auto gapSpread = [&]() {
        const QVector<QRectF> bounds = currentBounds();
        double minGap = std::numeric_limits<double>::infinity(), maxGap = -minGap;
        for (int i = 0; i + 1 < bounds.size(); ++i) {
            const double gap = lo(bounds[i + 1]) - (lo(bounds[i]) + len(bounds[i]));
            minGap = std::min(minGap, gap);
            maxGap = std::max(maxGap, gap);
        }
        return maxGap - minGap;
    };
    if (distribute) {
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
            return a.start < b.start;
        });
        items.first().goal = items.first().start;
        items.last().goal = items.last().start;
        layout();
    }
    bool changed = false;
    for (const auto &item : items)
        changed |= std::abs(edgeOf(boundsOf(item, item.original)) - item.goal) > 1e-9;
    if (!changed) return true;
    const TrackClipSnapshot before = snapshotTrackClips(this);
    // Solve ancestors first: moving a selected parent also moves its children, so
    // each child is solved against its parent's already-written world transform.
    const auto parentDepth = [&](const GroupClipTarget &target) {
        int depth = 0;
        QString key = trackMatteClipKey(target.trackIndex, target.clipIndex);
        QSet<QString> seen{key};
        while (depth <= 9) {
            const QString parent = m_clipParentEntries.value(key);
            if (parent.isEmpty() || seen.contains(parent)) break;
            seen.insert(parent);
            key = parent;
            ++depth;
        }
        return depth;
    };
    QVector<int> solveOrder;
    for (int i = 0; i < items.size(); ++i) solveOrder.append(i);
    std::stable_sort(solveOrder.begin(), solveOrder.end(), [&](int a, int b) {
        return parentDepth(items[a].target) < parentDepth(items[b].target);
    });
    // Each clip is solved on its rendered bounds. For a world-axis move s (px),
    // residual(s) = edgeOf(candidate moved by s) - goal. Plain clips are linear,
    // so the first step lands exactly. A position key can also change the
    // rotation and so the width (Auto Orient follows the path tangent), so the
    // solver keeps stepping (secant, then bisection once the root is bracketed).
    // A clip that does not come within kAcceptPx is left unchanged; the other
    // clips are still applied as one undo entry and a status message gives the
    // count. Return value: true when something was written or every target is
    // already in place (callers refresh the preview on it); false when nothing
    // was written because targets could not be placed (no undo entry).
    constexpr double kAcceptPx = 0.5;
    constexpr double kSolvedPx = 1e-6;
    constexpr int kMaxSolveSteps = 80;
    // A keyframed axis gets its playhead value set (or a key added there); writing
    // only the static value would leave the rendered frame unchanged. A key within
    // addKeyframe's 1 ms merge window is moved to exactly the playhead, since the
    // evaluator only returns a key's own value at its exact time.
    const auto moveAxis = [](ClipInfo &clip, double localSec, bool xAxis, double current,
                             double delta) {
        if (std::abs(delta) <= 1e-12) return;
        const QString runtimeName = xAxis ? QStringLiteral("motion.position.x")
                                          : QStringLiteral("motion.position.y");
        const QString publicName = xAxis ? QStringLiteral("positionX")
                                         : QStringLiteral("positionY");
        const auto hasKeys = [&](const QString &name) {
            const KeyframeTrack *t = clip.keyframes.track(name);
            return t && t->count() > 0;
        };
        KeyframeTrack *keys = hasKeys(runtimeName) ? clip.keyframes.track(runtimeName)
                            : hasKeys(publicName) ? clip.keyframes.track(publicName)
                                                  : nullptr;
        if (!keys) {
            (xAxis ? clip.videoDx : clip.videoDy) += delta;
            return;
        }
        for (int i = 0; i < keys->count(); ++i) {
            KeyframePoint point = keys->keyframes()[i];
            if (std::abs(point.time - localSec) < 0.001) {
                point.time = localSec;
                point.value = current + delta;
                keys->setKeyframePoint(i, point);
                return;
            }
        }
        keys->addKeyframe(localSec, current + delta);
    };
    const auto solveItem = [&](Item &item) {
        auto *track = m_videoTracks[item.target.trackIndex];
        const ClipInfo base = clipOf(item);
        // Re-evaluate against the current (possibly just moved) parent chain.
        const clipgeom::ClipTransform local = clipanim::effectiveTransformAt(base, item.renderLocalSec);
        clipgeom::ClipTransform parentWorld;
        QVector<QString> stack;
        const bool parented = resolveParent(item.target.trackIndex, item.target.clipIndex, 0,
                                            stack, &parentWorld) > 0;
        // Child-local displacement per world pixel along the arrangement axis.
        double ux = horizontal ? 1.0 / canvas.width() : 0.0;
        double uy = horizontal ? 0.0 : 1.0 / canvas.height();
        if (parented) {
            // Undo the parent's rotation and scale to move in child-local space.
            const double scale = parentWorld.videoScale;
            const double radians = qDegreesToRadians(parentWorld.rotationDeg);
            const double wx = horizontal ? 1.0 : 0.0;
            const double wy = horizontal ? 0.0 : 1.0;
            ux = (wx * std::cos(radians) + wy * std::sin(radians)) / scale / canvas.width();
            uy = (-wx * std::sin(radians) + wy * std::cos(radians)) / scale / canvas.height();
        }
        const auto candidate = [&](double s) {
            ClipInfo clip = base;
            if (s == 0.0) return clip;   // ux/uy are non-finite under a zero-scale parent
            moveAxis(clip, item.renderLocalSec, true, local.videoDx, s * ux);
            moveAxis(clip, item.renderLocalSec, false, local.videoDy, s * uy);
            return clip;
        };
        const auto residual = [&](double s) {
            clipgeom::ClipTransform transform =
                clipanim::effectiveTransformAt(candidate(s), item.renderLocalSec);
            if (parented) transform = clipgeom::composeParented(transform, parentWorld, canvas);
            return edgeOf(clipgeom::resolveTransform(transform, canvas)
                              .mapRect(QRectF(QPointF(0, 0), QSizeF(canvas)))) - item.goal;
        };
        const double g0 = residual(0.0);
        if (std::isfinite(g0) && std::abs(g0) <= 1e-9) return true;
        double bestS = 0.0, bestG = g0;
        if (std::isfinite(g0) && std::isfinite(ux) && std::isfinite(uy)) {
            double sPrev = 0.0, gPrev = g0;
            double s = -g0, g = residual(s);
            double bracketLo = 0.0, bracketHi = 0.0, gLo = 0.0;
            bool bracketed = false;
            const auto opposite = [](double a, double b) { return (a < 0.0) != (b < 0.0); };
            if (std::isfinite(g) && opposite(g, gPrev)) {
                bracketed = true;
                bracketLo = sPrev; gLo = gPrev; bracketHi = s;
            }
            for (int step = 0; std::isfinite(g); ++step) {
                if (std::abs(g) < std::abs(bestG)) { bestS = s; bestG = g; }
                if (std::abs(bestG) <= kSolvedPx || step >= kMaxSolveSteps) break;
                double next = 0.5 * (bracketLo + bracketHi);
                if (!bracketed) {
                    const double slope = (g - gPrev) / (s - sPrev);
                    next = std::isfinite(slope) && std::abs(slope) > 1e-9
                        ? s - g / slope : s + 2.0 * (s - sPrev);
                }
                const double gNext = residual(next);
                if (!std::isfinite(gNext)) break;
                if (bracketed) {
                    if (opposite(gNext, gLo)) bracketHi = next;
                    else { bracketLo = next; gLo = gNext; }
                } else if (opposite(gNext, g)) {
                    bracketed = true;
                    bracketLo = s; gLo = g; bracketHi = next;
                }
                sPrev = s; gPrev = g;
                s = next; g = gNext;
            }
        }
        if (!std::isfinite(bestG) || std::abs(bestG) > kAcceptPx) return false;
        // Within tolerance without moving: nothing to write.
        if (bestS == 0.0) return true;
        auto clips = track->clips();
        clips[item.target.clipIndex] = candidate(bestS);
        track->setClips(clips);
        item.wrote = true;
        return true;
    };
    int unresolved = 0;
    if (!distribute) {
        for (int i : solveOrder)
            if (!solveItem(items[i])) ++unresolved;
    } else {
        // A key edit can change a middle clip's width (Auto Orient), which shifts
        // the equal-gap layout itself: re-layout with the new widths and solve
        // again, up to kMaxLayoutPasses. If the gaps still differ by more than
        // kMaxGapSpreadPx, every selected clip is put back and reported.
        constexpr int kMaxLayoutPasses = 6;
        constexpr double kMaxGapSpreadPx = 1.0;
        double spread = std::numeric_limits<double>::infinity();
        for (int pass = 0; pass < kMaxLayoutPasses; ++pass) {
            if (pass > 0) layout();
            for (int i : solveOrder)
                solveItem(items[i]);
            spread = gapSpread();
            if (spread <= 0.01) break;
        }
        if (!std::isfinite(spread) || spread > kMaxGapSpreadPx) {
            for (auto &item : items) {
                auto *track = m_videoTracks[item.target.trackIndex];
                auto clips = track->clips();
                clips[item.target.clipIndex] = item.original;
                track->setClips(clips);
                item.wrote = false;
                ++unresolved;
            }
        }
    }
    bool wrote = false;
    for (const auto &item : items) wrote |= item.wrote;
    if (unresolved > 0)
        emit statusMessageRequested(
            QStringLiteral("%1 個のクリップは整列位置に合わせられないため変更しませんでした。")
                .arg(unresolved), 5000);
    if (!wrote) return unresolved == 0;
    remapTrackMatteLocked(before);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, before);
    saveUndoState(distribute ? QStringLiteral("均等配置") : QStringLiteral("整列"));
    refreshPlaybackSequence();
    return true;
}

bool Timeline::canGroupClips(const QVector<GroupClipTarget> &clips, QString *error) const
{
    auto reject = [&](const QString &message) { if (error) *error = message; return false; };
    if (clips.size() < 2)
        return reject(QStringLiteral("別々のトラックから 2 個以上のクリップを選択してください。"));
    QSet<TimelineTrack *> seen;
    for (const auto &target : clips) {
        auto *track = trackAt(target.audio, target.trackIndex);
        if (!track || target.clipIndex < 0 || target.clipIndex >= track->clipCount())
            return reject(QStringLiteral("クリップの位置が無効です。"));
        if (track->isLocked()) return reject(QStringLiteral("ロック中のトラックは変更できません。"));
        if (seen.contains(track))
            return reject(QStringLiteral("同一トラックの複数クリップはグループ化できません。"));
        seen.insert(track);
    }
    return true;
}

bool Timeline::setClipGrouping(const QVector<GroupClipTarget> &clips, bool grouped, QString *error)
{
    auto reject = [&](const QString &message) { if (error) *error = message; return false; };
    if (grouped && !canGroupClips(clips, error)) return false;
    if (clips.isEmpty()) return reject(QStringLiteral("クリップを選択してください。"));
    bool changed = grouped;
    for (const auto &target : clips) {
        auto *track = trackAt(target.audio, target.trackIndex);
        if (!track || target.clipIndex < 0 || target.clipIndex >= track->clipCount())
            return reject(QStringLiteral("クリップの位置が無効です。"));
        if (track->isLocked())
            return reject(QStringLiteral("ロック中のトラックは変更できません。"));
        changed = changed || track->clips()[target.clipIndex].linkGroup != 0;
    }
    if (!changed) return true;
    TrackClipSnapshot before = snapshotTrackClips(this);
    int group = 0;
    if (grouped) {
        QSet<int> used;
        for (const auto *track : m_videoTracks)
            for (const auto &clip : track->clips()) used.insert(clip.linkGroup);
        for (const auto *track : m_audioTracks)
            for (const auto &clip : track->clips()) used.insert(clip.linkGroup);
        do { group = allocateLinkGroup(); } while (used.contains(group));
    }
    for (const auto &target : clips) {
        auto *track = trackAt(target.audio, target.trackIndex);
        auto updated = track->clips();
        updated[target.clipIndex].linkGroup = group;
        track->setClips(updated);
        // Group identity changes, but indices do not. Teach the remapper the
        // new identity at the same slot so parent/matte entries survive.
        if (!target.audio) before[target.trackIndex][target.clipIndex].linkGroup = group;
    }
    remapTrackMatteLocked(before);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, before);
    saveUndoState(grouped ? QStringLiteral("グループ化") : QStringLiteral("グループ解除"));
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::deleteSelectedClip(bool leaveGap)
{
    // RM-5: snapshot carrier keys BEFORE the delete so we can remap after.
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    bool anyRemoved = false;
    bool v1Removed = false;
    // Walk every track and remove its selected clips (descending so indices
    // stay valid). Preserve downstream positions — we want the rest of the
    // sequence to stay put when a linked V/A pair is deleted out of the
    // middle of the timeline.
    auto deleteFrom = [&](TimelineTrack *t) {
        if (!t) return;
        QList<int> sel = t->selectedClips();
        if (sel.isEmpty()) return;
        std::sort(sel.begin(), sel.end(), std::greater<int>());
        for (int idx : sel) {
            // Anchors live only on V1; a linked A clip may share the V1 uid.
            if (t == m_videoTracks.value(0) && idx >= 0 && idx < t->clipCount()) {
                m_pendingDeletedUids[t->clips().at(idx).uid].append(
                    {0.0, t->clips().at(idx).effectiveDuration()});
                v1Removed = true;
            }
            t->removeClipPreservingDownstream(idx);
            anyRemoved = true;
        }
    };
    for (auto *t : m_videoTracks) deleteFrom(t);
    for (auto *t : m_audioTracks) deleteFrom(t);
    if (anyRemoved) {
        // US-1203: storyline mode ripples V1 only, and only when a V1 clip was
        // removed (the removed clips' linked audio went with the selection; the
        // rest follows in reconcile).
        if (m_storylineMode && !leaveGap && v1Removed)
            closeGapsOnTrack(m_videoTracks.value(0, nullptr));
        remapTrackMatteLocked(snapBefore);
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
        saveUndoState("Delete clip");
        updateInfoLabel();
    }
}

namespace {

bool trackClipTimeRangeAt(const TimelineTrack *track, int clipIndex,
                          double *outStart, double *outEnd)
{
    if (!track || clipIndex < 0 || clipIndex >= track->clipCount())
        return false;
    const auto &clips = track->clips();
    double cursor = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        const double clipStart = cursor + clips[i].leadInSec;
        const double clipEnd = clipStart + clips[i].effectiveDuration();
        if (i == clipIndex) {
            if (outStart) *outStart = clipStart;
            if (outEnd) *outEnd = clipEnd;
            return clipEnd > clipStart;
        }
        cursor = clipEnd;
    }
    return false;
}

bool trackRangeOverlapsClip(const TimelineTrack *track, double startSec, double endSec)
{
    if (!track)
        return false;
    constexpr double kEps = 1e-6;
    const auto &clips = track->clips();
    double cursor = 0.0;
    for (const ClipInfo &clip : clips) {
        const double clipStart = cursor + clip.leadInSec;
        const double clipEnd = clipStart + clip.effectiveDuration();
        if (clipStart < endSec - kEps && clipEnd > startSec + kEps)
            return true;
        cursor = clipEnd;
    }
    return false;
}

} // namespace

namespace {

bool setTimelineEditError(QString *err, const QString &message)
{
    if (err)
        *err = message;
    return false;
}

struct IndexEditTarget {
    TimelineTrack *track = nullptr;
    int clipIndex = -1;
    double startSec = 0.0;
    double endSec = 0.0;
    double localSplitSec = 0.0;
};

QVector<TimelineTrack *> timelineTracks(const Timeline *timeline)
{
    QVector<TimelineTrack *> tracks;
    if (!timeline)
        return tracks;
    tracks.reserve(timeline->videoTracks().size() + timeline->audioTracks().size());
    for (TimelineTrack *track : timeline->videoTracks())
        tracks.append(track);
    for (TimelineTrack *track : timeline->audioTracks())
        tracks.append(track);
    return tracks;
}

} // namespace

void Timeline::selectClipsInRange(double startSec, double endSec, int firstRow,
                                  int lastRow, bool additive)
{
    if (!std::isfinite(startSec) || !std::isfinite(endSec)) return;
    if (startSec > endSec) std::swap(startSec, endSec);
    if (firstRow > lastRow) std::swap(firstRow, lastRow);
    const auto tracks = timelineTracks(this);
    QSet<int> groups;
    QVector<QSet<int>> selected(tracks.size());
    for (int row = 0; row < tracks.size(); ++row) {
        auto *track = tracks[row];
        if (track->isLocked() || track->isHidden()) continue;
        const int x0 = track->secondsToX(startSec);
        const int x1 = track->secondsToX(endSec);
        for (int i = 0; i < track->clipCount(); ++i) {
            // Match painting and clipAtX, including the minimum visible clip width.
            const int cx = track->clipStartX(i);
            const int cw = qMax(20, static_cast<int>(
                track->clips()[i].effectiveDuration() * track->pixelsPerSecond()));
            const bool hit = row >= firstRow && row <= lastRow
                && cx < x1 && cx + cw > x0;
            if (hit || (additive && track->isClipSelected(i))) {
                selected[row].insert(i);
                if (track->clips()[i].linkGroup > 0)
                    groups.insert(track->clips()[i].linkGroup);
            }
        }
    }
    int primary = -1;
    m_activeVideoTrackIndex = -1;
    for (int row = 0; row < tracks.size(); ++row) {
        auto *track = tracks[row];
        const QSignalBlocker blocker(track);
        track->clearClipSelection();
        if (track->isLocked() || track->isHidden()) continue;
        for (int i = 0; i < track->clipCount(); ++i) {
            if (!selected[row].contains(i) && !groups.contains(track->clips()[i].linkGroup)) continue;
            track->toggleClipSelection(i);
            const int videoIndex = m_videoTracks.indexOf(track);
            if (primary < 0 || videoIndex >= 0) {
                primary = i;
                m_activeVideoTrackIndex = videoIndex;
            }
        }
    }
    emit clipSelected(primary);
    emit clipSelectedOnTrack(m_activeVideoTrackIndex, primary);
}

bool Timeline::splitClipByIndex(bool audio, int trackIndex, int clipIndex,
                                double timelineSeconds, QString *err)
{
    if (!std::isfinite(timelineSeconds) || timelineSeconds < 0.0)
        return setTimelineEditError(err, QStringLiteral("timelineSeconds must be non-negative"));

    TimelineTrack *selectedTrack = trackAt(audio, trackIndex);
    if (!selectedTrack)
        return setTimelineEditError(err, QStringLiteral("track index is out of range"));
    if (clipIndex < 0 || clipIndex >= selectedTrack->clipCount())
        return setTimelineEditError(err, QStringLiteral("clip index is out of range"));
    if (selectedTrack->isLocked())
        return setTimelineEditError(err, QStringLiteral("track is locked"));

    double selectedStart = 0.0;
    double selectedEnd = 0.0;
    if (!trackClipTimeRangeAt(selectedTrack, clipIndex, &selectedStart, &selectedEnd)
        || !std::isfinite(selectedStart) || !std::isfinite(selectedEnd)) {
        return setTimelineEditError(err, QStringLiteral("clip has no valid duration"));
    }
    // Keep the same safety margin as splitAtPlayhead().
    if (timelineSeconds <= selectedStart + 0.05
        || timelineSeconds >= selectedEnd - 0.05) {
        return setTimelineEditError(err, QStringLiteral("split point is outside the clip"));
    }

    const int oldGroup = selectedTrack->clips().at(clipIndex).linkGroup;
    const QVector<TimelineTrack *> tracks = timelineTracks(this);
    QVector<IndexEditTarget> targets;
    if (oldGroup <= 0) {
        targets.append(IndexEditTarget{selectedTrack, clipIndex, selectedStart,
                                       selectedEnd, timelineSeconds - selectedStart});
    } else {
        for (TimelineTrack *track : tracks) {
            if (!track)
                continue;
            for (int i = 0; i < track->clipCount(); ++i) {
                if (track->clips().at(i).linkGroup != oldGroup)
                    continue;
                double start = 0.0;
                double end = 0.0;
                if (!trackClipTimeRangeAt(track, i, &start, &end)
                    || !std::isfinite(start) || !std::isfinite(end)
                    || timelineSeconds <= start + 0.05
                    || timelineSeconds >= end - 0.05) {
                    return setTimelineEditError(
                        err, QStringLiteral("split point is outside the clip"));
                }
                if (track->isLocked())
                    return setTimelineEditError(err, QStringLiteral("track is locked"));
                targets.append(IndexEditTarget{track, i, start, end,
                                               timelineSeconds - start});
            }
        }
    }

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    const int newGroup = oldGroup > 0 ? allocateLinkGroup() : 0;
    for (TimelineTrack *track : tracks) {
        if (!track)
            continue;
        QVector<IndexEditTarget> trackTargets;
        for (const IndexEditTarget &target : targets) {
            if (target.track == track)
                trackTargets.append(target);
        }
        std::sort(trackTargets.begin(), trackTargets.end(),
                  [](const IndexEditTarget &a, const IndexEditTarget &b) {
                      return a.clipIndex > b.clipIndex;
                  });
        for (const IndexEditTarget &target : trackTargets) {
            track->splitClipAt(target.clipIndex, target.localSplitSec, false);
            if (newGroup > 0) {
                QVector<ClipInfo> clips = track->clips();
                clips[target.clipIndex + 1].linkGroup = newGroup;
                track->setClips(clips);
            }
        }
        if (!trackTargets.isEmpty())
            track->setClips(track->clips());
    }

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("Split clip (MCP)"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::deleteClipByIndex(bool audio, int trackIndex, int clipIndex,
                                 bool ripple, QString *err)
{
    TimelineTrack *selectedTrack = trackAt(audio, trackIndex);
    if (!selectedTrack)
        return setTimelineEditError(err, QStringLiteral("track index is out of range"));
    if (clipIndex < 0 || clipIndex >= selectedTrack->clipCount())
        return setTimelineEditError(err, QStringLiteral("clip index is out of range"));
    if (selectedTrack->isLocked())
        return setTimelineEditError(err, QStringLiteral("track is locked"));

    const int linkGroup = selectedTrack->clips().at(clipIndex).linkGroup;
    const QVector<TimelineTrack *> tracks = timelineTracks(this);
    QVector<IndexEditTarget> targets;
    for (TimelineTrack *track : tracks) {
        if (!track)
            continue;
        for (int i = 0; i < track->clipCount(); ++i) {
            if ((linkGroup > 0 && track->clips().at(i).linkGroup != linkGroup)
                || (linkGroup <= 0 && (track != selectedTrack || i != clipIndex))) {
                continue;
            }
            double start = 0.0;
            double end = 0.0;
            if (!trackClipTimeRangeAt(track, i, &start, &end))
                return setTimelineEditError(err, QStringLiteral("clip has no valid duration"));
            if (track->isLocked())
                return setTimelineEditError(err, QStringLiteral("track is locked"));
            targets.append(IndexEditTarget{track, i, start, end, 0.0});
        }
    }

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    for (const IndexEditTarget &target : targets) {
        // Anchors live only on V1; a linked A clip may share the V1 uid.
        if (target.track == m_videoTracks.value(0))
            m_pendingDeletedUids[target.track->clips().at(target.clipIndex).uid].append(
                {0.0, target.endSec - target.startSec});
    }
    bool changed = false;
    for (TimelineTrack *track : tracks) {
        if (!track)
            continue;
        QVector<IndexEditTarget> trackTargets;
        for (const IndexEditTarget &target : targets) {
            if (target.track == track)
                trackTargets.append(target);
        }
        std::sort(trackTargets.begin(), trackTargets.end(),
                  [](const IndexEditTarget &a, const IndexEditTarget &b) {
                      return a.clipIndex > b.clipIndex;
                  });
        for (const IndexEditTarget &target : trackTargets) {
            if (ripple)
                changed = track->rippleDeleteTimeRange(target.startSec,
                                                       target.endSec, false)
                    || changed;
            else {
                track->removeClipPreservingDownstream(target.clipIndex, false);
                changed = true;
            }
        }
        if (!trackTargets.isEmpty())
            track->setClips(track->clips());
    }

    if (!changed)
        return setTimelineEditError(err, QStringLiteral("clip could not be deleted"));
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("Delete clip (MCP)"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::moveClipByIndex(bool audio, int trackIndex, int clipIndex,
                               double newStartSec, double *settledStartSec,
                               QString *err)
{
    MoveClipResult result;
    const bool valid = moveClipByIndex(audio, trackIndex, clipIndex, newStartSec,
                                       trackIndex, &result, err);
    if (settledStartSec)
        *settledStartSec = result.actualStartSec;
    return valid;
}

bool Timeline::moveClipByIndex(bool audio, int trackIndex, int clipIndex,
                               double newStartSec, int newTrackIndex,
                               MoveClipResult *result, QString *err)
{
    MoveClipResult localResult;
    MoveClipResult &moveResult = result ? *result : localResult;
    moveResult = MoveClipResult{};

    if (!std::isfinite(newStartSec) || newStartSec < 0.0)
        return setTimelineEditError(err, QStringLiteral("newStartSec must be non-negative"));

    TimelineTrack *sourceTrack = trackAt(audio, trackIndex);
    if (!sourceTrack)
        return setTimelineEditError(err, QStringLiteral("track index is out of range"));
    if (clipIndex < 0 || clipIndex >= sourceTrack->clipCount())
        return setTimelineEditError(err, QStringLiteral("clip index is out of range"));
    TimelineTrack *destinationTrack = trackAt(audio, newTrackIndex);
    if (!destinationTrack)
        return setTimelineEditError(err, QStringLiteral("newTrackIndex is out of range"));
    if (sourceTrack->isLocked() || destinationTrack->isLocked())
        return setTimelineEditError(err, QStringLiteral("track is locked"));
    struct MovingMember {
        TimelineTrack *sourceTrack = nullptr;
        TimelineTrack *destinationTrack = nullptr;
        int sourceIndex = -1;
        int insertedIndex = -1;
        ClipInfo clip;
        double originalStartSec = 0.0;
        double desiredStartSec = 0.0;
        double actualStartSec = 0.0;
    };

    const QVector<TimelineTrack *> tracks = timelineTracks(this);
    const auto sameClipIdentity = [](const ClipInfo &left, const ClipInfo &right) {
        return left.filePath == right.filePath
            && left.linkGroup == right.linkGroup
            && qFuzzyCompare(left.inPoint + 1.0, right.inPoint + 1.0);
    };

    struct SelectionReference {
        TimelineTrack *track = nullptr;
        int clipIndex = -1;
        ClipInfo clip;
        int occurrence = 0;
    };
    SelectionReference selection;
    // 並べ替えで index が変わっても、移動対象でない選択を同じクリップへ
    // 戻せるように、GUI が保持する主選択を移動前に記録する。
    for (TimelineTrack *track : tracks) {
        if (!track)
            continue;
        const int selectedIndex = track->selectedClip();
        if (selectedIndex < 0 || selectedIndex >= track->clipCount())
            continue;
        selection.track = track;
        selection.clipIndex = selectedIndex;
        selection.clip = track->clips().at(selectedIndex);
        for (int i = 0; i < selectedIndex; ++i) {
            if (sameClipIdentity(track->clips().at(i), selection.clip))
                ++selection.occurrence;
        }
        break;
    }

    const int linkGroup = sourceTrack->clips().at(clipIndex).linkGroup;
    QVector<MovingMember> members;
    for (TimelineTrack *track : tracks) {
        if (!track)
            continue;
        for (int index = 0; index < track->clipCount(); ++index) {
            const bool isSelectedClip = track == sourceTrack && index == clipIndex;
            const bool isLinkedClip = linkGroup > 0
                && track->clips().at(index).linkGroup == linkGroup;
            if (!isSelectedClip && !isLinkedClip)
                continue;

            double startSec = 0.0;
            double endSec = 0.0;
            if (!trackClipTimeRangeAt(track, index, &startSec, &endSec)
                || !std::isfinite(startSec) || !std::isfinite(endSec)) {
                return setTimelineEditError(err, QStringLiteral("clip has no valid duration"));
            }
            if (track->isLocked())
                return setTimelineEditError(err, QStringLiteral("track is locked"));
            members.append(MovingMember{
                track, nullptr, index, -1, track->clips().at(index),
                startSec, newStartSec + (startSec - 0.0), 0.0
            });
        }
    }
    if (members.isEmpty())
        return setTimelineEditError(err, QStringLiteral("clip could not be found"));

    // リンク済み V/A は同じトラック番号へ移す。片方のトラックがまだ無い
    // 場合は GUI のクロストラック移動と同じく空の相方トラックを作る。
    bool needsOppositeTrack = false;
    for (const MovingMember &member : members) {
        const bool sameKind = member.sourceTrack->isAudioTrack() == audio;
        needsOppositeTrack = needsOppositeTrack || !sameKind;
    }
    TimelineTrack *oppositeDestination = nullptr;
    bool createOppositeTrack = false;
    if (needsOppositeTrack && !m_nudgeBatchActive) {
        oppositeDestination = trackAt(!audio, newTrackIndex);
        createOppositeTrack = !oppositeDestination;
        if (oppositeDestination && oppositeDestination->isLocked())
            return setTimelineEditError(err, QStringLiteral("track is locked"));
    }

    int selectedMemberIndex = -1;
    for (int i = 0; i < members.size(); ++i) {
        MovingMember &member = members[i];
        member.destinationTrack = m_nudgeBatchActive ? member.sourceTrack
            : (member.sourceTrack->isAudioTrack() == audio ? destinationTrack : oppositeDestination);
        if (member.sourceTrack == sourceTrack && member.sourceIndex == clipIndex)
            selectedMemberIndex = i;
    }
    if (selectedMemberIndex < 0)
        return setTimelineEditError(err, QStringLiteral("clip could not be found"));
    // members[0] is normally the selected clip, but do not rely on track order
    // when calculating the linked V/A offset.
    const double selectedOriginalStart = members[selectedMemberIndex].originalStartSec;
    for (MovingMember &member : members)
        member.desiredStartSec = newStartSec
            + (member.originalStartSec - selectedOriginalStart);

    // 対象トラックも開始時刻も変わらない要求は、後続クリップを一旦外した
    // シミュレーションで誤って「重なり」と判定しない。特に先頭クリップに
    // leadInSec がある場合、対象を除くと次のクリップが左へ詰まるためである。
    bool staysOnOriginalTracks = true;
    for (const MovingMember &member : members) {
        if (member.sourceTrack != member.destinationTrack
            || qAbs(member.desiredStartSec - member.originalStartSec) > 1e-6) {
            staysOnOriginalTracks = false;
            break;
        }
    }
    if (staysOnOriginalTracks) {
        moveResult.actualStartSec = selectedOriginalStart;
        moveResult.trackIndex = newTrackIndex;
        moveResult.clipIndex = clipIndex;
        return true;
    }

    struct PendingTrack {
        TimelineTrack *track = nullptr;
        QVector<ClipInfo> clips;
        QVector<int> memberIndices;
    };
    QVector<PendingTrack> pending;
    auto ensurePending = [&pending](TimelineTrack *track) -> int {
        for (int i = 0; i < pending.size(); ++i) {
            if (pending[i].track == track)
                return i;
        }
        pending.append(PendingTrack{track, track ? track->clips() : QVector<ClipInfo>{},
                                    QVector<int>{}});
        return static_cast<int>(pending.size() - 1);
    };
    for (int i = 0; i < members.size(); ++i) {
        const int sourcePlan = ensurePending(members[i].sourceTrack);
        const int destinationPlan = ensurePending(members[i].destinationTrack);
        pending[sourcePlan].memberIndices.append(i);
        if (destinationPlan != sourcePlan)
            pending[destinationPlan].memberIndices.append(i);
    }

    // まず対象を外して残りを詰め、その配列へ要求時刻順に再挿入する。
    // ここではまだ実トラックを書き換えないので、途中で重なりを検出しても
    // タイムラインを変更せずに actualStartSec を計算できる。
    for (PendingTrack &plan : pending) {
        QVector<char> removed(plan.clips.size(), 0);
        for (int memberIndex : plan.memberIndices) {
            const MovingMember &member = members[memberIndex];
            if (member.sourceTrack == plan.track
                && member.sourceIndex >= 0 && member.sourceIndex < removed.size()) {
                removed[member.sourceIndex] = 1;
            }
        }
        QVector<ClipInfo> remaining;
        remaining.reserve(plan.clips.size());
        double removedSpan = 0.0;
        for (int i = 0; i < plan.clips.size(); ++i) {
            if (!removed[i]) {
                ClipInfo clip = plan.clips.at(i);
                if (m_nudgeBatchActive) clip.leadInSec += removedSpan;
                remaining.append(clip);
                removedSpan = 0.0;
            } else {
                removedSpan += plan.clips.at(i).leadInSec + plan.clips.at(i).effectiveDuration();
            }
        }
        plan.clips = remaining;
        std::sort(plan.memberIndices.begin(), plan.memberIndices.end(),
                  [&members](int left, int right) {
                      return members[left].desiredStartSec
                          < members[right].desiredStartSec;
                  });
    }

    auto nearestAvailableStart = [this](const QVector<ClipInfo> &clips,
                                    double duration, double desired) {
        constexpr double kEps = 1e-6;
        double cursor = 0.0;
        double best = qMax(0.0, desired);
        double bestDistance = std::numeric_limits<double>::infinity();
        auto consider = [&](double lower, double upper) {
            if (upper < lower - kEps)
                return;
            const double candidate = qBound(lower, desired, upper);
            const double distance = qAbs(candidate - desired);
            if (distance < bestDistance) {
                best = candidate;
                bestDistance = distance;
            }
        };
        for (const ClipInfo &clip : clips) {
            const double clipStart = cursor + qMax(0.0, clip.leadInSec);
            const double clipDuration = qMax(0.0, clip.effectiveDuration());
            // 境界への挿入は後続を右へ押し出す並べ替えとして許可する。
            if (!m_nudgeBatchActive) consider(clipStart, clipStart);
            consider(cursor, clipStart - duration);
            cursor = clipStart + clipDuration;
        }
        const double tailCandidate = qMax(cursor, desired);
        if (qAbs(tailCandidate - desired) < bestDistance)
            best = tailCandidate;
        return best;
    };

    auto insertAtRequestedTime = [this](PendingTrack &plan, MovingMember &member) {
        constexpr double kEps = 1e-6;
        const double duration = member.clip.effectiveDuration();
        double cursor = 0.0;
        for (int i = 0; i < plan.clips.size(); ++i) {
            const double clipStart = cursor + qMax(0.0, plan.clips[i].leadInSec);
            const double clipEnd = clipStart + qMax(0.0, plan.clips[i].effectiveDuration());
            // 連続配置の境界は空き時間が無くても挿入位置として有効にする。
            // 現在のクリップを一つ右へ押し出すことで、A/B/C の A を時刻 5
            // に置くような並べ替えを可能にする。
            if (!m_nudgeBatchActive && qAbs(member.desiredStartSec - clipStart) <= kEps) {
                ClipInfo inserted = member.clip;
                inserted.leadInSec = qMax(0.0, member.desiredStartSec - cursor);
                plan.clips.insert(i, inserted);
                member.insertedIndex = i;
                member.actualStartSec = cursor + inserted.leadInSec;
                return true;
            }
            if (member.desiredStartSec + duration <= clipStart + kEps) {
                ClipInfo inserted = member.clip;
                inserted.leadInSec = qMax(0.0, member.desiredStartSec - cursor);
                const double consumed = inserted.leadInSec + duration;
                plan.clips.insert(i, inserted);
                if (i + 1 < plan.clips.size())
                    plan.clips[i + 1].leadInSec = qMax(
                        0.0, plan.clips[i + 1].leadInSec - consumed);
                member.insertedIndex = i;
                member.actualStartSec = cursor + inserted.leadInSec;
                return true;
            }
            if (member.desiredStartSec < clipEnd - kEps)
                return false;
            cursor = clipEnd;
        }

        ClipInfo inserted = member.clip;
        inserted.leadInSec = qMax(0.0, member.desiredStartSec - cursor);
        member.insertedIndex = plan.clips.size();
        member.actualStartSec = cursor + inserted.leadInSec;
        plan.clips.append(inserted);
        return true;
    };

    for (PendingTrack &plan : pending) {
        const QVector<int> planMembers = plan.memberIndices;
        for (int memberIndex : planMembers) {
            MovingMember &member = members[memberIndex];
            if (member.destinationTrack != plan.track)
                continue;
            if (!insertAtRequestedTime(plan, member)) {
                const double nearest = nearestAvailableStart(
                    plan.clips, qMax(0.0, member.clip.effectiveDuration()),
                    member.desiredStartSec);
                const double correction = nearest - member.desiredStartSec;
                moveResult.actualStartSec = qMax(0.0, newStartSec + correction);
                moveResult.trackIndex = newTrackIndex;
                moveResult.clipIndex = -1;
                moveResult.reason = QStringLiteral("指定位置は他のクリップと重なっています");
                return true;
            }
        }
    }

    MovingMember &selectedMember = members[selectedMemberIndex];
    moveResult.actualStartSec = selectedMember.actualStartSec;
    moveResult.trackIndex = newTrackIndex;
    moveResult.clipIndex = selectedMember.insertedIndex;

    bool changed = false;
    for (const MovingMember &member : members) {
        changed = changed || member.sourceTrack != member.destinationTrack
            || member.insertedIndex != member.sourceIndex
            || qAbs(member.actualStartSec - member.originalStartSec) > 1e-6;
    }
    if (!changed)
        return true;

    // 既存規約のスナップショットは、相方トラックを新設する場合も含めて
    // 実トラックを変更する直前に取得する。
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    if (createOppositeTrack) {
        // 相方トラックは要求位置のシミュレーションが成功した後で作る。
        // 先に作ると、隣接クリップとの重なりで ok:false を返す場合にも
        // 空トラックだけが残ってしまう。
        if (audio) {
            while (m_videoTracks.size() <= newTrackIndex)
                addVideoTrack(false);
            oppositeDestination = trackAt(false, newTrackIndex);
        } else {
            while (m_audioTracks.size() <= newTrackIndex)
                addAudioTrack(false);
            oppositeDestination = trackAt(true, newTrackIndex);
        }
        if (!oppositeDestination)
            return setTimelineEditError(err, QStringLiteral("リンク先のトラックを準備できません"));
        for (MovingMember &member : members) {
            if (!member.destinationTrack)
                member.destinationTrack = oppositeDestination;
        }
        for (PendingTrack &plan : pending) {
            if (!plan.track)
                plan.track = oppositeDestination;
        }
    }

    TimelineTrack *selectionTrack = nullptr;
    int selectionIndex = -1;
    if (selection.track) {
        for (const MovingMember &member : members) {
            if (member.sourceTrack == selection.track
                && member.sourceIndex == selection.clipIndex) {
                selectionTrack = member.destinationTrack;
                selectionIndex = member.insertedIndex;
                break;
            }
        }
        if (!selectionTrack) {
            for (const PendingTrack &plan : pending) {
                if (plan.track != selection.track)
                    continue;
                int occurrence = 0;
                for (int i = 0; i < plan.clips.size(); ++i) {
                    if (!sameClipIdentity(plan.clips.at(i), selection.clip))
                        continue;
                    if (occurrence == selection.occurrence) {
                        selectionTrack = plan.track;
                        selectionIndex = i;
                        break;
                    }
                    ++occurrence;
                }
                break;
            }
        }
        if (!selectionTrack && selection.clipIndex < selection.track->clipCount()) {
            // このトラックが pending に含まれない場合は内容を変更していない。
            selectionTrack = selection.track;
            selectionIndex = selection.clipIndex;
        }
    }

    // 既存規約: snapshotTrackClips -> 変更 -> remap*AfterMutation -> saveUndoState。
    for (const PendingTrack &plan : pending)
        plan.track->setClips(plan.clips);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    if (!m_nudgeBatchActive && selectionTrack && selectionIndex >= 0
        && selectionIndex < selectionTrack->clipCount()) {
        // 選択中のクリップを動かした場合も、並べ替えで index だけ変わった
        // クリップの場合も、GUI と同じく対象を選択し直す。リンク済み V/A
        // は setSelectedClip の伝播に任せる。
        clearAllSelections();
        selectionTrack->setSelectedClip(selectionIndex);
    }
    if (!m_nudgeBatchActive) saveUndoState(QStringLiteral("Move clip (MCP)"));
    moveResult.moved = true;
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipPropertyByIndex(bool audio, int trackIndex, int clipIndex,
                                      const QString &property, double value,
                                      QString *err, bool applyToLinked)
{
    if (!std::isfinite(value))
        return setTimelineEditError(err, QStringLiteral("value must be a finite number"));

    double lower = 0.0;
    double upper = 0.0;
    if (property == QStringLiteral("volume")) {
        upper = 2.0;
    } else if (property == QStringLiteral("opacity")) {
        upper = 1.0;
    } else if (property == QStringLiteral("speed")) {
        lower = 0.25;
        upper = 4.0;
    } else if (property == QStringLiteral("pan")) {
        lower = -1.0;
        upper = 1.0;
    } else if (property == QStringLiteral("videoScale")) {
        lower = 0.1;
        upper = 10.0;
    } else {
        return setTimelineEditError(err,
                                    QStringLiteral("unsupported property: %1").arg(property));
    }
    if (value < lower || value > upper)
        return setTimelineEditError(err, QStringLiteral("%1 out of range [%2, %3]")
                                         .arg(property).arg(lower).arg(upper));

    TimelineTrack *track = trackAt(audio, trackIndex);
    if (!track)
        return setTimelineEditError(err, QStringLiteral("track index is out of range"));
    if (clipIndex < 0 || clipIndex >= track->clipCount())
        return setTimelineEditError(err, QStringLiteral("clip index is out of range"));
    if (track->isLocked())
        return setTimelineEditError(err, QStringLiteral("track is locked"));

    // Property edits ignore linkGroup by default: changing one side's volume
    // (or another whitelisted property) is a normal MCP use case. Callers opt
    // in with applyToLinked for properties that change the effective duration
    // (speed) so the linked V/A pair keeps the same length.
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    const auto assignProperty = [&property, value](ClipInfo &target) {
        if (property == QStringLiteral("volume"))
            target.volume = value;
        else if (property == QStringLiteral("opacity"))
            target.opacity = value;
        else if (property == QStringLiteral("speed"))
            target.speed = value;
        else if (property == QStringLiteral("pan"))
            target.pan = value;
        else
            target.videoScale = value;
    };
    QVector<ClipInfo> clips = track->clips();
    const int linkGroup = clips[clipIndex].linkGroup;
    assignProperty(clips[clipIndex]);
    track->setClips(clips);
    if (applyToLinked && linkGroup > 0) {
        const QVector<TimelineTrack *> partnerTracks = m_videoTracks + m_audioTracks;
        for (TimelineTrack *partner : partnerTracks) {
            if (!partner || partner == track || partner->isLocked())
                continue;
            QVector<ClipInfo> partnerClips = partner->clips();
            bool changed = false;
            for (ClipInfo &candidate : partnerClips) {
                if (candidate.linkGroup != linkGroup)
                    continue;
                assignProperty(candidate);
                changed = true;
            }
            if (changed)
                partner->setClips(partnerClips);
        }
    }

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("Set clip property (MCP)"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipReversed(TrackKind kind, int trackIndex, int clipIndex,
                               bool reversed, bool applyToLinked)
{
    TimelineTrack *track = trackAt(kind == TrackKind::Audio, trackIndex);
    if (!track || track->isLocked()
        || clipIndex < 0 || clipIndex >= track->clipCount()) {
        return false;
    }

    const QVector<ClipInfo> targetClips = track->clips();
    const int linkGroup = targetClips[clipIndex].linkGroup;
    QVector<TimelineTrack *> affectedTracks{track};
    if (applyToLinked && linkGroup > 0) {
        const QVector<TimelineTrack *> allTracks = m_videoTracks + m_audioTracks;
        for (TimelineTrack *candidateTrack : allTracks) {
            if (!candidateTrack || candidateTrack == track)
                continue;
            const auto &clips = candidateTrack->clips();
            const bool hasLinkedClip = std::any_of(
                clips.cbegin(), clips.cend(), [linkGroup](const ClipInfo &clip) {
                    return clip.linkGroup == linkGroup;
                });
            if (!hasLinkedClip)
                continue;
            if (candidateTrack->isLocked())
                return false;
            affectedTracks.append(candidateTrack);
        }
    }

    bool needsChange = false;
    for (TimelineTrack *affected : affectedTracks) {
        const auto &clips = affected->clips();
        for (int i = 0; i < clips.size(); ++i) {
            const bool isTarget = affected == track && i == clipIndex;
            const bool isLinked = applyToLinked && linkGroup > 0
                && clips[i].linkGroup == linkGroup;
            if ((isTarget || isLinked) && clips[i].reversed != reversed) {
                needsChange = true;
                break;
            }
        }
        if (needsChange)
            break;
    }
    if (!needsChange)
        return true;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    for (TimelineTrack *affected : affectedTracks) {
        QVector<ClipInfo> clips = affected->clips();
        for (int i = 0; i < clips.size(); ++i) {
            const bool isTarget = affected == track && i == clipIndex;
            const bool isLinked = applyToLinked && linkGroup > 0
                && clips[i].linkGroup == linkGroup;
            if (isTarget || isLinked)
                clips[i].reversed = reversed;
        }
        affected->setClips(clips);
    }

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("逆再生"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipAutoOrientEnabled(TrackKind kind, int trackIndex,
                                        int clipIndex, bool enabled)
{
    TimelineTrack *track = trackAt(kind == TrackKind::Audio, trackIndex);
    if (!track || track->isLocked()
        || clipIndex < 0 || clipIndex >= track->clipCount()) {
        return false;
    }

    const QVector<ClipInfo> currentClips = track->clips();
    if (currentClips[clipIndex].autoOrientEnabled == enabled)
        return true;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = currentClips;
    clips[clipIndex].autoOrientEnabled = enabled;
    track->setClips(clips);

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("パスに沿って自動回転"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipAutoOrientToCamera(int trackIndex, int clipIndex, bool enabled)
{
    TimelineTrack *track = trackAt(false, trackIndex);
    if (!track || track->isLocked()
        || clipIndex < 0 || clipIndex >= track->clipCount()) {
        return false;
    }

    const QVector<ClipInfo> currentClips = track->clips();
    if (currentClips[clipIndex].autoOrientToCamera == enabled)
        return true;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = currentClips;
    clips[clipIndex].autoOrientToCamera = enabled;
    track->setClips(clips);

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("カメラへ自動的に向ける"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipWiggle(int trackIndex, int clipIndex, const wiggle::WiggleParams &params)
{
    TimelineTrack *track = trackAt(false, trackIndex);
    if (!track || track->isLocked()
        || clipIndex < 0 || clipIndex >= track->clipCount()) {
        return false;
    }

    const QVector<ClipInfo> currentClips = track->clips();
    if (wiggle::toJson(currentClips[clipIndex].wiggle) == wiggle::toJson(params))
        return true;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = currentClips;
    clips[clipIndex].wiggle = params;
    track->setClips(clips);

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("wiggle を変更"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipExpressions(int trackIndex, int clipIndex,
                                  const exprbind::ClipExpressionBindings &bindings)
{
    TimelineTrack *track = trackAt(false, trackIndex);
    if (!track || track->isLocked()
        || clipIndex < 0 || clipIndex >= track->clipCount()) {
        return false;
    }

    const QVector<ClipInfo> currentClips = track->clips();
    if (currentClips[clipIndex].expressions.toJson() == bindings.toJson())
        return true;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = currentClips;
    clips[clipIndex].expressions = bindings;
    track->setClips(clips);

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("エクスプレッションを変更"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

// US-1706: the 3D text / brush moved onto the clip; same shape as setClipWiggle.
bool Timeline::setClipText3DConfig(int trackIndex, int clipIndex, const QJsonObject &config)
{
    TimelineTrack *track = trackAt(false, trackIndex);
    if (!track || track->isLocked()
        || clipIndex < 0 || clipIndex >= track->clipCount()) {
        return false;
    }

    const QVector<ClipInfo> currentClips = track->clips();
    if (currentClips[clipIndex].text3DConfig == config)
        return true;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = currentClips;
    clips[clipIndex].text3DConfig = config;
    track->setClips(clips);

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("3D テキストを変更"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipBrushData(int trackIndex, int clipIndex, const QJsonObject &brushData)
{
    TimelineTrack *track = trackAt(false, trackIndex);
    if (!track || track->isLocked()
        || clipIndex < 0 || clipIndex >= track->clipCount()) {
        return false;
    }

    const QVector<ClipInfo> currentClips = track->clips();
    if (currentClips[clipIndex].brushData == brushData)
        return true;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = currentClips;
    clips[clipIndex].brushData = brushData;
    track->setClips(clips);

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("ブラシを変更"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

std::shared_ptr<const audiokf::ExprAudioEnv> Timeline::expressionAudioEnvForRead() const
{
    // Only the UI thread compares the generation and rebuilds; a render thread reads
    // whatever snapshot is published (std::atomic_load), never the counter.
    if (m_exprAudioEnvRebuildHook && QThread::currentThread() == thread()) {
        const auto current = std::atomic_load(&m_exprAudioEnv);
        if (!current || current->generation != m_clipGeneration)
            m_exprAudioEnvRebuildHook();
    }
    return std::atomic_load(&m_exprAudioEnv);
}

std::function<double(double)> Timeline::expressionAudioLevelSampler(const ClipInfo &clip,
                                                                    double clipStartSec) const
{
    const auto env = expressionAudioEnvForRead();
    if (!env)
        return {};
    return audiokf::makeLinkedAudioSamplerFrom(*env, clip.linkGroup, clipStartSec);
}

std::shared_ptr<const audiokf::ExprAudioEnv> Timeline::buildExpressionAudioEnv(
    std::shared_ptr<audiokf::EnvelopeCache> cache) const
{
    auto env = std::make_shared<audiokf::ExprAudioEnv>();
    env->audioClipsByTrack.reserve(m_audioTracks.size());
    for (const auto *track : m_audioTracks) {
        if (track) env->audioClipsByTrack.append(track->clips());
    }
    env->cache = std::move(cache);
    env->generation = m_clipGeneration;
    return env;
}

void Timeline::setExpressionAudioEnv(std::shared_ptr<const audiokf::ExprAudioEnv> env)
{
    // US-1710: audioLevel() is the only reader of the envelopes. Warm up just the first
    // audio clip of each linkGroup that a video clip with an audioLevel() expression
    // links to (the clip makeLinkedAudioSampler picks), de-duplicated, within the
    // EnvelopeCache LRU limit.
    QStringList warmPaths;
    if (env && env->cache) {
        QSet<int> seenGroups;
        for (const TimelineTrack *track : std::as_const(m_videoTracks)) {
            if (!track) continue;
            for (const ClipInfo &clip : track->clips()) {
                if (clip.linkGroup <= 0 || seenGroups.contains(clip.linkGroup)) continue;
                bool usesAudioLevel = false;
                for (const QString &path : clip.expressions.boundPaths())
                    usesAudioLevel = usesAudioLevel
                        || clip.expressions.expression(path).contains(QStringLiteral("audioLevel"));
                if (!usesAudioLevel) continue;
                seenGroups.insert(clip.linkGroup);
                const ClipInfo *audio = nullptr;
                for (const QVector<ClipInfo> &clips : env->audioClipsByTrack) {
                    for (const ClipInfo &candidate : clips) {
                        if (candidate.linkGroup == clip.linkGroup && !candidate.filePath.isEmpty()) {
                            audio = &candidate;
                            break;
                        }
                    }
                    if (audio) break;
                }
                if (audio && !warmPaths.contains(audio->filePath) && warmPaths.size() < 16)
                    warmPaths.append(audio->filePath);
            }
        }
    }
    const std::weak_ptr<audiokf::EnvelopeCache> cache = env ? env->cache : nullptr;
    std::atomic_store(&m_exprAudioEnv, std::move(env));
    if (!warmPaths.isEmpty())
        audiokf::EnvelopeCache::prefetch(cache, warmPaths);
}

void Timeline::setExpressionAudioEnvRebuildHook(std::function<void()> hook)
{
    m_exprAudioEnvRebuildHook = std::move(hook);
}

void Timeline::ensureExpressionAudioEnvCurrent()
{
    if (!m_exprAudioEnvRebuildHook || QThread::currentThread() != thread())
        return;
    const auto current = std::atomic_load(&m_exprAudioEnv);
    if (!current || current->generation != m_clipGeneration)
        m_exprAudioEnvRebuildHook();
}

bool Timeline::matchFrame(double timelineSec, MatchFrameResult *result,
                          QString *errorOut) const
{
    if (result)
        *result = MatchFrameResult{};
    if (errorOut)
        errorOut->clear();
    const auto fail = [errorOut](const QString &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    if (!std::isfinite(timelineSec) || timelineSec < 0.0)
        return fail(QStringLiteral("タイムライン時刻が不正です"));

    QVector<int> trackOrder;
    const auto appendTrack = [&trackOrder, this](int index) {
        if (index >= 0 && index < m_videoTracks.size()
            && m_videoTracks.at(index) && !trackOrder.contains(index)) {
            trackOrder.append(index);
        }
    };

    // 最後にユーザーが選択した動画トラックを最優先にし、選択情報が復元された
    // 直後など active index が無い場合も selectedClip() から拾う。
    if (m_activeVideoTrackIndex >= 0
        && m_activeVideoTrackIndex < m_videoTracks.size()
        && m_videoTracks.at(m_activeVideoTrackIndex)
        && m_videoTracks.at(m_activeVideoTrackIndex)->selectedClip() >= 0) {
        appendTrack(m_activeVideoTrackIndex);
    }
    for (int i = 0; i < m_videoTracks.size(); ++i) {
        if (m_videoTracks.at(i) && m_videoTracks.at(i)->selectedClip() >= 0)
            appendTrack(i);
    }
    appendTrack(0); // 選択トラックで見つからなければ V1。

    constexpr double kEpsilon = 1e-9;
    for (int trackIndex : std::as_const(trackOrder)) {
        const TimelineTrack *track = m_videoTracks.at(trackIndex);
        const QVector<ClipInfo> &clips = track->clips();
        double cursor = 0.0;
        for (int clipIndex = 0; clipIndex < clips.size(); ++clipIndex) {
            const ClipInfo &clip = clips.at(clipIndex);
            const double clipStart = cursor + qMax(0.0, clip.leadInSec);
            const double clipDuration = qMax(0.0, clip.effectiveDuration());
            const double clipEnd = clipStart + clipDuration;
            if (timelineSec + kEpsilon >= clipStart
                && timelineSec < clipEnd - kEpsilon) {
                if (clip.filePath.isEmpty())
                    return fail(QStringLiteral("クリップにソースファイルがありません"));
                if (clip.isSequenceReference()) {
                    return fail(QStringLiteral(
                        "シーケンス参照はソースモニターで開けません"));
                }
                if (result) {
                    result->filePath = clip.filePath;
                    result->sourceSec = clip.sourceSecondAtLocalTime(
                        qBound(0.0, timelineSec - clipStart, clipDuration));
                    result->trackIndex = trackIndex;
                    result->clipIndex = clipIndex;
                }
                return true;
            }
            cursor = clipEnd;
        }
    }

    return fail(QStringLiteral("再生ヘッド位置に動画クリップがありません"));
}

bool Timeline::replaceRenderedClip(int trackIndex, int clipIndex,
                                   const ClipInfo &clip, const QString &description)
{
    TimelineTrack *track = trackAt(false, trackIndex);
    if (!track || track->isLocked() || clipIndex < 0 || clipIndex >= track->clipCount())
        return false;
    const ClipInfo previous = track->clips()[clipIndex];
    const bool decomposing = previous.renderInPlaceOriginal
        && previous.renderInPlaceOriginal.get() == &clip;
    const auto startAt = [](const TimelineTrack *t, int index) {
        double start = 0.0;
        for (int i = 0; i <= index; ++i) {
            start += t->clips()[i].leadInSec;
            if (i < index) start += t->clips()[i].effectiveDuration();
        }
        return start;
    };
    const double videoStart = startAt(track, clipIndex);
    QVector<QVector<ClipInfo>> audioReplacements;
    for (const auto *audioTrack : m_audioTracks) {
        auto audioClips = audioTrack->clips();
        for (int i = 0; i < audioClips.size(); ++i) {
            const ClipInfo original = audioClips[i];
            if (previous.linkGroup == 0 || original.linkGroup != previous.linkGroup) continue;
            if (audioTrack->isLocked()) return false;
            if (decomposing) {
                if (!original.renderInPlaceOriginal || original.filePath != previous.filePath)
                    return false;
                audioClips[i] = *original.renderInPlaceOriginal;
                // US-1214: the live connection wins over the bake-time snapshot.
                audioClips[i].anchorUid = original.anchorUid;
                audioClips[i].anchorOffsetSec = original.anchorOffsetSec;
            } else {
                ClipInfo baked{};
                baked.filePath = clip.filePath;
                baked.displayName = original.displayName;
                baked.duration = clip.duration;
                baked.inPoint = clip.inPoint + startAt(audioTrack, i) - videoStart;
                baked.outPoint = baked.inPoint + original.effectiveDuration();
                baked.leadInSec = original.leadInSec;
                baked.linkGroup = original.linkGroup;
                baked.label = original.label;
                // US-1214: the baked clip is the same clip (identity and connection).
                baked.uid = original.uid;
                baked.anchorUid = original.anchorUid;
                baked.anchorOffsetSec = original.anchorOffsetSec;
                baked.renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
                audioClips[i] = baked;
            }
        }
        audioReplacements.append(audioClips);
    }
    TrackClipSnapshot before = snapshotTrackClips(this);
    auto clips = track->clips();
    clips[clipIndex] = clip;
    if (decomposing) {
        // US-1214: the live connection wins over the bake-time snapshot.
        clips[clipIndex].anchorUid = previous.anchorUid;
        clips[clipIndex].anchorOffsetSec = previous.anchorOffsetSec;
        // The restored nested originals are shared with every clone of the
        // baked clip, so a restored uid can collide across tracks (setClips
        // only repairs within one track). Re-mint any that collides.
        QSet<QString> taken;
        for (const auto *videoTrack : m_videoTracks) {
            const auto trackClips = videoTrack->clips();
            for (int i = 0; i < trackClips.size(); ++i)
                if (videoTrack != track || i != clipIndex) taken.insert(trackClips[i].uid);
        }
        for (const auto *audioTrack : m_audioTracks)
            for (const ClipInfo &audioClip : audioTrack->clips())
                if (previous.linkGroup == 0 || audioClip.linkGroup != previous.linkGroup)
                    taken.insert(audioClip.uid);
        const auto claim = [&taken](ClipInfo &restored) {
            if (taken.contains(restored.uid)) restored.uid.clear();
            else taken.insert(restored.uid);
        };
        claim(clips[clipIndex]);
        for (int t = 0; t < m_audioTracks.size(); ++t) {
            const auto currentClips = m_audioTracks[t]->clips();
            for (int i = 0; i < audioReplacements[t].size(); ++i)
                if (previous.linkGroup != 0 && currentClips[i].linkGroup == previous.linkGroup)
                    claim(audioReplacements[t][i]);
        }
    }
    track->setClips(clips);
    for (int i = 0; i < m_audioTracks.size(); ++i)
        m_audioTracks[i]->setClips(audioReplacements[i]);
    // US-1214: a decomposed V1 anchor whose uid was re-minted (or restored from
    // an older snapshot) keeps its connections.
    if (decomposing && trackIndex == 0)
        renameAnchorReferences(previous.uid, track->clips().at(clipIndex).uid);
    // Replacement preserves the position; update its identity before remap
    // (the same convention as replaceClipMedia).
    before[trackIndex][clipIndex] = {clip.filePath, clip.linkGroup, clip.inPoint};
    remapTrackMatteLocked(before);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, before);
    saveUndoState(description);
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::replaceClipMedia(TrackKind kind, int trackIndex, int clipIndex,
                                const QString &newPath,
                                const QString &newDisplayName,
                                double newSourceDurationSec,
                                QString *messageOut)
{
    if (messageOut)
        messageOut->clear();
    const auto fail = [messageOut](const QString &message) {
        if (messageOut)
            *messageOut = message;
        return false;
    };

    const QFileInfo fileInfo(newPath);
    if (newPath.isEmpty() || !fileInfo.exists() || !fileInfo.isFile())
        return fail(QStringLiteral("ファイルが見つかりません: %1").arg(newPath));

    TimelineTrack *targetTrack = trackAt(kind == TrackKind::Audio, trackIndex);
    if (!targetTrack)
        return fail(QStringLiteral("トラック番号が範囲外です"));
    if (clipIndex < 0 || clipIndex >= targetTrack->clipCount())
        return fail(QStringLiteral("クリップ番号が範囲外です"));
    if (targetTrack->isLocked())
        return fail(QStringLiteral("トラックがロックされています"));

    const TimelineMediaProbe mediaProbe = probeTimelineMedia(newPath);
    if (!mediaProbe.openedOk
        || (!mediaProbe.videoStreamFound && !mediaProbe.audioStreamFound)) {
        return fail(QStringLiteral("メディアとして開けません: %1").arg(newPath));
    }
    if (kind == TrackKind::Video && !mediaProbe.videoStreamFound)
        return fail(QStringLiteral("映像ストリームがありません: %1").arg(newPath));
    if (kind == TrackKind::Audio && !mediaProbe.audioStreamFound)
        return fail(QStringLiteral("音声ストリームがありません: %1").arg(newPath));

    const double sourceDuration = mediaProbe.durationSec > 0.0
        ? mediaProbe.durationSec : newSourceDurationSec;
    if (!std::isfinite(sourceDuration) || sourceDuration <= 0.0)
        return fail(QStringLiteral("新しい素材の長さを取得できません"));

    const QVector<ClipInfo> targetClips = targetTrack->clips();
    const int linkGroup = targetClips.at(clipIndex).linkGroup;
    const QString displayName = newDisplayName.isEmpty()
        ? fileInfo.fileName() : newDisplayName;

    const auto shouldReplace = [&](TimelineTrack *track, int index,
                                   bool audioTrack) {
        if (!track || index < 0 || index >= track->clipCount())
            return false;
        const bool isTarget = track == targetTrack && index == clipIndex;
        const bool isLinked = linkGroup > 0
            && track->clips().at(index).linkGroup == linkGroup;
        if (!isTarget && !isLinked)
            return false;
        return audioTrack ? mediaProbe.audioStreamFound
                          : mediaProbe.videoStreamFound;
    };

    bool linkedAudioLeftUnchanged = false;
    if (linkGroup > 0 && !mediaProbe.audioStreamFound) {
        for (TimelineTrack *audioTrack : std::as_const(m_audioTracks)) {
            if (!audioTrack)
                continue;
            const auto &clips = audioTrack->clips();
            linkedAudioLeftUnchanged = std::any_of(
                clips.cbegin(), clips.cend(), [linkGroup](const ClipInfo &clip) {
                    return clip.linkGroup == linkGroup;
                });
            if (linkedAudioLeftUnchanged)
                break;
        }
    }

    // 全対象を先に検証し、リンク相方がロック中・新素材が現在の inPoint より
    // 短い場合は 1 つも変更しない。
    const auto validateTracks = [&](const QVector<TimelineTrack *> &tracks,
                                    bool audioTrack) {
        for (TimelineTrack *track : tracks) {
            if (!track)
                continue;
            const auto &clips = track->clips();
            for (int i = 0; i < clips.size(); ++i) {
                if (!shouldReplace(track, i, audioTrack))
                    continue;
                if (track->isLocked())
                    return fail(QStringLiteral("リンクしたトラックがロックされています"));
                if (sourceDuration <= clips.at(i).inPoint + 1e-9) {
                    return fail(QStringLiteral(
                        "新しい素材が現在のイン点 (%1 秒) より短いため置き換えできません")
                                    .arg(clips.at(i).inPoint, 0, 'f', 3));
                }
            }
        }
        return true;
    };
    if (!validateTracks(m_videoTracks, false)
        || !validateTracks(m_audioTracks, true)) {
        return false;
    }

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    TrackClipSnapshot remapSnapshot = snapBefore;
    bool changed = false;
    bool shortened = false;
    double shortestTimelineDuration = std::numeric_limits<double>::infinity();
    const auto replaceOnTracks = [&](const QVector<TimelineTrack *> &tracks,
                                     bool audioTrack) {
        for (TimelineTrack *track : tracks) {
            if (!track)
                continue;
            QVector<ClipInfo> clips = track->clips();
            bool trackChanged = false;
            for (int i = 0; i < clips.size(); ++i) {
                if (!shouldReplace(track, i, audioTrack))
                    continue;

                ClipInfo &clip = clips[i];
                const double oldTimelineDuration = qMax(0.0, clip.effectiveDuration());
                const double oldSourceOut = clip.outPoint > 0.0
                    ? clip.outPoint : clip.duration;
                const double oldSourceSpan = qMax(0.0, oldSourceOut - clip.inPoint);
                const double requestedOut = clip.inPoint + oldSourceSpan;
                const double replacementOut = qMin(requestedOut, sourceDuration);

                clip.filePath = newPath;
                clip.displayName = displayName;
                clip.duration = sourceDuration;
                clip.outPoint = replacementOut;
                clip.sequenceRefId.clear();
                clip.waveform = WaveformData{};

                // ClipKeyId のfilePath も置き換わるため、古い識別子の
                // まま remap すると位置が変わっていないマット/親子参照が
                // 削除扱いになる。対象のみ新識別子に置き換えて、
                // 従来の snapshot -> mutation -> remap 手順で同一 index へ写す。
                if (!audioTrack) {
                    const int videoTrackIndex = m_videoTracks.indexOf(track);
                    if (videoTrackIndex >= 0
                        && videoTrackIndex < remapSnapshot.size()
                        && i < remapSnapshot[videoTrackIndex].size()) {
                        remapSnapshot[videoTrackIndex][i].filePath = newPath;
                    }
                }

                const double replacementTimelineDuration =
                    qMax(0.0, clip.effectiveDuration());
                const double lostDuration = qMax(
                    0.0, oldTimelineDuration - replacementTimelineDuration);
                if (lostDuration > 1e-9 && i + 1 < clips.size())
                    clips[i + 1].leadInSec += lostDuration;
                if (replacementOut + 1e-9 < requestedOut) {
                    shortened = true;
                    shortestTimelineDuration = qMin(
                        shortestTimelineDuration, replacementTimelineDuration);
                }
                trackChanged = true;
            }
            if (trackChanged) {
                track->setClips(clips);
                changed = true;
            }
        }
    };
    replaceOnTracks(m_videoTracks, false);
    replaceOnTracks(m_audioTracks, true);

    if (!changed)
        return fail(QStringLiteral("置き換え対象のクリップがありません"));

    remapTrackMatteLocked(remapSnapshot);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, remapSnapshot);
    saveUndoState(QStringLiteral("Replace clip media"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();

    QStringList warnings;
    if (shortened) {
        warnings.append(QStringLiteral(
            "新しい素材が短いため、クリップを %1 秒に短縮しました。")
                            .arg(shortestTimelineDuration, 0, 'f', 3));
    }
    if (linkedAudioLeftUnchanged) {
        warnings.append(QStringLiteral(
            "新しい素材に音声がないため、リンクした音声クリップは変更していません。"));
    }
    if (messageOut)
        *messageOut = warnings.join(QLatin1Char(' '));
    return true;
}

bool Timeline::relinkMediaPaths(const QHash<QString, QString> &oldToNew,
                                QString *errorOut,
                                const std::function<bool(
                                    const QHash<QString, QString> &)> &relinkSidecars)
{
    if (errorOut)
        errorOut->clear();
    const auto fail = [errorOut](const QString &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    if (oldToNew.isEmpty())
        return fail(QStringLiteral("mapping が空です"));

    for (auto it = oldToNew.cbegin(); it != oldToNew.cend(); ++it) {
        if (timeline_nesting::isSequenceClipFilePath(it.key())
            || clipgeom::isNullObjectFilePath(it.key())) {
            return fail(QStringLiteral("内部メディア参照は再リンクできません: %1")
                            .arg(it.key()));
        }
        const QFileInfo destination(it.value());
        if (it.key().isEmpty() || it.value().isEmpty()
            || !destination.exists() || !destination.isFile()) {
            return fail(QStringLiteral("ファイルが見つかりません: %1")
                            .arg(it.value()));
        }
    }

    syncActiveSequenceFromCurrentTracks();
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    TrackClipSnapshot remapSnapshot = snapBefore;
    bool changed = false;

    const auto relinkClip = [&oldToNew, &changed](ClipInfo &clip) {
        const auto media = oldToNew.constFind(clip.filePath);
        if (media != oldToNew.cend() && media.value() != clip.filePath) {
            clip.filePath = media.value();
            changed = true;
        }
        const auto lut = oldToNew.constFind(clip.lutFilePath);
        if (lut != oldToNew.cend() && lut.value() != clip.lutFilePath) {
            clip.lutFilePath = lut.value();
            changed = true;
        }
    };

    for (int trackIndex = 0; trackIndex < m_videoTracks.size(); ++trackIndex) {
        TimelineTrack *track = m_videoTracks.at(trackIndex);
        if (!track)
            continue;
        QVector<ClipInfo> clips = track->clips();
        bool trackChanged = false;
        for (int clipIndex = 0; clipIndex < clips.size(); ++clipIndex) {
            const QString oldFilePath = clips.at(clipIndex).filePath;
            const QString oldLutPath = clips.at(clipIndex).lutFilePath;
            relinkClip(clips[clipIndex]);
            if (clips.at(clipIndex).filePath != oldFilePath
                || clips.at(clipIndex).lutFilePath != oldLutPath) {
                trackChanged = true;
            }
            if (clips.at(clipIndex).filePath != oldFilePath
                && trackIndex < remapSnapshot.size()
                && clipIndex < remapSnapshot[trackIndex].size()) {
                remapSnapshot[trackIndex][clipIndex].filePath =
                    clips.at(clipIndex).filePath;
            }
        }
        if (trackChanged)
            track->setClips(clips);
    }

    for (TimelineTrack *track : std::as_const(m_audioTracks)) {
        if (!track)
            continue;
        QVector<ClipInfo> clips = track->clips();
        bool trackChanged = false;
        for (ClipInfo &clip : clips) {
            const QString oldFilePath = clip.filePath;
            const QString oldLutPath = clip.lutFilePath;
            relinkClip(clip);
            trackChanged = trackChanged || clip.filePath != oldFilePath
                || clip.lutFilePath != oldLutPath;
        }
        if (trackChanged)
            track->setClips(clips);
    }

    if (m_sequenceModelEnabled) {
        for (TimelineSequence &sequence : m_sequences) {
            for (auto &track : sequence.videoTracks) {
                for (ClipInfo &clip : track)
                    relinkClip(clip);
            }
            for (auto &track : sequence.audioTracks) {
                for (ClipInfo &clip : track)
                    relinkClip(clip);
            }
        }
    }

    // MainWindow owns project-level image overlays and particle emitter
    // configs. Let their owner join this already-validated mutation before
    // the single undo snapshot is saved. A sidecar-only mapping is still a
    // real relink even when no active/nested ClipInfo uses it.
    if (relinkSidecars)
        changed = relinkSidecars(oldToNew) || changed;

    if (!changed)
        return fail(QStringLiteral("置換対象のメディア参照がありません"));

    remapTrackMatteLocked(remapSnapshot);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, remapSnapshot);
    saveUndoState(QStringLiteral("メディアを再リンク"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipEnabled(TrackKind kind, int trackIndex, int clipIndex, bool enabled)
{
    TimelineTrack *track = trackAt(kind == TrackKind::Audio, trackIndex);
    if (!track || track->isLocked() || clipIndex < 0 || clipIndex >= track->clipCount())
        return false;
    QVector<ClipInfo> clips = track->clips();
    if (clipgeom::isNullObjectFilePath(clips[clipIndex].filePath))
        return false;
    if (clips[clipIndex].visible == enabled)
        return true;
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    clips[clipIndex].visible = enabled;
    track->setClips(clips);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("クリップの有効切替"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setClipLabel(TrackKind kind, int trackIndex, int clipIndex,
                            ClipLabel label)
{
    switch (label) {
    case ClipLabel::None:
    case ClipLabel::Red:
    case ClipLabel::Orange:
    case ClipLabel::Yellow:
    case ClipLabel::Green:
    case ClipLabel::Cyan:
    case ClipLabel::Blue:
    case ClipLabel::Purple:
    case ClipLabel::Pink:
        break;
    default:
        return false;
    }

    TimelineTrack *track = trackAt(kind == TrackKind::Audio, trackIndex);
    if (!track || track->isLocked()
        || clipIndex < 0 || clipIndex >= track->clipCount()) {
        return false;
    }

    QVector<ClipInfo> clips = track->clips();
    if (clips[clipIndex].label == label)
        return true;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    clips[clipIndex].label = label;
    track->setClips(clips);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("Clip label"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

QVector<Timeline::TimeRangeSec> Timeline::selectedClipTimeRanges() const
{
    QVector<TimeRangeSec> ranges;
    auto collect = [&](const QVector<TimelineTrack *> &tracks) {
        for (const auto *track : tracks) {
            if (!track)
                continue;
            for (int clipIndex : track->selectedClips()) {
                double start = 0.0;
                double end = 0.0;
                if (trackClipTimeRangeAt(track, clipIndex, &start, &end)
                    && end > start) {
                    ranges.append(TimeRangeSec{start, end});
                }
            }
        }
    };
    collect(m_videoTracks);
    collect(m_audioTracks);
    return ranges;
}

bool Timeline::gapTimeRangeAt(TimelineTrack *track, double timeSec, TimeRangeSec *outRange) const
{
    if (!track)
        return false;
    constexpr double kEps = 1e-6;
    timeSec = qMax(0.0, timeSec);
    const auto &clips = track->clips();
    double cursor = 0.0;
    for (const ClipInfo &clip : clips) {
        const double clipStart = cursor + clip.leadInSec;
        if (clipStart - cursor > kEps
            && timeSec >= cursor - kEps
            && timeSec < clipStart - kEps) {
            if (outRange)
                *outRange = TimeRangeSec{cursor, clipStart};
            return true;
        }
        cursor = clipStart + clip.effectiveDuration();
    }
    return false;
}

bool Timeline::applyRippleDeleteTimeRangesToAllTracks(QVector<TimeRangeSec> ranges,
                                                      const QString &undoLabel, bool skipLocked)
{
    constexpr double kEps = 1e-6;
    QVector<TimeRangeSec> valid;
    valid.reserve(ranges.size());
    for (const TimeRangeSec &range : ranges) {
        if (range.endSec - range.startSec > kEps)
            valid.append(range);
    }
    if (valid.isEmpty())
        return false;

    std::sort(valid.begin(), valid.end(), [&](const TimeRangeSec &a, const TimeRangeSec &b) {
        if (qAbs(a.startSec - b.startSec) > kEps)
            return a.startSec < b.startSec;
        return a.endSec < b.endSec;
    });

    QVector<TimeRangeSec> merged;
    for (const TimeRangeSec &range : valid) {
        if (merged.isEmpty() || range.startSec > merged.last().endSec + kEps) {
            merged.append(range);
        } else {
            merged.last().endSec = qMax(merged.last().endSec, range.endSec);
        }
    }

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    if (TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
        v1 && (!skipLocked || !v1->isLocked())) {
        QVector<QPair<double, double>> removed;
        for (const TimeRangeSec &range : merged) removed.append({range.startSec, range.endSec});
        const auto spans = rangeRemovedSpans(v1, removed);
        for (auto it = spans.cbegin(); it != spans.cend(); ++it)
            m_pendingDeletedUids[it.key()] += it.value();
    }
    bool changed = false;
    for (int i = merged.size() - 1; i >= 0; --i) {
        const TimeRangeSec range = merged[i];
        for (auto *track : m_videoTracks) {
            if (track && (!skipLocked || !track->isLocked()))
                changed = track->rippleDeleteTimeRange(range.startSec, range.endSec, false) || changed;
        }
        for (auto *track : m_audioTracks) {
            if (track && (!skipLocked || !track->isLocked()))
                changed = track->rippleDeleteTimeRange(range.startSec, range.endSec, false) || changed;
        }
    }

    if (!changed)
        return false;

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(undoLabel);
    if (!hasAnySelection()) {
        m_activeVideoTrackIndex = -1;
        emit clipSelected(-1);
        emit clipSelectedOnTrack(-1, -1);
    }
    ensureSequenceFitsViewport();
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
    updateInfoLabel();
    return true;
}

void Timeline::rippleDeleteSelectedClip()
{
    // US-1203: no all-track ripple in storyline mode (V1 only, as Delete).
    if (m_storylineMode) {
        deleteSelectedClip();
        return;
    }
    applyRippleDeleteTimeRangesToAllTracks(selectedClipTimeRanges(),
                                           QStringLiteral("リップル削除"));
}

void Timeline::closeAllGaps()
{
    // US-1203: storyline mode closes the V1 gaps only.
    if (m_storylineMode) {
        if (closeGapsOnTrack(m_videoTracks.value(0, nullptr)))
            saveUndoState(QStringLiteral("すべてのギャップを詰める"));
        return;
    }
    QVector<TimeRangeSec> occupied;
    for (TimelineTrack *track : timelineTracks(this)) {
        if (!track || track->isLocked()) continue;
        for (int i = 0; i < track->clipCount(); ++i) {
            double start = 0.0, end = 0.0;
            if (trackClipTimeRangeAt(track, i, &start, &end) && end > start)
                occupied.append(TimeRangeSec{start, end});
        }
    }
    std::sort(occupied.begin(), occupied.end(), [](const TimeRangeSec &a, const TimeRangeSec &b) {
        return a.startSec < b.startSec;
    });
    QVector<TimeRangeSec> gaps;
    double end = 0.0;
    for (const auto &range : occupied) {
        if (range.startSec > end + 1e-6)
            gaps.append(TimeRangeSec{end, range.startSec});
        end = qMax(end, range.endSec);
    }
    // The shared primitive merges ranges and applies them back to front,
    // then remaps carriers and records exactly one undo state.
    applyRippleDeleteTimeRangesToAllTracks(gaps, QStringLiteral("すべてのギャップを詰める"), true);
}

void Timeline::duplicateSelectedClips()
{
    struct Copy { TimelineTrack *track; ClipInfo clip; double end; };
    QVector<Copy> copies;
    QSet<int> groups;
    const auto tracks = timelineTracks(this);
    for (auto *track : tracks) {
        if (!track || track->isLocked()) continue;
        for (int i : track->selectedClips())
            if (i >= 0 && i < track->clipCount() && track->clips()[i].linkGroup > 0)
                groups.insert(track->clips()[i].linkGroup);
    }
    // A locked partner makes the entire linked operation ineligible.
    for (auto *track : tracks)
        if (track && track->isLocked())
            for (const auto &clip : track->clips()) groups.remove(clip.linkGroup);
    for (auto *track : tracks) {
        if (!track || track->isLocked()) continue;
        for (int i = 0; i < track->clipCount(); ++i) {
            const auto &clip = track->clips()[i];
            if (clip.linkGroup > 0 ? !groups.contains(clip.linkGroup) : !track->isClipSelected(i)) continue;
            double start = 0.0, end = 0.0;
            if (trackClipTimeRangeAt(track, i, &start, &end)) copies.append({track, clip, end});
        }
    }
    if (copies.isEmpty()) return;
    const TrackClipSnapshot before = snapshotTrackClips(this);
    QHash<int, int> newGroups;
    for (const auto &copy : copies) {
        const ClipInfo duplicate = copyClipForPaste(copy.clip, newGroups);
        double cursor = 0.0;
        int insertAt = copy.track->clipCount();
        double start = copy.end;
        for (int i = 0; i < copy.track->clipCount(); ++i) {
            const auto &existing = copy.track->clips()[i];
            const double nextStart = cursor + qMax(0.0, existing.leadInSec);
            if (start >= cursor - 1e-6 && start + duplicate.effectiveDuration() <= nextStart + 1e-6) {
                insertAt = i;
                break;
            }
            cursor = nextStart + existing.effectiveDuration();
        }
        start = qMax(start, cursor);
        // Shared paste/insert primitive preserves the complete ClipInfo payload.
        copy.track->insertClipPreservingDownstream(insertAt, duplicate, start - cursor);
    }
    remapTrackMatteLocked(before);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, before);
    saveUndoState(QStringLiteral("複製"));
    ensureSequenceFitsViewport();
    scheduleEmitSequenceChanged();
    updateInfoLabel();
}

void Timeline::setNudgeFrameRate(double fps)
{
    if (std::isfinite(fps) && fps > 0.0) m_nudgeFrameRate = fps;
}

void Timeline::nudgeSelectedClips(int frames)
{
    if (!frames) return;
    struct Target { TimelineTrack *track; int index; double start; int group; };
    QVector<Target> targets;
    QVector<Target> selection;
    QSet<int> groups;
    for (auto *track : timelineTracks(this)) {
        if (!track || track->isLocked()) continue;
        for (int i : track->selectedClips()) {
            if (i < 0 || i >= track->clipCount()) continue;
            const int group = track->clips()[i].linkGroup;
            double start = 0.0, end = 0.0;
            if (!trackClipTimeRangeAt(track, i, &start, &end)) continue;
            selection.append({track, i, start, group});
            if (group > 0 && groups.contains(group)) continue;
            targets.append({track, i, start, group});
            if (group > 0) groups.insert(group);
        }
    }
    // US-1305 (decisions 15 / 19): in storyline mode a V1 nudge moves the first
    // selected V1 clip and every clip after it (its leadInSec ±d; − needs a gap of
    // d). Selected linked partners of the V1 clips follow through the reconcile;
    // any other clip in the selection refuses the nudge.
    TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    int firstV1 = -1;
    QSet<int> v1Groups;
    for (const auto &selected : selection) {
        if (!m_storylineMode || selected.track != v1) continue;
        if (firstV1 < 0 || selected.index < firstV1) firstV1 = selected.index;
        if (selected.group > 0) v1Groups.insert(selected.group);
    }
    if (firstV1 >= 0) {
        for (const auto &selected : selection) {
            if (selected.track != v1 && !(selected.group > 0 && v1Groups.contains(selected.group))) {
                emit statusMessageRequested(
                    QStringLiteral("ストーリーラインモードでは V1 と他のトラックのクリップを同時にナッジできません"), 3000);
                return;
            }
        }
        // Same convention as the other linked ops: a locked partner of a
        // selected V1 clip makes the whole nudge ineligible (the reconcile
        // would leave it behind and break A/V sync).
        for (auto *track : timelineTracks(this)) {
            if (!track || !track->isLocked()) continue;
            for (const auto &clip : track->clips()) {
                if (clip.linkGroup > 0 && v1Groups.contains(clip.linkGroup)) {
                    emit statusMessageRequested(
                        QStringLiteral("リンクされたクリップがロック中のトラックにあるためナッジできません"), 3000);
                    return;
                }
            }
        }
        const double deltaSec = double(frames) / m_nudgeFrameRate;
        const double leadInSec = v1->clips().at(firstV1).leadInSec;
        if (leadInSec + deltaSec < -1e-9) {
            emit statusMessageRequested(QStringLiteral("前に空きが無いためナッジできません"), 3000);
            return;
        }
        const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
        v1->applyDragMove(firstV1, leadInSec + deltaSec, -1.0);
        remapTrackMatteLocked(snapBefore);
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
        onTrackModified();
        saveUndoState(QStringLiteral("ナッジ"));
        updateInfoLabel();
        return;
    }
    std::sort(targets.begin(), targets.end(), [frames](const Target &a, const Target &b) {
        return frames > 0 ? a.start > b.start : a.start < b.start;
    });
    bool changed = false;
    m_nudgeBatchActive = true;
    for (const auto &target : targets) {
        const bool audio = target.track->isAudioTrack();
        const int trackIndex = audio ? m_audioTracks.indexOf(target.track) : m_videoTracks.indexOf(target.track);
        int index = -1;
        for (int i = 0; i < target.track->clipCount(); ++i) {
            double start = 0.0, end = 0.0;
            if (trackClipTimeRangeAt(target.track, i, &start, &end)
                && qAbs(start - target.start) < 1e-6) { index = i; break; }
        }
        if (index < 0) continue;
        const double requested = target.start + double(frames) / m_nudgeFrameRate;
        MoveClipResult result;
        QString error;
        double boundedStart = qMax(0.0, requested);
        if (target.group > 0) {
            // Clamp the whole linked group at its earliest member, preserving offsets.
            for (auto *track : timelineTracks(this)) {
                if (!track) continue;
                for (int i = 0; i < track->clipCount(); ++i) {
                    if (track->clips()[i].linkGroup != target.group) continue;
                    double start = 0.0, end = 0.0;
                    if (trackClipTimeRangeAt(track, i, &start, &end))
                        boundedStart = qMax(boundedStart, target.start - start);
                }
            }
        }
        const bool valid = moveClipByIndex(audio, trackIndex, index,
            boundedStart, trackIndex, &result, &error);
        changed = changed || result.moved;
        if (result.moved) {
            for (auto &selected : selection)
                if ((target.group > 0 && selected.group == target.group)
                    || (selected.track == target.track && selected.index == target.index))
                    selected.start += result.actualStartSec - target.start;
        }
        if (!valid || !result.reason.isEmpty())
            emit statusMessageRequested(QStringLiteral("指定位置へ移動できません。候補: %1 秒 %2")
                .arg(result.actualStartSec, 0, 'f', 6).arg(error.isEmpty() ? result.reason : error), 3000);
        else if (qAbs(result.actualStartSec - requested) > 1e-6)
            emit statusMessageRequested(QStringLiteral("ナッジ位置を %1 秒に調整しました。")
                .arg(result.actualStartSec, 0, 'f', 6), 3000);
    }
    m_nudgeBatchActive = false;
    // Keep the complete multi-selection for repeated keyboard nudges.
    for (auto *track : timelineTracks(this)) {
        if (!track || track->isLocked()) continue;
        const QSignalBlocker blocker(track);
        track->clearClipSelection();
        for (const auto &selected : selection) {
            if (selected.track != track) continue;
            for (int i = 0; i < track->clipCount(); ++i) {
                double start = 0.0, end = 0.0;
                if (trackClipTimeRangeAt(track, i, &start, &end)
                    && qAbs(start - selected.start) < 1e-6) {
                    track->toggleClipSelection(i);
                    break;
                }
            }
        }
    }
    if (changed) {
        saveUndoState(QStringLiteral("ナッジ"));
        ensureSequenceFitsViewport();
        updateInfoLabel();
        const auto *active = trackAt(false, m_activeVideoTrackIndex);
        const int primary = active ? active->selectedClip() : -1;
        emit clipSelected(primary);
        emit clipSelectedOnTrack(m_activeVideoTrackIndex, primary);
    }
}

bool Timeline::closeGapAt(TimelineTrack *track, double timeSec)
{
    TimeRangeSec gap;
    if (!gapTimeRangeAt(track, timeSec, &gap))
        return false;
    // US-1203: storyline mode closes gaps on V1 only, never all tracks.
    if (m_storylineMode) {
        if (track != m_videoTracks.value(0, nullptr) || !closeGapsOnTrack(track))
            return false;
        saveUndoState(QStringLiteral("ギャップを詰める"));
        return true;
    }

    for (const auto *t : m_videoTracks) {
        if (trackRangeOverlapsClip(t, gap.startSec, gap.endSec))
            return false;
    }
    for (const auto *t : m_audioTracks) {
        if (trackRangeOverlapsClip(t, gap.startSec, gap.endSec))
            return false;
    }

    QVector<TimeRangeSec> ranges;
    ranges.append(gap);
    return applyRippleDeleteTimeRangesToAllTracks(ranges,
                                                  QStringLiteral("ギャップを詰める"));
}

void Timeline::showGapContextMenu(TimelineTrack *track, double timeSec, const QPoint &globalPos)
{
    TimeRangeSec gap;
    const bool hasGap = gapTimeRangeAt(track, timeSec, &gap);

    bool blockedByClip = false;
    if (hasGap) {
        for (const auto *t : m_videoTracks)
            blockedByClip = blockedByClip || trackRangeOverlapsClip(t, gap.startSec, gap.endSec);
        for (const auto *t : m_audioTracks)
            blockedByClip = blockedByClip || trackRangeOverlapsClip(t, gap.startSec, gap.endSec);
    }

    QMenu menu;
    QAction *closeGapAct = menu.addAction(QStringLiteral("ギャップを詰める"));
    closeGapAct->setEnabled(hasGap && !blockedByClip);
    menu.addSeparator();
    QAction *addAdjustmentAct = menu.addAction(QStringLiteral("調整レイヤーを作成"));
    addAdjustmentAct->setEnabled(!m_videoTracks.isEmpty());
    QAction *chosen = menu.exec(globalPos);
    if (chosen == closeGapAct && closeGapAct->isEnabled())
        closeGapAt(track, timeSec);
    else if (chosen == addAdjustmentAct && addAdjustmentAct->isEnabled()) {
        int trackIndex = m_videoTracks.indexOf(track);
        if (trackIndex < 0)
            trackIndex = (m_activeVideoTrackIndex >= 0) ? m_activeVideoTrackIndex : 0;
        const double startSec = qMax(0.0, timeSec);
        addAdjustmentLayer(makeAdjustmentLayer(
            startSec,
            startSec + static_cast<double>(kDefaultAdjustmentLayerDurationUs) / 1.0e6,
            trackIndex));
    }
}

bool Timeline::hasSelection() const
{
    return m_videoTrack && m_videoTrack->selectedClip() >= 0;
}

bool Timeline::hasAnySelection() const
{
    for (const auto *track : m_videoTracks) {
        if (track && track->selectedClip() >= 0)
            return true;
    }
    for (const auto *track : m_audioTracks) {
        if (track && track->selectedClip() >= 0)
            return true;
    }
    return false;
}

bool Timeline::addTextOverlayToFirstVideoClip(const EnhancedTextOverlay &overlay)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    clips[0].textManager.addOverlay(overlay);
    track->setClips(clips);
    saveUndoState("Add text overlay");
    refreshTextStrip();
    return true;
}

QVector<int> Timeline::addTextOverlayToVideoClipsInRange(const EnhancedTextOverlay &overlay,
                                                         double startSec, double endSec)
{
    QVector<int> touched;
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return touched;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    double cursor = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        const double start = cursor + clips[i].leadInSec;
        const double end = start + clips[i].effectiveDuration();
        cursor = end;
        if (end <= startSec || start >= endSec)
            continue;
        clips[i].textManager.addOverlay(overlay);
        touched.append(i);
    }
    if (touched.isEmpty())
        return touched;
    track->setClips(clips);
    saveUndoState("Add text overlay");
    refreshTextStrip();
    return touched;
}

QVector<EnhancedTextOverlay> Timeline::timelineTextOverlays() const
{
    QVector<EnhancedTextOverlay> result;
    if (!m_videoTracks.isEmpty() && m_videoTracks.first()) {
        const QVector<ClipInfo> &clips = m_videoTracks.first()->clips();
        if (!clips.isEmpty())
            result = clips.first().textManager.overlays();
    }
    result.reserve(result.size() + m_generatedCaptionOverlays.size());
    result += m_generatedCaptionOverlays;
    return result;
}

namespace {

QString comparableMediaPath(const QString &path)
{
    QFileInfo info(path);
    QString resolved = info.canonicalFilePath();
    if (resolved.isEmpty())
        resolved = info.absoluteFilePath();
    resolved = QDir::cleanPath(resolved);
#ifdef Q_OS_WIN
    resolved = resolved.toCaseFolded();
#endif
    return resolved;
}

bool sourceSecondToClipLocal(const ClipInfo &clip,
                             double sourceSecond,
                             double *localSecond)
{
    if (!localSecond || !std::isfinite(sourceSecond)
        || clip.speed <= 0.0 || clip.effectiveDuration() <= 0.0) {
        return false;
    }

    const double sourceOffset = sourceSecond - clip.inPoint;
    const double duration = clip.effectiveDuration();
    constexpr double eps = 1e-7;
    if (!clip.hasTimeRemap()) {
        const double local = sourceOffset / clip.speed;
        if (local < -eps || local > duration + eps)
            return false;
        *localSecond = qBound(0.0, local, duration);
        return true;
    }

    const QVector<timeremap::TimeRemapKey> &keys = clip.timeRemapCurve.keys;
    if (keys.size() < 2)
        return false; // a freeze/constant map has no unique inverse interval

    bool found = false;
    double resolved = 0.0;
    for (int i = 1; i < keys.size(); ++i) {
        const timeremap::TimeRemapKey &a = keys.at(i - 1);
        const timeremap::TimeRemapKey &b = keys.at(i);
        const double deltaSource = b.srcTime - a.srcTime;
        if (qAbs(deltaSource) <= eps)
            continue;
        const double low = qMin(a.srcTime, b.srcTime) - eps;
        const double high = qMax(a.srcTime, b.srcTime) + eps;
        if (sourceOffset < low || sourceOffset > high)
            continue;
        const double u = (sourceOffset - a.srcTime) / deltaSource;
        const double candidate = a.outTime + u * (b.outTime - a.outTime);
        if (candidate < -eps || candidate > duration + eps)
            continue;
        if (found && qAbs(candidate - resolved) > eps)
            return false; // non-monotonic curve: source time has two answers
        found = true;
        resolved = candidate;
    }
    if (!found)
        return false;
    *localSecond = qBound(0.0, resolved, duration);
    return true;
}

bool clipSourceCoverage(const ClipInfo &clip,
                        double *sourceStart,
                        double *sourceEnd)
{
    if (!sourceStart || !sourceEnd || clip.speed <= 0.0
        || clip.effectiveDuration() <= 0.0) {
        return false;
    }

    const double duration = clip.effectiveDuration();
    double minimumOffset = 0.0;
    double maximumOffset = 0.0;
    if (!clip.hasTimeRemap()) {
        maximumOffset = duration * clip.speed;
    } else {
        const QVector<timeremap::TimeRemapKey> &keys = clip.timeRemapCurve.keys;
        if (keys.size() < 2)
            return false;
        minimumOffset = clip.timeRemapCurve.srcTimeAt(0.0);
        maximumOffset = minimumOffset;
        const auto includeOffset = [&](double value) {
            minimumOffset = qMin(minimumOffset, value);
            maximumOffset = qMax(maximumOffset, value);
        };
        includeOffset(clip.timeRemapCurve.srcTimeAt(duration));
        for (const timeremap::TimeRemapKey &key : keys) {
            if (key.outTime >= 0.0 && key.outTime <= duration)
                includeOffset(key.srcTime);
        }
    }

    *sourceStart = clip.inPoint + minimumOffset;
    *sourceEnd = clip.inPoint + maximumOffset;
    if (*sourceEnd < *sourceStart)
        std::swap(*sourceStart, *sourceEnd);
    return *sourceEnd > *sourceStart;
}

} // namespace

bool Timeline::mapSourceCaptionTrackToTimeline(
    const caption::Track &sourceTrack,
    const QString &sourcePath,
    caption::Track *mappedTrack,
    QString *errorMessage) const
{
    if (errorMessage)
        errorMessage->clear();
    if (!mappedTrack) {
        if (errorMessage)
            *errorMessage = tr("字幕の変換先がありません。");
        return false;
    }
    mappedTrack->clear();
    if (sourceTrack.clipCount() <= 0 || sourcePath.isEmpty()
        || m_videoTracks.isEmpty() || !m_videoTracks.first()) {
        if (errorMessage)
            *errorMessage = tr("字幕または V1 の元動画がありません。");
        return false;
    }

    const QString wantedPath = comparableMediaPath(sourcePath);
    const QVector<ClipInfo> &clips = m_videoTracks.first()->clips();
    double cursor = 0.0;
    bool matchedMedia = false;
    for (const ClipInfo &mediaClip : clips) {
        const double clipStart = cursor + qMax(0.0, mediaClip.leadInSec);
        cursor = clipStart + qMax(0.0, mediaClip.effectiveDuration());
        if (comparableMediaPath(mediaClip.filePath) != wantedPath)
            continue;
        matchedMedia = true;

        for (const caption::Clip &sourceCaption : sourceTrack.clips()) {
            double coverageStart = 0.0;
            double coverageEnd = 0.0;
            if (!clipSourceCoverage(mediaClip, &coverageStart, &coverageEnd))
                continue;
            const double clippedSourceStart = qMax(
                sourceCaption.startMs / 1000.0, coverageStart);
            const double clippedSourceEnd = qMin(
                sourceCaption.endMs / 1000.0, coverageEnd);
            if (clippedSourceEnd <= clippedSourceStart)
                continue;

            double localA = 0.0;
            double localB = 0.0;
            if (!sourceSecondToClipLocal(
                    mediaClip, clippedSourceStart, &localA)
                || !sourceSecondToClipLocal(
                    mediaClip, clippedSourceEnd, &localB)) {
                continue;
            }

            caption::Clip mapped = sourceCaption;
            const double mappedStart = clipStart + qMin(localA, localB);
            const double mappedEnd = clipStart + qMax(localA, localB);
            mapped.startMs = qRound64(mappedStart * 1000.0);
            mapped.endMs = qRound64(mappedEnd * 1000.0);
            if (mapped.endMs <= mapped.startMs)
                continue;

            QList<caption::Word> mappedWords;
            bool wordMappingFailed = false;
            if (localA <= localB) {
                mappedWords.reserve(sourceCaption.words.size());
                for (const caption::Word &sourceWord : sourceCaption.words) {
                    const double clippedWordStart = qMax(
                        sourceWord.startMs / 1000.0, clippedSourceStart);
                    const double clippedWordEnd = qMin(
                        sourceWord.endMs / 1000.0, clippedSourceEnd);
                    if (clippedWordEnd <= clippedWordStart)
                        continue;
                    double wordA = 0.0;
                    double wordB = 0.0;
                    if (!sourceSecondToClipLocal(
                            mediaClip, clippedWordStart, &wordA)
                        || !sourceSecondToClipLocal(
                            mediaClip, clippedWordEnd, &wordB)
                        || wordB <= wordA) {
                        mappedWords.clear();
                        wordMappingFailed = true;
                        break;
                    }
                    caption::Word word = sourceWord;
                    word.startMs = qRound64((clipStart + wordA) * 1000.0);
                    word.endMs = qRound64((clipStart + wordB) * 1000.0);
                    mappedWords.append(word);
                }
                // A segment carrying real word timings must never fall back
                // to its untrimmed full text when every timed word was cut
                // away or could not be mapped.  Dropping that fragment is
                // safer than regenerating words that lie outside the clip.
                if (!sourceCaption.words.isEmpty()
                    && (wordMappingFailed || mappedWords.isEmpty())) {
                    continue;
                }
                if (!mappedWords.isEmpty()
                    && mappedWords.size() != sourceCaption.words.size()) {
                    QStringList retainedText;
                    retainedText.reserve(mappedWords.size());
                    for (const caption::Word &word : std::as_const(mappedWords))
                        retainedText.append(word.text.trimmed());
                    mapped.text = retainedText.join(QLatin1Char(' '));
                }
            }
            // Reverse/non-invertible word order deliberately falls back to
            // segment-level captions instead of publishing stale word timing.
            mapped.words = mappedWords;
            mappedTrack->addClip(mapped);
        }
    }

    mappedTrack->sortByStart();
    if (!matchedMedia) {
        if (errorMessage)
            *errorMessage = tr("選択した元動画は V1 に見つかりません。");
        return false;
    }
    if (mappedTrack->clipCount() <= 0) {
        if (errorMessage)
            *errorMessage = tr("トリムまたはタイムリマップ範囲内に字幕がありません。");
        return false;
    }
    return true;
}

bool Timeline::applySingleWordCaptionOverlays(
    const QVector<EnhancedTextOverlay> &overlays, QString *errorMessage)
{
    if (errorMessage)
        errorMessage->clear();

    if (overlays.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("適用できる字幕がありません。");
        return false;
    }
    if (m_videoTracks.isEmpty() || !m_videoTracks.first()) {
        if (errorMessage)
            *errorMessage = tr("字幕を適用する V1 トラックがありません。");
        return false;
    }

    TimelineTrack *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("字幕を適用する動画クリップがありません。");
        return false;
    }

    QVector<EnhancedTextOverlay> generated;
    generated.reserve(overlays.size());
    for (EnhancedTextOverlay overlay : overlays) {
        if (overlay.text.trimmed().isEmpty()
            || !std::isfinite(overlay.startTime)
            || !std::isfinite(overlay.endTime)
            || overlay.startTime < 0.0
            || overlay.endTime <= overlay.startTime) {
            if (errorMessage)
                *errorMessage = tr("字幕の本文または表示時刻が不正です。");
            return false;
        }
        overlay.templateName = CaptionOverlayBuilder::generatedTemplateName();
        generated.append(overlay);
    }
    std::stable_sort(generated.begin(), generated.end(),
                     [](const EnhancedTextOverlay &a, const EnhancedTextOverlay &b) {
                         return a.startTime < b.startTime;
                     });

    for (int i = 1; i < generated.size(); ++i) {
        if (generated.at(i).startTime < generated.at(i - 1).endTime) {
            if (errorMessage)
                *errorMessage = tr("1語字幕の表示時刻が重複しています。");
            return false;
        }
    }

    const QString generatedTag = CaptionOverlayBuilder::generatedTemplateName();
    for (ClipInfo &clip : clips) {
        QVector<EnhancedTextOverlay> replacement;
        const QVector<EnhancedTextOverlay> &existing = clip.textManager.overlays();
        replacement.reserve(existing.size());
        for (const EnhancedTextOverlay &overlay : existing) {
            if (overlay.templateName != generatedTag)
                replacement.append(overlay);
        }
        clip.textManager.setOverlays(replacement);
    }

    m_generatedCaptionOverlays = generated;
    track->setClips(clips);
    saveUndoState(tr("1語字幕をタイムラインに適用"));
    refreshTextStrip();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::updateTextOverlayText(int overlayIndex, const QString &newText)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    const int ordinaryCount = clips[0].textManager.count();
    if (overlayIndex < 0
        || overlayIndex >= ordinaryCount + m_generatedCaptionOverlays.size()) {
        return false;
    }
    if (overlayIndex < ordinaryCount) {
        if (clips[0].textManager.overlay(overlayIndex).text == newText)
            return false;
        clips[0].textManager.overlay(overlayIndex).text = newText;
        track->setClips(clips);
    } else {
        const int generatedIndex = overlayIndex - ordinaryCount;
        if (newText.trimmed().isEmpty()
            || m_generatedCaptionOverlays.at(generatedIndex).text == newText) {
            return false;
        }
        m_generatedCaptionOverlays[generatedIndex].text = newText;
    }
    saveUndoState("Edit text overlay");
    refreshTextStrip();
    return true;
}

bool Timeline::updateTextOverlayRect(int overlayIndex, double x, double y, double width, double height)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    const int ordinaryCount = clips[0].textManager.count();
    if (overlayIndex < 0
        || overlayIndex >= ordinaryCount + m_generatedCaptionOverlays.size())
        return false;
    const double boundedWidth = qMax(0.0, width);
    const double boundedHeight = qMax(0.0, height);
    const EnhancedTextOverlay &current = overlayIndex < ordinaryCount
        ? clips[0].textManager.overlay(overlayIndex)
        : m_generatedCaptionOverlays.at(overlayIndex - ordinaryCount);
    if (current.x == x && current.y == y
        && current.width == boundedWidth
        && current.height == boundedHeight) {
        return false;
    }
    if (overlayIndex < ordinaryCount) {
        EnhancedTextOverlay &overlay = clips[0].textManager.overlay(overlayIndex);
        overlay.x = x;
        overlay.y = y;
        overlay.width = boundedWidth;
        overlay.height = boundedHeight;
        track->setClips(clips);
    } else {
        EnhancedTextOverlay &overlay =
            m_generatedCaptionOverlays[overlayIndex - ordinaryCount];
        overlay.x = x;
        overlay.y = y;
        overlay.width = boundedWidth;
        overlay.height = boundedHeight;
    }
    saveUndoState("Resize text overlay");
    refreshTextStrip();
    return true;
}

bool Timeline::applyTrackingToOverlay(int overlayIndex, const EnhancedTextOverlay &updated)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    const int ordinaryCount = clips[0].textManager.count();
    if (overlayIndex < 0
        || overlayIndex >= ordinaryCount + m_generatedCaptionOverlays.size())
        return false;
    EnhancedTextOverlay replacement = updated;
    if (overlayIndex >= ordinaryCount) {
        const EnhancedTextOverlay &current =
            m_generatedCaptionOverlays.at(overlayIndex - ordinaryCount);
        replacement.templateName = CaptionOverlayBuilder::generatedTemplateName();
        replacement.startTime = current.startTime;
        replacement.endTime = current.endTime;
    }
    if (overlayIndex < ordinaryCount) {
        clips[0].textManager.overlay(overlayIndex) = replacement;
        track->setClips(clips);
    } else {
        m_generatedCaptionOverlays[overlayIndex - ordinaryCount] = replacement;
    }
    saveUndoState("Apply tracking to overlay");
    refreshTextStrip();
    return true;
}

bool Timeline::setClipVideoTransform(int trackIdx, int clipIdx,
                                     double scale, double dx, double dy)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return false;
    auto *track = m_videoTracks[trackIdx];
    if (!track) return false;
    QVector<ClipInfo> clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return false;
    clips[clipIdx].videoScale = qBound(0.1, scale, 10.0);
    clips[clipIdx].videoDx = qBound(-5.0, dx, 5.0);
    clips[clipIdx].videoDy = qBound(-5.0, dy, 5.0);
    track->setClips(clips);
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::setClipMotion(int trackIdx, int clipIdx,
                             const effectctrl::MotionState &motion)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return;
    auto *track = m_videoTracks[trackIdx];
    if (!track)
        return;

    QVector<ClipInfo> clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return;

    ClipInfo &clip = clips[clipIdx];
    clip.videoScale = qBound(0.0, motion.scale, 10.0);
    // videoDx/videoDy are stored as NORMALIZED fractions of canvas W/H
    // (same ±5.0 convention as setClipVideoTransform and all renderers).
    // All callers pass already-normalized values — clamp only.
    clip.videoDx = qBound(-5.0, motion.dx, 5.0);
    clip.videoDy = qBound(-5.0, motion.dy, 5.0);
    clip.rotation2DDegrees = qBound(-360.0, motion.rotation2DDeg, 360.0);
    clip.opacity = qBound(0.0, motion.opacity, 1.0);
    clip.is3DLayer = motion.is3DLayer;
    clip.layer3D.positionZ = motion.is3DLayer
        ? qBound(-10000.0, motion.posZ, 10000.0) : 0.0;
    clip.layer3D.worldX = motion.is3DLayer
        ? qBound(-10000.0, motion.worldX, 10000.0) : 0.0;
    clip.layer3D.worldY = motion.is3DLayer
        ? qBound(-10000.0, motion.worldY, 10000.0) : 0.0;
    clip.layer3D.rotationX = motion.is3DLayer
        ? qBound(-360.0, motion.rotX, 360.0) : 0.0;
    clip.layer3D.rotationY = motion.is3DLayer
        ? qBound(-360.0, motion.rotY, 360.0) : 0.0;
    clip.layer3D.rotationZ = motion.is3DLayer
        ? qBound(-360.0, motion.rotZ, 360.0) : 0.0;

    track->setClips(clips);
    emitSequenceChangedNow();
    emit positionChanged(m_playheadPos);
}

bool Timeline::updateTextOverlayTime(int overlayIndex, double startTime,
                                     double endTime, bool createUndo)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return false;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;
    const int ordinaryCount = clips[0].textManager.count();
    if (overlayIndex < 0
        || overlayIndex >= ordinaryCount + m_generatedCaptionOverlays.size())
        return false;

    double boundedStart = qMax(0.0, startTime);
    double boundedEnd = qMax(boundedStart + 0.01, endTime);
    const bool generated = overlayIndex >= ordinaryCount;
    const int generatedIndex = overlayIndex - ordinaryCount;
    const EnhancedTextOverlay &current = generated
        ? m_generatedCaptionOverlays.at(generatedIndex)
        : clips[0].textManager.overlay(overlayIndex);
    if (generated) {
        // Preserve the sorted, non-overlapping generated-caption suffix used
        // by the O(log words) preview/export lookup. A word can be resized up
        // to its neighbours but cannot cross them.
        if (generatedIndex > 0)
            boundedStart = qMax(
                boundedStart, m_generatedCaptionOverlays.at(generatedIndex - 1).endTime);
        if (generatedIndex + 1 < m_generatedCaptionOverlays.size())
            boundedEnd = qMin(
                boundedEnd, m_generatedCaptionOverlays.at(generatedIndex + 1).startTime);
        if (boundedEnd <= boundedStart)
            return false;
    }
    if (current.startTime == boundedStart && current.endTime == boundedEnd)
        return false;
    if (generated) {
        m_generatedCaptionOverlays[generatedIndex].startTime = boundedStart;
        m_generatedCaptionOverlays[generatedIndex].endTime = boundedEnd;
    } else {
        clips[0].textManager.overlay(overlayIndex).startTime = boundedStart;
        clips[0].textManager.overlay(overlayIndex).endTime = boundedEnd;
        track->setClips(clips);
    }
    if (createUndo || !m_textOverlayTimeEditActive) {
        saveUndoState(tr("字幕の表示時間を変更"));
    } else {
        m_textOverlayTimeEditChanged = true;
    }
    refreshTextStrip();
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::beginTextOverlayTimeEdit()
{
    m_textOverlayTimeEditActive = true;
    m_textOverlayTimeEditChanged = false;
}

void Timeline::finishTextOverlayTimeEdit()
{
    if (m_textOverlayTimeEditActive && m_textOverlayTimeEditChanged)
        saveUndoState(tr("字幕の表示時間を変更"));
    m_textOverlayTimeEditActive = false;
    m_textOverlayTimeEditChanged = false;
}

void Timeline::refreshTextStrip()
{
    if (!m_textStrip)
        return;
    const QVector<EnhancedTextOverlay> overlays = timelineTextOverlays();
    const double clipDur = totalDuration();
    m_textStrip->setOverlays(overlays, clipDur);
    m_textStrip->setPixelsPerSecond(m_zoomLevel);
}

void Timeline::copySelectedClip()
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return;
    m_clipboard = m_videoTrack->clips()[sel];
    // US-1203: keep the linked audio at its offset (a J / L cut) for a
    // storyline-mode paste.
    m_clipboardLinkedAudio.clear();
    if (m_clipboard->linkGroup <= 0) return;
    const double videoStartSec = clipStartSecs(m_videoTrack->clips())[sel];
    for (const TimelineTrack *track : m_audioTracks) {
        if (!track) continue;
        const auto starts = clipStartSecs(track->clips());
        for (int i = 0; i < track->clipCount(); ++i)
            if (track->clips().at(i).linkGroup == m_clipboard->linkGroup)
                m_clipboardLinkedAudio.append({track->clips().at(i), starts[i] - videoStartSec});
    }
}

ClipInfo Timeline::copyClipForPaste(const ClipInfo &source, QHash<int, int> &groups)
{
    ClipInfo result = source;
    // A paste / duplicate is a new clip with its own identity.
    result.uid.clear();
    TimelineTrack::ensureUid(result);
    // US-1214: and without the source's primary-storyline connection.
    result.anchorUid.clear();
    result.anchorOffsetSec = 0.0;
    if (source.linkGroup <= 0) return result;
    if (!groups.contains(source.linkGroup)) {
        QSet<int> used;
        for (auto *track : timelineTracks(this))
            if (track) for (const auto &clip : track->clips()) used.insert(clip.linkGroup);
        for (auto it = groups.cbegin(); it != groups.cend(); ++it) used.insert(it.value());
        int fresh = allocateLinkGroup();
        while (used.contains(fresh)) fresh = allocateLinkGroup();
        groups.insert(source.linkGroup, fresh);
    }
    result.linkGroup = groups.value(source.linkGroup);
    return result;
}

void Timeline::pasteClip()
{
    if (!m_clipboard.has_value()) return;
    if (!m_videoTrack || !m_audioTrack) return;
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    // US-1203: storyline mode inserts into V1 only, after the selected V1 clip
    // (else at the end); V1 linked audio follows and the pasted audio keeps its
    // offset from the pasted clip, moving to a free audio track on a collision.
    if (m_storylineMode && !m_videoTrack->isLocked()) {
        // The pasted audio goes to an unlocked audio track; with none, nothing changes.
        TimelineTrack *firstUnlockedAudio = nullptr;
        for (TimelineTrack *t : m_audioTracks) {
            if (t && !t->isLocked()) {
                firstUnlockedAudio = t;
                break;
            }
        }
        if (!firstUnlockedAudio) return;
        closeGapsOnTrack(m_videoTrack);
        const int sel = m_videoTrack->selectedClip();
        const auto starts = clipStartSecs(m_videoTrack->clips());
        const double atSec = sel >= 0 && sel < m_videoTrack->clipCount()
            ? starts[sel] + m_videoTrack->clips().at(sel).effectiveDuration()
            : (starts.isEmpty() ? 0.0
                                : starts.last() + m_videoTrack->clips().last().effectiveDuration());
        QHash<int, int> groups;
        const ClipInfo pastedVideo = copyClipForPaste(m_clipboard.value(), groups);
        double landedSec = 0.0;
        const int index = storylineInsertClip(m_videoTrack, atSec, pastedVideo, &landedSec);
        followStorylineLinks();
        QVector<QPair<ClipInfo, double>> audio = m_clipboardLinkedAudio;
        if (audio.isEmpty()) audio.append({m_clipboard.value(), 0.0});
        for (const auto &partner : audio) {
            const ClipInfo pastedAudio = copyClipForPaste(partner.first, groups);
            const double startSec = qMax(0.0, landedSec + partner.second);
            TimelineTrack::DropPlan plan;
            TimelineTrack *destination = storylineAudioSlot(0, pastedAudio, startSec, &plan);
            if (!destination) {
                // No free audio track: the end of the first unlocked audio track
                // (reconcile never adds tracks).
                destination = firstUnlockedAudio;
                double endSec = 0.0;
                for (const ClipInfo &c : destination->clips())
                    endSec += c.leadInSec + c.effectiveDuration();
                plan = destination->planDrop(endSec, pastedAudio.effectiveDuration());
            }
            destination->insertClipPreservingDownstream(plan.insertIdx, pastedAudio, plan.newLeadIn);
        }
        m_videoTrack->setSelectedClip(index);
        remapTrackMatteLocked(snapBefore);
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
        saveUndoState("Paste clip");
        updateInfoLabel();
        return;
    }
    // C-04-base: the pasted V/A pair is written to both V1 and A1, so a locked
    // either one refuses the whole paste (also reached in storyline mode when V1 is locked).
    if (m_videoTrack->isLocked() || m_audioTrack->isLocked()) {
        emit statusMessageRequested(QStringLiteral("ロック中のトラックには貼り付けできません。"), 3000);
        return;
    }
    int insertAt = m_videoTrack->selectedClip() + 1;
    if (insertAt <= 0) insertAt = m_videoTrack->clipCount();
    const int maxIndex = qMin(m_videoTrack->clipCount(), m_audioTrack->clipCount());
    insertAt = qBound(0, insertAt, maxIndex);
    // Give the pasted V/A pair a fresh linkGroup so it forms its own link
    // instead of aliasing the source clip's group (which would drag/select the
    // original together with the copy).
    QHash<int, int> groups;
    const ClipInfo pastedVideo = copyClipForPaste(m_clipboard.value(), groups);
    // Same linkGroup through `groups`, own uid for the audio partner.
    const ClipInfo pastedAudio = copyClipForPaste(m_clipboard.value(), groups);
    m_videoTrack->insertClip(insertAt, pastedVideo);
    m_audioTrack->insertClip(insertAt, pastedAudio);
    m_videoTrack->setSelectedClip(insertAt);
    m_audioTrack->setSelectedClip(insertAt);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState("Paste clip");
    updateInfoLabel();
}

void Timeline::cutSelectedClip()
{
    copySelectedClip();
    deleteSelectedClip();
}

void Timeline::unlinkClipGroup(int linkGroup)
{
    if (linkGroup <= 0) return;
    bool changed = false;
    auto strip = [&](TimelineTrack *t) {
        if (!t) return;
        auto clips = t->clips();
        bool local = false;
        for (auto &c : clips) {
            if (c.linkGroup == linkGroup) {
                c.linkGroup = 0;
                local = true;
            }
        }
        if (local) {
            t->setClips(clips);
            changed = true;
        }
    };
    for (auto *t : m_videoTracks) strip(t);
    for (auto *t : m_audioTracks) strip(t);
    if (changed) {
        saveUndoState("Unlink clip");
        updateInfoLabel();
        // US-1505: the audio-level snapshot (linkGroup) follows, as relinkClipAt does.
        scheduleEmitSequenceChanged();
    }
}

void Timeline::relinkClipAt(TimelineTrack *track, int clipIndex)
{
    if (!track || clipIndex < 0 || clipIndex >= track->clips().size()) return;

    const auto clickedClips = track->clips();
    const ClipInfo &clicked = clickedClips[clipIndex];
    if (clicked.linkGroup > 0) return;  // already linked — nothing to do

    auto clipTimelineStart = [](const QVector<ClipInfo> &clips, int idx) {
        double accum = 0.0;
        for (int i = 0; i <= idx && i < clips.size(); ++i) {
            accum += qMax(0.0, clips[i].leadInSec);
            if (i == idx) return accum;
            accum += clips[i].effectiveDuration();
        }
        return accum;
    };
    const double clickedStart = clipTimelineStart(clickedClips, clipIndex);

    // Find the best counterpart on the OPPOSITE track family (V→A or A→V).
    // Two-pass search: prefer UNLINKED candidates first (same filePath,
    // smallest |timelineStart - clickedStart|). Only fall back to an
    // already-linked candidate if no unlinked match exists — otherwise a
    // duplicate-imported clip or a split-clip neighbor could silently steal
    // the re-sync from the real counterpart.
    const bool clickedIsVideo = m_videoTracks.contains(track);
    const auto &oppositeTracks = clickedIsVideo ? m_audioTracks : m_videoTracks;

    struct Hit { TimelineTrack *track = nullptr; int idx = -1; double start = 0.0; };
    auto findBest = [&](bool requireUnlinked) {
        Hit best;
        double bestDist = 1e18;
        for (auto *t : oppositeTracks) {
            if (!t) continue;
            const auto &clips = t->clips();
            double accum = 0.0;
            for (int i = 0; i < clips.size(); ++i) {
                accum += qMax(0.0, clips[i].leadInSec);
                const double start = accum;
                accum += clips[i].effectiveDuration();
                if (clips[i].filePath != clicked.filePath) continue;
                if (requireUnlinked && clips[i].linkGroup > 0) continue;
                const double dist = qAbs(start - clickedStart);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = {t, i, start};
                }
            }
        }
        return best;
    };

    Hit hit = findBest(/*requireUnlinked=*/true);
    if (!hit.track) hit = findBest(/*requireUnlinked=*/false);
    if (!hit.track) return;

    // Snap the counterpart's timelineStart onto the clicked clip's start by
    // shifting its leadInSec, and compensate the NEXT clip's leadInSec so
    // downstream clips stay put (same trick as the linked-drag path). If
    // the move is clamped at 0 because the counterpart butts up against a
    // preceding neighbor, we settle for a partial alignment — the clips are
    // still re-grouped so further linked drags will track together.
    {
        auto clips = hit.track->clips();
        const double delta = clickedStart - hit.start;
        const double oldLeadIn = clips[hit.idx].leadInSec;
        const double newLeadIn = qMax(0.0, oldLeadIn + delta);
        const double applied = newLeadIn - oldLeadIn;
        clips[hit.idx].leadInSec = newLeadIn;
        if (hit.idx + 1 < clips.size()) {
            clips[hit.idx + 1].leadInSec =
                qMax(0.0, clips[hit.idx + 1].leadInSec - applied);
        }
        hit.track->setClips(clips);
    }

    const int targetGroup = (hit.track->clips()[hit.idx].linkGroup > 0)
        ? hit.track->clips()[hit.idx].linkGroup
        : allocateLinkGroup();

    auto stamp = [targetGroup](TimelineTrack *t, int idx) {
        auto clips = t->clips();
        if (idx < 0 || idx >= clips.size()) return;
        clips[idx].linkGroup = targetGroup;
        t->setClips(clips);
    };
    stamp(track, clipIndex);
    stamp(hit.track, hit.idx);

    saveUndoState("Relink clip");
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
    updateInfoLabel();
}

// PV-B SSOT: クリップ操作をプレビュー右クリックメニューと共有するため公開
// メソッドへ抽出。showClipContextMenu の各分岐と同一ロジック(挙動不変)。
void Timeline::applySnsFitToClip(TimelineTrack *track, int clipIndex,
                                 bool contain, bool cover, const QString &undoLabel)
{
    if (!track) return;
    QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size())
        return;
    ClipInfo &clip = clips[clipIndex];
    clip.fitContain = contain;
    clip.fitCover = cover;
    clip.videoScale = 1.0;
    clip.videoDx = 0.0;
    clip.videoDy = 0.0;
    clip.rotation2DDegrees = 0.0;
    track->setClips(clips);
    saveUndoState(undoLabel);
    emitSequenceChangedNow();
    emit positionChanged(m_playheadPos);
}

void Timeline::applySilenceCutToClip(TimelineTrack *track, int clipIndex)
{
    if (!track) return;
    QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size()) return;
    const ClipInfo &src = clips[clipIndex];

    QVector<float> samples;
    int sr = 0;
    if (!WaveformGenerator::decodeAudio(src.filePath, samples, sr) || samples.isEmpty() || sr <= 0) {
        QMessageBox::warning(nullptr, QStringLiteral("無音カット"),
                             QStringLiteral("音声のデコードに失敗しました。"));
        return;
    }

    const double srcOut = (src.outPoint > 0.0) ? src.outPoint : src.duration;
    const double totalSec = static_cast<double>(samples.size()) / sr;
    const double activeStart = std::max(src.inPoint, 0.0);
    const double activeEnd   = std::min(srcOut, totalSec);
    if (activeEnd <= activeStart) {
        QMessageBox::information(nullptr, QStringLiteral("無音カット"),
                                 QStringLiteral("有効な範囲がありません。"));
        return;
    }
    const int startSample = static_cast<int>(activeStart * sr);
    const int endSample   = static_cast<int>(activeEnd   * sr);
    QVector<float> activeSamples = samples.mid(startSample, endSample - startSample);

    QVector<silencecut::Segment> keeps =
        silencecut::detectKeepSegments(activeSamples, sr, silencecut::Config{});
    QVector<ClipInfo> subClips = silencecut::planKeepClips(src, keeps);

    if (subClips.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("無音カット"),
                                 QStringLiteral("カットする無音が見つかりませんでした。"));
        return;
    }
    if (subClips.size() == 1
        && qAbs(subClips[0].inPoint  - src.inPoint) < 0.01
        && qAbs(subClips[0].outPoint - srcOut)      < 0.01) {
        QMessageBox::information(nullptr, QStringLiteral("無音カット"),
                                 QStringLiteral("カットする無音がありません。"));
        return;
    }

    const QVector<silencecut::Segment> silences =
        silencecut::detectSilenceSegments(activeSamples, sr, silencecut::Config{});
    const int silenceCount = static_cast<int>(silences.size());
    const QString msg = QStringLiteral("%1 箇所の無音を除去し、%2 個のクリップに分割します。実行しますか?")
                            .arg(silenceCount)
                            .arg(subClips.size());
    if (QMessageBox::question(nullptr, QStringLiteral("無音カット"), msg)
            != QMessageBox::Yes)
        return;

    for (int i = 0; i < subClips.size(); ++i) {
        if (i > 0)
            subClips[i].leadInSec = 0.0;
        subClips[i].linkGroup = allocateLinkGroup();
    }
    if (track == m_videoTracks.value(0, nullptr) && !src.uid.isEmpty()) {
        // US-1214: the pieces of a V1 anchor are its splits (the first keeps the
        // uid) and the removed spans detach their connections; both in the
        // anchor's local seconds.
        const double speed = src.speed > 0.0 ? src.speed : 1.0;
        double removedFrom = 0.0;
        double pieceLocal = 0.0;
        for (int i = 0; i < subClips.size(); ++i) {
            const double localStart = (subClips[i].inPoint - src.inPoint) / speed;
            if (i > 0) {
                subClips[i].uid.clear();
                TimelineTrack::ensureUid(subClips[i]);
                m_pendingSplits.append({subClips[i - 1].uid, subClips[i].uid, localStart - pieceLocal});
                pieceLocal = localStart;
            }
            if (localStart > removedFrom + 1e-6)
                m_pendingDetachedSpans.append({src.uid, removedFrom, localStart});
            removedFrom = localStart + subClips[i].effectiveDuration();
        }
        if (src.effectiveDuration() > removedFrom + 1e-6)
            m_pendingDetachedSpans.append({src.uid, removedFrom, src.effectiveDuration()});
    }

    QVector<ClipInfo> newClips = clips;
    newClips.removeAt(clipIndex);
    for (int i = 0; i < subClips.size(); ++i)
        newClips.insert(clipIndex + i, subClips[i]);

    track->setClips(newClips);
    saveUndoState(QStringLiteral("Silence auto-cut"));
    emitSequenceChangedNow();
    emit positionChanged(m_playheadPos);
}

void Timeline::applyBeatMarkersToClip(TimelineTrack *track, int clipIndex)
{
    if (!track) return;
    const QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size()) return;
    const ClipInfo &src = clips[clipIndex];

    QVector<float> samples;
    int sr = 0;
    if (!WaveformGenerator::decodeAudio(src.filePath, samples, sr) || samples.isEmpty() || sr <= 0) {
        QMessageBox::warning(nullptr, QStringLiteral("ビートマーカー"),
                             QStringLiteral("音声のデコードに失敗しました。"));
        return;
    }

    const double srcOut = (src.outPoint > 0.0) ? src.outPoint : src.duration;
    const double totalSec = static_cast<double>(samples.size()) / sr;
    const double activeStart = std::max(src.inPoint, 0.0);
    const double activeEnd   = std::min(srcOut, totalSec);
    if (activeEnd <= activeStart) {
        QMessageBox::information(nullptr, QStringLiteral("ビートマーカー"),
                                 QStringLiteral("有効な範囲がありません。"));
        return;
    }
    const int startSample = static_cast<int>(activeStart * sr);
    const int endSample   = static_cast<int>(activeEnd   * sr);
    const QVector<float> activeSamples = samples.mid(startSample, endSample - startSample);

    const beatdetect::Result beats =
        beatdetect::detectBeats(activeSamples, sr, beatdetect::Config{});
    if (beats.beatTimesSec.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("ビートマーカー"),
                                 QStringLiteral("ビートが検出されませんでした。"));
        return;
    }

    double clipStartSec = 0.0;
    for (int i = 0; i < clipIndex && i < clips.size(); ++i)
        clipStartSec += clips[i].leadInSec + clips[i].effectiveDuration();
    clipStartSec += src.leadInSec;
    const double speed = (src.speed > 0.0) ? src.speed : 1.0;

    const QString msg = QStringLiteral("%1 個のビート (推定 %2 BPM) をマーカーとして追加しますか?")
                            .arg(beats.beatTimesSec.size())
                            .arg(beats.bpm, 0, 'f', 1);
    if (QMessageBox::question(nullptr, QStringLiteral("ビートマーカー"), msg)
            != QMessageBox::Yes)
        return;

    const QColor beatColor(QStringLiteral("#39c0ff"));
    int added = 0;
    for (double b : beats.beatTimesSec) {
        const double sourceSec = activeStart + b;
        const double timelineSec = clipStartSec + (sourceSec - src.inPoint) / speed;
        if (timelineSec < 0.0)
            continue;
        const qint64 timelineUs = static_cast<qint64>(timelineSec * 1.0e6);
        addMarker(timelineUs, QStringLiteral("Beat %1").arg(added + 1), beatColor);
        ++added;
    }
    saveUndoState(QStringLiteral("Beat markers"));
    if (added > 0)
        emit positionChanged(m_playheadPos);
}

bool Timeline::applyMusicRemix(int trackIndex, int clipIndex,
                               const remix::Plan &plan, bool ripple,
                               QString *errorOut)
{
    if (errorOut)
        errorOut->clear();
    const auto fail = [errorOut](const QString &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    if (!plan.error.isEmpty() || plan.segments.isEmpty())
        return fail(plan.error.isEmpty()
                        ? QStringLiteral("リミックス計画が空です") : plan.error);
    if (plan.segments.size() > remix::kMaxSegments)
        return fail(QStringLiteral("リミックス区間数が上限 (%1) を超えています")
                        .arg(remix::kMaxSegments));
    if (!std::isfinite(plan.resultDuration) || plan.resultDuration <= 0.0)
        return fail(QStringLiteral("リミックス後の尺が不正です"));
    if (!std::isfinite(plan.crossfadeSec) || plan.crossfadeSec < 0.0)
        return fail(QStringLiteral("クロスフェード時間が不正です"));

    TimelineTrack *track = trackAt(true, trackIndex);
    if (!track)
        return fail(QStringLiteral("音声トラックの index が範囲外です"));
    if (clipIndex < 0 || clipIndex >= track->clipCount())
        return fail(QStringLiteral("音声クリップの index が範囲外です"));
    if (track->isLocked())
        return fail(QStringLiteral("音声トラックがロックされています"));

    const ClipInfo original = track->clips().at(clipIndex);
    const double sourceOut = original.outPoint > 0.0
        ? original.outPoint : original.duration;
    const double sourceSpan = sourceOut - original.inPoint;
    const double speed = original.speed > 0.0 ? original.speed : 1.0;
    const double clipDuration = sourceSpan / speed;
    if (!std::isfinite(clipDuration) || clipDuration <= 0.0)
        return fail(QStringLiteral("対象クリップの尺が不正です"));

    QVector<ClipInfo> replacement;
    replacement.reserve(plan.segments.size());
    for (const remix::Segment &segment : plan.segments) {
        if (!std::isfinite(segment.srcStart) || !std::isfinite(segment.srcEnd)
            || segment.srcStart < -1.0e-6
            || segment.srcEnd > clipDuration + 1.0e-6
            || segment.srcEnd <= segment.srcStart + 1.0e-9) {
            return fail(QStringLiteral("リミックス区間が対象クリップ外です"));
        }
        ClipInfo part = original;
        const double localStart = qBound(0.0, segment.srcStart, clipDuration);
        const double localEnd = qBound(0.0, segment.srcEnd, clipDuration);
        part.inPoint = original.inPoint + localStart * speed;
        part.outPoint = original.inPoint + localEnd * speed;
        part.leadInSec = replacement.isEmpty() ? original.leadInSec : 0.0;
        part.leadIn = Transition{};
        part.trailOut = Transition{};
        replacement.append(part);
    }
    if (replacement.isEmpty())
        return fail(QStringLiteral("リミックス区間がありません"));

    const double actualDuration = std::accumulate(
        replacement.cbegin(), replacement.cend(), 0.0,
        [](double sum, const ClipInfo &clip) {
            return sum + qMax(0.0, clip.effectiveDuration());
        });
    if (!std::isfinite(actualDuration) || actualDuration <= 0.0)
        return fail(QStringLiteral("リミックス後の尺が不正です"));

    const QVector<ClipInfo> before = track->clips();
    const int nextIndexBefore = clipIndex + 1;
    const bool hasNext = nextIndexBefore < before.size();
    const double nextLeadIn = hasNext ? before[nextIndexBefore].leadInSec : 0.0;
    const double downstreamLeadIn = nextLeadIn + original.effectiveDuration()
                                  - actualDuration;
    if (!ripple && hasNext && downstreamLeadIn < -1.0e-6)
        return fail(QStringLiteral(
            "後続クリップの空きが足りません。「後続をリップル」を有効にしてください"));

    const Transition originalLeadIn = original.leadIn;
    const Transition originalTrailOut = original.trailOut;
    for (int i = 0; i + 1 < replacement.size(); ++i) {
        double duration = qMin(plan.crossfadeSec,
                               replacement[i].effectiveDuration());
        duration = qMin(duration, replacement[i + 1].effectiveDuration());
        if (duration <= 0.0)
            continue;
        Transition crossfade;
        crossfade.type = TransitionType::CrossDissolve;
        crossfade.duration = duration;
        replacement[i].trailOut = crossfade;
        replacement[i + 1].leadIn = crossfade;
    }
    replacement.first().leadIn = originalLeadIn;
    replacement.last().trailOut = originalTrailOut;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> after;
    after.reserve(before.size() - 1 + replacement.size());
    for (int i = 0; i < clipIndex; ++i)
        after.append(before.at(i));
    after += replacement;
    for (int i = clipIndex + 1; i < before.size(); ++i)
        after.append(before.at(i));
    const int nextIndexAfter = clipIndex + replacement.size();
    if (hasNext && nextIndexAfter < after.size() && !ripple)
        after[nextIndexAfter].leadInSec = qMax(0.0, downstreamLeadIn);
    track->setClips(after);
    track->setSelectedClip(clipIndex);

    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("ミュージックリミックス"));
    updateInfoLabel();
    ensureSequenceFitsViewport();
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::applyDialogueLevel(
    int trackIndex, int clipIndex,
    const QVector<AudioGainPoint> &envelope, QString *errorOut)
{
    if (errorOut)
        errorOut->clear();
    const auto fail = [errorOut](const QString &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    TimelineTrack *track = trackAt(true, trackIndex);
    if (!track)
        return fail(QStringLiteral("音声トラックの index が範囲外です"));
    if (clipIndex < 0 || clipIndex >= track->clipCount())
        return fail(QStringLiteral("音声クリップの index が範囲外です"));
    if (track->isLocked())
        return fail(QStringLiteral("音声トラックがロックされています"));
    if (envelope.isEmpty())
        return fail(QStringLiteral("音量エンベロープが空です"));

    const double duration = track->clips().at(clipIndex).effectiveDuration();
    if (!std::isfinite(duration) || duration <= 0.0)
        return fail(QStringLiteral("対象クリップの尺が不正です"));
    double previousTime = -1.0;
    for (const AudioGainPoint &point : envelope) {
        if (!std::isfinite(point.time) || !std::isfinite(point.gain)
            || point.time < 0.0 || point.time > duration + 1.0e-6
            || point.time + 1.0e-12 < previousTime || point.gain < 0.0) {
            return fail(QStringLiteral("音量エンベロープが不正です"));
        }
        previousTime = point.time;
    }

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = track->clips();
    clips[clipIndex].volumeEnvelope = envelope;
    track->setClips(clips);
    track->setSelectedClip(clipIndex);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("ダイアログレベラー"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

// 再生ヘッド直下の V1(最初の動画トラック)クリップを解決。見つかれば true。
bool Timeline::clipUnderPlayhead(TimelineTrack *&outTrack, int &outClipIndex) const
{
    outTrack = nullptr;
    outClipIndex = -1;
    if (m_videoTracks.isEmpty())
        return false;
    TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    if (!v1)
        return false;
    const QVector<ClipInfo> &clips = v1->clips();
    const double playSec = m_playheadPos;
    double start = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        const double clipStart = start + clips[i].leadInSec;
        const double clipEnd = clipStart + clips[i].effectiveDuration();
        if (playSec >= clipStart && playSec < clipEnd) {
            outTrack = v1;
            outClipIndex = i;
            return true;
        }
        start = clipEnd;
    }
    return false;
}

void Timeline::showClipContextMenu(TimelineTrack *track, int clipIndex, const QPoint &globalPos)
{
    if (!track || clipIndex < 0 || clipIndex >= track->clips().size()) return;

    // Make sure the right-clicked clip is the active selection — NLE users
    // expect "right click acts on the thing under the cursor".
    if (!track->isClipSelected(clipIndex))
        track->setSelectedClip(clipIndex);

    auto applyLabelToSelection = [&](ClipLabel label) {
        const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
        bool changed = false;
        const QVector<TimelineTrack *> tracks = m_videoTracks + m_audioTracks;
        for (TimelineTrack *selectedTrack : tracks) {
            if (!selectedTrack || selectedTrack->isLocked())
                continue;
            QVector<ClipInfo> clips = selectedTrack->clips();
            bool trackChanged = false;
            for (int selectedIndex : selectedTrack->selectedClips()) {
                if (selectedIndex < 0 || selectedIndex >= clips.size()
                    || clips[selectedIndex].label == label) {
                    continue;
                }
                clips[selectedIndex].label = label;
                trackChanged = true;
            }
            if (trackChanged) {
                selectedTrack->setClips(clips);
                changed = true;
            }
        }
        if (!changed)
            return;
        remapTrackMatteLocked(snapBefore);
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
        saveUndoState(QStringLiteral("Clip label"));
        updateInfoLabel();
        scheduleEmitSequenceChanged();
    };

    if (QSettings("VSimpleEditor", "Preferences")
            .value(rcpause::pauseOnRightClickKey(),
                   rcpause::kDefaultPauseOnRightClick).toBool()) {
        if (QWidget *topLevel = window()) {
            if (auto *player = topLevel->findChild<VideoPlayer *>())
                player->pause();
        }
    }

    // Audio-track context menu — visual transitions don't apply, so we
    // show a simplified menu with just the audio-relevant transitions
    // (FadeIn/Out + equal-power CrossDissolve) plus the standard
    // edit ops. AudioMixer reads leadIn/trailOut directly from the audio
    // clip so setting these here gives an audio-only fade with no video
    // mirror. Used for J-cut / L-cut workflows where audio fades earlier
    // or later than the video cut.
    if (m_audioTracks.contains(track)) {
        const ClipInfo &aClip = track->clips()[clipIndex];
        const int aLinkGroup = aClip.linkGroup;
        const bool aHasTrans = aClip.leadIn.type != TransitionType::None
                            || aClip.trailOut.type != TransitionType::None;
        QMenu aMenu;
        QAction *aCut = aMenu.addAction(QStringLiteral("カット"));
        QAction *aCopy = aMenu.addAction(QStringLiteral("コピー"));
        QAction *aDel = aMenu.addAction(QStringLiteral("削除"));
        QAction *aReplace = aMenu.addAction(QStringLiteral("置き換え"));
        aMenu.addSeparator();
        QAction *aUnlink = aMenu.addAction(QStringLiteral("同期を切る"));
        aUnlink->setEnabled(aLinkGroup > 0);
        aMenu.addSeparator();
        QAction *aNormalize = aMenu.addAction(QStringLiteral("ノーマライズ"));
        QAction *aDialogueLevel = aMenu.addAction(
            QStringLiteral("ダイアログレベラー…"));
        QAction *aRemix = aMenu.addAction(QStringLiteral("ミュージックリミックス…"));
        aMenu.addSeparator();
        QMenu *aChannelMenu = aMenu.addMenu(QStringLiteral("チャンネルマッピング"));
        QAction *aStereoAct = addAudioChannelModeAction(aChannelMenu, aClip.audioChannelMode,
                                                        AudioChannelMode::Stereo);
        QAction *aFillLeftAct = addAudioChannelModeAction(aChannelMenu, aClip.audioChannelMode,
                                                          AudioChannelMode::FillLeft);
        QAction *aFillRightAct = addAudioChannelModeAction(aChannelMenu, aClip.audioChannelMode,
                                                           AudioChannelMode::FillRight);
        QAction *aSwapAct = addAudioChannelModeAction(aChannelMenu, aClip.audioChannelMode,
                                                      AudioChannelMode::Swap);
        QAction *aMonoAct = addAudioChannelModeAction(aChannelMenu, aClip.audioChannelMode,
                                                      AudioChannelMode::Mono);
        aMenu.addSeparator();
        QAction *aClipEq = aMenu.addAction(QStringLiteral("クリップ EQ…"));
        QAction *aEnabledAction = aMenu.addAction(QStringLiteral("クリップを有効にする"));
        aEnabledAction->setCheckable(true);
        aEnabledAction->setChecked(track->clips()[clipIndex].visible);
        aEnabledAction->setEnabled(!track->isLocked() && !clipgeom::isNullObjectFilePath(track->clips()[clipIndex].filePath));
        QMenu *aLabelMenu = aMenu.addMenu(QStringLiteral("ラベルカラー"));
        const QList<QPair<QAction *, ClipLabel>> aLabelActions{
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::None), ClipLabel::None},
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::Red), ClipLabel::Red},
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::Orange), ClipLabel::Orange},
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::Yellow), ClipLabel::Yellow},
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::Green), ClipLabel::Green},
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::Cyan), ClipLabel::Cyan},
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::Blue), ClipLabel::Blue},
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::Purple), ClipLabel::Purple},
            {addClipLabelAction(aLabelMenu, aClip.label, ClipLabel::Pink), ClipLabel::Pink}
        };
        aMenu.addSeparator();
        QMenu *atMenu = aMenu.addMenu(QStringLiteral("音声トランジション"));
        QAction *aXdAct = atMenu->addAction(QStringLiteral("クロスフェード (コンスタントパワー)…"));
        aXdAct->setEnabled(clipIndex + 1 < track->clips().size());
        QAction *aFiAct = atMenu->addAction(QStringLiteral("フェードイン…"));
        QAction *aFoAct = atMenu->addAction(QStringLiteral("フェードアウト…"));
        QAction *aClearAct = nullptr;
        if (aHasTrans) {
            atMenu->addSeparator();
            aClearAct = atMenu->addAction(QStringLiteral("音声トランジション削除"));
        }
        QAction *aSequence = aMenu.addAction(QStringLiteral("このクリップから新規シーケンス"));
        aSequence->setEnabled(aClip.sequenceRefId.isEmpty() && !aClip.filePath.isEmpty());
        const ClipInfo sequenceSource = aClip;
        // US-1202: primary-storyline connection of an audio clip (no shortcut).
        aMenu.addSeparator();
        QAction *aConnect = aMenu.addAction(tr("主ストーリーラインに接続"));
        QAction *aDisconnect = aMenu.addAction(tr("接続を解除"));
        aDisconnect->setEnabled(!aClip.anchorUid.isEmpty());
        QAction *aChosen = aMenu.exec(globalPos);
        if (aChosen == aSequence) {
            createSequenceFromClip(sequenceSource);
            return;
        }
        if (!aChosen) return;
        if (aChosen == aConnect || aChosen == aDisconnect) {
            QString connErr;
            const int audioIdx = m_audioTracks.indexOf(track);
            const bool ok = aChosen == aConnect
                ? connectToStoryline(TrackKind::Audio, audioIdx, clipIndex, &connErr)
                : disconnectClip(TrackKind::Audio, audioIdx, clipIndex, &connErr);
            if (!ok) emit statusMessageRequested(connErr, 4000);
            return;
        }
        if (aChosen == aEnabledAction) {
            setClipEnabled(TrackKind::Audio, m_audioTracks.indexOf(track), clipIndex, aEnabledAction->isChecked());
            return;
        }

        auto applyAudioAction = [&](AudioFadeEdge edge, bool crossfade) {
            bool accepted = false;
            const double defaultDuration = crossfade ? 1.0 : 0.5;
            const QString title = crossfade
                ? QStringLiteral("コンスタントパワー・クロスフェード")
                : (edge == AudioFadeEdge::In
                    ? QStringLiteral("音声フェードイン")
                    : QStringLiteral("音声フェードアウト"));
            const double duration = QInputDialog::getDouble(
                this, title, QStringLiteral("時間 (秒):"), defaultDuration,
                0.01, 60.0, 2, &accepted);
            if (!accepted)
                return;
            QString error;
            const int audioTrackIndex = m_audioTracks.indexOf(track);
            const bool applied = crossfade
                ? applyAudioCrossfade(audioTrackIndex, clipIndex, duration, &error)
                : applyAudioFade(audioTrackIndex, clipIndex, edge, duration, &error);
            if (!applied && !error.isEmpty())
                QMessageBox::warning(this, QStringLiteral("音声トランジション"), error);
        };
        auto applyAudioChannelMode = [&](AudioChannelMode mode) {
            auto clips = track->clips();
            if (clipIndex >= clips.size()) return;
            if (clips[clipIndex].audioChannelMode == mode) return;
            clips[clipIndex].audioChannelMode = mode;
            track->setClips(clips);
            saveUndoState(QStringLiteral("Set audio channel mapping"));
            scheduleEmitSequenceChanged();
        };
        for (const auto &labelAction : aLabelActions) {
            if (aChosen == labelAction.first) {
                applyLabelToSelection(labelAction.second);
                return;
            }
        }
        if (aChosen == aCut) cutSelectedClip();
        else if (aChosen == aCopy) copySelectedClip();
        else if (aChosen == aDel) deleteSelectedClip();
        else if (aChosen == aReplace) {
            emit replaceClipRequested(TrackKind::Audio,
                                      m_audioTracks.indexOf(track), clipIndex);
        }
        else if (aChosen == aClipEq) {
            emit audioClipEqRequested(m_audioTracks.indexOf(track), clipIndex);
        }
        else if (aChosen == aUnlink) unlinkClipGroup(aLinkGroup);
        else if (aChosen == aNormalize) {
            const int trackIdx = m_audioTracks.indexOf(track);
            normalizeAudioClipPeak(trackIdx, clipIndex);
        }
        else if (aChosen == aDialogueLevel) {
            if (!aClip.volumeEnvelope.isEmpty()
                && QMessageBox::question(
                       this, QStringLiteral("ダイアログレベラー"),
                       QStringLiteral("既存の音量エンベロープを上書きしますか？"),
                       QMessageBox::Yes | QMessageBox::No,
                       QMessageBox::No) != QMessageBox::Yes) {
                return;
            }

            DialogueLevelerDialog dialog(this);
            if (dialog.exec() != QDialog::Accepted)
                return;

            QVector<float> samples;
            int sampleRate = 0;
            if (!WaveformGenerator::decodeAudio(aClip.filePath, samples, sampleRate)
                || samples.isEmpty() || sampleRate <= 0) {
                QMessageBox::warning(this, QStringLiteral("ダイアログレベラー"),
                                     QStringLiteral("音声のデコードに失敗しました。"));
                return;
            }

            const double sourceOut = aClip.outPoint > 0.0
                ? aClip.outPoint : aClip.duration;
            const double sourceDuration =
                static_cast<double>(samples.size()) / sampleRate;
            const double activeStart = qBound(0.0, aClip.inPoint, sourceDuration);
            const double activeEnd = qBound(activeStart, sourceOut, sourceDuration);
            const int firstSample = qBound(
                0, static_cast<int>(std::floor(activeStart * sampleRate)),
                static_cast<int>(samples.size()));
            const int lastSample = qBound(
                firstSample, static_cast<int>(std::ceil(activeEnd * sampleRate)),
                static_cast<int>(samples.size()));
            if (lastSample <= firstSample) {
                QMessageBox::information(
                    this, QStringLiteral("ダイアログレベラー"),
                    QStringLiteral("クリップの有効な音声範囲がありません。"));
                return;
            }

            leveler::Config config;
            config.targetShortTermLufs = dialog.targetLufs();
            config.smoothingSec = dialog.smoothingSec();
            QVector<float> activeSamples =
                samples.mid(firstSample, lastSample - firstSample);
            if (aClip.reversed)
                std::reverse(activeSamples.begin(), activeSamples.end());
            QVector<AudioGainPoint> envelope = leveler::computeEnvelope(
                activeSamples, sampleRate, config);
            const double speed = aClip.speed > 0.0 ? aClip.speed : 1.0;
            const double clipDuration = aClip.effectiveDuration();
            for (AudioGainPoint &point : envelope)
                point.time = qBound(0.0, point.time / speed, clipDuration);

            QString error;
            if (!applyDialogueLevel(m_audioTracks.indexOf(track), clipIndex,
                                    envelope, &error)) {
                QMessageBox::warning(this, QStringLiteral("ダイアログレベラー"),
                                     error);
            }
        }
        else if (aChosen == aRemix) {
            QVector<float> samples;
            int sampleRate = 0;
            if (!WaveformGenerator::decodeAudio(aClip.filePath, samples, sampleRate)
                || samples.isEmpty() || sampleRate <= 0) {
                QMessageBox::warning(this, QStringLiteral("ミュージックリミックス"),
                                     QStringLiteral("音声のデコードに失敗しました。"));
                return;
            }

            const double sourceOut = aClip.outPoint > 0.0
                ? aClip.outPoint : aClip.duration;
            const double totalSourceSec =
                static_cast<double>(samples.size()) / sampleRate;
            const double activeStart = qMax(0.0, aClip.inPoint);
            const double activeEnd = qMin(sourceOut, totalSourceSec);
            if (activeEnd <= activeStart) {
                QMessageBox::information(
                    this, QStringLiteral("ミュージックリミックス"),
                    QStringLiteral("クリップの有効な音声範囲がありません。"));
                return;
            }
            const int startSample = qBound(
                0, static_cast<int>(std::floor(activeStart * sampleRate)),
                static_cast<int>(samples.size()));
            const int endSample = qBound(
                startSample, static_cast<int>(std::ceil(activeEnd * sampleRate)),
                static_cast<int>(samples.size()));
            const QVector<float> activeSamples =
                samples.mid(startSample, endSample - startSample);
            const beatdetect::Result beats = beatdetect::detectBeats(
                activeSamples, sampleRate, beatdetect::Config{});
            if (beats.beatTimesSec.size() < 2) {
                QMessageBox::information(
                    this, QStringLiteral("ミュージックリミックス"),
                    QStringLiteral("ビートが 2 個未満のため適用できません。"));
                return;
            }

            const double speed = aClip.speed > 0.0 ? aClip.speed : 1.0;
            const double clipDuration = aClip.effectiveDuration();
            QVector<double> localBeatTimes;
            localBeatTimes.reserve(beats.beatTimesSec.size());
            for (double beat : beats.beatTimesSec) {
                const double sourceLocal = activeStart + beat - aClip.inPoint;
                if (sourceLocal >= -1.0e-6
                    && sourceLocal <= clipDuration * speed + 1.0e-6) {
                    localBeatTimes.append(qBound(0.0, sourceLocal / speed,
                                                 clipDuration));
                }
            }
            if (localBeatTimes.size() < 2) {
                QMessageBox::information(
                    this, QStringLiteral("ミュージックリミックス"),
                    QStringLiteral("ビート境界を作成できませんでした。"));
                return;
            }

            MusicRemixDialog dialog(this);
            dialog.setDetectedBpm(beats.bpm);
            dialog.setTargetDuration(clipDuration);
            if (dialog.exec() != QDialog::Accepted)
                return;
            const remix::Plan plan = remix::planRemix(
                localBeatTimes, clipDuration, dialog.targetDuration(), remix::Config{});
            if (!plan.valid) {
                QMessageBox::warning(this, QStringLiteral("ミュージックリミックス"),
                                     plan.error);
                return;
            }
            QString error;
            if (!applyMusicRemix(m_audioTracks.indexOf(track), clipIndex, plan,
                                 dialog.rippleFollowingClips(), &error)) {
                QMessageBox::warning(this, QStringLiteral("ミュージックリミックス"),
                                     error);
            }
        }
        else if (aChosen == aStereoAct) applyAudioChannelMode(AudioChannelMode::Stereo);
        else if (aChosen == aFillLeftAct) applyAudioChannelMode(AudioChannelMode::FillLeft);
        else if (aChosen == aFillRightAct) applyAudioChannelMode(AudioChannelMode::FillRight);
        else if (aChosen == aSwapAct) applyAudioChannelMode(AudioChannelMode::Swap);
        else if (aChosen == aMonoAct) applyAudioChannelMode(AudioChannelMode::Mono);
        else if (aChosen == aXdAct) applyAudioAction(AudioFadeEdge::Out, true);
        else if (aChosen == aFiAct) applyAudioAction(AudioFadeEdge::In, false);
        else if (aChosen == aFoAct) applyAudioAction(AudioFadeEdge::Out, false);
        else if (aClearAct && aChosen == aClearAct) {
            auto clips = track->clips();
            if (clipIndex < clips.size()) {
                const bool hadLead = clips[clipIndex].leadIn.type != TransitionType::None;
                const bool hadTrail = clips[clipIndex].trailOut.type != TransitionType::None;
                if (hadLead && clipIndex > 0
                    && clips[clipIndex - 1].trailOut.type != TransitionType::None) {
                    clips[clipIndex - 1].trailOut = Transition{};
                }
                if (hadTrail && clipIndex + 1 < clips.size()
                    && clips[clipIndex + 1].leadIn.type != TransitionType::None) {
                    clips[clipIndex + 1].leadIn = Transition{};
                }
                clips[clipIndex].leadIn = Transition{};
                clips[clipIndex].trailOut = Transition{};
                track->setClips(clips);
                saveUndoState("Clear audio transitions");
                scheduleEmitSequenceChanged();
            }
        }
        return;
    }

    const ClipInfo &clipInfo = track->clips()[clipIndex];
    const int linkGroup = clipInfo.linkGroup;
    const bool hasTransition = clipInfo.leadIn.type != TransitionType::None
                            || clipInfo.trailOut.type != TransitionType::None;
    auto currentLinkedAudioChannelMode = [&]() {
        if (linkGroup > 0) {
            for (const auto *audioTrack : m_audioTracks) {
                if (!audioTrack) continue;
                const auto &audioClips = audioTrack->clips();
                for (const auto &audioClip : audioClips) {
                    if (audioClip.linkGroup == linkGroup)
                        return audioClip.audioChannelMode;
                }
            }
        }
        return clipInfo.audioChannelMode;
    };

    QMenu menu;
    QAction *sequenceFromClip = menu.addAction(QStringLiteral("このクリップから新規シーケンス"));
    sequenceFromClip->setEnabled(clipInfo.sequenceRefId.isEmpty() && !clipInfo.filePath.isEmpty());
    const ClipInfo sequenceSource = clipInfo;
    const int renderTrack = m_videoTracks.indexOf(track);
    if (renderTrack >= 0) {
        auto *bake = menu.addAction(QStringLiteral("効果を焼き込んで差し替え…"));
        bake->setEnabled(!track->isLocked());
        connect(bake, &QAction::triggered, this, [this, renderTrack, clipIndex]() {
            emit renderInPlaceRequested(renderTrack, clipIndex);
        });
        auto *restore = menu.addAction(QStringLiteral("元のクリップに戻す"));
        restore->setEnabled(!track->isLocked() && bool(track->clips()[clipIndex].renderInPlaceOriginal));
        connect(restore, &QAction::triggered, this, [this, renderTrack, clipIndex]() {
            renderinplace::decomposeRenderInPlace(*this, renderTrack, clipIndex);
        });
        menu.addSeparator();
    }
    // US-1202: primary-storyline connection of a V2+ clip (no shortcut). The
    // audio branch above returns early and has its own entries.
    const TrackKind menuKind = TrackKind::Video;
    const int menuTrackIndex = m_videoTracks.indexOf(track);
    if (menuTrackIndex > 0) {
        auto *connectAct = menu.addAction(tr("主ストーリーラインに接続"));
        connect(connectAct, &QAction::triggered, this, [this, menuKind, menuTrackIndex, clipIndex]() {
            QString err;
            if (!connectToStoryline(menuKind, menuTrackIndex, clipIndex, &err))
                emit statusMessageRequested(err, 4000);
        });
        auto *disconnectAct = menu.addAction(tr("接続を解除"));
        disconnectAct->setEnabled(!clipInfo.anchorUid.isEmpty());
        connect(disconnectAct, &QAction::triggered, this, [this, menuKind, menuTrackIndex, clipIndex]() {
            QString err;
            if (!disconnectClip(menuKind, menuTrackIndex, clipIndex, &err))
                emit statusMessageRequested(err, 4000);
        });
        menu.addSeparator();
    }
    QAction *cutAct = menu.addAction(QStringLiteral("カット"));
    QAction *copyAct = menu.addAction(QStringLiteral("コピー"));
    QAction *deleteAct = menu.addAction(QStringLiteral("削除"));
    QAction *replaceAct = menu.addAction(QStringLiteral("置き換え"));
    QAction *freezeFrameAct = menu.addAction(QStringLiteral("フリーズフレームを追加"));
    auto playheadInsideClickedClip = [&]() {
        const auto &clips = track->clips();
        double cursor = 0.0;
        for (int i = 0; i < clips.size(); ++i) {
            const double start = cursor + qMax(0.0, clips[i].leadInSec);
            const double end = start + clips[i].effectiveDuration();
            if (i == clipIndex)
                return m_playheadPos >= start - 1e-6 && m_playheadPos < end - 1e-6;
            cursor = end;
        }
        return false;
    };
    freezeFrameAct->setEnabled(playheadInsideClickedClip());
    QAction *silenceCutAct = menu.addAction(QStringLiteral("無音を自動カット..."));
    QAction *beatMarkerAct = menu.addAction(QStringLiteral("ビートでマーカー..."));
    menu.addSeparator();
    QAction *unlinkAct = menu.addAction(QStringLiteral("同期を切る"));
    unlinkAct->setEnabled(linkGroup > 0);
    QAction *relinkAct = menu.addAction(QStringLiteral("再同期"));
    relinkAct->setEnabled(linkGroup == 0);

    menu.addSeparator();
    const AudioChannelMode currentChannelMode = currentLinkedAudioChannelMode();
    QMenu *channelMenu = menu.addMenu(QStringLiteral("チャンネルマッピング"));
    QAction *stereoAct = addAudioChannelModeAction(channelMenu, currentChannelMode,
                                                   AudioChannelMode::Stereo);
    QAction *fillLeftAct = addAudioChannelModeAction(channelMenu, currentChannelMode,
                                                     AudioChannelMode::FillLeft);
    QAction *fillRightAct = addAudioChannelModeAction(channelMenu, currentChannelMode,
                                                      AudioChannelMode::FillRight);
    QAction *swapAct = addAudioChannelModeAction(channelMenu, currentChannelMode,
                                                 AudioChannelMode::Swap);
    QAction *monoAct = addAudioChannelModeAction(channelMenu, currentChannelMode,
                                                 AudioChannelMode::Mono);
    menu.addSeparator();
    QAction *enabledAction = menu.addAction(QStringLiteral("クリップを有効にする"));
    enabledAction->setCheckable(true);
    enabledAction->setChecked(track->clips()[clipIndex].visible);
    enabledAction->setEnabled(!track->isLocked() && !clipgeom::isNullObjectFilePath(track->clips()[clipIndex].filePath));
    QMenu *labelMenu = menu.addMenu(QStringLiteral("ラベルカラー"));
    const QList<QPair<QAction *, ClipLabel>> labelActions{
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::None), ClipLabel::None},
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::Red), ClipLabel::Red},
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::Orange), ClipLabel::Orange},
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::Yellow), ClipLabel::Yellow},
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::Green), ClipLabel::Green},
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::Cyan), ClipLabel::Cyan},
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::Blue), ClipLabel::Blue},
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::Purple), ClipLabel::Purple},
        {addClipLabelAction(labelMenu, clipInfo.label, ClipLabel::Pink), ClipLabel::Pink}
    };
    menu.addSeparator();
    QMenu *transitionMenu = menu.addMenu(QStringLiteral("トランジション"));
    QAction *xdAct = transitionMenu->addAction(QStringLiteral("クロスディゾルブ (1.0s)"));
    QAction *fdAct = transitionMenu->addAction(QStringLiteral("フィルムディゾルブ (1.0s)"));
    QAction *ddAct = transitionMenu->addAction(QStringLiteral("ディザディゾルブ (1.0s)"));
    QAction *blAct = transitionMenu->addAction(QStringLiteral("ブラーディゾルブ (1.0s)"));
    QAction *pxAct = transitionMenu->addAction(QStringLiteral("ピクセレート (1.0s)"));
    QAction *fiAct = transitionMenu->addAction(QStringLiteral("フェードイン (0.5s)"));
    QAction *foAct = transitionMenu->addAction(QStringLiteral("フェードアウト (0.5s)"));
    QAction *dbAct = transitionMenu->addAction(QStringLiteral("黒へディップ (1.0s)"));
    QAction *dwAct = transitionMenu->addAction(QStringLiteral("白へディップ (1.0s)"));
    transitionMenu->addSeparator();
    QMenu *wipeMenu = transitionMenu->addMenu(QStringLiteral("ワイプ (1.0s)"));
    QAction *wlAct = wipeMenu->addAction(QStringLiteral("左 → 右"));
    QAction *wrAct = wipeMenu->addAction(QStringLiteral("右 → 左"));
    QAction *wuAct = wipeMenu->addAction(QStringLiteral("上 → 下"));
    QAction *wdAct = wipeMenu->addAction(QStringLiteral("下 → 上"));
    QAction *cwAct  = wipeMenu->addAction(QStringLiteral("時計回り"));
    QAction *cwcAct = wipeMenu->addAction(QStringLiteral("反時計回り"));
    QAction *bhAct  = wipeMenu->addAction(QStringLiteral("バーンドア (水平・開く)"));
    QAction *bhcAct = wipeMenu->addAction(QStringLiteral("バーンドア (水平・閉じる)"));
    QAction *bvAct  = wipeMenu->addAction(QStringLiteral("バーンドア (垂直・開く)"));
    QAction *bvcAct = wipeMenu->addAction(QStringLiteral("バーンドア (垂直・閉じる)"));
    QMenu *slideMenu = transitionMenu->addMenu(QStringLiteral("スライド (1.0s)"));
    QAction *slAct = slideMenu->addAction(QStringLiteral("左へ"));
    QAction *srAct = slideMenu->addAction(QStringLiteral("右へ"));
    QAction *suAct = slideMenu->addAction(QStringLiteral("上へ"));
    QAction *sdAct = slideMenu->addAction(QStringLiteral("下へ"));
    QMenu *pushMenu = transitionMenu->addMenu(QStringLiteral("プッシュ (1.0s)"));
    QAction *plAct = pushMenu->addAction(QStringLiteral("左へ"));
    QAction *prAct = pushMenu->addAction(QStringLiteral("右へ"));
    QAction *puAct = pushMenu->addAction(QStringLiteral("上へ"));
    QAction *pdAct = pushMenu->addAction(QStringLiteral("下へ"));
    QMenu *irisMenu = transitionMenu->addMenu(QStringLiteral("アイリス (1.0s)"));
    QAction *irAct  = irisMenu->addAction(QStringLiteral("円・開く"));
    QAction *ircAct = irisMenu->addAction(QStringLiteral("円・閉じる"));
    QAction *ibAct  = irisMenu->addAction(QStringLiteral("矩形・開く"));
    QAction *ibcAct = irisMenu->addAction(QStringLiteral("矩形・閉じる"));
    QAction *czAct = transitionMenu->addAction(QStringLiteral("クロスズーム (1.0s)"));
    QMenu *spinMenu = transitionMenu->addMenu(QStringLiteral("スピン (1.0s)"));
    QAction *scwAct  = spinMenu->addAction(QStringLiteral("時計回り"));
    QAction *sccwAct = spinMenu->addAction(QStringLiteral("反時計回り"));
    QMenu *whipMenu = transitionMenu->addMenu(QStringLiteral("ウィップパン (0.5s)"));
    QAction *wplAct = whipMenu->addAction(QStringLiteral("左へ"));
    QAction *wprAct = whipMenu->addAction(QStringLiteral("右へ"));
    QAction *glAct  = transitionMenu->addAction(QStringLiteral("グリッチ (0.5s)"));
    QAction *mcAct  = transitionMenu->addAction(QStringLiteral("モーフカット (1.0s)"));
    QAction *llAct  = transitionMenu->addAction(QStringLiteral("ライトリーク (1.0s)"));
    QAction *lfAct  = transitionMenu->addAction(QStringLiteral("レンズフレア (1.0s)"));
    QAction *fbAct  = transitionMenu->addAction(QStringLiteral("フィルムバーン (1.0s)"));
    QAction *skAct  = transitionMenu->addAction(QStringLiteral("カメラシェイク (0.5s)"));
    QAction *chAct  = transitionMenu->addAction(QStringLiteral("チャンネルシフト (0.5s)"));
    QMenu *flipMenu = transitionMenu->addMenu(QStringLiteral("フリップ (1.0s)"));
    QAction *fhAct  = flipMenu->addAction(QStringLiteral("水平 (Y軸回転)"));
    QAction *fvAct  = flipMenu->addAction(QStringLiteral("垂直 (X軸回転)"));
    transitionMenu->addSeparator();
    // User-saved presets — populated dynamically from QSettings via
    // TransitionPresetStore. Empty submenu when nothing is saved.
    QMenu *presetMenu = transitionMenu->addMenu(QStringLiteral("プリセット"));
    QList<QPair<QAction *, TransitionPreset>> presetActs;
    {
        const auto loaded = TransitionPresetStore::loadAll();
        if (loaded.isEmpty()) {
            QAction *empty = presetMenu->addAction(QStringLiteral("(プリセット未登録)"));
            empty->setEnabled(false);
        } else {
            for (const auto &p : loaded) {
                const QString label = QStringLiteral("%1  (%2, %3s)")
                    .arg(p.name)
                    .arg(Transition::typeName(p.transition.type))
                    .arg(p.transition.duration, 0, 'f', 1);
                QAction *act = presetMenu->addAction(label);
                presetActs.append({ act, p });
            }
        }
    }
    QAction *transDialogAct = transitionMenu->addAction(QStringLiteral("カスタム..."));
    QAction *transClearAct = nullptr;
    if (hasTransition) {
        transitionMenu->addSeparator();
        transClearAct = transitionMenu->addAction(QStringLiteral("トランジションを削除"));
    }

    QAction *shapeModifiersAct = nullptr;
    if (!clipInfo.shapes.isEmpty())
        shapeModifiersAct = menu.addAction(QStringLiteral("シェイプモディファイア…"));

    QAction *fxAct = menu.addAction(QStringLiteral("ビデオエフェクト..."));
    QAction *ccAct = menu.addAction(QStringLiteral("色補正 / グレーディング..."));
    QAction *adjustmentAct = menu.addAction(QStringLiteral("調整レイヤーを作成"));
    QAction *parentAct = menu.addAction(QStringLiteral("ペアレント..."));
    QAction *nullAct = menu.addAction(QStringLiteral("ヌルオブジェクトを作成"));
    QAction *autoOrientAct = menu.addAction(QStringLiteral("パスに沿って自動回転"));
    autoOrientAct->setCheckable(true);
    autoOrientAct->setChecked(clipInfo.autoOrientEnabled);
    QAction *autoOrientCameraAct = menu.addAction(QStringLiteral("カメラへ自動的に向ける"));
    autoOrientCameraAct->setCheckable(true);
    autoOrientCameraAct->setChecked(clipInfo.autoOrientToCamera);
    autoOrientCameraAct->setEnabled(clipInfo.is3DLayer);
    menu.addSeparator();
    // SNS 縦動画フィット (相互排他の3択): 幅フィット=レターボックスで全表示 /
    // 幅埋め=中央クロップで枠を歪みなく充填 / 解除=既定 (IgnoreAspectRatio で
    // 枠に伸ばす)。fitContain と fitCover は engine 側 (snsfit::maybeFit) で
    // cover 優先の相互排他なので、ここでも片方を立てたら他方は必ず倒す。
    QAction *snsFitAct = menu.addAction(QStringLiteral("SNS: 幅フィット中央(全表示・レターボックス)"));
    QAction *snsCoverAct = menu.addAction(QStringLiteral("SNS: 幅埋め(クロップ・歪みなし)"));
    QAction *snsFillAct = menu.addAction(QStringLiteral("SNS: フィット解除(全画面)"));

    auto applyLinkedAudioChannelMode = [&](AudioChannelMode mode) {
        bool changed = false;
        auto applyToTrackClip = [&](TimelineTrack *targetTrack, int targetClipIndex) {
            if (!targetTrack || targetClipIndex < 0
                || targetClipIndex >= targetTrack->clips().size()) {
                return;
            }
            auto clips = targetTrack->clips();
            if (clips[targetClipIndex].audioChannelMode == mode)
                return;
            clips[targetClipIndex].audioChannelMode = mode;
            targetTrack->setClips(clips);
            changed = true;
        };

        applyToTrackClip(track, clipIndex);
        if (linkGroup > 0) {
            for (auto *audioTrack : m_audioTracks) {
                if (!audioTrack) continue;
                const auto audioClips = audioTrack->clips();
                for (int i = 0; i < audioClips.size(); ++i) {
                    if (audioClips[i].linkGroup == linkGroup)
                        applyToTrackClip(audioTrack, i);
                }
            }
        }

        if (changed) {
            saveUndoState(QStringLiteral("Set audio channel mapping"));
            scheduleEmitSequenceChanged();
        }
    };

    QAction *chosen = menu.exec(globalPos);
    if (chosen == sequenceFromClip) {
        createSequenceFromClip(sequenceSource);
        return;
    }
    if (chosen == enabledAction) {
        setClipEnabled(TrackKind::Video, m_videoTracks.indexOf(track), clipIndex, enabledAction->isChecked());
        return;
    }

    if (!chosen) return;
    for (const auto &labelAction : labelActions) {
        if (chosen == labelAction.first) {
            applyLabelToSelection(labelAction.second);
            return;
        }
    }
    if (chosen == cutAct) cutSelectedClip();
    else if (chosen == copyAct) copySelectedClip();
    else if (chosen == deleteAct) deleteSelectedClip();
    else if (chosen == replaceAct) {
        emit replaceClipRequested(TrackKind::Video,
                                  m_videoTracks.indexOf(track), clipIndex);
    }
    else if (chosen == freezeFrameAct) freezeFrameAtPlayhead(track, clipIndex);
    else if (chosen == silenceCutAct) applySilenceCutToClip(track, clipIndex);
    else if (chosen == beatMarkerAct) applyBeatMarkersToClip(track, clipIndex);
    else if (chosen == unlinkAct) unlinkClipGroup(linkGroup);
    else if (chosen == relinkAct) relinkClipAt(track, clipIndex);
    else if (chosen == stereoAct) applyLinkedAudioChannelMode(AudioChannelMode::Stereo);
    else if (chosen == fillLeftAct) applyLinkedAudioChannelMode(AudioChannelMode::FillLeft);
    else if (chosen == fillRightAct) applyLinkedAudioChannelMode(AudioChannelMode::FillRight);
    else if (chosen == swapAct) applyLinkedAudioChannelMode(AudioChannelMode::Swap);
    else if (chosen == monoAct) applyLinkedAudioChannelMode(AudioChannelMode::Mono);
    else if (chosen == autoOrientAct) {
        setClipAutoOrientEnabled(TrackKind::Video, m_videoTracks.indexOf(track),
                                 clipIndex, autoOrientAct->isChecked());
    }
    else if (chosen == autoOrientCameraAct) {
        setClipAutoOrientToCamera(m_videoTracks.indexOf(track), clipIndex,
                                  autoOrientCameraAct->isChecked());
    }
    else if (chosen == snsFitAct) applySnsFitToClip(track, clipIndex, true, false, QStringLiteral("SNS width fit center"));
    else if (chosen == snsCoverAct) applySnsFitToClip(track, clipIndex, false, true, QStringLiteral("SNS width fill crop"));
    else if (chosen == snsFillAct) applySnsFitToClip(track, clipIndex, false, false, QStringLiteral("SNS restore fullscreen"));
    else if (chosen == xdAct) {
        Transition t;
        t.type = TransitionType::CrossDissolve;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == fdAct) {
        Transition t;
        t.type = TransitionType::FilmDissolve;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == ddAct) {
        Transition t;
        t.type = TransitionType::DitherDissolve;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == blAct) {
        Transition t;
        t.type = TransitionType::BlurDissolve;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == pxAct) {
        Transition t;
        t.type = TransitionType::Pixelate;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == fiAct) {
        Transition t;
        t.type = TransitionType::FadeIn;
        t.duration = 0.5;
        applyTransitionToSelected(t);
    }
    else if (chosen == foAct) {
        Transition t;
        t.type = TransitionType::FadeOut;
        t.duration = 0.5;
        applyTransitionToSelected(t);
    }
    else if (chosen == dbAct) {
        Transition t;
        t.type = TransitionType::DipToBlack;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == dwAct) {
        Transition t;
        t.type = TransitionType::DipToWhite;
        t.duration = 1.0;
        applyTransitionToSelected(t);
    }
    else if (chosen == wlAct || chosen == wrAct || chosen == wuAct
             || chosen == wdAct || chosen == cwAct || chosen == cwcAct
             || chosen == bhAct || chosen == bhcAct
             || chosen == bvAct || chosen == bvcAct
             || chosen == slAct || chosen == srAct
             || chosen == suAct || chosen == sdAct
             || chosen == plAct || chosen == prAct
             || chosen == puAct || chosen == pdAct
             || chosen == czAct || chosen == scwAct || chosen == sccwAct
             || chosen == wplAct || chosen == wprAct || chosen == glAct
             || chosen == mcAct || chosen == llAct || chosen == fhAct || chosen == fvAct
             || chosen == lfAct || chosen == fbAct
             || chosen == skAct || chosen == chAct
             || chosen == irAct || chosen == ircAct
             || chosen == ibAct || chosen == ibcAct) {
        Transition t;
        // Whip pan, glitch, camera shake, channel shift are punchy/short
        // by NLE convention; everyone else gets the 1.0s default we've
        // been using for context-menu presets. Custom dialog can override
        // either way.
        t.duration = (chosen == wplAct || chosen == wprAct || chosen == glAct
                      || chosen == skAct || chosen == chAct)
            ? 0.5 : 1.0;
        if (chosen == wlAct)      t.type = TransitionType::WipeLeft;
        else if (chosen == wrAct) t.type = TransitionType::WipeRight;
        else if (chosen == wuAct) t.type = TransitionType::WipeUp;
        else if (chosen == wdAct) t.type = TransitionType::WipeDown;
        else if (chosen == cwAct)  t.type = TransitionType::ClockWipe;
        else if (chosen == cwcAct) t.type = TransitionType::ClockWipeCCW;
        else if (chosen == bhAct)  t.type = TransitionType::BarnDoorHorizontal;
        else if (chosen == bhcAct) t.type = TransitionType::BarnDoorHClose;
        else if (chosen == bvAct)  t.type = TransitionType::BarnDoorVertical;
        else if (chosen == bvcAct) t.type = TransitionType::BarnDoorVClose;
        else if (chosen == slAct) t.type = TransitionType::SlideLeft;
        else if (chosen == srAct) t.type = TransitionType::SlideRight;
        else if (chosen == suAct) t.type = TransitionType::SlideUp;
        else if (chosen == sdAct) t.type = TransitionType::SlideDown;
        else if (chosen == plAct) t.type = TransitionType::PushLeft;
        else if (chosen == prAct) t.type = TransitionType::PushRight;
        else if (chosen == puAct) t.type = TransitionType::PushUp;
        else if (chosen == pdAct) t.type = TransitionType::PushDown;
        else if (chosen == czAct) t.type = TransitionType::CrossZoom;
        else if (chosen == scwAct)  t.type = TransitionType::SpinCW;
        else if (chosen == sccwAct) t.type = TransitionType::SpinCCW;
        else if (chosen == wplAct)  t.type = TransitionType::WhipPanLeft;
        else if (chosen == wprAct)  t.type = TransitionType::WhipPanRight;
        else if (chosen == glAct)   t.type = TransitionType::Glitch;
        else if (chosen == mcAct)   t.type = TransitionType::MorphCut;
        else if (chosen == llAct)   t.type = TransitionType::LightLeak;
        else if (chosen == lfAct)   t.type = TransitionType::LensFlare;
        else if (chosen == fbAct)   t.type = TransitionType::FilmBurn;
        else if (chosen == skAct)   t.type = TransitionType::CameraShake;
        else if (chosen == chAct)   t.type = TransitionType::ColorChannelShift;
        else if (chosen == fhAct)   t.type = TransitionType::FlipHorizontal;
        else if (chosen == fvAct)   t.type = TransitionType::FlipVertical;
        else if (chosen == irAct)  t.type = TransitionType::IrisRound;
        else if (chosen == ircAct) t.type = TransitionType::IrisRoundClose;
        else if (chosen == ibAct)  t.type = TransitionType::IrisBox;
        else                       t.type = TransitionType::IrisBoxClose;
        applyTransitionToSelected(t);
    }
    else if (chosen == transDialogAct) emit transitionDialogRequested();
    else if (transClearAct && chosen == transClearAct) clearTransitionsOnSelected();
    else if (shapeModifiersAct && chosen == shapeModifiersAct) {
        const int trackIdx = m_videoTracks.indexOf(track);
        if (trackIdx < 0 || track->isLocked()) return;
        // Copy before entering the nested event loop; preview replaces the clips vector.
        const ShapeModifiers original = track->clips()[clipIndex].shapes.first().modifiers;
        ShapeModifierDialog dialog(original, this);
        bool changed = false;
        connect(&dialog, &ShapeModifierDialog::modifiersChanged, this, [&]() {
            changed = true;
            setClipShapeModifiers(trackIdx, clipIndex, dialog.modifiers(), false);
        });
        const bool accepted = dialog.exec() == QDialog::Accepted;
        const ShapeModifiers finalValue = dialog.modifiers();
        // Restore the original before committing so one user operation is one undo.
        if (changed) setClipShapeModifiers(trackIdx, clipIndex, original, false);
        if (accepted && changed && finalValue.toJson() != original.toJson())
            setClipShapeModifiers(trackIdx, clipIndex, finalValue, true);
    }
    else if (chosen == fxAct) emit videoEffectsDialogRequested();
    else if (chosen == ccAct) emit colorCorrectionRequested();
    else if (chosen == adjustmentAct) {
        double startSec = m_playheadPos;
        double endSec = m_playheadPos
            + static_cast<double>(kDefaultAdjustmentLayerDurationUs) / 1.0e6;
        trackClipTimeRangeAt(track, clipIndex, &startSec, &endSec);
        addAdjustmentLayer(makeAdjustmentLayer(startSec, endSec,
                                               m_videoTracks.indexOf(track)));
    }
    else if (chosen == parentAct) emit clipParentDialogRequested();
    else if (chosen == nullAct) emit nullObjectRequested();
    else {
        // Preset dispatch — chosen action might be one of the dynamically
        // populated preset entries. Match by pointer since the labels
        // include the type/duration suffix and could collide with user
        // names that happen to match.
        for (const auto &pair : presetActs) {
            if (chosen != pair.first) continue;
            applyTransitionToSelected(pair.second.transition);
            break;
        }
    }
}

void Timeline::undo()
{
    undotrace::log("undo:enter");
    if (!canUndo()) return;
    restoreState(m_undoManager->undo());
    updateInfoLabel();
}

void Timeline::redo()
{
    if (!canRedo()) return;
    restoreState(m_undoManager->redo());
    updateInfoLabel();
}

bool Timeline::canUndo() const { return m_undoManager->canUndo(); }
bool Timeline::canRedo() const { return m_undoManager->canRedo(); }

void Timeline::setSnapEnabled(bool enabled)
{
    for (auto *t : m_videoTracks) t->setSnapEnabled(enabled);
    for (auto *t : m_audioTracks) t->setSnapEnabled(enabled);
    updateInfoLabel();
}

bool Timeline::snapEnabled() const {
    return m_videoTrack ? m_videoTrack->snapEnabled() : true;
}

qint64 Timeline::findSnapTarget(qint64 candidateUs, int *outScreenX) const
{
    const double candidateSec = static_cast<double>(candidateUs) / 1e6;
    const int candidateX = static_cast<int>(candidateSec * m_zoomLevel);

    struct Target {
        int x;
        int priority; // 1=clip-edge, 2=playhead, 4=t=0
        int distance;
    };
    QVector<Target> targets;

    auto collectClipEdges = [&](const QVector<TimelineTrack*> &tracks) {
        for (const auto *track : tracks) {
            if (!track) continue;
            int x = 0;
            const double pps = track->pixelsPerSecond();
            for (int i = 0; i < track->clipCount(); ++i) {
                const auto &clip = track->clips()[i];
                x += qMax(0, static_cast<int>(clip.leadInSec * pps));
                targets.append({x, 1, qAbs(candidateX - x)});
                const int cw = qMax(20, static_cast<int>(clip.effectiveDuration() * pps));
                targets.append({x + cw, 1, qAbs(candidateX - (x + cw))});
                x += cw;
            }
        }
    };
    collectClipEdges(m_videoTracks);
    collectClipEdges(m_audioTracks);

    // Playhead
    const int playheadX = static_cast<int>(m_playheadPos * m_zoomLevel);
    targets.append({playheadX, 2, qAbs(candidateX - playheadX)});

    // Time origin (t=0)
    targets.append({0, 4, qAbs(candidateX - 0)});

    // Find best: closest by pixel distance; on tie lower priority wins.
    Target best{-1, 999, INT_MAX};
    for (const auto &t : targets) {
        if (t.distance > TimelineTrack::SNAP_THRESHOLD) continue;
        if (t.distance < best.distance
            || (t.distance == best.distance && t.priority < best.priority)) {
            best = t;
        }
    }

    if (best.x >= 0) {
        *outScreenX = best.x;
        const double snappedSec = static_cast<double>(best.x) / m_zoomLevel;
        return static_cast<qint64>(snappedSec * 1e6);
    }
    *outScreenX = candidateX;
    return candidateUs;
}

void Timeline::triggerSnapLine(int screenX)
{
    m_snapLineX = screenX;
    m_snapLineFadeTimer.restart();
    // Repaint all tracks so the line appears on every row.
    for (auto *t : m_videoTracks)
        if (t) t->update();
    for (auto *t : m_audioTracks)
        if (t) t->update();
    // Schedule a clean-up repaint after the fade window.
    QTimer::singleShot(kSnapLineFadeMs, this, [this] {
        m_snapLineX = -1;
        for (auto *t : m_videoTracks) if (t) t->update();
        for (auto *t : m_audioTracks) if (t) t->update();
    });
}

int Timeline::snapLineX() const
{
    if (m_snapLineX < 0) return -1;
    if (m_snapLineFadeTimer.elapsed() >= kSnapLineFadeMs) return -1;
    return m_snapLineX;
}

int Timeline::snapLineAlpha() const
{
    if (m_snapLineX < 0) return 0;
    const qint64 elapsed = m_snapLineFadeTimer.elapsed();
    if (elapsed >= kSnapLineFadeMs) return 0;
    return 255 - static_cast<int>(255 * elapsed / kSnapLineFadeMs);
}

// Zoom
void Timeline::zoomIn() { setZoomLevel(m_zoomLevel * 1.25); }
void Timeline::zoomOut() { setZoomLevel(m_zoomLevel / 1.25); }

void Timeline::zoomToFitSequence()
{
    if (!m_scrollArea || totalDuration() <= 0.0) return;
    clearZoomAnchor();
    setZoomLevel(qMax(1, m_scrollArea->viewport()->width() - 60) / totalDuration());
    // Short clips occupy at least 20 px, so duration alone can underestimate
    // the drawn sequence width. Include audio tracks without changing totalDuration().
    const int available = qMax(1, m_scrollArea->viewport()->width() - 60);
    for (;;) {
        double nextZoom = m_zoomLevel;
        for (auto *track : timelineTracks(this)) {
            const auto &clips = track->clips();
            if (clips.isEmpty()) continue;
            const int end = track->clipStartX(clips.size() - 1)
                + qMax(20, static_cast<int>(clips.last().effectiveDuration() * m_zoomLevel));
            if (end <= available) continue;
            int fixedWidth = 0;
            double scalableDuration = 0.0;
            for (const auto &clip : clips) {
                scalableDuration += qMax(0.0, clip.leadInSec);
                if (clip.effectiveDuration() * m_zoomLevel < 20.0)
                    fixedWidth += 20;
                else
                    scalableDuration += clip.effectiveDuration();
            }
            const double fittingZoom = scalableDuration > 0.0
                ? (available - fixedWidth) / scalableDuration : 0.02;
            nextZoom = qMin(nextZoom, qMax(0.02, fittingZoom));
        }
        // At the lower bound, sequences with too many 20 px clips can only
        // be fitted on a best-effort basis.
        if (nextZoom >= m_zoomLevel) break;
        setZoomLevel(nextZoom);
    }
    m_scrollArea->horizontalScrollBar()->setValue(0);
}

bool Timeline::zoomToSelection()
{
    double first = std::numeric_limits<double>::max(), last = 0.0;
    for (auto *track : timelineTracks(this)) {
        for (int i : track->selectedClips()) {
            double start = 0.0, end = 0.0;
            if (trackClipTimeRangeAt(track, i, &start, &end)) {
                first = qMin(first, start);
                last = qMax(last, end);
            }
        }
    }
    if (last <= first || !m_scrollArea) return false;
    const double margin = (last - first) * 0.05;
    const double left = qMax(0.0, first - margin);
    const int available = qMax(1, m_scrollArea->viewport()->width() - 2);
    const int targetWidth = qMax(1, static_cast<int>(available / 1.1));
    const double initialZoom = qBound(0.02, available / (last + margin - left), 200.0);
    const auto tracks = timelineTracks(this);
    // Match clipStartX and the painted width without mutating zoom or layout.
    const auto boundsAt = [&](double pps) {
        int leftPx = std::numeric_limits<int>::max(), rightPx = 0;
        for (auto *track : tracks) {
            int x = 0;
            const auto &clips = track->clips();
            for (int i = 0; i < clips.size(); ++i) {
                x += qMax(0, static_cast<int>(clips[i].leadInSec * pps));
                const int width = qMax(20, static_cast<int>(clips[i].effectiveDuration() * pps));
                if (track->isClipSelected(i)) {
                    leftPx = qMin(leftPx, x);
                    rightPx = qMax(rightPx, x + width);
                }
                x += width;
            }
        }
        return qMakePair(leftPx, rightPx);
    };
    const auto fitsAt = [&](double pps) {
        const auto bounds = boundsAt(pps);
        return bounds.second - bounds.first <= targetWidth;
    };
    double fittingZoom = initialZoom;
    double upper = 200.0;
    // Different tracks accumulate different minimum-width clips: the span is
    // not monotonic in pps. Scan the whole range, including its lower endpoint.
    for (double candidate = 200.0;; candidate = qMax(0.02, candidate * 0.95)) {
        if (fitsAt(candidate)) {
            double lower = candidate;
            for (int iteration = 0; iteration < 10; ++iteration) {
                const double midpoint = (lower + upper) * 0.5;
                if (fitsAt(midpoint))
                    lower = midpoint;
                else
                    upper = midpoint;
            }
            fittingZoom = lower; // Always retain a value verified to fit.
            break;
        }
        if (candidate <= 0.02) break;
        upper = candidate;
    }
    // If minimum widths prevent fitting, keep the duration-based best effort.
    clearZoomAnchor();
    setZoomLevel(fittingZoom);
    const auto bounds = boundsAt(m_zoomLevel);
    if (m_tracksWidget && m_tracksWidget->layout()) m_tracksWidget->layout()->activate();
    if (auto *content = m_scrollArea->widget()) {
        if (content->layout()) content->layout()->activate();
        content->adjustSize();
    }
    QEvent layoutEvent(QEvent::LayoutRequest);
    QCoreApplication::sendEvent(m_scrollArea, &layoutEvent);
    const int padding = qMin(static_cast<int>((bounds.second - bounds.first) * 0.05),
                             qMax(0, (available - (bounds.second - bounds.first)) / 2));
    m_scrollArea->horizontalScrollBar()->setValue(qMax(0, bounds.first - padding));
    return true;
}

void Timeline::captureZoomAnchor()
{
    // Record the playhead's current viewport column (or the viewport center
    // if the playhead is offscreen). setZoomLevel uses this as a fixed
    // reference point so the playhead doesn't slide sideways while the user
    // drags the ruler to zoom.
    if (!m_scrollArea || !m_scrollArea->viewport() || !m_videoTrack) {
        m_zoomAnchorViewportX = -1;
        return;
    }
    const QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
    const int scrollX = hbar ? hbar->value() : 0;
    const int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 0) {
        m_zoomAnchorViewportX = -1;
        return;
    }
    const int playheadX = m_videoTrack->secondsToX(m_playheadPos);
    if (playheadX >= scrollX && playheadX <= scrollX + viewportW)
        m_zoomAnchorViewportX = playheadX - scrollX;
    else
        m_zoomAnchorViewportX = viewportW / 2;
}

void Timeline::clearZoomAnchor()
{
    m_zoomAnchorViewportX = -1;
}

void Timeline::setZoomLevel(double pixelsPerSecond)
{
    m_zoomLevel = qBound(0.02, pixelsPerSecond, 200.0);
    for (auto *t : m_videoTracks) t->setPixelsPerSecond(m_zoomLevel);
    for (auto *t : m_audioTracks) t->setPixelsPerSecond(m_zoomLevel);
    if (m_timeRuler)
        m_timeRuler->setPixelsPerSecond(m_zoomLevel);
    if (m_textStrip)
        m_textStrip->setPixelsPerSecond(m_zoomLevel);
    if (m_markerLane)
        m_markerLane->setPixelsPerSecond(m_zoomLevel);
    updateConnectionOverlay();

    if (m_zoomAnchorViewportX >= 0 && m_scrollArea && m_videoTrack) {
        // Force the scroll area to refresh its content width synchronously so
        // the horizontal scrollbar's max is current before setValue(). We
        // scope the layout activation and LayoutRequest to the scroll area so
        // it can't ripple into unrelated widgets.
        if (m_tracksWidget && m_tracksWidget->layout())
            m_tracksWidget->layout()->activate();
        if (QWidget *content = m_scrollArea->widget()) {
            if (content->layout()) content->layout()->activate();
            content->adjustSize();
        }
        QEvent layoutEvt(QEvent::LayoutRequest);
        QCoreApplication::sendEvent(m_scrollArea, &layoutEvt);

        // IMPORTANT: do NOT read hbar->value() here. The anchor is the
        // FIXED column captured at drag start; feeding the current (possibly
        // clamped) scroll value back into the anchor would create a runaway
        // loop on rapid drag events.
        if (QScrollBar *hbar = m_scrollArea->horizontalScrollBar()) {
            const int newPlayheadX = m_videoTrack->secondsToX(m_playheadPos);
            int target = newPlayheadX - m_zoomAnchorViewportX;
            if (target < 0) target = 0;
            hbar->setValue(target);
        }
        // Overlay update without auto-scroll (syncPlayheadOverlay would undo
        // our anchored scroll by re-centering on the playhead).
        if (m_playheadOverlay)
            m_playheadOverlay->setPlayheadX(m_videoTrack->secondsToX(m_playheadPos));
    } else {
        syncPlayheadOverlay();
    }
    updateInfoLabel();
}

// I/O markers
void Timeline::markIn() { m_markIn = m_playheadPos; updateInfoLabel(); }
void Timeline::markOut() { m_markOut = m_playheadPos; updateInfoLabel(); }

// Per-clip speed ramp
static const speedramp::SpeedRamp kIdentityRamp = speedramp::SpeedRamp::identity();

const speedramp::SpeedRamp &Timeline::speedRamp(int clipIndex) const
{
    if (clipIndex < 0 || !m_videoTrack) return kIdentityRamp;
    const auto &clips = m_videoTrack->clips();
    if (clipIndex >= clips.size()) return kIdentityRamp;
    return clips[clipIndex].speedRamp;
}

void Timeline::setSpeedRamp(int clipIndex, const speedramp::SpeedRamp &ramp)
{
    if (clipIndex < 0 || !m_videoTrack) return;
    auto clips = m_videoTrack->clips();
    if (clipIndex >= clips.size()) return;
    clips[clipIndex].speedRamp = ramp;
    m_videoTrack->setClips(clips);
    saveUndoState(QString("Set speed ramp clip %1").arg(clipIndex));
    scheduleEmitSequenceChanged();
}

// Clip speed
void Timeline::setClipSpeed(double speed)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    speed = qMax(0.25, qMin(4.0, speed));

    // Modify clips directly (need mutable access)
    auto videoClips = m_videoTrack->clips();
    auto audioClips = m_audioTrack->clips();
    videoClips[sel].speed = speed;
    if (sel < audioClips.size()) audioClips[sel].speed = speed;
    m_videoTrack->setClips(videoClips);
    m_audioTrack->setClips(audioClips);

    saveUndoState(QString("Set speed %1x").arg(speed));
    updateInfoLabel();
}

void Timeline::setClipVolume(double volume, bool recordUndo)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    volume = qMax(0.0, qMin(2.0, volume));
    auto audioClips = m_audioTrack->clips();
    if (sel < audioClips.size()) {
        audioClips[sel].volume = volume;
        m_audioTrack->setClips(audioClips);
    }
    if (recordUndo && !AudioClipEditor::isSliderDragUndoSuppressed())
        saveUndoState(QString("Set volume %1%").arg(static_cast<int>(volume * 100)));
    // Re-emit so AudioMixer picks up the new per-clip volume for the
    // matching PlaybackEntry on its next setSequence call.
    scheduleEmitSequenceChanged();
}

void Timeline::setClipPan(double pan, bool recordUndo)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    pan = qMax(-1.0, qMin(1.0, pan));
    auto audioClips = m_audioTrack->clips();
    if (sel < audioClips.size()) {
        audioClips[sel].pan = pan;
        m_audioTrack->setClips(audioClips);
    }
    if (recordUndo && !AudioClipEditor::isSliderDragUndoSuppressed())
        saveUndoState(QString("Set pan %1").arg(pan, 0, 'f', 2));
    scheduleEmitSequenceChanged();
}

void Timeline::setClipEq(int audioTrackIdx, int clipIdx, bool enabled,
                         const trackfx::EqSettings &eq, bool recordUndo)
{
    if (audioTrackIdx < 0 || audioTrackIdx >= m_audioTracks.size()
        || !m_audioTracks[audioTrackIdx])
        return;
    auto clips = m_audioTracks[audioTrackIdx]->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return;
    clips[clipIdx].clipEq = eq;
    clips[clipIdx].clipEqEnabled = enabled;
    m_audioTracks[audioTrackIdx]->setClips(clips);
    if (recordUndo)
        saveUndoState(QStringLiteral("クリップ EQ を変更"));
    scheduleEmitSequenceChanged();
}

// --- Phase 3: Color correction, effects, keyframes ---

void Timeline::setClipColorCorrection(const ColorCorrection &cc)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    clips[sel].colorCorrection = cc;
    m_videoTrack->setClips(clips);
    saveUndoState("Color correction");
}

void Timeline::setClipColorCorrection(int trackIdx, int clipIdx,
                                      const ColorCorrection &cc)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return;
    auto *track = m_videoTracks[trackIdx];
    if (!track)
        return;

    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return;
    clips[clipIdx].colorCorrection = cc;
    track->setClips(clips);
    saveUndoState("Color correction");
    scheduleEmitSequenceChanged();
}

bool Timeline::setClipLut(int trackIdx, int clipIdx, const QString &lutFilePath,
                          double intensity, QString *err)
{
    auto fail = [err](const QString &message) {
        if (err) *err = message;
        return false;
    };
    auto *track = m_videoTracks.value(trackIdx, nullptr);
    if (!track || clipIdx < 0 || clipIdx >= track->clipCount())
        return fail(QStringLiteral("映像クリップが見つかりません"));
    if (!std::isfinite(intensity))
        return fail(QStringLiteral("LUT 強度には有限の数値を指定してください"));
    const double clampedIntensity = lutFilePath.isEmpty()
        ? 1.0 : qBound(0.0, intensity, 1.0);
    auto clips = track->clips();
    auto &clip = clips[clipIdx];
    if (clip.lutFilePath == lutFilePath
        && std::abs(clip.lutIntensity - clampedIntensity) <= 1e-9)
        return true;
    clip.lutFilePath = lutFilePath;
    clip.lutIntensity = clampedIntensity;
    track->setClips(clips);
    saveUndoState("Clip LUT");
    refreshPlaybackSequence();
    return true;
}

void Timeline::setClipLayerStyle(const LayerStyle &style)
{
    int sel = m_videoTrack->selectedClip();
    setClipLayerStyle(0, sel, style);
}

void Timeline::setClipLayerStyle(int trackIdx, int clipIdx, const LayerStyle &style)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return;
    auto *track = m_videoTracks[trackIdx];
    if (!track)
        return;

    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return;

    clips[clipIdx].layerStyle = style;
    track->setClips(clips);
    saveUndoState("Layer style");
    scheduleEmitSequenceChanged();
}

void Timeline::setClipLayerMaterial(const LayerMaterial &material)
{
    const int selected = m_videoTrack ? m_videoTrack->selectedClip() : -1;
    setClipLayerMaterial(0, selected, material, true);
}

void Timeline::setClipLayerMaterial(int trackIdx, int clipIdx,
                                    const LayerMaterial &material,
                                    bool recordUndo)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return;
    auto *track = m_videoTracks[trackIdx];
    if (!track)
        return;

    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return;

    clips[clipIdx].material = material;
    track->setClips(clips);
    if (recordUndo)
        saveUndoState("Layer material");
    scheduleEmitSequenceChanged();
}

bool ClipInfo::hasMeshWarp() const
{
    if (meshWarp.rows < 2 || meshWarp.cols < 2
        || meshWarp.controlPoints.size() != meshWarp.rows)
        return false;
    bool changed = false;
    for (int r = 0; r < meshWarp.rows; ++r) {
        if (meshWarp.controlPoints[r].size() != meshWarp.cols)
            return false;
        for (int c = 0; c < meshWarp.cols; ++c) {
            const QPointF &p = meshWarp.controlPoints[r][c];
            if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
                return false;
            changed |= std::abs(p.x() - double(c) / (meshWarp.cols - 1)) >= 1e-6
                || std::abs(p.y() - double(r) / (meshWarp.rows - 1)) >= 1e-6;
        }
    }
    return changed;
}

void Timeline::setClipPuppetPins(int trackIdx, int clipIdx, const QVector<WarpPin> &pins)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size()) return;
    auto *track = m_videoTracks[trackIdx];
    if (!track || track->isLocked()) return;
    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) return;
    if (puppetpin::equal(clips[clipIdx].puppetPins, pins)) return;
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    clips[clipIdx].puppetPins = pins;
    track->setClips(clips);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("パペットピン"));
    refreshPlaybackSequence();
}

// Drag preview only; the tool restores its baseline before the undoable commit.
void Timeline::previewClipPuppetPins(int trackIdx, int clipIdx, const QVector<WarpPin> &pins)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size()) return;
    auto *track = m_videoTracks[trackIdx];
    if (!track || track->isLocked()) return;
    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) return;
    clips[clipIdx].puppetPins = pins;
    track->setClips(clips);
    refreshPlaybackSequence();
}

void Timeline::setClipMeshWarp(int trackIdx, int clipIdx, const MeshGrid &grid)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size()) return;
    auto *track = m_videoTracks[trackIdx];
    if (!track || track->isLocked()) return;
    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) return;
    const MeshGrid &old = clips[clipIdx].meshWarp;
    if (old.rows == grid.rows && old.cols == grid.cols
        && old.controlPoints == grid.controlPoints) return;
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    clips[clipIdx].meshWarp = grid;
    track->setClips(clips);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("メッシュワープ"));
    refreshPlaybackSequence();
}

// Drag preview only; the tool restores its baseline before the undoable commit.
void Timeline::previewClipMeshWarp(int trackIdx, int clipIdx, const MeshGrid &grid)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size()) return;
    auto *track = m_videoTracks[trackIdx];
    if (!track || track->isLocked()) return;
    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) return;
    clips[clipIdx].meshWarp = grid;
    track->setClips(clips);
    refreshPlaybackSequence();
}

void Timeline::resetClipMeshWarp(int trackIdx, int clipIdx, int rows, int cols)
{
    if (rows < 2 || cols < 2) return;
    setClipMeshWarp(trackIdx, clipIdx,
                   WarpDistortion::createDefaultMesh(QSize(1, 1), rows, cols));
}

void Timeline::setClipShapeModifiers(int trackIdx, int clipIdx,
                                     const ShapeModifiers &modifiers, bool recordUndo)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size()) return;
    auto *track = m_videoTracks[trackIdx];
    if (!track || track->isLocked()) return;
    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size() || clips[clipIdx].shapes.isEmpty()) return;
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    clips[clipIdx].shapes[0].modifiers = modifiers;
    track->setClips(clips);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    if (recordUndo) saveUndoState(QStringLiteral("シェイプモディファイア"));
    scheduleEmitSequenceChanged();
}

bool Timeline::applyAudioCrossfade(int trackIndex, int clipIndexA,
                                   double durationSec, QString *errorOut)
{
    if (trackIndex < 0 || trackIndex >= m_audioTracks.size()
        || !m_audioTracks[trackIndex]) {
        if (errorOut)
            *errorOut = QStringLiteral("音声トラックの index が範囲外です");
        return false;
    }
    TimelineTrack *track = m_audioTracks[trackIndex];
    if (track->isLocked()) {
        if (errorOut)
            *errorOut = QStringLiteral("音声トラックがロックされています");
        return false;
    }

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = track->clips();
    if (!audioxfade::applyCrossfade(clips, clipIndexA, durationSec, errorOut))
        return false;
    track->setClips(clips);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("音声クロスフェード"));
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::applyAudioFade(int trackIndex, int clipIndex,
                              AudioFadeEdge edge, double durationSec,
                              QString *errorOut)
{
    if (trackIndex < 0 || trackIndex >= m_audioTracks.size()
        || !m_audioTracks[trackIndex]) {
        if (errorOut)
            *errorOut = QStringLiteral("音声トラックの index が範囲外です");
        return false;
    }
    TimelineTrack *track = m_audioTracks[trackIndex];
    if (track->isLocked()) {
        if (errorOut)
            *errorOut = QStringLiteral("音声トラックがロックされています");
        return false;
    }

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    QVector<ClipInfo> clips = track->clips();
    if (!audioxfade::applyFade(clips, clipIndex, edge, durationSec, errorOut))
        return false;
    track->setClips(clips);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(edge == AudioFadeEdge::In
                      ? QStringLiteral("音声フェードイン")
                      : QStringLiteral("音声フェードアウト"));
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::applyTransitionToSelected(const Transition &t)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    if (sel >= clips.size()) return;
    // Boundary transitions apply to BOTH sides of the cut so the user
    // doesn't need two manual operations and the badge appears on both
    // adjacent clips. Pairing rules:
    //   FadeIn  → this.leadIn = FadeIn; prev.trailOut = FadeOut (mirror)
    //   FadeOut → this.trailOut = FadeOut; next.leadIn = FadeIn (mirror)
    //   CrossDissolve / Wipe / Slide → both this.trailOut and next.leadIn
    //                                  set to the SAME type
    // Sequence-edge cases (no neighbour on the paired side) fall back to
    // single-side application — that's how a clip "fades in from black"
    // at the start of the timeline still works.
    if (t.type == TransitionType::FadeIn) {
        clips[sel].leadIn = t;
        if (sel > 0) {
            Transition mirror;
            mirror.type = TransitionType::FadeOut;
            mirror.duration = t.duration;
            mirror.easing = t.easing;
            clips[sel - 1].trailOut = mirror;
        }
    } else if (t.type == TransitionType::FadeOut) {
        clips[sel].trailOut = t;
        if (sel + 1 < clips.size()) {
            Transition mirror;
            mirror.type = TransitionType::FadeIn;
            mirror.duration = t.duration;
            mirror.easing = t.easing;
            clips[sel + 1].leadIn = mirror;
        }
    } else {
        clips[sel].trailOut = t;
        if (sel + 1 < clips.size())
            clips[sel + 1].leadIn = t;
    }
    m_videoTrack->setClips(clips);

    // Mirror to the linked audio track at the same index so AudioMixer
    // applies the matching equal-power crossfade automatically. Pro NLEs
    // (Premiere/DaVinci/FCP X) all default to linked V+A transitions —
    // setting one without the other always sounds wrong (audio just
    // jump-cuts under a video crossfade). Users who want video-only
    // transitions can break the link with the unlink command first.
    if (m_audioTrack) {
        auto aClips = m_audioTrack->clips();
        if (sel < aClips.size()) {
            if (t.type == TransitionType::FadeIn) {
                aClips[sel].leadIn = t;
                if (sel > 0) {
                    Transition mirror;
                    mirror.type = TransitionType::FadeOut;
                    mirror.duration = t.duration;
                    mirror.easing = t.easing;
                    aClips[sel - 1].trailOut = mirror;
                }
            } else if (t.type == TransitionType::FadeOut) {
                aClips[sel].trailOut = t;
                if (sel + 1 < aClips.size()) {
                    Transition mirror;
                    mirror.type = TransitionType::FadeIn;
                    mirror.duration = t.duration;
                    mirror.easing = t.easing;
                    aClips[sel + 1].leadIn = mirror;
                }
            } else {
                aClips[sel].trailOut = t;
                if (sel + 1 < aClips.size())
                    aClips[sel + 1].leadIn = t;
            }
            m_audioTrack->setClips(aClips);
        }
    }

    saveUndoState(QString("Add transition: %1").arg(Transition::typeName(t.type)));
    scheduleEmitSequenceChanged();

    // Handle-shortage advisory. Pure overlap transitions (CrossDissolve /
    // Wipe / Slide / Iris / Dip / Barn / ClockWipe) need source media past
    // the selected clip's outPoint and / or before the next clip's inPoint
    // depending on alignment. We mirror computePlaybackSequence's math
    // here so the message matches what the user will actually see.
    if (isOverlapTransition(t.type) && sel + 1 < clips.size()) {
        const auto &a = clips[sel];
        const auto &b = clips[sel + 1];
        const double aSpeed = (a.speed > 0.0) ? a.speed : 1.0;
        const double bSpeed = (b.speed > 0.0) ? b.speed : 1.0;
        const double aOutPoint = (a.outPoint > 0.0) ? a.outPoint : a.duration;
        const double aTrail = qMax(0.0, (a.duration - aOutPoint) / aSpeed);
        const double bLead  = qMax(0.0, b.inPoint / bSpeed);
        const double askedD = t.duration;
        double aExtend = 0.0, bRetract = 0.0;
        switch (t.alignment) {
            case TransitionAlignment::Start:
                aExtend  = qMin(askedD, aTrail);
                break;
            case TransitionAlignment::End:
                bRetract = qMin(askedD, bLead);
                break;
            case TransitionAlignment::Center:
                aExtend  = qMin(askedD * 0.5, aTrail);
                bRetract = qMin(askedD * 0.5, bLead);
                if (aExtend + bRetract < askedD) {
                    if (bRetract < askedD * 0.5) {
                        aExtend += qMin(askedD - aExtend - bRetract,
                                        aTrail - aExtend);
                    }
                    if (aExtend + bRetract < askedD && aExtend < askedD * 0.5) {
                        bRetract += qMin(askedD - aExtend - bRetract,
                                         bLead - bRetract);
                    }
                }
                break;
        }
        const double effectiveD = aExtend + bRetract;
        if (effectiveD + 0.005 < askedD) {
            emit transitionShortened(Transition::typeName(t.type),
                                     askedD, effectiveD);
        }
    }
}

void Timeline::clearTransitionsOnSelected()
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    if (sel >= clips.size()) return;
    const bool hadLead = clips[sel].leadIn.type != TransitionType::None;
    const bool hadTrail = clips[sel].trailOut.type != TransitionType::None;
    if (!hadLead && !hadTrail) return;
    // Mirror teardown — applyTransitionToSelected pairs leadIn/trailOut
    // across adjacent clips (FadeIn pairs with prev.trailOut=FadeOut,
    // FadeOut with next.leadIn=FadeIn, CrossDissolve/Wipe/Slide with the
    // same type on the other side). Clearing only this clip's slots
    // would leave the neighbour with a half-paired transition that fires
    // an audio fade or visual blend with nothing to blend against.
    if (hadLead && sel > 0
        && clips[sel - 1].trailOut.type != TransitionType::None) {
        clips[sel - 1].trailOut = Transition{};
    }
    if (hadTrail && sel + 1 < clips.size()
        && clips[sel + 1].leadIn.type != TransitionType::None) {
        clips[sel + 1].leadIn = Transition{};
    }
    clips[sel].leadIn = Transition{};
    clips[sel].trailOut = Transition{};
    m_videoTrack->setClips(clips);

    // Mirror teardown on the linked audio track. AudioMixer reads its
    // crossfade window from PlaybackEntry.leadIn/trailOut which is sourced
    // from the audio clips, so leaving them set here would keep the audio
    // fading even after the visual transition is gone.
    if (m_audioTrack) {
        auto aClips = m_audioTrack->clips();
        if (sel < aClips.size()) {
            if (hadLead && sel > 0
                && aClips[sel - 1].trailOut.type != TransitionType::None) {
                aClips[sel - 1].trailOut = Transition{};
            }
            if (hadTrail && sel + 1 < aClips.size()
                && aClips[sel + 1].leadIn.type != TransitionType::None) {
                aClips[sel + 1].leadIn = Transition{};
            }
            aClips[sel].leadIn = Transition{};
            aClips[sel].trailOut = Transition{};
            m_audioTrack->setClips(aClips);
        }
    }

    saveUndoState("Clear transitions");
    scheduleEmitSequenceChanged();
}

void Timeline::setClipEffects(const QVector<VideoEffect> &effects)
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0) return;
    auto clips = m_videoTrack->clips();
    clips[sel].effects = effects;
    m_videoTrack->setClips(clips);
    saveUndoState("Video effects");
}

void Timeline::setClipEffectsAndKeyframes(int trackIdx, int clipIdx,
                                          const QVector<VideoEffect> &effects,
                                          const KeyframeManager &km)
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return;
    auto *track = m_videoTracks[trackIdx];
    if (!track)
        return;

    auto clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return;

    clips[clipIdx].effects = effects;
    clips[clipIdx].keyframes = km;
    track->setClips(clips);
    saveUndoState("Video effects");
    scheduleEmitSequenceChanged();
}

void Timeline::setClipKeyframes(const KeyframeManager &km)
{
    if (!m_videoTrack)
        return;
    int sel = m_videoTrack->selectedClip();
    if (sel < 0)
        return;
    setClipKeyframes(TrackKind::Video, 0, sel, km);
}

bool Timeline::setClipKeyframes(TrackKind kind, int trackIndex, int clipIndex,
                                const KeyframeManager &km)
{
    if (kind != TrackKind::Video)
        return false;
    TimelineTrack *track = trackAt(false, trackIndex);
    if (!track)
        return false;
    QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size())
        return false;
    clips[clipIndex].keyframes = km;
    track->setClips(clips);
    saveUndoState(QStringLiteral("Keyframes"));
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::applyDynamicZoom(TrackKind kind, int trackIndex, int clipIndex,
                                const dynzoom::Rect& start,
                                const dynzoom::Rect& end,
                                dynzoom::Easing easing)
{
    if (kind != TrackKind::Video)
        return false;
    TimelineTrack *track = trackAt(false, trackIndex);
    if (!track || track->isLocked())
        return false;

    QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size())
        return false;
    const double duration = clips.at(clipIndex).effectiveDuration();
    if (!std::isfinite(duration) || duration <= 0.0)
        return false;
    const auto validRect = [](const dynzoom::Rect& rect) {
        return std::isfinite(rect.cx) && std::isfinite(rect.cy)
            && std::isfinite(rect.w) && rect.w > 0.0;
    };
    if (!validRect(start) || !validRect(end))
        return false;

    const dynzoom::Result generated = dynzoom::build(
        start, end, 0.0, duration, easing);
    KeyframeManager keyframes = clips.at(clipIndex).keyframes;
    keyframes.addTrack(generated.positionX);
    keyframes.addTrack(generated.positionY);
    keyframes.addTrack(generated.scaleX);
    keyframes.addTrack(generated.scaleY);

    // Keep the four TransformAnimator tracks as the sole stored source.
    // clipanim resolves the runtime motion.* names from these public tracks
    // at read time, so no mirrored tracks are persisted. Removing stale
    // mirrors also migrates projects saved by the older dual-write scheme.
    keyframes.removeTrack(QStringLiteral("motion.position.x"));
    keyframes.removeTrack(QStringLiteral("motion.position.y"));
    keyframes.removeTrack(QStringLiteral("motion.scale"));

    return setClipKeyframes(kind, trackIndex, clipIndex, keyframes);
}

ColorCorrection Timeline::clipColorCorrection() const
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return {};
    return m_videoTrack->clips()[sel].colorCorrection;
}

LayerStyle Timeline::clipLayerStyle() const
{
    int sel = m_videoTrack->selectedClip();
    return clipLayerStyle(0, sel);
}

LayerStyle Timeline::clipLayerStyle(int trackIdx, int clipIdx) const
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return {};
    const auto *track = m_videoTracks[trackIdx];
    if (!track)
        return {};
    const auto &clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return {};
    return clips[clipIdx].layerStyle;
}

LayerMaterial Timeline::clipLayerMaterial() const
{
    const int selected = m_videoTrack ? m_videoTrack->selectedClip() : -1;
    return clipLayerMaterial(0, selected);
}

LayerMaterial Timeline::clipLayerMaterial(int trackIdx, int clipIdx) const
{
    if (trackIdx < 0 || trackIdx >= m_videoTracks.size())
        return {};
    const auto *track = m_videoTracks[trackIdx];
    if (!track)
        return {};
    const auto &clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return {};
    return clips[clipIdx].material;
}

QVector<VideoEffect> Timeline::clipEffects() const
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return {};
    return m_videoTrack->clips()[sel].effects;
}

KeyframeManager Timeline::clipKeyframes() const
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return {};
    return m_videoTrack->clips()[sel].keyframes;
}

double Timeline::selectedClipDuration() const
{
    int sel = m_videoTrack->selectedClip();
    if (sel < 0 || sel >= m_videoTrack->clips().size()) return 0.0;
    return m_videoTrack->clips()[sel].effectiveDuration();
}

int Timeline::selectedVideoClipIndex() const
{
    if (!m_videoTrack) return -1;
    return m_videoTrack->selectedClip();
}

bool Timeline::insertClip3PointActive(double timelineStartSec, const ClipInfo &clip)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first()) {
        emit statusMessageRequested(QStringLiteral("タイムラインがありません"), 5000);
        return false;
    }
    auto *track = m_videoTracks.first();
    if (track->isLocked()) {
        emit statusMessageRequested(QStringLiteral("ロック中のトラックには配置できません"), 5000);
        return false;
    }
    ClipKeyRemapBatch keyRemapBatch(this);
    const TrackClipSnapshot before = snapshotTrackClips(this);
    track->insertClip3Point(timelineStartSec, clip);
    // US-1203: storyline mode keeps V1 gap-free (an insert past the end).
    if (m_storylineMode)
        closeGapsOnTrack(track);
    remapCarriersAfterExternalMutation(before);
    saveUndoState("3点編集: インサート");
    return true;
}

bool Timeline::overwriteClip3PointActive(double timelineStartSec, const ClipInfo &clip)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first()) {
        emit statusMessageRequested(QStringLiteral("タイムラインがありません"), 5000);
        return false;
    }
    auto *track = m_videoTracks.first();
    if (track->isLocked()) {
        emit statusMessageRequested(QStringLiteral("ロック中のトラックには配置できません"), 5000);
        return false;
    }
    ClipKeyRemapBatch keyRemapBatch(this);
    const TrackClipSnapshot before = snapshotTrackClips(this);
    const auto spans = rangeRemovedSpans(
        track, {{timelineStartSec, timelineStartSec + clip.effectiveDuration()}});
    for (auto it = spans.cbegin(); it != spans.cend(); ++it)
        m_pendingDeletedUids[it.key()] += it.value();
    track->overwriteClip3Point(timelineStartSec, clip);
    remapCarriersAfterExternalMutation(before);
    saveUndoState("3点編集: 上書き");
    return true;
}

bool Timeline::insertShapeClipAtPlayhead(const ClipInfo &clip)
{
    if (clip.shapes.isEmpty() || m_videoTracks.isEmpty())
        return false;

    TimelineTrack *track = nullptr;
    if (m_activeVideoTrackIndex >= 0
        && m_activeVideoTrackIndex < m_videoTracks.size()) {
        track = m_videoTracks[m_activeVideoTrackIndex];
    }
    if (!track)
        track = m_videoTracks.first();
    if (!track || track->isLocked())
        return false;

    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    track->insertClip3Point(qMax(0.0, m_playheadPos), clip);
    remapTrackMatteLocked(snapBefore);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
    saveUndoState(QStringLiteral("シェイプクリップを挿入"));
    updateInfoLabel();
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::rippleDeleteTimeRangeActive(double startSec, double endSec)
{
    if (m_videoTracks.isEmpty())
        return;
    const TrackClipSnapshot snapBefore = snapshotTrackClips(this);
    // アクティブ動画トラック (無ければ先頭 V1) を選ぶ。
    TimelineTrack *track = nullptr;
    if (m_activeVideoTrackIndex >= 0
        && m_activeVideoTrackIndex < m_videoTracks.size())
        track = m_videoTracks[m_activeVideoTrackIndex];
    if (!track)
        track = m_videoTracks.first();
    if (!track)
        return;
    if (track == m_videoTracks.first()) {
        const auto spans = rangeRemovedSpans(track, {{startSec, endSec}});
        for (auto it = spans.cbegin(); it != spans.cend(); ++it)
            m_pendingDeletedUids[it.key()] += it.value();
    }
    const bool changed = track->rippleDeleteTimeRange(startSec, endSec);
    // no-op だった場合 (範囲外/空) は Undo スナップショットを積まない。
    if (changed) {
        remapTrackMatteLocked(snapBefore);
        remapClipParentEntriesAfterMutation(this, m_clipParentEntries, snapBefore);
        saveUndoState("リップル削除 (範囲)");
    }
}

bool Timeline::applyTrimActive(trimops::TrimType type, double deltaSec,
                               QString *errorOut)
{
    // アクティブ動画トラックの現在選択中クリップへトリムを適用する。
    // Roll は「選択クリップ = 編集点の左側クリップ」として扱うので、trimops::
    // applyTrim が clip[sel]/clip[sel+1] の編集点を動かす。選択が無ければ失敗。
    if (m_videoTracks.isEmpty()) {
        if (errorOut) *errorOut = QObject::tr("動画トラックがありません");
        return false;
    }
    TimelineTrack *track = nullptr;
    // US-1203: a storyline-mode ripple trim (Q / W) works on V1 only.
    const bool storylineRipple = m_storylineMode
        && (type == trimops::TrimType::RippleIn || type == trimops::TrimType::RippleOut);
    if (storylineRipple) {
        TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
        const int v1Sel = v1 ? v1->selectedClip() : -1;
        if (!v1 || v1Sel < 0 || v1Sel >= v1->clipCount()) {
            if (errorOut) *errorOut = QObject::tr("V1 のクリップを選択してください");
            return false;
        }
        track = v1;
        m_activeVideoTrackIndex = 0;
    } else if (m_activeVideoTrackIndex >= 0
        && m_activeVideoTrackIndex < m_videoTracks.size()) {
        auto *candidate = m_videoTracks[m_activeVideoTrackIndex];
        const int candidateSel = candidate ? candidate->selectedClip() : -1;
        if (candidate && candidateSel >= 0 && candidateSel < candidate->clipCount())
            track = candidate;
    }
    if (!track) {
        for (int i = 0; i < m_videoTracks.size(); ++i) {
            auto *candidate = m_videoTracks[i];
            const int candidateSel = candidate ? candidate->selectedClip() : -1;
            if (candidate && candidateSel >= 0 && candidateSel < candidate->clipCount()) {
                track = candidate;
                m_activeVideoTrackIndex = i;
                break;
            }
        }
    }
    if (!track) {
        if (errorOut) *errorOut = QObject::tr("トリム対象のクリップが選択されていません");
        return false;
    }
    const int sel = track->selectedClip();
    if (sel < 0 || sel >= track->clipCount()) {
        if (errorOut) *errorOut = QObject::tr("トリム対象のクリップが選択されていません");
        return false;
    }
    if (!applyTrimLinked(track, sel, type, deltaSec, errorOut))
        return false;
    if (storylineRipple)
        closeGapsOnTrack(track);
    saveUndoState("トリム");
    return true;
}

bool Timeline::applyTrimLinked(TimelineTrack *track, int clipIndex,
                               trimops::TrimType type, double deltaSec,
                               QString *errorOut)
{
    if (!track || clipIndex < 0 || clipIndex >= track->clipCount()) {
        if (errorOut) *errorOut = QObject::tr("トリム対象のクリップがありません");
        return false;
    }
    const int linkGroup = track->clips().at(clipIndex).linkGroup;
    const QVector<ClipInfo> sourceBefore = track->clips();
    const bool storylineRipple = m_storylineMode && track == m_videoTracks.value(0, nullptr)
        && (type == trimops::TrimType::RippleIn || type == trimops::TrimType::RippleOut);
    if (!track->applyTrim(clipIndex, type, deltaSec, errorOut))
        return false;
    // US-1203: V1 is gap-free before its linked audio follows, so the followers'
    // targets come from the final V1 layout (a gapped V1 closes here too).
    if (storylineRipple)
        closeGapsOnTrack(track);
    if (linkGroup <= 0)
        return true;
    const TrackSelectionUids selectionBefore =
        storylineRipple ? selectedUidsByTrack(m_audioTracks) : TrackSelectionUids{};
    TrackSelectionUids selection = selectionBefore;

    // Linked partners live on other tracks (V1 clip <-> A1 clip). Apply the
    // identical trim so audio stays in sync with the picture; if the partner
    // cannot take the trim (shorter source, boundary), restore the source track.
    QVector<QPair<TimelineTrack *, QVector<ClipInfo>>> touched;
    const QVector<TimelineTrack *> partnerTracks = m_videoTracks + m_audioTracks;
    for (TimelineTrack *partner : partnerTracks) {
        if (!partner || partner == track)
            continue;
        for (int i = 0; i < partner->clipCount(); ++i) {
            if (partner->clips().at(i).linkGroup != linkGroup)
                continue;
            const QVector<ClipInfo> partnerBefore = partner->clips();
            QString partnerError;
            if (!partner->applyTrim(i, type, deltaSec, &partnerError)) {
                for (int k = touched.size() - 1; k >= 0; --k)
                    touched[k].first->setClips(touched[k].second);
                track->setClips(sourceBefore);
                reselectByUid(selectionBefore);
                if (errorOut) {
                    *errorOut = QObject::tr("リンクしたクリップをトリムできません: %1")
                                    .arg(partnerError);
                }
                return false;
            }
            touched.append(qMakePair(partner, partnerBefore));
            // US-1203: a storyline-mode ripple trim on V1 ripples V1 only. The
            // partner keeps its trim; its own track gets the shift back. The
            // linked audio of the V1 clips after the trimmed one ripples with
            // them: taken out here and put back shifted (evacuated on a
            // collision). An extension that would push the next other clip on
            // the partner's track is refused.
            if (storylineRipple && i + 1 < partner->clipCount()) {
                const auto beforeStarts = clipStartSecs(partnerBefore);
                const double endBeforeSec = beforeStarts[i] + partnerBefore[i].effectiveDuration();
                const double shiftSec = clipStartSecs(partner->clips())[i]
                    + partner->clips().at(i).effectiveDuration() - endBeforeSec;
                const int partnerAudioIndex = m_audioTracks.indexOf(partner);
                // linkGroup -> how far its V1 clip moved (trim and gap closing).
                QHash<int, double> followingShiftSec;
                {
                    const auto startsBefore = clipStartSecs(sourceBefore);
                    const auto startsNow = clipStartSecs(track->clips());
                    for (int v = clipIndex + 1; v < track->clipCount(); ++v) {
                        const int group = track->clips().at(v).linkGroup;
                        if (group > 0 && !followingShiftSec.contains(group))
                            followingShiftSec.insert(group, startsNow[v] - startsBefore[v]);
                    }
                }
                struct Follower { ClipInfo clip; double originalSec; bool selected; };
                QVector<Follower> followers;  // ascending original start
                const bool movable = partnerAudioIndex >= 0 && !partner->isLocked();
                for (int j = partner->clipCount() - 1; movable && j > i; --j) {
                    if (!followingShiftSec.contains(partner->clips().at(j).linkGroup)) continue;
                    followers.prepend({partner->clips().at(j), beforeStarts[j], partner->isClipSelected(j)});
                    partner->removeClipPreservingDownstream(j, false);
                }
                QVector<ClipInfo> clips = partner->clips();
                if (i + 1 < clips.size()) {
                    const double nextLeadInSec = clips[i + 1].leadInSec - shiftSec;
                    if (nextLeadInSec < -kConnectionEps) {
                        for (int k = touched.size() - 1; k >= 0; --k)
                            touched[k].first->setClips(touched[k].second);
                        track->setClips(sourceBefore);
                        reselectByUid(selectionBefore);
                        onTrackModified();
                        if (errorOut)
                            *errorOut = QObject::tr("リンクしたクリップの後ろに空きが無いため伸ばせません");
                        return false;
                    }
                    clips[i + 1].leadInSec = qMax(0.0, nextLeadInSec);
                    partner->setClips(clips);
                }
                // As in followStorylineLinks: a pending follower owns its uid on
                // its own track, and the fallback is its own start, else the end.
                QHash<int, QHash<QString, int>> reservedUids;
                for (const Follower &f : followers)
                    if (!f.clip.uid.isEmpty()) ++reservedUids[partnerAudioIndex][f.clip.uid];
                for (const Follower &f : followers) {
                    if (!f.clip.uid.isEmpty()) {
                        QHash<QString, int> &own = reservedUids[partnerAudioIndex];
                        if (--own[f.clip.uid] <= 0) own.remove(f.clip.uid);
                    }
                    TimelineTrack::DropPlan plan;
                    const double targetSec =
                        qMax(0.0, f.originalSec + followingShiftSec.value(f.clip.linkGroup));
                    TimelineTrack *destination = storylineAudioSlot(
                        partnerAudioIndex, f.clip, targetSec, &plan, &reservedUids);
                    if (!destination) {
                        destination = partner;
                        plan = partner->planDrop(f.originalSec, f.clip.effectiveDuration());
                        if (!plan.valid) {
                            double endSec = 0.0;
                            for (const ClipInfo &c : partner->clips())
                                endSec += c.leadInSec + c.effectiveDuration();
                            plan = partner->planDrop(endSec, f.clip.effectiveDuration());
                        }
                    }
                    // An evacuation track is restored too if a later partner refuses.
                    bool known = false;
                    for (const auto &entry : touched)
                        known = known || entry.first == destination;
                    if (!known)
                        touched.append(qMakePair(destination, destination->clips()));
                    destination->insertClipPreservingDownstream(plan.insertIdx, f.clip, plan.newLeadIn);
                    if (f.selected) {
                        selection[partner].removeOne(f.clip.uid);
                        selection[destination].append(f.clip.uid);
                    }
                }
                reselectByUid(selection);
            }
            break;
        }
    }
    return true;
}

void Timeline::addAudioFile(const QString &filePath)
{
    AVFormatContext *fmt = nullptr;
    double duration = 0.0;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0)
            duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        avformat_close_input(&fmt);
    }
    ClipInfo clip;
    clip.filePath = filePath;
    clip.displayName = QFileInfo(filePath).fileName();
    clip.duration = duration;
    // Solo audio imports still get a unique linkGroup so they participate
    // in the selection/drag/delete plumbing; they just have no partner.
    clip.linkGroup = allocateLinkGroup();

    // Add to first audio track (or second if exists for BGM)
    TimelineTrack *target = m_audioTracks.size() > 1 ? m_audioTracks[1] : m_audioTrack;
    target->addClip(clip);
    saveUndoState("Add audio");
    updateInfoLabel();
}

void Timeline::insertAudioClipAtPlayhead(const QString &wavPath, int trackIdx)
{
    // Probe duration via FFmpeg
    AVFormatContext *fmt = nullptr;
    double duration = 0.0;
    if (avformat_open_input(&fmt, wavPath.toUtf8().constData(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0)
            duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        avformat_close_input(&fmt);
    }

    ClipInfo clip;
    clip.filePath = wavPath;
    clip.displayName = QFileInfo(wavPath).fileName();
    clip.duration = duration;
    clip.linkGroup = allocateLinkGroup();

    // Determine target audio track: trackIdx is 1-based (2 = A2).
    // If the requested track doesn't exist, create it.
    int targetIndex = trackIdx - 1; // convert to 0-based
    if (targetIndex < 0 || targetIndex >= m_audioTracks.size()) {
        // Create tracks up to the requested one
        while (m_audioTracks.size() <= targetIndex) {
            addAudioTrack(false);
        }
    }

    TimelineTrack *target = m_audioTracks[targetIndex];
    if (!target) return;

    // Insert at the playhead position while keeping later material stable
    // when there is enough gap, and splice the occupied clip when there isn't.
    const double playheadSec = qMax(0.0, m_playheadPos);
    double cursor = 0.0;
    const auto &clips = target->clips();
    for (int i = 0; i < clips.size(); ++i) {
        const double clipStart = cursor + clips[i].leadInSec;
        const double clipEnd = clipStart + clips[i].effectiveDuration();
        if (playheadSec < clipStart - 1e-6) {
            // Playhead sits in the gap before this clip — drop it there and
            // let downstream clips keep their absolute positions.
            target->insertClipPreservingDownstream(i, clip, playheadSec - cursor);
            saveUndoState("Insert voice-over");
            updateInfoLabel();
            scheduleEmitSequenceChanged();
            return;
        }
        if (playheadSec < clipEnd - 1e-6) {
            // Playhead is inside this clip. If it falls well inside, splice the
            // clip into two around the inserted clip; otherwise snap to the
            // nearer edge.
            const double localTimelineSec = playheadSec - clipStart;
            const double effectiveStart = clips[i].inPoint;
            const double effectiveEnd = (clips[i].outPoint > 0.0)
                ? clips[i].outPoint : clips[i].duration;
            const double splitPoint = effectiveStart + localTimelineSec * clips[i].speed;
            if (splitPoint > effectiveStart + 0.1 && splitPoint < effectiveEnd - 0.1) {
                QVector<ClipInfo> updated = clips;
                ClipInfo rightHalf = updated[i];
                rightHalf.inPoint = splitPoint;
                rightHalf.outPoint = effectiveEnd;
                rightHalf.leadInSec = 0.0;
                rightHalf.leadIn = Transition{};
                updated[i].outPoint = splitPoint;
                updated[i].trailOut = Transition{};
                clip.leadInSec = 0.0;
                updated.insert(i + 1, clip);
                updated.insert(i + 2, rightHalf);
                target->setClips(updated);
            } else if (playheadSec - clipStart < clipEnd - playheadSec) {
                target->insertClipPreservingDownstream(i, clip, playheadSec - cursor);
            } else {
                target->insertClipPreservingDownstream(i + 1, clip, 0.0);
            }
            saveUndoState("Insert voice-over");
            updateInfoLabel();
            scheduleEmitSequenceChanged();
            return;
        }
        cursor = clipEnd;
    }

    // Playhead is past every clip — append with a lead-in gap that lands it at
    // the playhead position.
    target->insertClipPreservingDownstream(clips.size(), clip, playheadSec - cursor);
    saveUndoState("Insert voice-over");
    updateInfoLabel();
    scheduleEmitSequenceChanged();
}

bool Timeline::replaceAudioClipMedia(int trackIdx, int clipIdx,
                                     const QString &wavPath)
{
    if (wavPath.isEmpty() || trackIdx < 0 || trackIdx >= m_audioTracks.size())
        return false;
    TimelineTrack *track = m_audioTracks[trackIdx];
    if (!track || clipIdx < 0 || clipIdx >= track->clips().size())
        return false;

    QVector<ClipInfo> updated = track->clips();
    if (updated[clipIdx].filePath == wavPath)
        return true;
    updated[clipIdx].filePath = wavPath;
    updated[clipIdx].displayName = QFileInfo(wavPath).fileName();
    updated[clipIdx].waveform = WaveformData{};
    track->setClips(updated);
    saveUndoState(QStringLiteral("音声分離を適用"));
    scheduleEmitSequenceChanged();
    updateInfoLabel();
    return true;
}

void Timeline::toggleMuteTrack(int audioTrackIndex)
{
    if (audioTrackIndex < 0 || audioTrackIndex >= m_audioTracks.size()) return;
    auto *track = m_audioTracks[audioTrackIndex];
    track->setMuted(!track->isMuted());
    syncTrackHeaderFlags(track);
    // Re-emit so AudioMixer picks up the new audioMuted flag for every
    // entry on this track. Without this the mute toggle stayed silent
    // until the next clip edit triggered a sequence rebuild.
    scheduleEmitSequenceChanged();
    updateInfoLabel();
}

void Timeline::toggleSoloTrack(int audioTrackIndex)
{
    if (audioTrackIndex < 0 || audioTrackIndex >= m_audioTracks.size()) return;
    bool newSolo = !m_audioTracks[audioTrackIndex]->isSolo();
    // Clear all solo first, then set the target
    for (int i = 0; i < m_audioTracks.size(); ++i) {
        const bool wasSolo = m_audioTracks[i]->isSolo();
        const bool nowSolo = (i == audioTrackIndex && newSolo);
        if (wasSolo != nowSolo) {
            m_audioTracks[i]->setSolo(nowSolo);
            syncTrackHeaderFlags(m_audioTracks[i]);
            emit trackSoloChanged(i, nowSolo);
        }
    }
    // Solo state lives on the mixer (per-track effectiveGain), so the
    // PlaybackEntry-shaped schedule is unchanged — no audioSequenceChanged
    // re-emit needed. trackSoloChanged carries the only state the mixer
    // cares about and the track header UI updates from setSolo directly.
    updateInfoLabel();
}

void Timeline::normalizeAudioClipPeak(int trackIdx, int clipIdx)
{
    if (trackIdx < 0 || trackIdx >= m_audioTracks.size()) return;
    TimelineTrack *track = m_audioTracks[trackIdx];
    if (!track || clipIdx < 0 || clipIdx >= track->clips().size()) return;

    const ClipInfo clipSrc = track->clips()[clipIdx];
    ClipInfo clip = clipSrc;
    const QString filePath = clip.filePath;
    if (filePath.isEmpty()) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: ファイルを読めません."), 5000);
        return;
    }

    NormalizeDecoderCtx dec;
    if (!dec.open(filePath)) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: ファイルを読めません."), 5000);
        return;
    }

    const double fileInSec = clip.inPoint;
    const double fileOutSec = (clip.outPoint > 0.0) ? clip.outPoint : clip.duration;
    const double searchLenSec = fileOutSec - fileInSec;
    if (searchLenSec <= 0.0) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: 信号が検出されませんでした."), 5000);
        return;
    }

    const int channels = dec.codec->ch_layout.nb_channels;
    const int sampleRate = dec.codec->sample_rate;
    const int stream = dec.streamIdx();
    const double timebase = av_q2d(dec.fmt->streams[stream]->time_base);

    // Seek to in-point
    const int64_t seekTs = static_cast<int64_t>(fileInSec / timebase);
    av_seek_frame(dec.fmt, stream, seekTs, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec.codec);

    // Progress dialog for clips > 60 seconds (existing pattern from MainWindow)
    QProgressDialog *progress = nullptr;
    bool cancelled = false;
    if (searchLenSec > 60.0) {
        progress = new QProgressDialog(QStringLiteral("ノーマライズ中..."),
                                        QStringLiteral("キャンセル"), 0, 100, this);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(0);
        progress->setValue(0);
    }

    double maxSampleAbs = 0.0;
    int64_t lastPts = AV_NOPTS_VALUE;

    while (av_read_frame(dec.fmt, dec.pkt) >= 0) {
        if (dec.pkt->stream_index != stream) {
            av_packet_unref(dec.pkt);
            continue;
        }

        if (progress && progress->wasCanceled()) {
            cancelled = true;
            av_packet_unref(dec.pkt);
            break;
        }

        if (avcodec_send_packet(dec.codec, dec.pkt) < 0) {
            av_packet_unref(dec.pkt);
            continue;
        }

        while (avcodec_receive_frame(dec.codec, dec.frame) >= 0) {
            if (dec.frame->pts != AV_NOPTS_VALUE)
                lastPts = dec.frame->pts;
            if (lastPts == AV_NOPTS_VALUE) {
                av_frame_unref(dec.frame);
                continue;
            }

            const double frameTimeSec = static_cast<double>(lastPts) * timebase;
            const double frameEndSec =
                frameTimeSec + static_cast<double>(dec.frame->nb_samples) / sampleRate;

            // Entire frame before or after the clip window → skip
            if (frameEndSec <= fileInSec || frameTimeSec >= fileOutSec) {
                av_frame_unref(dec.frame);
                continue;
            }

            // Clip the sample range to the clip window
            int sStart = 0;
            int sEnd = dec.frame->nb_samples;
            if (frameTimeSec < fileInSec) {
                sStart = static_cast<int>((fileInSec - frameTimeSec) * sampleRate);
                if (sStart > dec.frame->nb_samples) sStart = dec.frame->nb_samples;
            }
            if (frameEndSec > fileOutSec) {
                sEnd = dec.frame->nb_samples
                     - static_cast<int>((frameEndSec - fileOutSec) * sampleRate);
                if (sEnd < sStart) sEnd = sStart;
            }

            // Walk every channel's samples and track max |sample|
            const auto sampleFmt = dec.codec->sample_fmt;
            const bool planar = av_sample_fmt_is_planar(sampleFmt) != 0;
            for (int ch = 0; ch < channels; ++ch) {
                for (int s = sStart; s < sEnd; ++s) {
                    const int idx = planar ? s : (s * channels + ch);
                    const uint8_t *buf = planar ? dec.frame->data[ch]
                                                : dec.frame->data[0];
                    if (!buf) break;
                    double val = 0.0;
                    switch (sampleFmt) {
                    case AV_SAMPLE_FMT_S16:
                    case AV_SAMPLE_FMT_S16P:
                        val = std::abs(static_cast<double>(
                            reinterpret_cast<const int16_t *>(buf)[idx]));
                        break;
                    case AV_SAMPLE_FMT_S32:
                    case AV_SAMPLE_FMT_S32P:
                        val = std::abs(static_cast<double>(
                            reinterpret_cast<const int32_t *>(buf)[idx]))
                            / 2147483648.0 * 32768.0;
                        break;
                    case AV_SAMPLE_FMT_FLT:
                    case AV_SAMPLE_FMT_FLTP:
                        val = std::abs(static_cast<double>(
                            reinterpret_cast<const float *>(buf)[idx]))
                            * 32768.0;
                        break;
                    case AV_SAMPLE_FMT_DBL:
                    case AV_SAMPLE_FMT_DBLP:
                        val = std::abs(
                            reinterpret_cast<const double *>(buf)[idx])
                            * 32768.0;
                        break;
                    default:
                        break;
                    }
                    if (val > maxSampleAbs) maxSampleAbs = val;
                }
            }

            av_frame_unref(dec.frame);

            if (progress && !cancelled) {
                const double pct = (frameTimeSec - fileInSec) / searchLenSec * 100.0;
                progress->setValue(static_cast<int>(qBound(0.0, pct, 100.0)));
                QCoreApplication::processEvents();
            }
        }
        av_packet_unref(dec.pkt);
    }

    if (progress) {
        progress->close();
        delete progress;
    }

    if (cancelled) {
        emit statusMessageRequested(QStringLiteral("ノーマライズがキャンセルされました."), 3000);
        return;
    }

    // Result: peak analysis
    if (maxSampleAbs < 1e-12) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: 信号が検出されませんでした."), 5000);
        return;
    }

    const double peakDb = 20.0 * std::log10(maxSampleAbs / 32768.0);
    if (peakDb < -90.0) {
        emit statusMessageRequested(QStringLiteral("ノーマライズ失敗: 信号が検出されませんでした."), 5000);
        return;
    }

    double gainDb = -1.0 - peakDb;  // target peak = -1 dBFS
    gainDb = qBound(-24.0, gainDb, 12.0);  // clamp gain to [-24, +12]

    const double oldVol = clip.volume;
    const double gainLinear = std::pow(10.0, gainDb / 20.0);
    const double newVol = qBound(0.0, oldVol * gainLinear, 4.0);

    // Snapshot pre-mutation so Ctrl+Z restores the original volume.
    // Mirrors MainWindow::onMeterRequestNormalize ordering (saveState then
    // mutate). Must run before track->setClips below.
    saveUndoState(QStringLiteral("Normalize audio clip"));

    clip.volume = newVol;
    auto clips = track->clips();
    clips[clipIdx] = clip;
    track->setClips(clips);

    scheduleEmitSequenceChanged();

    const QString msg =
        QStringLiteral("A%1 クリップ %2 ノーマライズ: peak %3dB → gain %4→%5")
            .arg(trackIdx + 1)
            .arg(clipIdx + 1)
            .arg(peakDb, 0, 'f', 1)
            .arg(oldVol, 0, 'f', 2)
            .arg(clip.volume, 0, 'f', 2);
    emit statusMessageRequested(msg, 5000);
}

void Timeline::setPlayheadPosition(double seconds)
{
    if (m_skimmingEnabled) clearSkim();
    m_playheadPos = qMax(0.0, seconds);
    syncPlayheadOverlay();
    // Chase mode: when playback or an external seek pushes the playhead
    // out of the central 50% comfort band, scroll the timeline so the
    // playhead lands back in the middle. Skipped while the user is
    // actively dragging — resolvePlayheadDragX handles that case with a
    // looser pin so a plain click in the outer band still lands the bar
    // exactly where the user clicked.
    if (!m_playheadDragging) ensurePlayheadVisible();
}

void Timeline::ensurePlayheadVisible()
{
    if (!m_scrollArea || !m_scrollArea->viewport() || !m_videoTrack) return;
    QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
    if (!hbar) return;
    const int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 0) return;
    const int playheadX = m_videoTrack->secondsToX(m_playheadPos);
    const int viewportX = playheadX - hbar->value();
    const int leftZone  = static_cast<int>(viewportW * 0.25);
    const int rightZone = static_cast<int>(viewportW * 0.75);
    if (viewportX >= leftZone && viewportX <= rightZone) return;
    // Re-centre at the 50% mark so the user has equal forward / backward
    // headroom after the chase scroll. Clamp so we never push past the
    // scroll rails (start of timeline at min, last-clip-end + trailing
    // pad at max).
    const int target = viewportW / 2;
    const int newScroll = qBound(hbar->minimum(),
                                 playheadX - target,
                                 hbar->maximum());
    if (newScroll != hbar->value()) hbar->setValue(newScroll);
}

double Timeline::totalDuration() const
{
    // Maximum end time across ALL video tracks. Each track lays its clips out
    // sequentially from t=0, so each track's contribution is the sum of its
    // clip effective durations. Total sequence length is the longest track.
    double maxEnd = 0.0;
    for (auto *track : m_videoTracks) {
        if (!track) continue;
        double accum = 0.0;
        for (const auto &c : track->clips())
            accum += c.leadInSec + c.effectiveDuration();
        if (accum > maxEnd) maxEnd = accum;
    }
    return maxEnd;
}

void Timeline::onTrackClipClicked(int index)
{
    m_audioTrack->setSelectedClip(index);
    m_activeVideoTrackIndex = index < 0 ? -1 : 0;
    emit clipSelected(index);
    // V3 sprint — track-aware overload. m_videoTrack is m_videoTracks[0]
    // (V1) by construction, so this legacy single-track entry point always
    // refers to V1.
    emit clipSelectedOnTrack(index < 0 ? -1 : 0, index);
}

bool Timeline::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto *track = qobject_cast<TimelineTrack *>(watched);
        auto *me = static_cast<QMouseEvent *>(event);
        if (track && me->button() == Qt::LeftButton) {
            const int clipIndex = track->clipAtX(me->position().toPoint().x());
            const QVector<ClipInfo> &clips = track->clips();
            if (clipIndex >= 0 && clipIndex < clips.size()
                && clips[clipIndex].isSequenceReference()) {
                const QString sequenceId = resolveSequenceRefId(clips[clipIndex]);
                if (!sequenceId.isEmpty() && sequenceById(sequenceId)) {
                    QStringList path = breadcrumbPathForTimeline(this);
                    QString active = m_activeSequenceId;
                    if (active.isEmpty())
                        active = QString::fromLatin1(kDefaultSequenceId);
                    if (path.isEmpty())
                        path.append(active);
                    const int existing = path.indexOf(sequenceId);
                    if (existing >= 0)
                        path = path.mid(0, existing + 1);
                    else
                        path.append(sequenceId);
                    setBreadcrumbPathForTimeline(this, path);
                    if (setActiveSequence(sequenceId)) {
                        rebuildTimelineBreadcrumbBar(this);
                        emit statusMessageRequested(
                            QStringLiteral("Opened nested sequence: %1")
                                .arg(sequenceNameForBreadcrumb(this, sequenceId)),
                            3000);
                        return true;
                    }
                }
                emit statusMessageRequested(
                    QStringLiteral("Nested sequence is missing."),
                    3000);
                return true;
            }
        }
    }

    if (qobject_cast<TimelineTrack *>(watched)
        && event->type() == QEvent::MouseButtonPress) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::RightButton && m_videoTrack) {
            int contentX = me->position().toPoint().x();
            if (watched == m_scrollArea->viewport()) {
                if (QScrollBar *hbar = m_scrollArea->horizontalScrollBar())
                    contentX += hbar->value();
            } else if (auto *w = qobject_cast<QWidget *>(watched)) {
                contentX = m_tracksWidget
                    ? m_tracksWidget->mapFromGlobal(
                          w->mapToGlobal(me->position().toPoint())).x()
                    : contentX;
            }

            const int targetIndex = (m_activeVideoTrackIndex >= 0)
                ? m_activeVideoTrackIndex
                : 0;
            TimelineTrack *targetTrack = m_videoTracks.value(targetIndex, m_videoTrack);
            showGapContextMenu(targetTrack,
                               qMax(0.0, m_videoTrack->xToSeconds(qMax(0, contentX))),
                               me->globalPosition().toPoint());
            return true;
        }
        if (me->button() == Qt::LeftButton
            && !(me->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
            clearAllSelections();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Timeline::wireTrackSelection(TimelineTrack *track)
{
    if (track)
        track->installEventFilter(this);
    // US-1214: anchors live only on V1, so only its splits route connections;
    // the other tracks' splits trace a connected right piece back to its origin.
    connect(track, &TimelineTrack::clipSplit, this,
            [this, track](const QString &leftUid, const QString &rightUid, double splitLocalSec) {
        if (track == m_videoTracks.value(0, nullptr))
            m_pendingSplits.append({leftUid, rightUid, splitLocalSec});
        else
            m_pendingClipSplits[track].append({leftUid, rightUid, splitLocalSec});
    });

    // The viewport owns the overlay; global points keep the drag continuous across rows.
    auto *marquee = new QRubberBand(QRubberBand::Rectangle, m_scrollArea->viewport());
    marquee->setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(track, &QObject::destroyed, marquee, &QObject::deleteLater);
    connect(track, &TimelineTrack::marqueeDragged, this,
            [this, track, marquee](QPoint start, QPoint end, bool additive, bool finished) {
        const QRect rect(m_scrollArea->viewport()->mapFromGlobal(start),
                         m_scrollArea->viewport()->mapFromGlobal(end));
        marquee->setGeometry(rect.normalized());
        if (!finished) {
            marquee->show();
            marquee->raise();
            return;
        }
        marquee->hide();
        int first = -1, last = -1;
        const auto tracks = timelineTracks(this);
        const int top = qMin(start.y(), end.y()), bottom = qMax(start.y(), end.y());
        for (int row = 0; row < tracks.size(); ++row) {
            const int y = tracks[row]->mapToGlobal(QPoint(0, 0)).y();
            if (y <= bottom && y + tracks[row]->height() > top) {
                if (first < 0) first = row;
                last = row;
            }
        }
        if (first >= 0)
            selectClipsInRange(track->xToSeconds(track->mapFromGlobal(start).x()),
                               track->xToSeconds(track->mapFromGlobal(end).x()),
                               first, last, additive);
    });

    connect(track, &TimelineTrack::selectionChanged, this, [this, track](int index, bool additive) {
        if (m_inLinkedSelectionSync) return;
        m_inLinkedSelectionSync = true;

        // Non-additive click clears every OTHER track first so at most one
        // clip (or one link group) is selected across the whole timeline.
        if (!additive && index >= 0) {
            auto clearOther = [track](TimelineTrack *t) {
                if (!t || t == track || t->selectedClip() < 0) return;
                t->setSelectedClip(-1);
            };
            for (auto *t : m_videoTracks) clearOther(t);
            for (auto *t : m_audioTracks) clearOther(t);
        }

        // Propagate selection to every clip that shares a non-zero linkGroup
        // so the linked V/A pair (and anything else the user has grouped)
        // moves as a unit.
        if (index >= 0 && index < track->clips().size()) {
            const int linkGroup = track->clips()[index].linkGroup;
            if (linkGroup > 0 && m_linkedSelectionEnabled) {
                auto syncLinked = [linkGroup, additive](TimelineTrack *t) {
                    if (!t) return;
                    const auto &clips = t->clips();
                    for (int i = 0; i < clips.size(); ++i) {
                        if (clips[i].linkGroup == linkGroup && !t->isClipSelected(i)) {
                            if (additive) t->toggleClipSelection(i);
                            else t->setSelectedClip(i);
                        }
                    }
                };
                for (auto *t : m_videoTracks) if (t != track) syncLinked(t);
                for (auto *t : m_audioTracks) if (t != track) syncLinked(t);
            }
        }

        m_inLinkedSelectionSync = false;
        emit clipSelected(index);
        // V3 sprint — track-aware overload. Resolve which video-track row
        // the click landed on; audio-track clicks emit trackIdx=-1 so the
        // edit target falls back to follow-active.
        m_activeAudioTrackIndex = index >= 0 ? m_audioTracks.indexOf(track) : -1;
        int videoTrackIdx = m_videoTracks.indexOf(track);
        m_activeVideoTrackIndex = (index >= 0 && videoTrackIdx >= 0) ? videoTrackIdx : -1;
        emit clipSelectedOnTrack(index < 0 ? -1 : videoTrackIdx, index);
    });
    connect(track, &TimelineTrack::selectionShifted, this, [this, track](int primary) {
        // The selected clip moved; keep the cached (track, clip) of the listeners in step.
        const int videoTrackIdx = m_videoTracks.indexOf(track);
        if (videoTrackIdx < 0 || videoTrackIdx != m_activeVideoTrackIndex) return;
        emit clipSelected(primary);
        emit clipSelectedOnTrack(videoTrackIdx, primary);
    });
    connect(track, &TimelineTrack::emptyAreaClicked, this, [this]() {
        clearAllSelections();
    });
    connect(track, &TimelineTrack::modified, this, &Timeline::onTrackModified);
    connect(track, &TimelineTrack::interactionCompleted, this,
        [this](const QString &description) {
            // Discrete user interaction finished — push an undo entry so
            // Ctrl+Z reverts THIS interaction. Without this, drag/trim/
            // transition-handle/envelope edits leave no undo record and a
            // later Ctrl+Z (e.g. from "Add transition") jumps past them
            // back to the prior explicit-action snapshot, which looks like
            // unrelated clips disappearing.
            saveUndoState(description);
        });
    connect(track, &TimelineTrack::clipContextMenuRequested, this,
        [this, track](int clipIndex, const QPoint &globalPos) {
            showClipContextMenu(track, clipIndex, globalPos);
        });
    connect(track, &TimelineTrack::gapContextMenuRequested, this,
        [this, track](double timeSec, const QPoint &globalPos) {
            showGapContextMenu(track, timeSec, globalPos);
        });
    connect(track, &TimelineTrack::linkedDragStarted, this,
        [this, track](int clipIdx) {
            captureLinkedDragPartners(track, clipIdx);
        });
    connect(track, &TimelineTrack::linkedDragDelta, this,
        [this](int, double rawDeltaSec) {
            applyLinkedDragDelta(rawDeltaSec);
        });
    connect(track, &TimelineTrack::linkedDragCancelled, this, [this]() {
        revertLinkedDragPartners();
        clearLinkedDragState();
    });
    connect(track, &TimelineTrack::linkedTrimStarted, this,
        [this, track](int clipIdx, bool leftEdge) {
            captureLinkedTrimPartners(track, clipIdx, leftEdge);
        });
    connect(track, &TimelineTrack::linkedTrimDelta, this,
        [this](int, bool, double deltaSec) {
            applyLinkedTrimDelta(deltaSec);
        });
    connect(track, &TimelineTrack::linkedTrimFinished, this, [this]() {
        m_linkedTrimPartners.clear();
    });
    connect(track, &TimelineTrack::linkedTrimCancelled, this, [this]() {
        applyLinkedTrimDelta(0.0);
        m_linkedTrimPartners.clear();
    });
    connect(track, &TimelineTrack::clipMovedAcrossTracks, this,
        [this, track](TimelineTrack *src, int srcIdx, int destIdx) {
            remapCarriersAfterCrossTrackMove(m_videoTracks.indexOf(src), srcIdx,
                                             m_videoTracks.indexOf(track), destIdx);
        });
    connect(track, &TimelineTrack::crossTrackDropped, this,
        [this, track](int insertIdx, int linkGroup, double dropTime, TimelineTrack *src) {
            handleCrossTrackLinkedDrop(track, linkGroup, dropTime, insertIdx, src);
        });
    connect(track, &TimelineTrack::sameGroupCrossTrackDropped, this,
        [this, track](int insertIdx, int linkGroup, double dropTime, TimelineTrack *src,
                      const TrackClipSnapshot &before, const QVector<TimelineTrack::DetachedClip> &detached) {
            handleCrossTrackLinkedDrop(track, linkGroup, dropTime, insertIdx, src, &before, &detached);
        });
    connect(track, &TimelineTrack::effectDropped, this,
        [this, track](const QString &entryId, int clipIndex) {
            const int trackIdx = m_videoTracks.indexOf(track);
            if (trackIdx >= 0)
                emit effectDropped(trackIdx, clipIndex, entryId);
        });
    // RM-5: remap Timeline's carrier when clips are reordered within a track.
    // clipMoved supplies (fromIndex, toIndex) after the mutation. Because
    // moveClip is a pure permutation we can derive the exact old→new index map
    // by direct arithmetic — no pre-mutation snapshot is needed:
    //   clip at `from` → `to`
    //   clips in [from+1..to] (if from<to) shift to [from..to-1]  (down by 1)
    //   clips in [to..from-1] (if from>to) shift to [to+1..from]  (up by 1)
    connect(track, &TimelineTrack::clipMoved, this,
        [this, track](int fromIdx, int toIdx) {
            const int t = m_videoTracks.indexOf(track);
            if (t < 0) return;   // audio track — no video clip sidecars
            // Build old→new index map for track t only.
            QHash<QString, QString> oldToNew;
            oldToNew.insert(trackMatteClipKey(t, fromIdx),
                            trackMatteClipKey(t, toIdx));
            if (fromIdx < toIdx) {
                for (int i = fromIdx + 1; i <= toIdx; ++i)
                    oldToNew.insert(trackMatteClipKey(t, i),
                                    trackMatteClipKey(t, i - 1));
            } else {
                for (int i = toIdx; i < fromIdx; ++i)
                    oldToNew.insert(trackMatteClipKey(t, i),
                                    trackMatteClipKey(t, i + 1));
            }
            notifyClipKeysRemapped({oldToNew, {}});
            if (trackMatteEntries().isEmpty() && m_clipParentEntries.isEmpty()
                && rotoEntries().isEmpty()) return;
            const auto currentMattes = trackMatteEntries();
            if (!currentMattes.isEmpty()) {
                QHash<QString, TimelineTrackMatteEntry> rebuilt;
                rebuilt.reserve(currentMattes.size());
                for (auto it = currentMattes.cbegin();
                     it != currentMattes.cend(); ++it) {
                    const QString newKey = oldToNew.value(it.key(), it.key());
                    TimelineTrackMatteEntry e = it.value();
                    e.matteSourceClipId = oldToNew.value(
                        e.matteSourceClipId, e.matteSourceClipId);
                    rebuilt.insert(newKey, e);
                }
                commitTrackMatteEntries(std::move(rebuilt));
            }
            const auto rotoBefore = rotoEntries();
            if (!rotoBefore.isEmpty()) {
                QHash<QString, RotoClipEntry> rebuilt;
                rebuilt.reserve(rotoBefore.size());
                for (auto it = rotoBefore.cbegin(); it != rotoBefore.cend(); ++it)
                    rebuilt.insert(oldToNew.value(it.key(), it.key()), it.value());
                commitRotoEntries(std::move(rebuilt), true);
            }
            if (!m_clipParentEntries.isEmpty()) {
                QHash<QString, QString> rebuilt;
                rebuilt.reserve(m_clipParentEntries.size());
                for (auto it = m_clipParentEntries.cbegin();
                     it != m_clipParentEntries.cend(); ++it) {
                    const QString newChild = oldToNew.value(it.key(), it.key());
                    const QString newParent = oldToNew.value(it.value(), it.value());
                    if (newChild != newParent)
                        rebuilt.insert(newChild, newParent);
                }
                m_clipParentEntries = rebuilt;
            }
        });
}

void Timeline::captureLinkedDragPartners(TimelineTrack *source, int clipIdx)
{
    clearLinkedDragState();
    m_linkedDragSourceIsV1 = source && source == m_videoTracks.value(0, nullptr);
    if (!source || clipIdx < 0 || clipIdx >= source->clips().size()) return;
    const int linkGroup = source->clips()[clipIdx].linkGroup;
    if (linkGroup <= 0) return;
    // Include the source clip in the partners list so Timeline is the
    // single authoritative applier — simpler than splitting the math
    // between TimelineTrack and Timeline.
    auto capture = [&](TimelineTrack *t) {
        if (!t) return;
        const auto &clips = t->clips();
        for (int i = 0; i < clips.size(); ++i) {
            if (clips[i].linkGroup == linkGroup) {
                LinkedDragPartner p;
                p.track = t;
                p.clipIdx = i;
                p.origLeadIn = clips[i].leadInSec;
                p.origLeadInNext = (i + 1 < clips.size())
                    ? clips[i + 1].leadInSec
                    : -1.0;
                m_linkedDragPartners.append(p);
            }
        }
    };
    for (auto *t : m_videoTracks) capture(t);
    for (auto *t : m_audioTracks) capture(t);
}

void Timeline::applyLinkedDragDelta(double rawDeltaSec)
{
    if (m_linkedDragPartners.isEmpty()) return;
    // Global clamp: the tightest allowed delta across every partner, so
    // no partner crosses its own neighbor even when others have more room.
    double minDelta = -1e18;
    double maxDelta = 1e18;
    // US-1203: in storyline mode a drag that did not start on V1 keeps its V1
    // partner in place (V1 changes only through the reorder on release).
    TimelineTrack *pinnedV1 = (m_storylineMode && !m_linkedDragSourceIsV1)
        ? m_videoTracks.value(0, nullptr) : nullptr;
    for (const auto &p : m_linkedDragPartners) {
        const bool pinned = pinnedV1 && p.track == pinnedV1;
        const double lower = pinned ? 0.0 : -p.origLeadIn;
        const double upper = pinned ? 0.0 : (p.origLeadInNext >= 0.0) ? p.origLeadInNext : 1e18;
        if (lower > minDelta) minDelta = lower;
        if (upper < maxDelta) maxDelta = upper;
    }
    double clamped = rawDeltaSec;
    if (clamped < minDelta) clamped = minDelta;
    if (clamped > maxDelta) clamped = maxDelta;

    for (const auto &p : m_linkedDragPartners) {
        if (!p.track) continue;
        const double newLeadIn = p.origLeadIn + clamped;
        const double newNextLeadIn = (p.origLeadInNext >= 0.0)
            ? p.origLeadInNext - clamped
            : -1.0;
        p.track->applyDragMove(p.clipIdx, newLeadIn, newNextLeadIn);
    }
}

void Timeline::captureLinkedTrimPartners(TimelineTrack *source, int clipIdx, bool leftEdge)
{
    m_linkedTrimPartners.clear();
    m_linkedTrimLeftEdge = leftEdge;
    m_linkedTrimStoryline = false;
    if (!source || clipIdx < 0 || clipIdx >= source->clips().size()) return;
    const int linkGroup = source->clips()[clipIdx].linkGroup;
    if (linkGroup <= 0) return;
    bool lockedPartner = false;
    auto capture = [&](TimelineTrack *t) {
        if (!t) return;
        const auto &clips = t->clips();
        for (int i = 0; i < clips.size(); ++i) {
            if (clips[i].linkGroup != linkGroup) continue;
            if (t->isLocked()) lockedPartner = true;
            LinkedTrimPartner p;
            p.track = t;
            p.clipIdx = i;
            p.speed = qMax(0.001, clips[i].speed);
            p.origIn = clips[i].inPoint;
            p.origOut = clips[i].outPoint > 0 ? clips[i].outPoint : clips[i].duration;
            p.origLeadIn = clips[i].leadInSec;
            p.origLeadInNext = (i + 1 < clips.size()) ? clips[i + 1].leadInSec : -1.0;
            m_linkedTrimPartners.append(p);
        }
    };
    for (auto *t : m_videoTracks) capture(t);
    for (auto *t : m_audioTracks) capture(t);
    // Same convention as the other linked ops (duplicateSelectedClips): a
    // locked partner makes the entire linked operation ineligible.
    if (lockedPartner) {
        m_linkedTrimPartners.clear();
        source->rejectLinkedTrim();
        return;
    }
    if (m_linkedTrimPartners.isEmpty()) return;

    // US-1305: in storyline mode a group with a V1 member trims with a fixed
    // start on every member, and V1 ripples (the V1 clips after it follow the
    // length change). The other members compensate their next clip so it stays,
    // except linked audio of a later V1 clip, which ripples with that clip.
    TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    int v1ClipIdx = -1;
    for (const auto &p : m_linkedTrimPartners)
        if (m_storylineMode && p.track == v1 && v1ClipIdx < 0) v1ClipIdx = p.clipIdx;
    m_linkedTrimStoryline = v1ClipIdx >= 0;
    if (m_linkedTrimStoryline) {
        QSet<int> laterV1Groups;
        for (int v = v1ClipIdx + 1; v < v1->clipCount(); ++v)
            if (v1->clips().at(v).linkGroup > 0) laterV1Groups.insert(v1->clips().at(v).linkGroup);
        for (auto &p : m_linkedTrimPartners) {
            if (p.track == v1 || !p.track->isAudioTrack() || p.clipIdx + 1 >= p.track->clipCount())
                continue;
            const ClipInfo &next = p.track->clips().at(p.clipIdx + 1);
            p.nextFollowsV1 = next.anchorUid.isEmpty() && laterV1Groups.contains(next.linkGroup);
            if (!p.nextFollowsV1) continue;
            // The followers ripple with V1; the first non-follower after them
            // (BGM) keeps its start, as in applyTrimLinked.
            for (int h = p.clipIdx + 2; h < p.track->clipCount(); ++h) {
                const ClipInfo &c = p.track->clips().at(h);
                if (c.anchorUid.isEmpty() && laterV1Groups.contains(c.linkGroup)) continue;
                p.holdIdx = h;
                p.holdOrigLeadIn = c.leadInSec;
                break;
            }
        }
    }

    // Intersect every member's allowed TIMELINE delta. Source limits
    // (inPoint >= 0, outPoint <= duration, 0.1 s minimum length, same as the
    // single-clip trim branch) are converted with /speed per member; the
    // neighbour gaps (own leadInSec / next clip's leadInSec) are already
    // timeline seconds, so neighbours are never pushed or overlapped (US-1305:
    // in storyline mode V1 and a next clip that follows V1 are not limits).
    double lo = -1e18;
    double hi = 1e18;
    for (const auto &p : m_linkedTrimPartners) {
        const ClipInfo &c = p.track->clips()[p.clipIdx];
        const bool nextLimits = p.origLeadInNext >= 0.0 && !p.nextFollowsV1
            && !(m_linkedTrimStoryline && p.track == v1);
        if (leftEdge) {
            lo = qMax(lo, -p.origIn / p.speed);
            if (!m_linkedTrimStoryline)
                lo = qMax(lo, -p.origLeadIn);
            else if (nextLimits)
                lo = qMax(lo, -p.origLeadInNext);
            hi = qMin(hi, (p.origOut - 0.1 - p.origIn) / p.speed);
        } else {
            hi = qMin(hi, (c.duration - p.origOut) / p.speed);
            lo = qMax(lo, (p.origIn + 0.1 - p.origOut) / p.speed);
            if (nextLimits)
                hi = qMin(hi, p.origLeadInNext);
        }
        // The followers must not push the held clip (G17(d): refused).
        if (p.holdIdx >= 0) {
            if (leftEdge) lo = qMax(lo, -p.holdOrigLeadIn);
            else hi = qMin(hi, p.holdOrigLeadIn);
        }
    }
    // A member already past a limit must not force movement: 0 stays legal.
    lo = qMin(lo, 0.0);
    hi = qMax(hi, 0.0);
    source->setLinkedTrimBounds(lo, hi);
}

void Timeline::applyLinkedTrimDelta(double deltaSec)
{
    TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    for (const auto &p : m_linkedTrimPartners) {
        if (!p.track) continue;
        // US-1305: the next clip keeps its position unless the trim ripples it
        // (V1, or linked audio of a later V1 clip).
        const bool compensateNext = p.origLeadInNext >= 0.0 && !p.nextFollowsV1
            && !(m_linkedTrimStoryline && p.track == v1);
        if (m_linkedTrimLeftEdge && m_linkedTrimStoryline) {
            p.track->applyLinkedTrim(p.clipIdx, true, p.origIn + deltaSec * p.speed, p.origLeadIn,
                                     compensateNext ? p.origLeadInNext + deltaSec : -1.0);
        } else if (m_linkedTrimLeftEdge) {
            p.track->applyLinkedTrim(p.clipIdx, true,
                p.origIn + deltaSec * p.speed, p.origLeadIn + deltaSec);
        } else {
            p.track->applyLinkedTrim(p.clipIdx, false,
                p.origOut + deltaSec * p.speed,
                compensateNext ? p.origLeadInNext - deltaSec : -1.0);
        }
        if (p.holdIdx >= 0)
            p.track->applyDragMove(p.holdIdx,
                m_linkedTrimLeftEdge ? p.holdOrigLeadIn + deltaSec : p.holdOrigLeadIn - deltaSec, -1.0);
    }
}

void Timeline::revertLinkedDragPartners()
{
    for (const auto &p : m_linkedDragPartners) {
        if (!p.track) continue;
        p.track->applyDragMove(p.clipIdx, p.origLeadIn, p.origLeadInNext);
    }
}

void Timeline::clearLinkedDragState()
{
    m_linkedDragPartners.clear();
}

int Timeline::sameKindLinkTarget(const QVector<TimelineTrack *> &tracks, int memberTrack, int destIdx,
                                 int dragSourceIdx, bool intoStoryline)
{
    if (dragSourceIdx < 0 || intoStoryline)
        return memberTrack;
    const int target = qMax(0, memberTrack + destIdx - dragSourceIdx);
    // A locked track neither gives up nor takes a member.
    if (tracks[memberTrack]->isLocked()
        || (target < tracks.size() && tracks[target]->isLocked()))
        return memberTrack;
    return target;
}

void Timeline::handleCrossTrackLinkedDrop(TimelineTrack *destTrack, int linkGroup, double dropTime,
                                          int droppedIdx, TimelineTrack *dragSource,
                                          const TrackClipSnapshot *sameGroupBefore,
                                          const QVector<TimelineTrack::DetachedClip> *detached)
{
    if (linkGroup <= 0 || !destTrack) return;
    // US-1203: a storyline insert on V1. The dropped clip's audio is taken out
    // first so it cannot block the V1 linked audio after the insert point, which
    // then moves out of the way; the dropped clip's audio lands at its start on
    // A1, or on the next free audio track on a collision. US-1313: every audio
    // member of the group, in track order (a locked one keeps them all).
    if (m_storylineMode && destTrack == m_videoTracks.value(0, nullptr)) {
        struct AudioMember {
            TimelineTrack *track;
            int index;
            ClipInfo clip;
            double originalSec;
            bool selected;
        };
        QVector<AudioMember> members;
        bool lockedMember = false;
        for (TimelineTrack *track : m_audioTracks) {
            if (!track) continue;
            const auto starts = clipStartSecs(track->clips());
            for (int ci = 0; ci < track->clipCount(); ++ci) {
                if (track->clips().at(ci).linkGroup != linkGroup) continue;
                lockedMember = lockedMember || track->isLocked();
                members.append({track, ci, track->clips().at(ci), starts[ci], track->isClipSelected(ci)});
            }
        }
        if (members.isEmpty() || lockedMember) {
            followStorylineLinks();
            return;
        }
        for (int m = members.size() - 1; m >= 0; --m)
            members[m].track->removeClipPreservingDownstream(members[m].index, false);
        // A member still owns its uid on its track while the followers move and
        // until it is placed, so the fallbacks below stay uid-safe.
        QHash<int, QHash<QString, int>> reservedUids;
        for (const AudioMember &m : members)
            if (!m.clip.uid.isEmpty()) ++reservedUids[m_audioTracks.indexOf(m.track)][m.clip.uid];
        followStorylineLinks(reservedUids);
        TrackSelectionUids selection = selectedUidsByTrack(m_audioTracks);
        for (const AudioMember &m : members) {
            if (!m.clip.uid.isEmpty()) {
                QHash<QString, int> &own = reservedUids[m_audioTracks.indexOf(m.track)];
                if (--own[m.clip.uid] <= 0) own.remove(m.clip.uid);
            }
            TimelineTrack::DropPlan plan;
            TimelineTrack *destination = storylineAudioSlot(0, m.clip, dropTime, &plan, &reservedUids);
            if (!destination) {
                // No free audio track (no track is added): back where it was, else
                // at the end of its track.
                destination = m.track;
                plan = m.track->planDrop(m.originalSec, m.clip.effectiveDuration());
                if (!plan.valid) {
                    double endSec = 0.0;
                    for (const ClipInfo &c : m.track->clips())
                        endSec += c.leadInSec + c.effectiveDuration();
                    plan = m.track->planDrop(endSec, m.clip.effectiveDuration());
                }
            }
            destination->insertClipPreservingDownstream(plan.insertIdx, m.clip, plan.newLeadIn);
            if (m.selected)
                selection[destination].append(m.clip.uid);
        }
        reselectByUid(selection);
        return;
    }
    // Determine if dest is a video or audio track, and its index
    const int destVideoIdx = m_videoTracks.indexOf(destTrack);
    const bool destIsVideo = destVideoIdx >= 0;
    const int destIdx = destIsVideo ? destVideoIdx : m_audioTracks.indexOf(destTrack);
    if (destIdx < 0) return;
    const auto &sameTracks = destIsVideo ? m_videoTracks : m_audioTracks;
    const int dragSourceIdx = dragSource ? sameTracks.indexOf(dragSource) : -1;
    TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    // US-1203: audio dropped onto A1 brings its picture into V1 as a storyline
    // insert; the audio members are then placed by the V1 branch above.
    const bool intoStoryline = m_storylineMode && !destIsVideo && destIdx == 0;

    // US-1313 mapping (a two-clip link moves as before): the first member of the
    // other track type goes to the track with dest's index (created if needed),
    // later ones keep their track offset from it; a member of dest's type keeps
    // its offset from the drag source (stays when the source is unknown).
    struct Member {
        bool video;
        int track;
        int index;
        int target;
        ClipInfo clip;
        bool detached = false;   // US-1403: already taken off destTrack
    };
    QVector<Member> moving;
    int anchorTrack = -1;
    bool anchorSeen = false;
    auto collect = [&](bool video) {
        const auto &tracks = video ? m_videoTracks : m_audioTracks;
        const bool sameType = video == destIsVideo;
        for (int t = 0; t < tracks.size(); ++t) {
            for (int ci = 0; ci < tracks[t]->clipCount(); ++ci) {
                if (tracks[t]->clips().at(ci).linkGroup != linkGroup) continue;
                if (tracks[t] == destTrack && ci == droppedIdx) continue;
                int target = t;
                if (!sameType) {
                    const bool anchor = !anchorSeen;
                    if (anchor) {
                        anchorSeen = true;
                        anchorTrack = t;
                    }
                    target = destIdx + (t - anchorTrack);
                    // The anchor moves as before; a later member neither leaves
                    // nor lands on a locked track.
                    if (!anchor
                        && (tracks[t]->isLocked()
                            || (target < tracks.size() && tracks[target]->isLocked())))
                        target = t;
                } else {
                    target = sameKindLinkTarget(tracks, t, destIdx, dragSourceIdx, intoStoryline);
                }
                // A member already on its target track stays where it is.
                if (target != t)
                    moving.append({video, t, ci, target, tracks[t]->clips().at(ci)});
            }
        }
    };
    collect(!destIsVideo);
    collect(destIsVideo);
    // US-1403: the members acceptClipDrop took off destTrack move by the same
    // rule (it checked that their target is another track).
    if (detached) {
        for (const TimelineTrack::DetachedClip &d : *detached)
            moving.append({destIsVideo, destIdx, d.index,
                           sameKindLinkTarget(sameTracks, destIdx, destIdx, dragSourceIdx, intoStoryline),
                           d.clip, true});
    }
    if (moving.isEmpty()) return;

    for (const Member &m : moving) {
        auto &targetTracks = m.video ? m_videoTracks : m_audioTracks;
        while (targetTracks.size() <= m.target) {
            if (m.video)
                addVideoTrack(false);
            else
                addAudioTrack(false);
        }
    }
    // RM-5 / US-1313: snapshot (after the appended tracks, which move no key)
    // before any member leaves its track; the carriers follow every moved
    // member in one by-uid remap at the end.
    // US-1403: a same-group drop brings its snapshot from before the dropped
    // clip and the detached members moved.
    const TrackClipSnapshot before = sameGroupBefore ? *sameGroupBefore : snapshotTrackClips(this);
    // All members leave first (highest index of a track first), so no member
    // blocks the slot another one is moving to.
    bool leftV1 = false;
    for (int m = moving.size() - 1; m >= 0; --m) {
        if (moving[m].detached) continue;   // US-1403: already off destTrack
        TimelineTrack *src = (moving[m].video ? m_videoTracks : m_audioTracks)[moving[m].track];
        src->removeClipPreservingDownstream(moving[m].index);
        leftV1 = leftV1 || src == v1;
    }
    bool storylineInserted = false;
    double landedSec = 0.0;
    // Each member's index on its target track, kept exact as later members land
    // on the same track before it (uid twins rule out a lookup by uid).
    QVector<int> placedIdx(moving.size(), -1);
    // US-1403: the dropped clip's index on destTrack, kept exact the same way.
    int droppedNow = droppedIdx;
    auto recordPlaced = [&](int m, int index) {
        for (int e = 0; e < m; ++e)
            if (moving[e].video == moving[m].video && moving[e].target == moving[m].target
                && index <= placedIdx[e])
                ++placedIdx[e];
        if (moving[m].video == destIsVideo && moving[m].target == destIdx && index <= droppedNow)
            ++droppedNow;
        placedIdx[m] = index;
    };
    for (int mi = 0; mi < moving.size(); ++mi) {
        const Member &m = moving[mi];
        TimelineTrack *targetTrack = (m.video ? m_videoTracks : m_audioTracks)[m.target];
        if (intoStoryline && targetTrack == v1) {
            // The insert point snaps to a clip boundary, so the dropped audio is
            // then realigned to the picture's start.
            double sec = 0.0;
            recordPlaced(mi, storylineInsertClip(v1, dropTime, m.clip, &sec));
            if (!storylineInserted) landedSec = sec;
            storylineInserted = true;
            continue;
        }
        // Use planDrop on the target track to find the right insertion point.
        // Fall back to appending at end if the exact position can't fit.
        const double memberDur = m.clip.effectiveDuration();
        auto plan = targetTrack->planDrop(dropTime, memberDur);
        if (!plan.valid) {
            // Append at end of track as fallback
            plan.valid = true;
            plan.insertIdx = targetTrack->clipCount();
            // Subtract accumulated duration of existing clips
            double accum = 0.0;
            for (const auto &c : targetTrack->clips())
                accum += c.leadInSec + c.effectiveDuration();
            plan.newLeadIn = qMax(0.0, dropTime - accum);
        }
        targetTrack->insertClipPreservingDownstream(plan.insertIdx, m.clip, plan.newLeadIn);
        recordPlaced(mi, plan.insertIdx);
    }
    // US-1203: a picture taken out of V1 by its audio's drag closes V1
    // (indices do not change, so the remap below stays exact).
    if (m_storylineMode && leftV1)
        closeGapsOnTrack(v1);
    // The carriers follow the moved video members by uid, with their own old ->
    // new keys first (uid twins on several tracks cannot be told apart by uid).
    QHash<QString, QString> explicitMoves;
    bool videoMoved = false;
    for (int mi = 0; mi < moving.size(); ++mi) {
        const Member &m = moving[mi];
        if (!m.video) continue;
        videoMoved = true;
        // US-1403: `before` predates the dropped clip's move, so only the keys
        // known in its coordinates are named: the detached members here and the
        // dropped clip below; the other members follow by uid.
        if (sameGroupBefore && !m.detached) continue;
        explicitMoves.insert(trackMatteClipKey(m.track, m.index),
                             trackMatteClipKey(m.target, placedIdx[mi]));
    }
    const int dragSourceVideoIdx = m_videoTracks.indexOf(dragSource);
    if (sameGroupBefore && destIsVideo && dragSourceVideoIdx >= 0 && droppedNow >= 0
        && droppedNow < destTrack->clipCount()) {
        // Uids are unique per track: the dropped clip's row in `before`.
        const QString droppedUid = destTrack->clips().at(droppedNow).uid;
        const auto &srcRow = sameGroupBefore->value(dragSourceVideoIdx);
        for (int i = 0; !droppedUid.isEmpty() && i < srcRow.size(); ++i) {
            if (srcRow[i].uid != droppedUid) continue;
            explicitMoves.insert(trackMatteClipKey(dragSourceVideoIdx, i),
                                 trackMatteClipKey(destIdx, droppedNow));
            break;
        }
    }
    if (videoMoved) ++m_crossTrackMoveRemapCount;
    remapCarriersByUid(this, before, explicitMoves);
    if (storylineInserted)
        handleCrossTrackLinkedDrop(v1, linkGroup, landedSec);
}

void Timeline::remapCarriersAfterCrossTrackMove(int srcTrackIdx, int srcIdx,
                                                int destTrackIdx, int destIdx)
{
    ++m_crossTrackMoveRemapCount;
    if (srcTrackIdx < 0 && destTrackIdx < 0) return;   // audio -> audio: no keys
    // Called after the move: the source track lost one clip, the destination
    // gained one. An empty value drops the key (a video clip moved to audio).
    QHash<QString, QString> oldToNew;
    if (srcTrackIdx >= 0) {
        const int oldCount = m_videoTracks[srcTrackIdx]->clipCount() + 1;
        for (int i = srcIdx + 1; i < oldCount; ++i)
            oldToNew.insert(trackMatteClipKey(srcTrackIdx, i),
                            trackMatteClipKey(srcTrackIdx, i - 1));
        oldToNew.insert(trackMatteClipKey(srcTrackIdx, srcIdx),
                        destTrackIdx >= 0 ? trackMatteClipKey(destTrackIdx, destIdx)
                                          : QString());
    }
    if (destTrackIdx >= 0) {
        const int oldCount = m_videoTracks[destTrackIdx]->clipCount() - 1;
        for (int i = destIdx; i < oldCount; ++i)
            oldToNew.insert(trackMatteClipKey(destTrackIdx, i),
                            trackMatteClipKey(destTrackIdx, i + 1));
    }
    const auto rotoBefore = rotoEntries();
    if (!rotoBefore.isEmpty()) {
        QHash<QString, RotoClipEntry> rebuilt;
        rebuilt.reserve(rotoBefore.size());
        for (auto it = rotoBefore.cbegin(); it != rotoBefore.cend(); ++it) {
            const QString newKey = oldToNew.value(it.key(), it.key());
            if (!newKey.isEmpty()) rebuilt.insert(newKey, it.value());
        }
        commitRotoEntries(std::move(rebuilt), true);
    }
    // The matte key and its matteSourceClipId take the same map.
    const auto mattesBefore = trackMatteEntries();
    if (!mattesBefore.isEmpty()) {
        QHash<QString, TimelineTrackMatteEntry> rebuilt;
        rebuilt.reserve(mattesBefore.size());
        for (auto it = mattesBefore.cbegin(); it != mattesBefore.cend(); ++it) {
            const QString newKey = oldToNew.value(it.key(), it.key());
            TimelineTrackMatteEntry e = it.value();
            const auto srcIt = oldToNew.constFind(e.matteSourceClipId);
            if (srcIt != oldToNew.cend()) {
                if (srcIt.value().isEmpty()) continue;   // source moved to audio
                e.matteSourceClipId = srcIt.value();
            }
            if (!newKey.isEmpty()) rebuilt.insert(newKey, e);
        }
        commitTrackMatteEntries(std::move(rebuilt));
    }
    if (!m_clipParentEntries.isEmpty()) {
        QHash<QString, QString> rebuilt;
        rebuilt.reserve(m_clipParentEntries.size());
        for (auto it = m_clipParentEntries.cbegin(); it != m_clipParentEntries.cend(); ++it) {
            const QString newChild = oldToNew.value(it.key(), it.key());
            const QString newParent = oldToNew.value(it.value(), it.value());
            if (!newChild.isEmpty() && !newParent.isEmpty() && newChild != newParent)
                rebuilt.insert(newChild, newParent);
        }
        m_clipParentEntries = rebuilt;
    }
    // US-1303: a key whose clip left the video tracks is dropped, not moved.
    ClipKeyRemap remap;
    for (auto it = oldToNew.cbegin(); it != oldToNew.cend(); ++it) {
        if (it.value().isEmpty()) remap.dropped.insert(it.key());
        else remap.oldToNew.insert(it.key(), it.value());
    }
    notifyClipKeysRemapped(std::move(remap));
}

void Timeline::clearAllSelections()
{
    bool changed = false;
    auto clearOne = [&changed](TimelineTrack *t) {
        if (!t || t->selectedClip() < 0) return;
        const bool was = t->blockSignals(true);
        t->setSelectedClip(-1);
        t->blockSignals(was);
        t->update();
        changed = true;
    };
    for (auto *t : m_videoTracks) clearOne(t);
    for (auto *t : m_audioTracks) clearOne(t);
    if (changed) {
        m_activeVideoTrackIndex = -1;
        emit clipSelected(-1);
        // V3 sprint — track-aware overload, deselect path.
        emit clipSelectedOnTrack(-1, -1);
    }
}

bool Timeline::selectClipByIndex(bool audio, int trackIndex, int clipIndex, QString *err)
{
    if (trackIndex < 0 || !trackAt(audio, trackIndex))
        return setTimelineEditError(err, QStringLiteral("track index is out of range"));
    TimelineTrack *track = trackAt(audio, trackIndex);
    if (clipIndex < 0 || clipIndex >= track->clipCount())
        return setTimelineEditError(err, QStringLiteral("clip index is out of range"));

    // GUI のクリックと同じく、先に全トラックを解除してから setSelectedClip
    // を通す。これで linked V/A の選択伝播と clipSelectedOnTrack の通知も共通化できる。
    clearAllSelections();
    track->setSelectedClip(clipIndex);
    return true;
}

void Timeline::clearSelection()
{
    clearAllSelections();
    m_activeVideoTrackIndex = -1;
}

void Timeline::onTrackModified()
{
    // Any clip add/remove/move/trim/split bubbles up here. Auto-fit the zoom
    // first so the timeline widget never exceeds the viewport width (otherwise
    // long-form sequences hit the kMaxWidth hard cap and tail clips get
    // visually clipped). Then recompute the flat playback schedule.
    ensureSequenceFitsViewport();
    scheduleEmitSequenceChanged();
    scheduleEmitSequenceChanged();
}

void Timeline::setTrackHeight(int h)
{
    h = qBound(30, h, 200);
    if (h == m_trackHeight) return;
    m_trackHeight = h;
    for (auto *t : m_videoTracks) if (t) t->setRowHeight(m_trackHeight);
    for (auto *t : m_audioTracks) if (t) t->setRowHeight(m_trackHeight);
    // Preserve a user-customized text row height — if the user has
    // dragged the T1 row's bottom edge, don't clobber that override when
    // the global V/A row height changes.
    if (m_textStrip && m_textStripCustomHeight <= 0)
        m_textStrip->setFixedHeight(m_trackHeight);
    if (m_headerLayout) {
        for (int i = 0; i < m_headerLayout->count(); ++i) {
            if (auto *item = m_headerLayout->itemAt(i)) {
                if (auto *hw = item->widget()) {
                    // Keep the V/A boundary row (drop-band spacer + 3 px
                    // separator) and the audio drop-band spacer at their
                    // fixed heights so they don't balloon with the headers.
                    if (hw == m_vaBoundaryHeader || hw == m_newAudioTrackDropZoneHeader)
                        continue;
                    if (hw == m_textStripHeader && m_textStripCustomHeight > 0)
                        continue;
                    // m_magnetArea sits above the V/A header rows and pairs
                    // with the right side's [time-ruler 22 + playhead-overlay
                    // 15] strip. Its height is locked to 37 px so V1's
                    // header stays vertically aligned with the V1 track —
                    // resizing it with m_trackHeight is what made the lock
                    // and mute icons drift away from their tracks.
                    if (hw == m_magnetArea)
                        continue;
                    hw->setFixedHeight(m_trackHeight);
                }
            }
        }
    }
    updateInfoLabel();
}

void Timeline::increaseTrackHeight() { setTrackHeight(m_trackHeight + 10); }
void Timeline::decreaseTrackHeight() { setTrackHeight(m_trackHeight - 10); }

void Timeline::ensureSequenceFitsViewport()
{
    if (!m_scrollArea || !m_scrollArea->viewport())
        return;
    const double total = totalDuration();
    if (total <= 0.0)
        return;
    const int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 100)
        return;

    // Leave a bit of right-edge margin so the last clip end is visible.
    const int safeW = qMax(100, viewportW - 60);

    // Required pps to make the entire sequence fit safeW horizontally.
    const double requiredPps = static_cast<double>(safeW) / total;
    const double newPps = qBound(0.02, requiredPps, 200.0);

    // Only auto-zoom OUT — never zoom in (that's the user's prerogative).
    if (newPps < m_zoomLevel - 1e-6) {
        qInfo() << "Timeline::ensureSequenceFitsViewport autoZoom"
                << m_zoomLevel << "->" << newPps
                << "totalSec=" << total << "viewportW=" << viewportW;
        setZoomLevel(newPps);
    }
}

void Timeline::applyOverlapTransitionsToIntervals(
    QVector<OverlapInterval> &trackIvs,
    const std::function<double(const OverlapInterval &)> &trailAvailable,
    const std::function<double(const OverlapInterval &)> &leadAvailable)
{
    for (int j = 1; j < trackIvs.size(); ++j) {
        OverlapInterval &a = trackIvs[j - 1];
        OverlapInterval &b = trackIvs[j];
        if (!isOverlapTransition(a.trailOutType)) continue;
        if (a.trailOutType != b.leadInType) continue;
        if (qAbs(a.timelineEnd - b.timelineStart) > 1e-3) continue;
        const double askedD = qMin(a.trailOutDuration, b.leadInDuration);
        if (askedD <= 0.0) continue;
        const double aSpeed = (a.speed > 0.0) ? a.speed : 1.0;
        const double bSpeed = (b.speed > 0.0) ? b.speed : 1.0;
        // A's trail handle = source frames past clipOut, divided by speed.
        // We grab the source duration from the owning ClipInfo since the
        // Interval struct does not carry it.
        const double aTrailAvailable = trailAvailable(a);
        const double bLeadAvailable = leadAvailable(b);

        double aExtend = 0.0, bRetract = 0.0;
        switch (a.trailOutAlignment) {
            case TransitionAlignment::Start:
                // Premiere "Start at Cut": entire transition AFTER cut.
                // Consumes A's trail handle only; B stays put.
                aExtend = qMin(askedD, aTrailAvailable);
                break;
            case TransitionAlignment::End:
                // Premiere "End at Cut": entire transition BEFORE cut.
                // Consumes B's lead handle only; A stays put.
                bRetract = qMin(askedD, bLeadAvailable);
                break;
            case TransitionAlignment::Center:
                // Premiere default: D/2 each side, with borrowing when
                // one side runs short so the user gets the requested
                // duration whenever physics allow.
                aExtend  = qMin(askedD * 0.5, aTrailAvailable);
                bRetract = qMin(askedD * 0.5, bLeadAvailable);
                if (aExtend + bRetract < askedD) {
                    if (bRetract < askedD * 0.5) {
                        const double slack = qMin(askedD - aExtend - bRetract,
                                                  aTrailAvailable - aExtend);
                        if (slack > 0.0) aExtend += slack;
                    }
                    if (aExtend + bRetract < askedD && aExtend < askedD * 0.5) {
                        const double slack = qMin(askedD - aExtend - bRetract,
                                                  bLeadAvailable - bRetract);
                        if (slack > 0.0) bRetract += slack;
                    }
                }
                break;
        }
        const double effectiveD = aExtend + bRetract;
        if (effectiveD < 0.01) continue;

        a.clipOut       += aExtend * aSpeed;
        a.timelineEnd   += aExtend;
        b.timelineStart -= bRetract;
        b.clipIn        -= bRetract * bSpeed;
        a.trailOutDuration = effectiveD;
        b.leadInDuration   = effectiveD;
    }
}

QVector<QVector<OverlapInterval>> Timeline::videoOverlapIntervals() const
{
    QVector<QVector<OverlapInterval>> intervals;
    computePlaybackSequenceImpl(&intervals);
    return intervals;
}

QVector<PlaybackEntry> Timeline::computePlaybackSequence() const
{
    return computePlaybackSequenceImpl(nullptr);
}

QVector<PlaybackEntry> Timeline::computePlaybackSequenceImpl(
    QVector<QVector<OverlapInterval>> *overlapIntervals) const
{
    QVector<PlaybackEntry> result;
    QVector<VideoReversedPlaybackBinding> reversedBindings;
    if (m_videoTracks.isEmpty()) {
        if (!overlapIntervals)
            setVideoReversedPlaybackBindings(reversedBindings);
        return result;
    }

    // PiP overlay resolution: every visible track contributes its full clip
    // intervals without mutual subtraction. The compositor stacks layers using
    // sourceTrack (V1 = front, higher = back). VideoPlayer's findActiveEntryAt
    // walks the sorted sequence in order, so the (timelineStart, sourceTrack
    // asc) sort below keeps V1 as the "primary" entry when multiple overlap.

    struct Interval : OverlapInterval {
        bool reversed = false;
        QString filePath;
        int trackIdx;
        // US-T35 per-clip video source transform + owning clip index for
        // round-trip updates when the user drags the preview transform.
        double videoScale = 1.0;
        double videoDx = 0.0;
        double videoDy = 0.0;
        double rotation2DDegrees = 0.0;
        double opacity = 1.0;
        bool isVfxFootage = false;
        BlendMode blendMode = BlendMode::Normal;
        double vfxIntensity = 1.0;
        int vfxBlackLevel = 16;
        bool fitContain = false;
        bool fitCover = false;
        clipcolor::ColorMeta colorMeta;
        LayerStyle layerStyle;
        double volume = 1.0;
        double pan = 0.0;
        trackfx::EqSettings clipEq;
        bool clipEqEnabled = false;
        QVector<AudioGainPoint> volumeEnvelope;
        TransitionAlignment leadInAlignment = TransitionAlignment::Center;
        QVector<StabilizerKeyframe> stabilizerKeyframes;
    };

    const QVector<TimelineSequence> sequenceSnapshot = sequences();
    auto findSequence = [&sequenceSnapshot](const QString &id) -> const TimelineSequence * {
        for (const TimelineSequence &sequence : sequenceSnapshot) {
            if (sequence.id == id)
                return &sequence;
        }
        return nullptr;
    };

    std::function<bool(const ClipInfo &, double, int, int, QVector<Interval> &,
                       int, QVector<QString> &)> appendSequenceIntervals;
    appendSequenceIntervals = [&](const ClipInfo &parentClip,
                                  double parentTimelineStart,
                                  int parentTrackIdx,
                                  int parentClipIdx,
                                  QVector<Interval> &out,
                                  int depth,
                                  QVector<QString> &sequenceStack) -> bool {
        const QString refId = resolveSequenceRefId(parentClip);
        if (refId.isEmpty())
            return false;
        if (depth > 8 || sequenceStack.contains(refId))
            return true;
        const TimelineSequence *sequence = findSequence(refId);
        if (!sequence)
            return true;

        const double parentSpeed = (parentClip.speed > 0.0) ? parentClip.speed : 1.0;
        const double sourceIn = qMax(0.0, parentClip.inPoint);
        const double sourceOut = (parentClip.outPoint > 0.0)
            ? parentClip.outPoint
            : qMax(parentClip.duration, sequence->duration());
        if (sourceOut <= sourceIn)
            return true;

        QVector<QString> reverseScanStack = sequenceStack;
        const bool reverseCompositionActive = parentClip.reversed
            || sequenceContainsReversedClip(
                sequenceSnapshot, refId, /*audio=*/false,
                reverseScanStack);
        sequenceStack.append(refId);
        for (const QVector<ClipInfo> &track : sequence->videoTracks) {
            double childAccum = 0.0;
            for (int childIdx = 0; childIdx < track.size(); ++childIdx) {
                const ClipInfo &child = track[childIdx];
                childAccum += qMax(0.0, child.leadInSec);
                const double childDur = child.effectiveDuration();
                if (childDur <= 0.0) {
                    continue;
                }

                const double overlapStart = qMax(sourceIn, childAccum);
                const double overlapEnd = qMin(sourceOut, childAccum + childDur);
                if (overlapEnd <= overlapStart) {
                    childAccum += childDur;
                    continue;
                }

                const double childSpeed = (child.speed > 0.0) ? child.speed : 1.0;
                const double localIn = overlapStart - childAccum;
                const double localOut = overlapEnd - childAccum;
                const NestedSequenceIntervalMapping parentInterval =
                    mapNestedSequenceInterval(
                        parentClip, parentTimelineStart, sourceIn,
                        overlapStart, overlapEnd, parentSpeed,
                        reverseCompositionActive);
                const double childTimelineStart = parentInterval.timelineStart;
                const double childTimelineEnd = parentInterval.timelineEnd;
                double childSourceIn = child.inPoint + localIn * childSpeed;
                double childSourceOut = child.inPoint + localOut * childSpeed;
                double combinedSpeed = childSpeed * parentSpeed;
                bool effectiveReversed = false;
                if (reverseCompositionActive) {
                    const double mappedAtLocalIn =
                        child.sourceSecondAtLocalTime(localIn);
                    const double mappedAtLocalOut =
                        child.sourceSecondAtLocalTime(localOut);
                    childSourceIn = qMin(mappedAtLocalIn, mappedAtLocalOut);
                    childSourceOut = qMax(mappedAtLocalIn, mappedAtLocalOut);
                    const bool childDescending =
                        mappedAtLocalOut + 1e-9 < mappedAtLocalIn;
                    effectiveReversed = parentInterval.runsBackward
                        != childDescending;
                    const double timelineSpan = qMax(
                        1e-9, childTimelineEnd - childTimelineStart);
                    combinedSpeed = qMax(
                        1e-9,
                        (childSourceOut - childSourceIn) / timelineSpan);
                }

                if (child.isSequenceReference()) {
                    ClipInfo nested = child;
                    nested.inPoint = childSourceIn;
                    nested.outPoint = childSourceOut;
                    nested.speed = combinedSpeed;
                    nested.reversed = effectiveReversed;
                    nested.opacity = child.opacity * parentClip.opacity;
                    nested.volume = child.volume * parentClip.volume;
                    nested.pan = qBound(-1.0, child.pan + parentClip.pan, 1.0);
                    // US-1110 clip EQ: child first, else the parent's (never both).
                    if (!child.clipEqEnabled) {
                        nested.clipEq = parentClip.clipEq;
                        nested.clipEqEnabled = parentClip.clipEqEnabled;
                    }
                    appendSequenceIntervals(nested, childTimelineStart,
                                            parentTrackIdx, parentClipIdx,
                                            out, depth + 1, sequenceStack);
                    childAccum += childDur;
                    continue;
                }

                Interval iv;
                iv.timelineStart = childTimelineStart;
                iv.timelineEnd = childTimelineEnd;
                iv.clipIn = childSourceIn;
                iv.clipOut = childSourceOut;
                iv.speed = combinedSpeed;
                iv.reversed = effectiveReversed;
                if (child.timeRemapCurve.keys.size() == 1) {
                    const double childSourceOut = (child.outPoint > 0.0) ? child.outPoint : child.duration;
                    const double holdSource =
                        qBound(child.inPoint,
                               child.inPoint + qMax(0.0, child.timeRemapCurve.keys.first().srcTime),
                               childSourceOut);
                    iv.clipIn = holdSource;
                    iv.speed = 1.0e-9;
                }
                iv.filePath = child.filePath;
                // Nested sequences render as one parent-layer image in the
                // CPU SSOT. The flattened PlaybackEntry keeps the parent clip
                // identity for decoder timing, edit-target stability, and
                // audio routing; nested visual track fidelity is not derived
                // from this flattened track index.
                iv.trackIdx = parentTrackIdx;
                iv.videoScale = child.videoScale;
                iv.videoDx = child.videoDx;
                iv.videoDy = child.videoDy;
                iv.rotation2DDegrees = child.rotation2DDegrees;
                iv.opacity = child.opacity * parentClip.opacity;
                iv.isVfxFootage = child.isVfxFootage;
                iv.blendMode = child.blendMode;
                iv.vfxIntensity = child.vfxIntensity;
                iv.vfxBlackLevel = child.vfxBlackLevel;
                iv.fitContain = child.fitContain;
                iv.fitCover = child.fitCover;
                iv.colorMeta = child.colorMeta;
                iv.layerStyle = child.layerStyle;
                iv.volume = child.volume * parentClip.volume;
                iv.pan = qBound(-1.0, child.pan + parentClip.pan, 1.0);
                const ClipInfo &clipEqOwner = child.clipEqEnabled ? child : parentClip;
                iv.clipEq = clipEqOwner.clipEq;
                iv.clipEqEnabled = clipEqOwner.clipEqEnabled;
                iv.volumeEnvelope = child.volumeEnvelope;
                iv.clipIdx = parentClipIdx;
                iv.leadInType = child.leadIn.type;
                iv.leadInDuration = child.leadIn.duration;
                iv.leadInAlignment = child.leadIn.alignment;
                iv.leadInEasing = child.leadIn.easing;
                iv.trailOutType = child.trailOut.type;
                iv.trailOutDuration = child.trailOut.duration;
                iv.trailOutAlignment = child.trailOut.alignment;
                iv.trailOutEasing = child.trailOut.easing;
                iv.softness = child.trailOut.softness;
                iv.borderWidth = child.trailOut.borderWidth;
                iv.borderColor = child.trailOut.borderColor;
                iv.stabilizerKeyframes = child.stabilizerKeyframes;
                out.append(iv);

                childAccum += childDur;
            }
        }
        sequenceStack.removeLast();
        return true;
    };

    QVector<QVector<Interval>> trackIntervals;
    trackIntervals.reserve(m_videoTracks.size());
    for (int t = 0; t < m_videoTracks.size(); ++t) {
        QVector<Interval> ivs;
        auto *track = m_videoTracks[t];
        // Skip hidden video tracks entirely — they contribute nothing to playback.
        if (track && !track->isHidden()) {
            const auto &clips = track->clips();
            double accum = 0.0;
            for (int ci = 0; ci < clips.size(); ++ci) {
                const auto &c = clips[ci];
                accum += qMax(0.0, c.leadInSec); // leading gap before this clip
                const double clipDur = c.effectiveDuration();
                if (clipDur <= 0.0) continue;
                QVector<QString> sequenceStack;
                if (!m_activeSequenceId.isEmpty())
                    sequenceStack.append(m_activeSequenceId);
                if (c.isSequenceReference()
                    && appendSequenceIntervals(c, accum, t, ci, ivs,
                                               /*depth=*/0, sequenceStack)) {
                    accum += clipDur;
                    continue;
                }
                Interval iv;
                iv.timelineStart = accum;
                iv.timelineEnd = accum + clipDur;
                iv.clipIn = c.inPoint;
                iv.clipOut = (c.outPoint > 0.0) ? c.outPoint : c.duration;
                iv.speed = (c.speed > 0.0) ? c.speed : 1.0;
                iv.reversed = c.reversed;
                if (c.timeRemapCurve.keys.size() == 1) {
                    const double sourceOut = (c.outPoint > 0.0) ? c.outPoint : c.duration;
                    const double holdSource =
                        qBound(c.inPoint,
                               c.inPoint + qMax(0.0, c.timeRemapCurve.keys.first().srcTime),
                               sourceOut);
                    iv.clipIn = holdSource;
                    iv.speed = 1.0e-9;
                }
                iv.filePath = c.filePath;
                iv.trackIdx = t;
                iv.videoScale = c.videoScale;
                iv.videoDx = c.videoDx;
                iv.videoDy = c.videoDy;
                iv.rotation2DDegrees = c.rotation2DDegrees;
                iv.opacity = c.opacity;
                iv.isVfxFootage = c.isVfxFootage;
                iv.blendMode = c.blendMode;
                iv.vfxIntensity = c.vfxIntensity;
                iv.vfxBlackLevel = c.vfxBlackLevel;
                iv.fitContain = c.fitContain;
                iv.fitCover = c.fitCover;
                iv.colorMeta = c.colorMeta;
                iv.layerStyle = c.layerStyle;
                iv.volume = c.volume;
                iv.pan = c.pan;
                iv.clipEq = c.clipEq;
                iv.clipEqEnabled = c.clipEqEnabled;
                iv.volumeEnvelope = c.volumeEnvelope;
                iv.clipIdx = ci;
                iv.leadInType = c.leadIn.type;
                iv.leadInDuration = c.leadIn.duration;
                iv.leadInAlignment = c.leadIn.alignment;
                iv.leadInEasing = c.leadIn.easing;
                iv.trailOutType = c.trailOut.type;
                iv.trailOutDuration = c.trailOut.duration;
                iv.trailOutAlignment = c.trailOut.alignment;
                iv.trailOutEasing = c.trailOut.easing;
                iv.softness = c.trailOut.softness;
                iv.borderWidth = c.trailOut.borderWidth;
                iv.borderColor = c.trailOut.borderColor;
                iv.stabilizerKeyframes = c.stabilizerKeyframes;
                ivs.append(iv);
                accum += clipDur;
            }
        }
        trackIntervals.append(ivs);
    }

    for (auto &trackIvs : trackIntervals) {
        QVector<OverlapInterval> shared;
        for (const auto &iv : trackIvs)
            shared.append(static_cast<const OverlapInterval &>(iv));
        applyOverlapTransitionsToIntervals(shared,
            [&](const OverlapInterval &a) {
                const int index = int(&a - shared.constData());
                auto *track = m_videoTracks.value(trackIvs[index].trackIdx, nullptr);
                if (!track || a.clipIdx < 0 || a.clipIdx >= track->clips().size())
                    return 0.0;
                return qMax(0.0, (track->clips()[a.clipIdx].duration - a.clipOut)
                                      / (a.speed > 0.0 ? a.speed : 1.0));
            },
            [](const OverlapInterval &b) {
                return qMax(0.0, b.clipIn / (b.speed > 0.0 ? b.speed : 1.0));
            });
        for (int i = 0; i < trackIvs.size(); ++i)
            static_cast<OverlapInterval &>(trackIvs[i]) = shared[i];
        if (overlapIntervals)
            overlapIntervals->append(shared);
    }

    // The export accessor needs only bounds, not playback bindings or the
    // flattened public entries. Keep its read path free of binding writes.
    if (overlapIntervals)
        return result;

    QVector<Interval> visible;
    for (const auto &trackClips : trackIntervals) {
        for (const auto &iv : trackClips)
            visible.append(iv);
    }

    // (timelineStart, sourceTrack asc) so overlapping entries at the same time
    // list V1 first. findActiveEntryAt returns the first match, giving V1 the
    // "primary" playback role until the multi-layer compositor lands.
    std::sort(visible.begin(), visible.end(),
              [](const Interval &a, const Interval &b) {
                  if (a.timelineStart != b.timelineStart)
                      return a.timelineStart < b.timelineStart;
                  return a.trackIdx < b.trackIdx;
              });

    const auto matteSnapshot = trackMatteEntries();
    result.reserve(visible.size());
    for (const auto &iv : visible) {
        if (iv.timelineEnd - iv.timelineStart < 1e-6) continue;
        PlaybackEntry e;
        e.filePath = iv.filePath;
        e.clipIn = iv.clipIn;
        e.clipOut = iv.clipOut;
        e.timelineStart = iv.timelineStart;
        e.timelineEnd = iv.timelineEnd;
        e.speed = iv.speed;
        e.sourceTrack = iv.trackIdx;
        e.videoScale = iv.videoScale;
        e.videoDx = iv.videoDx;
        e.videoDy = iv.videoDy;
        e.rotation2DDegrees = iv.rotation2DDegrees;
        e.opacity = iv.opacity;
        e.isVfxFootage = iv.isVfxFootage;
        e.blendMode = iv.blendMode;
        e.vfxIntensity = iv.vfxIntensity;
        e.vfxBlackLevel = iv.vfxBlackLevel;
        e.fitContain = iv.fitContain;
        e.fitCover = iv.fitCover;
        e.colorMeta = iv.colorMeta;
        e.layerStyle = iv.layerStyle;
        e.volume = iv.volume;
        e.pan = iv.pan;
        e.clipEq = iv.clipEq;
        e.clipEqEnabled = iv.clipEqEnabled;
        e.volumeEnvelope = iv.volumeEnvelope;
        e.sourceClipIndex = iv.clipIdx;
        e.leadInType = iv.leadInType;
        e.leadInDuration = iv.leadInDuration;
        e.leadInEasing = iv.leadInEasing;
        e.trailOutType = iv.trailOutType;
        e.trailOutDuration = iv.trailOutDuration;
        e.trailOutEasing = iv.trailOutEasing;
        e.softness = iv.softness;
        e.borderWidth = iv.borderWidth;
        e.borderColor = iv.borderColor;
        e.stabilizerKeyframes = iv.stabilizerKeyframes;
        reversedBindings.append({e, iv.reversed});
        // STAGE4B: carry this entry's track-matte assignment (if any) so the
        // live GPU compositor can apply it identically to the export path
        // (TimelineFrameRenderer.cpp:792-814). This is pure data plumbing —
        // no behavior changes until part 2 wires the GPU gate. The matte hash
        // is keyed by trackMatteClipKey(trackIdx,clipIdx), the same key space
        // export uses (renderClipId). Read a locked carrier snapshot so the
        // GUI setter cannot race the playback reader. Default-OFF leaves
        // matteTypeOrdinal=0 / matteSourceClipId empty == today's behavior.
        {
            const QString matteKey = trackMatteClipKey(e.sourceTrack, e.sourceClipIndex);
            const auto mit = matteSnapshot.constFind(matteKey);
            if (mit != matteSnapshot.cend()) {
                e.matteTypeOrdinal = static_cast<int>(mit.value().matteType);
                e.matteSourceClipId = mit.value().matteSourceClipId;
            }
            const auto pit = m_clipParentEntries.constFind(matteKey);
            if (pit != m_clipParentEntries.cend() && pit.value() != matteKey)
                e.parentClipId = pit.value();
        }
        qInfo() << "[SEQ] entry idx=" << result.size()
                << "tl=[" << iv.timelineStart << "," << iv.timelineEnd << "]"
                << "clip=[" << iv.clipIn << "," << iv.clipOut << "]"
                << "track=" << iv.trackIdx << "file=" << iv.filePath;
        // Audio routing: the corresponding A<n> track at the same index acts as
        // the audio mute switch for this entry's media file.
        if (iv.trackIdx >= 0 && iv.trackIdx < m_audioTracks.size()
            && m_audioTracks[iv.trackIdx] && m_audioTracks[iv.trackIdx]->isMuted()) {
            e.audioMuted = true;
        }
        result.append(e);
    }
    setVideoReversedPlaybackBindings(reversedBindings);
    return result;
}

namespace {
// GUI-thread test controls; resetting the switch also resets instrumentation.
bool audioOverlapEnabled = true;
int audioOverlapCalls = 0;
}

void Timeline::setAudioOverlapEnabledForTest(bool enabled)
{
    audioOverlapEnabled = enabled;
    audioOverlapCalls = 0;
}

int Timeline::audioOverlapCallCountForTest()
{
    return audioOverlapCalls;
}

QVector<PlaybackEntry> Timeline::computeAudioPlaybackSequence() const
{
    // Sum-mix every visible audio track. AudioMixer in VideoPlayer combines
    // each emitted entry into a single stereo output, so unlike the legacy
    // A1-wins gap-fill (which silently dropped overlapping clips) every
    // unmuted track contributes simultaneously. Track mute, per-clip
    // volume, and source identity all ride on the PlaybackEntry — the
    // mixer keys decoders by (filePath, clipIn, sourceTrack, sourceClipIndex)
    // to reuse open file contexts across re-emits.
    QVector<PlaybackEntry> result;
    QVector<AudioChannelModePlaybackBinding> channelModeBindings;
    QVector<AudioReversedPlaybackBinding> reversedBindings;
    if (m_audioTracks.isEmpty()) {
        setAudioChannelModePlaybackBindings(channelModeBindings);
        setAudioReversedPlaybackBindings(reversedBindings);
        return result;
    }

    const auto appendReversedBinding = [&reversedBindings](
        const PlaybackEntry &entry, bool reversed) {
        reversedBindings.append({entry, reversed});
    };

    const QVector<TimelineSequence> sequenceSnapshot = sequences();
    auto findSequence = [&sequenceSnapshot](const QString &id) -> const TimelineSequence * {
        for (const TimelineSequence &sequence : sequenceSnapshot) {
            if (sequence.id == id)
                return &sequence;
        }
        return nullptr;
    };

    // First pass: project each audio track into the SAME overlap solver as
    // video. Reverse sources use reflected coordinates so the solver's
    // clipOut always represents the timeline's trailing source edge.
    const auto audioIntervals = [](const QVector<ClipInfo> &clips) {
        QVector<OverlapInterval> intervals;
        double accum = 0.0;
        bool hasPair = false;
        for (int i = 0; i < clips.size(); ++i) {
            const ClipInfo &c = clips[i];
            accum += qMax(0.0, c.leadInSec); // including zero-length clips
            const double duration = c.effectiveDuration();
            if (duration <= 0.0)
                continue;
            OverlapInterval iv;
            iv.clipIdx = i;
            iv.timelineStart = accum;
            iv.timelineEnd = accum + duration;
            iv.speed = c.speed > 0.0 ? c.speed : 1.0;
            const double out = c.outPoint > 0.0 ? c.outPoint : c.duration;
            iv.clipIn = c.reversed ? c.duration - out : c.inPoint;
            iv.clipOut = c.reversed ? c.duration - c.inPoint : out;
            // US-206's compatibility exception is specifically CrossDissolve.
            // Other authored audio transitions retain their legacy positions;
            // only this solver projection masks them, never the emitted entry.
            iv.leadInType = c.leadIn.type == TransitionType::CrossDissolve
                ? c.leadIn.type : TransitionType::None;
            iv.leadInDuration = c.leadIn.duration;
            iv.leadInEasing = c.leadIn.easing;
            iv.trailOutType = c.trailOut.type == TransitionType::CrossDissolve
                ? c.trailOut.type : TransitionType::None;
            iv.trailOutDuration = c.trailOut.duration;
            iv.trailOutAlignment = c.trailOut.alignment;
            iv.trailOutEasing = c.trailOut.easing;
            if (!intervals.isEmpty()) {
                const auto &a = intervals.last();
                hasPair |= isOverlapTransition(a.trailOutType)
                    && a.trailOutType == iv.leadInType
                    && qAbs(a.timelineEnd - iv.timelineStart) <= 1e-3
                    && qMin(a.trailOutDuration, iv.leadInDuration) > 0.0;
            }
            intervals.append(iv);
            accum += duration;
        }
        if (audioOverlapEnabled && hasPair) {
            ++audioOverlapCalls;
            Timeline::applyOverlapTransitionsToIntervals(intervals,
                [&clips](const OverlapInterval &a) {
                    return qMax(0.0, (clips[a.clipIdx].duration - a.clipOut) / a.speed);
                }, [](const OverlapInterval &b) {
                    return qMax(0.0, b.clipIn / b.speed);
                });
        }
        return intervals;
    };
    const auto intervalClip = [](const ClipInfo &original, const OverlapInterval &iv) {
        ClipInfo c = original;
        // Apply deltas, avoiding roundoff and outPoint normalization on the
        // disabled/default path (including reversed clips).
        const double out = c.outPoint > 0.0 ? c.outPoint : c.duration;
        const double oldIn = c.reversed ? c.duration - out : c.inPoint;
        const double oldOut = c.reversed ? c.duration - c.inPoint : out;
        if (iv.clipIn != oldIn || iv.clipOut != oldOut) {
            c.inPoint = c.reversed ? c.inPoint - (iv.clipOut - oldOut) : iv.clipIn;
            c.outPoint = c.reversed ? out - (iv.clipIn - oldIn) : iv.clipOut;
        }
        c.leadIn.duration = iv.leadInDuration;
        c.trailOut.duration = iv.trailOutDuration;
        return c;
    };

    std::function<bool(const ClipInfo &, double, int, int, bool, int,
                       QVector<QString> &)> appendSequenceAudioEntries;
    appendSequenceAudioEntries = [&](const ClipInfo &parentClip,
                                     double parentTimelineStart,
                                     int parentTrackIdx,
                                     int parentClipIdx,
                                     bool trackMuted,
                                     int depth,
                                     QVector<QString> &sequenceStack) -> bool {
        const QString refId = resolveSequenceRefId(parentClip);
        if (refId.isEmpty())
            return false;
        if (depth > 8 || sequenceStack.contains(refId))
            return true;
        const TimelineSequence *sequence = findSequence(refId);
        if (!sequence)
            return true;

        const double parentSpeed = (parentClip.speed > 0.0) ? parentClip.speed : 1.0;
        const double sourceIn = qMax(0.0, parentClip.inPoint);
        const double sourceOut = (parentClip.outPoint > 0.0)
            ? parentClip.outPoint
            : qMax(parentClip.duration, sequence->duration());
        if (sourceOut <= sourceIn)
            return true;

        QVector<QString> reverseScanStack = sequenceStack;
        const bool reverseCompositionActive = parentClip.reversed
            || sequenceContainsReversedClip(
                sequenceSnapshot, refId, /*audio=*/true,
                reverseScanStack);
        sequenceStack.append(refId);
        for (const QVector<ClipInfo> &track : sequence->audioTracks) {
            const auto intervals = audioIntervals(track);
            const bool expanded = std::any_of(intervals.cbegin(), intervals.cend(),
                [&track](const OverlapInterval &iv) {
                    const auto &c = track[iv.clipIdx];
                    const double out = c.outPoint > 0.0 ? c.outPoint : c.duration;
                    return iv.clipIn != (c.reversed ? c.duration - out : c.inPoint)
                        || iv.clipOut != (c.reversed ? c.duration - c.inPoint : out);
                });
            for (const OverlapInterval &iv : intervals) {
                const ClipInfo child = intervalClip(track[iv.clipIdx], iv);
                if (!child.visible && !clipgeom::isNullObjectFilePath(child.filePath)) continue;
                const double childAccum = iv.timelineStart;

                const double overlapStart = qMax(sourceIn, childAccum);
                const double overlapEnd = qMin(sourceOut, iv.timelineEnd);
                if (overlapEnd <= overlapStart) {
                    continue;
                }

                const double childSpeed = (child.speed > 0.0) ? child.speed : 1.0;
                const double localIn = overlapStart - childAccum;
                const double localOut = overlapEnd - childAccum;
                const NestedSequenceIntervalMapping parentInterval =
                    mapNestedSequenceInterval(
                        parentClip, parentTimelineStart, sourceIn,
                        overlapStart, overlapEnd, parentSpeed,
                        reverseCompositionActive);
                const double childTimelineStart = parentInterval.timelineStart;
                const double childTimelineEnd = parentInterval.timelineEnd;
                double childSourceIn = child.inPoint + localIn * childSpeed;
                double childSourceOut = child.inPoint + localOut * childSpeed;
                double combinedSpeed = childSpeed * parentSpeed;
                bool effectiveReversed = false;
                // Preserve the legacy nested-audio flattening exactly while
                // reverse is OFF. Only reverse-enabled branches need the
                // shared source mapper and direction composition.
                if (reverseCompositionActive) {
                    const double mappedAtLocalIn =
                        child.sourceSecondAtLocalTime(localIn);
                    const double mappedAtLocalOut =
                        child.sourceSecondAtLocalTime(localOut);
                    childSourceIn = qMin(mappedAtLocalIn, mappedAtLocalOut);
                    childSourceOut = qMax(mappedAtLocalIn, mappedAtLocalOut);
                    const bool childDescending =
                        mappedAtLocalOut + 1e-9 < mappedAtLocalIn;
                    effectiveReversed = parentInterval.runsBackward
                        != childDescending;
                    const double timelineSpan = qMax(
                        1e-9, childTimelineEnd - childTimelineStart);
                    combinedSpeed = qMax(
                        1e-9,
                        (childSourceOut - childSourceIn) / timelineSpan);
                }

                if (child.isSequenceReference()) {
                    ClipInfo nested = child;
                    nested.inPoint = childSourceIn;
                    nested.outPoint = childSourceOut;
                    nested.speed = combinedSpeed;
                    nested.reversed = effectiveReversed;
                    nested.volume = child.volume * parentClip.volume;
                    nested.pan = qBound(-1.0, child.pan + parentClip.pan, 1.0);
                    // US-1110 clip EQ: child first, else the parent's (never both).
                    if (!child.clipEqEnabled) {
                        nested.clipEq = parentClip.clipEq;
                        nested.clipEqEnabled = parentClip.clipEqEnabled;
                    }
                    appendSequenceAudioEntries(nested, childTimelineStart,
                                               parentTrackIdx, parentClipIdx,
                                               trackMuted, depth + 1,
                                               sequenceStack);
                    continue;
                }

                PlaybackEntry e;
                e.filePath = child.filePath;
                e.clipIn = childSourceIn;
                e.clipOut = childSourceOut;
                e.timelineStart = childTimelineStart;
                e.timelineEnd = childTimelineEnd;
                e.speed = combinedSpeed;
                // Audio flattening also keeps parent identity so sequence
                // clips added as linked V/A references route as one parent
                // timeline item while recursively exposing the leaf media.
                e.sourceTrack = parentTrackIdx;
                e.audioMuted = trackMuted;
                e.volume = child.volume * parentClip.volume;
                e.pan = qBound(-1.0, child.pan + parentClip.pan, 1.0);
                // US-1110 clip EQ: child first, else the parent's (never both).
                const ClipInfo &clipEqOwner = child.clipEqEnabled ? child : parentClip;
                e.clipEq = clipEqOwner.clipEq;
                e.clipEqEnabled = clipEqOwner.clipEqEnabled;
                e.volumeEnvelope = child.volumeEnvelope;
                e.sourceClipIndex = parentClipIdx;
                // Transitions are attached to timeline edges, not source
                // direction. Reverse the edges only when the parent sequence
                // itself runs backward; a child-only reverse keeps its
                // authored fade-in/fade-out positions.
                const Transition &timelineLead = parentInterval.runsBackward
                    ? child.trailOut : child.leadIn;
                const Transition &timelineTrail = parentInterval.runsBackward
                    ? child.leadIn : child.trailOut;
                e.leadInType = timelineLead.type;
                e.leadInDuration = timelineLead.duration;
                e.leadInEasing = timelineLead.easing;
                e.trailOutType = timelineTrail.type;
                e.trailOutDuration = timelineTrail.duration;
                e.trailOutEasing = timelineTrail.easing;
                // Newly expanded edges live in child-sequence seconds. Map
                // their fade windows along with their intervals, including
                // a sped-up or backward-running parent. Leave legacy fades
                // byte-identical when this track has not been expanded.
                if (expanded) {
                    const double timeScale = (childTimelineEnd - childTimelineStart)
                        / (overlapEnd - overlapStart);
                    if (e.leadInType == TransitionType::CrossDissolve)
                        e.leadInDuration *= timeScale;
                    if (e.trailOutType == TransitionType::CrossDissolve)
                        e.trailOutDuration *= timeScale;
                }
                result.append(e);
                appendReversedBinding(e, effectiveReversed);
                channelModeBindings.append({
                    qRound64(e.clipIn * 1000.0),
                    e.sourceTrack,
                    e.sourceClipIndex,
                    child.audioChannelMode
                });
            }
        }
        sequenceStack.removeLast();
        return true;
    };

    for (int t = 0; t < m_audioTracks.size(); ++t) {
        auto *track = m_audioTracks[t];
        if (!track || track->isHidden()) continue;
        const auto &clips = track->clips();
        const bool trackMuted = track->isMuted();
        const auto intervals = audioIntervals(clips);
        for (const OverlapInterval &iv : intervals) {
            const int ci = iv.clipIdx;
            const ClipInfo c = intervalClip(clips[ci], iv);
            if (!c.visible && !clipgeom::isNullObjectFilePath(c.filePath)) continue;
            const double accum = iv.timelineStart;
            QVector<QString> sequenceStack;
            if (!m_activeSequenceId.isEmpty())
                sequenceStack.append(m_activeSequenceId);
            if (c.isSequenceReference()
                && appendSequenceAudioEntries(c, accum, t, ci, trackMuted,
                                              /*depth=*/0, sequenceStack)) {
                continue;
            }
            PlaybackEntry e;
            e.filePath = c.filePath;
            e.clipIn = c.inPoint;
            e.clipOut = (c.outPoint > 0.0) ? c.outPoint : c.duration;
            e.timelineStart = accum;
            e.timelineEnd = iv.timelineEnd;
            e.speed = (c.speed > 0.0) ? c.speed : 1.0;
            e.sourceTrack = t;
            e.audioMuted = trackMuted;
            e.volume = c.volume;
            e.pan = c.pan;
            e.clipEq = c.clipEq;
            e.clipEqEnabled = c.clipEqEnabled;
            e.volumeEnvelope = c.volumeEnvelope;
            e.sourceClipIndex = ci;
            e.leadInType = c.leadIn.type;
            e.leadInDuration = c.leadIn.duration;
            e.leadInEasing = c.leadIn.easing;
            e.trailOutType = c.trailOut.type;
            e.trailOutDuration = c.trailOut.duration;
            e.trailOutEasing = c.trailOut.easing;
            result.append(e);
            appendReversedBinding(e, c.reversed);
            channelModeBindings.append({
                qRound64(e.clipIn * 1000.0),
                e.sourceTrack,
                e.sourceClipIndex,
                c.audioChannelMode
            });
        }
    }

    // Stable order: (timelineStart asc, sourceTrack asc) — purely cosmetic
    // for the mixer (entry lookup is by AudioTrackKey) but keeps debug logs
    // easy to follow.
    std::sort(result.begin(), result.end(),
              [](const PlaybackEntry &a, const PlaybackEntry &b) {
                  if (a.timelineStart != b.timelineStart)
                      return a.timelineStart < b.timelineStart;
                  return a.sourceTrack < b.sourceTrack;
              });
    setAudioChannelModePlaybackBindings(channelModeBindings);
    setAudioReversedPlaybackBindings(reversedBindings);
    return result;
}

void Timeline::refreshPlaybackSequence()
{
    // External hook for proxy generation / proxy mode toggle. The clip graph
    // hasn't changed, but the resolved playback paths have, so just re-emit
    // the same sequences we'd produce for any other rebuild trigger and let
    // MainWindow's getProxyPath translation in the sequenceChanged handler
    // pick up the now-Ready proxies.
    scheduleEmitSequenceChanged();
}

void Timeline::setExternalTrackStateHooks(std::function<QJsonObject()> collect,
                                          std::function<void(const QJsonObject&)> apply)
{
    m_collectExternalTrackState = std::move(collect);
    m_applyExternalTrackState = std::move(apply);
    captureExternalTrackState();
}

void Timeline::captureExternalTrackStateForCompoundEdit()
{
    captureExternalTrackState();
}

void Timeline::captureExternalTrackState()
{
    if (m_collectExternalTrackState)
        m_undoManager->updateCurrentExternalTrackState(m_collectExternalTrackState());
}

void Timeline::setBusRoutingBridge(std::function<QJsonObject()> provider,
                                   std::function<void(const QJsonObject&)> applier)
{
    m_busRoutingProvider = std::move(provider);
    m_busRoutingApplier = std::move(applier);
}

void Timeline::setSidecarStateHooks(std::function<QJsonObject()> collect,
                                    std::function<void(const QJsonObject&)> apply)
{
    m_sidecarStateCollector = std::move(collect);
    m_sidecarStateApplier = std::move(apply);
}

void Timeline::absorbIntoCurrentUndoPoint()
{
    reconcileConnectedClips();
    m_undoManager->updateCurrentState(currentState());
    captureReconcileBaseline();
}

void Timeline::saveUndoState(const QString &description)
{
    // An emptied history (load / new project: UndoManager::clear) starts from
    // an empty baseline, so its first reconcile only re-derives offsets.
    if (m_undoManager->currentIndex() < 0)
        clearReconcileBaseline();
    reconcileConnectedClips();
    flushPendingKeyRemap();
    m_undoManager->saveState(currentState(), description);
    captureReconcileBaseline();
    updateConnectionOverlay();
}

bool Timeline::AnchorSignature::sameAs(const AnchorSignature &o) const
{
    return std::abs(startSec - o.startSec) <= kConnectionEps
        && std::abs(inPoint - o.inPoint) <= kConnectionEps
        && std::abs(outPoint - o.outPoint) <= kConnectionEps
        && std::abs(speed - o.speed) <= kConnectionEps
        && reversed == o.reversed && rampIdentity == o.rampIdentity
        && remapEmpty == o.remapEmpty;
}

Timeline::AnchorSignature Timeline::AnchorSignature::of(const ClipInfo &clip, double startSec)
{
    AnchorSignature sig;
    sig.startSec = startSec;
    sig.inPoint = clip.inPoint;
    sig.outPoint = clip.outPoint > 0.0 ? clip.outPoint : clip.duration;
    sig.speed = clip.speed;
    sig.reversed = clip.reversed;
    sig.rampIdentity = clip.speedRamp.isIdentity();
    sig.remapEmpty = !clip.hasTimeRemap();
    sig.filePath = clip.filePath;
    return sig;
}

bool Timeline::hasConnectedClips() const
{
    for (const bool audio : {false, true}) {
        for (const TimelineTrack *track : audio ? m_audioTracks : m_videoTracks) {
            if (!track) continue;
            for (const ClipInfo &clip : track->clips())
                if (!clip.anchorUid.isEmpty()) return true;
        }
    }
    return false;
}

void Timeline::renameAnchorReferences(const QString &fromUid, const QString &toUid)
{
    if (fromUid.isEmpty() || fromUid == toUid)
        return;
    for (const bool audio : {false, true}) {
        for (TimelineTrack *track : audio ? m_audioTracks : m_videoTracks) {
            if (!track) continue;
            QVector<ClipInfo> clips = track->clips();
            bool changed = false;
            for (ClipInfo &clip : clips) {
                if (clip.anchorUid != fromUid) continue;
                clip.anchorUid = toUid;
                changed = true;
            }
            if (changed) track->setClips(clips);
        }
    }
}

QString Timeline::reconcileSequenceKey() const
{
    return m_activeSequenceId.isEmpty() ? QString::fromLatin1(kDefaultSequenceId)
                                        : m_activeSequenceId;
}

void Timeline::clearReconcileBaseline()
{
    m_reconcileBaseline = ReconcileBaseline{};
    m_lastKeyRemap.reset();
    m_pendingDeletedUids.clear();
    m_pendingSplits.clear();
    m_pendingClipSplits.clear();
    m_pendingDetachedSpans.clear();
}

void Timeline::appendConnectionWarning(const QString &message)
{
    m_connectionWarnings.append(message);
    // Nobody drains the list in an ordinary GUI session; keep it bounded.
    while (m_connectionWarnings.size() > 100)
        m_connectionWarnings.removeFirst();
}

QStringList Timeline::takeConnectionWarnings()
{
    return std::exchange(m_connectionWarnings, QStringList{});
}

void Timeline::captureReconcileBaseline()
{
    m_reconcileBaseline = ReconcileBaseline{};
    m_reconcileBaseline.sequenceId = reconcileSequenceKey();
    QHash<QString, AnchorSignature> anchors;
    QSet<QString> ambiguous; // a uid on two V1 clips is no anchor (reconcile detaches)
    if (const TimelineTrack *v1 = m_videoTracks.value(0, nullptr)) {
        const auto &clips = v1->clips();
        const auto starts = clipStartSecs(clips);
        for (int i = 0; i < clips.size(); ++i) {
            if (clips[i].uid.isEmpty()) continue;
            if (anchors.contains(clips[i].uid)) ambiguous.insert(clips[i].uid);
            else anchors.insert(clips[i].uid, AnchorSignature::of(clips[i], starts[i]));
        }
    }
    for (const QString &uid : ambiguous) anchors.remove(uid);
    for (const bool audio : {false, true}) {
        const auto &tracks = audio ? m_audioTracks : m_videoTracks;
        for (int t = 0; t < tracks.size(); ++t) {
            if (!tracks[t]) continue;
            const auto &clips = tracks[t]->clips();
            const auto starts = clipStartSecs(clips);
            for (int i = 0; i < clips.size(); ++i) {
                const ClipInfo &clip = clips[i];
                if (clip.anchorUid.isEmpty() || clip.uid.isEmpty()) continue;
                ConnectionBaselineEntry entry;
                entry.kind = audio ? TrackKind::Audio : TrackKind::Video;
                entry.track = t;
                entry.startSec = starts[i];
                entry.anchorUid = clip.anchorUid;
                entry.anchorOffsetSec = clip.anchorOffsetSec;
                const auto anchor = anchors.constFind(clip.anchorUid);
                entry.anchorFound = anchor != anchors.cend();
                if (entry.anchorFound) entry.anchorSig = anchor.value();
                m_reconcileBaseline.clips.insert(connectionBaselineKey(audio, t, clip.uid), entry);
            }
        }
    }
    // US-1203: the linked audio of each V1 clip is its implicit connection. A
    // link group cut on V1 names the piece the audio clip overlaps most.
    const TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    if (!v1) return;
    const auto &v1Clips = v1->clips();
    const auto v1Starts = clipStartSecs(v1Clips);
    QHash<int, QVector<int>> v1ByGroup;
    for (int i = 0; i < v1Clips.size(); ++i)
        if (v1Clips[i].linkGroup > 0 && anchors.contains(v1Clips[i].uid))
            v1ByGroup[v1Clips[i].linkGroup].append(i);
    if (v1ByGroup.isEmpty()) return;
    for (int t = 0; t < m_audioTracks.size(); ++t) {
        if (!m_audioTracks[t]) continue;
        const auto &clips = m_audioTracks[t]->clips();
        const auto starts = clipStartSecs(clips);
        for (int i = 0; i < clips.size(); ++i) {
            const ClipInfo &clip = clips[i];
            if (clip.linkGroup <= 0 || clip.uid.isEmpty() || !clip.anchorUid.isEmpty()) continue;
            const QVector<int> candidates = v1ByGroup.value(clip.linkGroup);
            int best = -1;
            double bestOverlap = 0.0;
            for (const int k : candidates) {
                const double overlap = qMin(starts[i] + clip.effectiveDuration(),
                                            v1Starts[k] + v1Clips[k].effectiveDuration())
                    - qMax(starts[i], v1Starts[k]);
                if (best < 0 || overlap > bestOverlap) {
                    best = k;
                    bestOverlap = overlap;
                }
            }
            if (best < 0) continue;
            ReconcileBaseline::StorylineLink link;
            link.anchorUid = v1Clips[best].uid;
            link.linkGroup = clip.linkGroup;
            link.startSec = starts[i];
            link.anchorStartSec = v1Starts[best];
            m_reconcileBaseline.storylineLinks.insert(connectionBaselineKey(true, t, clip.uid), link);
        }
    }
}

// US-1201 (design r4 §6-4): bring connected clips back to their anchors right
// before an undo point is saved. Order: (0) take the pending deletions,
// (1) an empty / other-sequence baseline only re-derives offsets, (3) a clip
// absent from the baseline re-derives its offset, (4) an anchor gone from V1
// deletes the connection only when a Timeline delete function recorded it, the
// track is unlocked and the offset lies in [0, length) of the deleted piece,
// otherwise it detaches with a warning, (5) a clip that did not move follows an
// anchor whose mapping signature changed (linear anchor: same source frame;
// non-linear: same offset), a moved clip re-derives its offset.
void Timeline::reconcileConnectedClips()
{
    if (m_reconcilingConnections)
        return;
    const QHash<QString, QVector<QPair<double, double>>> deleted =
        std::exchange(m_pendingDeletedUids, {});
    const QVector<SplitRecord> splits = std::exchange(m_pendingSplits, {});
    const QHash<const TimelineTrack *, QVector<SplitRecord>> clipSplits =
        std::exchange(m_pendingClipSplits, {});
    const QVector<DetachedSpan> detachedSpans = std::exchange(m_pendingDetachedSpans, {});
    // US-1203: the implicit connections (storyline mode) first, so the explicit
    // ones below place around the linked audio's new positions.
    m_reconcilingConnections = true;
    followStorylineLinks();
    m_reconcilingConnections = false;
    if (!hasConnectedClips())
        return;
    m_reconcilingConnections = true;

    const bool baselineValid = !m_reconcileBaseline.sequenceId.isEmpty()
        && m_reconcileBaseline.sequenceId == reconcileSequenceKey();
    struct AnchorNow { double startSec = 0.0; AnchorSignature sig; };
    QHash<QString, AnchorNow> anchors;
    // A uid held by two V1 clips (a cross-track drop onto V1 keeps its uid)
    // names no anchor: its connections detach instead of following either.
    QSet<QString> ambiguousAnchors;
    if (const TimelineTrack *v1 = m_videoTracks.value(0, nullptr)) {
        const auto &clips = v1->clips();
        const auto starts = clipStartSecs(clips);
        for (int i = 0; i < clips.size(); ++i) {
            if (clips[i].uid.isEmpty()) continue;
            if (anchors.contains(clips[i].uid)) ambiguousAnchors.insert(clips[i].uid);
            else anchors.insert(clips[i].uid, {starts[i], AnchorSignature::of(clips[i], starts[i])});
        }
    }

    enum class Action { Keep, Rederive, Detach, Delete, Move };
    struct Decision {
        bool audio = false;
        int track = -1;
        int clipIndex = -1;   // the field-only setClips below keeps the order
        QString uid;
        int linkGroup = 0;
        QString displayName;
        Action action = Action::Keep;
        QString reason;
        QString anchorUid;        // Rederive / Move: the (split-routed) anchor
        double offsetSec = 0.0;   // Rederive: the new offset
        double targetSec = 0.0;   // Move: the new start
        double anchorStartSec = 0.0;
    };
    QVector<Decision> decisions;
    // A connection the baseline knew (same uid, track and anchor) is judged on
    // its pre-edit anchor-local offset. A link group is judged as a whole on
    // its earliest member (the connection point), so it is never split across
    // anchor pieces, deleted in part or detached in part.
    auto knownBase = [&](bool audio, int t, const ClipInfo &clip) {
        if (!baselineValid) return m_reconcileBaseline.clips.cend();
        const auto base = m_reconcileBaseline.clips.constFind(connectionBaselineKey(audio, t, clip.uid));
        return base != m_reconcileBaseline.clips.cend() && base->anchorUid == clip.anchorUid
            ? base : m_reconcileBaseline.clips.cend();
    };
    auto groupKey = [](const ClipInfo &clip) {
        return QString::number(clip.linkGroup) + QLatin1Char(':') + clip.anchorUid;
    };
    // A connected clip's own split piece (a fresh uid the baseline lacks) follows
    // its track's splits back to the piece the baseline knew: its pre-edit
    // connection point is that piece's offset plus the split seconds on the way.
    auto lineagePoint = [&](bool audio, int t, const ClipInfo &clip, double *pointSec) {
        if (!baselineValid) return false;
        const QVector<SplitRecord> lineage = clipSplits.value(trackAt(audio, t));
        QString uid = clip.uid;
        double sumSec = 0.0;
        for (int step = 0; step < lineage.size(); ++step) {
            const auto split = std::find_if(lineage.cbegin(), lineage.cend(),
                [&](const SplitRecord &s) { return s.rightUid == uid; });
            if (split == lineage.cend()) return false;
            uid = split->leftUid;
            sumSec += split->splitLocalSec;
            const auto base = m_reconcileBaseline.clips.constFind(connectionBaselineKey(audio, t, uid));
            if (base == m_reconcileBaseline.clips.cend()) continue;
            if (base->anchorUid != clip.anchorUid) return false;
            *pointSec = base->anchorOffsetSec + sumSec;
            return true;
        }
        return false;
    };
    // A linked group is judged per component. A link group holds one clip per
    // track (canGroupClips), so the known members alone on their track (in the
    // group) are the uncut pre-edit group and stay together whatever their spans.
    // Any other member (a split piece, the known piece it was cut from, or a known
    // remnant sharing its track with another remnant of an earlier cut) joins the
    // members its post-edit span overlaps, so the pieces of one cut (a linked pair
    // cut at the same boundary) go to one anchor piece with the member they
    // overlap, and the remnants of different cuts (in this edit or an earlier
    // one) are judged apart, each by its own point.
    struct GroupMember {
        QString memberKey;
        QString trackKey;
        bool known = false;
        double pointSec = 0.0;
        double startSec = 0.0;
        double endSec = 0.0;
    };
    auto memberKey = [](bool audio, int t, int i) {
        return QStringLiteral("%1:%2:%3").arg(int(audio)).arg(t).arg(i);
    };
    QHash<QString, QVector<GroupMember>> groupMembers;
    for (const bool audio : {false, true}) {
        const auto &tracks = audio ? m_audioTracks : m_videoTracks;
        for (int t = 0; t < tracks.size(); ++t) {
            if (!tracks[t] || (!audio && t == 0)) continue;
            const auto &clips = tracks[t]->clips();
            const auto starts = clipStartSecs(clips);
            for (int i = 0; i < clips.size(); ++i) {
                const ClipInfo &clip = clips[i];
                if (clip.anchorUid.isEmpty() || clip.linkGroup <= 0) continue;
                const auto base = knownBase(audio, t, clip);
                GroupMember member;
                member.memberKey = memberKey(audio, t, i);
                member.trackKey = QStringLiteral("%1:%2").arg(int(audio)).arg(t);
                member.startSec = starts[i];
                member.endSec = starts[i] + clip.effectiveDuration();
                member.known = base != m_reconcileBaseline.clips.cend();
                if (member.known)
                    member.pointSec = base->anchorOffsetSec;
                else if (!lineagePoint(audio, t, clip, &member.pointSec))
                    continue;
                groupMembers[groupKey(clip)].append(member);
            }
        }
    }
    QHash<QString, QString> memberGroup;           // member -> component key
    QHash<QString, double> groupConnectionOffset;  // component key -> point
    for (auto group = groupMembers.cbegin(); group != groupMembers.cend(); ++group) {
        const QVector<GroupMember> &members = group.value();
        QVector<int> root(members.size());
        std::iota(root.begin(), root.end(), 0);
        auto find = [&root](int i) {
            while (root[i] != i) i = root[i] = root[root[i]];
            return i;
        };
        QHash<QString, int> onTrack;
        for (const GroupMember &member : members)
            ++onTrack[member.trackKey];
        auto intact = [&onTrack](const GroupMember &member) {
            return member.known && onTrack.value(member.trackKey) == 1;
        };
        for (int i = 0; i < members.size(); ++i)
            for (int j = i + 1; j < members.size(); ++j)
                if ((intact(members[i]) && intact(members[j]))
                    || qMin(members[i].endSec, members[j].endSec)
                        - qMax(members[i].startSec, members[j].startSec) > kConnectionEps)
                    root[find(j)] = find(i);
        for (int i = 0; i < members.size(); ++i) {
            const QString key = group.key() + QLatin1Char('#') + QString::number(find(i));
            memberGroup.insert(members[i].memberKey, key);
            const auto it = groupConnectionOffset.constFind(key);
            groupConnectionOffset.insert(key, it == groupConnectionOffset.cend()
                ? members[i].pointSec : qMin(it.value(), members[i].pointSec));
        }
    }
    auto inSpans = [](const QVector<QPair<double, double>> &spans, double offsetSec) {
        for (const auto &span : spans)
            if (offsetSec >= span.first - kConnectionEps && offsetSec < span.second - kConnectionEps)
                return true;
        return false;
    };
    for (const bool audio : {false, true}) {
        const auto &tracks = audio ? m_audioTracks : m_videoTracks;
        for (int t = 0; t < tracks.size(); ++t) {
            const TimelineTrack *track = tracks[t];
            if (!track) continue;
            const auto &clips = track->clips();
            const auto starts = clipStartSecs(clips);
            for (int i = 0; i < clips.size(); ++i) {
                const ClipInfo &clip = clips[i];
                if (clip.anchorUid.isEmpty()) continue;
                Decision d;
                d.audio = audio;
                d.track = t;
                d.clipIndex = i;
                d.uid = clip.uid;
                d.linkGroup = clip.linkGroup;
                d.displayName = clip.displayName.isEmpty()
                    ? QFileInfo(clip.filePath).fileName() : clip.displayName;
                const double startSec = starts[i];
                if (!audio && t == 0) {
                    // V1 clips cannot carry a connection (moved onto V1).
                    d.action = Action::Detach;
                    decisions.append(d);
                    continue;
                }
                const auto base = knownBase(audio, t, clip);
                const bool known = base != m_reconcileBaseline.clips.cend();
                double lineageOffset = 0.0;
                const bool lineage = !known && lineagePoint(audio, t, clip, &lineageOffset);
                const double baseOffset = known ? base->anchorOffsetSec
                    : lineage ? lineageOffset : clip.anchorOffsetSec;
                const double connectionOffset = (known || lineage) && clip.linkGroup > 0
                    ? groupConnectionOffset.value(memberGroup.value(memberKey(audio, t, i)), baseOffset)
                    : baseOffset;
                if (known || lineage) {
                    // (4) An explicit delete removes the connections whose point lies in
                    // a deleted anchor-local span; a silence cut detaches them.
                    if (inSpans(deleted.value(clip.anchorUid), connectionOffset)) {
                        if (track->isLocked()) {
                            d.action = Action::Detach;
                            d.reason = QStringLiteral("トラックがロックされています");
                        } else {
                            d.action = Action::Delete;
                        }
                        decisions.append(d);
                        continue;
                    }
                    bool silenced = false;
                    for (const DetachedSpan &span : detachedSpans)
                        silenced = silenced || (span.anchorUid == clip.anchorUid
                            && connectionOffset >= span.localStart - kConnectionEps
                            && connectionOffset < span.localEnd - kConnectionEps);
                    if (silenced) {
                        d.action = Action::Detach;
                        d.reason = QStringLiteral("接続先の区間が無音カットで除去されました");
                        decisions.append(d);
                        continue;
                    }
                }
                // (2) Splits of the anchor, in order and in split-time local seconds:
                // a connection point at or after the cut moves to the right piece.
                // The position does not change; the right piece's baseline signature
                // is the left one's shifted by the cut. A connected clip's own split
                // piece routes by its pre-edit point, never by where the edit put it.
                QString anchorUid = clip.anchorUid;
                double routedOffset = baseOffset;
                double routedPoint = connectionOffset;
                AnchorSignature routedSig = known ? base->anchorSig : AnchorSignature{};
                if (known || lineage) {
                    for (const SplitRecord &split : splits) {
                        if (split.leftUid != anchorUid
                            || routedPoint < split.splitLocalSec - kConnectionEps)
                            continue;
                        anchorUid = split.rightUid;
                        routedOffset -= split.splitLocalSec;
                        routedPoint -= split.splitLocalSec;
                        routedSig.startSec += split.splitLocalSec;
                        routedSig.inPoint += split.splitLocalSec * routedSig.speed;
                    }
                }
                const bool rerouted = anchorUid != clip.anchorUid;
                d.anchorUid = anchorUid;
                if (ambiguousAnchors.contains(anchorUid)) {
                    d.action = Action::Detach;
                    d.reason = QStringLiteral("接続先の V1 クリップが一意ではありません");
                    decisions.append(d);
                    continue;
                }
                const auto anchor = anchors.constFind(anchorUid);
                if (anchor == anchors.cend()) {
                    // Not an explicit delete of the connection point: detach.
                    d.action = Action::Detach;
                    d.reason = track->isLocked()
                        ? QStringLiteral("トラックがロックされています")
                        : QStringLiteral("接続先のクリップが V1 にありません");
                    decisions.append(d);
                    continue;
                }
                d.anchorStartSec = anchor->startSec;
                const double offsetNow = startSec - anchor->startSec;
                const bool inBaseline = baselineValid
                    && m_reconcileBaseline.clips.contains(connectionBaselineKey(audio, t, clip.uid));
                if (baselineValid && !inBaseline && m_rederiveDisabledForTest) {
                    // Test seam: no (3) re-derivation, the stored offset places the clip.
                    const double storedSec = qMax(0.0, anchor->startSec + clip.anchorOffsetSec);
                    if (std::abs(storedSec - startSec) > kConnectionEps) {
                        d.action = track->isLocked() ? Action::Detach : Action::Move;
                        d.reason = track->isLocked() ? QStringLiteral("トラックがロックされています") : QString();
                        d.targetSec = storedSec;
                        decisions.append(d);
                    }
                    continue;
                }
                const bool moved = !known
                    || !base->anchorFound
                    || base->track != t
                    || std::abs(base->startSec - startSec) > kConnectionEps;
                if (moved || routedSig.sameAs(anchor->sig)) {
                    if (rerouted || std::abs(clip.anchorOffsetSec - offsetNow) > kConnectionEps) {
                        d.action = Action::Rederive;
                        d.offsetSec = offsetNow;
                        decisions.append(d);
                    }
                    continue;
                }
                double targetSec = anchor->startSec + routedOffset;
                if (routedSig.linear() && anchor->sig.linear()
                    && routedSig.filePath == anchor->sig.filePath) {
                    // Unclamped linear source mapping (not ClipInfo's clamped API).
                    const double sourceSec = routedSig.inPoint + routedOffset * routedSig.speed;
                    targetSec = anchor->startSec
                        + (sourceSec - anchor->sig.inPoint) / anchor->sig.speed;
                }
                targetSec = qMax(0.0, targetSec);
                if (std::abs(targetSec - startSec) <= kConnectionEps) {
                    if (rerouted || std::abs(clip.anchorOffsetSec - offsetNow) > kConnectionEps) {
                        d.action = Action::Rederive;
                        d.offsetSec = offsetNow;
                        decisions.append(d);
                    }
                    continue;
                }
                if (track->isLocked()) {
                    d.action = Action::Detach;
                    d.reason = QStringLiteral("トラックがロックされています");
                } else {
                    d.action = Action::Move;
                    d.targetSec = targetSec;
                }
                decisions.append(d);
            }
        }
    }

    // A link group is connected, moved and deleted only as a whole: one member
    // that must detach detaches every member of the group where it stands.
    QSet<int> detachGroups;
    for (const Decision &d : decisions)
        if (d.linkGroup > 0 && d.action == Action::Detach && !d.reason.isEmpty())
            detachGroups.insert(d.linkGroup);
    QSet<int> warnedGroups;
    QStringList warnings;
    for (Decision &d : decisions) {
        if ((d.action == Action::Move || d.action == Action::Rederive || d.action == Action::Delete)
            && d.linkGroup > 0 && detachGroups.contains(d.linkGroup)) {
            d.action = Action::Detach;
            d.reason = QStringLiteral("接続グループの一部を動かせません");
        }
        if (d.action != Action::Detach || d.reason.isEmpty()) continue;
        if (d.linkGroup > 0 && warnedGroups.contains(d.linkGroup)) continue;
        if (d.linkGroup > 0) warnedGroups.insert(d.linkGroup);
        warnings.append(QStringLiteral("「%1」の接続を解除しました (%2)").arg(d.displayName, d.reason));
    }

    auto trackOf = [this](bool audio, int index) { return trackAt(audio, index); };

    // Field-only changes (offset / detach) keep every position: setClips per track.
    for (const bool audio : {false, true}) {
        const int count = audio ? m_audioTracks.size() : m_videoTracks.size();
        for (int t = 0; t < count; ++t) {
            TimelineTrack *track = trackOf(audio, t);
            if (!track) continue;
            QVector<ClipInfo> clips = track->clips();
            bool changed = false;
            QSet<int> decided;
            for (const Decision &d : decisions) {
                if (d.audio != audio || d.track != t) continue;
                decided.insert(d.clipIndex);
                if (d.action != Action::Rederive && d.action != Action::Detach) continue;
                const int index = d.clipIndex;
                if (index < 0 || index >= clips.size()) continue;
                if (d.action == Action::Rederive) {
                    clips[index].anchorUid = d.anchorUid;
                    clips[index].anchorOffsetSec = d.offsetSec;
                } else {
                    clips[index].anchorUid.clear();
                    clips[index].anchorOffsetSec = 0.0;
                }
                changed = true;
            }
            // A connected member that needed no change still leaves a detached group.
            for (int i = 0; i < clips.size(); ++i) {
                if (decided.contains(i) || clips[i].anchorUid.isEmpty()) continue;
                if (clips[i].linkGroup <= 0 || !detachGroups.contains(clips[i].linkGroup)) continue;
                clips[i].anchorUid.clear();
                clips[i].anchorOffsetSec = 0.0;
                changed = true;
            }
            if (changed) track->setClips(clips);
        }
    }

    // Deletions and moves: take every affected clip out first (descending
    // index, preserving downstream positions) so moving clips never collide
    // with each other, then place the moved ones by target time.
    // Placement is per link group: when a member finds no slot while another
    // member ends up connected, every track goes back to this snapshot, the
    // whole group is detached where it stands and the pass runs again. The
    // set of such groups only grows, so the loop ends.
    // The selections and the active video track go back with the clips: setClips
    // keeps m_selectedClips, whose indices a rolled-back pass may have moved.
    QVector<QVector<ClipInfo>> videoSnapshot;
    QVector<QVector<ClipInfo>> audioSnapshot;
    QVector<QList<int>> videoSelectionSnapshot;
    QVector<QList<int>> audioSelectionSnapshot;
    for (const bool audio : {false, true}) {
        const int count = audio ? m_audioTracks.size() : m_videoTracks.size();
        for (int t = 0; t < count; ++t) {
            const TimelineTrack *track = trackOf(audio, t);
            (audio ? audioSnapshot : videoSnapshot).append(track ? track->clips() : QVector<ClipInfo>{});
            (audio ? audioSelectionSnapshot : videoSelectionSnapshot)
                .append(track ? track->selectedClips() : QList<int>{});
        }
    }
    const int activeVideoTrackSnapshot = m_activeVideoTrackIndex;
    bool rolledBack = false;
    struct Pending {
        bool audio;
        int track;
        ClipInfo clip;
        double targetSec;
        double originalSec;
        QString oldKey;       // video: the "t:i" key in `before`
        int destTrack = -1;
        bool wasSelected = false;   // selected when the Move removed it
    };
    // Uids are unique only per track: a track that already holds the clip's
    // uid is no destination (a later delete there could hit the wrong clip).
    auto holdsUid = [](const TimelineTrack *track, const QString &uid) {
        for (const ClipInfo &clip : track->clips())
            if (clip.uid == uid) return true;
        return false;
    };
    QHash<int, QString> forcedDetachGroups;   // linkGroup -> name of the clip with no slot
    TrackClipSnapshot before;
    QVector<Pending> placing;
    QStringList placementWarnings;
    bool structural = false;
    bool lostActiveSelection = false;
    for (;;) {
        before = snapshotTrackClips(this);
        placing.clear();
        placementWarnings.clear();
        structural = false;
        lostActiveSelection = false;
        for (const bool audio : {false, true}) {
            const int count = audio ? m_audioTracks.size() : m_videoTracks.size();
            for (int t = 0; t < count; ++t) {
                TimelineTrack *track = trackOf(audio, t);
                if (!track) continue;
                QVector<QPair<int, const Decision *>> removals;
                for (const Decision &d : decisions) {
                    if (d.audio != audio || d.track != t) continue;
                    if (d.action != Action::Delete && d.action != Action::Move) continue;
                    if (d.clipIndex >= 0 && d.clipIndex < track->clipCount())
                        removals.append({d.clipIndex, &d});
                }
                std::sort(removals.begin(), removals.end(),
                          [](const auto &a, const auto &b) { return a.first > b.first; });
                // Removing preserves downstream positions, so these starts stay valid.
                const auto starts = clipStartSecs(track->clips());
                for (const auto &removal : removals) {
                    ClipInfo clip = track->clips().at(removal.first);
                    const bool wasSelected = removal.second->action == Action::Move
                        && track->isClipSelected(removal.first);
                    // A Move is an internal relocation: its removal must not announce
                    // "no selection" (the final notification below reports the real one).
                    const bool prevSync = m_inLinkedSelectionSync;
                    if (removal.second->action == Action::Move) m_inLinkedSelectionSync = true;
                    track->removeClipPreservingDownstream(removal.first);
                    m_inLinkedSelectionSync = prevSync;
                    structural = true;
                    if (removal.second->action == Action::Move) {
                        clip.anchorUid = removal.second->anchorUid;
                        clip.anchorOffsetSec = removal.second->targetSec - removal.second->anchorStartSec;
                        // Descending removal: removal.first still indexes `before`.
                        placing.append({audio, t, clip, removal.second->targetSec, starts[removal.first],
                                        audio ? QString() : trackMatteClipKey(t, removal.first)});
                        placing.last().wasSelected = wasSelected;
                    }
                }
            }
        }
        std::sort(placing.begin(), placing.end(),
                  [](const Pending &a, const Pending &b) { return a.targetSec < b.targetSec; });
        // A pending clip still owns its uid on its own track: placing another clip
        // with that uid there first would leave its fallback no uid-safe slot.
        QHash<QPair<bool, int>, QHash<QString, int>> reservedUids;
        for (const Pending &p : placing)
            if (!p.clip.uid.isEmpty()) ++reservedUids[qMakePair(p.audio, p.track)][p.clip.uid];
        QSet<int> placementWarned = warnedGroups;
        QHash<int, QString> failedGroups;
        for (Pending &p : placing) {
            if (!p.clip.uid.isEmpty()) {
                QHash<QString, int> &own = reservedUids[qMakePair(p.audio, p.track)];
                if (--own[p.clip.uid] <= 0) own.remove(p.clip.uid);
            }
            const double duration = p.clip.effectiveDuration();
            const int count = p.audio ? m_audioTracks.size() : m_videoTracks.size();
            TimelineTrack *destination = nullptr;
            TimelineTrack::DropPlan plan;
            // The clip's own track first, then the next free one above (video) /
            // below (audio), which are both the higher indices.
            for (int t = p.track; t < count && !destination; ++t) {
                TimelineTrack *candidate = trackOf(p.audio, t);
                if (!candidate || (t != p.track && candidate->isLocked())) continue;
                if (!p.clip.uid.isEmpty()
                    && (holdsUid(candidate, p.clip.uid)
                        || reservedUids.value(qMakePair(p.audio, t)).contains(p.clip.uid)))
                    continue;
                plan = candidate->planDrop(p.targetSec, duration);
                if (plan.valid) {
                    destination = candidate;
                    p.destTrack = t;
                }
            }
            ClipInfo clip = p.clip;
            if (!destination) {
                // Reconcile never changes the track count. A structural undo applies
                // the external state (mixer / mattes / particles) of the pre-edit
                // entry, and reconcile runs after the edit inside saveUndoState, where
                // no capture is right for both (mattes are post-edit, the mixer is not).
                // So without a free track the clip goes back to its own start (its slot
                // is free), else to the end of its own track, and is detached.
                destination = trackOf(p.audio, p.track);
                p.destTrack = p.track;
                // The reservation keeps the own track free of this uid.
                Q_ASSERT(p.clip.uid.isEmpty() || !holdsUid(destination, p.clip.uid));
                plan = destination->planDrop(p.originalSec, duration);
                if (!plan.valid) {
                    double endSec = 0.0;
                    for (const ClipInfo &c : destination->clips())
                        endSec += c.leadInSec + c.effectiveDuration();
                    plan = destination->planDrop(endSec, duration);
                }
                clip.anchorUid.clear();
                clip.anchorOffsetSec = 0.0;
                const QString name = clip.displayName.isEmpty()
                    ? QFileInfo(clip.filePath).fileName() : clip.displayName;
                if (clip.linkGroup > 0 && !failedGroups.contains(clip.linkGroup))
                    failedGroups.insert(clip.linkGroup, name);
                if (clip.linkGroup <= 0 || !placementWarned.contains(clip.linkGroup)) {
                    if (clip.linkGroup > 0) placementWarned.insert(clip.linkGroup);
                    placementWarnings.append(QStringLiteral("「%1」の接続を解除しました (%2)")
                                                 .arg(name, QStringLiteral("退避先のトラックがありません")));
                }
            }
            destination->insertClipPreservingDownstream(plan.insertIdx, clip, plan.newLeadIn);
        }
        // The Move removals dropped the selection silently: reselect each selected
        // clip by uid at its destination (no notification, the catch-all below
        // reports it) and follow the active video track.
        for (const Pending &p : placing) {
            if (!p.wasSelected) continue;
            TimelineTrack *destination = trackOf(p.audio, p.destTrack);
            int found = -1;
            if (destination && !p.clip.uid.isEmpty()) {
                for (int i = 0; i < destination->clipCount(); ++i)
                    if (destination->clips().at(i).uid == p.clip.uid) { found = i; break; }
            }
            if (found < 0) {
                if (!p.audio && p.track == m_activeVideoTrackIndex) lostActiveSelection = true;
                continue;
            }
            QList<int> selection = destination->selectedClips();
            if (!selection.contains(found)) selection.append(found);
            destination->restoreClipSelection(selection);
            if (!p.audio && p.track == m_activeVideoTrackIndex) m_activeVideoTrackIndex = p.destTrack;
        }
        // A group with a member that found no slot must not keep another member
        // connected (moved, or in place).
        QSet<int> partial;
        for (const bool audio : {false, true}) {
            for (const TimelineTrack *track : audio ? m_audioTracks : m_videoTracks) {
                if (!track) continue;
                for (const ClipInfo &clip : track->clips())
                    if (clip.linkGroup > 0 && !clip.anchorUid.isEmpty()
                        && failedGroups.contains(clip.linkGroup))
                        partial.insert(clip.linkGroup);
            }
        }
        if (partial.isEmpty()) break;
        for (const int group : partial) forcedDetachGroups.insert(group, failedGroups.value(group));
        for (Decision &d : decisions)
            if (d.linkGroup > 0 && forcedDetachGroups.contains(d.linkGroup)
                && (d.action == Action::Move || d.action == Action::Delete))
                d.action = Action::Detach;
        for (const bool audio : {false, true}) {
            QVector<QVector<ClipInfo>> &snapshot = audio ? audioSnapshot : videoSnapshot;
            for (int t = 0; t < snapshot.size(); ++t) {
                for (ClipInfo &clip : snapshot[t]) {
                    if (clip.linkGroup <= 0 || !forcedDetachGroups.contains(clip.linkGroup)) continue;
                    clip.anchorUid.clear();
                    clip.anchorOffsetSec = 0.0;
                }
                if (TimelineTrack *track = trackOf(audio, t)) {
                    track->setClips(snapshot[t]);
                    // The snapshot is the pre-pass array, so its indices are the
                    // pre-pass selection (restoreClipSelection announces nothing).
                    track->restoreClipSelection(
                        (audio ? audioSelectionSnapshot : videoSelectionSnapshot).at(t));
                }
            }
        }
        m_activeVideoTrackIndex = activeVideoTrackSnapshot;
        rolledBack = true;
    }
    warnings.append(placementWarnings);
    for (auto it = forcedDetachGroups.cbegin(); it != forcedDetachGroups.cend(); ++it)
        warnings.append(QStringLiteral("「%1」の接続を解除しました (%2)")
                            .arg(it.value(), QStringLiteral("退避先のトラックがありません")));
    if (structural) {
        // The carriers follow the clips by uid, with the reconcile's own old ->
        // new video keys first (uid twins on several tracks cannot be told apart
        // by uid). clipMovedAcrossTracks is not emitted: its receiver would remap
        // the same move a second time.
        QHash<QString, QString> explicitMoves;
        for (const Pending &p : placing) {
            if (p.audio || p.clip.uid.isEmpty()) continue;
            const TimelineTrack *track = trackOf(false, p.destTrack);
            if (!track) continue;
            for (int i = 0; i < track->clipCount(); ++i) {
                if (track->clips().at(i).uid != p.clip.uid) continue;
                explicitMoves.insert(p.oldKey, trackMatteClipKey(p.destTrack, i));
                break;
            }
        }
        remapCarriersByUid(this, before, explicitMoves);
        refreshPlaybackSequence();
        scheduleEmitSequenceChanged();
    }
    m_reconcilingConnections = false;
    // Catch-all for every caller that edits V1 and then saves (media import and the
    // insertClipPreservingDownstream sites): the moves above kept the active video
    // track, so tell the listeners once where its selection is now (also after a
    // rolled-back pass, whose last pass may have moved nothing).
    if ((!placing.isEmpty() || rolledBack) && m_activeVideoTrackIndex >= 0) {
        if (const TimelineTrack *active = m_videoTracks.value(m_activeVideoTrackIndex, nullptr);
            active && active->selectedClip() >= 0) {
            emit clipSelected(active->selectedClip());
            emit clipSelectedOnTrack(m_activeVideoTrackIndex, active->selectedClip());
        } else if (lostActiveSelection) {
            m_activeVideoTrackIndex = -1;
            emit clipSelected(-1);
            emit clipSelectedOnTrack(-1, -1);
        }
    }
    if (!warnings.isEmpty()) {
        for (const QString &warning : warnings) appendConnectionWarning(warning);
        emit connectionWarningsChanged();
    }
}

bool Timeline::connectClip(TrackKind kind, int trackIndex, int clipIndex,
                           const QString &anchorUid, QString *err)
{
    if (!applyConnection(kind, trackIndex, clipIndex, anchorUid, err))
        return false;
    saveUndoState(tr("主ストーリーラインに接続"));
    return true;
}

bool Timeline::applyConnection(TrackKind kind, int trackIndex, int clipIndex,
                               const QString &anchorUid, QString *err)
{
    if (err) err->clear();
    const bool audio = kind == TrackKind::Audio;
    TimelineTrack *target = trackAt(audio, trackIndex);
    if (!target || clipIndex < 0 || clipIndex >= target->clipCount())
        return setTimelineEditError(err, QStringLiteral("クリップの指定が範囲外です"));
    const TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    double anchorStartSec = 0.0;
    int anchorMatches = 0;
    if (v1 && !anchorUid.isEmpty()) {
        const auto starts = clipStartSecs(v1->clips());
        for (int i = 0; i < v1->clipCount(); ++i) {
            if (v1->clips().at(i).uid == anchorUid) {
                if (anchorMatches == 0) anchorStartSec = starts[i];
                ++anchorMatches;
            }
        }
    }
    if (anchorMatches == 0)
        return setTimelineEditError(err, QStringLiteral("接続先は V1 のクリップに限られます"));
    if (anchorMatches > 1)
        return setTimelineEditError(err, QStringLiteral("接続先の V1 クリップが一意ではありません"));
    const int linkGroup = target->clips().at(clipIndex).linkGroup;
    struct Member { TimelineTrack *track; int index; double startSec; };
    QVector<Member> members;
    for (const bool memberAudio : {false, true}) {
        const auto &tracks = memberAudio ? m_audioTracks : m_videoTracks;
        for (int t = 0; t < tracks.size(); ++t) {
            TimelineTrack *track = tracks[t];
            if (!track) continue;
            const auto starts = clipStartSecs(track->clips());
            for (int i = 0; i < track->clipCount(); ++i) {
                const bool member = linkGroup > 0
                    ? track->clips().at(i).linkGroup == linkGroup
                    : (track == target && i == clipIndex);
                if (!member) continue;
                if (!memberAudio && t == 0)
                    return setTimelineEditError(err, QStringLiteral("V1 のクリップは接続できません"));
                if (track->isLocked())
                    return setTimelineEditError(err, QStringLiteral("ロック中のトラックのクリップは接続できません"));
                members.append({track, i, starts[i]});
            }
        }
    }
    for (const Member &member : members) {
        QVector<ClipInfo> clips = member.track->clips();
        clips[member.index].anchorUid = anchorUid;
        clips[member.index].anchorOffsetSec = member.startSec - anchorStartSec;
        member.track->setClips(clips);
    }
    return true;
}

QString Timeline::storylineAnchorAt(double startSec, double *anchorStartSec, bool allowPreceding) const
{
    const TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    if (!v1) return QString();
    const auto starts = clipStartSecs(v1->clips());
    int found = -1;
    for (int i = 0; i < v1->clipCount(); ++i) {
        const double end = starts[i] + v1->clips().at(i).effectiveDuration();
        if (startSec >= starts[i] - kConnectionEps && startSec < end - kConnectionEps) {
            found = i;
            break;
        }
        if (allowPreceding && end <= startSec + kConnectionEps) found = i;
    }
    if (found < 0) return QString();
    if (anchorStartSec) *anchorStartSec = starts[found];
    return v1->clips().at(found).uid;
}

bool Timeline::connectToStoryline(TrackKind kind, int trackIndex, int clipIndex, QString *err)
{
    if (err) err->clear();
    const bool audio = kind == TrackKind::Audio;
    const TimelineTrack *target = trackAt(audio, trackIndex);
    if (!target || clipIndex < 0 || clipIndex >= target->clipCount())
        return setTimelineEditError(err, QStringLiteral("クリップの指定が範囲外です"));
    if (!audio && trackIndex == 0)
        return setTimelineEditError(err, QStringLiteral("V1 のクリップは接続できません"));
    const QString anchorUid = storylineAnchorAt(clipStartSecs(target->clips())[clipIndex], nullptr, true);
    if (anchorUid.isEmpty())
        return setTimelineEditError(err, QStringLiteral("接続先の V1 クリップがありません"));
    return connectClip(kind, trackIndex, clipIndex, anchorUid, err);
}

bool Timeline::dropMediaOnTrack(TimelineTrack *track, const QString &filePath,
                                double dropTimeSec, QString *err)
{
    if (err) err->clear();
    if (!m_storylineMode)
        return setTimelineEditError(err, QStringLiteral("ストーリーラインモードが OFF です"));
    const bool audio = track && track->isAudioTrack();
    const int trackIndex = track ? (audio ? m_audioTracks : m_videoTracks).indexOf(track) : -1;
    if (trackIndex < 0)
        return setTimelineEditError(err, QStringLiteral("ドロップ先のトラックがありません"));
    if (track->isLocked())
        return setTimelineEditError(err, QStringLiteral("ロック中のトラックには配置できません"));
    const double startSec = qMax(0.0, dropTimeSec);
    if (!audio && trackIndex > 0) {
        // The linked audio partner of a V2+ import lands on A<trackIndex>. A V1 drop
        // takes its audio from storylineAudioSlot (locked tracks skipped) instead.
        const TimelineTrack *partner = m_audioTracks.value(trackIndex, nullptr);
        if (partner && partner->isLocked() && probeTimelineMedia(filePath).audioStreamFound)
            return setTimelineEditError(err, QStringLiteral("ロック中のトラックには配置できません"));
    }
    const QString anchorUid = dropAnchorUid(audio, trackIndex, startSec);
    return importMedia(filePath, trackIndex, startSec, nullptr, err,
                       audio ? ImportMediaKind::AudioOnly : ImportMediaKind::Auto, anchorUid);
}

QString Timeline::dropAnchorUid(bool audio, int trackIndex, double startSec) const
{
    // A drop connects only to the V1 clip it overlaps; otherwise it is imported unconnected.
    // A V1 drop is a storyline insert, never a connection.
    return trackIndex == 0 && !audio ? QString() : storylineAnchorAt(startSec, nullptr, false);
}

bool Timeline::dropMediaOnNewTrack(bool audio, const QString &filePath, double dropTimeSec,
                                   QString *err)
{
    if (err) err->clear();
    if (!m_storylineMode)
        return setTimelineEditError(err, QStringLiteral("ストーリーラインモードが OFF です"));
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile())
        return setTimelineEditError(err, QStringLiteral("ファイルが見つかりません: %1").arg(filePath));
    const TimelineMediaProbe probe = probeTimelineMedia(filePath);
    if (!probe.openedOk || (!probe.videoStreamFound && !probe.audioStreamFound))
        return setTimelineEditError(
            err, QStringLiteral("メディアとして開けません (動画/音声ストリームがありません): %1").arg(filePath));
    if (!audio && !probe.videoStreamFound)
        return setTimelineEditError(err, QStringLiteral("映像の帯には映像ファイルだけ置けます"));
    if (audio && !probe.audioStreamFound)
        return setTimelineEditError(err, QStringLiteral("音声の帯には音声を持つファイルだけ置けます"));
    const double startSec = qMax(0.0, dropTimeSec);
    // addVideoTrack(false) / addAudioTrack(false) skip the external-state capture;
    // take it before the track exists so importMedia's one undo point also removes it.
    captureExternalTrackStateForCompoundEdit();
    if (audio)
        addAudioTrack(false);
    else
        addVideoTrack(false);
    const int newIdx = int((audio ? m_audioTracks : m_videoTracks).size()) - 1;
    const QString anchorUid = dropAnchorUid(audio, newIdx, startSec);
    QString importErr;
    if (importMedia(filePath, newIdx, startSec, nullptr, &importErr,
                    audio ? ImportMediaKind::AudioOnly : ImportMediaKind::Auto, anchorUid)) {
        return true;
    }
    removeTrackInternal(audio, newIdx);
    return setTimelineEditError(err, importErr);
}

bool Timeline::closeGapsOnTrack(TimelineTrack *track)
{
    if (!track || track->isLocked())
        return false;
    QVector<ClipInfo> clips = track->clips();
    bool changed = false;
    for (ClipInfo &clip : clips) {
        if (clip.leadInSec <= 0.0) continue;
        clip.leadInSec = 0.0;
        changed = true;
    }
    if (!changed)
        return false;
    track->setClips(clips);
    scheduleEmitSequenceChanged();
    return true;
}

int Timeline::storylineInsertClip(TimelineTrack *v1, double timelineSec, const ClipInfo &clip,
                                  double *landedSec)
{
    // The insert point is the 3-point insert's (insertClip3Point): before the
    // first clip starting at or after timelineSec. On a gap-free V1 the insert
    // pushes everything after it; the gaps are closed afterwards.
    const int index = threepoint::insertIndexForTime(v1->clips(), timelineSec, nullptr);
    v1->insertClipPreservingDownstream(index, clip, 0.0);
    closeGapsOnTrack(v1);
    if (landedSec) *landedSec = clipStartSecs(v1->clips()).value(index, 0.0);
    return index;
}

bool Timeline::storylineMoveClip(TimelineTrack *v1, int clipIndex, double deltaSec)
{
    if (!v1 || v1->isLocked() || clipIndex < 0 || clipIndex >= v1->clipCount()
        || std::abs(deltaSec) <= kConnectionEps)
        return false;
    const QVector<ClipInfo> original = v1->clips();
    const double targetSec = qMax(0.0, clipStartSecs(original)[clipIndex] + deltaSec);
    const TrackClipSnapshot before = snapshotTrackClips(this);
    const ClipInfo moved = original[clipIndex];
    // Taken out with the other clips in place, so the insert point is found in
    // the positions the user dragged over.
    v1->removeClipPreservingDownstream(clipIndex, false);
    const int index = storylineInsertClip(v1, targetSec, moved);
    const QVector<ClipInfo> &now = v1->clips();
    bool changed = false;
    for (int i = 0; !changed && i < now.size(); ++i)
        changed = now[i].uid != original[i].uid
            || std::abs(now[i].leadInSec - original[i].leadInSec) > kConnectionEps;
    if (!changed) {
        v1->setClips(original);
        v1->setSelectedClip(clipIndex);
        return false;
    }
    v1->setSelectedClip(index);
    // A reorder changes V1 keys a monotonic walk cannot follow.
    remapCarriersByUid(this, before);
    return true;
}

TimelineTrack *Timeline::storylineAudioSlot(int fromTrack, const ClipInfo &clip, double startSec,
                                            TimelineTrack::DropPlan *plan,
                                            const QHash<int, QHash<QString, int>> *reservedUids) const
{
    for (int t = qMax(0, fromTrack); t < m_audioTracks.size(); ++t) {
        TimelineTrack *candidate = m_audioTracks[t];
        if (!candidate || candidate->isLocked()) continue;
        bool holdsUid = false;
        for (const ClipInfo &existing : candidate->clips())
            holdsUid = holdsUid || (!clip.uid.isEmpty() && existing.uid == clip.uid);
        if (holdsUid || (!clip.uid.isEmpty() && reservedUids
                         && reservedUids->value(t).contains(clip.uid)))
            continue;
        const TimelineTrack::DropPlan candidatePlan = candidate->planDrop(startSec, clip.effectiveDuration());
        if (!candidatePlan.valid) continue;
        if (plan) *plan = candidatePlan;
        return candidate;
    }
    return nullptr;
}

// US-1203 (design r4 §4 US-1203 (c), §6-2 rule 9): in storyline mode the linked
// audio of a V1 clip is an implicit connection. Audio that stayed where the
// baseline saw it while its V1 clip moved follows at the baseline offset; audio
// absent from the baseline, or moved by the edit itself, keeps its position (its
// offset is re-derived, never reset to 0). Locked tracks stay. A clip with no
// room on its track goes to the next free audio track below, else back to its
// own start (the reconcile's placement rule). No connection warnings.
bool Timeline::followStorylineLinks(const QHash<int, QHash<QString, int>> &extraReserved)
{
    if (!m_storylineMode || m_reconcileBaseline.storylineLinks.isEmpty()
        || m_reconcileBaseline.sequenceId != reconcileSequenceKey())
        return false;
    const TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    if (!v1) return false;
    struct Anchor { double startSec = 0.0; int linkGroup = 0; };
    QHash<QString, Anchor> anchors;
    QSet<QString> duplicated;
    {
        const auto starts = clipStartSecs(v1->clips());
        for (int i = 0; i < v1->clipCount(); ++i) {
            const ClipInfo &clip = v1->clips().at(i);
            if (clip.uid.isEmpty()) continue;
            if (anchors.contains(clip.uid)) duplicated.insert(clip.uid);
            anchors.insert(clip.uid, {starts[i], clip.linkGroup});
        }
    }
    struct Move { int track; int index; double targetSec; double originalSec; };
    QVector<Move> moves;
    for (int t = 0; t < m_audioTracks.size(); ++t) {
        const TimelineTrack *track = m_audioTracks[t];
        if (!track || track->isLocked()) continue;
        const auto &clips = track->clips();
        const auto starts = clipStartSecs(clips);
        for (int i = 0; i < clips.size(); ++i) {
            const ClipInfo &clip = clips[i];
            if (clip.linkGroup <= 0 || !clip.anchorUid.isEmpty()) continue;
            const auto base = m_reconcileBaseline.storylineLinks.constFind(
                connectionBaselineKey(true, t, clip.uid));
            if (base == m_reconcileBaseline.storylineLinks.cend() || base->linkGroup != clip.linkGroup)
                continue;
            const auto anchor = anchors.constFind(base->anchorUid);
            if (anchor == anchors.cend() || duplicated.contains(base->anchorUid)
                || anchor->linkGroup != clip.linkGroup)
                continue;
            if (std::abs(starts[i] - base->startSec) > kConnectionEps
                || std::abs(anchor->startSec - base->anchorStartSec) <= kConnectionEps)
                continue;
            const double targetSec = qMax(0.0, anchor->startSec + base->startSec - base->anchorStartSec);
            if (std::abs(targetSec - starts[i]) > kConnectionEps)
                moves.append({t, i, targetSec, starts[i]});
        }
    }
    if (moves.isEmpty())
        return false;
    // Take every moving clip out first (descending, downstream positions kept),
    // then place them by target time.
    std::sort(moves.begin(), moves.end(), [](const Move &a, const Move &b) {
        return a.track != b.track ? a.track < b.track : a.index > b.index;
    });
    struct Pending { int track; ClipInfo clip; double targetSec; double originalSec; bool selected; };
    QVector<Pending> placing;
    TrackSelectionUids selection = selectedUidsByTrack(m_audioTracks);
    for (const Move &move : moves) {
        TimelineTrack *track = m_audioTracks[move.track];
        placing.append({move.track, track->clips().at(move.index), move.targetSec, move.originalSec,
                        track->isClipSelected(move.index)});
        track->removeClipPreservingDownstream(move.index, false);
    }
    std::sort(placing.begin(), placing.end(),
              [](const Pending &a, const Pending &b) { return a.targetSec < b.targetSec; });
    // As in the reconcile: a pending clip still owns its uid on its own track, so
    // no other clip with that uid lands there first and its fallback stays uid-safe.
    // The caller's extra reservations hold for the whole placement.
    QHash<int, QHash<QString, int>> reservedUids = extraReserved;
    for (const Pending &p : placing)
        if (!p.clip.uid.isEmpty()) ++reservedUids[p.track][p.clip.uid];
    for (const Pending &p : placing) {
        if (!p.clip.uid.isEmpty()) {
            QHash<QString, int> &own = reservedUids[p.track];
            if (--own[p.clip.uid] <= 0) own.remove(p.clip.uid);
        }
        TimelineTrack::DropPlan plan;
        TimelineTrack *destination = storylineAudioSlot(p.track, p.clip, p.targetSec, &plan, &reservedUids);
        if (!destination) {
            destination = m_audioTracks[p.track];
            plan = destination->planDrop(p.originalSec, p.clip.effectiveDuration());
            if (!plan.valid) {
                double endSec = 0.0;
                for (const ClipInfo &c : destination->clips())
                    endSec += c.leadInSec + c.effectiveDuration();
                plan = destination->planDrop(endSec, p.clip.effectiveDuration());
            }
        }
        destination->insertClipPreservingDownstream(plan.insertIdx, p.clip, plan.newLeadIn);
        if (p.selected) {
            selection[m_audioTracks[p.track]].removeOne(p.clip.uid);
            selection[destination].append(p.clip.uid);
        }
    }
    reselectByUid(selection);
    refreshPlaybackSequence();
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::updateConnectionOverlay()
{
    if (m_connectionOverlay) m_connectionOverlay->update();
}

void Timeline::paintConnections(QPainter &painter) const
{
    const TimelineTrack *v1 = m_videoTracks.value(0, nullptr);
    if (!v1 || !m_tracksWidget || !hasConnectedClips())
        return;
    // Unique V1 uid -> V1 clip index (a duplicated uid has no anchor).
    QHash<QString, int> anchors;
    QSet<QString> duplicated;
    {
        for (int i = 0; i < v1->clipCount(); ++i) {
            const ClipInfo &clip = v1->clips().at(i);
            if (clip.uid.isEmpty()) continue;
            if (anchors.contains(clip.uid)) duplicated.insert(clip.uid);
            anchors.insert(clip.uid, i);
        }
    }
    int dragIndex = -1;
    double dragDeltaSec = 0.0;
    QString dragUid;
    if (v1->moveDragInfo(&dragIndex, &dragDeltaSec) && std::abs(dragDeltaSec) > kConnectionEps)
        dragUid = v1->clips().at(dragIndex).uid;
    auto rowOf = [this](const TimelineTrack *track) {
        if (!track || track->isHidden() || !m_tracksWidget->isAncestorOf(track))
            return QRect();
        return QRect(track->mapTo(m_tracksWidget, QPoint(0, 0)), track->size());
    };
    const QRect v1Row = rowOf(v1);
    if (v1Row.isNull()) return;
    const QColor lineColor(0xff, 0xc8, 0x3c);
    painter.save();
    QFont badgeFont = painter.font();
    badgeFont.setPixelSize(9);
    painter.setFont(badgeFont);
    for (const bool audio : {false, true}) {
        const auto &tracks = audio ? m_audioTracks : m_videoTracks;
        for (int t = audio ? 0 : 1; t < tracks.size(); ++t) {
            const TimelineTrack *track = tracks[t];
            const QRect row = rowOf(track);
            if (row.isNull()) continue;
            const bool above = row.center().y() < v1Row.center().y();
            for (int i = 0; i < track->clipCount(); ++i) {
                const ClipInfo &clip = track->clips().at(i);
                if (clip.anchorUid.isEmpty() || duplicated.contains(clip.anchorUid)) continue;
                const auto anchor = anchors.constFind(clip.anchorUid);
                if (anchor == anchors.constEnd()) continue;
                const int x = row.left() + track->clipStartX(i);
                const int fromY = above ? row.bottom() : row.top();
                const int toY = above ? v1Row.top() + 3 : v1Row.bottom() - 3;
                painter.setPen(QPen(lineColor, 1.5));
                painter.drawLine(x, fromY, x, toY);
                painter.setBrush(lineColor);
                painter.drawEllipse(QPoint(x, toY), 3, 3);
                const int anchorIdx = anchor.value();
                const int anchorLeft = v1Row.left() + v1->clipStartX(anchorIdx);
                const int anchorRight = anchorLeft
                    + qMax(20, int(v1->clips().at(anchorIdx).effectiveDuration() * v1->pixelsPerSecond()));
                if (x < anchorLeft || x > anchorRight) {
                    painter.setPen(QPen(lineColor, 1.0, Qt::DashLine));
                    painter.drawLine(x, toY, qBound(anchorLeft, x, anchorRight), toY);
                }
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0x7a, 0x5a, 0x10, 220));
                const QRect badge(x + 12, row.top() + 2, 26, 11);
                painter.drawRoundedRect(badge, 3, 3);
                painter.setPen(QColor(0xff, 0xf0, 0xc0));
                painter.drawText(badge, Qt::AlignCenter, tr("接続"));
                if (!dragUid.isEmpty() && clip.anchorUid == dragUid) {
                    const int px = row.left()
                        + qMax(0, track->clipStartX(i) + int(dragDeltaSec * track->pixelsPerSecond()));
                    const int pw = qMax(20, int(clip.effectiveDuration() * track->pixelsPerSecond()));
                    painter.setPen(QPen(lineColor, 1.0, Qt::DashLine));
                    painter.setBrush(QColor(0xff, 0xc8, 0x3c, 60));
                    painter.drawRect(QRect(px, row.top() + 1, qMax(2, pw), row.height() - 2));
                }
            }
        }
    }
    painter.restore();
}

namespace {
QStringList offlineCandidatePaths(const QVector<TimelineTrack *> &videoTracks,
                                  const QVector<TimelineTrack *> &audioTracks)
{
    QSet<QString> seen;
    QStringList paths;
    for (const auto *tracks : {&videoTracks, &audioTracks}) {
        for (const TimelineTrack *track : *tracks) {
            if (!track) continue;
            for (const ClipInfo &clip : track->clips()) {
                if (!clip.filePath.isEmpty() && !seen.contains(clip.filePath)) {
                    seen.insert(clip.filePath);
                    paths.append(clip.filePath);
                }
            }
        }
    }
    return paths;
}
} // namespace

void Timeline::setOfflinePaths(const QStringList &missing)
{
    const QSet<QString> missingSet(missing.cbegin(), missing.cend());
    m_offlinePaths.clear();
    for (const QString &path : offlineCandidatePaths(m_videoTracks, m_audioTracks))
        m_offlinePaths.insert(path, missingSet.contains(path));
    for (const auto *tracks : {&m_videoTracks, &m_audioTracks}) {
        for (TimelineTrack *track : *tracks) {
            if (track) track->update();
        }
    }
}

void Timeline::refreshOfflinePathsForUnknown()
{
    bool changed = false;
    for (const QString &path : offlineCandidatePaths(m_videoTracks, m_audioTracks)) {
        if (m_offlinePaths.contains(path)) continue;
        ++m_offlineProbeCount;
        m_offlinePaths.insert(path, mediapaths::isMissingFilesystemPath(path));
        changed = true;
    }
    if (!changed) return;
    for (const auto *tracks : {&m_videoTracks, &m_audioTracks}) {
        for (TimelineTrack *track : *tracks) {
            if (track) track->update();
        }
    }
}

int Timeline::offlineClipCount() const
{
    int count = 0;
    for (const auto *tracks : {&m_videoTracks, &m_audioTracks}) {
        for (const TimelineTrack *track : *tracks) {
            if (!track) continue;
            for (const ClipInfo &clip : track->clips()) {
                if (!clip.filePath.isEmpty() && m_offlinePaths.value(clip.filePath, false))
                    ++count;
            }
        }
    }
    return count;
}

void Timeline::autoConnectDroppedClip(TimelineTrack *track, int clipIndex)
{
    const bool audio = track && track->isAudioTrack();
    const int trackIndex = track ? (audio ? m_audioTracks : m_videoTracks).indexOf(track) : -1;
    if (trackIndex < 0 || (!audio && trackIndex == 0) || clipIndex < 0
        || clipIndex >= track->clipCount())
        return;
    const QString anchorUid = storylineAnchorAt(clipStartSecs(track->clips())[clipIndex], nullptr, false);
    if (anchorUid.isEmpty())
        return;
    QString err;
    if (!applyConnection(audio ? TrackKind::Audio : TrackKind::Video, trackIndex, clipIndex,
                         anchorUid, &err))
        emit statusMessageRequested(err, 4000);
}

bool Timeline::disconnectClip(TrackKind kind, int trackIndex, int clipIndex, QString *err)
{
    if (err) err->clear();
    const bool audio = kind == TrackKind::Audio;
    TimelineTrack *target = trackAt(audio, trackIndex);
    if (!target || clipIndex < 0 || clipIndex >= target->clipCount())
        return setTimelineEditError(err, QStringLiteral("クリップの指定が範囲外です"));
    const int linkGroup = target->clips().at(clipIndex).linkGroup;
    bool changed = false;
    for (const bool memberAudio : {false, true}) {
        for (TimelineTrack *track : memberAudio ? m_audioTracks : m_videoTracks) {
            if (!track) continue;
            QVector<ClipInfo> clips = track->clips();
            bool local = false;
            for (int i = 0; i < clips.size(); ++i) {
                const bool member = linkGroup > 0 ? clips[i].linkGroup == linkGroup
                                                  : (track == target && i == clipIndex);
                if (!member || clips[i].anchorUid.isEmpty()) continue;
                clips[i].anchorUid.clear();
                clips[i].anchorOffsetSec = 0.0;
                local = true;
            }
            if (local) {
                track->setClips(clips);
                changed = true;
            }
        }
    }
    if (!changed)
        return setTimelineEditError(err, QStringLiteral("このクリップは接続されていません"));
    saveUndoState(tr("接続を解除"));
    return true;
}

void Timeline::setProjectOutputConfig(int width, int height, bool explicitOutput, double fps)
{
    m_projectWidth = width;
    m_projectHeight = height;
    m_projectExplicitOutput = explicitOutput;
    if (std::isfinite(fps) && fps > 0.0) m_projectFrameRate = fps;
}

TimelineSequence Timeline::currentSequenceSnapshot(const QString &id,
                                                   const QString &name) const
{
    TimelineSequence sequence;
    sequence.id = id;
    sequence.name = name;
    sequence.videoTracks.reserve(m_videoTracks.size());
    for (const TimelineTrack *track : m_videoTracks)
        sequence.videoTracks.append(track ? track->clips() : QVector<ClipInfo>{});
    sequence.audioTracks.reserve(m_audioTracks.size());
    for (const TimelineTrack *track : m_audioTracks)
        sequence.audioTracks.append(track ? track->clips() : QVector<ClipInfo>{});
    sequence.generatedCaptionOverlays = m_generatedCaptionOverlays;
    return sequence;
}

void Timeline::upsertSequenceSnapshot(const TimelineSequence &sequence)
{
    upsertSequence(m_sequences, sequence);
}

TimelineSequence *Timeline::sequenceById(const QString &sequenceId)
{
    for (TimelineSequence &sequence : m_sequences) {
        if (sequence.id == sequenceId)
            return &sequence;
    }
    return nullptr;
}

const TimelineSequence *Timeline::sequenceById(const QString &sequenceId) const
{
    for (const TimelineSequence &sequence : m_sequences) {
        if (sequence.id == sequenceId)
            return &sequence;
    }
    return nullptr;
}

void Timeline::syncActiveSequenceFromCurrentTracks()
{
    if (!m_sequenceModelEnabled)
        return;
    if (m_activeSequenceId.isEmpty())
        m_activeSequenceId = QString::fromLatin1(kDefaultSequenceId);

    QString name = QStringLiteral("Sequence 1");
    if (const TimelineSequence *existing = sequenceById(m_activeSequenceId)) {
        if (!existing->name.isEmpty())
            name = existing->name;
    }
    upsertSequenceSnapshot(currentSequenceSnapshot(m_activeSequenceId, name));
}

QVector<TimelineSequence> Timeline::sequences() const
{
    if (!m_sequenceModelEnabled)
        return {};

    QVector<TimelineSequence> snapshot = m_sequences;
    QString active = m_activeSequenceId;
    if (active.isEmpty())
        active = QString::fromLatin1(kDefaultSequenceId);

    QString name = QStringLiteral("Sequence 1");
    for (const TimelineSequence &sequence : snapshot) {
        if (sequence.id == active && !sequence.name.isEmpty()) {
            name = sequence.name;
            break;
        }
    }
    upsertSequence(snapshot, currentSequenceSnapshot(active, name));
    return snapshot;
}

QVector<TimelineSequence> Timeline::sequenceList() const
{
    if (!m_sequenceModelEnabled)
        return {currentSequenceSnapshot(QString::fromLatin1(kDefaultSequenceId),
                                        QStringLiteral("メインシーケンス"))};
    auto result = sequences();
    for (auto &sequence : result) {
        if (sequence.id == QLatin1String(kDefaultSequenceId)
            && sequence.name == QStringLiteral("Sequence 1"))
            sequence.name = QStringLiteral("メインシーケンス");
    }
    return result;
}

bool Timeline::renameSequence(const QString &id, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return false;
    if (!m_sequenceModelEnabled) {
        if (id != QLatin1String(kDefaultSequenceId)) return false;
        if (trimmed == QStringLiteral("メインシーケンス")) return true;
        m_sequenceModelEnabled = true;
        m_activeSequenceId = id;
        upsertSequenceSnapshot(currentSequenceSnapshot(id, trimmed));
    } else {
        auto *sequence = sequenceById(id);
        if (!sequence) return false;
        if (sequence->name == trimmed) return true;
        sequence->name = trimmed;
    }
    // Metadata only: clip indices and their carrier bindings do not change.
    saveUndoState(QStringLiteral("シーケンスの名前を変更"));
    rebuildTimelineBreadcrumbBar(this);
    emit sequencesChanged();
    return true;
}

bool Timeline::createSequence(const QString &name)
{
    return createSequence(name, QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}},
                          QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}});
}

bool Timeline::createSequence(const QString &name,
                              const QVector<QVector<ClipInfo>> &videoTracks,
                              const QVector<QVector<ClipInfo>> &audioTracks)
{
    if (name.trimmed().isEmpty()) return false;
    captureExternalTrackState();
    // Sequence navigation does not push undo. Preserve the actual pre-create
    // active ID in the existing top entry, including navigation since its save.
    // externalTrackState is an undo-only carrier; this never enters project JSON.
    if (m_sequenceModelEnabled) {
        QJsonObject external = currentState().externalTrackState;
        external.insert(QStringLiteral("_sequenceCreationActiveId"), m_activeSequenceId);
        m_undoManager->updateCurrentExternalTrackState(external);
    }
    TimelineSequence sequence;
    sequence.id = uniqueSequenceId(m_sequences, QStringLiteral("sequence"), m_sequences.size());
    sequence.name = name.trimmed();
    sequence.videoTracks = videoTracks;
    sequence.audioTracks = audioTracks;
    if (sequence.videoTracks.isEmpty()) sequence.videoTracks.append(QVector<ClipInfo>{});
    if (sequence.audioTracks.isEmpty()) sequence.audioTracks.append(QVector<ClipInfo>{});
    if (!addSequence(sequence) || !setActiveSequence(sequence.id)) return false;
    // Sequence navigation preserves the existing carrier keys; remapping is
    // only valid for clip mutations within the same sequence.
    saveUndoState(QStringLiteral("新規シーケンス"));
    // US-1408: EDL / FCPXML imports (GUI and MCP) land here with clips; an empty
    // "new sequence" has nothing to judge.
    bool hasClips = false;
    for (const auto &track : sequence.videoTracks) hasClips = hasClips || !track.isEmpty();
    for (const auto &track : sequence.audioTracks) hasClips = hasClips || !track.isEmpty();
    if (hasClips) emit mediaImported();
    return true;
}

bool Timeline::createSequenceFromMedia(const QString &filePath, const QString &displayName)
{
    ClipInfo clip;
    clip.filePath = filePath;
    clip.displayName = displayName;
    clip.duration = probeTimelineMedia(filePath).durationSec;
    clip.outPoint = clip.duration;
    return createSequenceFromClip(clip);
}

bool Timeline::createSequenceFromClip(const ClipInfo &source)
{
    const auto media = probeTimelineMedia(source.filePath);
    if (!media.openedOk || (!media.videoStreamFound && !media.audioStreamFound)
        || !std::isfinite(source.effectiveDuration()) || source.effectiveDuration() <= 0.0) {
        emit statusMessageRequested(QStringLiteral("このクリップからシーケンスを作成できません。"), 5000);
        return false;
    }
    ClipInfo clip = source;
    clip.leadInSec = 0.0;
    // US-1214: the new sequence does not inherit the source's connection.
    clip.anchorUid.clear();
    clip.anchorOffsetSec = 0.0;
    // Loaded sequences may contain groups beyond this session's allocator.
    QSet<int> usedGroups;
    const auto existingSequences = sequenceList();
    for (const auto &sequence : existingSequences) {
        for (const auto &track : sequence.videoTracks)
            for (const auto &existing : track) usedGroups.insert(existing.linkGroup);
        for (const auto &track : sequence.audioTracks)
            for (const auto &existing : track) usedGroups.insert(existing.linkGroup);
    }
    do { clip.linkGroup = allocateLinkGroup(); } while (usedGroups.contains(clip.linkGroup));
    QString name = clip.displayName.trimmed();
    if (name.isEmpty()) name = QFileInfo(clip.filePath).completeBaseName();
    const QString base = name;
    QSet<QString> names;
    for (const auto &sequence : existingSequences) names.insert(sequence.name);
    for (int suffix = 2; names.contains(name); ++suffix)
        name = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
    QVector<QVector<ClipInfo>> video{QVector<ClipInfo>{}};
    QVector<QVector<ClipInfo>> audio{QVector<ClipInfo>{}};
    if (media.videoStreamFound) video[0].append(clip);
    if (media.audioStreamFound) audio[0].append(clip);
    if (!createSequence(name, video, audio)) return false;
    const QString info = QStringLiteral("素材 %1x%2 %3fps / プロジェクト %4x%5 %6fps")
        .arg(media.videoWidth).arg(media.videoHeight).arg(media.frameRate, 0, 'g', 6)
        .arg(m_projectWidth).arg(m_projectHeight).arg(m_projectFrameRate, 0, 'g', 6);
    const bool differs = media.videoStreamFound && (media.videoWidth != m_projectWidth
        || media.videoHeight != m_projectHeight || std::abs(media.frameRate - m_projectFrameRate) > 0.001);
    emit statusMessageRequested(differs
        ? QStringLiteral("プロジェクト設定と異なります (%1)").arg(info) : info, 8000);
    return true;
}

void Timeline::setSequences(const QVector<TimelineSequence> &sequences,
                            const QString &activeSequenceId)
{
    QString active = activeSequenceId;
    m_sequences = normalizeSequences(sequences, &active);
    m_sequenceModelEnabled = !m_sequences.isEmpty();
    m_activeSequenceId = m_sequenceModelEnabled ? active : QString();

    const TimelineSequence *sequence = sequenceById(m_activeSequenceId);
    if (!sequence) {
        emit sequencesChanged();
        return;
    }

    m_generatedCaptionOverlays = sequence->generatedCaptionOverlays;
    restoreFromProject(sequence->videoTracks, sequence->audioTracks,
                       m_playheadPos, m_markIn, m_markOut, int(m_zoomLevel));
    m_activeSequenceId = sequence->id;
    m_sequenceModelEnabled = true;
    syncActiveSequenceFromCurrentTracks();
    emit sequencesChanged();
}

bool Timeline::addSequence(const TimelineSequence &sequence)
{
    if (!m_sequenceModelEnabled) {
        m_sequenceModelEnabled = true;
        m_activeSequenceId = QString::fromLatin1(kDefaultSequenceId);
        upsertSequenceSnapshot(currentSequenceSnapshot(m_activeSequenceId,
                                                       QStringLiteral("Sequence 1")));
    }

    TimelineSequence normalized = sequence;
    QVector<TimelineSequence> existing = m_sequences;
    normalized.id = uniqueSequenceId(existing, normalized.id, existing.size());
    if (normalized.name.trimmed().isEmpty())
        normalized.name = QStringLiteral("Sequence %1").arg(existing.size() + 1);
    upsertSequenceSnapshot(normalized);
    emit sequencesChanged();
    return true;
}

bool Timeline::setActiveSequence(const QString &sequenceId)
{
    if (sequenceId.isEmpty())
        return false;
    if (!m_sequenceModelEnabled)
        return sequenceId == QLatin1String(kDefaultSequenceId);
    const TimelineSequence *target = sequenceById(sequenceId);
    if (!target)
        return false;
    if (sequenceId == m_activeSequenceId)
        return true;

    syncActiveSequenceFromCurrentTracks();
    target = sequenceById(sequenceId);
    if (!target)
        return false;

    const TimelineSequence targetCopy = *target;
    m_activeSequenceId = sequenceId;
    clearReconcileBaseline();
    while (m_videoTracks.size() < targetCopy.videoTracks.size())
        addVideoTrack(false);
    while (m_audioTracks.size() < targetCopy.audioTracks.size())
        addAudioTrack(false);

    for (int i = 0; i < m_videoTracks.size(); ++i) {
        if (!m_videoTracks[i])
            continue;
        const QVector<ClipInfo> clips = (i < targetCopy.videoTracks.size())
            ? targetCopy.videoTracks[i]
            : QVector<ClipInfo>{};
        m_videoTracks[i]->setClips(clips);
    }
    for (int i = 0; i < m_audioTracks.size(); ++i) {
        if (!m_audioTracks[i])
            continue;
        const QVector<ClipInfo> clips = (i < targetCopy.audioTracks.size())
            ? targetCopy.audioTracks[i]
            : QVector<ClipInfo>{};
        m_audioTracks[i]->setClips(clips);
    }
    m_generatedCaptionOverlays = targetCopy.generatedCaptionOverlays;

    m_activeSequenceId = sequenceId;
    m_sequenceModelEnabled = true;
    clearAllSelections();
    syncActiveSequenceFromCurrentTracks();
    updateInfoLabel();
    refreshTextStrip();
    ensureSequenceFitsViewport();
    scheduleEmitSequenceChanged();
    rebuildTimelineBreadcrumbBar(this);
    emit sequencesChanged();
    return true;
}

ClipInfo Timeline::makeSequenceClip(const QString &sequenceId,
                                    const QString &displayName) const
{
    ClipInfo clip;
    clip.sequenceRefId = sequenceId;
    clip.filePath = timeline_nesting::sequenceClipFilePath(sequenceId);
    clip.displayName = displayName;

    const QVector<TimelineSequence> snapshot = sequences();
    for (const TimelineSequence &sequence : snapshot) {
        if (sequence.id == sequenceId) {
            if (clip.displayName.isEmpty())
                clip.displayName = sequence.name.isEmpty() ? sequence.id : sequence.name;
            clip.duration = sequence.duration();
            clip.outPoint = clip.duration;
            return clip;
        }
    }

    if (clip.displayName.isEmpty())
        clip.displayName = sequenceId;
    clip.duration = 0.0;
    clip.outPoint = 0.0;
    return clip;
}

bool Timeline::addSequenceClip(const QString &sequenceId, int videoTrackIndex)
{
    if (sequenceById(sequenceId) == nullptr)
        return false;
    const int targetTrackIndex = qMax(0, videoTrackIndex);
    while (m_videoTracks.size() <= targetTrackIndex)
        addVideoTrack(false);
    TimelineTrack *track = m_videoTracks.value(targetTrackIndex, nullptr);
    if (!track)
        return false;

    ClipInfo clip = makeSequenceClip(sequenceId);
    if (clip.duration <= 0.0)
        return false;
    track->addClip(clip);

    while (m_audioTracks.size() <= targetTrackIndex)
        addAudioTrack(false);
    if (TimelineTrack *audioTrack = m_audioTracks.value(targetTrackIndex, nullptr))
        audioTrack->addClip(clip);

    saveUndoState(QStringLiteral("Add sequence clip"));
    scheduleEmitSequenceChanged();
    return true;
}

void Timeline::setClipParentEntries(const QHash<QString, QString> &entries)
{
    m_clipParentEntries.clear();
    bool decodedSequenceStore = false;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (it.key() == timeline_nesting::sequenceStoreParentKey()) {
            QVector<TimelineSequence> sequences;
            QString activeSequenceId;
            if (sequencesFromStoreJson(
                    timeline_nesting::decodeSequenceStoreObject(it.value()),
                    &sequences,
                    &activeSequenceId)) {
                m_sequences = sequences;
                m_activeSequenceId = activeSequenceId;
                m_sequenceModelEnabled = true;
                decodedSequenceStore = true;
            }
            continue;
        }
        if (!it.key().isEmpty() && !it.value().isEmpty() && it.key() != it.value())
            m_clipParentEntries.insert(it.key(), it.value());
    }
    if (!decodedSequenceStore) {
        m_sequences.clear();
        m_activeSequenceId.clear();
        m_sequenceModelEnabled = false;
    }
    rebuildTimelineBreadcrumbBar(this);
    emit sequencesChanged();
}

QHash<QString, QString> Timeline::clipParentEntries() const
{
    QHash<QString, QString> entries = m_clipParentEntries;
    if (m_sequenceModelEnabled) {
        const QString active = m_activeSequenceId.isEmpty()
            ? QString::fromLatin1(kDefaultSequenceId)
            : m_activeSequenceId;
        const QJsonObject store = sequenceStoreToJson(sequences(), active);
        const QString encoded = timeline_nesting::encodeSequenceStoreObject(store);
        if (!encoded.isEmpty())
            entries.insert(timeline_nesting::sequenceStoreParentKey(), encoded);
    }
    return entries;
}

void Timeline::setClipParent(const QString& childKey, const QString& parentKey)
{
    if (childKey.isEmpty())
        return;
    if (parentKey.isEmpty() || childKey == parentKey) {
        m_clipParentEntries.remove(childKey);
        scheduleEmitSequenceChanged();
        return;
    }
    if (m_clipParentEntries.value(childKey) == parentKey)
        return;
    m_clipParentEntries.insert(childKey, parentKey);
    scheduleEmitSequenceChanged();
}

void Timeline::clearClipParent(const QString& childKey)
{
    if (childKey.isEmpty() || !m_clipParentEntries.contains(childKey))
        return;
    m_clipParentEntries.remove(childKey);
    scheduleEmitSequenceChanged();
}

void Timeline::commitTrackMatteEntries(QHash<QString, TimelineTrackMatteEntry> next)
{
    QMutexLocker lock(&m_trackMatteMutex);
    m_trackMatteEntries = std::move(next);
}

void Timeline::setTrackMatteEntries(
    const QHash<QString, TimelineTrackMatteEntry> &entries)
{
    commitTrackMatteEntries(entries);
}

QHash<QString, TimelineTrackMatteEntry> Timeline::trackMatteEntries() const
{
    QMutexLocker lock(&m_trackMatteMutex);
    return m_trackMatteEntries;
}

void Timeline::remapTrackMatteLocked(const TrackClipSnapshot &before)
{
    // The helper mutates its argument in place. Work on a stable snapshot and
    // publish the result through the sole writer after the helper returns.
    auto next = trackMatteEntries();
    remapTimelineCarrierAfterMutation(this, next, before);
    commitTrackMatteEntries(std::move(next));
}

void Timeline::remapCarriersAfterExternalMutation(const TrackClipSnapshot &before)
{
    // The matte and parent helpers also invoke the roto remapper. Its
    // post-mutation memo makes their repeated calls idempotent.
    remapRotoEntriesAfterMutation(before);
    remapTrackMatteLocked(before);
    remapClipParentEntriesAfterMutation(this, m_clipParentEntries, before);
}

void Timeline::commitRotoEntries(QHash<QString, RotoClipEntry> next, bool clearMemo)
{
    QMutexLocker lock(&m_rotoMutex);
    for (auto it = next.begin(); it != next.end(); ++it)
        it.value().clipId = it.key();
    m_rotoEntries = std::move(next);
    if (clearMemo) {
        m_rotoRemapMemoAfter.clear();
        m_rotoRemapMemoValid = false;
        m_lastKeyRemap.reset();
    }
}

static bool sameClipSnapshot(const TrackClipSnapshot &a, const TrackClipSnapshot &b)
{
    if (a.size() != b.size()) return false;
    for (int track = 0; track < a.size(); ++track) {
        if (a[track].size() != b[track].size()) return false;
        for (int clip = 0; clip < a[track].size(); ++clip)
            if (!a[track][clip].sameAs(b[track][clip])) return false;
    }
    return true;
}

void Timeline::notifyClipKeysRemapped(ClipKeyRemap remap)
{
    for (auto it = remap.oldToNew.begin(); it != remap.oldToNew.end();)
        it = it.key() == it.value() ? remap.oldToNew.erase(it) : std::next(it);
    if (remap.oldToNew.isEmpty() && remap.dropped.isEmpty()) {
        // A mutation that moved no key still changes the placement the memo was
        // taken against; keep it only while the snapshot is unchanged.
        if (m_lastKeyRemap && !sameClipSnapshot(snapshotTrackClips(this), m_lastKeyRemap->after))
            m_lastKeyRemap.reset();
        return;
    }
    TrackClipSnapshot after = snapshotTrackClips(this);
    if (m_lastKeyRemap && m_lastKeyRemap->remap == remap
        && sameClipSnapshot(after, m_lastKeyRemap->after))
        return;
    m_lastKeyRemap = LastKeyRemap{std::move(after), remap};
    if (m_keyRemapBatchDepth == 0) {
        emit clipKeysRemapped(remap);
        return;
    }
    // Compose `remap` after the pending one: a pending old -> mid follows mid, and a
    // key no pending pair or drop touched (an identity key) takes the new pair / drop.
    ClipKeyRemap composed;
    QSet<QString> pendingTargets;
    for (auto it = m_pendingKeyRemap.oldToNew.cbegin(); it != m_pendingKeyRemap.oldToNew.cend(); ++it) {
        pendingTargets.insert(it.value());
        const auto to = remap.oldToNew.constFind(it.value());
        if (to != remap.oldToNew.cend())
            composed.oldToNew.insert(it.key(), to.value());
        else if (remap.dropped.contains(it.value()))
            composed.dropped.insert(it.key());
        else
            composed.oldToNew.insert(it.key(), it.value());
    }
    composed.dropped.unite(m_pendingKeyRemap.dropped);
    auto identityKey = [&](const QString &key) {
        return !pendingTargets.contains(key) && !m_pendingKeyRemap.oldToNew.contains(key)
            && !m_pendingKeyRemap.dropped.contains(key);
    };
    for (auto it = remap.oldToNew.cbegin(); it != remap.oldToNew.cend(); ++it)
        if (identityKey(it.key())) composed.oldToNew.insert(it.key(), it.value());
    for (const QString &key : std::as_const(remap.dropped))
        if (identityKey(key)) composed.dropped.insert(key);
    for (auto it = composed.oldToNew.begin(); it != composed.oldToNew.end();)
        it = it.key() == it.value() ? composed.oldToNew.erase(it) : std::next(it);
    m_pendingKeyRemap = std::move(composed);
}

void Timeline::flushPendingKeyRemap()
{
    if (m_keyRemapBatchDepth == 0) return;
    ClipKeyRemap remap = std::exchange(m_pendingKeyRemap, ClipKeyRemap{});
    if (!remap.oldToNew.isEmpty() || !remap.dropped.isEmpty())
        emit clipKeysRemapped(remap);
}

Timeline::ClipKeyRemapBatch::~ClipKeyRemapBatch()
{
    if (--m_timeline->m_keyRemapBatchDepth > 0) return;
    ClipKeyRemap remap = std::exchange(m_timeline->m_pendingKeyRemap, ClipKeyRemap{});
    if (!remap.oldToNew.isEmpty() || !remap.dropped.isEmpty())
        emit m_timeline->clipKeysRemapped(remap);
}

void Timeline::setRotoEntries(const QHash<QString, RotoClipEntry> &entries)
{
    commitRotoEntries(entries, true);
}

QHash<QString, RotoClipEntry> Timeline::rotoEntries() const
{
    QMutexLocker lock(&m_rotoMutex);
    return m_rotoEntries;
}

void Timeline::remapRotoEntriesAfterMutation(const TrackClipSnapshot &before)
{
    // US-1303: the key map is announced with or without roto entries; a key of
    // `before` the walk did not bind belongs to a clip that is gone.
    const auto oldToNew = alignedClipKeyMap(this, before);
    ClipKeyRemap remap;
    remap.oldToNew = oldToNew;
    for (int t = 0; t < before.size(); ++t)
        for (int i = 0; i < before[t].size(); ++i)
            if (!oldToNew.contains(trackMatteClipKey(t, i)))
                remap.dropped.insert(trackMatteClipKey(t, i));
    notifyClipKeysRemapped(std::move(remap));
    if (rotoEntries().isEmpty()) return;
    const TrackClipSnapshot after = snapshotTrackClips(this);
    {
        QMutexLocker lock(&m_rotoMutex);
        if (m_rotoRemapMemoValid && sameClipSnapshot(after, m_rotoRemapMemoAfter))
            return;
    }
    const auto entries = rotoEntries();
    QHash<QString, RotoClipEntry> rebuilt;
    rebuilt.reserve(entries.size());
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const auto key = oldToNew.constFind(it.key());
        if (key != oldToNew.cend()) rebuilt.insert(key.value(), it.value());
    }
    commitRotoEntries(std::move(rebuilt), false);
    QMutexLocker lock(&m_rotoMutex);
    m_rotoRemapMemoAfter = after;
    m_rotoRemapMemoValid = true;
}

TimelineState Timeline::currentState() const
{
    TimelineState state;
    state.videoTracks.reserve(m_videoTracks.size());
    for (const auto *t : m_videoTracks)
        state.videoTracks.append(t ? t->clips() : QVector<ClipInfo>{});
    state.audioTracks.reserve(m_audioTracks.size());
    for (const auto *t : m_audioTracks)
        state.audioTracks.append(t ? t->clips() : QVector<ClipInfo>{});
    for (const auto *track : m_videoTracks) {
        state.videoTrackNames.append(track ? track->customName : QString{});
        state.videoTrackColors.append(track ? track->color : QColor{});
    }
    for (const auto *track : m_audioTracks) {
        state.audioTrackNames.append(track ? track->customName : QString{});
        state.audioTrackColors.append(track ? track->color : QColor{});
    }
    state.generatedCaptionOverlays = m_generatedCaptionOverlays;
    state.selectedClip = m_videoTrack->selectedClip();

    for (int i = 0; i < m_videoTracks.size(); ++i) {
        if (!m_videoTracks[i]) continue;
        const int sel = m_videoTracks[i]->selectedClip();
        if (sel >= 0) {
            state.selectedVideoTrackIndex = i;
            state.selectedVideoClipIndex = sel;
            break;
        }
    }
    for (int i = 0; i < m_audioTracks.size(); ++i) {
        if (!m_audioTracks[i]) continue;
        const int sel = m_audioTracks[i]->selectedClip();
        if (sel >= 0) {
            state.selectedAudioTrackIndex = i;
            state.selectedAudioClipIndex = sel;
            break;
        }
    }

    state.activeVideoTrackIndex = m_activeVideoTrackIndex;
    state.activeAudioTrackIndex = m_activeAudioTrackIndex;
    state.playheadPos = m_playheadPos;
    state.clipParentEntries = clipParentEntries();
    state.trackMatteEntries = trackMatteEntries();
    state.rotoEntries = rotoEntries();
    state.trackStructureRevision = m_trackStructureRevision;

    if (m_audioMixer) {
        const int n = audioTrackCount();
        state.audioTrackGains.resize(n);
        state.audioTrackPans.resize(n);
        for (int i = 0; i < n; ++i) {
            state.audioTrackGains[i] = m_audioMixer->trackGain(i);
            state.audioTrackPans[i] = m_audioMixer->trackPan(i);
        }
    }

    if (m_collectExternalTrackState) state.externalTrackState = m_collectExternalTrackState();
    if (m_busRoutingProvider) state.audioBusRouting = m_busRoutingProvider();
    if (m_sidecarStateCollector) state.sidecarState = m_sidecarStateCollector();
    state.projectWidth = m_projectWidth;
    state.projectHeight = m_projectHeight;
    state.projectExplicitOutput = m_projectExplicitOutput;

    return state;
}

void Timeline::restoreState(const TimelineState &state)
{
    undotrace::log("restoreState:enter");
    noteClipsChanged();
    const bool structureChanged = m_trackStructureRevision != state.trackStructureRevision
        || m_videoTracks.size() != state.videoTracks.size()
        || m_audioTracks.size() != state.audioTracks.size();
    // US-1302: sidecars follow on every restore, structural or not, while the
    // clips they are keyed on are still the ones being left.
    if (m_sidecarStateApplier && !state.sidecarState.isEmpty())
        m_sidecarStateApplier(state.sidecarState);
    // Deferred deletion keeps menu callbacks safe; remap receivers clear retained pointers.
    while (m_videoTracks.size() > qMax(1, int(state.videoTracks.size())))
        removeTrackInternal(false, m_videoTracks.size() - 1);
    while (m_audioTracks.size() > qMax(1, int(state.audioTracks.size())))
        removeTrackInternal(true, m_audioTracks.size() - 1);
    while (m_videoTracks.size() < state.videoTracks.size()) addVideoTrack(false);
    while (m_audioTracks.size() < state.audioTracks.size()) addAudioTrack(false);

    for (int i = 0; i < m_videoTracks.size(); ++i) {
        if (!m_videoTracks[i]) continue;
        const auto &clips = (i < state.videoTracks.size())
                            ? state.videoTracks[i]
                            : QVector<ClipInfo>{};
        m_videoTracks[i]->setClips(clips);
        m_videoTracks[i]->customName = state.videoTrackNames.value(i);
        m_videoTracks[i]->color = state.videoTrackColors.value(i);
        syncTrackHeaderFlags(m_videoTracks[i]);
        m_videoTracks[i]->update();
    }
    for (int i = 0; i < m_audioTracks.size(); ++i) {
        if (!m_audioTracks[i]) continue;
        const auto &clips = (i < state.audioTracks.size())
                            ? state.audioTracks[i]
                            : QVector<ClipInfo>{};
        m_audioTracks[i]->setClips(clips);
        m_audioTracks[i]->customName = state.audioTrackNames.value(i);
        m_audioTracks[i]->color = state.audioTrackColors.value(i);
        syncTrackHeaderFlags(m_audioTracks[i]);
        m_audioTracks[i]->update();
    }
    m_generatedCaptionOverlays = state.generatedCaptionOverlays;
    commitTrackMatteEntries(state.trackMatteEntries);
    commitRotoEntries(state.rotoEntries, true);
    setClipParentEntries(state.clipParentEntries);
    rebuildTimelineBreadcrumbBar(this);

    // Clear every track's UI selection (via blockSignals so we don't emit
    // a cascade of intermediate selectionChanged signals from stale tracks).
    // Then restore the selection on the correct track, letting signals
    // through so wireTrackSelection can sync linked clips and emit
    // clipSelected/clipSelectedOnTrack.
    clearAllSelections();
    m_activeVideoTrackIndex = -1;

    const bool legacySelectionState =
        state.selectedVideoTrackIndex < 0
        && state.selectedVideoClipIndex < 0
        && state.selectedAudioTrackIndex < 0
        && state.selectedAudioClipIndex < 0
        && state.selectedClip >= 0;

    bool videoSelSet = false;
    if (state.selectedVideoTrackIndex >= 0
        && state.selectedVideoTrackIndex < m_videoTracks.size()) {
        TimelineTrack *vt = m_videoTracks[state.selectedVideoTrackIndex];
        if (vt && state.selectedVideoClipIndex >= 0
            && state.selectedVideoClipIndex < vt->clipCount()) {
            vt->setSelectedClip(state.selectedVideoClipIndex);
            videoSelSet = true;
        }
    }
    // Restore explicit "no video selection" snapshots too; only mirror V1 to
    // A1 for genuinely legacy snapshots that predate the track-aware fields.
    if (!videoSelSet && (legacySelectionState || state.selectedVideoTrackIndex < 0)) {
        const bool vWas = m_videoTrack->blockSignals(true);
        const int selected = state.selectedClip >= 0 && state.selectedClip < m_videoTrack->clipCount()
            ? state.selectedClip : -1;
        m_videoTrack->setSelectedClip(selected);
        m_videoTrack->blockSignals(vWas);
        emit clipSelected(selected);
        m_activeVideoTrackIndex = selected < 0 ? -1 : 0;
        emit clipSelectedOnTrack(m_activeVideoTrackIndex, selected);
    }

    if (state.selectedAudioTrackIndex >= 0
        && state.selectedAudioTrackIndex < m_audioTracks.size()) {
        TimelineTrack *at = m_audioTracks[state.selectedAudioTrackIndex];
        if (at && state.selectedAudioClipIndex >= 0
            && state.selectedAudioClipIndex < at->clipCount()) {
            // Block signals so the audio track restore doesn't re-emit
            // clipSelected/clipSelectedOnTrack (the video path or legacy
            // fallback already handled those).
            const bool aWas = at->blockSignals(true);
            at->setSelectedClip(state.selectedAudioClipIndex);
            at->blockSignals(aWas);
        }
    } else if (legacySelectionState) {
        // Legacy fallback: old undo entries predating the V2 track-aware
        // fields restore selection on A1 the same as V1.
        const bool aWas = m_audioTrack->blockSignals(true);
        m_audioTrack->setSelectedClip(state.selectedClip >= 0 && state.selectedClip < m_audioTrack->clipCount()
            ? state.selectedClip : -1);
        m_audioTrack->blockSignals(aWas);
    }

    // Preserve the active row independently of the first selected row.
    if (state.activeVideoTrackIndex >= 0 && state.activeVideoTrackIndex < m_videoTracks.size())
        m_activeVideoTrackIndex = state.activeVideoTrackIndex;
    m_activeAudioTrackIndex = state.activeAudioTrackIndex >= 0
        && state.activeAudioTrackIndex < m_audioTracks.size() ? state.activeAudioTrackIndex : -1;

    m_playheadPos = state.playheadPos;
    syncPlayheadOverlay();

    if (m_audioMixer && !state.audioTrackGains.isEmpty()) {
        const int n = qMin(state.audioTrackGains.size(), audioTrackCount());
        for (int i = 0; i < n; ++i)
            m_audioMixer->setTrackGain(i, state.audioTrackGains[i]);
    }
    if (m_audioMixer && !state.audioTrackPans.isEmpty()) {
        const int n = qMin(state.audioTrackPans.size(), audioTrackCount());
        for (int i = 0; i < n; ++i)
            m_audioMixer->setTrackPan(i, state.audioTrackPans[i]);
    }

    // スナップショットに捕捉されたプロジェクト出力ジオメトリ(サイズ)を復元する。
    // SNS プリセットのリサイズを undo したとき、プロジェクトサイズも一緒に戻して
    // 縦伸びを断つ。projectWidth <= 0 のレガシー/空スナップショットはスキップ
    // (現在サイズを壊さない)。scheduleEmitSequenceChanged の前に行い、サイズ更新後の
    // シーケンス再構築でプレビューが新サイズで合成されるようにする。
    if (state.projectWidth > 0 && state.projectHeight > 0) {
        m_projectWidth = state.projectWidth;
        m_projectHeight = state.projectHeight;
        m_projectExplicitOutput = state.projectExplicitOutput;
        emit projectOutputConfigRestored(state.projectWidth, state.projectHeight,
                                         state.projectExplicitOutput);
    }

    // setClips bypasses the modified() signal path; trigger explicitly so the
    // VideoPlayer rebuilds its sequence after undo/redo.
    refreshTextStrip();
    scheduleEmitSequenceChanged();
    m_trackStructureRevision = state.trackStructureRevision;
    m_nextTrackStructureRevision = qMax(m_nextTrackStructureRevision, m_trackStructureRevision);
    // Playback flags and mixer edits are not ordinary timeline undo state.
    // Restore external owners only when crossing a track-structure change.
    if (structureChanged && m_applyExternalTrackState)
        m_applyExternalTrackState(state.externalTrackState);
    // Bus routing is ordinary undo state: applied on every restore, after the
    // external "buses" capture above (same undo point), so this one wins.
    if (m_busRoutingApplier && !state.audioBusRouting.isEmpty())
        m_busRoutingApplier(state.audioBusRouting);
    emit trackStateRestored();
    syncActiveSequenceFromCurrentTracks();
    // The restored entry was saved after its reconcile: it is the baseline.
    // Restoring back across a sequence creation switches and empties it below.
    m_pendingDeletedUids.clear();
    m_pendingSplits.clear();
    m_pendingClipSplits.clear();
    m_pendingDetachedSpans.clear();
    captureReconcileBaseline();
    const QString beforeSequenceCreation = state.externalTrackState
        .value(QStringLiteral("_sequenceCreationActiveId")).toString();
    if (!beforeSequenceCreation.isEmpty()) setActiveSequence(beforeSequenceCreation);
    undotrace::log("restoreState:exit");
}

// --- Project save/load ---

bool Timeline::setTrackLocked(TrackKind kind, int trackIndex, bool locked)
{
    TimelineTrack *track = trackAt(kind == TrackKind::Audio, trackIndex);
    if (!track)
        return false;
    track->setLocked(locked);
    syncTrackHeaderFlags(track);
    return true;
}

bool Timeline::setTrackAppearance(bool audio, int idx, const QString &name,
                                  const QColor &color, QString *err)
{
    TimelineTrack *track = trackAt(audio, idx);
    if (!track) {
        if (err) *err = QStringLiteral("トラック番号が範囲外です");
        return false;
    }
    const QColor normalized = color.isValid() ? QColor(color.name()) : QColor{};
    if (track->customName == name && track->color == normalized) return true;
    // No clip mutation: clip identity/remapping is unchanged.
    track->customName = name;
    track->color = normalized;
    syncTrackHeaderFlags(track);
    saveUndoState(QStringLiteral("トラックの名前・色を変更"));
    scheduleEmitSequenceChanged();
    return true;
}

bool Timeline::setTrackCustomName(bool audio, int idx, const QString &name, QString *err)
{
    const auto *track = trackAt(audio, idx);
    return setTrackAppearance(audio, idx, name, track ? track->color : QColor{}, err);
}

bool Timeline::setTrackColor(bool audio, int idx, const QColor &color, QString *err)
{
    const auto *track = trackAt(audio, idx);
    return setTrackAppearance(audio, idx, track ? track->customName : QString{}, color, err);
}

QJsonObject Timeline::trackFlagsToJson() const
{
    auto flagsForTracks = [](const QVector<TimelineTrack *> &tracks) {
        QJsonArray result;
        for (const TimelineTrack *track : tracks) {
            QJsonObject flags;
            if (track) {
                if (!track->customName.isEmpty()) flags.insert(QStringLiteral("name"), track->customName);
                if (track->color.isValid()) flags.insert(QStringLiteral("color"), track->color.name());
                const bool locked = track->isLocked();
                const bool muted = track->isMuted();
                const bool solo = track->isSolo();
                const bool hidden = track->isHidden();
                if (locked || muted || solo || hidden) {
                    flags.insert(QStringLiteral("locked"), locked);
                    flags.insert(QStringLiteral("muted"), muted);
                    flags.insert(QStringLiteral("solo"), solo);
                    flags.insert(QStringLiteral("hidden"), hidden);
                }
            }
            result.append(flags);
        }
        return result;
    };

    return QJsonObject{
        {QStringLiteral("video"), flagsForTracks(m_videoTracks)},
        {QStringLiteral("audio"), flagsForTracks(m_audioTracks)}
    };
}

void Timeline::applyTrackFlagsFromJson(const QJsonObject &flags)
{
    bool playbackFlagsChanged = false;
    auto applyFlags = [this, &playbackFlagsChanged](
                          const QVector<TimelineTrack *> &tracks,
                          const QJsonArray &items, bool audio) {
        for (int index = 0; index < tracks.size(); ++index) {
            TimelineTrack *track = tracks.at(index);
            if (!track)
                continue;
            const QJsonObject item = index < items.size()
                ? items.at(index).toObject() : QJsonObject{};
            const bool locked = item.value(QStringLiteral("locked")).toBool(false);
            const bool muted = item.value(QStringLiteral("muted")).toBool(false);
            const bool solo = item.value(QStringLiteral("solo")).toBool(false);
            const bool hidden = item.value(QStringLiteral("hidden")).toBool(false);
            const bool soloChanged = track->isSolo() != solo;
            playbackFlagsChanged = playbackFlagsChanged
                || track->isMuted() != muted || track->isHidden() != hidden || soloChanged;
            track->customName = item.value(QStringLiteral("name")).toString();
            track->color = QColor(item.value(QStringLiteral("color")).toString());
            track->setLocked(locked);
            track->setMuted(muted);
            track->setSolo(solo);
            track->setHidden(hidden);
            syncTrackHeaderFlags(track);
            if (audio && soloChanged)
                emit trackSoloChanged(index, solo);
        }
    };

    applyFlags(m_videoTracks,
               flags.value(QStringLiteral("video")).toArray(), false);
    applyFlags(m_audioTracks,
               flags.value(QStringLiteral("audio")).toArray(), true);
    if (playbackFlagsChanged)
        scheduleEmitSequenceChanged();
    updateInfoLabel();
}

ProjectTrackClips Timeline::allVideoTracks() const
{
    ProjectTrackClips result;
    for (const auto *t : m_videoTracks)
        result.append(t->clips());
    result.trackFlagsSnapshot = trackFlagsToJson();
    return result;
}

ProjectTrackClips Timeline::allAudioTracks() const
{
    ProjectTrackClips result;
    for (const auto *t : m_audioTracks)
        result.append(t->clips());
    result.trackFlagsSnapshot = trackFlagsToJson();
    return result;
}

void Timeline::restoreFromProject(const QVector<QVector<ClipInfo>> &videoTracks,
                                   const QVector<QVector<ClipInfo>> &audioTracks,
                                   double playhead, double markInVal, double markOutVal, int zoom)
{
    // Grow to fit the incoming track counts, then rewrite EVERY existing
    // track. Tracks beyond the incoming set are cleared (empty clips) so
    // leftovers from a previously loaded project don't linger.
    clearReconcileBaseline();
    while (m_videoTracks.size() < videoTracks.size()) addVideoTrack(false);
    while (m_audioTracks.size() < audioTracks.size()) addAudioTrack(false);

    // Video uids are repaired across tracks (the first track keeps its uid);
    // audio tracks are left alone.
    QSet<QString> videoSeenUids;
    g_loadVideoSeenUids = &videoSeenUids;
    for (int i = 0; i < m_videoTracks.size(); ++i) {
        if (!m_videoTracks[i]) continue;
        const auto &clips = (i < videoTracks.size())
                            ? videoTracks[i]
                            : QVector<ClipInfo>{};
        m_videoTracks[i]->setClips(clips);
    }
    g_loadVideoSeenUids = nullptr;

    for (int i = 0; i < m_audioTracks.size(); ++i) {
        if (!m_audioTracks[i]) continue;
        const auto &clips = (i < audioTracks.size())
                            ? audioTracks[i]
                            : QVector<ClipInfo>{};
        m_audioTracks[i]->setClips(clips);
    }
    if (m_sequenceModelEnabled)
        syncActiveSequenceFromCurrentTracks();

    m_playheadPos = playhead;
    m_markIn = markInVal;
    m_markOut = markOutVal;
    setZoomLevel(zoom);
    syncPlayheadOverlay();
    saveUndoState("Load project");
    updateInfoLabel();
    refreshTextStrip();
    // setClips bypasses modified(); trigger sequence rebuild explicitly.
    scheduleEmitSequenceChanged();
}

void Timeline::restoreFromProject(const ProjectTrackClips &videoTracks,
                                  const ProjectTrackClips &audioTracks,
                                  double playhead, double markInVal,
                                  double markOutVal, int zoom)
{
    QJsonObject flags = videoTracks.trackFlagsSnapshot;
    if (flags.isEmpty())
        flags = audioTracks.trackFlagsSnapshot;
    // Capture the loaded appearance in the load undo snapshot as well.
    while (m_videoTracks.size() < videoTracks.size()) addVideoTrack(false);
    while (m_audioTracks.size() < audioTracks.size()) addAudioTrack(false);
    applyTrackFlagsFromJson(flags);
    restoreFromProject(static_cast<const QVector<QVector<ClipInfo>> &>(videoTracks),
                       static_cast<const QVector<QVector<ClipInfo>> &>(audioTracks),
                       playhead, markInVal, markOutVal, zoom);
}

// --- Timeline markers (Premiere Pro / DaVinci Resolve parity) ---
// Markers are stored sorted by timelineUs so navigation is O(log N) and
// lane painting is left-to-right without per-paint sorting. Mutations emit
// markersChanged() so any future panel UI re-syncs without manual hooks.

namespace {
// Insert `m` into the sorted vector by timelineUs. Stable for equal stamps.
inline void insertMarkerSorted(QVector<Marker> &vec, const Marker &m)
{
    auto it = std::lower_bound(vec.begin(), vec.end(), m.timelineUs,
        [](const Marker &lhs, qint64 rhs) { return lhs.timelineUs < rhs; });
    vec.insert(it, m);
}
} // namespace

int Timeline::addMarker(qint64 timelineUs, const QString &label, QColor color)
{
    Marker m;
    m.id = m_nextMarkerId++;
    m.timelineUs = qMax<qint64>(0, timelineUs);
    m.label = label;
    m.color = color.isValid() ? color : QColor(QStringLiteral("#ff5050"));
    insertMarkerSorted(m_markersData, m);
    if (m_markerLane) m_markerLane->update();
    emit markersChanged();
    return m.id;
}

bool Timeline::removeMarker(int id)
{
    for (int i = 0; i < m_markersData.size(); ++i) {
        if (m_markersData[i].id == id) {
            m_markersData.removeAt(i);
            if (m_markerLane) m_markerLane->update();
            emit markersChanged();
            return true;
        }
    }
    return false;
}

bool Timeline::updateMarker(int id, const Marker &updated)
{
    for (int i = 0; i < m_markersData.size(); ++i) {
        if (m_markersData[i].id == id) {
            // Preserve the original id even if `updated.id` was 0 / unset
            // (callers shouldn't have to thread the id back through).
            Marker copy = updated;
            copy.id = id;
            copy.timelineUs = qMax<qint64>(0, copy.timelineUs);
            copy.durationUs = qMax<qint64>(0, copy.durationUs);
            if (!copy.color.isValid())
                copy.color = QColor(QStringLiteral("#ff5050"));
            // Re-sort if the time changed, otherwise just overwrite in place.
            if (copy.timelineUs == m_markersData[i].timelineUs) {
                m_markersData[i] = copy;
            } else {
                m_markersData.removeAt(i);
                insertMarkerSorted(m_markersData, copy);
            }
            if (m_markerLane) m_markerLane->update();
            emit markersChanged();
            return true;
        }
    }
    return false;
}

void Timeline::setMarkerDuration(int markerId, qint64 durationUs)
{
    // MK-1: clamp negatives to 0 (point marker). The span never moves the
    // marker's start, so no re-sort is needed — sort order is by timelineUs.
    const qint64 dur = qMax<qint64>(0, durationUs);
    for (int i = 0; i < m_markersData.size(); ++i) {
        if (m_markersData[i].id == markerId) {
            if (m_markersData[i].durationUs == dur)
                return;  // no change, avoid spurious repaint/signal
            m_markersData[i].durationUs = dur;
            if (m_markerLane) m_markerLane->update();
            emit markersChanged();
            return;
        }
    }
}

Marker Timeline::markerById(int id) const
{
    for (const auto &m : m_markersData)
        if (m.id == id) return m;
    return Marker{}; // default-constructed (id == -1) on miss
}

QVector<Marker> Timeline::markersInRange(qint64 startUs, qint64 endUs) const
{
    QVector<Marker> result;
    if (endUs < startUs) std::swap(startUs, endUs);
    // Markers are sorted; binary-search the lower bound for cheap range queries.
    auto lo = std::lower_bound(m_markersData.begin(), m_markersData.end(), startUs,
        [](const Marker &m, qint64 v) { return m.timelineUs < v; });
    auto hi = std::upper_bound(m_markersData.begin(), m_markersData.end(), endUs,
        [](qint64 v, const Marker &m) { return v < m.timelineUs; });
    for (auto it = lo; it != hi; ++it)
        result.append(*it);
    return result;
}

int Timeline::nextMarkerAfter(qint64 timelineUs) const
{
    auto it = std::upper_bound(m_markersData.begin(), m_markersData.end(), timelineUs,
        [](qint64 v, const Marker &m) { return v < m.timelineUs; });
    if (it == m_markersData.end()) return -1;
    return it->id;
}

int Timeline::prevMarkerBefore(qint64 timelineUs) const
{
    auto it = std::lower_bound(m_markersData.begin(), m_markersData.end(), timelineUs,
        [](const Marker &m, qint64 v) { return m.timelineUs < v; });
    if (it == m_markersData.begin()) return -1;
    --it;
    return it->id;
}

void Timeline::setMarkers(const QVector<Marker> &markers)
{
    m_markersData = markers;
    for (Marker &m : m_markersData) {
        m.timelineUs = qMax<qint64>(0, m.timelineUs);
        m.durationUs = qMax<qint64>(0, m.durationUs);
        if (!m.color.isValid())
            m.color = QColor(QStringLiteral("#ff5050"));
    }
    std::sort(m_markersData.begin(), m_markersData.end(),
        [](const Marker &a, const Marker &b) { return a.timelineUs < b.timelineUs; });
    // Restore the monotonic id counter so newly-added markers don't collide
    // with serialized ids from a loaded project.
    int maxId = 0;
    for (const auto &m : m_markersData) maxId = qMax(maxId, m.id);
    m_nextMarkerId = maxId + 1;
    if (m_markerLane) m_markerLane->update();
    emit markersChanged();
}

// --- Adjustment layers (Premiere/Photoshop parity) ---
// CRUD methods mirror the marker pattern above. Stored unsorted; the
// composeAdjustmentLayersAt() helper handles trackIndex stacking at
// render time. Id counter is monotonic and resets on setAdjustmentLayers().

int Timeline::addAdjustmentLayer(const AdjustmentLayer &layer)
{
    AdjustmentLayer copy = layer;
    copy.id = m_nextAdjustmentLayerId++;
    // Defensive: clamp negative timestamps and keep start <= end.
    copy.timelineStartUs = qMax<qint64>(0, copy.timelineStartUs);
    copy.timelineEndUs   = qMax<qint64>(copy.timelineStartUs, copy.timelineEndUs);
    const int maxTrackIndex = qMax(0, static_cast<int>(m_videoTracks.size()) - 1);
    copy.trackIndex = qBound(0, copy.trackIndex, maxTrackIndex);
    m_adjustmentLayers.append(copy);
    for (auto *track : m_videoTracks)
        if (track) track->setPixelsPerSecond(m_zoomLevel);
    emit adjustmentLayersChanged();
    scheduleEmitSequenceChanged();
    return copy.id;
}

bool Timeline::removeAdjustmentLayer(int id)
{
    for (int i = 0; i < m_adjustmentLayers.size(); ++i) {
        if (m_adjustmentLayers[i].id == id) {
            m_adjustmentLayers.removeAt(i);
            for (auto *track : m_videoTracks)
                if (track) track->setPixelsPerSecond(m_zoomLevel);
            emit adjustmentLayersChanged();
            scheduleEmitSequenceChanged();
            return true;
        }
    }
    return false;
}

bool Timeline::updateAdjustmentLayer(int id, const AdjustmentLayer &layer)
{
    for (int i = 0; i < m_adjustmentLayers.size(); ++i) {
        if (m_adjustmentLayers[i].id == id) {
            // Preserve the original id even if `layer.id` was 0 / unset.
            AdjustmentLayer copy = layer;
            copy.id = id;
            copy.timelineStartUs = qMax<qint64>(0, copy.timelineStartUs);
            copy.timelineEndUs   = qMax<qint64>(copy.timelineStartUs, copy.timelineEndUs);
            const int maxTrackIndex = qMax(0, static_cast<int>(m_videoTracks.size()) - 1);
            copy.trackIndex = qBound(0, copy.trackIndex, maxTrackIndex);
            m_adjustmentLayers[i] = copy;
            for (auto *track : m_videoTracks)
                if (track) track->setPixelsPerSecond(m_zoomLevel);
            emit adjustmentLayersChanged();
            scheduleEmitSequenceChanged();
            return true;
        }
    }
    return false;
}

AdjustmentLayer Timeline::adjustmentLayerById(int id) const
{
    for (const auto &l : m_adjustmentLayers)
        if (l.id == id) return l;
    return AdjustmentLayer{}; // default-constructed (id == -1) on miss
}

void Timeline::setAdjustmentLayers(const QVector<AdjustmentLayer> &layers)
{
    m_adjustmentLayers = layers;
    // Restore the monotonic id counter so newly-added layers don't collide
    // with serialized ids from a loaded project.
    int maxId = 0;
    const int maxTrackIndex = qMax(0, static_cast<int>(m_videoTracks.size()) - 1);
    for (AdjustmentLayer &l : m_adjustmentLayers) {
        l.timelineStartUs = qMax<qint64>(0, l.timelineStartUs);
        l.timelineEndUs = qMax<qint64>(l.timelineStartUs, l.timelineEndUs);
        l.trackIndex = qBound(0, l.trackIndex, maxTrackIndex);
        maxId = qMax(maxId, l.id);
    }
    m_nextAdjustmentLayerId = maxId + 1;
    for (auto *track : m_videoTracks)
        if (track) track->setPixelsPerSecond(m_zoomLevel);
    emit adjustmentLayersChanged();
    scheduleEmitSequenceChanged();
}

void Timeline::onPlayheadAutoScrollTick()
{
    // Drives auto-scroll while the user drags the playhead bar with the
    // cursor held in the outer 15% of the viewport. The bar visually pins
    // at the 15% / 85% boundary; the timeline scrolls under it, advancing
    // m_playheadPos so the user keeps scrubbing without losing sight of
    // the bar.
    if (!m_playheadDragging) {
        if (m_playheadAutoScrollTimer)
            m_playheadAutoScrollTimer->stop();
        return;
    }
    if (!m_scrollArea || !m_scrollArea->viewport() || !m_videoTrack)
        return;
    QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
    if (!hbar)
        return;

    const int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 0)
        return;
    const int scrollX = hbar->value();
    const int leftZone = static_cast<int>(viewportW * 0.25);
    const int rightZone = static_cast<int>(viewportW * 0.75);

    int delta = 0;
    if (m_playheadDragViewportX < leftZone) {
        const int overshoot = leftZone - m_playheadDragViewportX;
        delta = -qBound(2, overshoot / 2, 80);
    } else if (m_playheadDragViewportX > rightZone) {
        const int overshoot = m_playheadDragViewportX - rightZone;
        delta = qBound(2, overshoot / 2, 80);
    } else {
        return; // cursor inside the central 70% free zone — no auto-scroll
    }

    const int newScrollX = qBound(hbar->minimum(), scrollX + delta, hbar->maximum());
    if (newScrollX == scrollX)
        return; // already at the rail; nothing more to scroll

    hbar->setValue(newScrollX);

    const int boundary = (delta < 0) ? leftZone : rightZone;
    const int barX = newScrollX + boundary;
    m_playheadPos = m_videoTrack->xToSeconds(barX);
    if (m_playheadOverlay)
        m_playheadOverlay->setPlayheadX(barX);
    emit scrubPositionChanged(m_playheadPos);
}

void Timeline::syncPlayheadOverlay()
{
    if (!m_playheadOverlay || !m_videoTrack)
        return;

    const int playheadX = m_videoTrack->secondsToX(m_playheadPos);
    m_playheadOverlay->setPlayheadX(playheadX);

    if (m_scrollArea) {
        QScrollBar *hbar = m_scrollArea->horizontalScrollBar();
        if (hbar) {
            const int viewportW = m_scrollArea->viewport() ? m_scrollArea->viewport()->width() : 0;
            const int scrollX = hbar->value();
            const int margin = qMax(40, viewportW / 10);
            if (viewportW > 0 && (playheadX < scrollX + margin || playheadX > scrollX + viewportW - margin)) {
                int target = playheadX - viewportW / 2;
                target = qBound(hbar->minimum(), target, hbar->maximum());
                hbar->setValue(target);
            }
        }
    }
}

void Timeline::updateInfoLabel()
{
    QString info = QString("Timeline - %1 clip(s) | Zoom %2 pps | %3")
        .arg(m_videoTrack->clipCount())
        .arg(m_zoomLevel, 0, 'f', 2)
        .arg(snapEnabled() ? "Snap ON" : "Snap OFF");
    if (m_videoTracks.size() > 1 || m_audioTracks.size() > 1)
        info += QString(" | V%1 A%2").arg(m_videoTracks.size()).arg(m_audioTracks.size());
    if (hasMarkedRange())
        info += QString(" | I/O: %1s-%2s").arg(m_markIn, 0, 'f', 1).arg(m_markOut, 0, 'f', 1);
    m_infoLabel->setText(info);
}

// US-INT-4: V1-only writer. Mutates the underlying TimelineTrack clip
// vector via clips()/setClips() so the change participates in the normal
// invalidation/repaint flow.
void Timeline::setClipStabilizerKeyframes(int clipIndex,
                                          const QVector<StabilizerKeyframe> &kfs)
{
    if (m_videoTracks.isEmpty() || !m_videoTracks.first())
        return;
    auto *track = m_videoTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clipIndex < 0 || clipIndex >= clips.size())
        return;
    clips[clipIndex].stabilizerKeyframes = kfs;
    track->setClips(clips);
    track->update();
    scheduleEmitSequenceChanged();
}
