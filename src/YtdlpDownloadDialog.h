#pragma once
#include <QDialog>
#include <QString>

class QLineEdit;
class QProgressBar;
class QLabel;
class QPushButton;
class YtdlpDownloader;

class YtdlpDownloadDialog : public QDialog {
    Q_OBJECT
public:
    explicit YtdlpDownloadDialog(QWidget* parent = nullptr);
    ~YtdlpDownloadDialog() override;

    // ダウンロード完了後の output file path (空なら未完了 / 失敗)
    QString downloadedFilePath() const;

    // output ディレクトリ (default: QStandardPaths::DownloadLocation/v-simple-editor-imports)
    void setOutputDir(const QString& dir);

private slots:
    void onDownloadClicked();
    void onCancelClicked();
    void onProgressUpdated(int percent, const QString& message);
    void onFinished(bool ok, const QString& outputPath, const QString& errorMessage);

private:
    YtdlpDownloader* m_downloader = nullptr;
    QLineEdit*   m_urlEdit       = nullptr;
    QProgressBar* m_progressBar  = nullptr;
    QLabel*      m_statusLabel   = nullptr;
    QPushButton* m_downloadButton = nullptr;
    QPushButton* m_cancelButton  = nullptr;
    QPushButton* m_closeButton   = nullptr;
    QString m_outputDir;
    QString m_downloadedFilePath;
};
