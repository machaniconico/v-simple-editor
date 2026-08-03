#include "EffectLibraryPanel.h"

#include "LayerCompositor.h"
#include "VfxFootageLibrary.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QDir>
#include <QUrl>
#include <QDrag>
#include <QMimeData>

namespace {

constexpr int kEntryIdRole = Qt::UserRole + 1;

QPixmap loadingPixmap()
{
    QPixmap pixmap(144, 81);
    pixmap.fill(QColor(24, 29, 38));
    QPainter painter(&pixmap);
    painter.setPen(QColor(96, 106, 122));
    painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    painter.drawLine(0, pixmap.height() - 1, pixmap.width() - 1, 0);
    painter.end();
    return pixmap;
}

class EffectGridWidget final : public QListWidget
{
public:
    explicit EffectGridWidget(QWidget *parent = nullptr)
        : QListWidget(parent)
    {
    }

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        QListWidgetItem *item = currentItem();
        if (!item)
            return;
        const QString id = item->data(kEntryIdRole).toString();
        if (id.isEmpty())
            return;

        auto *mime = new QMimeData();
        mime->setData("application/x-vse-effect-library", id.toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(item->icon().pixmap(iconSize()));
        drag->exec(supportedActions & Qt::CopyAction
                       ? Qt::CopyAction
                       : Qt::IgnoreAction);
    }
};

QString categoryLabel(const QString &category)
{
    return category.isEmpty() ? QStringLiteral("すべて") : category;
}

} // namespace

