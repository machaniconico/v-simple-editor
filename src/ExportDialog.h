#pragma once

#include <QDialog>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QPlainTextEdit>
#include "ProjectSettings.h"
#include "Timeline.h"

enum class ExportType {
    Video,       // 既存 video encode (preset/codec 設定を使う)
    PremiereXml, // Premiere Pro XML (FCP7) — PremiereXmlExporter を呼出
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
    HDRSettings hdrSettings; // extended HDR metadata

    QString codecDisplayName() const;
};

class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExportDialog(const ProjectConfig &project, QWidget *parent = nullptr);

    ExportConfig config() const { return m_config; }
    void setSourceIsHdr(bool hdr);

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
    void updateSummary();
    void regenerateChapters();
    QString defaultExtension() const;

    ExportConfig m_config;
    ProjectConfig m_projectConfig;
    QVector<ClipInfo> m_clips;

    QComboBox *m_exportTypeCombo = nullptr;
    QComboBox *m_presetCombo;
    QComboBox *m_videoCodecCombo;
    QComboBox *m_audioCodecCombo;
    QComboBox *m_hwEncoderCombo;
    QSpinBox *m_videoBitrateSpin;
    QSpinBox *m_audioBitrateSpin;
    QLineEdit *m_outputEdit;
    QLabel *m_summaryLabel;
    QLabel *m_hdrWarningLabel = nullptr;
    bool m_sourceIsHdr = false;

    QCheckBox *m_chapterCheckbox = nullptr;
    QPlainTextEdit *m_chapterText = nullptr;
    QPushButton *m_chapterCopyBtn = nullptr;
};
