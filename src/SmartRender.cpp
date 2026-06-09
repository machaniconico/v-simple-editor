#include "SmartRender.h"

#include "ClipGeometry.h"
#include "TrackMatteKey.h"
#include "libavcore/Decode.h"
#include "libavcore/Probe.h"

#include <QFileInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QtGlobal>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace {

constexpr double kEpsilon = 0.000001;

struct SourceSignature {
    bool present = false;
    QString codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    QString pixFmt;
    bool hasAudio = false;
    QString audioCodec;
};

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) <= kEpsilon;
}

QString normalizedCodecFamily(QString codec)
{
    codec = codec.trimmed().toLower();
    if (codec.isEmpty())
        return QString();

    if (codec == QLatin1String("libx264")
        || codec == QLatin1String("h264_nvenc")
        || codec == QLatin1String("h264_qsv")
        || codec == QLatin1String("h264_amf")
        || codec == QLatin1String("h264_mf")
        || codec == QLatin1String("avc")
        || codec == QLatin1String("avc1"))
        return QStringLiteral("h264");

    if (codec == QLatin1String("libx265")
        || codec == QLatin1String("hevc_nvenc")
        || codec == QLatin1String("hevc_qsv")
        || codec == QLatin1String("hevc_amf")
        || codec == QLatin1String("hevc_mf")
        || codec == QLatin1String("h265"))
        return QStringLiteral("hevc");

    if (codec == QLatin1String("libsvtav1")
        || codec == QLatin1String("libaom-av1"))
        return QStringLiteral("av1");

    if (codec == QLatin1String("libvpx-vp9"))
        return QStringLiteral("vp9");

    if (codec == QLatin1String("prores_ks"))
        return QStringLiteral("prores");

    return codec;
}

QString expectedOutputPixFmt(const QString& outputCodec)
{
    const QString family = normalizedCodecFamily(outputCodec);
    if (family == QLatin1String("h264")
        || family == QLatin1String("hevc")
        || family == QLatin1String("av1")
        || family == QLatin1String("vp9")
        || family == QLatin1String("mpeg4"))
        return QStringLiteral("yuv420p");
    if (family == QLatin1String("prores"))
        return QStringLiteral("yuv422p10le");
    return QString();
}

SourceSignature parseSourceSignature(const QString& text)
{
    SourceSignature sig;
    if (!text.startsWith(QStringLiteral("smartrender://")))
        return sig;

    const QUrl url(text);
    const QUrlQuery query(url);
    sig.present = true;
    sig.codec = query.queryItemValue(QStringLiteral("codec")).trimmed();
    sig.width = query.queryItemValue(QStringLiteral("width")).toInt();
    sig.height = query.queryItemValue(QStringLiteral("height")).toInt();
    sig.fps = query.queryItemValue(QStringLiteral("fps")).toDouble();
    sig.pixFmt = query.queryItemValue(QStringLiteral("pixfmt")).trimmed().toLower();
    sig.audioCodec =
        query.queryItemValue(QStringLiteral("audio")).trimmed().toLower();
    sig.hasAudio = !sig.audioCodec.isEmpty();
    return sig;
}

SourceSignature sourceSignatureFor(const ClipInfo& clip)
{
    SourceSignature sig = parseSourceSignature(clip.filePath);
    if (sig.present)
        return sig;
    return parseSourceSignature(clip.displayName);
}

smartrender::SegmentEligibility reject(const QString& reason)
{
    return { false, reason };
}

smartrender::PassThroughEligibility passReject(const QString& reason)
{
    return { false, reason };
}

double timelineDurationForTracks(const QVector<QVector<ClipInfo>>& tracks)
{
    double maxEnd = 0.0;
    for (const auto& clips : tracks) {
        double accum = 0.0;
        for (const ClipInfo& clip : clips)
            accum += qMax(0.0, clip.leadInSec) + clip.effectiveDuration();
        maxEnd = qMax(maxEnd, accum);
    }
    return maxEnd;
}

