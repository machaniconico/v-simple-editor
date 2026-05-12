#pragma once

#include <QDialog>
#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

class QTreeWidget;

// User-customizable "お気に入り" (favorites) editor.
//
// Presents every favoritable menu command, grouped by its parent menu
// (menuPath), each with a checkbox. The set of checked ids — in the original
// menu order — is what selectedIds() returns. MainWindow persists this list
// under QSettings("VSimpleEditor","Preferences")/favoriteActions and rebuilds
// the お気に入り menu from it.
//
// Decoupled from MainWindow's internal FavoritableAction struct on purpose:
// callers pass plain (id, label) pairs plus an id→menuPath map, so this dialog
// has no dependency on MainWindow internals.
class FavoritesEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FavoritesEditDialog(QWidget *parent = nullptr);

    // idLabelPairs: (stable id, display label) in the order they should appear
    //               (the natural menu order). idToMenuPath: stable id → parent
    //               menu title used for grouping. Call before setSelectedIds().
    void setAvailableActions(const QVector<QPair<QString, QString>> &idLabelPairs,
                             const QHash<QString, QString> &idToMenuPath);

    // ids currently registered as favorites (a subset of the available ids).
    void setSelectedIds(const QStringList &ids);

    // The checked ids, returned in the original menu order.
    QStringList selectedIds() const;

private slots:
    void checkAll();
    void uncheckAll();

private:
    void rebuildTree();

    QTreeWidget *m_tree = nullptr;
    QVector<QPair<QString, QString>> m_available; // (id, label) in menu order
    QHash<QString, QString> m_idToMenuPath;
    QStringList m_selected;
};
