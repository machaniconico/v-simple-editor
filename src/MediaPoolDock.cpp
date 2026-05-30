#include "MediaPoolDock.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLineEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QInputDialog>
#include <QVariant>
#include <QSize>

namespace {
// ルート (すべてのメディア) を表す特別な binId。空文字列で表現する。
const char *kRootName = "すべてのメディア";

// QListWidgetItem に asset id を持たせるための role。
constexpr int kAssetIdRole   = Qt::UserRole;
constexpr int kAssetPathRole = Qt::UserRole + 1;
// QTreeWidgetItem に bin id を持たせるための role。
constexpr int kBinIdRole     = Qt::UserRole;
} // namespace

MediaPoolDock::MediaPoolDock(QWidget *parent)
    : QDockWidget(tr("メディアプール"), parent)
{
    setObjectName(QStringLiteral("MediaPoolDock"));

    QWidget *root = new QWidget(this);
    QVBoxLayout *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    // --- 検索ボックス -------------------------------------------------------
    m_searchEdit = new QLineEdit(root);
    m_searchEdit->setPlaceholderText(tr("検索..."));
    m_searchEdit->setClearButtonEnabled(true);
    rootLayout->addWidget(m_searchEdit);

    // --- 左: ビンツリー / 右: 素材一覧 (横 Splitter) ------------------------
    QSplitter *splitter = new QSplitter(Qt::Horizontal, root);

    m_binTree = new QTreeWidget(splitter);
    m_binTree->setHeaderHidden(true);
    m_binTree->setColumnCount(1);

    m_assetList = new QListWidget(splitter);
    m_assetList->setViewMode(QListView::IconMode);
    m_assetList->setResizeMode(QListView::Adjust);
    m_assetList->setMovement(QListView::Static);
    m_assetList->setIconSize(QSize(64, 64));
    m_assetList->setGridSize(QSize(96, 96));
    m_assetList->setWordWrap(true);

    splitter->addWidget(m_binTree);
    splitter->addWidget(m_assetList);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    rootLayout->addWidget(splitter, 1);

    // --- 下部ボタン行 -------------------------------------------------------
    QHBoxLayout *btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);

    m_addBinBtn = new QPushButton(tr("ビン追加"), root);
    m_removeBtn = new QPushButton(tr("削除"), root);
    m_importBtn = new QPushButton(tr("読み込み..."), root);

    btnRow->addWidget(m_addBinBtn);
    btnRow->addWidget(m_removeBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(m_importBtn);
    rootLayout->addLayout(btnRow);

    setWidget(root);

    // --- シグナル接続 -------------------------------------------------------
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &MediaPoolDock::onSearchTextChanged);
    connect(m_binTree, &QTreeWidget::currentItemChanged,
            this, &MediaPoolDock::onBinSelectionChanged);
    connect(m_assetList, &QListWidget::itemDoubleClicked,
            this, &MediaPoolDock::onAssetDoubleClicked);
    connect(m_addBinBtn, &QPushButton::clicked,
            this, &MediaPoolDock::onAddBin);
    connect(m_removeBtn, &QPushButton::clicked,
            this, &MediaPoolDock::onRemoveSelected);
    connect(m_importBtn, &QPushButton::clicked,
            this, &MediaPoolDock::onImportClicked);

    refresh();
}

void MediaPoolDock::setPool(mediapool::MediaPool *pool)
{
    m_pool = pool;
    refresh();
}

// ---------------------------------------------------------------------------
// 再描画
// ---------------------------------------------------------------------------

void MediaPoolDock::refresh()
{
    rebuildBinTree();

    // 検索文字列が入っていれば検索結果、なければ選択ビンの素材を出す。
    const QString query = m_searchEdit ? m_searchEdit->text() : QString();
    if (!query.isEmpty() && m_pool) {
        showAssets(m_pool->search(query));
    } else {
        showAssetsForCurrentBin();
    }
}

void MediaPoolDock::rebuildBinTree()
{
    m_rebuilding = true;
    m_binTree->clear();

    // ルート項目「すべてのメディア」(binId = "")。
    QTreeWidgetItem *rootItem =
        new QTreeWidgetItem(m_binTree, QStringList(tr(kRootName)));
    rootItem->setData(0, kBinIdRole, QString());

    if (m_pool) {
        addBinItems(rootItem, QString());
    }

    rootItem->setExpanded(true);
    m_binTree->setCurrentItem(rootItem);
    m_rebuilding = false;
}