QString signatureUrlFor(const SourceSignature& sig)
{
    QUrl url(QStringLiteral("smartrender://clip"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("codec"), sig.codec);
    query.addQueryItem(QStringLiteral("width"), QString::number(sig.width));
    query.addQueryItem(QStringLiteral("height"), QString::number(sig.height));
    query.addQueryItem(QStringLiteral("fps"), QString::number(sig.fps, 'f', 6));
    query.addQueryItem(QStringLiteral("pixfmt"), sig.pixFmt);
    url.setQuery(query);
    return url.toString();
}

std::optional<SourceSignature> probedSourceSignature(
    const ClipInfo& clip,
    QString* reason)
{
    SourceSignature sig = sourceSignatureFor(clip);
    if (sig.present)
        return sig;

    const QFileInfo info(clip.filePath);
    if (!info.exists() || !info.isFile()) {
        if (reason)
            *reason = QStringLiteral("source file does not exist");
        return std::nullopt;
    }

    const std::string sourcePath = clip.filePath.toUtf8().toStdString();
    libavcore::MediaDecoder decoder;
    if (const std::optional<std::string> err =
            decoder.open(sourcePath, true)) {
        if (reason) {
            *reason = QStringLiteral("source stream metadata probe failed: ")
                + QString::fromStdString(*err);
        }
        return std::nullopt;
    }

    const libavcore::VideoStreamProps props = decoder.videoProps();
    sig.present = true;
    sig.codec = QString::fromStdString(props.codecName).trimmed();
    if (sig.codec.isEmpty()) {
        const auto codec =
            libavcore::probeVideoCodecName(sourcePath);
        if (codec.has_value())
            sig.codec = QString::fromStdString(*codec).trimmed();
    }
    sig.width = props.width;
    sig.height = props.height;
    if (props.frameRate.num > 0 && props.frameRate.den > 0)
        sig.fps = av_q2d(props.frameRate);
    const char* pixFmtName = av_get_pix_fmt_name(props.pixelFormat);
    if (pixFmtName)
        sig.pixFmt = QString::fromLatin1(pixFmtName).toLower();
    if (decoder.hasAudio()) {
        sig.hasAudio = true;
        sig.audioCodec = QString::fromStdString(
            decoder.audioProps().codecName).trimmed().toLower();
    }

    return sig;
}

std::optional<int64_t> sourceDurationUs(const ClipInfo& clip)
{
    const SourceSignature sig = sourceSignatureFor(clip);
    if (sig.present && clip.duration > 0.0) {
        return static_cast<int64_t>(std::llround(clip.duration * 1'000'000.0));
    }

    const QFileInfo info(clip.filePath);
    if (!info.exists() || !info.isFile())
        return std::nullopt;
    return libavcore::probeDurationMicroseconds(
        clip.filePath.toUtf8().toStdString());
}

bool hasTrackMatteForClip(
    const QHash<QString, TimelineTrackMatteEntry>& matteEntries,
    int trackIdx,
    int clipIdx)
{
    const QString matteKey = trackMatteClipKey(trackIdx, clipIdx);
    const auto it = matteEntries.constFind(matteKey);
    return it != matteEntries.cend()
        && it.value().matteType != TrackMatteType::None;
}

bool hasAnyTrackMatte(
    const QHash<QString, TimelineTrackMatteEntry>& matteEntries)
{
    for (auto it = matteEntries.cbegin(); it != matteEntries.cend(); ++it) {
        if (it.value().matteType != TrackMatteType::None)
            return true;
    }
    return false;
}

bool boolAt(const QVector<bool>& values, int index)
{
    return index >= 0 && index < values.size() && values[index];
}

bool nearlyZero(double value)
{
    return std::fabs(value) <= kEpsilon;
}

bool nearlyOne(double value)
{
    return nearlyEqual(value, 1.0);
}

int64_t frameToleranceUs(double fps)
{
    if (!std::isfinite(fps) || fps <= 0.0)
        return 67'000;
    return static_cast<int64_t>(std::ceil((2.0 * 1'000'000.0) / fps));
}

bool isPartialRange(const smartrender::PassThroughTimelineRequest& request,
                    int64_t timelineDurationUs)
{
    const int64_t toleranceUs = frameToleranceUs(request.outputFps);
    if (std::llabs(request.jobStartUs) > toleranceUs)
        return true;
    if (request.jobEndUs > 0
        && std::llabs(request.jobEndUs - timelineDurationUs) > toleranceUs)
        return true;
    if (request.exportMarkedRangeOnly && request.hasMarkedRange) {
        const int64_t markedInUs =
            static_cast<int64_t>(std::llround(request.markedIn * 1'000'000.0));
        const int64_t markedOutUs =
            static_cast<int64_t>(std::llround(request.markedOut * 1'000'000.0));
        if (std::llabs(markedInUs) > toleranceUs
            || std::llabs(markedOutUs - timelineDurationUs) > toleranceUs) {
            return true;
        }
    }
    return false;
}

bool containerAllowsCodec(const QString& outputPath, const QString& sourceCodec)
{
    const QString ext = QFileInfo(outputPath).suffix().toLower();
    const QString family = normalizedCodecFamily(sourceCodec);
    if (ext.isEmpty() || family.isEmpty())
        return false;

    if (ext == QLatin1String("mp4") || ext == QLatin1String("m4v"))
        return family == QLatin1String("h264")
            || family == QLatin1String("hevc")
            || family == QLatin1String("av1")
            || family == QLatin1String("mpeg4");
    if (ext == QLatin1String("mov"))
        return family == QLatin1String("h264")
            || family == QLatin1String("hevc")
            || family == QLatin1String("prores");
    if (ext == QLatin1String("mkv"))
        return family == QLatin1String("h264")
            || family == QLatin1String("hevc")
            || family == QLatin1String("av1")
            || family == QLatin1String("vp9");
    if (ext == QLatin1String("webm"))
        return family == QLatin1String("vp9")
            || family == QLatin1String("av1");

    return false;
}

bool containerAllowsAudioCodec(const QString& outputPath,
                               const SourceSignature& source)
{
    if (!source.hasAudio)
        return true;

    const QString ext = QFileInfo(outputPath).suffix().toLower();
    const QString audio = source.audioCodec;
    if (ext.isEmpty() || audio.isEmpty())
        return false;

    if (ext == QLatin1String("mp4") || ext == QLatin1String("m4v"))
        return audio == QLatin1String("aac")
            || audio == QLatin1String("mp3")
            || audio == QLatin1String("ac3")
            || audio == QLatin1String("eac3")
            || audio == QLatin1String("alac");
    if (ext == QLatin1String("mov"))
        return audio == QLatin1String("aac")
            || audio == QLatin1String("mp3")
            || audio == QLatin1String("alac")
            || audio == QLatin1String("pcm_s16le")
            || audio == QLatin1String("pcm_s24le");
    if (ext == QLatin1String("mkv"))
        return audio == QLatin1String("aac")
            || audio == QLatin1String("mp3")
            || audio == QLatin1String("ac3")
            || audio == QLatin1String("eac3")
            || audio == QLatin1String("opus")
            || audio == QLatin1String("vorbis")
            || audio == QLatin1String("flac")
            || audio == QLatin1String("pcm_s16le")
            || audio == QLatin1String("pcm_s24le");
    if (ext == QLatin1String("webm"))
        return audio == QLatin1String("opus")
            || audio == QLatin1String("vorbis");

    return false;
}

bool sameExistingFile(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    const QFileInfo ai(a);
    const QFileInfo bi(b);
    if (!ai.exists() || !bi.exists())
        return false;
    const QString ac = ai.canonicalFilePath();
    const QString bc = bi.canonicalFilePath();
    return !ac.isEmpty() && ac == bc;
}

bool hasAudioProcessing(const ClipInfo& clip, bool trackMuted)
{
    return trackMuted
        || !nearlyOne(clip.volume)
        || !nearlyZero(clip.pan)
        || !clip.volumeEnvelope.isEmpty()
        || clip.atempoEnabled;
}

QString specialOverlayReasonForClip(const ClipInfo& clip)
{
    if (clipgeom::isNullObjectFilePath(clip.filePath))
        return QStringLiteral("clip is a generated/special null object");
    if (!clip.visible)
        return QStringLiteral("clip visibility is disabled");
    if (!nearlyOne(clip.opacity))
        return QStringLiteral("clip opacity is not 1.0");
    if (clip.is3DLayer)
        return QStringLiteral("clip is a 3D layer");
    if (clip.motionBlurEnabled)
        return QStringLiteral("clip has motion blur");
    if (clip.fitContain || clip.fitCover)
        return QStringLiteral("clip has SNS fit framing");
    if (clip.hasMask())
        return QStringLiteral("clip has a mask");
    if (!clip.stabilizerKeyframes.isEmpty())
        return QStringLiteral("clip has stabilization keyframes");
    if (clip.textManager.count() > 0)
        return QStringLiteral("clip has text overlays");
    if (clip.colorMeta.isHdr)
        return QStringLiteral("clip source is HDR");
    return QString();
}

} // namespace

