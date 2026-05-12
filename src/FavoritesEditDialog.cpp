#include "FavoritesEditDialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {
// Per-item role storing the stable favoritable-action id on leaf rows.
constexpr int kIdRole = Qt::UserRole + 1;
}

FavoritesEditDialog::FavoritesEditDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("お気に入りメニューを編集"));
    resize(480, 560);

    auto *outer = new QVBoxLayout(this);

    auto *intro = new QLabel(
        QStringLiteral("チェックを付けた機能が「お気に入り」メニューに表示されます。"),
        this);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(1);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    if (m_tree->header())
        m_tree->header()->setStretchLastSection(true);
    outer->addWidget(m_tree, /*stretch=*/1);

    auto *bulkRow = new QHBoxLayout;
    auto *checkAllBtn = new QPushButton(QStringLiteral("すべて選択"), this);
    auto *uncheckAllBtn = new QPushButton(QStringLiteral("すべて解除"), this);
    bulkRow->addWidget(checkAllBtn);
    bulkRow->addWidget(uncheckAllBtn);
    bulkRow->addStretch(1);
    outer->addLayout(bulkRow);
    connect(checkAllBtn, &QPushButton::clicked, this, &FavoritesEditDialog::checkAll);
    connect(uncheckAllBtn, &QPushButton::clicked, this, &FavoritesEditDialog::uncheckAll);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, this);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void FavoritesEditDialog::setAvailableActions(
    const QVector<QPair<QString, QString>> &idLabelPairs,
    const QHash<QString, QString> &idToMenuPath)
{
    m_available = idLabelPairs;
    m_idToMenuPath = idToMenuPath;
    rebuildTree();
}

void FavoritesEditDialog::setSelectedIds(const QStringList &ids)
{
    m_selected = ids;
    rebuildTree();
}

void FavoritesEditDialog::rebuildTree()
{
    if (!m_tree)
        return;

    m_tree->clear();

    const QSet<QString> selectedSet(m_selected.cbegin(), m_selected.cend());

    // One parent node per menuPath. Groups are appended to the tree the first
    // time their menuPath is seen, so the tree's top-level order follows the
    // natural menu-bar order of m_available.
    QHash<QString, QTreeWidgetItem *> groupItems;

    for (const auto &pair : m_available) {
        const QString &id = pair.first;
        const QString &label = pair.second;
        const QString menuPath = m_idToMenuPath.value(id, QStringLiteral("その他"));

        QTreeWidgetItem *group = groupItems.value(menuPath, nullptr);
        if (!group) {
            group = new QTreeWidgetItem(m_tree);
            group->setText(0, menuPath);
            group->setFlags(Qt::ItemIsEnabled);
            QFont f = group->font(0);
            f.setBold(true);
            group->setFont(0, f);
            group->setExpanded(true);
            groupItems.insert(menuPath, group);
        }

        auto *leaf = new QTreeWidgetItem(group);
        leaf->setText(0, label.isEmpty() ? id : label);
        leaf->setData(0, kIdRole, id);
        leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        leaf->setCheckState(0, selectedSet.contains(id) ? Qt::Checked : Qt::Unchecked);
    }

    m_tree->expandAll();
}

QStringList FavoritesEditDialog::selectedIds() const
{
    QStringList result;
    if (!m_tree)
        return result;

    // Walk in (id, label) order so the result preserves the natural menu order
    // rather than the tree's grouped order.
    QSet<QString> checked;
    const int topCount = m_tree->topLevelItemCount();
    for (int i = 0; i < topCount; ++i) {
        QTreeWidgetItem *group = m_tree->topLevelItem(i);
        if (!group)
            continue;
        const int childCount = group->childCount();
        for (int j = 0; j < childCount; ++j) {
            QTreeWidgetItem *leaf = group->child(j);
            if (!leaf)
                continue;
            if (leaf->checkState(0) == Qt::Checked) {
                const QString id = leaf->data(0, kIdRole).toString();
                if (!id.isEmpty())
                    checked.insert(id);
            }
        }
    }

    for (const auto &pair : m_available) {
        if (checked.contains(pair.first))
            result.append(pair.first);
    }
    return result;
}

void FavoritesEditDialog::checkAll()
{
    if (!m_tree)
        return;
    const int topCount = m_tree->topLevelItemCount();
    for (int i = 0; i < topCount; ++i) {
        QTreeWidgetItem *group = m_tree->topLevelItem(i);
        if (!group)
            continue;
        const int childCount = group->childCount();
        for (int j = 0; j < childCount; ++j) {
            QTreeWidgetItem *leaf = group->child(j);
            if (leaf)
                leaf->setCheckState(0, Qt::Checked);
        }
    }
}

void FavoritesEditDialog::uncheckAll()
{
    if (!m_tree)
        return;
    const int topCount = m_tree->topLevelItemCount();
    for (int i = 0; i < topCount; ++i) {
        QTreeWidgetItem *group = m_tree->topLevelItem(i);
        if (!group)
            continue;
        const int childCount = group->childCount();
        for (int j = 0; j < childCount; ++j) {
            QTreeWidgetItem *leaf = group->child(j);
            if (leaf)
                leaf->setCheckState(0, Qt::Unchecked);
        }
    }
}
