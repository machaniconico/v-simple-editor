#include "AiChatDock.h"

#include "MainWindow.h"
#include "mcp/McpHttpServer.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
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
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>

namespace {

constexpr int kDefaultAiChatTimeoutMs = 120000;

bool containsUnsafeShellCharacter(const QString& value)
{
    return value.contains(QLatin1Char('"'))
        || value.contains(QLatin1Char('%'))
        || value.contains(QLatin1Char('!'))
        || value.contains(QLatin1Char('\r'))
        || value.contains(QLatin1Char('\n'));
}

} // namespace

AiChatDock::AiChatDock(MainWindow *mainWindow, mcp::McpHttpServer *server,
                       QWidget *parent)
    : QDockWidget(QStringLiteral("AI チャット"), parent)
    , m_mainWindow(mainWindow)
    , m_server(server)
{
    cleanupStaleConfigs();
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
    m_statusLabel = new QLabel(QStringLiteral("待機中"), content);
    m_statusLabel->setObjectName(QStringLiteral("AiChatStatus"));
    connect(m_sendButton, &QPushButton::clicked, this, &AiChatDock::sendPrompt);
    connect(m_stopButton, &QPushButton::clicked, this, &AiChatDock::stopProcess);
    buttons->addWidget(m_statusLabel);
    buttons->addStretch(1);
    buttons->addWidget(m_sendButton);
    buttons->addWidget(m_stopButton);
    layout->addLayout(buttons);

    // 接続状態と接続ボタン (送信ボタンの行の直下)。MCP サーバの待受と claude CLI の
    // 検出状況を常時表示し、サーバが止まっていればここから起動 (稼働中は停止) できる。
    auto *connection = new QHBoxLayout();
    m_connectionLabel = new QLabel(content);
    m_connectionLabel->setObjectName(QStringLiteral("AiChatConnectionStatus"));
    m_connectionLabel->setTextFormat(Qt::PlainText);
    m_providerCombo = new QComboBox(content);
    m_providerCombo->setObjectName(QStringLiteral("AiChatProviderCombo"));
    m_providerCombo->addItem(QStringLiteral("Claude Code"), QStringLiteral("claude"));
    m_providerCombo->addItem(QStringLiteral("Codex CLI"), QStringLiteral("codex"));
    m_providerCombo->setToolTip(QStringLiteral(
        "この Dock が起動する CLI。どちらもログイン済みのサブスク枠で動き、MCP でこのエディタを操作します。"));
    {
        QSettings settings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
        const QString saved = settings.value(QStringLiteral("aiChatProvider"),
                                             QStringLiteral("claude")).toString();
        m_provider = saved == QStringLiteral("codex") ? Provider::Codex : Provider::Claude;
        m_providerCombo->setCurrentIndex(m_provider == Provider::Codex ? 1 : 0);
    }
    connect(m_providerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
        const Provider next = m_providerCombo->itemData(index).toString()
                == QStringLiteral("codex") ? Provider::Codex : Provider::Claude;
        if (next != m_provider) {
            // セッション id とモデルは CLI ごとの値なので切替時に捨てる。
            m_provider = next;
            m_sessionId.clear();
            m_model.clear();
        }
        QSettings settings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
        settings.setValue(QStringLiteral("aiChatProvider"),
                          m_provider == Provider::Codex ? QStringLiteral("codex")
                                                        : QStringLiteral("claude"));
        refreshConnectionStatus();
    });
    m_connectButton = new QPushButton(QStringLiteral("接続"), content);
    m_connectButton->setObjectName(QStringLiteral("AiChatConnectButton"));
    connect(m_connectButton, &QPushButton::clicked, this, &AiChatDock::toggleConnection);
    connection->addWidget(m_connectionLabel, 1);
    connection->addWidget(m_providerCombo);
    connection->addWidget(m_connectButton);
    layout->addLayout(connection);
    if (m_server) {
        connect(m_server, &mcp::McpHttpServer::started, this,
                [this](quint16) { refreshConnectionStatus(); });
        connect(m_server, &mcp::McpHttpServer::stopped, this,
                &AiChatDock::refreshConnectionStatus);
        connect(m_server, &mcp::McpHttpServer::clientInitialized, this,
                [this](const QString& name, const QString& version) {
            m_lastClientName = name;
            m_lastClientVersion = version;
            refreshConnectionStatus();
            appendLog(QStringLiteral("MCP クライアント接続: %1 %2").arg(name, version).trimmed(),
                      QColor(Qt::gray));
        });
        m_lastClientName = m_server->lastClientName();
        m_lastClientVersion = m_server->lastClientVersion();
    }
    refreshConnectionStatus();

    setWidget(content);
    setMinimumWidth(360);

    m_watchdog = new QTimer(this);
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, &QTimer::timeout, this, [this]() {
        if (!m_process || m_process->state() == QProcess::NotRunning)
            return;
        const int seconds = qMax(1, (m_watchdogTimeoutMs + 999) / 1000);
        appendLog(QStringLiteral(
                       "%1 秒間応答がありません。停止ボタンで中断できます。")
                      .arg(seconds), QColor(Qt::red));
    });

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(1000);
    connect(m_statusTimer, &QTimer::timeout,
            this, &AiChatDock::updateRunningStatus);

    // Anthropic の API キー方式は従量課金で、Pro/Max のサブスク枠は使えない。
    // サブスク枠を使うにはログイン済みの claude CLI をサブプロセスとして回し、
    // そこから MCP 経由でエディタを操作させるしかない。子プロセスの環境から
    // ANTHROPIC_API_KEY / ANTHROPIC_AUTH_TOKEN を削除しないと、Claude Code は
    // Console の従量課金に落ちるため、起動時に childEnvironment() を適用する。
}

