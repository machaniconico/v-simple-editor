#include "MediaPoolDock.h"
#include "ThumbnailCache.h"
#include <QSettings>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMouseEvent>
#include <QPixmap>
#include <QIcon>

#include <QAbstractItemView>
#include <QItemSelectionModel>
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
#include <QLabel>
#include <QMessageBox>
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
constexpr int kAssetDurationRole = Qt::UserRole + 2;
constexpr int kAssetTypeRole = Qt::UserRole + 3;
// QTreeWidgetItem に bin id を持たせるための role。
constexpr int kBinIdRole     = Qt::UserRole;
constexpr int kBinKindRole = Qt::UserRole + 1;
enum BinKind { Folder = 0, Keyword, Smart, Section };
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
    m_binTree->setObjectName(QStringLiteral("MediaPoolBinTree"));
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
    const auto *current = m_binTree->currentItem();
    const QString selectedBin = current ? current->data(0, kBinIdRole).toString() : QString();
    const int selectedKind = current ? current->data(0, kBinKindRole).toInt() : Folder;
    m_rebuilding = true;
    m_binTree->clear();

    // ルート項目「すべてのメディア」(binId = "")。
    QTreeWidgetItem *rootItem =
        new QTreeWidgetItem(m_binTree, QStringList(tr(kRootName)));
    rootItem->setData(0, kBinIdRole, QString());

    if (m_pool) {
        addBinItems(rootItem, QString());
        QStringList keywords;
        for (const auto &asset : m_pool->assets())
            keywords.append(asset.tags);
        keywords.removeAll(QString());
        keywords.removeDuplicates();
        keywords.sort();
        const auto addSection = [this](const QString &name) {
            auto *section = new QTreeWidgetItem(m_binTree, {name});
            section->setData(0, kBinKindRole, Section);
            section->setFlags(section->flags() & ~Qt::ItemIsSelectable);
            section->setExpanded(true);
            return section;
        };
        if (!keywords.isEmpty()) {
            auto *section = addSection(tr("キーワード"));
            for (const auto &tag : keywords) {
                auto *item = new QTreeWidgetItem(section, {tag});
                item->setData(0, kBinKindRole, Keyword);
                item->setData(0, kBinIdRole, tag);
            }
        }
        if (!m_pool->smartBins().isEmpty()) {
            auto *section = addSection(tr("スマートビン"));
            for (const auto &bin : m_pool->smartBins()) {
                auto *item = new QTreeWidgetItem(section, {bin.name});
                item->setData(0, kBinKindRole, Smart);
                item->setData(0, kBinIdRole, bin.id);
            }
        }
    }

    rootItem->setExpanded(true);
    m_binTree->setCurrentItem(rootItem);
    for (QTreeWidgetItemIterator it(m_binTree); *it; ++it) {
        if ((*it)->data(0, kBinIdRole).toString() == selectedBin
            && (*it)->data(0, kBinKindRole).toInt() == selectedKind) {
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
    const auto *current = m_binTree->currentItem();
    const int kind = current ? current->data(0, kBinKindRole).toInt() : Folder;
    const QString value = current ? current->data(0, kBinIdRole).toString() : QString();
    QSet<int> smartIds;
    if (kind == Smart) {
        for (const auto &asset : m_pool->evaluateSmartBin(value))
            smartIds.insert(asset.id);
    }
    for (const auto &asset : filtered) {
        if ((kind == Smart && smartIds.contains(asset.id))
            || (kind == Keyword && asset.tags.contains(value))
            || (kind == Folder && (binId.isEmpty() || asset.binId == binId)))
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
        item->setData(kAssetDurationRole, asset.durationMs);
        item->setData(kAssetTypeRole, static_cast<int>(asset.type));
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
    if (!cur || cur->data(0, kBinKindRole).toInt() != Folder) {
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
    m_assetList->setCurrentItem(item, item->isSelected()
        ? QItemSelectionModel::NoUpdate : QItemSelectionModel::ClearAndSelect);
    const int id = item->data(kAssetIdRole).toInt();
    const auto *asset = m_pool->getAsset(id);
    if (!asset)
        return;
    // メニューのイベントループ中にモデルや一覧が更新されても参照を保持しない。
    const auto selected = *asset;
    QMenu menu(this);
    // MainWindow owns the command; no additional dock signals are needed.
    for (QAction *action : actions()) {
        if (action->objectName() == QStringLiteral("sequenceFromMedia")) {
            action->setData(QStringList{selected.filePath, selected.displayName});
            menu.addAction(action);
            menu.addSeparator();
            break;
        }
    }
    QAction *rename = menu.addAction(tr("名前を変更…"));
    QAction *keywords = menu.addAction(tr("キーワードを編集…"));
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
    if (chosen == keywords) {
        editSelectedKeywords();
        return;
    } else if (chosen == rename) {
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
        refresh();
    }
}

void MediaPoolDock::showBinContextMenu(const QPoint &pos)
{
    if (!m_pool)
        return;
    QTreeWidgetItem *item = m_binTree->itemAt(pos);
    if (item)
        m_binTree->setCurrentItem(item);
    const int kind = item ? item->data(0, kBinKindRole).toInt() : Section;
    const QString id = item ? item->data(0, kBinIdRole).toString() : QString();
    const QString oldName = item ? item->text(0) : QString();
    QMenu menu(this);
    QAction *createSmart = menu.addAction(tr("スマートビンを作成…"));
    QAction *reduceProject = menu.addAction(tr("未使用素材を削除…"));
    reduceProject->setEnabled(bool(m_usedPathsProvider));
    QAction *editSmart = kind == Smart ? menu.addAction(tr("スマートビンを編集…")) : nullptr;
    QAction *rename = kind == Folder && !id.isEmpty() ? menu.addAction(tr("名前を変更…")) : nullptr;
    QAction *remove = (kind == Folder || kind == Smart) && !id.isEmpty()
        ? menu.addAction(tr("削除")) : nullptr;
    QAction *chosen = menu.exec(m_binTree->viewport()->mapToGlobal(pos));
    if (!chosen || !m_pool)
        return;
    if (chosen == reduceProject) {
        onRemoveUnused();
        return;
    }
    if (chosen == createSmart || chosen == editSmart) {
        editSmartBin(chosen == createSmart ? QString() : id);
        return;
    }
    bool changed = false;
    if (chosen == rename) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("名前を変更…"), tr("ビン名:"),
                                                   QLineEdit::Normal, oldName, &ok).trimmed();
        if (m_pool && ok && !name.isEmpty() && name != oldName)
            changed = m_pool->renameBin(id, name);
    } else if (chosen == remove) {
        changed = kind == Smart ? m_pool->removeSmartBin(id) : m_pool->removeBin(id);
    }
    if (changed) {
        emit poolChanged();
        refresh();
    }
}

void MediaPoolDock::onRemoveUnused()
{
    if (!m_pool || !m_usedPathsProvider)
        return;
    auto *pool = m_pool;
    const auto unused = pool->filtered(QString(), mediapool::AssetFilterMode::Unused,
                                       m_usedPathsProvider());
    if (unused.isEmpty()) {
        QMessageBox::information(this, tr("未使用素材を削除"), tr("未使用素材はありません。"));
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("ReduceProjectConfirmation"));
    dialog.setWindowTitle(tr("未使用素材を削除"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *message = new QLabel(tr("未使用素材 %1 件をメディアプールから削除しますか？\n"
        "この操作は元に戻せません。ディスク上のファイルは削除されないので、必要になれば再度読み込めます")
        .arg(unused.size()), &dialog);
    message->setObjectName(QStringLiteral("ReduceProjectMessage"));
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("ReduceProjectList"));
    for (const auto &asset : unused)
        list->addItem(QFileInfo(asset.filePath).fileName() + QStringLiteral("\n") + asset.filePath);
    layout->addWidget(list);
    auto *buttons = new QDialogButtonBox(&dialog);
    buttons->setObjectName(QStringLiteral("ReduceProjectButtons"));
    auto *yes = buttons->addButton(tr("はい"), QDialogButtonBox::AcceptRole);
    auto *no = buttons->addButton(tr("いいえ"), QDialogButtonBox::RejectRole);
    yes->setAutoDefault(false);
    no->setDefault(true);
    no->setFocus();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(560, 400);
    if (dialog.exec() != QDialog::Accepted || m_pool != pool || !m_usedPathsProvider)
        return;

    // Recheck through the same provider after the modal event loop.
    const auto usedPaths = m_usedPathsProvider();
    bool changed = false;
    for (const auto &asset : unused) {
        const auto *current = pool->getAsset(asset.id);
        if (current && current->filePath == asset.filePath && !usedPaths.contains(current->filePath))
            changed = pool->removeAsset(asset.id) || changed;
    }
    if (changed) {
        emit poolChanged();
        refresh();
    }
}

void MediaPoolDock::editSelectedKeywords()
{
    if (!m_pool) return;
    auto *pool = m_pool;
    QVector<int> ids;
    QStringList common;
    for (const auto *item : m_assetList->selectedItems()) {
        const int id = item->data(kAssetIdRole).toInt();
        const auto *asset = pool->getAsset(id);
        if (!asset) continue;
        if (ids.isEmpty()) common = asset->tags;
        else {
            for (int i = int(common.size()) - 1; i >= 0; --i)
                if (!asset->tags.contains(common[i])) common.removeAt(i);
        }
        ids.append(id);
    }
    if (ids.isEmpty()) return;
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(this, tr("キーワードを編集…"),
        tr("共通キーワード（1行に1つ）:\n削除した行は全選択素材から削除し、追加した行は全選択素材に追加します。\n素材ごとの固有キーワードは保持します。"),
        common.join(QLatin1Char('\n')), &ok);
    if (!ok || m_pool != pool) return;
    QStringList tags;
    for (const auto &line : text.split(QLatin1Char('\n'))) {
        const auto tag = line.trimmed();
        if (!tag.isEmpty() && !tags.contains(tag)) tags.append(tag);
    }
    bool changed = false;
    for (int id : ids) {
        const auto *asset = pool->getAsset(id);
        if (!asset) continue;
        auto updated = asset->tags;
        for (const auto &tag : common)
            if (!tags.contains(tag)) updated.removeAll(tag);
        for (const auto &tag : tags)
            if (!updated.contains(tag)) updated.append(tag);
        if (updated != asset->tags)
            changed = pool->setAssetTags(id, updated) || changed;
    }
    if (changed) {
        emit poolChanged();
        refresh();
    }
}

void MediaPoolDock::editSmartBin(const QString &id)
{
    if (!m_pool) return;
    auto *pool = m_pool;
    mediapool::SmartBin bin;
    if (!id.isEmpty()) {
        bool found = false;
        for (const auto &existing : pool->smartBins()) {
            if (existing.id == id) { bin = existing; found = true; break; }
        }
        if (!found) return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(id.isEmpty() ? tr("スマートビンを作成…") : tr("スマートビンを編集…"));
    QFormLayout layout(&dialog);
    QLineEdit name(bin.name, &dialog);
    name.setObjectName(QStringLiteral("SmartBinName"));
    QLineEdit tag(bin.tagFilter, &dialog);
    tag.setObjectName(QStringLiteral("SmartBinTag"));
    QLineEdit query(bin.nameQuery, &dialog);
    query.setObjectName(QStringLiteral("SmartBinQuery"));
    QComboBox type(&dialog);
    type.setObjectName(QStringLiteral("SmartBinType"));
    type.addItem(tr("すべて"), static_cast<int>(mediapool::MediaType::Unknown));
    type.addItem(tr("映像"), static_cast<int>(mediapool::MediaType::Video));
    type.addItem(tr("音声"), static_cast<int>(mediapool::MediaType::Audio));
    type.addItem(tr("画像"), static_cast<int>(mediapool::MediaType::Image));
    type.setCurrentIndex(type.findData(static_cast<int>(bin.typeFilter)));
    layout.addRow(tr("ビン名:"), &name);
    layout.addRow(tr("キーワード（部分一致、空欄は条件なし）:"), &tag);
    layout.addRow(tr("素材名（部分一致）:"), &query);
    layout.addRow(tr("種類:"), &type);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons.button(QDialogButtonBox::Ok)->setText(tr("保存"));
    buttons.button(QDialogButtonBox::Cancel)->setText(tr("キャンセル"));
    buttons.button(QDialogButtonBox::Ok)->setEnabled(!name.text().trimmed().isEmpty());
    connect(&name, &QLineEdit::textChanged, &dialog, [&buttons](const QString &text) {
        buttons.button(QDialogButtonBox::Ok)->setEnabled(!text.trimmed().isEmpty());
    });
    layout.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || m_pool != pool || name.text().trimmed().isEmpty()) return;
    bin.name = name.text().trimmed();
    bin.tagFilter = tag.text().trimmed();
    bin.nameQuery = query.text().trimmed();
    bin.typeFilter = static_cast<mediapool::MediaType>(type.currentData().toInt());
    if (id.isEmpty()) pool->addSmartBin(bin);
    else if (!pool->updateSmartBin(id, bin)) return;
    emit poolChanged();
    refresh();
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
                const QString path = item->data(kAssetPathRole).toString();
                const auto frames = m_thumbnailCache->frames(path);
                if (frames.isEmpty()
                    && item->data(kAssetTypeRole).toInt() != static_cast<int>(mediapool::MediaType::Audio)) {
                    m_thumbnailCache->request(path, path,
                        item->data(kAssetDurationRole).toLongLong() / 1000.0);
                }
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
