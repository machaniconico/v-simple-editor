#include "../MediaPool.h"
#include "../MediaPoolDock.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <cstdio>

namespace {
QTreeWidgetItem *findBin(QTreeWidget *tree, int kind, const QString &value)
{
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole + 1).toInt() == kind
            && (*it)->data(0, Qt::UserRole).toString() == value)
            return *it;
    }
    return nullptr;
}

QSet<int> visibleIds(QListWidget *list)
{
    QSet<int> ids;
    for (int i = 0; i < list->count(); ++i)
        ids.insert(list->item(i)->data(Qt::UserRole).toInt());
    return ids;
}

bool editKeywords(MediaPoolDock &dock, const QString &text, bool accept = true)
{
    bool opened = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &dock, [&] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        opened = true;
        dialog->setTextValue(text);
        if (accept) dialog->accept();
        else dialog->reject();
    });
    timer.start(0);
    const bool invoked = QMetaObject::invokeMethod(&dock, "editSelectedKeywords", Qt::DirectConnection);
    return invoked && opened;
}

bool editSmart(MediaPoolDock &dock, const QString &id, const QString &tag)
{
    bool opened = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &dock, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("SmartBinName"));
        auto *keyword = dialog->findChild<QLineEdit *>(QStringLiteral("SmartBinTag"));
        auto *query = dialog->findChild<QLineEdit *>(QStringLiteral("SmartBinQuery"));
        auto *type = dialog->findChild<QComboBox *>(QStringLiteral("SmartBinType"));
        if (!name || !keyword || !query || !type) { dialog->reject(); return; }
        opened = true;
        name->setText(QStringLiteral("撮影素材"));
        keyword->setText(tag);
        query->setText(QStringLiteral("TAKE"));
        type->setCurrentIndex(type->findData(static_cast<int>(mediapool::MediaType::Audio)));
        dialog->accept();
    });
    timer.start(0);
    const bool invoked = QMetaObject::invokeMethod(&dock, "editSmartBin", Qt::DirectConnection,
                                                  Q_ARG(QString, id));
    return invoked && opened;
}
} // namespace

