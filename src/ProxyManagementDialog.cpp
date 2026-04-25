#include "ProxyManagementDialog.h"
#include "ProxyManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QFileInfo>
#include <QDialogButtonBox>
#include <QMessageBox>

ProxyManagementDialog::ProxyManagementDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("プロキシ管理"));
    resize(800, 500);

    auto *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("クリップ名"),
        QStringLiteral("元ファイル"),
        QStringLiteral("Proxy パス"),
        QStringLiteral("サイズ"),
        QStringLiteral("ステータス"),
        QStringLiteral("削除"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    layout->addWidget(m_table);

    auto *bottomRow = new QHBoxLayout();
    m_totalSizeLabel = new QLabel(this);
    bottomRow->addWidget(m_totalSizeLabel);
    bottomRow->addStretch(1);
    auto *deleteAllBtn = new QPushButton(QStringLiteral("全削除"), this);
    bottomRow->addWidget(deleteAllBtn);
    layout->addLayout(bottomRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(buttons);

    connect(deleteAllBtn, &QPushButton::clicked, this, [this]() {
        const auto reply = QMessageBox::question(
            this,
            QStringLiteral("プロキシ管理"),
            QStringLiteral("全てのプロキシを削除しますか? この操作は取り消せません。"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;
        ProxyManager::instance().deleteAllProxies();
        refreshTable();
    });

    refreshTable();
}

void ProxyManagementDialog::refreshTable()
{
    auto &pm = ProxyManager::instance();
    // Snapshot keys before mutating the table — deleteProxy on a row click
    // will invalidate any iterator into m_entries.
    QStringList keys = pm.entries().keys();
    keys.sort(Qt::CaseInsensitive);

    m_table->setRowCount(keys.size());
    qint64 totalReadyBytes = 0;

    for (int row = 0; row < keys.size(); ++row) {
        const QString origPath = keys.at(row);
        const ProxyEntry entry = pm.entries().value(origPath);
        const QFileInfo origInfo(origPath);
        const QFileInfo proxyInfo(entry.proxyPath);

        QString statusStr;
        switch (entry.status) {
            case ProxyStatus::None:       statusStr = QStringLiteral("None"); break;
            case ProxyStatus::Generating: statusStr = QStringLiteral("Generating"); break;
            case ProxyStatus::Ready:      statusStr = QStringLiteral("Ready"); break;
            case ProxyStatus::Error:      statusStr = QStringLiteral("Error"); break;
        }

        const qint64 sz = (entry.status == ProxyStatus::Ready && proxyInfo.exists())
                          ? proxyInfo.size() : 0;
        if (entry.status == ProxyStatus::Ready)
            totalReadyBytes += sz;
        const QString sizeStr = (sz > 0)
            ? QString::number(sz / (1024.0 * 1024.0), 'f', 1) + " MB"
            : QStringLiteral("-");

        m_table->setItem(row, 0, new QTableWidgetItem(origInfo.fileName()));
        m_table->setItem(row, 1, new QTableWidgetItem(origPath));
        m_table->setItem(row, 2, new QTableWidgetItem(entry.proxyPath));
        m_table->setItem(row, 3, new QTableWidgetItem(sizeStr));
        m_table->setItem(row, 4, new QTableWidgetItem(statusStr));

        auto *delBtn = new QPushButton(QStringLiteral("削除"), m_table);
        // Capture origPath by value so the lambda survives table rebuild.
        connect(delBtn, &QPushButton::clicked, this, [this, origPath]() {
            ProxyManager::instance().deleteProxy(origPath);
            refreshTable();
        });
        m_table->setCellWidget(row, 5, delBtn);
    }

    m_table->resizeColumnsToContents();
    m_totalSizeLabel->setText(QStringLiteral("合計サイズ: %1 MB")
        .arg(QString::number(totalReadyBytes / (1024.0 * 1024.0), 'f', 1)));
}
