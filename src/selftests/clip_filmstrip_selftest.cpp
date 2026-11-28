#include "../Timeline.h"
#include "../ThumbnailCache.h"
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <limits>

int runClipFilmstripSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int n, bool ok) {
        ok ? ++passed : ++failed;
        std::fprintf(stderr, "[clip-filmstrip] %s G%d\n", ok ? "PASS" : "FAIL", n);
    };
    ThumbnailCache cache;
    Timeline timeline;
    timeline.setFilmstripCache(&cache);
    ClipInfo clip;
    clip.filePath = QStringLiteral("test_assets/e2e_clip.mp4");
    clip.displayName = QStringLiteral("フィルムストリップ");
    clip.duration = 1.0;
    clip.inPoint = 0.125;
    clip.outPoint = 0.875;
    clip.leadInSec = 0.25;
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
        QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    auto *track = timeline.videoTracks().first();
    track->setPixelsPerSecond(200.0);
    track->resize(400, track->height());
    const auto render = [&] {
        QImage image(track->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        track->render(&image);
        return image;
    };
    render();
    gate(1, !timeline.filmstripEnabled() && track->paintCountForTest() > 0
        && track->filmstripDrawCountForTest() == 0
        && track->filmstripBranchCountForTest() == 0 && cache.requestCount() == 0);

    // Model the media pool's request, including its initially unknown duration.
    // Enabling the timeline must reuse this pending request and its sample times.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool ready = false;
    QObject::connect(&cache, &ThumbnailCache::ready, &loop,
        [&](const QString &key, const QVector<QImage> &frames) {
            if (key == clip.filePath) { ready = frames.size() == 8; loop.quit(); }
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    cache.request(clip.filePath, clip.filePath, 0.0, 8);
    timeline.setFilmstripEnabled(true);
    render();
    timeout.start(30000);
    if (!ready) loop.exec();
    render();
    const auto tiles = track->filmstripTilesForTest(0);
    const QRect clipRect(track->clipStartX(0), 0, 150, track->height());
    bool valid = ready && cache.duration(clip.filePath) > 0.0 && !tiles.isEmpty()
        && track->filmstripDrawCountForTest() > 0 && cache.requestCount() == 1;
    for (const auto &tile : tiles)
        valid = valid && clipRect.contains(tile.rect) && !tile.rect.isEmpty()
            && tile.imageIndex >= 0 && tile.imageIndex <= 7;
    const auto branches = track->filmstripBranchCountForTest();
    const auto draws = track->filmstripDrawCountForTest();
    timeline.setFilmstripEnabled(false);
    render();
    valid = valid && track->filmstripBranchCountForTest() == branches
        && track->filmstripDrawCountForTest() == draws && cache.requestCount() == 1;
    gate(2, valid);

    const auto count = TimelineTrack::filmstripTileCount;
    const auto index = TimelineTrack::filmstripImageIndex;
    bool mapping = count(0, 80) == 0 && count(80, 0) == 0
        && count(1, 80) == 1 && count(80, 80) == 1
        && count(81, 80) == 2 && count(640, 80) == 8
        && index(-1.0, 8.0, 8) == 0 && index(99.0, 8.0, 8) == 7
        && index(4.0, 0.0, 8) == 0 && index(4.0, 8.0, 0) == 0
        && index(std::numeric_limits<double>::quiet_NaN(), 8.0, 8) == 0
        && index(2.49, 8.0, 8) == 2 && index(2.51, 8.0, 8) == 3;
    for (int i = 0; i < 8; ++i)
        mapping = mapping && index(double(i), 8.0, 8) == i
            && index(double(i), 8.0, 1) == 0; // Still images repeat frame zero.
    gate(3, mapping);
    std::fprintf(stderr, "[clip-filmstrip] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
