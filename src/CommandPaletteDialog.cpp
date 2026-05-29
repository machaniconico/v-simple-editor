#include "CommandPaletteDialog.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

CommandPaletteDialog::CommandPaletteDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("コマンドパレット"));

    auto* layout = new QVBoxLayout(this);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("機能を検索 (名前や操作内容で)..."));
    m_searchEdit->setClearButtonEnabled(true);
    layout->addWidget(m_searchEdit);

    m_listWidget = new QListWidget(this);
    layout->addWidget(m_listWidget);

    m_searchEdit->installEventFilter(this);

    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &CommandPaletteDialog::onSearchTextChanged);
    connect(m_searchEdit, &QLineEdit::returnPressed,
            this, &CommandPaletteDialog::acceptCurrentItem);
    connect(m_listWidget, &QListWidget::itemActivated,
            this, [this](QListWidgetItem*) { acceptCurrentItem(); });
    connect(m_listWidget, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { acceptCurrentItem(); });

    resize(520, 420);
}

void CommandPaletteDialog::setCommands(const QVector<cmdsearch::CommandEntry>& commands)
{
    m_commands = commands;

    QVector<int> order;
    order.reserve(m_commands.size());
    for (int i = 0; i < m_commands.size(); ++i) {
        order.append(i);
    }
    rebuildList(order);

    m_searchEdit->clear();
    m_searchEdit->setFocus();
}

void CommandPaletteDialog::rebuildList(const QVector<int>& order)
{
    m_listWidget->clear();
    for (int idx : order) {
        if (idx < 0 || idx >= m_commands.size()) {
            continue;
        }
        const cmdsearch::CommandEntry& entry = m_commands.at(idx);
        QString label = entry.title;
        if (!entry.keywords.isEmpty()) {
            label += QStringLiteral("   (") + entry.keywords + QStringLiteral(")");
        }
        auto* item = new QListWidgetItem(label, m_listWidget);
        item->setData(Qt::UserRole, entry.id);
    }
    if (m_listWidget->count() > 0) {
        m_listWidget->setCurrentRow(0);
    }
}

void CommandPaletteDialog::onSearchTextChanged(const QString& text)
{
    const QVector<int> order = cmdsearch::rankMatches(m_commands, text);
    rebuildList(order);
}

void CommandPaletteDialog::acceptCurrentItem()
{
    // ダブルクリックは itemActivated と itemDoubleClicked の両方を発火しうるため、
    // 二重 emit / 二重 accept を防ぐ re-entrancy ガード。
    if (m_accepting) {
        return;
    }

    QListWidgetItem* item = m_listWidget->currentItem();
    if (!item && m_listWidget->count() > 0) {
        item = m_listWidget->item(0);
    }
    if (!item) {
        return;
    }

    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) {
        return;
    }

    m_accepting = true;
    emit commandTriggered(id);
    accept();
}

bool CommandPaletteDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_searchEdit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down) {
            QApplication::sendEvent(m_listWidget, keyEvent);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}
