#include "CaptionEditorDialog.h"
#include "SubtitleIO.h"
#include "SpeechRecognizer.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// コンストラクタ
// ---------------------------------------------------------------------------
CaptionEditorDialog::CaptionEditorDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("字幕エディタ"));
    setObjectName(QStringLiteral("captionEditorDialog"));
    resize(960, 600);

    m_track = caption::Track{};
    m_style = caption::defaultStyle();

    // ===================== 左パネル =====================
    // クリップテーブル
    m_clipTable = new QTableWidget(this);
    m_clipTable->setColumnCount(4);
    m_clipTable->setHorizontalHeaderLabels({tr("開始(ms)"), tr("終了(ms)"), tr("テキスト"), tr("話者")});
    m_clipTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_clipTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_clipTable->setAlternatingRowColors(true);
    m_clipTable->horizontalHeader()->setStretchLastSection(true);
    m_clipTable->verticalHeader()->setVisible(false);

    // クリップ操作ボタン
    m_addClipButton    = new QPushButton(tr("追加"), this);
    m_removeClipButton = new QPushButton(tr("削除"), this);
    m_importButton     = new QPushButton(tr("SRT/VTT 取り込み…"), this);
    m_exportButton     = new QPushButton(tr("SRT エクスポート…"), this);

    auto* clipButtonBar = new QHBoxLayout;
    clipButtonBar->addWidget(m_addClipButton);
    clipButtonBar->addWidget(m_removeClipButton);
    clipButtonBar->addWidget(m_importButton);
    clipButtonBar->addWidget(m_exportButton);

    // ASR 行
    m_recognizerCombo = new QComboBox(this);
    for (auto& rec : speech::availableRecognizers())
        m_recognizerCombo->addItem(rec->name());

    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItems({QStringLiteral("auto"), QStringLiteral("ja"), QStringLiteral("en")});

    m_recognizeButton = new QPushButton(tr("音声認識実行…"), this);

    auto* asrRow = new QHBoxLayout;
    asrRow->addWidget(new QLabel(tr("認識:"), this));
    asrRow->addWidget(m_recognizerCombo, 1);
    asrRow->addWidget(new QLabel(tr("言語:"), this));
    asrRow->addWidget(m_languageCombo);
    asrRow->addWidget(m_recognizeButton);

    auto* leftLayout = new QVBoxLayout;
    leftLayout->addWidget(m_clipTable, 1);
    leftLayout->addLayout(clipButtonBar);
    leftLayout->addLayout(asrRow);

    auto* leftWidget = new QWidget(this);
    leftWidget->setFixedWidth(420);
    leftWidget->setLayout(leftLayout);

    // ===================== 右パネル =====================
    // テキスト GroupBox
    m_textEdit = new QTextEdit(this);
    m_textEdit->setFixedHeight(100);

    m_startMsSpin = new QSpinBox(this);
    m_startMsSpin->setRange(0, 3600000);
    m_startMsSpin->setSuffix(QStringLiteral(" ms"));

    m_endMsSpin = new QSpinBox(this);
    m_endMsSpin->setRange(0, 3600000);
    m_endMsSpin->setSuffix(QStringLiteral(" ms"));

    auto* timeRow = new QHBoxLayout;
    timeRow->addWidget(new QLabel(tr("開始:"), this));
    timeRow->addWidget(m_startMsSpin, 1);
    timeRow->addWidget(new QLabel(tr("終了:"), this));
    timeRow->addWidget(m_endMsSpin, 1);

    auto* textGroupLayout = new QVBoxLayout;
    textGroupLayout->addWidget(m_textEdit);
    textGroupLayout->addLayout(timeRow);

    auto* textGroup = new QGroupBox(tr("テキスト"), this);
    textGroup->setLayout(textGroupLayout);

    // スタイル GroupBox
    m_fontCombo = new QFontComboBox(this);

    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setRange(8, 72);

    m_boldCheck   = new QCheckBox(tr("太字"), this);
    m_italicCheck = new QCheckBox(tr("斜体"), this);

    auto* boldItalicRow = new QHBoxLayout;
    boldItalicRow->addWidget(m_boldCheck);
    boldItalicRow->addWidget(m_italicCheck);
    boldItalicRow->addStretch();

    m_textColorButton    = new QPushButton(this);
    m_outlineColorButton = new QPushButton(this);

    m_outlineWidthSpin = new QDoubleSpinBox(this);
    m_outlineWidthSpin->setRange(0.0, 10.0);
    m_outlineWidthSpin->setSingleStep(0.5);
    m_outlineWidthSpin->setDecimals(1);

    auto* outlineRow = new QHBoxLayout;
    outlineRow->addWidget(m_outlineColorButton, 1);
    outlineRow->addWidget(m_outlineWidthSpin);

    m_bgCheck       = new QCheckBox(tr("背景を有効化"), this);
    m_bgColorButton = new QPushButton(this);

    auto* bgRow = new QHBoxLayout;
    bgRow->addWidget(m_bgCheck);
    bgRow->addWidget(m_bgColorButton, 1);

    m_anchorCombo = new QComboBox(this);
    m_anchorCombo->addItems(caption::anchorNames());

    auto* styleForm = new QFormLayout;
    styleForm->addRow(tr("フォント:"),      m_fontCombo);
    styleForm->addRow(tr("サイズ:"),        m_fontSizeSpin);
    styleForm->addRow(tr("スタイル:"),      boldItalicRow);
    styleForm->addRow(tr("文字色:"),        m_textColorButton);
    styleForm->addRow(tr("縁取り色/太さ:"), outlineRow);
    styleForm->addRow(tr("背景:"),          bgRow);
    styleForm->addRow(tr("位置:"),          m_anchorCombo);

    auto* styleGroup = new QGroupBox(tr("スタイル"), this);
    styleGroup->setLayout(styleForm);

    // プレビュー
    m_previewLabel = new QLabel(this);
    m_previewLabel->setFixedSize(320, 80);
    m_previewLabel->setAlignment(Qt::AlignCenter);

    // ダイアログボタン
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* rightLayout = new QVBoxLayout;
    rightLayout->addWidget(textGroup);
    rightLayout->addWidget(styleGroup);
    rightLayout->addWidget(m_previewLabel, 0, Qt::AlignHCenter);
    rightLayout->addStretch();
    rightLayout->addWidget(m_buttonBox);

    // ===================== メインレイアウト =====================
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->addWidget(leftWidget);
    mainLayout->addLayout(rightLayout, 1);

    // ===================== シグナル接続 =====================
    connect(m_clipTable, &QTableWidget::currentCellChanged,
            this, [this](int row, int, int, int) { onClipRowChanged(row); });

    connect(m_textEdit, &QTextEdit::textChanged, this, &CaptionEditorDialog::onClipTextEdited);
    connect(m_startMsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &CaptionEditorDialog::onClipTimeEdited);
    connect(m_endMsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &CaptionEditorDialog::onClipTimeEdited);

    connect(m_addClipButton,    &QPushButton::clicked, this, &CaptionEditorDialog::onAddClipClicked);
    connect(m_removeClipButton, &QPushButton::clicked, this, &CaptionEditorDialog::onRemoveClipClicked);
    connect(m_importButton,     &QPushButton::clicked, this, &CaptionEditorDialog::onImportClicked);
    connect(m_exportButton,     &QPushButton::clicked, this, &CaptionEditorDialog::onExportClicked);
    connect(m_recognizeButton,  &QPushButton::clicked, this, &CaptionEditorDialog::onRecognizeClicked);

    // スタイルコントロール → onStyleChanged
    connect(m_fontCombo,    &QFontComboBox::currentFontChanged, this, [this](const QFont&) { onStyleChanged(); });
    connect(m_fontSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { onStyleChanged(); });
    connect(m_boldCheck,    &QCheckBox::toggled, this, [this](bool) { onStyleChanged(); });
    connect(m_italicCheck,  &QCheckBox::toggled, this, [this](bool) { onStyleChanged(); });
    connect(m_outlineWidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { onStyleChanged(); });
    connect(m_bgCheck,    &QCheckBox::toggled, this, [this](bool) { onStyleChanged(); });
    connect(m_anchorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onStyleChanged(); });

    // 色ボタン — クリックで QColorDialog
    connect(m_textColorButton, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_style.textColor, this, tr("文字色を選択"));
        if (c.isValid()) {
            m_style.textColor = c;
            m_textColorButton->setStyleSheet(
                QStringLiteral("background-color: %1").arg(c.name()));
            onStyleChanged();
        }
    });
    connect(m_outlineColorButton, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_style.outlineColor, this, tr("縁取り色を選択"));
        if (c.isValid()) {
            m_style.outlineColor = c;
            m_outlineColorButton->setStyleSheet(
                QStringLiteral("background-color: %1").arg(c.name()));
            onStyleChanged();
        }
    });
    connect(m_bgColorButton, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_style.backgroundColor, this, tr("背景色を選択"));
        if (c.isValid()) {
            m_style.backgroundColor = c;
            m_bgColorButton->setStyleSheet(
                QStringLiteral("background-color: %1").arg(c.name()));
            onStyleChanged();
        }
    });

    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 初期状態
    updateStyleControls();
    rebuildClipTable();
    updatePreview();
}

