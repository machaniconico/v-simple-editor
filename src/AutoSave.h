#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>

struct AutoSaveConfig {
    int interval = 120;        // seconds
    int maxBackups = 5;
    bool enabled = true;
    QString backupDir;         // defaults to ~/.veditor/autosave/
};

namespace autosavetest {
// Test seam: when non-empty, the next performAutoSave writes under this file
// name (once). Default empty = the generated utc name.
void setFixedBackupNameForTest(const QString &fileName);
}

class AutoSave : public QObject
{
    Q_OBJECT

public:
    explicit AutoSave(QObject *parent = nullptr);
    ~AutoSave();

    void start(const AutoSaveConfig &config);
    void stop();

    void setConfig(const AutoSaveConfig &config) { m_config = config; }
    void setProjectData(std::function<QString()> dataCallback);
    void resetLastSavedHash() { m_lastSavedHash.clear(); }
    void seedLastSavedHashFromNewestBackup();
    void performAutoSave();
    void cleanOldBackups();
    void markCleanShutdown();

    static bool wasCleanShutdown();
    static bool hasRecoveryFile();
    static QStringList recoveryFiles();
    static QString recoverFromFile(const QString &filePath);

    bool isRunning() const { return m_timer.isActive(); }

signals:
    void autoSaved(const QString &filePath);
    void recoveryAvailable(const QStringList &files);
    void saveFailed(const QString &filePath, const QString &error);

private:
    void checkForRecovery();
    static QString defaultBackupDir();
    static QString cleanShutdownMarkerPath();

    QTimer m_timer;
    AutoSaveConfig m_config;
    std::function<QString()> m_dataCallback;
    QByteArray m_lastSavedHash;
};