void AiChatDock::focusPrompt()
{
    if (m_input)
        m_input->setFocus(Qt::OtherFocusReason);
}

void AiChatDock::refreshConnectionStatus()
{
    if (!m_connectionLabel || !m_connectButton)
        return;
    const bool running = m_server && m_server->isRunning();
    const bool codex = m_provider == Provider::Codex;
    const QSettings settings(QStringLiteral("VSimpleEditor"),
                             QStringLiteral("Preferences"));
    const QString command = codex
        ? settings.value(QStringLiteral("aiChatCodexCommand"), QStringLiteral("codex")).toString()
        : settings.value(QStringLiteral("aiChatCommand"), QStringLiteral("claude")).toString();
    const CliCommand cli = resolveCliCommand(command);
    const QString serverText = running
        ? QStringLiteral("● MCP サーバ: 待受中 (ポート %1)").arg(m_server->port())
        : QStringLiteral("○ MCP サーバ: 停止中");
    const QString clientText = m_lastClientName.isEmpty()
        ? QStringLiteral("接続元: まだ接続なし")
        : QStringLiteral("接続元: %1 %2").arg(m_lastClientName, m_lastClientVersion).trimmed();
    QString model = m_model;
    if (model.isEmpty() && codex)
        model = codexConfiguredModel();
    const QString modelText = model.isEmpty()
        ? (codex ? QStringLiteral("モデル: Codex 既定") : QStringLiteral("モデル: 送信後に表示"))
        : QStringLiteral("モデル: %1").arg(model);
    const QString cliText = QStringLiteral("CLI: %1 %2")
        .arg(command, cli.program.isEmpty() ? QStringLiteral("未検出") : QStringLiteral("検出済み"));
    m_connectionLabel->setText(serverText + QStringLiteral(" / ") + clientText
                               + QLatin1Char('\n') + cliText + QStringLiteral(" / ") + modelText);
    m_connectionLabel->setStyleSheet(running
        ? QStringLiteral("QLabel { color: #3fb950; }")
        : QStringLiteral("QLabel { color: #8b949e; }"));
    const QString installHint = codex
        ? QStringLiteral("npm i -g @openai/codex")
        : QStringLiteral("npm i -g @anthropic-ai/claude-code");
    m_connectionLabel->setToolTip(cli.program.isEmpty()
        ? QStringLiteral("%1 が見つかりません。%2 を実行してください。\n探索:\n%3")
              .arg(command, installHint, cli.searched.join(QLatin1Char('\n')))
        : cli.program);
    m_connectButton->setText(running ? QStringLiteral("切断") : QStringLiteral("接続"));
    m_connectButton->setToolTip(running
        ? QStringLiteral("MCP サーバを停止します (Claude Code / Codex CLI からの接続も切れます)")
        : QStringLiteral("MCP サーバを起動して、Claude Code から接続できるようにします"));
}

