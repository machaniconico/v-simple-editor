#include "AiChatDock.h"

#include "MainWindow.h"
#include "mcp/McpHttpServer.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QUuid>

AiChatDock::AiChatDock(MainWindow *mainWindow, mcp::McpHttpServer *server,
                       QWidget *parent)
    : QDockWidget(QStringLiteral("AI チャット"), parent)
    , m_mainWindow(mainWindow)
    , m_server(server)
{
    setObjectName(QStringLiteral("AiChatDock"));

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    m_log = new QPlainTextEdit(content);
    m_log->setReadOnly(true);
    m_log->setObjectName(QStringLiteral("AiChatLog"));
    layout->addWidget(m_log, 1);

    m_input = new QPlainTextEdit(content);
    m_input->setObjectName(QStringLiteral("AiChatInput"));
    m_input->setPlaceholderText(QStringLiteral("Claude Code に依頼する内容を入力…"));
    m_input->setFixedHeight(72);
    m_input->installEventFilter(this);
    layout->addWidget(m_input);

    auto *buttons = new QHBoxLayout();
    m_sendButton = new QPushButton(QStringLiteral("送信"), content);
    m_sendButton->setDefault(true);
    m_sendButton->setToolTip(QStringLiteral("Ctrl+Enter でも送信できます。"));
    m_stopButton = new QPushButton(QStringLiteral("停止"), content);
    m_stopButton->setEnabled(false);
    connect(m_sendButton, &QPushButton::clicked, this, &AiChatDock::sendPrompt);
    connect(m_stopButton, &QPushButton::clicked, this, &AiChatDock::stopProcess);
    buttons->addStretch(1);
    buttons->addWidget(m_sendButton);
    buttons->addWidget(m_stopButton);
    layout->addLayout(buttons);

    setWidget(content);
    setMinimumWidth(360);

    // Anthropic の API キー方式は従量課金で、Pro/Max のサブスク枠は使えない。
    // サブスク枠を使うにはログイン済みの claude CLI をサブプロセスとして回し、
    // そこから MCP 経由でエディタを操作させるしかない。子プロセスの環境から
    // ANTHROPIC_API_KEY / ANTHROPIC_AUTH_TOKEN を削除しないと、Claude Code は
    // Console の従量課金に落ちるため、起動時に childEnvironment() を適用する。
}

AiChatDock::~AiChatDock()
{
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning)
            m_process->kill();
        m_process->waitForFinished(2000);
    }
    if (!m_configPath.isEmpty())
        QFile::remove(m_configPath);
}

QProcessEnvironment AiChatDock::childEnvironment(
    const QProcessEnvironment& base)
{
    QProcessEnvironment environment = base;
    environment.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    environment.remove(QStringLiteral("ANTHROPIC_AUTH_TOKEN"));
    return environment;
}

QStringList AiChatDock::buildArguments(const QString& configPath,
                                       const QString& prompt)
{
    // プロンプトは -p の直後に置く。--allowedTools は可変長引数なので、
    // その後ろにプロンプトを置くと「許可ツール名のもう 1 つ」として飲み込まれ、
    // claude が "Input must be provided either through stdin or as a prompt
    // argument when using --print" で終了する (実測、2026-08-27)。
    // 可変長オプションは必ず最後に置くこと。
    return {
        QStringLiteral("-p"), prompt,
        QStringLiteral("--output-format"), QStringLiteral("stream-json"),
        QStringLiteral("--verbose"),
        QStringLiteral("--mcp-config"), configPath,
        QStringLiteral("--strict-mcp-config"),
        QStringLiteral("--allowedTools"), QStringLiteral("mcp__veditor")
    };
}

