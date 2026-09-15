#pragma once

#include <QDialog>
#include <QHash>
#include <QStringList>

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class MediaRelinkDialog : public QDialog
{
public:
    explicit MediaRelinkDialog(const QStringList &missingPaths,
                               QWidget *parent = nullptr);

    QHash<QString, QString> mapping() const { return m_mapping; }

private:
    void searchFolder();
    void chooseIndividualFile();
    void setCandidate(QTreeWidgetItem *item, const QString &candidate);
    void updateApplyState();

    QStringList m_missingPaths;
    QHash<QString, QString> m_mapping;
    QTreeWidget *m_files = nullptr;
    QPushButton *m_searchFolderButton = nullptr;
    QPushButton *m_chooseFileButton = nullptr;
    QPushButton *m_applyButton = nullptr;
};
