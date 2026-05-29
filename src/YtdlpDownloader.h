#pragma once
#include <QObject>
#include <QProcess>
#include <QString>

class YtdlpDownloader : public QObject {
    Q_OBJECT
public:
    explicit YtdlpDownloader(QObject* parent = nullptr);
    ~YtdlpDownloader() override;

    // YouTube URL かどうか判定 (regex r'https?://(www\.)?(youtube\.com|youtu\.be)/')
    static bool isYoutubeUrl(const QString& url);

    // outtmpl 構築 — %(title).100B 形式で UTF-8 byte limit (Windows MAX_PATH safety)
    static QString buildOutputTemplate(const QString& outputDir);

    // download 開始 — yt-dlp.exe を subprocess 呼び出し
    void start(const QString& url, const QString& outputDir);

    // cancel — subprocess kill
    void cancel();

    bool isRunning() const;

signals:
    void progressUpdated(int percent, const QString& message);
    void finished(bool ok, const QString& outputPath, const QString& errorMessage);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessStdout();
    void onProcessStderr();

private:
    QProcess* m_process = nullptr;
    QString m_currentOutputDir;
    QString m_stderrAccumulated;
};
