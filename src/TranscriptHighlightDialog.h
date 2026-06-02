#pragma once

#include <QDialog>
#include <QList>

#include "TranscriptHighlighter.h"
#include "CaptionTrack.h"

class QComboBox;
class QSpinBox;
class QLabel;
class QPlainTextEdit;
class QDialogButtonBox;

class TranscriptHighlightDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TranscriptHighlightDialog(QWidget* parent = nullptr);

    // MainWindow から現在の字幕トラックを渡す
    void setTranscript(const QList<caption::Clip>& transcript);

    // 結果表示エリアを更新 (MainWindow から呼ぶ)
    void setResultText(const QString& text);

    // 現在の UI 値 + transcript から HighlightRequest を組んで返す
    transcripthl::HighlightRequest request() const;

private:
    void updateDetectState();

    QList<caption::Clip> m_transcript;

    QComboBox*        m_providerCombo = nullptr;
    QSpinBox*         m_countSpin     = nullptr;
    QLabel*           m_descLabel     = nullptr;
    QLabel*           m_apiKeyWarningLabel = nullptr;
    QPlainTextEdit*   m_resultEdit    = nullptr;
    QDialogButtonBox* m_buttonBox     = nullptr;
};
