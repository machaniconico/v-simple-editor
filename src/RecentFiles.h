#pragma once

#include <QObject>
#include <QMenu>
#include <QStringList>

class RecentFilesManager : public QObject
{
    Q_OBJECT

public:
    explicit RecentFilesManager(QObject *parent = nullptr);

    void addFile(const QString &filePath);
    void removeFile(const QString &filePath);
    void clear();

    QStringList recentFiles() const;

    void setMaxFiles(int maxFiles);
    int maxFiles() const;

signals:
    void recentFilesChanged();

private:
    void load();
    void save();

    QStringList m_files;
    int m_maxFiles = 10;
};


class RecentFilesMenu : public QMenu
{
    Q_OBJECT

public:
    explicit RecentFilesMenu(RecentFilesManager *manager, QWidget *parent = nullptr);

signals:
    void fileSelected(const QString &filePath);

private slots:
    void rebuild();
    void onFileActionTriggered();
    void onRemoveMissingTriggered();
    void onClearTriggered();

private:
    RecentFilesManager *m_manager;
};