EffectLibraryPanel::EffectLibraryPanel(QWidget *parent)
    : QDockWidget(QStringLiteral("エフェクトライブラリ"), parent)
{
    setObjectName(QStringLiteral("EffectLibraryDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto *root = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    auto *searchRow = new QHBoxLayout();
    auto *searchLabel = new QLabel(QStringLiteral("検索"), root);
    m_search = new QLineEdit(root);
    m_search->setObjectName(QStringLiteral("EffectLibrarySearch"));
    m_search->setPlaceholderText(QStringLiteral("名前またはタグ"));
    m_search->setClearButtonEnabled(true);
    searchLabel->setBuddy(m_search);
    searchRow->addWidget(searchLabel);
    searchRow->addWidget(m_search, 1);
    rootLayout->addLayout(searchRow);

    auto *filterRow = new QHBoxLayout();
    auto *categoryLabelWidget = new QLabel(QStringLiteral("カテゴリ"), root);
    m_category = new QComboBox(root);
    m_category->setObjectName(QStringLiteral("EffectLibraryCategory"));
    categoryLabelWidget->setBuddy(m_category);
    filterRow->addWidget(categoryLabelWidget);
    filterRow->addWidget(m_category, 1);
    m_favoritesOnly = new QCheckBox(QStringLiteral("★ お気に入りのみ"), root);
    m_favoritesOnly->setObjectName(QStringLiteral("EffectLibraryFavoritesOnly"));
    filterRow->addWidget(m_favoritesOnly);
    rootLayout->addLayout(filterRow);

    auto *actionRow = new QHBoxLayout();
    m_preview = new QCheckBox(QStringLiteral("プレビュー"), root);
    m_preview->setObjectName(QStringLiteral("EffectLibraryPreview"));
    m_preview->setToolTip(QStringLiteral("選択中のエフェクトを確定せず表示します"));
    actionRow->addWidget(m_preview);
    m_filterCount = new QLabel(root);
    m_filterCount->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    actionRow->addWidget(m_filterCount, 1);
    auto *savePreset = new QPushButton(QStringLiteral("選択スタックをプリセット保存"), root);
    savePreset->setObjectName(QStringLiteral("EffectLibrarySavePreset"));
    actionRow->addWidget(savePreset);
    rootLayout->addLayout(actionRow);

    m_footageEmptyState = new QWidget(root);
    auto *footageEmptyLayout = new QHBoxLayout(m_footageEmptyState);
    footageEmptyLayout->setContentsMargins(8, 6, 8, 6);
    auto *footageEmptyLabel = new QLabel(
        QStringLiteral("素材フォルダに動画を入れると、ここに VFX 素材として並びます"),
        m_footageEmptyState);
    footageEmptyLabel->setWordWrap(true);
    footageEmptyLayout->addWidget(footageEmptyLabel, 1);
    m_openFootageFolder = new QPushButton(QStringLiteral("素材フォルダを開く"),
                                          m_footageEmptyState);
    m_openFootageFolder->setObjectName(QStringLiteral("OpenVfxFootageFolder"));
    m_openFootageFolder->setAccessibleName(QStringLiteral("VFX素材フォルダを開く"));
    footageEmptyLayout->addWidget(m_openFootageFolder);
    rootLayout->addWidget(m_footageEmptyState);

    auto *splitterLayout = new QVBoxLayout();
    splitterLayout->setContentsMargins(0, 0, 0, 0);
    splitterLayout->setSpacing(6);

    m_grid = new EffectGridWidget(root);
    m_grid->setObjectName(QStringLiteral("EffectLibraryGrid"));
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setIconSize(QSize(144, 81));
    m_grid->setGridSize(QSize(158, 112));
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setDragEnabled(true);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setWordWrap(true);
    m_grid->setUniformItemSizes(true);
    splitterLayout->addWidget(m_grid, 1);

    m_inspectorScroll = new QScrollArea(root);
    m_inspectorScroll->setObjectName(QStringLiteral("EffectLibraryInspector"));
    m_inspectorScroll->setWidgetResizable(true);
    m_inspectorScroll->setFrameShape(QFrame::StyledPanel);
    m_inspectorWidget = new QWidget();
    m_inspectorLayout = new QVBoxLayout(m_inspectorWidget);
    m_inspectorLayout->setContentsMargins(8, 8, 8, 8);
    m_inspectorLayout->setSpacing(6);
    m_inspectorScroll->setWidget(m_inspectorWidget);
    splitterLayout->addWidget(m_inspectorScroll, 0);
    rootLayout->addLayout(splitterLayout, 1);
    setWidget(root);

    connect(m_search, &QLineEdit::textChanged, this,
            &EffectLibraryPanel::refreshItems);
    connect(m_category, &QComboBox::currentTextChanged, this,
            &EffectLibraryPanel::refreshItems);
    connect(m_favoritesOnly, &QCheckBox::toggled, this,
            &EffectLibraryPanel::refreshItems);
    connect(m_preview, &QCheckBox::toggled, this,
            &EffectLibraryPanel::previewToggled);
    connect(savePreset, &QPushButton::clicked, this,
            &EffectLibraryPanel::savePresetRequested);
    connect(m_openFootageFolder, &QPushButton::clicked, this, [this]() {
        const QString directory = vfxfootage::VfxFootageLibrary::defaultDirectory();
        QDir().mkpath(directory);
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    connect(m_grid, &QListWidget::itemSelectionChanged, this,
            &EffectLibraryPanel::selectionChanged);
    connect(m_grid, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
        if (item) {
            const QString id = item->data(kEntryIdRole).toString();
            if (!id.isEmpty())
                emit applyRequested(id);
        }
    });
    connect(m_grid, &QWidget::customContextMenuRequested, this,
            &EffectLibraryPanel::showContextMenu);
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &EffectLibraryPanel::scheduleVisibleThumbnails);
    connect(m_grid->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            &EffectLibraryPanel::scheduleVisibleThumbnails);

    refreshItems();
}

bool EffectLibraryPanel::previewEnabled() const
{
    return m_preview && m_preview->isChecked();
}

void EffectLibraryPanel::setThumbnailSource(const QImage &source)
{
    const qint64 sourceKey = source.isNull() ? 0 : source.cacheKey();
    if (sourceKey != m_thumbnailSourceKey) {
        m_thumbnailCache.clear();
        m_thumbnailSourceKey = sourceKey;
    }
    m_thumbnailSource = source;
    scheduleVisibleThumbnails();
}

void EffectLibraryPanel::refreshCatalog()
{
    m_model.registerAll();
    refreshItems();
}

void EffectLibraryPanel::updateFilterCount()
{
    if (!m_filterCount)
        return;
    int count = 0;
    for (int i = 0; i < m_grid->count(); ++i) {
        if (!m_grid->item(i)->data(kEntryIdRole).toString().isEmpty())
            ++count;
    }
    m_filterCount->setText(QStringLiteral("%1 件").arg(count));
}

void EffectLibraryPanel::refreshItems()
{
    const QString oldSelection = m_selectedId;
    QVector<efxlib::LibraryEntry> filtered = m_model.search(
        m_search ? m_search->text() : QString());
    const bool onlyFavorites = m_favoritesOnly && m_favoritesOnly->isChecked();
    if (m_footageEmptyState)
        m_footageEmptyState->setVisible(!m_model.hasFootageEntries());

    m_category->blockSignals(true);
    const QString categoryBefore = m_category->currentText();
    m_category->clear();
    m_category->addItem(QStringLiteral("すべて"));
    for (const QString &category : m_model.categories())
        m_category->addItem(categoryLabel(category));
    int categoryIndex = m_category->findText(categoryBefore);
    if (categoryIndex < 0)
        categoryIndex = 0;
    m_category->setCurrentIndex(categoryIndex);
    m_category->blockSignals(false);

    const QString effectiveCategory = m_category->currentText();
    QVector<efxlib::LibraryEntry> visible;
    for (const auto &entry : filtered) {
        if (onlyFavorites && !entry.favorite)
            continue;
        if (effectiveCategory != QStringLiteral("すべて")
            && entry.category != effectiveCategory) {
            continue;
        }
        visible.append(entry);
    }

    QSignalBlocker blocker(m_grid);
    m_grid->clear();
    const QPixmap loadingIcon = loadingPixmap();
    for (const auto &entry : visible) {
        auto *item = new QListWidgetItem(QIcon(loadingIcon), entry.displayName, m_grid);
        item->setData(kEntryIdRole, entry.id);
        item->setToolTip(QStringLiteral("%1\n%2")
                             .arg(entry.category, entry.tags.join(QStringLiteral(", "))));
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(158, 112));
        if (entry.favorite) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
    }
    if (visible.isEmpty()) {
        auto *empty = new QListWidgetItem(QStringLiteral("該当するエフェクトはありません"), m_grid);
        empty->setFlags(empty->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
        empty->setSizeHint(QSize(260, 48));
    }

    int selectedIndex = -1;
    for (int i = 0; i < m_grid->count(); ++i) {
        if (m_grid->item(i)->data(kEntryIdRole).toString() == oldSelection) {
            selectedIndex = i;
            break;
        }
    }
    if (selectedIndex >= 0)
        m_grid->setCurrentRow(selectedIndex);
    else
        m_selectedId.clear();

    updateFilterCount();
    buildInspector();
    scheduleVisibleThumbnails();
    if (m_selectedId.isEmpty())
        emit previewRequested(QString(), false);
}

void EffectLibraryPanel::scheduleVisibleThumbnails()
{
    QMetaObject::invokeMethod(this, &EffectLibraryPanel::refreshVisibleThumbnails,
                              Qt::QueuedConnection);
}

void EffectLibraryPanel::refreshVisibleThumbnails()
{
    if (!m_grid)
        return;
    const QRect viewportRect = m_grid->viewport()->rect();
    const QPixmap fallback = loadingPixmap();
    for (int i = 0; i < m_grid->count(); ++i) {
        QListWidgetItem *item = m_grid->item(i);
        const QString id = item->data(kEntryIdRole).toString();
        if (id.isEmpty() || !m_grid->visualItemRect(item).intersects(viewportRect))
            continue;
        QPixmap thumbnail = m_thumbnailCache.value(id);
        if (thumbnail.isNull()) {
            const QImage image = m_model.thumbnail(
                id, m_thumbnailSource, QSize(144, 81));
            if (!image.isNull()) {
                thumbnail = QPixmap::fromImage(image);
                m_thumbnailCache.insert(id, thumbnail);
            }
        }
        item->setIcon(thumbnail.isNull() ? QIcon(fallback) : QIcon(thumbnail));
    }
}

void EffectLibraryPanel::selectionChanged()
{
    QListWidgetItem *item = m_grid ? m_grid->currentItem() : nullptr;
    m_selectedId = item ? item->data(kEntryIdRole).toString() : QString();
    buildInspector();
    emit previewRequested(m_selectedId, previewEnabled() && !m_selectedId.isEmpty());
}

void EffectLibraryPanel::previewToggled(bool enabled)
{
    emit previewRequested(m_selectedId, enabled && !m_selectedId.isEmpty());
}

void EffectLibraryPanel::clearInspector()
{
    if (!m_inspectorLayout)
        return;
    while (QLayoutItem *item = m_inspectorLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

void EffectLibraryPanel::buildInspector()
{
    clearInspector();
    if (m_selectedId.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("エフェクトを選択するとパラメータを表示します"),
                                 m_inspectorWidget);
        empty->setWordWrap(true);
        m_inspectorLayout->addWidget(empty);
        m_inspectorLayout->addStretch();
        return;
    }

    efxlib::LibraryEntry entry;
    if (!m_model.entryById(m_selectedId, &entry))
        return;

    auto *title = new QLabel(entry.displayName, m_inspectorWidget);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    m_inspectorLayout->addWidget(title);

    auto *meta = new QLabel(QStringLiteral("%1  |  %2")
                                .arg(entry.category, entry.tags.join(QStringLiteral(", "))),
                            m_inspectorWidget);
    meta->setWordWrap(true);
    m_inspectorLayout->addWidget(meta);

    auto *favorite = new QCheckBox(QStringLiteral("お気に入り"), m_inspectorWidget);
    favorite->setChecked(entry.favorite);
    connect(favorite, &QCheckBox::toggled, this, [this](bool checked) {
        m_model.setFavorite(m_selectedId, checked);
        refreshItems();
    });
    m_inspectorLayout->addWidget(favorite);

    // VFX generators are created as RGBA generator clips through their own
    // dialog. They do not map to ClipInfo::effects, so do not expose the
    // VideoEffect keyframe affordance for them.
    const bool supportsClipEffectKeyframes =
        entry.kind != efxlib::SourceKind::VfxGenerator
        && entry.kind != efxlib::SourceKind::Footage;

    const auto specs = m_model.parameters(m_selectedId);
    if (specs.isEmpty()) {
        auto *info = new QLabel(QStringLiteral("このエントリは既存 API の既定値でプレビューします"),
                                m_inspectorWidget);
        info->setWordWrap(true);
        m_inspectorLayout->addWidget(info);
    }

    for (const efxlib::ParameterSpec &spec : specs) {
        auto *row = new QWidget(m_inspectorWidget);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel(spec.displayName, row);
        label->setMinimumWidth(90);
        layout->addWidget(label);
        if (spec.color) {
            auto *colorButton = new QPushButton(row);
            colorButton->setText(QStringLiteral("色を選択"));
            colorButton->setAccessibleName(spec.displayName);
            colorButton->setProperty("color", spec.defaultColor);
            connect(colorButton, &QPushButton::clicked, this,
                    [this, colorButton, spec]() {
                const QColor current = colorButton->property("color").value<QColor>();
                const QColor chosen = QColorDialog::getColor(current, this,
                                                              spec.displayName);
                if (!chosen.isValid())
                    return;
                colorButton->setProperty("color", chosen);
                colorButton->setStyleSheet(QStringLiteral("background:%1").arg(chosen.name()));
                m_model.setParameterOverride(m_selectedId, spec.name, chosen);
                if (previewEnabled())
                    emit previewRequested(m_selectedId, true);
            });
            colorButton->setStyleSheet(QStringLiteral("background:%1")
                                           .arg(spec.defaultColor.isValid()
                                                    ? spec.defaultColor.name()
                                                    : QStringLiteral("#444")));
            layout->addWidget(colorButton, 1);
        } else {
            auto *spin = new QDoubleSpinBox(row);
            spin->setRange(spec.minValue, spec.maxValue);
            spin->setDecimals(spec.integer ? 0 : 3);
            spin->setSingleStep(spec.integer ? 1.0 : 0.01);
            const QVariant overrideValue = m_model.parameterOverride(
                m_selectedId, spec.name);
            spin->setValue(overrideValue.isValid()
                               ? overrideValue.toDouble() : spec.defaultValue);
            spin->setKeyboardTracking(true);
            connect(spin, &QDoubleSpinBox::valueChanged, this,
                    [this, spec](double value) {
                m_model.setParameterOverride(m_selectedId, spec.name, value);
                if (previewEnabled())
                    emit previewRequested(m_selectedId, true);
            });
            layout->addWidget(spin, 1);
        }

        if (!spec.color && supportsClipEffectKeyframes) {
            auto *keyframe = new QPushButton(QStringLiteral("◇"), row);
            keyframe->setToolTip(QStringLiteral("現在位置にキーフレームを追加"));
            keyframe->setAccessibleName(QStringLiteral("%1 のキーフレーム").arg(spec.displayName));
            keyframe->setFixedWidth(30);
            connect(keyframe, &QPushButton::clicked, this, [this, spec]() {
                emit keyframeRequested(m_selectedId, spec.name);
            });
            layout->addWidget(keyframe);
        }
        m_inspectorLayout->addWidget(row);
    }

    if (entry.kind == efxlib::SourceKind::Footage) {
        auto *row = new QWidget(m_inspectorWidget);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel(QStringLiteral("合成モード"), row);
        label->setMinimumWidth(90);
        layout->addWidget(label);
        auto *blendMode = new QComboBox(row);
        blendMode->setAccessibleName(QStringLiteral("VFX素材の合成モード"));
        for (int i = 0; i <= static_cast<int>(BlendMode::Lighten); ++i) {
            const BlendMode mode = static_cast<BlendMode>(i);
            blendMode->addItem(CompositeLayer::blendModeName(mode),
                               static_cast<int>(mode));
        }
        const QString selectedMode = m_model.parameterOverride(
            m_selectedId, QStringLiteral("blendMode")).toString();
        const QString effectiveMode = selectedMode.isEmpty()
            ? QStringLiteral("Screen") : selectedMode;
        const int modeIndex = blendMode->findText(effectiveMode);
        blendMode->setCurrentIndex(modeIndex >= 0 ? modeIndex :
                                   blendMode->findText(QStringLiteral("Screen")));
        connect(blendMode, &QComboBox::currentTextChanged, this,
                [this](const QString &mode) {
            m_model.setParameterOverride(m_selectedId,
                                          QStringLiteral("blendMode"), mode);
            if (previewEnabled())
                emit previewRequested(m_selectedId, true);
        });
        layout->addWidget(blendMode, 1);
        m_inspectorLayout->addWidget(row);

        auto *hint = new QLabel(
            QStringLiteral("ダブルクリックまたはドラッグでプレイヘッドへ配置"),
            m_inspectorWidget);
        hint->setWordWrap(true);
        m_inspectorLayout->addWidget(hint);
    }
    m_inspectorLayout->addStretch();
}

void EffectLibraryPanel::showContextMenu(const QPoint &position)
{
    QListWidgetItem *item = m_grid ? m_grid->itemAt(position) : nullptr;
    if (!item)
        return;
    const QString id = item->data(kEntryIdRole).toString();
    efxlib::LibraryEntry entry;
    if (id.isEmpty() || !m_model.entryById(id, &entry) || !entry.isUserPreset)
        return;

    QMenu menu(this);
    QAction *rename = menu.addAction(QStringLiteral("名前を変更"));
    QAction *remove = menu.addAction(QStringLiteral("削除"));
    QAction *chosen = menu.exec(m_grid->viewport()->mapToGlobal(position));
    if (chosen == rename)
        emit renamePresetRequested(id);
    else if (chosen == remove)
        emit deletePresetRequested(id);
}
