#include "PluginBrowserDialog.h"
#include "PluginManifest.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

PluginBrowserDialog::PluginBrowserDialog(const QString& pluginRootDir, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("プラグインブラウザ"));
    resize(800, 500);

    // ---- top row ----
    auto* topLayout = new QHBoxLayout;
    auto* dirLabel  = new QLabel(tr("プラグインフォルダ:"), this);
    m_dirEdit       = new QLineEdit(pluginRootDir, this);
    m_browseBtn     = new QPushButton(tr("参照..."), this);
    m_rescanBtn     = new QPushButton(tr("再スキャン"), this);
    topLayout->addWidget(dirLabel);
    topLayout->addWidget(m_dirEdit, 1);
    topLayout->addWidget(m_browseBtn);
    topLayout->addWidget(m_rescanBtn);

    // ---- centre: splitter tree | detail ----
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Name"), tr("Vendor"), tr("Version"), tr("Category")});
    m_tree->setRootIsDecorated(false);
    m_tree->setSortingEnabled(true);

    m_detailEdit = new QTextEdit(this);
    m_detailEdit->setReadOnly(true);
    m_detailEdit->setPlaceholderText(tr("プラグインを選択すると manifest JSON が表示されます"));

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_tree);
    splitter->addWidget(m_detailEdit);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);

    // ---- bottom row ----
    m_runBtn = new QPushButton(tr("プラグイン実行"), this);
    m_runBtn->setEnabled(false);
    m_runBtn->setToolTip(tr("次の Sprint で対応"));

    m_closeBtn = new QPushButton(tr("閉じる"), this);

    auto* bottomLayout = new QHBoxLayout;
    bottomLayout->addWidget(m_runBtn);
    bottomLayout->addStretch();
    bottomLayout->addWidget(m_closeBtn);

    // ---- root layout ----
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->addLayout(topLayout);
    rootLayout->addWidget(splitter, 1);
    rootLayout->addLayout(bottomLayout);

    // ---- connections ----
    connect(m_browseBtn,  &QPushButton::clicked,
            this, &PluginBrowserDialog::onBrowseDir);
    connect(m_rescanBtn,  &QPushButton::clicked,
            this, &PluginBrowserDialog::onRescan);
    connect(m_closeBtn,   &QPushButton::clicked,
            this, &QDialog::close);
    connect(m_tree, &QTreeWidget::itemSelectionChanged,
            this, &PluginBrowserDialog::onTreeSelectionChanged);

    // ---- initial scan ----
    rescan();
}

// ---------------------------------------------------------------------------
QString PluginBrowserDialog::currentPluginManifestPath() const
{
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item)
        return {};
    return item->data(0, Qt::UserRole).toString();
}

// ---------------------------------------------------------------------------
void PluginBrowserDialog::onBrowseDir()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("プラグインフォルダ"), m_dirEdit->text());
    if (!dir.isEmpty()) {
        m_dirEdit->setText(dir);
        rescan();
    }
}

void PluginBrowserDialog::onRescan()
{
    rescan();
}

// ---------------------------------------------------------------------------
void PluginBrowserDialog::rescan()
{
    m_plugins = PluginManifestScanner::scanFolder(m_dirEdit->text());
    populateTree();
}

void PluginBrowserDialog::populateTree()
{
    m_tree->clear();
    m_detailEdit->clear();

    for (const PluginInfo& info : m_plugins) {
        auto* item = new QTreeWidgetItem(m_tree,
            {info.name, info.vendor, info.version, info.category});
        item->setData(0, Qt::UserRole, info.manifestPath);
    }

    m_tree->resizeColumnToContents(0);
    m_tree->resizeColumnToContents(1);
    m_tree->resizeColumnToContents(2);
    m_tree->resizeColumnToContents(3);
}

// ---------------------------------------------------------------------------
void PluginBrowserDialog::onTreeSelectionChanged()
{
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item) {
        m_detailEdit->clear();
        return;
    }

    const QString path = item->data(0, Qt::UserRole).toString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_detailEdit->clear();
        return;
    }

    QByteArray raw = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isNull()) {
        m_detailEdit->setPlainText(QString::fromUtf8(raw));
        return;
    }

    m_detailEdit->setPlainText(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
}
