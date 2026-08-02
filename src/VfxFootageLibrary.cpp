#include "VfxFootageLibrary.h"

#include "LayerCompositor.h"
#include "libavcore/Decode.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtMath>

#include <cmath>
#include <optional>
#include <string>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace vfxfootage {

namespace {

QString normalizedStem(const QString &fileName)
{
    QString stem = QFileInfo(fileName).completeBaseName().toCaseFolded();
    stem.replace(QLatin1Char('-'), QLatin1Char('_'));
    stem.replace(QLatin1Char('.'), QLatin1Char('_'));
    stem.replace(QLatin1Char(' '), QLatin1Char('_'));
    return stem;
}

QImage frameToImage(const AVFrame *frame)
{
    if (!frame || frame->width <= 0 || frame->height <= 0
        || frame->format < 0) {
        return {};
    }

    const AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(frame->format);
    SwsContext *sws = sws_getContext(
        frame->width, frame->height, sourceFormat,
        frame->width, frame->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws)
        return {};

    QImage image(frame->width, frame->height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        sws_freeContext(sws);
        return {};
    }

    uint8_t *dstData[4] = { image.bits(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
    const int scaled = sws_scale(sws, frame->data, frame->linesize,
                                 0, frame->height, dstData, dstLinesize);
    sws_freeContext(sws);
    return scaled == frame->height ? image : QImage();
}

double frameMeanLuma(const QImage &image)
{
    if (image.isNull())
        return 0.0;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    double sum = 0.0;
    const qint64 pixelCount = static_cast<qint64>(argb.width())
        * static_cast<qint64>(argb.height());
    if (pixelCount <= 0)
        return 0.0;
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x)
            sum += qGray(line[x]);
    }
    return sum / static_cast<double>(pixelCount);
}

double clampGain(double gain)
{
    if (!std::isfinite(gain))
        return 1.0;
    return qMax(0.0, gain);
}

int clampThreshold(int threshold)
{
    return qBound(0, threshold, 64);
}

} // namespace

QString VfxFootageLibrary::defaultDirectory()
{
    const QString overridePath = qEnvironmentVariable(
        "VEDITOR_VFX_FOOTAGE_DIR");
    if (!overridePath.isEmpty())
        return QDir(overridePath).absolutePath();

    const QString relative = QStringLiteral("vfx素材");
    const QString applicationDir = QCoreApplication::instance()
        ? QCoreApplication::applicationDirPath() : QDir::currentPath();
    QDir candidate(applicationDir);
    for (int level = 0; level < 4; ++level) {
        const QString path = candidate.filePath(relative);
        if (QFileInfo(path).isDir())
            return QDir(path).absolutePath();
        // Keep the default rooted at the repository even before the optional
        // directory has been created. This also makes the empty-state button
        // create the right folder when the application was launched from a
        // desktop shortcut with an unrelated working directory.
        if (QFileInfo(candidate.filePath(QStringLiteral("CMakeLists.txt"))).isFile())
            return QDir(path).absolutePath();
        if (!candidate.cdUp())
            break;
    }

    const QString currentPath = QDir::current().filePath(relative);
    return QDir(currentPath).absolutePath();
}

bool VfxFootageLibrary::isSupportedVideoFile(const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toCaseFolded();
    return suffix == QStringLiteral("mp4")
        || suffix == QStringLiteral("mov")
        || suffix == QStringLiteral("webm");
}

QString VfxFootageLibrary::inferCategory(const QString &fileName)
{
    const QString stem = normalizedStem(fileName);
    const auto has = [&stem](const QStringList &tokens) {
        const QString padded = QLatin1Char('_') + stem + QLatin1Char('_');
        for (const QString &token : tokens) {
            const QString normalizedToken = token.toCaseFolded();
            if (stem == normalizedToken
                || padded.contains(QLatin1Char('_') + normalizedToken
                                   + QLatin1Char('_'))) {
                return true;
            }
        }
        return false;
    };

    if (has({QStringLiteral("fire"), QStringLiteral("flame"), QStringLiteral("炎")}))
        return QStringLiteral("炎");
    if (has({QStringLiteral("smoke"), QStringLiteral("煙")}))
        return QStringLiteral("煙");
    if (has({QStringLiteral("explosion"), QStringLiteral("爆発")}))
        return QStringLiteral("爆発");
    if (has({QStringLiteral("spark"), QStringLiteral("sparks"), QStringLiteral("火花")}))
        return QStringLiteral("火花");
    if (has({QStringLiteral("lightning"), QStringLiteral("thunder"), QStringLiteral("稲妻")}))
        return QStringLiteral("稲妻");
    if (has({QStringLiteral("dust"), QStringLiteral("debris"), QStringLiteral("塵")}))
        return QStringLiteral("塵");
    if (has({QStringLiteral("rain"), QStringLiteral("雨")}))
        return QStringLiteral("雨");
    if (has({QStringLiteral("snow"), QStringLiteral("雪")}))
        return QStringLiteral("雪");
    return QStringLiteral("その他");
}

