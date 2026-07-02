#include "FrameExport.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>

#include <algorithm>

namespace frameexport {
namespace {

QByteArray writerFormat(ImageFormat format)
{
    return format == ImageFormat::Jpeg ? QByteArrayLiteral("JPEG")
                                       : QByteArrayLiteral("PNG");
}

bool isSupportedSuffix(const QString &suffix)
{
    const QString lower = suffix.toLower();
    return lower == QStringLiteral("png")
        || lower == QStringLiteral("jpg")
        || lower == QStringLiteral("jpeg");
}

QString timecodeFromUsec(qint64 timelineUsec, int fps)
{
    const int safeFps = std::max(1, fps);
    const qint64 clampedUsec = std::max<qint64>(0, timelineUsec);
    const qint64 totalFrames =
        (clampedUsec * static_cast<qint64>(safeFps) + 500000LL) / 1000000LL;
    const qint64 totalSeconds = totalFrames / safeFps;
    const int frame = static_cast<int>(totalFrames % safeFps);
    const int hours = static_cast<int>(totalSeconds / 3600);
    const int minutes = static_cast<int>((totalSeconds / 60) % 60);
    const int seconds = static_cast<int>(totalSeconds % 60);

    return QStringLiteral("%1-%2-%3-%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frame, 2, 10, QLatin1Char('0'));
}

} // namespace

QString fileDialogFilter()
{
    return QStringLiteral("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg)");
}

QString extensionForFormat(ImageFormat format)
{
    return format == ImageFormat::Jpeg ? QStringLiteral("jpg")
                                       : QStringLiteral("png");
}

QString defaultFileName(qint64 timelineUsec, int fps, ImageFormat format)
{
    return QStringLiteral("frame_%1.%2")
        .arg(timecodeFromUsec(timelineUsec, fps), extensionForFormat(format));
}

ImageFormat formatFromPathOrDefault(const QString &path, ImageFormat fallback)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg"))
        return ImageFormat::Jpeg;
    if (suffix == QStringLiteral("png"))
        return ImageFormat::Png;
    return fallback;
}

QString ensureExtensionForFormat(const QString &path, ImageFormat format)
{
    const QFileInfo info(path);
    if (isSupportedSuffix(info.suffix()))
        return path;

    QString withExtension = path;
    if (!withExtension.endsWith(QLatin1Char('.')))
        withExtension += QLatin1Char('.');
    withExtension += extensionForFormat(format);
    return withExtension;
}

bool saveFrameImage(const QImage &image, const QString &path,
                    ImageFormat format, QString *error, SaveOptions options)
{
    if (error)
        error->clear();

    if (image.isNull()) {
        if (error)
            *error = QStringLiteral("frame image is null");
        return false;
    }
    if (path.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("output path is empty");
        return false;
    }

    const QString finalPath = ensureExtensionForFormat(path, format);
    const QFileInfo info(finalPath);
    const QDir parent = info.absoluteDir();
    if (!parent.exists()) {
        if (error)
            *error = QStringLiteral("output directory does not exist: %1")
                .arg(parent.absolutePath());
        return false;
    }

    const QImage normalized = (format == ImageFormat::Jpeg)
        ? image.convertToFormat(QImage::Format_RGB888)
        : image.convertToFormat(QImage::Format_RGBA8888);

    QImageWriter writer(finalPath, writerFormat(format));
    if (format == ImageFormat::Jpeg)
        writer.setQuality(std::max(0, std::min(100, options.jpegQuality)));

    if (!writer.write(normalized)) {
        if (error)
            *error = writer.errorString();
        return false;
    }

    return true;
}

bool saveFrameImage(const QImage &image, const QString &path,
                    QString *error, SaveOptions options)
{
    const ImageFormat format = formatFromPathOrDefault(path, ImageFormat::Png);
    return saveFrameImage(image, path, format, error, options);
}

} // namespace frameexport