int runKeywordBinsSelftest()
{
    using namespace mediapool;
    int passed = 0, failed = 0;
    const auto check = [&](int gate, bool ok) {
        ok ? ++passed : ++failed;
        std::fprintf(stderr, "[keyword-bins] %s G%d\n", ok ? "PASS" : "FAIL", gate);
    };
    MediaPool pool;
    QVector<int> ids;
    for (int i = 0; i < 4; ++i) {
        MediaAsset asset;
        // Audio metadata only: no decoder, image fixture or thumbnail worker needed.
        asset.type = MediaType::Audio;
        asset.filePath = QStringLiteral("keyword-test-%1.wav").arg(i);
        asset.displayName = i == 2 ? QStringLiteral("other") : QStringLiteral("take %1").arg(i);
        ids.append(pool.addAsset(asset));
    }
    MediaPoolDock dock;
    dock.setPool(&pool);
    auto *tree = dock.findChild<QTreeWidget *>(QStringLiteral("MediaPoolBinTree"));
    auto *list = dock.findChild<QListWidget *>(QStringLiteral("MediaPoolAssetList"));
    int changes = 0;
    QObject::connect(&dock, &MediaPoolDock::poolChanged, &dock, [&] { ++changes; });
    if (!tree || !list) {
        for (int gate = 1; gate <= 4; ++gate) check(gate, false);
    } else {
        pool.setAssetTags(ids[0], {QStringLiteral("固有")});
        dock.refresh();
        list->item(0)->setSelected(true);
        list->item(1)->setSelected(true);
        bool g1 = editKeywords(dock, QStringLiteral(" 共通 \n共通\n"));
        auto *keyword = findBin(tree, 1, QStringLiteral("共通"));
        g1 = g1 && keyword && changes == 1
            && pool.getAsset(ids[0])->tags.contains(QStringLiteral("固有"))
            && pool.getAsset(ids[1])->tags == QStringList{QStringLiteral("共通")};
        if (keyword) tree->setCurrentItem(keyword);
        g1 = g1 && visibleIds(list) == QSet<int>{ids[0], ids[1]};
        check(1, g1);

        list->selectAll();
        const auto beforeCancel = pool.toJson();
        bool g2 = editKeywords(dock, QStringLiteral("取消"), false)
            && beforeCancel == pool.toJson() && changes == 1;
        g2 = editKeywords(dock, QString()) && g2;
        g2 = g2 && !findBin(tree, 1, QStringLiteral("共通")) && changes == 2
            && findBin(tree, 1, QStringLiteral("固有"))
            && pool.getAsset(ids[0])->tags == QStringList{QStringLiteral("固有")}
            && pool.getAsset(ids[1])->tags.isEmpty()
            && list->count() == 4; // Deleted virtual bin falls back to the root.
        pool.setAssetTags(ids[0], {});
        dock.refresh();
        g2 = g2 && tree->topLevelItemCount() == 1 && !pool.setAssetTags(-1, {});
        check(2, g2);

        pool.setAssetTags(ids[0], {QStringLiteral("Outdoor")});
        pool.setAssetTags(ids[1], {QStringLiteral("OUTDOOR close")});
        pool.setAssetTags(ids[2], {QStringLiteral("Outdoor")});
        dock.refresh();
        bool g3 = editSmart(dock, QString(), QStringLiteral("outdoor"))
            && pool.smartBins().size() == 1;
        const QString smartId = pool.smartBins().isEmpty() ? QString() : pool.smartBins().first().id;
        const auto matchesEngine = [&] {
            auto *item = findBin(tree, 2, smartId);
            if (!item) return false;
            tree->setCurrentItem(item);
            QSet<int> expected;
            for (const auto &asset : pool.evaluateSmartBin(smartId)) expected.insert(asset.id);
            return visibleIds(list) == expected;
        };
        g3 = matchesEngine() && g3 && visibleIds(list) == QSet<int>{ids[0], ids[1]};
        g3 = editSmart(dock, smartId, QStringLiteral("close")) && g3;
        g3 = matchesEngine() && g3 && visibleIds(list) == QSet<int>{ids[1]};
        MediaPool restored;
        restored.fromJson(QJsonDocument::fromJson(QJsonDocument(pool.toJson()).toJson()).object());
        g3 = g3 && restored.toJson() == pool.toJson()
            && restored.getAsset(ids[0])->tags == pool.getAsset(ids[0])->tags
            && restored.evaluateSmartBin(smartId).size() == 1;
        dock.setPool(&restored);
        QSet<int> restoredIds;
        for (const auto &asset : restored.evaluateSmartBin(smartId)) restoredIds.insert(asset.id);
        auto *restoredBin = findBin(tree, 2, smartId);
        if (restoredBin) tree->setCurrentItem(restoredBin);
        g3 = g3 && restoredBin && visibleIds(list) == restoredIds;
        g3 = restored.removeSmartBin(smartId) && g3;
        dock.refresh();
        g3 = g3 && !findBin(tree, 2, smartId) && list->count() == 4
            && !restored.removeSmartBin(smartId) && !restored.updateSmartBin(smartId, SmartBin{});
        check(3, g3);
        dock.setPool(&pool);
        pool.removeSmartBin(smartId);
        for (int id : ids) pool.setAssetTags(id, {});
        dock.refresh();
        QSet<int> expected;
        for (const auto &asset : pool.filtered(QString(), AssetFilterMode::All)) expected.insert(asset.id);
        bool g4 = tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 0
            && visibleIds(list) == expected && list->count() == 4;
        const QString folder = pool.createBin(QStringLiteral("通常ビン"));
        pool.moveAssetToBin(ids[0], folder);
        dock.refresh();
        tree->setCurrentItem(findBin(tree, 0, folder));
        g4 = g4 && visibleIds(list) == QSet<int>{ids[0]};
        check(4, g4);
    }
    std::fprintf(stderr, "[keyword-bins] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
