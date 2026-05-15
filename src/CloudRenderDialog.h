#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

#include "CloudRenderClient.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

class CloudRenderDialog : public QDialog {
    Q_OBJECT
public:
    explicit CloudRenderDialog(QWidget* parent = nullptr);
    ~CloudRenderDialog() override = default;

private slots:
    void onBrowseLocalFile();
    void onSubmitClicked();
    void onCancelSelectedClicked();
    void onProviderChanged();
    void onPollTimeout();

    void onJobSubmitted(const QString& jobId);
    void onJobProgress(const QString& jobId, int percent);
    void onJobCompleted(const QString& jobId, const QString& outputUrl);
    void onJobFailed(const QString& jobId, const QString& error);

private:
    cloudrender::Provider currentProvider() const;
    QString providerSettingsKey(cloudrender::Provider provider) const;
    void persistApiKey(cloudrender::Provider provider);
    void loadSavedApiKey(cloudrender::Provider provider);
    void applyProviderDefaults(cloudrender::Provider provider);

    int ensureJobRow(const QString& jobId, const QString& localFilePath);
    void setJobOutputUrl(const QString& jobId, const QString& outputUrl);
    void setJobStatus(const QString& jobId,
                      cloudrender::JobStatus status,
                      const QString& detail = QString());
    void setJobProgressValue(const QString& jobId, int percent);
    QString statusText(cloudrender::JobStatus status,
                       const QString& detail = QString()) const;
    void refreshPollTimer();

    cloudrender::Client* m_client = nullptr;

    QComboBox*      m_providerCombo   = nullptr;
    QLineEdit*      m_endpointEdit    = nullptr;
    QLineEdit*      m_apiKeyEdit      = nullptr;
    QCheckBox*      m_saveApiKeyCheck = nullptr;
    QLineEdit*      m_localFileEdit   = nullptr;
    QLineEdit*      m_inputUrlEdit    = nullptr;
    QLineEdit*      m_outputUrlEdit   = nullptr;
    QPlainTextEdit* m_ffmpegArgsEdit  = nullptr;
    QPushButton*    m_submitButton    = nullptr;
    QPushButton*    m_cancelButton    = nullptr;
    QTableWidget*   m_jobTable        = nullptr;
    QTimer*         m_pollTimer       = nullptr;

    bool m_providerLoaded = false;
    cloudrender::Provider m_loadedProvider = cloudrender::Provider::Generic;

    QHash<QString, int> m_jobIdToRow;
    QHash<QString, cloudrender::JobStatus> m_jobStates;
    QHash<QString, QString> m_jobErrors;
    QHash<QString, cloudrender::ProviderConfig> m_jobConfigs;
};
