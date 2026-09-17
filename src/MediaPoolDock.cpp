#include "MediaPoolDock.h"
#include "ThumbnailCache.h"
#include <QSettings>
#include <QMouseEvent>
#include <QPixmap>
#include <QIcon>

#include <QAbstractItemView>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLineEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMimeData>
#include <QPushButton>
#include <QInputDialog>
#include <QUrl>
#include <QVariant>
#include <QSize>
#include <QComboBox>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QFont>
#include <QBrush>
#include <QTreeWidgetItemIterator>
#include <utility>

namespace {
// ルート (すべてのメディア) を表す特別な binId。空文字列で表現する。
const char *kRootName = "すべてのメディア";

// QListWidgetItem に asset id を持たせるための role。
constexpr int kAssetIdRole   = Qt::UserRole;
constexpr int kAssetPathRole = Qt::UserRole + 1;
// QTreeWidgetItem に bin id を持たせるための role。
constexpr int kBinIdRole     = Qt::UserRole;
} // namespace

MediaPoolAssetListWidget::MediaPoolAssetListWidget(QWidget *parent)
    : QListWidget(parent)
{
}

QStringList MediaPoolAssetListWidget::mimeTypes() const
{
    return { QStringLiteral("text/uri-list") };
}

QMimeData *MediaPoolAssetListWidget::mimeData(const QList<QListWidgetItem *> &items) const
{
    return createMimeDataForItems(items);
}

QMimeData *MediaPoolAssetListWidget::createMimeDataForItems(const QList<QListWidgetItem *> &items) const
{
    QList<QUrl> urls;
    urls.reserve(items.size());

    for (const QListWidgetItem *item : items) {
        if (!item) {
            continue;
        }
        const QString filePath = item->data(kAssetPathRole).toString();
        if (!filePath.isEmpty()) {
            urls.push_back(QUrl::fromLocalFile(filePath));
        }
    }

    QMimeData *data = new QMimeData;
    if (!urls.isEmpty()) {
        data->setUrls(urls);
    }
    return data;
}

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
    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(m_searchEdit, 1);
    m_filterCombo = new QComboBox(root);
    m_filterCombo->setObjectName(QStringLiteral("MediaPoolFilter"));
    m_filterCombo->addItem(tr("すべて"), static_cast<int>(mediapool::AssetFilterMode::All));
    m_filterCombo->addItem(tr("お気に入り"), static_cast<int>(mediapool::AssetFilterMode::Favorites));
    m_filterCombo->addItem(tr("却下を除く"), static_cast<int>(mediapool::AssetFilterMode::ExcludeRejected));
    m_filterCombo->addItem(tr("未使用"), static_cast<int>(mediapool::AssetFilterMode::Unused));
    searchRow->addWidget(m_filterCombo);
    rootLayout->addLayout(searchRow);

    // --- 左: ビンツリー / 右: 素材一覧 (横 Splitter) ------------------------
    QSplitter *splitter = new QSplitter(Qt::Horizontal, root);

    m_binTree = new QTreeWidget(splitter);
    m_binTree->setHeaderHidden(true);
    m_binTree->setColumnCount(1);

    m_assetList = new MediaPoolAssetListWidget(splitter);
    m_assetList->setObjectName(QStringLiteral("MediaPoolAssetList"));
    m_assetList->setViewMode(QListView::IconMode);
    m_assetList->setResizeMode(QListView::Adjust);
    m_assetList->setMovement(QListView::Static);
    m_assetList->setIconSize(QSize(96, 54));
    m_assetList->setGridSize(QSize(112, 96));
    m_assetList->setWordWrap(true);
    m_assetList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_assetList->setDragEnabled(true);
    m_assetList->setDragDropMode(QAbstractItemView::DragOnly);
    m_assetList->setDefaultDropAction(Qt::CopyAction);

    m_thumbnailsEnabled = QSettings().value(QStringLiteral("mediaPool/showThumbnails"), true).toBool();
    m_thumbnailCache = new ThumbnailCache(this);
    m_assetList->viewport()->setMouseTracking(true);
    m_assetList->viewport()->installEventFilter(this);
    connect(m_thumbnailCache, &ThumbnailCache::ready, this,
            [this](const QString &key, const QVector<QImage> &frames) {
        if (!m_thumbnailsEnabled || frames.isEmpty()) return;
        for (int i = 0; i < m_assetList->count(); ++i) {
            auto *item = m_assetList->item(i);
            if (item->data(kAssetPathRole).toString() == key)
                item->setIcon(QIcon(QPixmap::fromImage(frames.first())));
        }
    });

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

    m_assetList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_binTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_assetList, &QWidget::customContextMenuRequested,
            this, &MediaPoolDock::showAssetContextMenu);
    connect(m_binTree, &QWidget::customContextMenuRequested,
            this, &MediaPoolDock::showBinContextMenu);
    connect(m_filterCombo, &QComboBox::currentIndexChanged,
            this, [this] { showAssetsForCurrentBin(); });
    refresh();
}

