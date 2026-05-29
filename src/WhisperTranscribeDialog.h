#pragma once

#include <QDialog>
#include <QString>

#include "WhisperTranscriber.h"

class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QDialogButtonBox;

class WhisperTranscribeDialog : public QDialog {
    Q_OBJECT

public:
    explicit WhisperTranscribeDialog(QWidget* parent = nullptr);

    // 入力メディアパスを外部から初期設定 (現在開いている動画など)
    void setMediaPath(const QString& path);

    // 結果表示用ラベルを更新 (MainWindow から呼ぶ)
    void setResultText(const QString& text);

    // 現在の UI 値から TranscribeRequest を組んで返す
    whisper::TranscribeRequest request() const;

private slots:
    void onBrowseClicked();
    void updateAcceptState();

private:
    QLineEdit*        m_pathEdit       = nullptr;
    QPushButton*      m_browseButton   = nullptr;
    QComboBox*        m_modelCombo     = nullptr;
    QComboBox*        m_languageCombo  = nullptr;
    QLabel*           m_resultLabel    = nullptr;
    QDialogButtonBox* m_buttonBox      = nullptr;
};