void MediaPoolDock::addBinItems(QTreeWidgetItem *parentItem, const QString &parentId)
{
    if (!m_pool) {
        return;
    }
    const QVector<mediapool::MediaBin> &bins = m_pool->bins();
    for (const mediapool::MediaBin &bin : bins) {
        if (bin.parentId != parentId) {
            continue;
        }
        QTreeWidgetItem *item =
            new QTreeWidgetItem(parentItem, QStringList(bin.name));
        item->setData(0, kBinIdRole, bin.id);
        item->setExpanded(true);
        // 子ビンを再帰的に追加。
        addBinItems(item, bin.id);
    }
}

void MediaPoolDock::showAssetsForCurrentBin()
{
    if (!m_pool) {
        m_assetList->clear();
        return;
    }

    QTreeWidgetItem *cur = m_binTree ? m_binTree->currentItem() : nullptr;
    if (!cur || !cur->parent()) {
        showAssets(m_pool->assets());
        return;
    }

    showAssets(m_pool->assetsInBin(currentBinId()));
}

void MediaPoolDock::showAssets(const QVector<mediapool::MediaAsset> &assets)
{
    m_assetList->clear();
    for (const mediapool::MediaAsset &asset : assets) {
        const QString label =
            asset.displayName.isEmpty() ? asset.filePath : asset.displayName;
        QListWidgetItem *item = new QListWidgetItem(label, m_assetList);
        item->setData(kAssetIdRole, asset.id);
        item->setData(kAssetPathRole, asset.filePath);
        item->setToolTip(asset.filePath);
    }
}

QString MediaPoolDock::currentBinId() const
{
    QTreeWidgetItem *cur = m_binTree ? m_binTree->currentItem() : nullptr;
    if (!cur) {
        return QString();
    }
    return cur->data(0, kBinIdRole).toString();
}

// ---------------------------------------------------------------------------
// スロット
// ---------------------------------------------------------------------------

void MediaPoolDock::onSearchTextChanged(const QString &text)
{
    if (!m_pool) {
        m_assetList->clear();
        return;
    }
    if (text.isEmpty()) {
        // 検索クリア時は選択中ビンの素材へ戻す。
        showAssetsForCurrentBin();
        return;
    }
    showAssets(m_pool->search(text));
}

void MediaPoolDock::onBinSelectionChanged(QTreeWidgetItem *current,
                                          QTreeWidgetItem *previous)
{
    Q_UNUSED(current);
    Q_UNUSED(previous);
    if (m_rebuilding) {
        return;
    }
    // ビン切替時は検索を解除して、そのビンの素材を出す。
    if (m_searchEdit && !m_searchEdit->text().isEmpty()) {
        // textChanged → onSearchTextChanged 経由で再描画される。
        m_searchEdit->clear();
        return;
    }
    showAssetsForCurrentBin();
}

void MediaPoolDock::onAssetDoubleClicked(QListWidgetItem *item)
{
    if (!item) {
        return;
    }
    const QString filePath = item->data(kAssetPathRole).toString();
    if (!filePath.isEmpty()) {
        emit assetActivated(filePath);
    }
}

void MediaPoolDock::onAddBin()
{
    if (!m_pool) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("ビン追加"), tr("ビン名:"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    // 選択中のビンを親にして作成する (ルート選択時は parentId="")。
    m_pool->createBin(name.trimmed(), currentBinId());
    refresh();
}

void MediaPoolDock::onRemoveSelected()
{
    if (!m_pool) {
        return;
    }

    // 素材リストで選択されていれば asset を、なければ選択中ビンを削除する。
    QListWidgetItem *assetItem = m_assetList->currentItem();
    if (assetItem) {
        bool ok = false;
        const int assetId = assetItem->data(kAssetIdRole).toInt(&ok);
        if (ok && assetId >= 0) {
            m_pool->removeAsset(assetId);
            refresh();
            return;
        }
    }

    // ビン削除 (ルート「すべてのメディア」は削除不可)。
    const QString binId = currentBinId();
    if (!binId.isEmpty()) {
        m_pool->removeBin(binId);
        refresh();
    }
}

void MediaPoolDock::onImportClicked()
{
    emit importRequested();
}
