// PptxExportDialog: PowerPoint (.pptx) 書き出しの UI 実装。
// デッキ組み立て + ファイル I/O のみを担い、生成は pptxexport::buildPptx に委譲する。

#include "PptxExportDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// ミリ秒を HH:MM:SS タイムコード文字列に整形する純粋ヘルパ。
QString formatTimecode(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 totalSec = ms / 1000;
    const qint64 h = totalSec / 3600;
    const qint64 m = (totalSec % 3600) / 60;
    const qint64 s = totalSec % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace

PptxExportDialog::PptxExportDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("PowerPoint 資料を書き出し (.pptx)"));
    setObjectName(QStringLiteral("pptxExportDialog"));

    auto *root = new QVBoxLayout(this);

    // --- デッキ種別 ---
    auto *kindGroup = new QGroupBox(QStringLiteral("スライドの内容"), this);
    auto *kindLayout = new QVBoxLayout(kindGroup);
    m_kindCombo = new QComboBox(kindGroup);
    m_kindCombo->addItem(QStringLiteral("文字起こしスライド (1 セリフ = 1 枚)"),
                         DeckTranscript);
    m_kindCombo->addItem(QStringLiteral("マーカー / 章一覧 (1 マーカー = 1 枚)"),
                         DeckMarkers);
    m_kindCombo->addItem(QStringLiteral("タイトルのみ (1 枚)"),
                         DeckTitleOnly);
    kindLayout->addWidget(m_kindCombo);
    root->addWidget(kindGroup);

    // --- プレゼン情報 ---
    auto *infoForm = new QFormLayout();
    m_titleEdit = new QLineEdit(this);
    m_titleEdit->setText(QStringLiteral("プレゼンテーション"));
    m_titleEdit->setPlaceholderText(QStringLiteral("プレゼンテーションのタイトル"));
    infoForm->addRow(QStringLiteral("タイトル:"), m_titleEdit);

    m_authorEdit = new QLineEdit(this);
    m_authorEdit->setPlaceholderText(QStringLiteral("作成者 (任意)"));
    infoForm->addRow(QStringLiteral("作成者:"), m_authorEdit);
    root->addLayout(infoForm);

    // --- 出力先 ---
    auto *outForm = new QFormLayout();
    auto *outRow = new QHBoxLayout();
    m_outputEdit = new QLineEdit(this);
    m_outputEdit->setPlaceholderText(QStringLiteral("出力先 .pptx ファイル"));
    m_browseBtn = new QPushButton(QStringLiteral("参照…"), this);
    outRow->addWidget(m_outputEdit, 1);
    outRow->addWidget(m_browseBtn);
    outForm->addRow(QStringLiteral("出力先:"), outRow);
    root->addLayout(outForm);

    // --- 操作ボタン ---
    auto *buttons = new QDialogButtonBox(this);
    m_exportBtn = buttons->addButton(QStringLiteral("書き出し"),
                                     QDialogButtonBox::AcceptRole);
    m_closeBtn = buttons->addButton(QStringLiteral("閉じる"),
                                    QDialogButtonBox::RejectRole);
    root->addWidget(buttons);

    connect(m_browseBtn, &QPushButton::clicked,
            this, &PptxExportDialog::browseOutputPath);
    connect(m_exportBtn, &QPushButton::clicked,
            this, &PptxExportDialog::doExport);
    connect(m_closeBtn, &QPushButton::clicked,
            this, &PptxExportDialog::reject);
}

void PptxExportDialog::setCaptions(const QList<caption::Clip> &captions)
{
    m_captions = captions;
}

void PptxExportDialog::setMarkers(const QVector<Marker> &markers)
{
    m_markers = markers;
}

void PptxExportDialog::browseOutputPath()
{
    QString initial = m_outputEdit->text();
    if (initial.isEmpty())
        initial = QStringLiteral("presentation.pptx");
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("PowerPoint ファイルの保存先"),
        initial,
        QStringLiteral("PowerPoint (*.pptx)"));
    if (!path.isEmpty())
        m_outputEdit->setText(path);
}

