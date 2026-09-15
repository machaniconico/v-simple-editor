#include "ProjectDiffDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QRegularExpression>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
QString label(projdiff::Change::Type type)
{
    switch (type) {
    case projdiff::Change::Added: return QStringLiteral("追加");
    case projdiff::Change::Removed: return QStringLiteral("削除");
    case projdiff::Change::Moved: return QStringLiteral("移動");
    case projdiff::Change::Trimmed: return QStringLiteral("トリム");
    case projdiff::Change::PropertyChanged: return QStringLiteral("プロパティ変更");
    case projdiff::Change::EffectsChanged: return QStringLiteral("効果変更");
    case projdiff::Change::TransitionChanged: return QStringLiteral("トランジション変更");
    case projdiff::Change::TrackFlagChanged: return QStringLiteral("トラック設定変更");
    }
    return {};
}
}

ProjectDiffDialog::ProjectDiffDialog(const QVector<projdiff::Change> &changes, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("保存版と比較"));
    resize(980, 520);
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(changes.isEmpty()
        ? QStringLiteral("変更はありません。")
        : QStringLiteral("%1 件の変更。行をダブルクリックすると現在のクリップへ移動します。削除済みのクリップには移動できません。")
              .arg(changes.size()), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *table = new QTableWidget(int(changes.size()), 4, this);
    table->setObjectName(QStringLiteral("projectDiffTable"));
    table->setHorizontalHeaderLabels({QStringLiteral("種別"), QStringLiteral("場所"),
                                     QStringLiteral("変更前"), QStringLiteral("変更後")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setWordWrap(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (int row = 0; row < changes.size(); ++row) {
        const auto &change = changes[row];
        const QStringList cells{label(change.type), change.path, change.before, change.after};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells[column]);
            item->setToolTip(cells[column]);
            table->setItem(row, column, item);
        }
    }
    connect(table, &QTableWidget::cellDoubleClicked, this, [this, changes](int row, int) {
        if (row < 0 || row >= changes.size() || changes[row].type == projdiff::Change::Removed) return;
        static const QRegularExpression pattern(QStringLiteral("^(video|audio)\\[(\\d+)\\]\\.clips\\[(\\d+)\\]"));
        const auto match = pattern.match(changes[row].path);
        if (match.hasMatch())
            emit clipActivated(match.captured(1) == QStringLiteral("audio"),
                               match.captured(2).toInt(), match.captured(3).toInt());
    });
    layout->addWidget(table);
    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(QStringLiteral("閉じる"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
