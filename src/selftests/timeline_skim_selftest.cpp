#include "../Timeline.h"
#include "../ThumbnailCache.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"
#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QMouseEvent>
#include <QTimer>
#include <QStackedWidget>
#include <QPixmap>
#include <cstdio>

int runTimelineSkimSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int n, bool ok) {
        ok ? ++passed : ++failed;
        std::fprintf(stderr, "[timeline-skim] %s G%d\n", ok ? "PASS" : "FAIL", n);
    };
    ThumbnailCache cache;
    Timeline timeline;
    VideoPlayer player;
    timeline.setFilmstripCache(&cache);
    ClipInfo clip;
    clip.filePath = QStringLiteral("test_assets/e2e_clip.mp4");
    clip.duration = 1.0;
    clip.inPoint = 0.0;
    clip.outPoint = 1.0;
    player.setGLAcceleration(false);
    player.loadFile(clip.filePath);
    player.seek(250);
    auto *stack = player.findChild<QStackedWidget *>();
    auto *playheadSurface = stack ? qobject_cast<QLabel *>(stack->currentWidget()) : nullptr;
    const QImage playheadImage = playheadSurface ? playheadSurface->pixmap().toImage() : QImage();
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
        QVector<QVector<ClipInfo>>{}, 0.25, -1.0, -1.0, 100);
    auto *track = timeline.videoTracks().first();
    track->setPixelsPerSecond(200.0);
    track->resize(400, track->height());
    const double playhead = timeline.playheadPosition();
    const auto playerPosition = player.timelinePositionUs();
    const auto serial = timeline.undoManager()->saveSerial();
    const int undoIndex = timeline.undoManager()->currentIndex();
    const auto history = timeline.undoManager()->historyDescriptions();
    int updates = 0, restores = 0;
    QImage selected;
    QObject::connect(&timeline, &Timeline::skimPreviewRequested, &player,
                     &VideoPlayer::showTimelineSkim);
    QObject::connect(&timeline, &Timeline::skimPreviewCleared, &player,
                     &VideoPlayer::clearTimelineSkim);
    QObject::connect(&timeline, &Timeline::skimPreviewRequested, &timeline,
        [&](const QImage &image) { ++updates; selected = image; });
    QObject::connect(&timeline, &Timeline::skimPreviewCleared, &timeline,
        [&] { ++restores; });
    const auto hover = [&](int x) {
        const QPointF pos(x, track->height() / 2);
        QMouseEvent event(QEvent::MouseMove, pos, pos,
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(track, &event);
    };
    hover(0); hover(100); hover(199); hover(300);
    gate(1, !timeline.skimmingEnabled() && cache.requestCount() == 0
        && updates == 0 && restores == 0);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&cache, &ThumbnailCache::ready, &loop,
        [&](const QString &key, const QVector<QImage> &) {
            if (key == clip.filePath) loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeline.setSkimmingEnabled(true);
    hover(100); // Cold request must also publish when asynchronous decode finishes.
    timeout.start(30000);
    if (cache.frames(clip.filePath).isEmpty()) loop.exec();
    const auto frames = cache.frames(clip.filePath);
    bool mapping = frames.size() == 8 && updates > 0 && cache.requestCount() == 1;
    for (int x : {0, 100, 199}) {
        const int before = updates;
        hover(x);
        const int expected = ThumbnailCache::skimIndex(x, 200, int(frames.size()));
        mapping = mapping && updates == before + 1 && !frames.isEmpty()
            && selected == frames[expected];
    }
    auto *overlay = player.findChild<QLabel *>(QStringLiteral("timelineSkimPreview"));
    gate(2, mapping && overlay && !overlay->isHidden() && !overlay->pixmap().isNull());

    hover(300);
    bool restored = restores == 1 && overlay && overlay->isHidden()
        && !playheadImage.isNull() && playheadSurface
        && playheadSurface->pixmap().toImage() == playheadImage;
    hover(100);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(track, &leave);
    restored = restored && restores == 2 && overlay && overlay->isHidden();
    hover(100);
    timeline.setSkimmingPlaybackActive(true);
    const int beforePlayback = updates;
    const auto requests = cache.requestCount();
    hover(0);
    restored = restored && restores == 3 && updates == beforePlayback
        && cache.requestCount() == requests && overlay && overlay->isHidden();
    timeline.setSkimmingPlaybackActive(false);
    hover(100);
    timeline.setSkimmingEnabled(false);
    const int beforeOff = updates;
    hover(0);
    gate(3, restored && restores == 4 && updates == beforeOff
        && cache.requestCount() == requests && overlay && overlay->isHidden()
        && playheadSurface && playheadSurface->pixmap().toImage() == playheadImage);
    gate(4, timeline.playheadPosition() == playhead
        && player.timelinePositionUs() == playerPosition
        && timeline.undoManager()->saveSerial() == serial
        && timeline.undoManager()->currentIndex() == undoIndex
        && timeline.undoManager()->historyDescriptions() == history);
    std::fprintf(stderr, "[timeline-skim] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
