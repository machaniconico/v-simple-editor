#include "../MediaPoolDock.h"

#include "SelftestRegistry.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QListWidget>
#include <QMimeData>
#include <QScopedPointer>
#include <QSet>
#include <QUrl>

namespace {

mediapool::MediaAsset makeDragAsset(const QString &filePath,
                                    const QString &displayName)
{
    mediapool::MediaAsset asset;
    asset.filePath = filePath;
    asset.displayName = displayName;
    asset.type = mediapool::MediaType::Video;
    asset.importedAtIso = QStringLiteral("2026-06-12T00:00:00Z");
    return asset;
}

} // namespace

int runMediaPoolDragSelftest()
{
    qInfo().noquote() << "[mediapool-drag] selftest start";

    QString error;
    bool ok = true;
    auto expect = [&](bool condition, const QString &message) {
        if (selftests::requireSelftest(condition, message, &error)) {
            qInfo().noquote() << "[mediapool-drag] PASS" << message;
            return true;
        }
        qWarning().noquote() << "[mediapool-drag] FAIL" << error;
        ok = false;
        return false;
    };

    if (!expect(QApplication::instance() != nullptr,
                QStringLiteral("QApplication is available"))) {
        return 1;
    }

    const QString pathA = QDir::temp().absoluteFilePath(QStringLiteral("mediapool-drag-a.mp4"));
    const QString pathB = QDir::temp().absoluteFilePath(QStringLiteral("mediapool-drag-b.wav"));

    mediapool::MediaPool pool;
    pool.addAsset(makeDragAsset(pathA, QStringLiteral("Drag A")));
    pool.addAsset(makeDragAsset(pathB, QStringLiteral("Drag B")));

    MediaPoolDock dock;
    dock.setPool(&pool);

    QListWidget *baseList =
        dock.findChild<QListWidget *>(QStringLiteral("MediaPoolAssetList"));
    MediaPoolAssetListWidget *assetList =
        dynamic_cast<MediaPoolAssetListWidget *>(baseList);

    if (!expect(assetList != nullptr,
                QStringLiteral("media pool asset list widget is discoverable"))) {
        return 1;
    }

    ok &= expect(assetList->dragEnabled(),
                 QStringLiteral("asset list has drag enabled"));
    ok &= expect(assetList->selectionMode() == QAbstractItemView::ExtendedSelection,
                 QStringLiteral("asset list allows multiple selection"));
    ok &= expect(assetList->count() == 2,
                 QStringLiteral("asset list is populated from the media pool"));

    if (!ok) {
        return 1;
    }

    QList<QListWidgetItem *> singleItems;
    singleItems.push_back(assetList->item(0));
    QScopedPointer<QMimeData> singleMime(assetList->createMimeDataForItems(singleItems));
    const QList<QUrl> singleUrls = singleMime->urls();
    ok &= expect(singleMime->hasUrls() && singleUrls.size() == 1,
                 QStringLiteral("single drag returns one URL"));
    ok &= expect(singleUrls.size() == 1
                     && singleUrls.first().isLocalFile()
                     && singleUrls.first().toLocalFile() == pathA,
                 QStringLiteral("single drag URL matches the asset file path"));

    assetList->clearSelection();
    assetList->item(0)->setSelected(true);
    assetList->item(1)->setSelected(true);

    const QList<QListWidgetItem *> selectedItems = assetList->selectedItems();
    QScopedPointer<QMimeData> multiMime(assetList->createMimeDataForItems(selectedItems));
    const QList<QUrl> multiUrls = multiMime->urls();
    QSet<QString> multiPaths;
    for (const QUrl &url : multiUrls) {
        if (url.isLocalFile()) {
            multiPaths.insert(url.toLocalFile());
        }
    }

    ok &= expect(selectedItems.size() == 2,
                 QStringLiteral("multiple selected items are included in the drag"));
    ok &= expect(multiMime->hasUrls() && multiUrls.size() == 2,
                 QStringLiteral("multi drag returns two URLs"));
    ok &= expect(multiPaths.contains(pathA) && multiPaths.contains(pathB),
                 QStringLiteral("multi drag URLs match the selected asset file paths"));

    qInfo().noquote().nospace()
        << "[mediapool-drag] selftest end, failed=" << (ok ? 0 : 1);
    return ok ? 0 : 1;
}