void AiChatDock::processCodexOutputLine(const QJsonObject& object)
{
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("thread.started")) {
        const QString threadId = object.value(QStringLiteral("thread_id")).toString();
        if (!threadId.isEmpty())
            m_sessionId = threadId;
        return;
    }
    if (type == QStringLiteral("item.started") || type == QStringLiteral("item.completed")) {
        const QJsonObject item = object.value(QStringLiteral("item")).toObject();
        const QString itemType = item.value(QStringLiteral("type")).toString();
        const bool completed = type == QStringLiteral("item.completed");
        if (itemType == QStringLiteral("agent_message")) {
            if (completed)
                appendLog(item.value(QStringLiteral("text")).toString());
        } else if (itemType == QStringLiteral("mcp_tool_call")) {
            const QString tool = item.value(QStringLiteral("server")).toString()
                + QLatin1Char('/') + item.value(QStringLiteral("tool")).toString();
            if (!completed)
                appendLog(QStringLiteral("ツール実行: ") + tool, QColor(Qt::gray));
            else if (item.value(QStringLiteral("status")).toString() == QStringLiteral("failed"))
                appendLog(QStringLiteral("ツール失敗: ") + tool, QColor(Qt::red));
        } else if (itemType == QStringLiteral("command_execution")) {
            if (!completed) {
                appendLog(QStringLiteral("コマンド実行: ")
                              + item.value(QStringLiteral("command")).toString(),
                          QColor(Qt::gray));
            }
        } else if (itemType == QStringLiteral("error")) {
            appendLog(QStringLiteral("codex エラー: ")
                          + item.value(QStringLiteral("message")).toString(),
                      QColor(Qt::red));
            m_gotError = true;
        }
        return;
    }
    if (type == QStringLiteral("turn.completed")) {
        m_gotResult = true;
        return;
    }
    if (type == QStringLiteral("turn.failed") || type == QStringLiteral("error")) {
        QString message = object.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            message = object.value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString();
        }
        appendLog(QStringLiteral("codex エラー: ")
                      + (message.isEmpty() ? QStringLiteral("(詳細なし)") : message),
                  QColor(Qt::red));
        m_gotError = true;
        m_gotResult = true;
    }
}

QStringList AiChatDock::buildCodexArguments(quint16 port, const QString& token,
                                            const QString& editorPath,
                                            const QString& threadId)
{
    // 値は TOML のシングルクォート文字列にする (" を含むと cmd.exe 経由で拒否される)。
    // -s はトップレベルの exec だけが受けるので resume では付けない。
    QStringList arguments{QStringLiteral("exec")};
    if (!threadId.isEmpty())
        arguments << QStringLiteral("resume");
    arguments << QStringLiteral("--json") << QStringLiteral("--skip-git-repo-check");
    if (threadId.isEmpty())
        arguments << QStringLiteral("-s") << QStringLiteral("read-only");
    arguments << QStringLiteral("-c")
              << QStringLiteral("mcp_servers.veditor.command='%1'")
                     .arg(QDir::fromNativeSeparators(editorPath))
              << QStringLiteral("-c")
              << QStringLiteral("mcp_servers.veditor.args=['--mcp-stdio','--port','%1']")
                     .arg(port)
              << QStringLiteral("-c")
              << QStringLiteral("mcp_servers.veditor.env={VEDITOR_MCP_TOKEN='%1'}")
                     .arg(token)
              // Codex は MCP ツール呼び出しごとに承認を求め、非対話の exec では
              // 「user cancelled MCP tool call」で自動拒否される。このエディタのツールは
              // モーダルを開かず Ctrl+Z で戻せるので、このサーバだけ自動承認にする。
              << QStringLiteral("-c")
              << QStringLiteral("mcp_servers.veditor.default_tools_approval_mode='approve'");
    if (!threadId.isEmpty())
        arguments << threadId;
    arguments << QStringLiteral("-"); // プロンプトは stdin
    return arguments;
}

