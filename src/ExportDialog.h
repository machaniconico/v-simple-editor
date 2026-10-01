#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QPlainTextEdit>
#include "ProjectSettings.h"
#include "SubtitleIO.h"
#include "Timeline.h"

enum class ExportType {
    Video,       // 既存 video encode (preset/codec 設定を使う)
    PremiereXml, // Premiere Pro XML (FCP7) — PremiereXmlExporter を呼出
    ImageSequence, // PNG / TIFF 連番静止画
};

struct ExportPreset {
    QString name;
    QString videoCodec;  // libx264, libx265, libsvtav1, libvpx-vp9
    QString audioCodec;  // aac, libopus, libmp3lame
    QString container;   // mp4, mkv, webm
    int videoBitrate;    // kbps
    int audioBitrate;    // kbps
    int maxFileSizeMB;   // 0 = no limit
    bool hdr10 = false;  // when true: 10-bit pipeline + BT.2020/PQ metadata
    int proresProfile = -1;  // -1 = not ProRes; 0=Proxy,1=LT,2=SQ,3=HQ,4=4444,5=4444XQ
};

struct HDRSettings {
    QString mode = "sdr";                    // "sdr" | "hdr10" | "hlg"
    double masterDisplayLuminanceMin = 0.01; // cd/m²
    double masterDisplayLuminanceMax = 1000.0;
    int maxCll = 1000;                       // cd/m²
    int maxFall = 400;
    QString previewToneMap = "reinhard";     // "reinhard" | "hable" | "none"
};

struct ExportConfig {
    enum class RateControl { Bitrate, Crf };
    RateControl rateControl = RateControl::Bitrate;
    subtitle::ExportMode subtitleMode = subtitle::ExportMode::BurnIn;
    void appendSubtitleSettings(QJsonObject &json) const;
    int crf = -1; // -1: codec default (23, or 30 for AV1)
    QString title, author, copyright, comment; // Empty: omit container tags.
    void appendMetadataSettings(QJsonObject &json) const;
    QString outputPath;
    QString videoCodec = "libx264";
    QString audioCodec = "aac";
    QString container = "mp4";
    int videoBitrate = 10000;
    int audioBitrate = 192;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    bool useHardwareAccel = false;
    QString hwEncoder;       // "", "auto" → auto-detect; "none" → SW only; "nvenc"/"qsv"/"amf" → vendor explicit
    int maxFileSizeMB = 0;
    bool hdr10 = false;  // 10-bit BT.2020/PQ output when true (preserved for backward compat)
    int proresProfile = -1;  // -1 = not ProRes; 0..5 = Proxy/LT/SQ/HQ/4444/4444XQ
    bool keepAlpha = false;
    bool imageSequence = false;
    QString sequenceFolder;
    QString sequencePattern = "name_%06d.png";
    int sequenceDigits = 6;
    int sequenceStart = 0;
    void appendSequenceSettings(QJsonObject &json) const;
    HDRSettings hdrSettings; // extended HDR metadata
    bool exportMarkedRangeOnly = false;
    bool autoLoudness = false;
    double targetLufs = -14.0;
    void appendLoudnessSettings(QJsonObject &json) const;
    bool audioOnly = false;
    static QString audioCodecForContainer(const QString &container);

    QString codecDisplayName() const;
};

namespace exportcfg {
// Shared ExportConfig -> RenderJob::exportConfig mapping. Values that depend on
// the live timeline or MainWindow are added by buildExportRenderJob.
QJsonObject toJobJson(const ExportConfig &config);
}

class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExportDialog(const ProjectConfig &project, QWidget *parent = nullptr);

    ExportConfig config() const { return m_config; }
    void setSourceIsHdr(bool hdr);
    void setMarkedRangeAvailable(bool hasRange);

    // clips を渡すと Premiere XML export / YouTube チャプター生成時に
    // highlight list を構築する。clips が空ならチャプター生成 checkbox を
    // 無効化する (空タイムラインでは生成不能のため)。
    void setClips(const QVector<ClipInfo> &clips) {
        m_clips = clips;
        if (m_chapterCheckbox)
            m_chapterCheckbox->setEnabled(!m_clips.isEmpty());
    }

    static QVector<ExportPreset> presets();

private slots:
    void onPresetChanged(int index);
    void onExportTypeChanged(int index);
    void onBrowseOutput();
    void onExport();

private:
    void setupUI();
    void ensureSequenceSettings();
    void reloadUserPresets(const QString &selected = {});
    void applyPreset(const ExportConfig &config, bool userPreset = true);
    ExportConfig currentSettings() const;
    void updateSummary();
    void updateAudioOnlyControls(bool exportTypeChanged = false);
    void updateRateControlControls();
    void updateMarkedRangeCheckboxEnabled();
    void regenerateChapters();
    QString defaultExtension() const;

    ExportConfig m_config;
    ProjectConfig m_projectConfig;
    QVector<ClipInfo> m_clips;

    QWidget *m_sequenceSettings = nullptr;
    QComboBox *m_sequenceFormat = nullptr;
    QLineEdit *m_sequencePattern = nullptr;
    QSpinBox *m_sequenceDigits = nullptr;
    QSpinBox *m_sequenceStart = nullptr;
    QString m_videoOutputBeforeSequence; // video file path, restored when leaving sequence mode
    QString m_sequenceOutputBeforeVideo; // sequence folder, restored when re-entering sequence mode
    ExportType m_lastExportType = ExportType::Video;
    QCheckBox *m_keepAlphaCheckbox = nullptr;
    QCheckBox *m_audioOnlyCheckbox = nullptr;
    QCheckBox *m_autoLoudness = nullptr;
    QComboBox *m_loudnessPreset = nullptr;
    QDoubleSpinBox *m_targetLufs = nullptr;
    QComboBox *m_audioContainerCombo = nullptr;
    QComboBox *m_exportTypeCombo = nullptr;
    QComboBox *m_presetCombo;
    QComboBox *m_videoCodecCombo;
    QComboBox *m_audioCodecCombo;
    QComboBox *m_hwEncoderCombo;
    QComboBox *m_subtitleModeCombo = nullptr;
    QComboBox *m_rateControlCombo = nullptr;
    QSpinBox *m_crfSpin = nullptr;
    QSpinBox *m_videoBitrateSpin;
    QSpinBox *m_audioBitrateSpin;
    QLineEdit *m_outputEdit;
    QLineEdit *m_metadataTitle = nullptr;
    QLineEdit *m_metadataAuthor = nullptr;
    QLineEdit *m_metadataCopyright = nullptr;
    QLineEdit *m_metadataComment = nullptr;
    QLabel *m_summaryLabel;
    QLabel *m_hdrWarningLabel = nullptr;
    QCheckBox *m_markedRangeCheckbox = nullptr;
    bool m_markedRangeStateKnown = false;
    bool m_markedRangeAvailable = false;
    bool m_sourceIsHdr = false;

    QCheckBox *m_chapterCheckbox = nullptr;
    QPlainTextEdit *m_chapterText = nullptr;
    QPushButton *m_chapterCopyBtn = nullptr;
};
