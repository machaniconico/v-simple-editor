#include "../FrameClipboard.h"
#include "../MainWindow.h"
#include "../ShortcutManager.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"

#include <cstring>
#include <memory>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QMetaObject>
#include <QMimeData>
#include <QSize>
#include <QStatusBar>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVector>

namespace {

constexpr int kClipWidth = 64;
constexpr int kClipHeight = 36;
constexpr int kFps = 12;
constexpr int kFrameCount = 18;
constexpr qint64 kPlayheadUsec = 750000;
const QSize kProjectSize(1920, 1080);
const QString kSentinelFormat =
    QStringLiteral("application/x-vse-frame-clipboard-sentinel");
const QByteArray kSentinelBytes("frame-clipboard-sentinel\0bytes", 30);

bool equalRgbaBytes(const QImage &actual, const QImage &expected)
{
    const QImage a = actual.convertToFormat(QImage::Format_RGBA8888);
    const QImage e = expected.convertToFormat(QImage::Format_RGBA8888);
    if (a.size() != e.size() || a.isNull() || e.isNull())
        return false;

    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), e.constScanLine(y),
                        static_cast<std::size_t>(a.width() * 4)) != 0) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<QMimeData> cloneMimeData(const QMimeData *source)
{
    if (!source)
        return {};

    auto clone = std::make_unique<QMimeData>();
    const QStringList formats = source->formats();
    for (const QString &format : formats)
        clone->setData(format, source->data(format));
    if (source->hasImage())
        clone->setImageData(source->imageData());
    if (source->hasColor())
        clone->setColorData(source->colorData());
    if (source->hasHtml())
        clone->setHtml(source->html());
    if (source->hasUrls())
        clone->setUrls(source->urls());
    if (source->hasText())
        clone->setText(source->text());
    return clone;
}

class ClipboardRestore final
{
public:
    explicit ClipboardRestore(QClipboard *clipboard)
        : m_clipboard(clipboard),
          m_original(cloneMimeData(clipboard ? clipboard->mimeData() : nullptr))
    {
    }

    ~ClipboardRestore()
    {
        if (!m_clipboard)
            return;
        if (m_original)
            m_clipboard->setMimeData(m_original.release());
        else
            m_clipboard->clear();
    }

private:
    QClipboard *m_clipboard = nullptr;
    std::unique_ptr<QMimeData> m_original;
};

QImage sentinelImage()
{
    QImage image(3, 2, QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, QColor(0x01, 0x23, 0x45, 0x00));
    image.setPixelColor(1, 0, QColor(0x67, 0x89, 0xab, 0x33));
    image.setPixelColor(2, 0, QColor(0xcd, 0xef, 0x10, 0x66));
    image.setPixelColor(0, 1, QColor(0x32, 0x54, 0x76, 0x99));
    image.setPixelColor(1, 1, QColor(0x98, 0xba, 0xdc, 0xcc));
    image.setPixelColor(2, 1, QColor(0xfe, 0xdc, 0xba, 0xff));
    return image;
}

void seedSentinelClipboard(QClipboard *clipboard, const QImage &image)
{
    if (!clipboard)
        return;
    auto *mime = new QMimeData();
    mime->setData(kSentinelFormat, kSentinelBytes);
    mime->setImageData(image);
    clipboard->setMimeData(mime);
}

bool clipboardMatchesSentinel(const QClipboard *clipboard,
                              const QImage &expectedImage)
{
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime || !mime->hasFormat(kSentinelFormat)
        || mime->data(kSentinelFormat) != kSentinelBytes
        || !mime->hasImage()) {
        return false;
    }
    return equalRgbaBytes(qvariant_cast<QImage>(mime->imageData()),
                          expectedImage);
}

QImage makePatternFrame(int frameIndex)
{
    QImage frame(kClipWidth, kClipHeight, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const int r = (x * 7 + y * 3 + frameIndex * 29) & 255;
            const int g = (x * 2 + y * 11 + frameIndex * 17) & 255;
            const int b = (x * y + frameIndex * 43 + 31) & 255;
            frame.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return frame;
}

bool writeSyntheticClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest request;
    request.width = kClipWidth;
    request.height = kClipHeight;
    request.fps = kFps;
    request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 800000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (const auto openError = encoder.open(request)) {
        if (error) {
            *error = QStringLiteral("FrameEncoder::open failed: %1")
                         .arg(QString::fromStdString(*openError));
        }
        return false;
    }

    for (int frameIndex = 0; frameIndex < kFrameCount; ++frameIndex) {
        if (!encoder.pushFrame(makePatternFrame(frameIndex), frameIndex)) {
            if (error) {
                *error = QStringLiteral("FrameEncoder::pushFrame failed at %1")
                             .arg(frameIndex);
            }
            return false;
        }
    }

    if (const auto finalizeError = encoder.finalize()) {
        if (error) {
            *error = QStringLiteral("FrameEncoder::finalize failed: %1")
                         .arg(QString::fromStdString(*finalizeError));
        }
        return false;
    }
    return true;
}