namespace smartrender {

SegmentEligibility canStreamCopy(const ClipInfo& clip,
                                 const QString& outputCodec,
                                 int outputWidth,
                                 int outputHeight,
                                 double outputFps,
                                 bool hasEffects,
                                 bool hasColorCorrection,
                                 bool hasTransform,
                                 bool hasTransitions,
                                 bool hasSpeedChange,
                                 bool hasKeyframes,
                                 bool hasLayerStyle,
                                 bool hasTrackMatte,
                                 bool isOverlayLayer)
{
    if (hasEffects || !clip.effects.isEmpty())
        return reject(QStringLiteral("clip has effects"));
    if (hasColorCorrection || !clip.colorCorrection.isDefault() || clip.hasLut())
        return reject(QStringLiteral("clip has color correction"));
    if (hasTransform
        || !nearlyEqual(clip.videoScale, 1.0)
        || !nearlyEqual(clip.videoDx, 0.0)
        || !nearlyEqual(clip.videoDy, 0.0)
        || !nearlyEqual(clip.rotation2DDegrees, 0.0))
        return reject(QStringLiteral("clip transform is not identity"));
    if (hasTransitions
        || clip.leadIn.type != TransitionType::None
        || clip.trailOut.type != TransitionType::None)
        return reject(QStringLiteral("clip has transitions"));
    if (hasSpeedChange || !nearlyEqual(clip.speed, 1.0) || !clip.speedRamp.isIdentity())
        return reject(QStringLiteral("clip speed is not 1.0"));
    if (hasKeyframes || clip.keyframes.hasAnyKeyframes())
        return reject(QStringLiteral("clip has keyframes"));
    if (hasLayerStyle || !clip.layerStyle.isIdentity())
        return reject(QStringLiteral("clip has layer style"));
    if (hasTrackMatte)
        return reject(QStringLiteral("clip uses a track matte"));
    if (isOverlayLayer)
        return reject(QStringLiteral("clip is on an overlay layer"));

    const QString outputFamily = normalizedCodecFamily(outputCodec);
    if (outputFamily.isEmpty())
        return reject(QStringLiteral("output codec is unknown"));

    // ClipInfo currently does not carry a persisted source stream signature.
    // Until ingest writes codec/resolution/fps/pixfmt into ClipInfo, ordinary
    // clips stay conservatively ineligible. The smartrender:// metadata form is
    // a pure-data seam for the selftest and for the staged RenderQueue hook.
    const SourceSignature source = sourceSignatureFor(clip);
    if (!source.present)
        return reject(QStringLiteral("source stream metadata is unavailable on ClipInfo"));

    const QString sourceFamily = normalizedCodecFamily(source.codec);
    if (sourceFamily.isEmpty())
        return reject(QStringLiteral("source codec is unknown"));
    if (sourceFamily != outputFamily)
        return reject(QStringLiteral("codec mismatch: source %1 vs output %2")
            .arg(sourceFamily, outputFamily));

    if (source.width <= 0 || source.height <= 0)
        return reject(QStringLiteral("source resolution is unknown"));
    if (source.width != outputWidth || source.height != outputHeight)
        return reject(QStringLiteral("resolution mismatch: source %1x%2 vs output %3x%4")
            .arg(source.width)
            .arg(source.height)
            .arg(outputWidth)
            .arg(outputHeight));

    if (source.fps <= 0.0)
        return reject(QStringLiteral("source fps is unknown"));
    if (!nearlyEqual(source.fps, outputFps))
        return reject(QStringLiteral("fps mismatch: source %1 vs output %2")
            .arg(source.fps, 0, 'f', 6)
            .arg(outputFps, 0, 'f', 6));

    const QString outputPixFmt = expectedOutputPixFmt(outputCodec);
    if (outputPixFmt.isEmpty())
        return reject(QStringLiteral("output pixel format is unknown"));
    if (source.pixFmt.isEmpty())
        return reject(QStringLiteral("source pixel format is unknown"));
    if (source.pixFmt != outputPixFmt)
        return reject(QStringLiteral("pixel format mismatch: source %1 vs output %2")
            .arg(source.pixFmt, outputPixFmt));

    return { true, QString() };
}

PassThroughEligibility timelinePassThrough(
    const PassThroughTimelineRequest& request)
{
    if (request.clipOdtEnabled
        || request.hdrExport16Enabled
        || request.hdrMatte16Enabled
        || request.hdr10
        || request.hlg) {
        return passReject(QStringLiteral("HDR/ODT export flags are enabled"));
    }

    int videoClipCount = 0;
    int onlyTrack = -1;
    int onlyClip = -1;
    for (int trackIdx = 0; trackIdx < request.videoTracks.size(); ++trackIdx) {
        const QVector<ClipInfo>& clips = request.videoTracks[trackIdx];
        for (int clipIdx = 0; clipIdx < clips.size(); ++clipIdx) {
            ++videoClipCount;
            onlyTrack = trackIdx;
            onlyClip = clipIdx;
        }
    }
    if (videoClipCount != 1 || onlyTrack != 0 || onlyClip != 0) {
        return passReject(QStringLiteral(
            "timeline must contain exactly one video clip on track 0"));
    }

    if (boolAt(request.videoTrackHidden, onlyTrack))
        return passReject(QStringLiteral("video track 0 is hidden"));

    const ClipInfo& clip = request.videoTracks[onlyTrack][onlyClip];
    if (!nearlyZero(clip.leadInSec))
        return passReject(QStringLiteral("clip timeline start is not 0"));

    const QString overlayReason = specialOverlayReasonForClip(clip);
    if (!overlayReason.isEmpty())
        return passReject(overlayReason);
    if (request.hasAdjustmentLayers)
        return passReject(QStringLiteral("timeline has adjustment layers"));
    if (hasAnyTrackMatte(request.trackMatteEntries))
        return passReject(QStringLiteral("timeline has track matte entries"));
    if (!request.clipParentEntries.isEmpty())
        return passReject(QStringLiteral("timeline has parented clip entries"));

    for (int trackIdx = 0; trackIdx < request.audioTracks.size(); ++trackIdx) {
        const QVector<ClipInfo>& clips = request.audioTracks[trackIdx];
        if (!clips.isEmpty())
            return passReject(QStringLiteral("timeline has separate audio track clips"));
    }

    const bool sourceAudioMuted =
        boolAt(request.audioTrackMuted, onlyTrack)
        || boolAt(request.videoTrackMuted, onlyTrack);
    if (hasAudioProcessing(clip, sourceAudioMuted))
        return passReject(QStringLiteral("clip audio is not pass-through"));

    QString probeReason;
    std::optional<SourceSignature> source =
        probedSourceSignature(clip, &probeReason);
    if (!source.has_value())
        return passReject(probeReason.isEmpty()
            ? QStringLiteral("source stream metadata is unavailable")
            : probeReason);

    if (!containerAllowsCodec(request.outputPath, source->codec)) {
        return passReject(QStringLiteral(
            "output container is not safe for source codec %1").arg(source->codec));
    }
    if (!containerAllowsAudioCodec(request.outputPath, *source)) {
        return passReject(QStringLiteral(
            "output container is not safe for source audio codec %1")
            .arg(source->audioCodec.isEmpty()
                ? QStringLiteral("unknown") : source->audioCodec));
    }
    if (sameExistingFile(clip.filePath, request.outputPath))
        return passReject(QStringLiteral("output path is the source file"));

    if (std::fabs(clip.inPoint) > kEpsilon)
        return passReject(QStringLiteral("clip has non-zero inPoint"));

    const std::optional<int64_t> fullDurationUs = sourceDurationUs(clip);
    if (!fullDurationUs.has_value() || *fullDurationUs <= 0)
        return passReject(QStringLiteral("source duration probe failed"));
    const double clipOut = (clip.outPoint > 0.0) ? clip.outPoint : clip.duration;
    const int64_t usedDurationUs =
        static_cast<int64_t>(std::llround(
            qMax(0.0, clipOut - clip.inPoint) * 1'000'000.0));
    const int64_t toleranceUs = frameToleranceUs(request.outputFps);
    if (std::llabs(usedDurationUs - *fullDurationUs) > toleranceUs) {
        return passReject(QStringLiteral(
            "clip trim is present: used %1 us vs source %2 us")
            .arg(usedDurationUs)
            .arg(*fullDurationUs));
    }

    const double timelineDurationSec = request.timelineDurationSec > 0.0
        ? request.timelineDurationSec
        : timelineDurationForTracks(request.videoTracks);
    const int64_t timelineDurationUs =
        static_cast<int64_t>(std::llround(timelineDurationSec * 1'000'000.0));
    if (isPartialRange(request, timelineDurationUs))
        return passReject(QStringLiteral("export range is partial"));

    ClipInfo predicateClip = clip;
    predicateClip.displayName = signatureUrlFor(*source);

    const bool hasTrackMatte =
        hasTrackMatteForClip(request.trackMatteEntries, onlyTrack, onlyClip);
    const bool hasTransitions =
        clip.leadIn.type != TransitionType::None
        || clip.trailOut.type != TransitionType::None;
    const bool hasSpeedChange =
        !clip.speedRamp.isIdentity()
        || std::fabs(clip.speed - 1.0) > kEpsilon;
    const bool hasTransform =
        std::fabs(clip.videoScale - 1.0) > kEpsilon
        || std::fabs(clip.videoDx) > kEpsilon
        || std::fabs(clip.videoDy) > kEpsilon
        || std::fabs(clip.rotation2DDegrees) > kEpsilon
        || clip.is3DLayer;
    const bool hasKeyframes =
        clip.keyframes.hasAnyKeyframes()
        || !clip.stabilizerKeyframes.isEmpty();

    const SegmentEligibility segment =
        canStreamCopy(predicateClip,
                      request.outputCodec,
                      request.outputWidth,
                      request.outputHeight,
                      request.outputFps,
                      !clip.effects.isEmpty(),
                      !clip.colorCorrection.isDefault(),
                      hasTransform,
                      hasTransitions,
                      hasSpeedChange,
                      hasKeyframes,
                      !clip.layerStyle.isIdentity(),
                      hasTrackMatte,
                      false);
    if (!segment.eligible)
        return { false, segment.reason };

    return { true, QString() };
}

PassThroughEligibility timelinePassThrough(const Timeline* timeline,
                                           const QString& outputPath,
                                           const QString& outputCodec,
                                           int outputWidth,
                                           int outputHeight,
                                           double outputFps,
                                           qint64 jobStartUs,
                                           qint64 jobEndUs,
                                           bool exportMarkedRangeOnly,
                                           bool clipOdtEnabled,
                                           bool hdrExport16Enabled,
                                           bool hdrMatte16Enabled,
                                           bool hdr10,
                                           bool hlg)
{
    if (!timeline)
        return passReject(QStringLiteral("timeline is null"));

    PassThroughTimelineRequest request;
    request.videoTracks = timeline->allVideoTracks();
    request.audioTracks = timeline->allAudioTracks();
    request.videoTrackMuted.reserve(timeline->videoTracks().size());
    request.videoTrackHidden.reserve(timeline->videoTracks().size());
    for (const TimelineTrack* track : timeline->videoTracks()) {
        request.videoTrackMuted.append(track && track->isMuted());
        request.videoTrackHidden.append(!track || track->isHidden());
    }
    request.audioTrackMuted.reserve(timeline->audioTracks().size());
    request.audioTrackHidden.reserve(timeline->audioTracks().size());
    for (const TimelineTrack* track : timeline->audioTracks()) {
        request.audioTrackMuted.append(track && track->isMuted());
        request.audioTrackHidden.append(!track || track->isHidden());
    }
    request.trackMatteEntries = timeline->trackMatteEntries();
    request.clipParentEntries = timeline->clipParentEntries();
    request.hasAdjustmentLayers = !timeline->adjustmentLayers().isEmpty();
    request.hasMarkedRange = timeline->hasMarkedRange();
    request.markedIn = timeline->markedIn();
    request.markedOut = timeline->markedOut();
    request.timelineDurationSec = timeline->totalDuration();
    request.outputPath = outputPath;
    request.outputCodec = outputCodec;
    request.outputWidth = outputWidth;
    request.outputHeight = outputHeight;
    request.outputFps = outputFps;
    request.jobStartUs = jobStartUs;
    request.jobEndUs = jobEndUs;
    request.exportMarkedRangeOnly = exportMarkedRangeOnly;
    request.clipOdtEnabled = clipOdtEnabled;
    request.hdrExport16Enabled = hdrExport16Enabled;
    request.hdrMatte16Enabled = hdrMatte16Enabled;
    request.hdr10 = hdr10;
    request.hlg = hlg;
    return timelinePassThrough(request);
}

} // namespace smartrender
