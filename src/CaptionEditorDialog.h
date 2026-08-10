#pragma once
#include <QDialog>
#include "CaptionTrack.h"
#include "CaptionStyle.h"
#include "SpeechRecognizer.h"
#include "SubtitleTrackRenderer.h"

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

    void setSubtitleStyle(const SubtitleStyle& style);
    SubtitleStyle subtitleStyle() const;

    bool singleWordModeEnabled() const;
    void setSingleWordModeEnabled(bool enabled);
    QString applyError() const;
    void setApplyError(const QString& message);
    void setRecognizedSegments(const QList<speech::Segment>& segments,
                               const QString& sourcePath = QString());
    QString recognizedSourcePath() const { return m_recognizedSourcePath; }

signals:
    void trackChanged(const caption::Track& track);
    void styleChanged(const caption::Style& style);
    void subtitleStyleChanged(const SubtitleStyle& style);
    void applyToTimelineRequested();

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
    void onApplyPresetClicked();
    void onApplyToTimelineClicked();

private:
    void rebuildClipTable();
    void refreshClipRow(int row);
    void updateStyleControls();
    void updatePreview();
    void syncSubtitleStyleFromCaptionStyle();

    caption::Track m_track;
    caption::Style m_style;
    SubtitleStyle m_subtitleStyle;

    QTableWidget*    m_clipTable        = nullptr;
    QTextEdit*       m_textEdit         = nullptr;
    QSpinBox*        m_startMsSpin      = nullptr;
    QSpinBox*        m_endMsSpin        = nullptr;

    QFontComboBox*   m_fontCombo        = nullptr;
    QComboBox*       m_presetCombo      = nullptr;
    QPushButton*     m_applyPresetButton = nullptr;
    QSpinBox*        m_fontSizeSpin     = nullptr;
    QCheckBox*       m_boldCheck        = nullptr;
    QCheckBox*       m_italicCheck      = nullptr;
    QPushButton*     m_textColorButton  = nullptr;
    QPushButton*     m_outlineColorButton = nullptr;
    QDoubleSpinBox*  m_outlineWidthSpin = nullptr;
    QCheckBox*       m_bgCheck          = nullptr;
    QPushButton*     m_bgColorButton    = nullptr;
    QCheckBox*       m_karaokeCheck     = nullptr;
    QPushButton*     m_karaokeColorButton = nullptr;
    QCheckBox*       m_singleWordCheck  = nullptr;
    QComboBox*       m_anchorCombo      = nullptr;

    QLabel*          m_previewLabel     = nullptr;
    QLabel*          m_applyErrorLabel  = nullptr;
    QPushButton*     m_applyToTimelineButton = nullptr;

    QPushButton*     m_addClipButton    = nullptr;
    QPushButton*     m_removeClipButton = nullptr;
    QPushButton*     m_importButton     = nullptr;
    QPushButton*     m_exportButton     = nullptr;
    QPushButton*     m_recognizeButton  = nullptr;
    QComboBox*       m_recognizerCombo  = nullptr;
    QComboBox*       m_languageCombo    = nullptr;
    QDialogButtonBox* m_buttonBox       = nullptr;

    int m_currentRow = -1;
    // Non-empty only when this dialog itself transcribed a media file.  The
    // MainWindow apply path uses it to map source-media times through V1
    // trim/speed/time-remap before building timeline captions.
    QString m_recognizedSourcePath;
};