// ---------------------------------------------------------------------------
// setTrack / track
// ---------------------------------------------------------------------------
void CaptionEditorDialog::setTrack(const caption::Track& track)
{
    m_track = track;
    rebuildClipTable();
    updatePreview();
    emit trackChanged(m_track);
}

caption::Track CaptionEditorDialog::track() const
{
    return m_track;
}

// ---------------------------------------------------------------------------
// setStyle / style
// ---------------------------------------------------------------------------
void CaptionEditorDialog::setStyle(const caption::Style& style)
{
    m_style = style;
    updateStyleControls();
    updatePreview();
    emit styleChanged(m_style);
}

caption::Style CaptionEditorDialog::style() const
{
    return m_style;
}

// ---------------------------------------------------------------------------
// onClipRowChanged
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onClipRowChanged(int row)
{
    m_currentRow = row;
    if (row < 0 || row >= m_track.clipCount())
        return;

    const caption::Clip clip = m_track.clipAt(row);

    QSignalBlocker b1(m_textEdit);
    QSignalBlocker b2(m_startMsSpin);
    QSignalBlocker b3(m_endMsSpin);

    m_textEdit->setPlainText(clip.text);
    m_startMsSpin->setValue(static_cast<int>(clip.startMs));
    m_endMsSpin->setValue(static_cast<int>(clip.endMs));

    updatePreview();
}

