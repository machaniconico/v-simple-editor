#pragma once

// src/MediaPoolDock.h
// MP-4: MediaPool の UI ドック。左にビン階層 (QTreeWidget)、右に素材一覧
// (QListWidget / IconMode)、上部に検索ボックスを持つ。MediaPool は所有せず
// 表示対象を指すだけ (pool=nullptr のときは安全に no-op)。

#include <QDockWidget>
#include <QListWidget>
#include <QStringList>
#include <functional>

#include "MediaPool.h"

class QTreeWidget;
class QTreeWidgetItem;
class QListWidgetItem;
class QLineEdit;
class QMimeData;
class QComboBox;
class QPushButton;

class MediaPoolAssetListWidget : public QListWidget
{
public:
    explicit MediaPoolAssetListWidget(QWidget *parent = nullptr);

    QMimeData *createMimeDataForItems(const QList<QListWidgetItem *> &items) const;

protected:
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
};

class MediaPoolDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit MediaPoolDock(QWidget *parent = nullptr);

    // 表示対象を設定する。所有はしない (呼び出し側がライフタイム管理)。
    void setPool(mediapool::MediaPool *pool);

    // pool の状態からビンツリーと素材一覧を再描画する。
    void refresh();
    void setUsedPathsProvider(std::function<QSet<QString>()> provider);
    void refreshUsedPaths();

    // 素材一覧で現在選択されている項目のパス。選択なしは空文字列。
    QString selectedAssetPath() const;

signals:
    // 素材をダブルクリックしたとき (MainWindow がタイムラインへ取り込む)。
    void assetActivated(const QString &filePath);
    // 「読み込み...」ボタン押下時。
    void importRequested();
    void poolChanged();

private slots:
    void onSearchTextChanged(const QString &text);
    void onBinSelectionChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void onAssetDoubleClicked(QListWidgetItem *item);
    void onAddBin();
    void onRemoveSelected();
    void onImportClicked();

private:
    void showAssetContextMenu(const QPoint &pos);
    void showBinContextMenu(const QPoint &pos);
    void rebuildBinTree();
    void showAssets(const QVector<mediapool::MediaAsset> &assets);
    void showAssetsForCurrentBin();
    void addBinItems(QTreeWidgetItem *parentItem, const QString &parentId);

    // 現在選択中のビン id を返す ("" = ルート / すべてのメディア)。
    QString currentBinId() const;

    mediapool::MediaPool *m_pool = nullptr;

    std::function<QSet<QString>()> m_usedPathsProvider;
    QComboBox *m_filterCombo = nullptr;

    QLineEdit   *m_searchEdit  = nullptr;
    QTreeWidget *m_binTree     = nullptr;
    QListWidget *m_assetList   = nullptr;
    QPushButton *m_addBinBtn   = nullptr;
    QPushButton *m_removeBtn   = nullptr;
    QPushButton *m_importBtn   = nullptr;

    // ツリー再構築中の currentItemChanged を無視するためのガード。
    bool m_rebuilding = false;
};
