#pragma once

#include <QImage>
#include <QString>
#include <QtGlobal>

namespace frameexport {

enum class ImageFormat {
    Png,
    Jpeg
};

struct SaveOptions {
    int jpegQuality = 95;
};

QString fileDialogFilter();
QString extensionForFormat(ImageFormat format);
QString defaultFileName(qint64 timelineUsec, int fps,
                        ImageFormat format = ImageFormat::Png);
ImageFormat formatFromPathOrDefault(const QString &path, ImageFormat fallback);
QString ensureExtensionForFormat(const QString &path, ImageFormat format);

bool saveFrameImage(const QImage &image, const QString &path,
                    ImageFormat format, QString *error = nullptr,
                    SaveOptions options = SaveOptions{});
bool saveFrameImage(const QImage &image, const QString &path,
                    QString *error = nullptr,
                    SaveOptions options = SaveOptions{});

} // namespace frameexport
