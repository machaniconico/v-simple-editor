#include "RecentFiles.h"
#include <QAction>
#include <QFileInfo>
#include <QSettings>

// ---------------------------------------------------------------------------
// RecentFilesManager
// ---------------------------------------------------------------------------

RecentFilesManager::RecentFilesManager(QObject *parent)
    : QObject(parent)
{
    load();
}

void RecentFilesManager::addFile(const QString &filePath)
{
    if (filePath.isEmpty())
        return;

    // Remove duplicates (case-sensitive path comparison)
    m_files.removeAll(filePath);

    // Prepend so the most recent is first
    m_files.prepend(filePath);

    // Cap to maxFiles
    while (m_files.size() > m_maxFiles)
        m_files.removeLast();

    save();
    emit recentFilesChanged();
}

void RecentFilesManager::removeFile(const QString &filePath)
{
    if (m_files.removeAll(filePath) > 0) {
        save();
        emit recentFilesChanged();
    }
}

void RecentFilesManager::clear()
{
    if (m_files.isEmpty())
        return;

    m_files.clear();
    save();
    emit recentFilesChanged();
}

QStringList RecentFilesManager::recentFiles() const
{
    return m_files;
}

void RecentFilesManager::setMaxFiles(int maxFiles)
{
    if (maxFiles < 1)
        maxFiles = 1;

    m_maxFiles = maxFiles;

    // Trim if necessary
    bool trimmed = false;
    while (m_files.size() > m_maxFiles) {
        m_files.removeLast();
        trimmed = true;
    }

    if (trimmed) {
        save();
        emit recentFilesChanged();
    }
}

int RecentFilesManager::maxFiles() const
{
    return m_maxFiles;
}

void RecentFilesManager::load()
{
    QSettings settings("VEditorSimple", "RecentFiles");
    m_files = settings.value("recentFiles").toStringList();

    // Enforce cap in case maxFiles was changed externally
    while (m_files.size() > m_maxFiles)
        m_files.removeLast();
}

void RecentFilesManager::save()
{
    QSettings settings("VEditorSimple", "RecentFiles");
    settings.setValue("recentFiles", m_files);
}

// ---------------------------------------------------------------------------
// RecentFilesMenu
// ---------------------------------------------------------------------------

RecentFilesMenu::RecentFilesMenu(RecentFilesManager *manager, QWidget *parent)
    : QMenu(parent)
    , m_manager(manager)
{
    setTitle(tr("Recent Files"));

    connect(m_manager, &RecentFilesManager::recentFilesChanged,
            this, &RecentFilesMenu::rebuild);

    rebuild();
}

void RecentFilesMenu::rebuild()
{
    clear();

    const QStringList files = m_manager->recentFiles();

    if (files.isEmpty()) {
        QAction *placeholder = addAction(tr("(No recent files)"));
        placeholder->setEnabled(false);
        return;
    }

    int index = 1;
    for (const QString &filePath : files) {
        QFileInfo info(filePath);
        const bool exists = info.exists();

        // Label: "1. filename.ext"
        QString label = QString("%1. %2").arg(index).arg(info.fileName());
        QAction *action = addAction(label);
        action->setData(filePath);
        action->setToolTip(filePath);

        if (!exists) {
            // Gray out missing files
            action->setEnabled(false);
            action->setText(label + tr(" (missing)"));

            // Sub-action to remove the missing entry
            QAction *removeAction = addAction(tr("  Remove \"%1\" from list").arg(info.fileName()));
            removeAction->setData(filePath);
            connect(removeAction, &QAction::triggered,
                    this, &RecentFilesMenu::onRemoveMissingTriggered);
        } else {
            connect(action, &QAction::triggered,
                    this, &RecentFilesMenu::onFileActionTriggered);
        }

        ++index;
    }

    addSeparator();

    QAction *clearAction = addAction(tr("Clear Recent Files"));
    connect(clearAction, &QAction::triggered,
            this, &RecentFilesMenu::onClearTriggered);
}

void RecentFilesMenu::onFileActionTriggered()
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (!action)
        return;

    emit fileSelected(action->data().toString());
}

void RecentFilesMenu::onRemoveMissingTriggered()
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (!action)
        return;

    m_manager->removeFile(action->data().toString());
    // rebuild() is triggered automatically via recentFilesChanged signal
}

void RecentFilesMenu::onClearTriggered()
{
    m_manager->clear();
    // rebuild() is triggered automatically via recentFilesChanged signal
}