QString AiChatDock::codexConfiguredModel()
{
    QString home = qEnvironmentVariable("CODEX_HOME");
    if (home.isEmpty())
        home = QDir::homePath() + QStringLiteral("/.codex");
    QFile file(QDir(home).filePath(QStringLiteral("config.toml")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*model\\s*=\\s*[\"']([^\"']+)[\"']"));
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        if (line.trimmed().startsWith(QLatin1Char('[')))
            break; // トップレベルの model だけ見る (profiles の中は無視)
        const QRegularExpressionMatch match = pattern.match(line);
        if (match.hasMatch())
            return match.captured(1);
    }
    return {};
}

void AiChatDock::toggleConnection()
{
    if (!m_mainWindow)
        return;
    const bool wasRunning = m_server && m_server->isRunning();
    m_mainWindow->toggleMcpServer(!wasRunning);
    refreshConnectionStatus();
    const bool running = m_server && m_server->isRunning();
    if (!wasRunning && running) {
        appendLog(QStringLiteral("MCP サーバを起動しました (ポート %1)。")
                      .arg(m_server->port()), QColor(Qt::gray));
    } else if (!wasRunning) {
        appendLog(QStringLiteral("MCP サーバを起動できませんでした。"), QColor(Qt::red));
    } else if (!running) {
        appendLog(QStringLiteral("MCP サーバを停止しました。"), QColor(Qt::gray));
    }
}

AiChatDock::~AiChatDock()
{
    if (m_watchdog)
        m_watchdog->stop();
    if (m_statusTimer)
        m_statusTimer->stop();
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
#ifdef Q_OS_WIN
            const QStringList taskkillArguments{
                QStringLiteral("/PID"), QString::number(m_process->processId()),
                QStringLiteral("/T"), QStringLiteral("/F")
            };
            QProcess::startDetached(QStringLiteral("taskkill"), taskkillArguments);
#else
            m_process->kill();
#endif
        }
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

AiChatDock::CliCommand AiChatDock::resolveCliCommand(
    const QString& command, const QStringList& searchPaths)
{
    CliCommand result;
    const auto setProgram = [&result](const QString& path) {
        result.program = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        const QString suffix = QFileInfo(result.program).suffix().toLower();
        result.needsShell = suffix == QStringLiteral("cmd")
            || suffix == QStringLiteral("bat");
    };

    if (command.isEmpty()) {
        result.searched.append(QStringLiteral("候補: (空のコマンド名)"));
        return result;
    }

    const QFileInfo commandInfo(command);
    if (commandInfo.isAbsolute() && commandInfo.exists()) {
        setProgram(command);
        return result;
    }

    QStringList candidates{command};
    const QString suffix = commandInfo.suffix().toLower();
    if (suffix != QStringLiteral("cmd")
        && suffix != QStringLiteral("bat")
        && suffix != QStringLiteral("exe")) {
        candidates.append(command + QStringLiteral(".cmd"));
        candidates.append(command + QStringLiteral(".bat"));
        candidates.append(command + QStringLiteral(".exe"));
    }

    for (const QString& candidate : candidates)
        result.searched.append(QStringLiteral("候補: ") + candidate);

    if (!searchPaths.isEmpty()) {
        for (const QString& path : searchPaths)
            result.searched.append(QStringLiteral("検索先: ") + path);
        for (const QString& candidate : candidates) {
            const QString resolved = QStandardPaths::findExecutable(
                candidate, searchPaths);
            if (!resolved.isEmpty()) {
                setProgram(resolved);
                return result;
            }
        }
        return result;
    }

    const QStringList pathEntries = qEnvironmentVariable("PATH")
        .split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString& path : pathEntries)
        result.searched.append(QStringLiteral("検索先: ") + path);
    for (const QString& candidate : candidates) {
        const QString resolved = QStandardPaths::findExecutable(candidate);
        if (!resolved.isEmpty()) {
            setProgram(resolved);
            return result;
        }
    }

    const QStringList fallbackPaths{
        QDir::home().filePath(QStringLiteral("AppData/Roaming/npm")),
        QDir::home().filePath(QStringLiteral(".local/bin"))
    };
    for (const QString& path : fallbackPaths)
        result.searched.append(QStringLiteral("検索先: ") + path);
    for (const QString& candidate : candidates) {
        const QString resolved = QStandardPaths::findExecutable(
            candidate, fallbackPaths);
        if (!resolved.isEmpty()) {
            setProgram(resolved);
            return result;
        }
    }
    return result;
}