// ---------------------------------------------------------------------------
// onClipTextEdited
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onClipTextEdited()
{
    if (m_currentRow < 0 || m_currentRow >= m_track.clipCount())
        return;

    caption::Clip clip = m_track.clipAt(m_currentRow);
    clip.text = m_textEdit->toPlainText();
    m_track.updateClip(m_currentRow, clip);
    refreshClipRow(m_currentRow);
    emit trackChanged(m_track);
    updatePreview();
}

// ---------------------------------------------------------------------------
// onClipTimeEdited
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onClipTimeEdited()
{
    if (m_currentRow < 0 || m_currentRow >= m_track.clipCount())
        return;

    caption::Clip clip = m_track.clipAt(m_currentRow);
    clip.startMs = m_startMsSpin->value();
    clip.endMs   = m_endMsSpin->value();
    m_track.updateClip(m_currentRow, clip);
    refreshClipRow(m_currentRow);
    emit trackChanged(m_track);
}

// ---------------------------------------------------------------------------
// onAddClipClicked
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onAddClipClicked()
{
    caption::Clip c;
    c.startMs = 0;
    c.endMs   = 2000;
    c.text    = tr("新しい字幕");

    m_track.addClip(c);
    m_track.sortByStart();
    rebuildClipTable();

    // 追加した clip の行を選択 (先頭にある可能性が高い)
    if (m_track.clipCount() > 0)
        m_clipTable->selectRow(0);

    emit trackChanged(m_track);
}

// ---------------------------------------------------------------------------
// onRemoveClipClicked
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onRemoveClipClicked()
{
    if (m_currentRow < 0 || m_currentRow >= m_track.clipCount())
        return;

    m_track.removeClipAt(m_currentRow);
    rebuildClipTable();
    emit trackChanged(m_track);
}

// ---------------------------------------------------------------------------
// onImportClicked
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onImportClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("字幕ファイルを開く"),
        QString(),
        tr("SRT (*.srt);;VTT (*.vtt);;すべて (*)"));

    if (path.isEmpty())
        return;

    subtitle::ImportResult result;
    if (path.endsWith(QStringLiteral(".srt"), Qt::CaseInsensitive))
        result = subtitle::importSrt(path);
    else
        result = subtitle::importVtt(path);

    if (!result.success) {
        QMessageBox::warning(this, tr("取り込みエラー"), result.error);
        return;
    }

    m_track.clear();
    for (const auto& clip : result.clips)
        m_track.addClip(clip);
    m_track.sortByStart();

    rebuildClipTable();
    emit trackChanged(m_track);
}

