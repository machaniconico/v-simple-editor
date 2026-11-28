#include "AutoSave.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace {
QString g_fixedBackupName;
}

void autosavetest::setFixedBackupNameForTest(const QString &fileName)
{
    g_fixedBackupName = fileName;
}

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

    seedLastSavedHashFromNewestBackup();

    // Remove clean shutdown marker (session is now active)
    QFile::remove(cleanShutdownMarkerPath());

    if (m_config.enabled) {
        m_timer.start(m_config.interval * 1000);
    }

    // Defer recovery check to next event loop iteration
    // (MainWindow must be fully constructed before showing dialogs)
    QTimer::singleShot(0, this, &AutoSave::checkForRecovery);
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

    const QByteArray bytes = json.toUtf8();
    const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
    if (hash == m_lastSavedHash)
        return;

    QString fileName;
    if (!g_fixedBackupName.isEmpty()) {
        fileName = g_fixedBackupName;
        g_fixedBackupName.clear();
    } else {
        // UTC ms stamp plus a zero-padded max-plus-one counter: names stay unique
        // within one ms and always sort after the old local-time names ('u' > '2').
        const QString prefix = QString("project_autosave_utc_%1_")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz"));
        int next = 0;
        const QStringList same = dir.entryList({prefix + "*.veditor"}, QDir::Files);
        for (const QString &name : same) {
            const QString base = name.left(name.size() - 8);
            bool ok = false;
            const int n = base.mid(prefix.size()).toInt(&ok);
            if (ok && n + 1 > next)
                next = n + 1;
        }
        fileName = QString("%1%2.veditor").arg(prefix).arg(next, 3, 10, QChar('0'));
    }
    const QString filePath = dir.filePath(fileName);

    QSaveFile file(filePath);
    if (file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size()
        && file.commit()) {
        m_lastSavedHash = hash;
        cleanOldBackups();
        emit autoSaved(filePath);
    } else {
        emit saveFailed(filePath, file.errorString());
    }
}

void AutoSave::seedLastSavedHashFromNewestBackup()
{
    m_lastSavedHash.clear();
    QDir dir(m_config.backupDir);
    const QFileInfoList entries = dir.entryInfoList({"project_autosave_*.veditor"}, QDir::Files,
                                                    QDir::Name | QDir::Reversed);
    if (entries.isEmpty())
        return;
    QFile file(entries.first().absoluteFilePath());
    if (file.open(QIODevice::ReadOnly))
        m_lastSavedHash = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha1);
}

void AutoSave::cleanOldBackups()
{
    QDir dir(m_config.backupDir);
    QStringList filters;
    filters << "project_autosave_*.veditor";
    QFileInfoList entries = dir.entryInfoList(filters, QDir::Files,
                                           QDir::Name | QDir::Reversed);

    // entries sorted newest first by name (utc_ names sort after the old local-time names)
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
    QFileInfoList entries = dir.entryInfoList(filters, QDir::Files,
                                           QDir::Name | QDir::Reversed);

    QStringList result;
    result.reserve(entries.size());
    for (const QFileInfo &info : entries)
        result.append(info.absoluteFilePath());

    return result;  // sorted newest first by name
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