void AiChatDock::cleanupStaleConfigs()
{
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.isEmpty())
        tempDir = QDir::tempPath();

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    const QFileInfoList configFiles = QDir(tempDir).entryInfoList(
        QStringList{QStringLiteral("veditor-mcp-*.json")}, QDir::Files);
    for (const QFileInfo& fileInfo : configFiles) {
        if (fileInfo.lastModified() <= cutoff)
            QFile::remove(fileInfo.absoluteFilePath());
    }
}

QStringList AiChatDock::buildArguments(const QString& configPath,
                                       const QString& sessionId)
{
    // プロンプトは argv に置かず stdin へ渡す。--allowedTools は可変長引数
    // なので必ず最後に置き、--resume はその前に置くこと。
    QStringList arguments{
        QStringLiteral("-p"),
        QStringLiteral("--output-format"), QStringLiteral("stream-json"),
        QStringLiteral("--verbose"),
        QStringLiteral("--mcp-config"), configPath,
        QStringLiteral("--strict-mcp-config")
    };
    if (!sessionId.isEmpty())
        arguments << QStringLiteral("--resume") << sessionId;
    arguments << QStringLiteral("--allowedTools") << QStringLiteral("mcp__veditor");
    return arguments;
}

QString AiChatDock::buildShellCommandLine(const QString& program,
                                          const QStringList& args,
                                          QString* error)
{
    if (error)
        error->clear();

    const auto quote = [error](const QString& value) {
        if (containsUnsafeShellCharacter(value)) {
            if (error) {
                *error = QStringLiteral("cmd.exe で安全に扱えない値: ") + value;
            }
            return QString();
        }
        return QStringLiteral("\"") + value + QStringLiteral("\"");
    };

    if (program.isEmpty()) {
        if (error)
            *error = QStringLiteral("実行ファイルのパスが空です");
        return {};
    }

    QStringList tokens;
    tokens.reserve(args.size() + 1);
    const QString quotedProgram = quote(program);
    if (quotedProgram.isEmpty())
        return {};
    tokens.append(quotedProgram);
    for (const QString& arg : args) {
        const QString quotedArg = quote(arg);
        if (quotedArg.isEmpty())
            return {};
        tokens.append(quotedArg);
    }
    return tokens.join(QLatin1Char(' '));
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
    // Windows の QFile::setPermissions は NTFS ACL ではなく ReadOnly 属性を
    // 操作するだけだが、TempLocation (%LOCALAPPDATA%\\Temp) 自体はユーザー
    // 限定 ACL になっている。開いたファイルに対して書き込み前に設定する。
    const bool permissionsOk = config.setPermissions(
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!permissionsOk) {
        appendLog(QStringLiteral(
            "警告: MCP 設定ファイルの所有者専用権限を設定できませんでした。"),
                  QColor(Qt::gray));
    }
    const QByteArray data = buildMcpConfig(m_server->port(), m_server->token());
    const bool writeOk = config.write(data) == data.size();
    config.close();
    return writeOk;
}