void MediaPoolDock::setPool(mediapool::MediaPool *pool)
{
    m_hoverItem = nullptr;
    m_thumbnailCache->clear();
    m_pool = pool;
    refresh();
}

// ---------------------------------------------------------------------------
// 再描画
// ---------------------------------------------------------------------------

void MediaPoolDock::refresh()
{
    rebuildBinTree();

    showAssetsForCurrentBin();
}

QString MediaPoolDock::selectedAssetPath() const
{
    const QListWidgetItem *item = m_assetList ? m_assetList->currentItem() : nullptr;
    return item && item->isSelected()
        ? item->data(kAssetPathRole).toString() : QString();
}

void MediaPoolDock::rebuildBinTree()
{
    const QString selectedBin = currentBinId();
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
    for (QTreeWidgetItemIterator it(m_binTree); *it; ++it) {
        if ((*it)->data(0, kBinIdRole).toString() == selectedBin) {
            m_binTree->setCurrentItem(*it);
            break;
        }
    }
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
        m_hoverItem = nullptr;
        m_assetList->clear();
        return;
    }

    const auto mode = static_cast<mediapool::AssetFilterMode>(m_filterCombo->currentData().toInt());
    const QSet<QString> usedPaths = mode == mediapool::AssetFilterMode::Unused && m_usedPathsProvider
        ? m_usedPathsProvider() : QSet<QString>();
    const auto filtered = m_pool->filtered(m_searchEdit->text(), mode, usedPaths);
    QVector<mediapool::MediaAsset> visible;
    const QString binId = currentBinId();
    for (const auto &asset : filtered) {
        if (binId.isEmpty() || asset.binId == binId)
            visible.append(asset);
    }
    showAssets(visible);
}

void MediaPoolDock::showAssets(const QVector<mediapool::MediaAsset> &assets)
{
    m_hoverItem = nullptr;
    m_assetList->clear();
    for (const mediapool::MediaAsset &asset : assets) {
        QString label =
            asset.displayName.isEmpty() ? asset.filePath : asset.displayName;
        if (asset.stars > 0)
            label += tr(" ★%1").arg(asset.stars);
        QListWidgetItem *item = new QListWidgetItem(label, m_assetList);
        QFont font = item->font();
        font.setBold(asset.flag == mediapool::AssetFlag::Favorite);
        item->setFont(font);
        if (asset.flag == mediapool::AssetFlag::Rejected)
            item->setForeground(QBrush(Qt::gray));
        item->setData(kAssetIdRole, asset.id);
        item->setData(kAssetPathRole, asset.filePath);
        item->setToolTip(asset.filePath);
        if (m_thumbnailsEnabled && asset.type != mediapool::MediaType::Audio) {
            setThumbnail(item);
            m_thumbnailCache->request(asset.filePath, asset.filePath, asset.durationMs / 1000.0);
        }
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
    Q_UNUSED(text);
    showAssetsForCurrentBin();
}

void MediaPoolDock::onBinSelectionChanged(QTreeWidgetItem *current,
                                          QTreeWidgetItem *previous)
{
    Q_UNUSED(current);
    Q_UNUSED(previous);
    if (m_rebuilding) {
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
    emit poolChanged();
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
            if (m_pool->removeAsset(assetId))
                emit poolChanged();
            refresh();
            return;
        }
    }

    // ビン削除 (ルート「すべてのメディア」は削除不可)。
    const QString binId = currentBinId();
    if (!binId.isEmpty()) {
        if (m_pool->removeBin(binId))
            emit poolChanged();
        refresh();
    }
}

void MediaPoolDock::onImportClicked()
{
    emit importRequested();
}

void MediaPoolDock::setUsedPathsProvider(std::function<QSet<QString>()> provider)
{
    m_usedPathsProvider = std::move(provider);
    refreshUsedPaths();
}

void MediaPoolDock::refreshUsedPaths()
{
    if (static_cast<mediapool::AssetFilterMode>(m_filterCombo->currentData().toInt())
        == mediapool::AssetFilterMode::Unused)
        showAssetsForCurrentBin();
}

