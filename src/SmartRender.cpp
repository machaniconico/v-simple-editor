#include "SmartRender.h"

#include <QUrl>
#include <QUrlQuery>
#include <QtGlobal>

#include <cmath>

namespace {

constexpr double kEpsilon = 0.000001;

struct SourceSignature {
    bool present = false;
    QString codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    QString pixFmt;
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

} // namespace smartrender
