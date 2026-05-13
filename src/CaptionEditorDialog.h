#pragma once
#include <QDialog>
#include "CaptionTrack.h"
#include "CaptionStyle.h"

class QTableWidget;
class QTextEdit;
class QSpinBox;
class QFontComboBox;
class QComboBox;
class QPushButton;
class QCheckBox;
class QLabel;
class QDoubleSpinBox;
class QDialogButtonBox;

class CaptionEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit CaptionEditorDialog(QWidget* parent = nullptr);

    void setTrack(const caption::Track& track);
    caption::Track track() const;

    void setStyle(const caption::Style& style);
    caption::Style style() const;

signals:
    void trackChanged(const caption::Track& track);
    void styleChanged(const caption::Style& style);

private slots:
    void onAddClipClicked();
    void onRemoveClipClicked();
    void onImportClicked();
    void onExportClicked();
    void onRecognizeClicked();
    void onClipRowChanged(int row);
    void onClipTextEdited();
    void onClipTimeEdited();
    void onStyleChanged();

private:
    void rebuildClipTable();
    void refreshClipRow(int row);
    void updateStyleControls();
    void updatePreview();

    caption::Track m_track;
    caption::Style m_style;

    QTableWidget*    m_clipTable        = nullptr;
    QTextEdit*       m_textEdit         = nullptr;
    QSpinBox*        m_startMsSpin      = nullptr;
    QSpinBox*        m_endMsSpin        = nullptr;

    QFontComboBox*   m_fontCombo        = nullptr;
    QSpinBox*        m_fontSizeSpin     = nullptr;
    QCheckBox*       m_boldCheck        = nullptr;
    QCheckBox*       m_italicCheck      = nullptr;
    QPushButton*     m_textColorButton  = nullptr;
    QPushButton*     m_outlineColorButton = nullptr;
    QDoubleSpinBox*  m_outlineWidthSpin = nullptr;
    QCheckBox*       m_bgCheck          = nullptr;
    QPushButton*     m_bgColorButton    = nullptr;
    QComboBox*       m_anchorCombo      = nullptr;

    QLabel*          m_previewLabel     = nullptr;

    QPushButton*     m_addClipButton    = nullptr;
    QPushButton*     m_removeClipButton = nullptr;
    QPushButton*     m_importButton     = nullptr;
    QPushButton*     m_exportButton     = nullptr;
    QPushButton*     m_recognizeButton  = nullptr;
    QComboBox*       m_recognizerCombo  = nullptr;
    QComboBox*       m_languageCombo    = nullptr;
    QDialogButtonBox* m_buttonBox       = nullptr;

    int m_currentRow = -1;
};