void AiChatDock::sendPrompt()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        appendLog(QStringLiteral("実行中です。完了または停止を待ってください。"),
                  QColor(Qt::gray));
        return;
    }

    const QString prompt = m_input ? m_input->toPlainText().trimmed() : QString();
    if (prompt.isEmpty())
        return;

    const QSettings settings(QStringLiteral("VSimpleEditor"),
                             QStringLiteral("Preferences"));
    const bool codex = m_provider == Provider::Codex;
    const QString command = codex
        ? settings.value(QStringLiteral("aiChatCodexCommand"), QStringLiteral("codex")).toString()
        : settings.value(QStringLiteral("aiChatCommand"), QStringLiteral("claude")).toString();
    const CliCommand cli = resolveCliCommand(command);
    if (cli.program.isEmpty()) {
        const QString searched = cli.searched.isEmpty()
            ? QStringLiteral("(なし)")
            : cli.searched.join(QLatin1Char('\n'));
        appendLog(QStringLiteral(
                       "%1 が見つかりません\n探索:\n%2\n%3 を実行してください。")
                      .arg(command, searched,
                           codex ? QStringLiteral("npm i -g @openai/codex")
                                 : QStringLiteral("npm i -g @anthropic-ai/claude-code")),
                  QColor(Qt::red));
        if (m_statusLabel)
            m_statusLabel->setText(QStringLiteral("エラー"));
        return;
    }

    if (!m_server || !m_server->isRunning()) {
        if (m_mainWindow)
            m_mainWindow->toggleMcpServer(true);
    }
    if (!m_server || !m_server->isRunning()) {
        appendLog(QStringLiteral("MCP サーバを起動できませんでした。"),
                  QColor(Qt::red));
        if (m_statusLabel)
            m_statusLabel->setText(QStringLiteral("エラー"));
        return;
    }

    const QString workDir = m_mainWindow ? m_mainWindow->projectDirectory()
                                         : QString();
    const QString resolvedWorkDir = workDir.isEmpty() ? QDir::homePath() : workDir;
    if (!m_sessionWorkDir.isEmpty() && m_sessionWorkDir != resolvedWorkDir)
        m_sessionId.clear();
    m_sessionWorkDir = resolvedWorkDir;

    if (!writeMcpConfig()) {
        if (!m_configPath.isEmpty()) {
            QFile::remove(m_configPath);
            m_configPath.clear();
        }
        appendLog(QStringLiteral("MCP 設定ファイルを書き込めませんでした。"),
                  QColor(Qt::red));
        if (m_statusLabel)
            m_statusLabel->setText(QStringLiteral("エラー"));
        return;
    }

    bool timeoutOk = false;
    const int configuredTimeout = settings.value(
        QStringLiteral("aiChatTimeoutMs"), kDefaultAiChatTimeoutMs).toInt(&timeoutOk);
    m_watchdogTimeoutMs = timeoutOk && configuredTimeout > 0
        ? configuredTimeout : kDefaultAiChatTimeoutMs;

    // Codex は MCP 設定ファイルではなく -c 上書きで stdio ブリッジ (このエディタ自身) を渡す。
    const QStringList arguments = codex
        ? buildCodexArguments(m_server->port(), m_server->token(),
                              QDir::fromNativeSeparators(QCoreApplication::applicationFilePath()),
                              m_sessionId)
        : buildArguments(m_configPath, m_sessionId);
#ifdef Q_OS_WIN
    QString shellLine;
    if (cli.needsShell) {
        QString shellError;
        shellLine = buildShellCommandLine(cli.program, arguments, &shellError);
        if (shellLine.isEmpty()) {
            if (!m_configPath.isEmpty()) {
                QFile::remove(m_configPath);
                m_configPath.clear();
            }
            appendLog(QStringLiteral(
                           "一時ディレクトリまたはコマンドのパスに使えない文字が含まれています: ")
                          + shellError, QColor(Qt::red));
            if (m_statusLabel)
                m_statusLabel->setText(QStringLiteral("エラー"));
            return;
        }
    }
#endif

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

    m_process->setProcessEnvironment(childEnvironment(
        QProcessEnvironment::systemEnvironment()));
    m_process->setWorkingDirectory(resolvedWorkDir);

    appendLog(QStringLiteral("You: ") + prompt);
    m_input->clear();
    m_sendButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_stderrText.clear();
    m_gotResult = false;
    m_gotError = false;
    m_sessionResumeFailureReported = false;
    m_runTimer.start();
    if (m_statusLabel)
        m_statusLabel->setText(QStringLiteral("実行中 (0 秒)"));
    if (m_statusTimer)
        m_statusTimer->start();

    if (cli.needsShell) {
#ifdef Q_OS_WIN
        const QString comspec = qEnvironmentVariable("COMSPEC").isEmpty()
            ? QStringLiteral("cmd.exe") : qEnvironmentVariable("COMSPEC");
        m_process->setProgram(comspec);
        m_process->setArguments(QStringList());
        m_process->setNativeArguments(QStringLiteral("/d /s /c \"")
                                      + shellLine + QLatin1Char('"'));
        m_process->start();
#else
        m_process->start(cli.program, arguments);
#endif
    } else {
        m_process->start(cli.program, arguments);
    }
    armWatchdog();
    m_process->write(prompt.toUtf8());
    m_process->closeWriteChannel();
}