// 現在の選択・入力から pptxexport::Deck を組み立てる。副作用なし。
pptxexport::Deck PptxExportDialog::buildDeck() const
{
    pptxexport::Deck deck;
    deck.title  = m_titleEdit->text().trimmed();
    if (deck.title.isEmpty())
        deck.title = QStringLiteral("プレゼンテーション");
    deck.author = m_authorEdit->text().trimmed();

    const int kind = m_kindCombo->currentData().toInt();

    switch (kind) {
    case DeckTranscript: {
        // 1 clip = 1 slide。タイトルは連番 + タイムコード、本文は clip の文字列。
        int index = 1;
        for (const caption::Clip &c : m_captions) {
            pptxexport::Slide slide;
            slide.title = QStringLiteral("%1. %2")
                              .arg(index)
                              .arg(formatTimecode(c.startMs));
            slide.bullets = QStringList{ c.text };
            deck.slides.append(slide);
            ++index;
        }
        break;
    }
    case DeckMarkers: {
        // 1 marker = 1 slide。タイトルはタイムコード、本文はラベル/ノート。
        int index = 1;
        for (const Marker &mk : m_markers) {
            pptxexport::Slide slide;
            const qint64 ms = mk.timelineUs / 1000;
            const QString label = mk.label.isEmpty()
                ? QStringLiteral("マーカー %1").arg(index)
                : mk.label;
            slide.title = QStringLiteral("%1  %2")
                              .arg(formatTimecode(ms))
                              .arg(label);
            if (!mk.note.isEmpty())
                slide.bullets = QStringList{ mk.note };
            deck.slides.append(slide);
            ++index;
        }
        break;
    }
    case DeckTitleOnly:
    default:
        // タイトルのみ: スライド 0 枚 (buildPptx がタイトル 1 枚を補う)。
        break;
    }

    return deck;
}

void PptxExportDialog::doExport()
{
    QString path = m_outputEdit->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
            QStringLiteral("出力先のファイルを指定してください。"));
        return;
    }
    // 拡張子を補う。
    if (!path.endsWith(QStringLiteral(".pptx"), Qt::CaseInsensitive))
        path += QStringLiteral(".pptx");

    // 選択内容の妥当性を軽く確認 (空でも buildPptx は有効ファイルを返すが、
    // ユーザの意図と食い違うケースを案内する)。
    const int kind = m_kindCombo->currentData().toInt();
    if (kind == DeckTranscript && m_captions.isEmpty()) {
        QMessageBox::information(this, windowTitle(),
            QStringLiteral("文字起こし結果がありません。"
                           "タイトル 1 枚のみの資料を書き出します。"));
    } else if (kind == DeckMarkers && m_markers.isEmpty()) {
        QMessageBox::information(this, windowTitle(),
            QStringLiteral("マーカーがありません。"
                           "タイトル 1 枚のみの資料を書き出します。"));
    }

    // デッキ組み立て → 純粋エンジンで .pptx バイト列を生成。
    const pptxexport::Deck deck = buildDeck();
    const QByteArray bytes = pptxexport::buildPptx(deck);
    if (bytes.isEmpty()) {
        QMessageBox::critical(this, windowTitle(),
            QStringLiteral("PPTX の生成に失敗しました。"));
        return;
    }

    // ファイル書き込み。
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, windowTitle(),
            QStringLiteral("ファイルを書き込めませんでした:\n%1\n\n%2")
                .arg(path, file.errorString()));
        return;
    }
    const qint64 written = file.write(bytes);
    const bool flushed = (written == bytes.size()) && file.flush();
    const QString writeError = file.errorString();
    file.close();

    if (written != bytes.size() || !flushed) {
        QMessageBox::critical(this, windowTitle(),
            QStringLiteral("ファイルの書き込みが途中で失敗しました:\n%1\n\n%2")
                .arg(path, writeError));
        return;
    }

    QMessageBox::information(this, windowTitle(),
        QStringLiteral("PowerPoint 資料を書き出しました:\n%1\n\n(%2 スライド)")
            .arg(QFileInfo(path).absoluteFilePath())
            .arg(qMax(1, deck.slides.size())));  // 0 枚はタイトル 1 枚に補完される
    accept();
}