void MediaPoolDock::showAssetContextMenu(const QPoint &pos)
{
    if (!m_pool)
        return;
    QListWidgetItem *item = m_assetList->itemAt(pos);
    if (!item)
        return;
    m_assetList->setCurrentItem(item);
    const int id = item->data(kAssetIdRole).toInt();
    const auto *asset = m_pool->getAsset(id);
    if (!asset)
        return;
    // メニューのイベントループ中にモデルや一覧が更新されても参照を保持しない。
    const auto selected = *asset;
    QMenu menu(this);
    QAction *rename = menu.addAction(tr("名前を変更…"));
    QAction *favorite = menu.addAction(tr("お気に入り"));
    favorite->setCheckable(true);
    favorite->setChecked(selected.flag == mediapool::AssetFlag::Favorite);
    QAction *rejected = menu.addAction(tr("却下"));
    rejected->setCheckable(true);
    rejected->setChecked(selected.flag == mediapool::AssetFlag::Rejected);
    QMenu *stars = menu.addMenu(tr("星"));
    QActionGroup starGroup(stars);
    for (int n = 0; n <= 5; ++n) {
        QAction *action = stars->addAction(n == 0 ? tr("なし") : tr("★%1").arg(n));
        action->setCheckable(true);
        action->setChecked(selected.stars == n);
        action->setData(n);
        starGroup.addAction(action);
    }
    QAction *reveal = menu.addAction(tr("エクスプローラーで表示"));
    reveal->setEnabled(!selected.filePath.isEmpty());
    menu.addSeparator();
    QAction *remove = menu.addAction(tr("削除"));
    QAction *chosen = menu.exec(m_assetList->viewport()->mapToGlobal(pos));
    if (!chosen || !m_pool)
        return;
    bool changed = false;
    if (chosen == rename) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("名前を変更…"), tr("素材名:"),
                                                   QLineEdit::Normal, selected.displayName, &ok).trimmed();
        if (m_pool && ok && !name.isEmpty() && name != selected.displayName)
            changed = m_pool->renameAsset(id, name);
    } else if (chosen == favorite || chosen == rejected) {
        const auto flag = !chosen->isChecked() ? mediapool::AssetFlag::None
            : chosen == favorite ? mediapool::AssetFlag::Favorite : mediapool::AssetFlag::Rejected;
        changed = m_pool->setAssetFlag(id, flag);
    } else if (starGroup.actions().contains(chosen)) {
        const int n = chosen->data().toInt();
        if (n != selected.stars)
            changed = m_pool->setAssetStars(id, n);
    } else if (chosen == reveal) {
#ifdef Q_OS_WIN
        QProcess::startDetached(QStringLiteral("explorer.exe"),
            {QStringLiteral("/select,"), QDir::toNativeSeparators(selected.filePath)});
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(selected.filePath).absolutePath()));
#endif
    } else if (chosen == remove) {
        changed = m_pool->removeAsset(id);
    }
    if (changed) {
        emit poolChanged();
        showAssetsForCurrentBin();
    }
}

void MediaPoolDock::showBinContextMenu(const QPoint &pos)
{
    if (!m_pool)
        return;
    QTreeWidgetItem *item = m_binTree->itemAt(pos);
    if (!item || !item->parent())
        return;
    m_binTree->setCurrentItem(item);
    const QString id = item->data(0, kBinIdRole).toString();
    const QString oldName = item->text(0);
    QMenu menu(this);
    QAction *rename = menu.addAction(tr("名前を変更…"));
    QAction *remove = menu.addAction(tr("削除"));
    QAction *chosen = menu.exec(m_binTree->viewport()->mapToGlobal(pos));
    if (!chosen || !m_pool)
        return;
    bool changed = false;
    if (chosen == rename) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("名前を変更…"), tr("ビン名:"),
                                                   QLineEdit::Normal, oldName, &ok).trimmed();
        if (m_pool && ok && !name.isEmpty() && name != oldName)
            changed = m_pool->renameBin(id, name);
    } else if (chosen == remove) {
        changed = m_pool->removeBin(id);
    }
    if (changed) {
        emit poolChanged();
        refresh();
    }
}

void MediaPoolDock::setThumbnailsEnabled(bool enabled)
{
    QSettings().setValue(QStringLiteral("mediaPool/showThumbnails"), enabled);
    if (m_thumbnailsEnabled == enabled) return;
    m_thumbnailsEnabled = enabled;
    m_hoverItem = nullptr;
    m_thumbnailCache->clear();
    showAssetsForCurrentBin();
}

void MediaPoolDock::setThumbnail(QListWidgetItem *item, int index)
{
    if (!m_thumbnailsEnabled || !item) return;
    const auto frames = m_thumbnailCache->frames(item->data(kAssetPathRole).toString());
    if (!frames.isEmpty())
        item->setIcon(QIcon(QPixmap::fromImage(frames[qBound(0, index, int(frames.size()) - 1)])));
}

bool MediaPoolDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_assetList->viewport() && m_thumbnailsEnabled) {
        if (event->type() == QEvent::MouseMove) {
            const auto pos = static_cast<QMouseEvent *>(event)->position().toPoint();
            auto *item = m_assetList->itemAt(pos);
            if (m_hoverItem != item) {
                setThumbnail(m_hoverItem);
                m_hoverItem = item;
            }
            if (item) {
                const auto frames = m_thumbnailCache->frames(item->data(kAssetPathRole).toString());
                const QRect rect = m_assetList->visualItemRect(item);
                setThumbnail(item, ThumbnailCache::skimIndex(pos.x() - rect.x(), rect.width(), int(frames.size())));
            }
        } else if (event->type() == QEvent::Leave) {
            setThumbnail(m_hoverItem);
            m_hoverItem = nullptr;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}
