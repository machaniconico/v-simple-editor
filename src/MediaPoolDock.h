#pragma once

// src/MediaPoolDock.h
// MP-4: MediaPool の UI ドック。左にビン階層 (QTreeWidget)、右に素材一覧
// (QListWidget / IconMode)、上部に検索ボックスを持つ。MediaPool は所有せず
// 表示対象を指すだけ (pool=nullptr のときは安全に no-op)。

#include <QDockWidget>

#include "MediaPool.h"

class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QPushButton;

class MediaPoolDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit MediaPoolDock(QWidget *parent = nullptr);

    // 表示対象を設定する。所有はしない (呼び出し側がライフタイム管理)。
    void setPool(mediapool::MediaPool *pool);

    // pool の状態からビンツリーと素材一覧を再描画する。
    void refresh();

signals:
    // 素材をダブルクリックしたとき (MainWindow がタイムラインへ取り込む)。
    void assetActivated(const QString &filePath);
    // 「読み込み...」ボタン押下時。
    void importRequested();

private slots:
    void onSearchTextChanged(const QString &text);
    void onBinSelectionChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void onAssetDoubleClicked(QListWidgetItem *item);
    void onAddBin();
    void onRemoveSelected();
    void onImportClicked();

private:
    void rebuildBinTree();
    void showAssets(const QVector<mediapool::MediaAsset> &assets);
    void showAssetsForCurrentBin();
    void addBinItems(QTreeWidgetItem *parentItem, const QString &parentId);

    // 現在選択中のビン id を返す ("" = ルート / すべてのメディア)。
    QString currentBinId() const;

    mediapool::MediaPool *m_pool = nullptr;

    QLineEdit   *m_searchEdit  = nullptr;
    QTreeWidget *m_binTree     = nullptr;
    QListWidget *m_assetList   = nullptr;
    QPushButton *m_addBinBtn   = nullptr;
    QPushButton *m_removeBtn   = nullptr;
    QPushButton *m_importBtn   = nullptr;

    // ツリー再構築中の currentItemChanged を無視するためのガード。
    bool m_rebuilding = false;
};
