#pragma once

#include <QDockWidget>
#include <QColor>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

class MainWindow;
class QEvent;
class QPlainTextEdit;
class QPushButton;

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
    static QStringList buildArguments(const QString& configPath,
                                      const QString& prompt);
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
    void appendLog(const QString& text, const QColor& color = QColor());
    void processOutputLine(const QByteArray& line);
    bool writeMcpConfig();

    MainWindow *m_mainWindow = nullptr;
    mcp::McpHttpServer *m_server = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QString m_configPath;
};
