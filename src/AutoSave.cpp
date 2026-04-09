#include "AutoSave.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QStandardPaths>
#include <algorithm>

AutoSave::AutoSave(QObject *parent)
    : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &AutoSave::performAutoSave);
}

AutoSave::~AutoSave()
{
    stop();
}

void AutoSave::start(const AutoSaveConfig &config)
{
    m_config = config;
    if (m_config.backupDir.isEmpty())
        m_config.backupDir = defaultBackupDir();

    QDir dir(m_config.backupDir);
    if (!dir.exists())
        dir.mkpath(".");

    // Remove clean shutdown marker (session is now active)
    QFile::remove(cleanShutdownMarkerPath());

    if (m_config.enabled) {
        m_timer.start(m_config.interval * 1000);
    }

    checkForRecovery();
}

void AutoSave::stop()
{
    m_timer.stop();
}

void AutoSave::setProjectData(std::function<QString()> dataCallback)
{
    m_dataCallback = std::move(dataCallback);
}

void AutoSave::performAutoSave()
{
    if (!m_dataCallback)
        return;

    QString json = m_dataCallback();
    if (json.isEmpty())
        return;

    QDir dir(m_config.backupDir);
    if (!dir.exists())
        dir.mkpath(".");

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString fileName = QString("project_autosave_%1.veditor").arg(timestamp);
    QString filePath = dir.filePath(fileName);

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(json.toUtf8());
        file.close();
        cleanOldBackups();
        emit autoSaved(filePath);
    }
}

void AutoSave::cleanOldBackups()
{
    QDir dir(m_config.backupDir);
    QStringList filters;
    filters << "project_autosave_*.veditor";
    QFileInfoList entries = dir.entryInfoList(filters, QDir::Files, QDir::Time);

    // entries sorted newest first by QDir::Time
    while (entries.size() > m_config.maxBackups) {
        QFile::remove(entries.takeLast().absoluteFilePath());
    }
}

void AutoSave::markCleanShutdown()
{
    stop();

    QFile marker(cleanShutdownMarkerPath());
    if (marker.open(QIODevice::WriteOnly)) {
        marker.write(QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8());
        marker.close();
    }
}

bool AutoSave::wasCleanShutdown()
{
    return QFile::exists(cleanShutdownMarkerPath());
}

bool AutoSave::hasRecoveryFile()
{
    if (wasCleanShutdown())
        return false;

    return !recoveryFiles().isEmpty();
}

QStringList AutoSave::recoveryFiles()
{
    QDir dir(defaultBackupDir());
    if (!dir.exists())
        return {};

    QStringList filters;
    filters << "project_autosave_*.veditor";
    QFileInfoList entries = dir.entryInfoList(filters, QDir::Files, QDir::Time);

    QStringList result;
    result.reserve(entries.size());
    for (const QFileInfo &info : entries)
        result.append(info.absoluteFilePath());

    return result;  // already sorted newest first by QDir::Time
}

QString AutoSave::recoverFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    return QString::fromUtf8(file.readAll());
}

void AutoSave::checkForRecovery()
{
    if (hasRecoveryFile()) {
        QStringList files = recoveryFiles();
        if (!files.isEmpty())
            emit recoveryAvailable(files);
    }
}

QString AutoSave::defaultBackupDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
           + "/.veditor/autosave";
}

QString AutoSave::cleanShutdownMarkerPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
           + "/.veditor/.clean_shutdown";
}
