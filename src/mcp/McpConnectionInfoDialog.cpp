#include "McpConnectionInfoDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString tomlString(const QString& value)
{
    QString escaped = value;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

QWidget *snippetRow(const QString& snippet, const QString& buttonText,
                    QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *text = new QPlainTextEdit(row);
    text->setReadOnly(true);
    text->setPlainText(snippet);
    text->setMinimumHeight(70);
    layout->addWidget(text, 1);

    auto *copy = new QPushButton(buttonText, row);
    copy->setToolTip(QStringLiteral("この設定をクリップボードへコピーします。"));
    QObject::connect(copy, &QPushButton::clicked, row, [text]() {
        if (QClipboard *clipboard = QApplication::clipboard())
            clipboard->setText(text->toPlainText());
    });
    layout->addWidget(copy);
    return row;
}

} // namespace

McpConnectionInfoDialog::McpConnectionInfoDialog(const QString& endpoint,
                                                 const QString& token,
                                                 QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("MCP サーバ接続情報"));
    resize(720, 520);

    auto *layout = new QVBoxLayout(this);

    auto *endpointLabel = new QLabel(QStringLiteral("エンドポイント"), this);
    layout->addWidget(endpointLabel);
    auto *endpointEdit = new QLineEdit(endpoint, this);
    endpointEdit->setReadOnly(true);
    layout->addWidget(endpointEdit);

    auto *tokenLabel = new QLabel(QStringLiteral("トークン"), this);
    layout->addWidget(tokenLabel);
    auto *tokenEdit = new QLineEdit(token, this);
    tokenEdit->setReadOnly(true);
    tokenEdit->setEchoMode(QLineEdit::Normal);
    layout->addWidget(tokenEdit);

    const QString claudeSnippet = QString::fromUtf8(
        QJsonDocument(QJsonObject{
            {QStringLiteral("mcpServers"), QJsonObject{
                {QStringLiteral("veditor"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("http")},
                    {QStringLiteral("url"), endpoint},
                    {QStringLiteral("headers"), QJsonObject{
                        {QStringLiteral("Authorization"),
                         QStringLiteral("Bearer ") + token}
                    }}
                }}
            }}
        }).toJson(QJsonDocument::Compact));

    layout->addWidget(new QLabel(QStringLiteral("Claude Code 用"), this));
    layout->addWidget(snippetRow(claudeSnippet, QStringLiteral("コピー"), this));
    layout->addWidget(new QLabel(
        QStringLiteral("使い方: claude --mcp-config veditor-mcp.json --strict-mcp-config"),
        this));

    const QString executable = QDir::toNativeSeparators(
        QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath());
    const QString codexSnippet = QStringLiteral(
        "[mcp_servers.veditor]\n"
        "command = %1\n"
        "args = [\"--mcp-stdio\", \"--port\", \"%2\"]\n"
        "env = { VEDITOR_MCP_TOKEN = %3 }\n")
        .arg(tomlString(executable),
             QString::number(QUrl(endpoint).port()),
             tomlString(token));

    layout->addWidget(new QLabel(QStringLiteral("Codex CLI 用"), this));
    layout->addWidget(snippetRow(codexSnippet, QStringLiteral("コピー"), this));

    auto *warning = new QLabel(
        QStringLiteral("このサーバに接続した LLM は、確認なしにタイムラインを編集します。\n"
                       "変更は Ctrl+Z で戻せます。トークンを他人に渡さないでください。"),
        this);
    warning->setStyleSheet(QStringLiteral("color: #d32f2f;"));
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
