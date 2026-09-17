#include "../libavcore/FrameGrab.h"
#include "../ThumbnailCache.h"
#include "../MediaPoolDock.h"
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QSettings>
#include <QThread>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPixmap>
#include <QColor>
#include <functional>
#include <cstdio>

namespace {
bool waitUntil(const std::function<bool()> &done)
{
    if (done()) return true;
    QEventLoop loop;
    QTimer poll, timeout;
    poll.setInterval(10);
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (done()) loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    timeout.start(30000);
    loop.exec();
    return done();
}

struct RestoreSetting {
    QSettings settings;
    const QString key = QStringLiteral("mediaPool/showThumbnails");
    bool existed = settings.contains(key);
    QVariant value = settings.value(key);
    ~RestoreSetting() { if (existed) settings.setValue(key, value); else settings.remove(key); }
};
}

int runMediaPoolThumbsSelftest()
{
    int passed = 0, failed = 0;
    const auto check = [&](int gate, bool ok) {
        ok ? ++passed : ++failed;
        std::fprintf(stderr, "[media-pool-thumbs] %s G%d\n", ok ? "PASS" : "FAIL", gate);
    };
    const QString video = QStringLiteral("test_assets/e2e_clip.mp4");
    QTemporaryDir temp;
    const QString png = temp.filePath(QStringLiteral("alpha.png"));
    QImage source(32, 16, QImage::Format_RGBA8888);
    source.fill(QColor(80, 120, 200, 91));
    source.setPixelColor(0, 0, QColor(10, 20, 30, 0));
    bool g1 = temp.isValid() && source.save(png);
    for (double seconds : {0.0, 0.5}) {
        const auto frame = libavcore::grabFrameAt(video, seconds);
        g1 = g1 && !frame.isNull() && frame.format() == QImage::Format_RGBA8888;
        for (int y = 0; y < frame.height(); ++y)
            for (int x = 0; x < frame.width(); ++x)
                g1 = g1 && frame.constScanLine(y)[4 * x + 3] == 255;
        const auto small = libavcore::grabFrameAt(video, seconds, QSize(64, 36));
        g1 = g1 && !small.isNull() && small.format() == QImage::Format_RGBA8888
            && small.size() == frame.size().scaled(QSize(64, 36), Qt::KeepAspectRatio);
    }
    const auto alpha = libavcore::grabFrameAt(png, 12.0);
    check(1, g1 && alpha.format() == QImage::Format_RGBA8888 && alpha == source
        && libavcore::grabFrameAt(QStringLiteral("test_assets/e2e_hum.wav"), 0).isNull()
        && libavcore::grabFrameAt(temp.filePath(QStringLiteral("missing.mp4")), 0).isNull());

    ThumbnailCache cache;
    int readyCount = 0;
    bool uiThread = true;
    QObject::connect(&cache, &ThumbnailCache::ready, &cache,
        [&](const QString &, const QVector<QImage> &) {
            ++readyCount;
            uiThread = uiThread && QThread::currentThread() == QCoreApplication::instance()->thread();
        });
    cache.request(video, video, 1.0);
    cache.request(video, video, 1.0);
    cache.request(png, png, 12.0);
    bool g2 = cache.requestCount() == 2 && cache.activeCount() <= 2
        && waitUntil([&] { return readyCount == 2; });
    g2 = g2 && cache.frames(video).size() == 8 && cache.frames(png).size() == 1 && uiThread;
    cache.request(video, video, 1.0);
    g2 = g2 && cache.requestCount() == 2 && readyCount == 2;
    cache.clear();
    cache.request(video, video, 1.0);
    cache.request(png, png, 0.0);
    cache.clear(); // Active results from the old generation must not be emitted.
    cache.request(png, png, 0.0);
    g2 = g2 && cache.activeCount() <= 2 && waitUntil([&] { return cache.activeCount() == 0; })
        && readyCount == 3 && cache.frames(video).isEmpty() && cache.frames(png).size() == 1;
    // Room for two PNGs: touching a keeps it when c evicts b.
    const int entryBytes = int(source.sizeInBytes()) + 1;
    ThumbnailCache lru(nullptr, 2 * entryBytes);
    lru.request(QStringLiteral("a"), png, 0);
    g2 = waitUntil([&] { return lru.activeCount() == 0; }) && g2;
    lru.request(QStringLiteral("b"), png, 0);
    g2 = waitUntil([&] { return lru.activeCount() == 0; }) && g2;
    g2 = g2 && lru.frames(QStringLiteral("a")).size() == 1;
    lru.request(QStringLiteral("c"), png, 0);
    g2 = waitUntil([&] { return lru.activeCount() == 0; }) && g2;
    check(2, g2 && lru.frames(QStringLiteral("b")).isEmpty()
        && lru.frames(QStringLiteral("a")).size() == 1
        && lru.frames(QStringLiteral("c")).size() == 1
        && lru.memoryBytes() <= 2 * entryBytes);

    bool g3 = ThumbnailCache::skimIndex(-100, 80, 8) == 0
        && ThumbnailCache::skimIndex(80, 80, 8) == 7
        && ThumbnailCache::skimIndex(900, 80, 8) == 7
        && ThumbnailCache::skimIndex(5, 0, 8) == 0
        && ThumbnailCache::skimIndex(5, 80, 0) == 0;
    for (int i = 0; i < 8; ++i)
        g3 = g3 && ThumbnailCache::skimIndex(i * 10 + 5, 80, 8) == i;
    check(3, g3);

    RestoreSetting restore;
    restore.settings.setValue(restore.key, true);
    mediapool::MediaPool pool;
    mediapool::MediaAsset asset;
    asset.filePath = video;
    asset.type = mediapool::MediaType::Video;
    asset.durationMs = 1000;
    pool.addAsset(asset);
    MediaPoolDock dock;
    dock.resize(640, 400);
    dock.setPool(&pool);
    dock.show();
    auto *list = dock.findChild<QListWidget *>(QStringLiteral("MediaPoolAssetList"));
    auto *dockCache = dock.findChild<ThumbnailCache *>();
    bool g4 = list && dockCache && waitUntil([&] { return list->count() == 1 && !list->item(0)->icon().isNull(); });
    if (g4) {
        const auto frames = dockCache->frames(video);
        g4 = frames.size() == 8;
        if (g4) {
            const QRect rect = list->visualItemRect(list->item(0));
            const QPointF pos(rect.right() - 1, rect.center().y());
            QMouseEvent move(QEvent::MouseMove, pos, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(list->viewport(), &move);
            const auto iconImage = [&] { return list->item(0)->icon().pixmap(frames[0].size()).toImage().convertToFormat(QImage::Format_RGBA8888); };
            g4 = iconImage() == frames[7];
            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(list->viewport(), &leave);
            g4 = g4 && iconImage() == frames[0];
        }
        dock.refresh();
        g4 = g4 && !list->item(0)->icon().isNull();
        dock.setThumbnailsEnabled(false);
        const auto requests = dockCache->requestCount();
        dock.refresh();
        g4 = g4 && list->item(0)->icon().isNull() && dockCache->requestCount() == requests;
        MediaPoolDock disabled;
        disabled.setPool(&pool);
        auto *disabledList = disabled.findChild<QListWidget *>(QStringLiteral("MediaPoolAssetList"));
        g4 = g4 && !disabled.thumbnailsEnabled() && disabledList && disabledList->item(0)->icon().isNull()
            && disabled.findChild<ThumbnailCache *>()->requestCount() == 0;
        dock.setThumbnailsEnabled(true);
        g4 = g4 && waitUntil([&] { return !list->item(0)->icon().isNull(); });
        // Turning off during an in-flight request must also suppress stale icons.
        dock.setPool(&pool);
        dock.setThumbnailsEnabled(false);
        g4 = waitUntil([&] { return dockCache->activeCount() == 0; }) && g4
            && list->item(0)->icon().isNull();
    }
    check(4, g4);
    std::fprintf(stderr, "[media-pool-thumbs] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
