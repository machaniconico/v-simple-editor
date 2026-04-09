#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QLabel>
#include <QObject>
#include <QString>
#include <QProcess>
#include "ProjectFile.h"
#include "ProjectSettings.h"

// ---------------------------------------------------------------------------
// RemotionExportConfig
// ---------------------------------------------------------------------------

struct RemotionExportConfig {
    QString outputDir;
    QString projectName        = "my-remotion-video";
    int     fps                = 30;
    int     width              = 1920;
    int     height             = 1080;
    int     durationInFrames   = 0;   // 0 = auto-calculated from timeline
    bool    includeAssets      = true;
    bool    generatePackageJson = true;
    QString remotionVersion    = "4.0";
};

// ---------------------------------------------------------------------------
// RemotionExporter
// ---------------------------------------------------------------------------

class RemotionExporter : public QObject
{
    Q_OBJECT

public:
    explicit RemotionExporter(QObject *parent = nullptr);

    bool exportProject(const RemotionExportConfig &config, const ProjectData &data);

signals:
    void exportProgress(int percent);
    void exportComplete(QString outputDir);
    void exportError(QString message);

private:
    // Directory / file creation helpers
    bool createDirectoryStructure(const QString &base, const QString &projectName);
    bool writePackageJson(const QString &base, const RemotionExportConfig &config);
    bool writeTsConfig(const QString &base);
    bool writeRootTsx(const QString &srcDir, const RemotionExportConfig &config);
    bool writeVideoTsx(const QString &srcDir, const RemotionExportConfig &config,
                       const ProjectData &data);
    bool writeTimelineTs(const QString &libDir, const RemotionExportConfig &config,
                         const ProjectData &data);

    // Component generators
    bool writeVideoClipTsx(const QString &compDir);
    bool writeAudioClipTsx(const QString &compDir);
    bool writeTextOverlayTsx(const QString &compDir);
    bool writeImageOverlayTsx(const QString &compDir);
    bool writeTransitionTsx(const QString &compDir);
    bool writeEffectsTsx(const QString &compDir);

    // Asset copy
    bool copyAssets(const QString &assetsDir, const ProjectData &data);

    // Code generation helpers
    QString generateVideoCompositionBody(const RemotionExportConfig &config,
                                         const ProjectData &data) const;
    QString generateTimelineData(const RemotionExportConfig &config,
                                  const ProjectData &data) const;
    QString colorCorrectionToCSS(const ColorCorrection &cc) const;
    QString effectTypeToCSS(const VideoEffect &effect) const;
    QString interpolationName(KeyframePoint::Interpolation interp) const;
    QString animationTypeToRemotionCode(TextAnimationType type,
                                        const QString &progressVar) const;
    int calculateDurationInFrames(const ProjectData &data, int fps) const;

    bool writeFile(const QString &path, const QString &content);
};

// ---------------------------------------------------------------------------
// RemotionPreview
// ---------------------------------------------------------------------------

class RemotionPreview : public QObject
{
    Q_OBJECT

public:
    explicit RemotionPreview(QObject *parent = nullptr);
    ~RemotionPreview() override;

    void launchPreview(const QString &projectDir);
    void launchRender(const QString &projectDir, const QString &outputPath);
    void terminate();

signals:
    void processOutput(QString text);
    void processFinished(int exitCode);

private slots:
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);

private:
    QProcess *m_process = nullptr;
};

// ---------------------------------------------------------------------------
// RemotionExportDialog
// ---------------------------------------------------------------------------

class RemotionExportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RemotionExportDialog(const ProjectConfig &project,
                                  const ProjectData  &data,
                                  QWidget *parent = nullptr);

    RemotionExportConfig config() const { return m_config; }

private slots:
    void onBrowseOutput();
    void onExport();
    void onExportAndOpen();
    void onExportProgress(int percent);
    void onExportComplete(QString outputDir);
    void onExportError(QString message);
    void updateStructurePreview();

private:
    void setupUI();
    void setControlsEnabled(bool enabled);
    void runNpmInstall(const QString &projectDir);

    RemotionExportConfig m_config;
    const ProjectData   &m_data;

    QLineEdit   *m_projectNameEdit  = nullptr;
    QLineEdit   *m_outputDirEdit    = nullptr;
    QSpinBox    *m_fpsSpin          = nullptr;
    QSpinBox    *m_widthSpin        = nullptr;
    QSpinBox    *m_heightSpin       = nullptr;
    QCheckBox   *m_includeAssetsCheck    = nullptr;
    QCheckBox   *m_generatePkgJsonCheck  = nullptr;
    QCheckBox   *m_runNpmCheck           = nullptr;
    QTreeWidget *m_structureTree    = nullptr;
    QProgressBar *m_progressBar     = nullptr;
    QLabel      *m_statusLabel      = nullptr;
    QPushButton *m_exportBtn        = nullptr;
    QPushButton *m_exportOpenBtn    = nullptr;

    RemotionExporter *m_exporter = nullptr;
    bool m_openAfterExport       = false;
};
