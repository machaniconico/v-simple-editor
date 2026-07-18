#include "TextBasedEditDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QBrush>
#include <QColor>

namespace {

// ms を mm:ss.mmm 形式に整形する (タイムライン時刻表示用)。
QString formatMs(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 totalSec = ms / 1000;
    const qint64 millis   = ms % 1000;
    return QStringLiteral("%1:%2.%3")
        .arg(totalSec / 60, 2, 10, QLatin1Char('0'))
        .arg(totalSec % 60, 2, 10, QLatin1Char('0'))
        .arg(millis, 3, 10, QLatin1Char('0'));
}

} // namespace

TextBasedEditDialog::TextBasedEditDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("テキストベース編集"));
    resize(560, 480);

    auto* layout = new QVBoxLayout(this);

    auto* help = new QLabel(
        QStringLiteral("文字起こし結果を読み上げ順に一覧表示します。削除したい行にチェックを入れ、"
                       "「タイムラインに適用 (リップル削除)」を押すと、その区間がタイムラインから"
                       "詰めて削除されます。"),
        this);
    help->setWordWrap(true);
    layout->addWidget(help);

    // 検索行
    auto* searchRow = new QHBoxLayout();
    searchRow->addWidget(new QLabel(QStringLiteral("検索:"), this));
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("テキストの一部を入力するとヒット行を強調します"));
    searchRow->addWidget(m_searchEdit, 1);
    layout->addLayout(searchRow);

    // トランスクリプト一覧 (各 clip = 1 行、チェックで削除対象)
    m_listWidget = new QListWidget(this);
    layout->addWidget(m_listWidget, 1);

    // サマリ
    m_summaryLabel = new QLabel(this);
    layout->addWidget(m_summaryLabel);

    // ボタン: 適用 (Ok 位置) + 閉じる (Cancel)
    m_buttonBox = new QDialogButtonBox(this);
    m_applyButton = m_buttonBox->addButton(
        QStringLiteral("タイムラインに適用 (リップル削除)"), QDialogButtonBox::AcceptRole);
    m_buttonBox->addButton(QStringLiteral("閉じる"), QDialogButtonBox::RejectRole);
    layout->addWidget(m_buttonBox);

    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &TextBasedEditDialog::onSearchTextChanged);
    connect(m_listWidget, &QListWidget::itemChanged,
            this, &TextBasedEditDialog::onItemChanged);
    connect(m_applyButton, &QPushButton::clicked,
            this, &TextBasedEditDialog::onApply);
    connect(m_buttonBox, &QDialogButtonBox::rejected,
            this, &TextBasedEditDialog::reject);

    rebuildList();
    updateSummary();
}

void TextBasedEditDialog::setTranscript(const QList<caption::Clip>& transcript)
{
    m_transcript = transcript;
    m_deletedIndices.clear();
    rebuildList();
    if (m_searchEdit)
        onSearchTextChanged(m_searchEdit->text());
    updateSummary();
}

QVector<textedit::TimeRange> TextBasedEditDialog::deletionRanges() const
{
    return textedit::deletionRanges(m_transcript, m_deletedIndices);
}

QString TextBasedEditDialog::rowLabel(int index, const caption::Clip& clip) const
{
    QString text = clip.text.trimmed();
    if (text.isEmpty())
        text = QStringLiteral("(無音)");
    return QStringLiteral("[%1 - %2] %3")
        .arg(formatMs(clip.startMs))
        .arg(formatMs(clip.endMs))
        .arg(text);
}

void TextBasedEditDialog::rebuildList()
{
    if (!m_listWidget)
        return;

    m_suppressItemChanged = true;
    m_listWidget->clear();

    for (int i = 0; i < m_transcript.size(); ++i) {
        const caption::Clip& clip = m_transcript.at(i);
        auto* item = new QListWidgetItem(rowLabel(i, clip), m_listWidget);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(m_deletedIndices.contains(i) ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, i);
    }

    m_suppressItemChanged = false;
}

void TextBasedEditDialog::onSearchTextChanged(const QString& query)
{
    if (!m_listWidget)
        return;

    // 純粋エンジンでヒット index 集合を得て、その行を強調する。空クエリは全行非強調に戻す。
    const QString trimmed = query.trimmed();
    QSet<int> hits;
    if (!trimmed.isEmpty()) {
        const QVector<int> matched = textedit::search(m_transcript, trimmed);
        for (int idx : matched)
            hits.insert(idx);
    }

    const QColor highlight(255, 245, 157); // 淡い黄色 (ヒット行)
    for (int row = 0; row < m_listWidget->count(); ++row) {
        QListWidgetItem* item = m_listWidget->item(row);
        if (!item)
            continue;
        const int idx = item->data(Qt::UserRole).toInt();
        const bool hit = !trimmed.isEmpty() && hits.contains(idx);
        item->setBackground(hit ? QBrush(highlight) : QBrush());
    }
}

void TextBasedEditDialog::onItemChanged()
{
    if (m_suppressItemChanged || !m_listWidget)
        return;

    m_deletedIndices.clear();
    for (int row = 0; row < m_listWidget->count(); ++row) {
        QListWidgetItem* item = m_listWidget->item(row);
        if (!item)
            continue;
        if (item->checkState() == Qt::Checked)
            m_deletedIndices.insert(item->data(Qt::UserRole).toInt());
    }
    updateSummary();
}

void TextBasedEditDialog::updateSummary()
{
    if (!m_summaryLabel)
        return;

    const QVector<textedit::TimeRange> ranges = deletionRanges();
    const qint64 deletedMs = textedit::totalDeletedMs(ranges);

    m_summaryLabel->setText(
        QStringLiteral("セグメント数: %1 / 削除対象: %2 / 削除区間: %3 / 削除合計: %4")
            .arg(m_transcript.size())
            .arg(m_deletedIndices.size())
            .arg(ranges.size())
            .arg(formatMs(deletedMs)));

    if (m_applyButton)
        m_applyButton->setEnabled(!ranges.isEmpty());
}

void TextBasedEditDialog::onApply()
{
    const QVector<textedit::TimeRange> ranges = deletionRanges();
    if (ranges.isEmpty()) {
        // 削除対象が無ければ何もせず閉じない (誤操作防止)。
        return;
    }
    emit applyDeletions(ranges);
    accept();
}
