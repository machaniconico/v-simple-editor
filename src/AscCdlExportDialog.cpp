// AscCdlExportDialog: ASC CDL 書き出しの UI 実装。
// SOP 抽出 (MainWindow 側) + ファイル I/O のみを担い、
// XML 生成は asccdl::buildCc / buildCcc / buildCdl に委譲する。

#include "AscCdlExportDialog.h"

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
#include <QVector>

namespace {

bool stripKnownCdlExtension(QString &path)
{
    for (const QString &known : { QStringLiteral(".ccc"),
                                  QStringLiteral(".cdl"),
                                  QStringLiteral(".cc") }) {
        if (path.endsWith(known, Qt::CaseInsensitive)) {
            path.chop(known.size());
            return true;
        }
    }
    return false;
}

} // namespace

AscCdlExportDialog::AscCdlExportDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("ASC CDL を書き出し (.cc / .ccc / .cdl)"));
    setObjectName(QStringLiteral("ascCdlExportDialog"));

    auto *root = new QVBoxLayout(this);

    // --- 形式 ---
    auto *fmtGroup = new QGroupBox(QStringLiteral("書き出し形式"), this);
    auto *fmtLayout = new QVBoxLayout(fmtGroup);
    m_formatCombo = new QComboBox(fmtGroup);
    m_formatCombo->addItem(
        QStringLiteral("単一補正 ColorCorrection (.cc)"), FormatCc);
    m_formatCombo->addItem(
        QStringLiteral("コレクション ColorCorrectionCollection (.ccc)"), FormatCcc);
    m_formatCombo->addItem(
        QStringLiteral("デシジョンリスト ColorDecisionList (.cdl)"), FormatCdl);
    fmtLayout->addWidget(m_formatCombo);
    root->addWidget(fmtGroup);

    // --- 補正情報 ---
    auto *infoForm = new QFormLayout();
    m_idEdit = new QLineEdit(this);
    m_idEdit->setText(QStringLiteral("cc0001"));
    m_idEdit->setPlaceholderText(QStringLiteral("ColorCorrection の id (任意)"));
    infoForm->addRow(QStringLiteral("補正 ID:"), m_idEdit);
    root->addLayout(infoForm);

    auto *hint = new QLabel(
        QStringLiteral("現在のカラーグレーディング (Lift/Gamma/Gain + 彩度) を "
                       "ASC CDL の SOP に変換して書き出します。"),
        this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    // --- 出力先 ---
    auto *outForm = new QFormLayout();
    auto *outRow = new QHBoxLayout();
    m_outputEdit = new QLineEdit(this);
    m_outputEdit->setPlaceholderText(QStringLiteral("出力先ファイル"));
    m_browseBtn = new QPushButton(QStringLiteral("参照…"), this);
    outRow->addWidget(m_outputEdit, 1);
    outRow->addWidget(m_browseBtn);
    outForm->addRow(QStringLiteral("出力先:"), outRow);
    root->addLayout(outForm);

    // --- 操作ボタン ---
    auto *buttons = new QDialogButtonBox(this);
    m_exportBtn = buttons->addButton(QStringLiteral("現在のグレーディングから書き出し"),
                                     QDialogButtonBox::AcceptRole);
    m_closeBtn = buttons->addButton(QStringLiteral("閉じる"),
                                    QDialogButtonBox::RejectRole);
    root->addWidget(buttons);

    connect(m_formatCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AscCdlExportDialog::onFormatChanged);
    connect(m_browseBtn, &QPushButton::clicked,
            this, &AscCdlExportDialog::browseOutputPath);
    connect(m_exportBtn, &QPushButton::clicked,
            this, &AscCdlExportDialog::doExport);
    connect(m_closeBtn, &QPushButton::clicked,
            this, &AscCdlExportDialog::reject);
}

void AscCdlExportDialog::setCorrection(const asccdl::CdlCorrection &correction)
{
    m_correction = correction;
    if (m_idEdit && !correction.id.isEmpty())
        m_idEdit->setText(correction.id);
}

QString AscCdlExportDialog::extForFormat(int format)
{
    switch (format) {
    case FormatCcc: return QStringLiteral(".ccc");
    case FormatCdl: return QStringLiteral(".cdl");
    case FormatCc:
    default:        return QStringLiteral(".cc");
    }
}

void AscCdlExportDialog::onFormatChanged()
{
    // 出力欄に既にパスがあれば、選択形式に合わせて拡張子を付け替える。
    const int format = m_formatCombo->currentData().toInt();
    const QString ext = extForFormat(format);
    QString path = m_outputEdit->text().trimmed();
    if (path.isEmpty())
        return;

    // 既知の CDL 拡張子を剥がしてから付け直す。
    stripKnownCdlExtension(path);
    m_outputEdit->setText(path + ext);
}

void AscCdlExportDialog::browseOutputPath()
{
    const int format = m_formatCombo->currentData().toInt();
    const QString ext = extForFormat(format);

    QString initial = m_outputEdit->text().trimmed();
    if (initial.isEmpty())
        initial = QStringLiteral("grade") + ext;

    QString filter;
    switch (format) {
    case FormatCcc:
        filter = QStringLiteral("ASC CDL Collection (*.ccc)");
        break;
    case FormatCdl:
        filter = QStringLiteral("ASC Color Decision List (*.cdl)");
        break;
    case FormatCc:
    default:
        filter = QStringLiteral("ASC Color Correction (*.cc)");
        break;
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("ASC CDL ファイルの保存先"),
        initial,
        filter);
    if (!path.isEmpty())
        m_outputEdit->setText(path);
}

void AscCdlExportDialog::doExport()
{
    const int format = m_formatCombo->currentData().toInt();
    const QString ext = extForFormat(format);

    QString path = m_outputEdit->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
            QStringLiteral("出力先のファイルを指定してください。"));
        return;
    }
    // 選択形式の拡張子を補う。既知の CDL 拡張子が手入力されていれば付け替える。
    if (!path.endsWith(ext, Qt::CaseInsensitive)) {
        stripKnownCdlExtension(path);
        path += ext;
    }

    // id 入力欄を補正に反映する。
    asccdl::CdlCorrection c = m_correction;
    c.id = m_idEdit->text().trimmed();

    // 形式に応じて XML 文字列を生成する。
    QString xml;
    switch (format) {
    case FormatCcc:
        xml = asccdl::buildCcc(QVector<asccdl::CdlCorrection>{ c });
        break;
    case FormatCdl:
        xml = asccdl::buildCdl(QVector<asccdl::CdlCorrection>{ c });
        break;
    case FormatCc:
    default:
        xml = asccdl::buildCc(c);
        break;
    }

    if (xml.isEmpty()) {
        QMessageBox::critical(this, windowTitle(),
            QStringLiteral("ASC CDL の生成に失敗しました。"));
        return;
    }

    // UTF-8 で書き込み。
    const QByteArray bytes = xml.toUtf8();
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
        QStringLiteral("ASC CDL を書き出しました:\n%1")
            .arg(QFileInfo(path).absoluteFilePath()));
    accept();
}
