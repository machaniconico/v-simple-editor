#pragma once
#include <QDialog>
#include <QString>
#include <QVector>
#include "PluginManifest.h"

class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTextEdit;

class PluginBrowserDialog : public QDialog {
    Q_OBJECT
public:
    explicit PluginBrowserDialog(const QString& pluginRootDir, QWidget* parent = nullptr);
    QString currentPluginManifestPath() const;

private slots:
    void onBrowseDir();
    void onRescan();
    void onTreeSelectionChanged();

private:
    void rescan();
    void populateTree();

    QLineEdit*   m_dirEdit    = nullptr;
    QPushButton* m_browseBtn  = nullptr;
    QPushButton* m_rescanBtn  = nullptr;
    QTreeWidget* m_tree       = nullptr;
    QTextEdit*   m_detailEdit = nullptr;
    QPushButton* m_runBtn     = nullptr;
    QPushButton* m_closeBtn   = nullptr;

    QVector<PluginInfo> m_plugins;
};
