#include "../MainWindow.h"
#include "../MediaPoolDock.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace {
// Use the real MainWindow provider without exposing test APIs in its header.
// Same explicit-instantiation access pattern as loudness_persist_selftest.
template<class Tag, typename Tag::Type Member>
struct Access {
    friend typename Tag::Type member(Tag) { return Member; }
};
#define ACCESS(Name, TypeExpr, MemberExpr) \
    struct Name { using Type = TypeExpr; friend Type member(Name); }; \
    template struct Access<Name, MemberExpr>;
ACCESS(Pool, mediapool::MediaPool MainWindow::*, &MainWindow::m_mediaPool)
ACCESS(Overlays, QVector<OverlayItem> MainWindow::*, &MainWindow::m_projectOverlays)
using ParticleConfigs = QHash<QString, ParticleEmitterConfig>;
ACCESS(Particles, ParticleConfigs MainWindow::*, &MainWindow::m_particleClipConfigs)
ACCESS(Provider, std::function<QSet<QString>()> MediaPoolDock::*, &MediaPoolDock::m_usedPathsProvider)
#undef ACCESS

bool writeVideo(const QString &path)
{
    libavcore::EncodeRequest request;
    request.outputPath = path.toStdString();
    request.width = 32;
    request.height = 32;
    request.fps = 10;
    request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoCodecName = "mpeg4";
    request.videoBitrateBits = 600000;
    request.useHardwareAccel = false;
    request.hwVendorHint = "none";
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(32, 32, QImage::Format_RGB888);
    frame.fill(Qt::blue);
    if (!encoder.pushFrame(frame, 0)) return false;
    return !encoder.finalize();
}

struct RestoreThumbnailSetting {
    QSettings settings;
    const QString key = QStringLiteral("mediaPool/showThumbnails");
    bool existed = settings.contains(key);
    QVariant value = settings.value(key);
    ~RestoreThumbnailSetting() {
        if (existed) settings.setValue(key, value);
        else settings.remove(key);
    }
};
}

int runReduceProjectSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "[reduce-project] %s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir temp;
    RestoreThumbnailSetting restoreSetting;
    restoreSetting.settings.setValue(restoreSetting.key, false);
    MainWindow window;
    auto *dock = window.findChild<MediaPoolDock *>(QStringLiteral("MediaPoolDock"));
    auto *timeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
    if (!dock || !timeline || !temp.isValid()) {
        for (int g = 1; g <= 4; ++g) gate(g, false);
        std::fprintf(stderr, "[reduce-project] summary: %d PASS, %d FAIL\n", passed, failed);
        return failed;
    }
    auto &pool = window.*member(Pool{});
    pool.clear();
    const QStringList names{QStringLiteral("active.mp4"), QStringLiteral("audio.mp4"),
        QStringLiteral("nested.mp4"), QStringLiteral("overlay.bmp"),
        QStringLiteral("particle.bin"), QStringLiteral("active.cube"),
        QStringLiteral("nested.cube"), QStringLiteral("unused-a.mp4"),
        QStringLiteral("unused-b.mp4")};
    QStringList paths;
    QVector<int> ids;
    bool ready = true;
    for (int i = 0; i < names.size(); ++i) {
        const QString path = temp.filePath(names[i]);
        paths.append(path);
        if (i == 0) {
            ready = writeVideo(path) && ready;
        } else if (names[i].endsWith(QLatin1String(".mp4"))) {
            ready = QFile::copy(paths[0], path) && ready;
        } else if (names[i].endsWith(QLatin1String(".bmp"))) {
            QImage image(32, 32, QImage::Format_RGB888);
            image.fill(Qt::red);
            ready = image.save(path, "BMP") && ready;
        } else {
            QFile file(path);
            ready = file.open(QIODevice::WriteOnly) && ready;
            const QByteArray contents = names[i].endsWith(QLatin1String(".cube"))
                ? QByteArray("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n")
                : QByteArray("fixture\n");
            ready = (file.write(contents) == contents.size()) && ready;
        }
        mediapool::MediaAsset asset;
        asset.filePath = path;
        asset.displayName = names[i];
        ids.append(pool.addAsset(asset));
    }
    ClipInfo video;
    video.filePath = paths[0];
    video.lutFilePath = paths[5];
    video.duration = video.outPoint = 0.1;
    ClipInfo audio = video;
    audio.filePath = paths[1];
    audio.lutFilePath.clear();
    ClipInfo child = video;
    child.filePath = paths[2];
    child.lutFilePath = paths[6];
    TimelineSequence active;
    active.id = QStringLiteral("reduce-active");
    active.name = QStringLiteral("Active");
    active.videoTracks = {QVector<ClipInfo>{video}};
    active.audioTracks = {QVector<ClipInfo>{audio}};
    TimelineSequence nested;
    nested.id = QStringLiteral("reduce-nested");
    nested.name = QStringLiteral("Nested");
    nested.videoTracks = {QVector<ClipInfo>{child}};
    ClipInfo nestedReference;
    nestedReference.filePath = timeline_nesting::sequenceClipFilePath(nested.id);
    nestedReference.duration = nestedReference.outPoint = 0.1;
    active.videoTracks.append(QVector<ClipInfo>{nestedReference});
    {
        // These gates inspect references, not playback; avoid starting decoders.
        const QSignalBlocker blocker(timeline);
        timeline->setSequences({active, nested}, active.id);
    }
    OverlayItem overlay;
    overlay.type = QStringLiteral("image");
    overlay.text = paths[3];
    window.*member(Overlays{}) = {overlay};
    (window.*member(Particles{})).clear();
    (window.*member(Particles{})).insert(paths[4], ParticleEmitterConfig{});
    dock->refresh();

    // Run the retained original provider, not a reimplementation of it.
    dock->setProperty("reduceProjectLegacyPathsForSelftest", true);
    const auto legacyUnused = pool.filtered(QString(), mediapool::AssetFilterMode::Unused,
                                            (dock->*member(Provider{}))());
    dock->setProperty("reduceProjectLegacyPathsForSelftest", QVariant());
    bool extensionProven = true;
    for (int i : {3, 4, 5, 6}) {
        bool found = false;
        for (const auto &asset : legacyUnused) found = found || asset.id == ids[i];
        extensionProven = extensionProven && found;
    }
    auto *filter = dock->findChild<QComboBox *>(QStringLiteral("MediaPoolFilter"));
    auto *assetList = dock->findChild<QListWidget *>(QStringLiteral("MediaPoolAssetList"));
    if (filter) filter->setCurrentIndex(filter->findData(int(mediapool::AssetFilterMode::Unused)));
    const bool unusedDisplayCorrect = assetList && assetList->count() == 2;

    int changes = 0;
    QObject::connect(dock, &MediaPoolDock::poolChanged, &window, [&] { ++changes; });
    const auto confirm = [&](bool accept) {
        bool inspected = false;
        // The timer runs inside onRemoveUnused's modal dialog event loop.
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &window, [&] {
            auto *dialog = dock->findChild<QDialog *>(QStringLiteral("ReduceProjectConfirmation"));
            if (!dialog) {
                if (auto *other = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                    other->reject();
                return;
            }
            auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("ReduceProjectButtons"));
            auto *list = dialog->findChild<QListWidget *>(QStringLiteral("ReduceProjectList"));
            auto *label = dialog->findChild<QLabel *>(QStringLiteral("ReduceProjectMessage"));
            const auto firstName = [](const auto &found) {
                return found.isEmpty() ? QString() : found.first()->objectName();
            };
            std::fprintf(stderr,
                "[reduce-project] confirm-dialog found=%s/%s/%s typeOnlyFirst=%s/%s/%s\n",
                buttons ? qPrintable(buttons->objectName()) : "(null)",
                list ? qPrintable(list->objectName()) : "(null)",
                label ? qPrintable(label->objectName()) : "(null)",
                qPrintable(firstName(dialog->findChildren<QDialogButtonBox *>())),
                qPrintable(firstName(dialog->findChildren<QListWidget *>())),
                qPrintable(firstName(dialog->findChildren<QLabel *>())));
            QPushButton *yes = nullptr, *no = nullptr;
            if (buttons) {
                for (auto *button : buttons->buttons()) {
                    if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole)
                        yes = qobject_cast<QPushButton *>(button);
                    if (buttons->buttonRole(button) == QDialogButtonBox::RejectRole)
                        no = qobject_cast<QPushButton *>(button);
                }
            }
            inspected = yes && no && no->isDefault() && !yes->isDefault()
                && no->text() == QStringLiteral("いいえ") && list && list->count() == 2
                && list->item(0)->text().contains(names[7])
                && list->item(1)->text().contains(names[8]) && label
                && label->text().contains(QStringLiteral("2 件"))
                && label->text().contains(QStringLiteral("この操作は元に戻せません。ディスク上のファイルは削除されないので、必要になれば再度読み込めます"));
            if (yes && no) (accept ? yes : no)->click();
            else dialog->reject();
        });
        timer.start(0);
        const bool invoked = QMetaObject::invokeMethod(dock, "onRemoveUnused", Qt::DirectConnection);
        return invoked && inspected;
    };
    const auto beforeCancel = pool.toJson();
    const bool cancelled = confirm(false) && beforeCancel == pool.toJson() && changes == 0;
    const bool confirmed = confirm(true);
    gate(1, ready && confirmed && changes == 1 && pool.assets().size() == 7
        && pool.getAsset(ids[0]) && pool.getAsset(ids[1])
        && !pool.getAsset(ids[7]) && !pool.getAsset(ids[8]) && window.isWindowModified());
    gate(2, ready && confirmed && pool.getAsset(ids[2]));
    gate(3, ready && confirmed && extensionProven && unusedDisplayCorrect
        && pool.getAsset(ids[3]) && pool.getAsset(ids[4])
        && pool.getAsset(ids[5]) && pool.getAsset(ids[6]));
    bool filesIntact = ready;
    for (const auto &path : paths) filesIntact = QFileInfo::exists(path) && filesIntact;
    gate(4, confirmed && cancelled && filesIntact);
    window.setWindowModified(false);
    std::fprintf(stderr, "[reduce-project] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
