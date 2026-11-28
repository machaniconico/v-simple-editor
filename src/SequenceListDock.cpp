#include "SequenceListDock.h"
#include "SequenceSettingsDialog.h"
#include "Timeline.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

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
        QString rawName = item->data(Qt::UserRole + 1).toString();
        if (rawName.isEmpty()) rawName = item->text();
        bool hasCustom = false;
        for (const auto &seq : m_timeline->sequenceList()) {
            if (seq.id == id) {
                rawName = seq.name;
                hasCustom = seq.hasCustomSettings();
                break;
            }
        }
        QMenu menu(this);
        auto *renameAct = menu.addAction(QStringLiteral("名前を変更…"));
        auto *settingsAct = menu.addAction(QStringLiteral("シーケンス設定（解像度 / FPS）…"));
        QAction *resetAct = nullptr;
        if (hasCustom)
            resetAct = menu.addAction(QStringLiteral("プロジェクト設定に戻す（継承）"));
        QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
        if (!chosen || !m_timeline) return;
        if (chosen == renameAct) {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("名前を変更"),
                QStringLiteral("シーケンス名"), QLineEdit::Normal, rawName, &ok);
            if (ok && m_timeline) m_timeline->renameSequence(id, name);
        } else if (chosen == settingsAct) {
            int w = 1920, h = 1080;
            double fps = 30.0;
            m_timeline->effectiveSequenceSettings(id, w, h, fps);
            ProjectConfig initial;
            initial.name = rawName;
            initial.width = w;
            initial.height = h;
            initial.fps = qMax(1, static_cast<int>(std::lround(fps)));
            initial.explicitOutputResolution = true;
            SequenceSettingsDialog dialog(initial, this);
            dialog.setWindowTitle(QStringLiteral("シーケンス設定"));
            if (dialog.exec() == QDialog::Accepted && m_timeline) {
                const ProjectConfig cfg = dialog.config();
                if (!cfg.name.trimmed().isEmpty() && cfg.name.trimmed() != rawName)
                    m_timeline->renameSequence(id, cfg.name.trimmed());
                m_timeline->setSequenceSettings(id, cfg.width, cfg.height, static_cast<double>(cfg.fps));
            }
        } else if (resetAct && chosen == resetAct) {
            m_timeline->setSequenceSettings(id, 0, 0, 0.0);
        }
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
        const QString label = sequence.hasCustomSettings()
            ? QStringLiteral("%1 (%2x%3 %4fps)")
                  .arg(sequence.name)
                  .arg(sequence.width)
                  .arg(sequence.height)
                  .arg(sequence.fps, 0, 'g', 4)
            : sequence.name;
        auto *item = new QListWidgetItem(label, m_list);
        item->setData(Qt::UserRole, sequence.id);
        item->setData(Qt::UserRole + 1, sequence.name);
        item->setToolTip(sequence.hasCustomSettings()
            ? QStringLiteral("解像度: %1x%2 / %3 fps（シーケンス個別設定）")
                  .arg(sequence.width).arg(sequence.height).arg(sequence.fps, 0, 'g', 4)
            : QStringLiteral("プロジェクト設定を継承"));
        QFont font = item->font();
        font.setBold(sequence.id == active);
        item->setFont(font);
        if (sequence.id == active) m_list->setCurrentItem(item);
    }
}