ClipInfo makeClip(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("frame-clipboard-integration");
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

QString targetShortcutConflictDetail(const MainWindow &window,
                                     const QAction *expectedOwner,
                                     const QKeySequence &target)
{
    QList<const QAction *> owners;
    const QList<QAction *> actions = window.findChildren<QAction *>();
    for (const QAction *action : actions) {
        if (action && action->shortcut() == target)
            owners.append(action);
    }
    if (owners.size() == 1 && owners.first() == expectedOwner)
        return {};

    QStringList labels;
    for (const QAction *owner : owners) {
        labels.append(owner->objectName().isEmpty()
                          ? owner->text()
                          : owner->objectName());
    }
    return QStringLiteral("%1 has %2 live owners: %3")
        .arg(target.toString(QKeySequence::PortableText))
        .arg(owners.size())
        .arg(labels.join(QStringLiteral(", ")));
}

} // namespace

int runFrameClipboardSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[frame-clipboard] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote()
                << QStringLiteral("[frame-clipboard] FAIL G%1 %2%3")
                       .arg(gate).arg(QString::fromLatin1(name))
                       .arg(detail.isEmpty() ? QString()
                                             : QStringLiteral(": ") + detail);
        }
    };

    QClipboard *clipboard = QGuiApplication::clipboard();
    check(1, "QApplication and system clipboard are available",
          QApplication::instance() != nullptr && clipboard != nullptr);
    if (!clipboard)
        return failed;
    ClipboardRestore restoreClipboard(clipboard);

    QString error;
    const std::unique_ptr<QMimeData> emptyMime =
        FrameClipboard::createMimeData(QImage(), &error);
    check(2, "empty image is rejected",
          !emptyMime && !error.isEmpty(), error);

    QImage source(2, 2, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(0x12, 0x34, 0x56, 0x00));
    source.setPixelColor(1, 0, QColor(0x78, 0x9a, 0xbc, 0x40));
    source.setPixelColor(0, 1, QColor(0xde, 0xf0, 0x11, 0x80));
    source.setPixelColor(1, 1, QColor(0x22, 0x44, 0x66, 0xff));

    error.clear();
    const std::unique_ptr<QMimeData> mime =
        FrameClipboard::createMimeData(source, &error);
    check(3, "RGBA MIME payload contains imageData and image/png",
          mime && mime->hasImage()
              && mime->hasFormat(QStringLiteral("image/png")),
          error);

    const QImage imageData = mime
        ? qvariant_cast<QImage>(mime->imageData())
        : QImage();
    check(4, "imageData RGBA round-trip preserves alpha",
          equalRgbaBytes(imageData, source));

    const QByteArray pngBytes = mime
        ? mime->data(QStringLiteral("image/png"))
        : QByteArray();
    const QImage decodedPng = QImage::fromData(pngBytes, "PNG");
    check(5, "image/png RGBA round-trip preserves alpha",
          !pngBytes.isEmpty() && equalRgbaBytes(decodedPng, source));

    const QImage sentinel = sentinelImage();
    seedSentinelClipboard(clipboard, sentinel);
    const bool sentinelBeforeFailure =
        clipboardMatchesSentinel(clipboard, sentinel);
    error.clear();
    const bool copiedEmpty =
        FrameClipboard::copyImage(QImage(), clipboard, &error);
    check(6, "helper failure preserves sentinel MIME bytes and image",
          sentinelBeforeFailure && !copiedEmpty && !error.isEmpty()
              && clipboardMatchesSentinel(clipboard, sentinel),
          error);

    MainWindow window;
    Timeline *timeline = window.findChild<Timeline *>();
    QAction *action = window.findChild<QAction *>(
        QStringLiteral("action_copy_current_frame_to_clipboard"));
    auto *shortcutManager =
        window.findChild<shortcut::ShortcutManager *>();
    VideoPlayer *player = window.findChild<VideoPlayer *>();
    check(7, "MainWindow exposes live action, timeline, player, and shortcuts",
          timeline && action && shortcutManager && player);
    if (!timeline || !action || !shortcutManager || !player)
        return failed;

    QMenu *actionMenu = qobject_cast<QMenu *>(action->parent());
    check(8, "live Edit action has stable accessible identity",
          actionMenu && actionMenu->title().contains(QStringLiteral("編集"))
              && action->text()
                     == QStringLiteral("現在のフレームをクリップボードへコピー")
              && action->property("accessibleName").toString()
                     == QStringLiteral("現在のフレームをクリップボードへコピー"));

    const QKeySequence copyFrameShortcut(Qt::CTRL | Qt::ALT | Qt::Key_C);
    const shortcut::Binding registeredBinding =
        shortcutManager->bindingFor(QStringLiteral("edit.copy_current_frame"));
    shortcutManager->resetAllToDefaults();
    const shortcut::Binding liveBinding =
        shortcutManager->bindingFor(QStringLiteral("edit.copy_current_frame"));
    check(9, "stable ShortcutManager binding drives Ctrl+Alt+C",
          registeredBinding.actionId
                  == QStringLiteral("edit.copy_current_frame")
              && registeredBinding.defaultSequence == copyFrameShortcut
              && liveBinding.sequence == copyFrameShortcut
              && action->shortcut() == copyFrameShortcut);

    const QString duplicateDetail =
        targetShortcutConflictDetail(window, action, copyFrameShortcut);
    check(10, "Ctrl+Alt+C has one owner across all live QActions",
          duplicateDetail.isEmpty(), duplicateDetail);

    seedSentinelClipboard(clipboard, sentinel);
    action->trigger();
    check(11, "empty timeline action preserves clipboard and reports failure",
          clipboardMatchesSentinel(clipboard, sentinel)
              && window.statusBar()->currentMessage().contains(
                  QStringLiteral("現在のフレームをコピーできませんでした")));

    timeline->setPlayheadPosition(
        static_cast<double>(kPlayheadUsec) / 1000000.0);
    QImage poison(11, 7, QImage::Format_RGBA8888);
    poison.fill(QColor(0xf4, 0x0c, 0xbe, 0x4d));
    const bool poisonEmitted = QMetaObject::invokeMethod(
        player, "frameComposited", Qt::DirectConnection,
        Q_ARG(QImage, poison));
    check(12, "preview cache receives a distinct poison frame",
          poisonEmitted && poison.size() != kProjectSize);

    QTemporaryDir tempDir;
    error.clear();
    const QString clipPath =
        tempDir.filePath(QStringLiteral("frame_clipboard_timevarying.mp4"));
    const bool mediaReady = tempDir.isValid()
        && writeSyntheticClip(clipPath, &error);
    check(13, "time-varying synthetic MP4 is available", mediaReady, error);

    bool timelineReady = mediaReady
        && !timeline->videoTracks().isEmpty()
        && timeline->videoTracks().first();
    if (timelineReady) {
        timeline->videoTracks().first()->setClips(
            QVector<ClipInfo>{makeClip(clipPath)});
    }

    const QImage expected = timelineReady
        ? tlrender::renderFrameAt(timeline, kPlayheadUsec, kProjectSize)
              .convertToFormat(QImage::Format_RGBA8888)
        : QImage();
    const QImage atStart = timelineReady
        ? tlrender::renderFrameAt(timeline, 0, kProjectSize)
              .convertToFormat(QImage::Format_RGBA8888)
        : QImage();
    check(14, "direct renderer proves nonzero playhead at 1920x1080",
          !expected.isNull() && expected.size() == kProjectSize
              && !atStart.isNull() && !equalRgbaBytes(expected, atStart));

    action->trigger();
    const QMimeData *actionMime = clipboard->mimeData();
    const QImage actionImage = actionMime
        ? qvariant_cast<QImage>(actionMime->imageData())
        : QImage();
    check(15, "live action copies direct render, never poisoned preview cache",
          actionMime && actionMime->hasImage()
              && equalRgbaBytes(actionImage, expected)
              && !equalRgbaBytes(actionImage, poison));

    const QByteArray actionPngBytes = actionMime
        ? actionMime->data(QStringLiteral("image/png"))
        : QByteArray();
    const QImage actionPng = QImage::fromData(actionPngBytes, "PNG");
    check(16, "live action image/png matches direct render with alpha",
          actionMime
              && actionMime->hasFormat(QStringLiteral("image/png"))
              && !actionPngBytes.isEmpty()
              && equalRgbaBytes(actionPng, expected)
              && !equalRgbaBytes(actionPng, poison));

    check(17, "live action reports successful completion",
          window.statusBar()->currentMessage()
              == QStringLiteral("現在のフレームをクリップボードへコピーしました。"));

    qInfo().noquote()
        << QStringLiteral("[frame-clipboard] summary: %1 PASS, %2 FAIL")
               .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