// ---------------------------------------------------------------------------
// onExportClicked
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onExportClicked()
{
    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("字幕を保存"),
        QString(),
        tr("SRT (*.srt);;VTT (*.vtt)"));

    if (path.isEmpty())
        return;

    bool ok = false;
    if (path.endsWith(QStringLiteral(".vtt"), Qt::CaseInsensitive))
        ok = subtitle::exportVtt(path, m_track.clips());
    else
        ok = subtitle::exportSrt(path, m_track.clips());

    if (!ok)
        QMessageBox::warning(this, tr("エクスポートエラー"), tr("ファイルの書き込みに失敗しました。"));
}

// ---------------------------------------------------------------------------
// onRecognizeClicked
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onRecognizeClicked()
{
    const QString audioPath = QFileDialog::getOpenFileName(
        this,
        tr("音声/動画ファイルを選択"),
        QString(),
        tr("音声/動画 (*.wav *.mp3 *.aac *.mp4 *.mov *.mkv);;すべて (*)"));

    if (audioPath.isEmpty())
        return;

    auto recognizer = speech::recognizerByName(m_recognizerCombo->currentText());
    if (!recognizer) {
        QMessageBox::warning(this, tr("認識エラー"), tr("認識エンジンが見つかりません。"));
        return;
    }

    speech::RecognizeParams params;
    params.audioPath = audioPath;
    params.language  = m_languageCombo->currentText();
    params.modelId   = QStringLiteral("base");

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const speech::RecognizeResult res = recognizer->recognize(params);
    QApplication::restoreOverrideCursor();

    if (!res.success) {
        QMessageBox::warning(this, tr("認識失敗"), res.error);
        return;
    }

    m_track.clear();
    for (const auto& seg : res.segments) {
        caption::Clip clip;
        clip.startMs = seg.startMs;
        clip.endMs   = seg.endMs;
        clip.text    = seg.text;
        m_track.addClip(clip);
    }
    m_track.sortByStart();
    rebuildClipTable();
    emit trackChanged(m_track);

    QMessageBox::information(
        this,
        tr("認識完了"),
        tr("%1 個のセグメントを取り込みました。").arg(res.segments.size()));
}

// ---------------------------------------------------------------------------
// onStyleChanged — UI → m_style
// ---------------------------------------------------------------------------
void CaptionEditorDialog::onStyleChanged()
{
    m_style.fontFamily       = m_fontCombo->currentFont().family();
    m_style.fontSizePt       = m_fontSizeSpin->value();
    m_style.bold             = m_boldCheck->isChecked();
    m_style.italic           = m_italicCheck->isChecked();
    m_style.outlineThickness = m_outlineWidthSpin->value();
    m_style.background       = m_bgCheck->isChecked();
    m_style.anchor           = caption::anchorFromString(m_anchorCombo->currentText());

    updatePreview();
    emit styleChanged(m_style);
}

// ---------------------------------------------------------------------------
// rebuildClipTable
// ---------------------------------------------------------------------------
void CaptionEditorDialog::rebuildClipTable()
{
    const int count = m_track.clipCount();

    {
        QSignalBlocker blocker(m_clipTable);

        m_clipTable->setRowCount(0);
        m_clipTable->setColumnCount(4);
        m_clipTable->setHorizontalHeaderLabels(
            {tr("開始(ms)"), tr("終了(ms)"), tr("テキスト"), tr("話者")});

        m_clipTable->setRowCount(count);

        for (int i = 0; i < count; ++i) {
            const caption::Clip& clip = m_track.clipAt(i);
            m_clipTable->setItem(i, 0, new QTableWidgetItem(QString::number(clip.startMs)));
            m_clipTable->setItem(i, 1, new QTableWidgetItem(QString::number(clip.endMs)));
            m_clipTable->setItem(i, 2, new QTableWidgetItem(clip.text));
            m_clipTable->setItem(i, 3, new QTableWidgetItem(clip.actor));
        }

        m_currentRow = -1;
    } // blocker 解除 — ここから currentCellChanged が発火する

    if (count > 0) {
        m_clipTable->selectRow(0);      // signal fires → onClipRowChanged(0)
    } else {
        // テーブルが空のとき編集欄をクリア
        onClipRowChanged(-1);
    }
}