void AiChatDock::stopProcess()
{
    if (!m_process || m_process->state() == QProcess::NotRunning)
        return;
    if (m_watchdog)
        m_watchdog->stop();
#ifdef Q_OS_WIN
    const QStringList taskkillArguments{
        QStringLiteral("/PID"), QString::number(m_process->processId()),
        QStringLiteral("/T"), QStringLiteral("/F")
    };
    QProcess::startDetached(QStringLiteral("taskkill"), taskkillArguments);
#else
    m_process->kill();
#endif
}

void AiChatDock::readStandardOutput()
{
    if (!m_process)
        return;
    armWatchdog();
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
    armWatchdog();
    m_stderrBuffer += m_process->readAllStandardError();
    while (true) {
        const qsizetype newline = m_stderrBuffer.indexOf('\n');
        if (newline < 0)
            break;
        const QByteArray line = m_stderrBuffer.left(newline).trimmed();
        m_stderrBuffer.remove(0, newline + 1);
        const QString error = QString::fromUtf8(line).trimmed();
        if (error.isEmpty())
            continue;
        if (!m_stderrText.isEmpty())
            m_stderrText += QLatin1Char('\n');
        m_stderrText += error;
        appendLog(QStringLiteral("stderr: ") + error, QColor(Qt::gray));
    }
}

void AiChatDock::updateRunningStatus()
{
    if (!m_statusLabel || !m_process
        || m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_statusLabel->setText(QStringLiteral("実行中 (%1 秒)")
                               .arg(m_runTimer.elapsed() / 1000));
}

void AiChatDock::armWatchdog()
{
    if (!m_watchdog || !m_process
        || m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_watchdog->start(m_watchdogTimeoutMs);
}

void AiChatDock::processOutputLine(const QByteArray& line)
{
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) {
        const QString text = QString::fromUtf8(line).trimmed();
        if (!text.isEmpty()) {
            appendLog((m_provider == Provider::Codex ? QStringLiteral("codex: ")
                                                     : QStringLiteral("claude: ")) + text,
                      QColor(Qt::gray));
        }
        return;
    }
    const QJsonObject object = document.object();
    if (m_provider == Provider::Codex) {
        processCodexOutputLine(object);
        return;
    }
    const QString sessionId = object.value(QStringLiteral("session_id"))
        .toString();
    if (!sessionId.isEmpty())
        m_sessionId = sessionId;

    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("system")
        && object.value(QStringLiteral("subtype")).toString() == QStringLiteral("init")) {
        // 起動時イベントにモデル名が入る。接続状態行の「モデル」に反映する。
        const QString model = object.value(QStringLiteral("model")).toString();
        if (!model.isEmpty() && model != m_model) {
            m_model = model;
            refreshConnectionStatus();
        }
    }
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
        m_gotResult = true;
        const bool isError = object.value(QStringLiteral("is_error"))
            .toBool(false);
        const QString subtype = object.value(QStringLiteral("subtype"))
            .toString();
        const QString result = object.value(QStringLiteral("result"))
            .toString();
        if (isError) {
            m_gotError = true;
            QString message = QStringLiteral("claude エラー");
            if (!subtype.isEmpty())
                message += QStringLiteral(": ") + subtype;
            if (!result.isEmpty()) {
                message += QLatin1Char('\n');
                message += result;
            }
            appendLog(message, QColor(Qt::red));

            const bool resumeFailed = subtype
                    == QStringLiteral("error_during_execution")
                || m_stderrText.contains(QStringLiteral("No conversation found"));
            if (resumeFailed) {
                m_sessionId.clear();
                if (!m_sessionResumeFailureReported) {
                    appendLog(QStringLiteral(
                                  "セッションを再開できなかったので次回は新規に開始します。"),
                              QColor(Qt::red));
                    m_sessionResumeFailureReported = true;
                }
            }
        } else {
            appendLog(result);
        }
    } else if (type == QStringLiteral("rate_limit_event")) {
        const QString status = object.value(QStringLiteral("rate_limit_info"))
            .toObject().value(QStringLiteral("status")).toString();
        if (status != QStringLiteral("allowed")) {
            appendLog(QStringLiteral("claude のレート制限状態: ")
                          + (status.isEmpty() ? QStringLiteral("不明") : status),
                      QColor(Qt::gray));
        }
    }
}

