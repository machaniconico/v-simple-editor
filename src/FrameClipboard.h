#pragma once

#include <memory>

#include <QImage>

class QClipboard;
class QMimeData;
class QString;

class FrameClipboard final
{
public:
    static std::unique_ptr<QMimeData> createMimeData(
        const QImage &frame, QString *error = nullptr);

    // The system clipboard is only replaced after every MIME payload has
    // been prepared successfully. Any failure therefore leaves it unchanged.
    static bool copyImage(const QImage &frame, QClipboard *clipboard,
                          QString *error = nullptr);
};