QStringList VfxFootageLibrary::inferTags(const QString &fileName,
                                         const QString &category)
{
    const QFileInfo info(fileName);
    const QString stem = info.completeBaseName();
    QStringList tags;
    if (!stem.isEmpty()) {
        tags.append(stem);
        for (const QString &token : stem.split(QRegularExpression(QStringLiteral("[_ .-]+")),
                                               Qt::SkipEmptyParts)) {
            if (!tags.contains(token, Qt::CaseInsensitive))
                tags.append(token);
        }
    }
    const QString effectiveCategory = category.isEmpty()
        ? inferCategory(fileName) : category;
    if (!effectiveCategory.isEmpty() && !tags.contains(effectiveCategory))
        tags.append(effectiveCategory);
    if (!tags.contains(QStringLiteral("VFX"), Qt::CaseInsensitive))
        tags.append(QStringLiteral("VFX"));
    if (!tags.contains(QStringLiteral("footage"), Qt::CaseInsensitive))
        tags.append(QStringLiteral("footage"));
    return tags;
}

QVector<FootageItem> VfxFootageLibrary::scan(const QString &directory)
{
    QVector<FootageItem> result;
    const QDir dir(directory);
    if (!dir.exists())
        return result;

    const QFileInfoList files = dir.entryInfoList(
        QDir::Files, QDir::Name | QDir::IgnoreCase);
    result.reserve(files.size());
    for (const QFileInfo &info : files) {
        if (!isSupportedVideoFile(info.fileName()))
            continue;
        FootageItem item;
        item.filePath = info.absoluteFilePath();
        item.displayName = info.completeBaseName();
        item.category = inferCategory(info.fileName());
        item.tags = inferTags(info.fileName(), item.category);
        item.durationSeconds = probeDurationSeconds(item.filePath,
                                                    &item.frameSize);
        result.append(item);
    }
    return result;
}

double VfxFootageLibrary::probeDurationSeconds(const QString &filePath,
                                               QSize *frameSize)
{
    if (frameSize)
        *frameSize = QSize();
    if (!isSupportedVideoFile(filePath))
        return 0.0;

    libavcore::MediaDecoder decoder;
    const std::optional<std::string> error = decoder.open(
        filePath.toUtf8().toStdString(), false);
    if (error.has_value())
        return 0.0;
    const libavcore::VideoStreamProps props = decoder.videoProps();
    if (frameSize)
        *frameSize = QSize(props.width, props.height);
    return props.durationUs > 0
        ? static_cast<double>(props.durationUs) / 1'000'000.0 : 0.0;
}

QImage VfxFootageLibrary::representativeFrame(const QString &filePath,
                                              double *durationSeconds,
                                              QSize *frameSize)
{
    if (durationSeconds)
        *durationSeconds = 0.0;
    if (frameSize)
        *frameSize = QSize();
    if (!isSupportedVideoFile(filePath))
        return {};

    libavcore::MediaDecoder decoder;
    const std::optional<std::string> error = decoder.open(
        filePath.toUtf8().toStdString(), false);
    if (error.has_value())
        return {};
    const libavcore::VideoStreamProps props = decoder.videoProps();
    if (durationSeconds && props.durationUs > 0)
        *durationSeconds = static_cast<double>(props.durationUs) / 1'000'000.0;
    if (frameSize)
        *frameSize = QSize(props.width, props.height);

    const double duration = props.durationUs > 0
        ? static_cast<double>(props.durationUs) / 1'000'000.0 : 0.0;
    const double target = duration > 0.0 ? duration / 3.0 : 0.0;
    if (target > 0.0)
        decoder.seek(target);

    QImage best;
    double bestLuma = -1.0;
    // A few neighboring frames make the thumbnail useful even when the exact
    // one-third frame is a fade-to-black or a sparse effect frame.
    for (int i = 0; i < 12; ++i) {
        AVFrame *frame = decoder.nextVideoFrame();
        if (!frame)
            break;
        const QImage image = frameToImage(frame);
        if (image.isNull())
            continue;
        const double luma = frameMeanLuma(image);
        if (best.isNull() || luma > bestLuma) {
            best = image;
            bestLuma = luma;
        }
    }
    return best;
}

QImage VfxFootageLibrary::applyBlackLevel(const QImage &source, int threshold)
{
    const int t = clampThreshold(threshold);
    if (source.isNull() || t == 0)
        return source;

    QImage result = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < result.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const QRgb pixel = line[x];
            if (qGray(pixel) <= t)
                line[x] = qRgba(0, 0, 0, qAlpha(pixel));
        }
    }
    return result;
}

QImage VfxFootageLibrary::applyIntensity(const QImage &source, double gain)
{
    const double safeGain = clampGain(gain);
    if (source.isNull() || qFuzzyCompare(safeGain, 1.0))
        return source;

    QImage result = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < result.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const QRgb pixel = line[x];
            line[x] = qRgba(
                qBound(0, qRound(qRed(pixel) * safeGain), 255),
                qBound(0, qRound(qGreen(pixel) * safeGain), 255),
                qBound(0, qRound(qBlue(pixel) * safeGain), 255),
                qAlpha(pixel));
        }
    }
    return result;
}

QImage VfxFootageLibrary::screenComposite(const QImage &base,
                                          const QImage &overlay,
                                          double opacity)
{
    if (base.isNull() || overlay.isNull())
        return {};
    return LayerCompositor::blendImages(base, overlay, BlendMode::Screen,
                                        qBound(0.0, opacity, 1.0));
}

} // namespace vfxfootage
