#include "MediaRelinkDialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kOriginalPathRole = Qt::UserRole;
}

MediaRelinkDialog::MediaRelinkDialog(const QStringList &missingPaths,
                                     QWidget *parent)
    : QDialog(parent)
    , m_missingPaths(missingPaths)
{
    setObjectName(QStringLiteral("mediaRelinkDialog"));
    setWindowTitle(QStringLiteral("オフラインメディアを再リンク"));
    setModal(true);
    resize(760, 420);

    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(
        QStringLiteral("参照先が見つからないメディアがあります。新しい場所を指定するか、オフラインのままプロジェクトを開けます。"),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);

    m_files = new QTreeWidget(this);
    m_files->setObjectName(QStringLiteral("missingMediaList"));
    m_files->setColumnCount(2);
    m_files->setHeaderLabels({QStringLiteral("見つからないファイル"),
                              QStringLiteral("再リンク先")});
    m_files->setSelectionMode(QAbstractItemView::SingleSelection);
    m_files->setRootIsDecorated(false);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_files->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (const QString &path : m_missingPaths) {
        auto *item = new QTreeWidgetItem(m_files);
        item->setText(0, QDir::toNativeSeparators(path));
        item->setText(1, QStringLiteral("未指定"));
        item->setData(0, kOriginalPathRole, path);
        item->setToolTip(0, QDir::toNativeSeparators(path));
    }
    if (m_files->topLevelItemCount() > 0)
        m_files->setCurrentItem(m_files->topLevelItem(0));
    layout->addWidget(m_files, 1);

    auto *actions = new QHBoxLayout;
    m_searchFolderButton = new QPushButton(
        QStringLiteral("フォルダを指定して検索"), this);
    m_searchFolderButton->setObjectName(QStringLiteral("searchRelinkFolderButton"));
    m_chooseFileButton = new QPushButton(QStringLiteral("個別に選択"), this);
    m_chooseFileButton->setObjectName(QStringLiteral("chooseRelinkFileButton"));
    actions->addWidget(m_searchFolderButton);
    actions->addWidget(m_chooseFileButton);
    actions->addStretch();
    layout->addLayout(actions);

    auto *buttons = new QDialogButtonBox(this);
    m_applyButton = buttons->addButton(QStringLiteral("再リンクを適用"),
                                       QDialogButtonBox::AcceptRole);
    m_applyButton->setObjectName(QStringLiteral("applyRelinkButton"));
    auto *offlineButton = buttons->addButton(
        QStringLiteral("オフラインのまま開く"), QDialogButtonBox::RejectRole);
    offlineButton->setObjectName(QStringLiteral("openOfflineButton"));
    layout->addWidget(buttons);

    connect(m_searchFolderButton, &QPushButton::clicked,
            this, [this]() { searchFolder(); });
    connect(m_chooseFileButton, &QPushButton::clicked,
            this, [this]() { chooseIndividualFile(); });
    connect(m_files, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *, int) { chooseIndividualFile(); });
    connect(m_applyButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(offlineButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_files, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
                m_chooseFileButton->setEnabled(current != nullptr);
            });

    setTabOrder(m_files, m_searchFolderButton);
    setTabOrder(m_searchFolderButton, m_chooseFileButton);
    setTabOrder(m_chooseFileButton, m_applyButton);
    setTabOrder(m_applyButton, offlineButton);
    updateApplyState();
}

void MediaRelinkDialog::searchFolder()
{
    const QString root = QFileDialog::getExistingDirectory(
        this, QStringLiteral("再リンク先のフォルダを選択"));
    if (root.isEmpty())
        return;

    QHash<QString, QStringList> byFileName;
    QDirIterator iterator(root, QDir::Files | QDir::Readable,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        byFileName[QFileInfo(path).fileName().toCaseFolded()].append(path);
    }

    int matched = 0;
    for (int row = 0; row < m_files->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_files->topLevelItem(row);
        const QString original = item->data(0, kOriginalPathRole).toString();
        const QStringList candidates =
            byFileName.value(QFileInfo(original).fileName().toCaseFolded());
        if (candidates.isEmpty())
            continue;

        QString selected = candidates.first();
        if (candidates.size() > 1) {
            QStringList displayCandidates;
            for (const QString &candidate : candidates)
                displayCandidates.append(QDir::toNativeSeparators(candidate));
            bool ok = false;
            const QString displaySelection = QInputDialog::getItem(
                this, QStringLiteral("再リンク候補を選択"),
                QFileInfo(original).fileName(), displayCandidates, 0, false, &ok);
            if (!ok)
                continue;
            const int selectedIndex = displayCandidates.indexOf(displaySelection);
            if (selectedIndex < 0)
                continue;
            selected = candidates.at(selectedIndex);
        }
        setCandidate(item, selected);
        ++matched;
    }

    if (matched == 0) {
        QMessageBox::information(
            this, QStringLiteral("再リンク候補なし"),
            QStringLiteral("選択したフォルダ以下に、同じファイル名の候補は見つかりませんでした。"));
    }
}

void MediaRelinkDialog::chooseIndividualFile()
{
    QTreeWidgetItem *item = m_files->currentItem();
    if (!item)
        return;
    const QString original = item->data(0, kOriginalPathRole).toString();
    const QString candidate = QFileDialog::getOpenFileName(
        this, QStringLiteral("%1 の再リンク先を選択")
                  .arg(QFileInfo(original).fileName()),
        QFileInfo(original).absolutePath(), QStringLiteral("すべてのファイル (*)"));
    if (!candidate.isEmpty())
        setCandidate(item, candidate);
}

void MediaRelinkDialog::setCandidate(QTreeWidgetItem *item,
                                     const QString &candidate)
{
    if (!item)
        return;
    const QFileInfo info(candidate);
    if (!info.exists() || !info.isFile())
        return;
    const QString original = item->data(0, kOriginalPathRole).toString();
    const QString absoluteCandidate = info.absoluteFilePath();
    m_mapping.insert(original, absoluteCandidate);
    item->setText(1, QDir::toNativeSeparators(absoluteCandidate));
    item->setToolTip(1, QDir::toNativeSeparators(absoluteCandidate));
    updateApplyState();
}

void MediaRelinkDialog::updateApplyState()
{
    m_applyButton->setEnabled(!m_mapping.isEmpty());
    m_chooseFileButton->setEnabled(m_files->currentItem() != nullptr);
}