QByteArray AiChatDock::buildMcpConfig(quint16 port, const QString& token)
{
    const QJsonObject root{
        {QStringLiteral("mcpServers"), QJsonObject{
            {QStringLiteral("veditor"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("http")},
                {QStringLiteral("url"),
                 QStringLiteral("http://127.0.0.1:%1/mcp").arg(port)},
                {QStringLiteral("headers"), QJsonObject{
                    {QStringLiteral("Authorization"),
                     QStringLiteral("Bearer ") + token}
                }}
            }}
        }}
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool AiChatDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return
            && keyEvent->modifiers().testFlag(Qt::ControlModifier)) {
            sendPrompt();
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void AiChatDock::appendLog(const QString& text, const QColor& color)
{
    if (!m_log)
        return;
    QTextCursor cursor(m_log->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    if (color.isValid())
        format.setForeground(color);
    cursor.insertText(text + QLatin1Char('\n'), format);
    m_log->setTextCursor(cursor);
    m_log->ensureCursorVisible();
}

bool AiChatDock::writeMcpConfig()
{
    if (!m_server || !m_server->isRunning())
        return false;

    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.isEmpty())
        tempDir = QDir::tempPath();
    if (!m_configPath.isEmpty())
        QFile::remove(m_configPath);
    m_configPath = QDir(tempDir).filePath(
        QStringLiteral("veditor-mcp-%1.json")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    QFile config(m_configPath);
    if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray data = buildMcpConfig(m_server->port(), m_server->token());
    const bool writeOk = config.write(data) == data.size();
    config.close();
    const bool permissionsOk = QFile::setPermissions(
        m_configPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!permissionsOk) {
        appendLog(QStringLiteral(
            "警告: MCP 設定ファイルの所有者専用権限を設定できませんでした。"),
                  QColor(Qt::gray));
    }
    return writeOk;
}

void AiChatDock::sendPrompt()
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        return;

    const QString prompt = m_input ? m_input->toPlainText().trimmed() : QString();
    if (prompt.isEmpty())
        return;

    if (!m_server || !m_server->isRunning()) {
        if (m_mainWindow)
            m_mainWindow->toggleMcpServer(true);
    }
    if (!m_server || !m_server->isRunning()) {
        appendLog(QStringLiteral("MCP サーバを起動できませんでした。"));
        return;
    }
    if (!writeMcpConfig()) {
        appendLog(QStringLiteral("MCP 設定ファイルを書き込めませんでした。"));
        return;
    }

    if (m_process)
        m_process->deleteLater();
    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &AiChatDock::readStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &AiChatDock::readStandardError);
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &AiChatDock::processFinished);
    connect(m_process, &QProcess::errorOccurred,
            this, &AiChatDock::processError);

    const QString command = QSettings(QStringLiteral("VSimpleEditor"),
                                      QStringLiteral("Preferences"))
        .value(QStringLiteral("aiChatCommand"), QStringLiteral("claude"))
        .toString();
    m_process->setProcessEnvironment(childEnvironment(
        QProcessEnvironment::systemEnvironment()));
    const QString workDir = m_mainWindow ? m_mainWindow->projectDirectory()
                                         : QString();
    m_process->setWorkingDirectory(workDir.isEmpty() ? QDir::homePath() : workDir);

    appendLog(QStringLiteral("You: ") + prompt);
    m_input->clear();
    m_sendButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_stdoutBuffer.clear();
    m_process->start(command, buildArguments(m_configPath, prompt));
}

void AiChatDock::stopProcess()
{
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->kill();
}

void AiChatDock::readStandardOutput()
{
    if (!m_process)
        return;
    m_stdoutBuffer += m_process->readAllStandardOutput();
    while (true) {
        const qsizetype newline = m_stdoutBuffer.indexOf('\n');
        if (newline < 0)
            break;
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (!line.isEmpty())
            processOutputLine(line);
    }
}

void AiChatDock::readStandardError()
{
    if (!m_process)
        return;
    const QString error = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
    if (!error.isEmpty())
        appendLog(QStringLiteral("stderr: ") + error, QColor(Qt::gray));
}

void AiChatDock::processOutputLine(const QByteArray& line)
{
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject())
        return;
    const QJsonObject object = document.object();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("assistant")) {
        const QJsonArray content = object.value(QStringLiteral("message"))
            .toObject().value(QStringLiteral("content")).toArray();
        for (const QJsonValue& value : content) {
            const QJsonObject item = value.toObject();
            const QString itemType = item.value(QStringLiteral("type")).toString();
            if (itemType == QStringLiteral("text"))
                appendLog(item.value(QStringLiteral("text")).toString());
            else if (itemType == QStringLiteral("tool_use"))
                appendLog(QStringLiteral("ツール実行: ")
                              + item.value(QStringLiteral("name")).toString(),
                          QColor(Qt::gray));
        }
    } else if (type == QStringLiteral("result")) {
        appendLog(object.value(QStringLiteral("result")).toString());
        if (m_sendButton)
            m_sendButton->setEnabled(true);
    }
}

void AiChatDock::processFinished(int, QProcess::ExitStatus)
{
    readStandardOutput();
    readStandardError();
    if (!m_stdoutBuffer.trimmed().isEmpty()) {
        processOutputLine(m_stdoutBuffer.trimmed());
        m_stdoutBuffer.clear();
    }
    if (m_sendButton)
        m_sendButton->setEnabled(true);
    if (m_stopButton)
        m_stopButton->setEnabled(false);
    if (!m_configPath.isEmpty()) {
        QFile::remove(m_configPath);
        m_configPath.clear();
    }
}

void AiChatDock::processError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        const QString command = QSettings(QStringLiteral("VSimpleEditor"),
                                          QStringLiteral("Preferences"))
            .value(QStringLiteral("aiChatCommand"), QStringLiteral("claude"))
            .toString();
        appendLog(QStringLiteral("%1 が見つかりません。Claude Code をインストールし、PATH を通してください "
                               "(npm i -g @anthropic-ai/claude-code)。設定の aiChatCommand で別の CLI を指定できます。")
                      .arg(command));
        if (!m_configPath.isEmpty()) {
            QFile::remove(m_configPath);
            m_configPath.clear();
        }
    } else if (m_process) {
        appendLog(QStringLiteral("AI チャットのプロセスでエラーが発生しました: ")
                      + m_process->errorString(), QColor(Qt::gray));
    }
    if (m_sendButton)
        m_sendButton->setEnabled(true);
    if (m_stopButton)
        m_stopButton->setEnabled(false);
}
