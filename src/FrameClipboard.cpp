#include "FrameClipboard.h"

#include <QBuffer>
#include <QByteArray>
#include <QClipboard>
#include <QImageWriter>
#include <QMimeData>
#include <QString>

std::unique_ptr<QMimeData> FrameClipboard::createMimeData(
    const QImage &frame, QString *error)
{
    if (error)
        error->clear();
    if (frame.isNull() || frame.width() <= 0 || frame.height() <= 0) {
        if (error)
            *error = QStringLiteral("現在のフレームが空です。");
        return {};
    }

    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
    QByteArray pngBytes;
    QBuffer pngBuffer(&pngBytes);
    if (!pngBuffer.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("PNGデータを準備できませんでした。");
        return {};
    }

    QImageWriter writer(&pngBuffer, "png");
    if (!writer.write(rgba)) {
        if (error) {
            *error = QStringLiteral(
                         "現在のフレームをPNGとしてエンコードできませんでした: %1")
                         .arg(writer.errorString());
        }
        return {};
    }

    auto mime = std::make_unique<QMimeData>();
    mime->setImageData(rgba);
    mime->setData(QStringLiteral("image/png"), pngBytes);
    return mime;
}

bool FrameClipboard::copyImage(const QImage &frame, QClipboard *clipboard,
                               QString *error)
{
    std::unique_ptr<QMimeData> mime = createMimeData(frame, error);
    if (!mime)
        return false;
    if (!clipboard) {
        if (error)
            *error = QStringLiteral("システムクリップボードを利用できません。");
        return false;
    }

    clipboard->setMimeData(mime.release(), QClipboard::Clipboard);
    return true;
}
