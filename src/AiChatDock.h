#pragma once

#include <QDockWidget>
#include <QColor>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

class MainWindow;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace mcp { class McpHttpServer; }

class AiChatDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit AiChatDock(MainWindow *mainWindow, mcp::McpHttpServer *server,
                        QWidget *parent = nullptr);
    ~AiChatDock() override;

    static QProcessEnvironment childEnvironment(
        const QProcessEnvironment& base);
    struct CliCommand {
        QString program;         // 実行ファイルの絶対パス (空 = 未解決)
        bool needsShell = false; // .cmd/.bat → cmd.exe /c 経由
        QStringList searched;    // 表示用: 探索した候補名とディレクトリ
    };
    static CliCommand resolveCliCommand(const QString& command,
                                        const QStringList& searchPaths = {});
    static QStringList buildArguments(const QString& configPath,
                                      const QString& sessionId);
    // cmd.exe /s /c 用。各引数を "…" で囲み、" % ! \r \n を含む値は拒否する。
    static QString buildShellCommandLine(const QString& program,
                                         const QStringList& args,
                                         QString* error = nullptr);
    static QByteArray buildMcpConfig(quint16 port, const QString& token);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void sendPrompt();
    void stopProcess();
    void readStandardOutput();
    void readStandardError();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void processError(QProcess::ProcessError error);

private:
    static void cleanupStaleConfigs();
    void appendLog(const QString& text, const QColor& color = QColor());
    void processOutputLine(const QByteArray& line);
    bool writeMcpConfig();
    void updateRunningStatus();
    void armWatchdog();

    MainWindow *m_mainWindow = nullptr;
    mcp::McpHttpServer *m_server = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProcess *m_process = nullptr;
    QTimer *m_watchdog = nullptr;
    QTimer *m_statusTimer = nullptr;
    QElapsedTimer m_runTimer;
    int m_watchdogTimeoutMs = 120000;
    QByteArray m_stdoutBuffer;
    QByteArray m_stderrBuffer;
    QString m_stderrText;
    QString m_configPath;
    QString m_sessionId;
    QString m_sessionWorkDir;
    bool m_gotResult = false;
    bool m_gotError = false;
    bool m_sessionResumeFailureReported = false;
};