// ---------------------------------------------------------------------------
// refreshClipRow — 1 行のみ更新
// ---------------------------------------------------------------------------
void CaptionEditorDialog::refreshClipRow(int row)
{
    if (row < 0 || row >= m_track.clipCount())
        return;

    const caption::Clip& clip = m_track.clipAt(row);

    QSignalBlocker blocker(m_clipTable);
    m_clipTable->item(row, 0)->setText(QString::number(clip.startMs));
    m_clipTable->item(row, 1)->setText(QString::number(clip.endMs));
    m_clipTable->item(row, 2)->setText(clip.text);
    m_clipTable->item(row, 3)->setText(clip.actor);
}

// ---------------------------------------------------------------------------
// updateStyleControls — m_style → UI
// ---------------------------------------------------------------------------
void CaptionEditorDialog::updateStyleControls()
{
    QSignalBlocker b1(m_fontCombo);
    QSignalBlocker b2(m_fontSizeSpin);
    QSignalBlocker b3(m_boldCheck);
    QSignalBlocker b4(m_italicCheck);
    QSignalBlocker b5(m_outlineWidthSpin);
    QSignalBlocker b6(m_bgCheck);
    QSignalBlocker b7(m_anchorCombo);

    QFont f(m_style.fontFamily);
    m_fontCombo->setCurrentFont(f);
    m_fontSizeSpin->setValue(m_style.fontSizePt);
    m_boldCheck->setChecked(m_style.bold);
    m_italicCheck->setChecked(m_style.italic);
    m_outlineWidthSpin->setValue(m_style.outlineThickness);
    m_bgCheck->setChecked(m_style.background);

    const QString anchorStr = caption::anchorToString(m_style.anchor);
    const int anchorIdx = m_anchorCombo->findText(anchorStr);
    if (anchorIdx >= 0)
        m_anchorCombo->setCurrentIndex(anchorIdx);

    m_textColorButton->setStyleSheet(
        QStringLiteral("background-color: %1").arg(m_style.textColor.name()));
    m_outlineColorButton->setStyleSheet(
        QStringLiteral("background-color: %1").arg(m_style.outlineColor.name()));
    m_bgColorButton->setStyleSheet(
        QStringLiteral("background-color: %1").arg(m_style.backgroundColor.name()));
}

// ---------------------------------------------------------------------------
// updatePreview — 簡易レンダリング → m_previewLabel
// ---------------------------------------------------------------------------
void CaptionEditorDialog::updatePreview()
{
    const QSize sz = m_previewLabel->size();
    QPixmap pix(sz);
    pix.fill(QColor(QStringLiteral("#1e1e1e")));

    const QString text = (m_currentRow >= 0 && m_currentRow < m_track.clipCount())
                         ? m_track.clipAt(m_currentRow).text
                         : m_textEdit ? m_textEdit->toPlainText() : QString();

    if (!text.isEmpty()) {
        QPainter painter(&pix);
        painter.setRenderHint(QPainter::Antialiasing);

        QFont font(m_style.fontFamily, m_style.fontSizePt);
        font.setBold(m_style.bold);
        font.setItalic(m_style.italic);
        painter.setFont(font);

        const QRectF bounds(4, 4, sz.width() - 8, sz.height() - 8);

        // outline (stroke via QPainterPath)
        if (m_style.outlineThickness > 0.0) {
            QPainterPath path;
            path.addText(
                bounds.left(),
                bounds.center().y() + QFontMetrics(font).ascent() / 2.0,
                font,
                text);

            QPen pen(m_style.outlineColor, m_style.outlineThickness * 2.0);
            pen.setJoinStyle(Qt::RoundJoin);
            pen.setCapStyle(Qt::RoundCap);
            painter.strokePath(path, pen);
            painter.fillPath(path, QBrush(m_style.textColor));
        } else {
            painter.setPen(m_style.textColor);
            painter.drawText(bounds, Qt::AlignCenter | Qt::TextWordWrap, text);
        }

        painter.end();
    }

    m_previewLabel->setPixmap(pix);
}