void AiChatDock::processFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_watchdog)
        m_watchdog->stop();
    if (m_statusTimer)
        m_statusTimer->stop();
    readStandardOutput();
    readStandardError();
    if (!m_stdoutBuffer.trimmed().isEmpty()) {
        processOutputLine(m_stdoutBuffer.trimmed());
        m_stdoutBuffer.clear();
    }
    if (!m_stderrBuffer.trimmed().isEmpty()) {
        const QString error = QString::fromUtf8(m_stderrBuffer).trimmed();
        if (!error.isEmpty()) {
            if (!m_stderrText.isEmpty())
                m_stderrText += QLatin1Char('\n');
            m_stderrText += error;
            appendLog(QStringLiteral("stderr: ") + error, QColor(Qt::gray));
        }
        m_stderrBuffer.clear();
    }

    if (!m_sessionResumeFailureReported
        && m_stderrText.contains(QStringLiteral("No conversation found"))) {
        m_sessionId.clear();
        appendLog(QStringLiteral(
                      "セッションを再開できなかったので次回は新規に開始します。"),
                  QColor(Qt::red));
        m_sessionResumeFailureReported = true;
    }

    const bool abnormalExit = exitStatus == QProcess::CrashExit || exitCode != 0;
    if (abnormalExit) {
        QString message = QStringLiteral("%1 が終了コード %2 で終了しました")
            .arg(m_provider == Provider::Codex ? QStringLiteral("codex")
                                               : QStringLiteral("claude"))
            .arg(exitCode);
        if (!m_stderrText.isEmpty()) {
            message += QLatin1Char('\n');
            message += m_stderrText;
        }
        appendLog(message, QColor(Qt::red));
        m_gotError = true;
    }
    if (!m_gotResult) {
        appendLog(QStringLiteral("応答が返りませんでした。"), QColor(Qt::red));
        m_gotError = true;
    }
    if (m_sendButton)
        m_sendButton->setEnabled(true);
    if (m_stopButton)
        m_stopButton->setEnabled(false);
    if (m_statusLabel)
        m_statusLabel->setText(m_gotError ? QStringLiteral("エラー")
                                          : QStringLiteral("完了"));
    if (!m_configPath.isEmpty()) {
        QFile::remove(m_configPath);
        m_configPath.clear();
    }
}

void AiChatDock::processError(QProcess::ProcessError error)
{
    if (m_watchdog)
        m_watchdog->stop();
    if (m_statusTimer)
        m_statusTimer->stop();
    m_gotError = true;
    if (error == QProcess::FailedToStart) {
        const QString command = QSettings(QStringLiteral("VSimpleEditor"),
                                          QStringLiteral("Preferences"))
            .value(QStringLiteral("aiChatCommand"), QStringLiteral("claude"))
            .toString();
        appendLog(QStringLiteral("%1 が見つかりません。Claude Code をインストールし、PATH を通してください "
                               "(npm i -g @anthropic-ai/claude-code)。設定の aiChatCommand で別の CLI を指定できます。")
                      .arg(command), QColor(Qt::red));
    } else if (m_process) {
        appendLog(QStringLiteral("AI チャットのプロセスでエラーが発生しました: ")
                      + m_process->errorString(), QColor(Qt::red));
    }
    if (!m_configPath.isEmpty()) {
        QFile::remove(m_configPath);
        m_configPath.clear();
    }
    if (m_sendButton)
        m_sendButton->setEnabled(true);
    if (m_stopButton)
        m_stopButton->setEnabled(false);
    if (m_statusLabel)
        m_statusLabel->setText(QStringLiteral("エラー"));
}
