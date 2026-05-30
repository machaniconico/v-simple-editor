#include "YtdlpDownloader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QRegularExpression>
#include <QProcess>

static const QRegularExpression kYoutubeRegex(
    QStringLiteral(R"(https?://(www\.)?(youtube\.com|youtu\.be)/)"));

static const QRegularExpression kProgressRegex(
    QStringLiteral(R"(\[download\]\s+(\d+(?:\.\d+)?)%)"));

YtdlpDownloader::YtdlpDownloader(QObject* parent)
    : QObject(parent)
{
}

YtdlpDownloader::~YtdlpDownloader()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
}

bool YtdlpDownloader::isYoutubeUrl(const QString& url)
{
    return kYoutubeRegex.match(url).hasMatch();
}

QString YtdlpDownloader::buildOutputTemplate(const QString& outputDir)
{
    return outputDir + QStringLiteral("/%(title).100B.%(ext)s");
}

void YtdlpDownloader::start(const QString& url, const QString& outputDir)
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        return;
    }

    // Determine yt-dlp.exe path: prefer tools/yt-dlp.exe next to the exe,
    // fall back to system PATH.
    QString ytdlpBin;
    const QString toolsPath = QCoreApplication::applicationDirPath()
                              + QStringLiteral("/tools/yt-dlp.exe");
    if (QFileInfo::exists(toolsPath)) {
        ytdlpBin = toolsPath;
    } else {
        ytdlpBin = QStringLiteral("yt-dlp.exe");
    }

    m_currentOutputDir = outputDir;
    m_stderrAccumulated.clear();

    // Ensure output dir exists.
    QDir().mkpath(outputDir);

    const QString outtmpl = buildOutputTemplate(outputDir);

    QStringList args;
    args << QStringLiteral("-f")
         << QStringLiteral("bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best")
         << QStringLiteral("--merge-output-format") << QStringLiteral("mp4")
         << QStringLiteral("-o") << outtmpl
         << QStringLiteral("--retries") << QStringLiteral("10")
         << QStringLiteral("--fragment-retries") << QStringLiteral("10")
         << QStringLiteral("--file-access-retries") << QStringLiteral("5")
         << QStringLiteral("--socket-timeout") << QStringLiteral("30")
         << QStringLiteral("--http-chunk-size") << QStringLiteral("10485760")
         << QStringLiteral("--newline")
         << url;

    if (!m_process) {
        m_process = new QProcess(this);
        connect(m_process, &QProcess::readyReadStandardOutput,
                this, &YtdlpDownloader::onProcessStdout);
        connect(m_process, &QProcess::readyReadStandardError,
                this, &YtdlpDownloader::onProcessStderr);
        connect(m_process,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &YtdlpDownloader::onProcessFinished);
    }

    m_cancelled = false;
    m_process->start(ytdlpBin, args);
}

void YtdlpDownloader::cancel()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_cancelled = true;
    m_process->kill();
    // finished() は同期 emit しない。kill 完了後に届く QProcess::finished を
    // onProcessFinished で 1 回だけ "cancelled by user" として emit する。
    // (同期 emit すると二重発火し、かつ kill が非同期なため cancel 直後の
    //  再 start が running-guard(L41) に弾かれて無言ドロップする競合を生む。)
}

bool YtdlpDownloader::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void YtdlpDownloader::onProcessStdout()
{
    if (!m_process) return;
    const QString text = QString::fromUtf8(m_process->readAllStandardOutput());
    for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QRegularExpressionMatch m = kProgressRegex.match(line);
        if (m.hasMatch()) {
            const int percent = qBound(0, qRound(m.captured(1).toDouble()), 100);
            emit progressUpdated(percent, line.trimmed());
        }
    }
}

void YtdlpDownloader::onProcessStderr()
{
    if (!m_process) return;
    m_stderrAccumulated += QString::fromUtf8(m_process->readAllStandardError());
}

void YtdlpDownloader::onProcessFinished(int exitCode, QProcess::ExitStatus /*status*/)
{
    if (m_cancelled) {
        // kill() による終了。ここで初めて & 1 回だけ cancel の finished を emit する。
        // この時点で process は NotRunning なので後続の start() も正しく通る。
        m_cancelled = false;
        emit finished(false, QString(), QStringLiteral("cancelled by user"));
        return;
    }
    if (exitCode != 0) {
        emit finished(false, QString(), m_stderrAccumulated.trimmed());
        return;
    }

    // Scan outputDir for the most recently modified .mp4 file.
    QDir dir(m_currentOutputDir);
    const QFileInfoList mp4List = dir.entryInfoList(
        QStringList() << QStringLiteral("*.mp4"),
        QDir::Files, QDir::Time);

    if (!mp4List.isEmpty()) {
        emit finished(true, mp4List.first().absoluteFilePath(), QString());
    } else {
        emit finished(false, QString(),
                      QStringLiteral("yt-dlp succeeded but no .mp4 found in output dir"));
    }
}
