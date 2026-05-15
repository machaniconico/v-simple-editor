#pragma once

#include <QDialog>

class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QPlainTextEdit;

// ---------------------------------------------------------------------------
// TwitchStreamDialog — Sprint 20 US-TWI-1
// Twitch ライブ配信設定ダイアログ。モードレス。
// ffmpeg RTMP コマンドをプレビュー・クリップボードコピーできる。
// ---------------------------------------------------------------------------

class TwitchStreamDialog : public QDialog {
    Q_OBJECT
public:
    explicit TwitchStreamDialog(QWidget *parent = nullptr);
    ~TwitchStreamDialog() override = default;

private slots:
    void onGenerateClicked();
    void onCopyClicked();

private:
    QLineEdit     *m_streamKeyEdit  = nullptr;
    QComboBox     *m_serverCombo    = nullptr;
    QSpinBox      *m_bitrateSpin    = nullptr;
    QSpinBox      *m_fpsSpin        = nullptr;
    QCheckBox     *m_saveKeyCheck   = nullptr;
    QPlainTextEdit *m_commandView   = nullptr;
};
