// src/MarkerPanelDock.cpp
// MK-2: マーカー パネル ドックの実装。MarkerPanelDock.h のコントラクトを実装する。

#include "MarkerPanelDock.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QBrush>
#include <QColor>
#include <QSignalBlocker>

namespace {
// 時刻列に埋め込むマーカー id / 生のタイムライン位置 (マイクロ秒) 用の
// UserRole。行の表示順が変わっても id・位置を表示文字列の再パースなしで
// 正確に取り出せるよう、アイテム自身に持たせる。
constexpr int kMarkerIdRole   = Qt::UserRole + 1;
constexpr int kMarkerTimeRole = Qt::UserRole + 2;

// 列定義。enum と見出しを 1 箇所にまとめておく。
enum Column {
    ColTime = 0,
    ColLabel,
    ColDuration,
    ColColor,
    ColNote,
    ColumnCount
};
}  // namespace

MarkerPanelDock::MarkerPanelDock(QWidget *parent)
    : QDockWidget(tr("マーカー"), parent)
{
    setObjectName(QStringLiteral("markerPanelDock"));

    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_table = new QTableWidget(root);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels(
        {tr("時刻"), tr("ラベル"), tr("期間"), tr("色"), tr("ノート")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked
                             | QAbstractItemView::EditKeyPressed);
    layout->addWidget(m_table);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_delBtn = new QPushButton(tr("削除"), root);
    btnRow->addWidget(m_delBtn);
    layout->addLayout(btnRow);

    setWidget(root);

    connect(m_table, &QTableWidget::itemDoubleClicked,
            this, &MarkerPanelDock::onItemDoubleClicked);
    connect(m_table, &QTableWidget::itemChanged,
            this, &MarkerPanelDock::onItemChanged);
    connect(m_delBtn, &QPushButton::clicked,
            this, &MarkerPanelDock::onDeleteClicked);
}

QString MarkerPanelDock::formatTime(qint64 timelineUs)
{
    const qint64 us = qMax<qint64>(0, timelineUs);
    const qint64 totalMs = us / 1000;
    const qint64 minutes = totalMs / 60000;
    const qint64 seconds = (totalMs / 1000) % 60;
    const qint64 millis  = totalMs % 1000;
    return QStringLiteral("%1:%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(millis,  3, 10, QLatin1Char('0'));
}

QString MarkerPanelDock::formatDuration(qint64 durationUs)
{
    if (durationUs <= 0)
        return QStringLiteral("—");  // 点マーカー
    return formatTime(durationUs);
}

void MarkerPanelDock::setMarkers(const QVector<Marker> &markers)
{
    // 再構築中の itemChanged を無視する (ノート編集シグナルを誤発火させない)。
    QSignalBlocker tableBlocker(m_table);
    m_populating = true;
    m_table->clearContents();
    m_table->setRowCount(markers.size());

    for (int row = 0; row < markers.size(); ++row) {
        const Marker &m = markers[row];

        auto *timeItem = new QTableWidgetItem(formatTime(m.timelineUs));
        timeItem->setData(kMarkerIdRole, m.id);
        timeItem->setData(kMarkerTimeRole,
                          static_cast<qlonglong>(m.timelineUs));
        timeItem->setFlags(timeItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, ColTime, timeItem);

        auto *labelItem = new QTableWidgetItem(m.label);
        labelItem->setFlags(labelItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, ColLabel, labelItem);

        auto *durItem = new QTableWidgetItem(formatDuration(m.durationUs));
        durItem->setFlags(durItem->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, ColDuration, durItem);

        auto *colorItem = new QTableWidgetItem(
            m.color.isValid() ? m.color.name(QColor::HexRgb)
                              : QStringLiteral("#ff5050"));
        colorItem->setFlags(colorItem->flags() & ~Qt::ItemIsEditable);
        if (m.color.isValid())
            colorItem->setForeground(QBrush(m.color));
        m_table->setItem(row, ColColor, colorItem);

        // ノート列だけ編集可能。
        auto *noteItem = new QTableWidgetItem(m.note);
        m_table->setItem(row, ColNote, noteItem);
    }

    m_populating = false;
}

int MarkerPanelDock::markerIdForRow(int row) const
{
    if (row < 0 || row >= m_table->rowCount())
        return -1;
    QTableWidgetItem *timeItem = m_table->item(row, ColTime);
    if (!timeItem)
        return -1;
    bool ok = false;
    const int id = timeItem->data(kMarkerIdRole).toInt(&ok);
    return ok ? id : -1;
}

void MarkerPanelDock::onItemDoubleClicked(QTableWidgetItem *item)
{
    if (!item)
        return;
    // ノート列のダブルクリックは編集トリガなのでジャンプしない。
    if (item->column() == ColNote)
        return;
    QTableWidgetItem *timeItem = m_table->item(item->row(), ColTime);
    if (!timeItem)
        return;
    // 表示文字列の再パースではなく、保持した生のマイクロ秒値から復元する
    // (mm:ss.mmm は ms 丸めのため往復で誤差が出るのを避ける)。
    const qint64 us =
        static_cast<qint64>(timeItem->data(kMarkerTimeRole).toLongLong());
    emit jumpToMarker(us);
}

void MarkerPanelDock::onItemChanged(QTableWidgetItem *item)
{
    if (m_populating || !item)
        return;
    if (item->column() != ColNote)
        return;
    const int id = markerIdForRow(item->row());
    if (id < 0)
        return;
    emit markerNoteEdited(id, item->text());
}

void MarkerPanelDock::onDeleteClicked()
{
    const int row = m_table->currentRow();
    const int id = markerIdForRow(row);
    if (id < 0)
        return;
    emit markerDeleteRequested(id);
}
