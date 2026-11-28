#include "SequenceListDock.h"
#include "Timeline.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

SequenceListDock::SequenceListDock(Timeline *timeline, QWidget *parent)
    : QDockWidget(QStringLiteral("シーケンス一覧"), parent), m_timeline(timeline)
{
    setObjectName(QStringLiteral("SequenceListDock"));
    auto *body = new QWidget(this);
    auto *layout = new QVBoxLayout(body);
    m_list = new QListWidget(body);
    m_list->setObjectName(QStringLiteral("sequenceList"));
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_list);
    auto *create = new QPushButton(QStringLiteral("新規シーケンス"), body);
    create->setObjectName(QStringLiteral("createSequence"));
    layout->addWidget(create);
    setWidget(body);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (m_timeline) m_timeline->setActiveSequence(item->data(Qt::UserRole).toString());
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto *item = m_list->itemAt(pos);
        if (!item || !m_timeline) return;
        const QString id = item->data(Qt::UserRole).toString();
        const QString oldName = item->text();
        QMenu menu(this);
        auto *rename = menu.addAction(QStringLiteral("名前を変更…"));
        if (menu.exec(m_list->viewport()->mapToGlobal(pos)) != rename) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("名前を変更"),
            QStringLiteral("シーケンス名"), QLineEdit::Normal, oldName, &ok);
        if (ok && m_timeline) m_timeline->renameSequence(id, name);
    });
    connect(create, &QPushButton::clicked, this, [this] {
        if (m_timeline) m_timeline->createSequence(QStringLiteral("新規シーケンス"));
    });
    if (timeline) connect(timeline, &Timeline::sequencesChanged, this, &SequenceListDock::refresh);
    refresh();
}

void SequenceListDock::refresh()
{
    m_list->clear();
    if (!m_timeline) return;
    const QString active = m_timeline->activeSequenceId().isEmpty()
        ? QStringLiteral("main") : m_timeline->activeSequenceId();
    for (const auto &sequence : m_timeline->sequenceList()) {
        auto *item = new QListWidgetItem(sequence.name, m_list);
        item->setData(Qt::UserRole, sequence.id);
        QFont font = item->font();
        font.setBold(sequence.id == active);
        item->setFont(font);
        if (sequence.id == active) m_list->setCurrentItem(item);
    }
}
