#pragma once

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

class AutoSave : public QObject
{
    Q_OBJECT

public:
    explicit AutoSave(QObject *parent = nullptr);
    ~AutoSave();

    void start(const AutoSaveConfig &config);
    void stop();

    void setProjectData(std::function<QString()> dataCallback);
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

private:
    void checkForRecovery();
    static QString defaultBackupDir();
    static QString cleanShutdownMarkerPath();

    QTimer m_timer;
    AutoSaveConfig m_config;
    std::function<QString()> m_dataCallback;
};
