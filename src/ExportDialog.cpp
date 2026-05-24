#include "ExportDialog.h"
#include "CodecDetector.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QStandardItemModel>

QString ExportConfig::codecDisplayName() const
{
    if (videoCodec == "libx264") return "H.264";
    if (videoCodec == "libx265") return "H.265 (HEVC)";
    if (videoCodec == "libsvtav1") return "AV1";
    if (videoCodec == "libvpx-vp9") return "VP9";
    return videoCodec;
}

QVector<ExportPreset> ExportDialog::presets()
{
    return {
        {"YouTube (1080p H.264)",       "libx264",    "aac",      "mp4",  10000, 192, 0},
        {"YouTube (1440p H.264)",       "libx264",    "aac",      "mp4",  16000, 192, 0},
        {"YouTube (4K H.264)",          "libx264",    "aac",      "mp4",  35000, 192, 0},
        {"HDR10 (HEVC Main10)",         "libx265",    "aac",      "mp4",  25000, 192, 0, true},
        {"ProRes 422 Proxy",            "prores_ks",  "pcm_s16le","mov",  45000, 1536, 0, false, 0},
        {"ProRes 422 LT",               "prores_ks",  "pcm_s16le","mov", 102000, 1536, 0, false, 1},
        {"ProRes 422",                  "prores_ks",  "pcm_s16le","mov", 147000, 1536, 0, false, 2},
        {"ProRes 422 HQ",               "prores_ks",  "pcm_s16le","mov", 220000, 1536, 0, false, 3},
        {"ProRes 4444",                 "prores_ks",  "pcm_s16le","mov", 330000, 1536, 0, false, 4},
        {"YouTube (AV1 高圧縮)",        "libsvtav1",  "aac",      "mp4",   8000, 192, 0},
        {"YouTube Shorts",              "libx264",    "aac",      "mp4",   8000, 192, 0},
        {"TikTok / Reels",              "libx264",    "aac",      "mp4",   8000, 192, 0},
        {"X / Twitter",                 "libx264",    "aac",      "mp4",  10000, 192, 512},
        {"Facebook",                    "libx264",    "aac",      "mp4",  10000, 192, 0},
        {"Twitch Clip (60fps)",         "libx264",    "aac",      "mp4",   8000, 192, 0},
        {"Discord (25MB制限)",          "libx264",    "aac",      "mp4",   2000, 128, 25},
        {"ニコニコ動画",                 "libx264",    "aac",      "mp4",   8000, 192, 0},
        {"H.265 高画質",                "libx265",    "aac",      "mkv",  12000, 192, 0},
        {"VP9 WebM",                    "libvpx-vp9", "libopus",  "webm",  8000, 128, 0},
        {"Custom",                      "libx264",    "aac",      "mp4",  10000, 192, 0},
    };
}

ExportDialog::ExportDialog(const ProjectConfig &project, QWidget *parent)
    : QDialog(parent), m_projectConfig(project)
{
    setWindowTitle("Export Video");
    setMinimumWidth(500);
    m_config.width = project.width;
    m_config.height = project.height;
    m_config.fps = project.fps;
    setupUI();
}

void ExportDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    // Preset
    auto *presetGroup = new QGroupBox("Export Preset");
    auto *presetLayout = new QVBoxLayout(presetGroup);
    m_presetCombo = new QComboBox(this);
    const auto presetList = presets();
    for (const auto &p : presetList)
        m_presetCombo->addItem(p.name);
    presetLayout->addWidget(m_presetCombo);

    m_hdrWarningLabel = new QLabel(
        "Warning: source is SDR; output will be tagged HDR10 but not actually HDR",
        this);
    m_hdrWarningLabel->setStyleSheet("color: #c97c1a; font-size: 11px; padding: 2px;");
    m_hdrWarningLabel->setWordWrap(true);
    m_hdrWarningLabel->hide();
    presetLayout->addWidget(m_hdrWarningLabel);

    mainLayout->addWidget(presetGroup);

    // Codec settings
    auto *codecGroup = new QGroupBox("Codec Settings");
    auto *codecForm = new QFormLayout(codecGroup);

    m_videoCodecCombo = new QComboBox(this);
    {
        auto videoEncoders = CodecDetector::availableVideoEncoders();
        for (const auto &enc : videoEncoders) {
            QString stars = QString("★").repeated(enc.quality) + QString("☆").repeated(5 - enc.quality);
            QString label = enc.available
                ? QString("%1 %2").arg(enc.name, stars)
                : QString("%1 (not found)").arg(enc.name);
            m_videoCodecCombo->addItem(label, enc.ffmpegName);
            if (!enc.available) {
                auto *model = qobject_cast<QStandardItemModel*>(m_videoCodecCombo->model());
                if (model) model->item(m_videoCodecCombo->count() - 1)->setEnabled(false);
            }
        }
    }
    codecForm->addRow("Video Codec:", m_videoCodecCombo);

    m_audioCodecCombo = new QComboBox(this);
    {
        auto audioEncoders = CodecDetector::availableAudioEncoders();
        QString bestAAC = CodecDetector::bestAACEncoder();
        int bestIdx = 0;
        for (int i = 0; i < audioEncoders.size(); ++i) {
            const auto &enc = audioEncoders[i];
            QString stars = QString("★").repeated(enc.quality) + QString("☆").repeated(5 - enc.quality);
            QString label = enc.available
                ? QString("%1 %2").arg(enc.name, stars)
                : QString("%1 (not found)").arg(enc.name);
            m_audioCodecCombo->addItem(label, enc.ffmpegName);
            if (!enc.available) {
                auto *model = qobject_cast<QStandardItemModel*>(m_audioCodecCombo->model());
                if (model) model->item(m_audioCodecCombo->count() - 1)->setEnabled(false);
            }
            if (enc.ffmpegName == bestAAC) bestIdx = i;
        }
        m_audioCodecCombo->setCurrentIndex(bestIdx);
    }
    codecForm->addRow("Audio Codec:", m_audioCodecCombo);

    m_videoBitrateSpin = new QSpinBox(this);
    m_videoBitrateSpin->setRange(500, 100000);
    m_videoBitrateSpin->setValue(10000);
    m_videoBitrateSpin->setSuffix(" kbps");
    m_videoBitrateSpin->setSingleStep(500);
    codecForm->addRow("Video Bitrate:", m_videoBitrateSpin);

    m_audioBitrateSpin = new QSpinBox(this);
    m_audioBitrateSpin->setRange(64, 512);
    m_audioBitrateSpin->setValue(192);
    m_audioBitrateSpin->setSuffix(" kbps");
    codecForm->addRow("Audio Bitrate:", m_audioBitrateSpin);

    // Hardware encoder combo
    m_hwEncoderCombo = new QComboBox(this);
    {
        // Get available HW encoders to determine which vendors are present
        auto hwEncoders = CodecDetector::hwAccelVideoEncoders();
        auto hasVendor = [&hwEncoders](const QString &vendor) {
            for (const auto &enc : hwEncoders)
                if (enc.ffmpegName.contains(vendor)) return true;
            return false;
        };

        const bool hasNvenc = hasVendor("nvenc");
        const bool hasQsv   = hasVendor("qsv");
        const bool hasAmf   = hasVendor("amf");
        const bool anyHw    = hasNvenc || hasQsv || hasAmf;

        // Add items: index 0=none, 1=auto, 2=nvenc, 3=qsv, 4=amf
        m_hwEncoderCombo->addItem("ソフトウェア (libx264/x265)", QVariant(QString("none")));
        m_hwEncoderCombo->addItem("自動 (利用可能なら GPU)",     QVariant(QString("auto")));
        m_hwEncoderCombo->addItem("NVIDIA NVENC",               QVariant(QString("nvenc")));
        m_hwEncoderCombo->addItem("Intel QuickSync",            QVariant(QString("qsv")));
        m_hwEncoderCombo->addItem("AMD AMF",                    QVariant(QString("amf")));

        auto *hwModel = qobject_cast<QStandardItemModel*>(m_hwEncoderCombo->model());
        if (hwModel) {
            if (!hasNvenc) {
                hwModel->item(2)->setEnabled(false);
                hwModel->item(2)->setToolTip("NVENC not detected");
            }
            if (!hasQsv) {
                hwModel->item(3)->setEnabled(false);
                hwModel->item(3)->setToolTip("QuickSync not detected");
            }
            if (!hasAmf) {
                hwModel->item(4)->setEnabled(false);
                hwModel->item(4)->setToolTip("AMD AMF not detected");
            }
        }

        if (anyHw) {
            m_hwEncoderCombo->setCurrentIndex(1); // "auto"
            m_config.hwEncoder = "auto";
            m_config.useHardwareAccel = true;
        } else {
            m_hwEncoderCombo->setCurrentIndex(0); // "none"
            m_hwEncoderCombo->setEnabled(false);
            m_config.hwEncoder = "none";
            m_config.useHardwareAccel = false;
        }
    }
    codecForm->addRow("ハードウェアエンコード:", m_hwEncoderCombo);

    mainLayout->addWidget(codecGroup);

    // Output file
    auto *outputGroup = new QGroupBox("Output");
    auto *outputLayout = new QHBoxLayout(outputGroup);
    m_outputEdit = new QLineEdit(this);
    m_outputEdit->setPlaceholderText("Select output file...");
    auto *browseBtn = new QPushButton("Browse...", this);
    outputLayout->addWidget(m_outputEdit);
    outputLayout->addWidget(browseBtn);
    mainLayout->addWidget(outputGroup);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet("color: #666; font-size: 12px; padding: 4px;");
    mainLayout->addWidget(m_summaryLabel);

    // Buttons
    auto *buttons = new QDialogButtonBox(this);
    auto *exportBtn = buttons->addButton("Export", QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    mainLayout->addWidget(buttons);

    // Connections
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ExportDialog::onPresetChanged);
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::onBrowseOutput);
    connect(exportBtn, &QPushButton::clicked, this, &ExportDialog::onExport);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_videoCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { updateSummary(); });
    connect(m_videoBitrateSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { updateSummary(); });
    connect(m_audioBitrateSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { updateSummary(); });
    connect(m_hwEncoderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        m_config.hwEncoder = m_hwEncoderCombo->currentData().toString();
        m_config.useHardwareAccel = (m_config.hwEncoder != "none");
        updateSummary();
    });

    onPresetChanged(0);
    updateSummary();
}

void ExportDialog::onPresetChanged(int index)
{
    const auto presetList = presets();
    bool isCustom = (index >= presetList.size() - 1);

    m_videoCodecCombo->setEnabled(isCustom);
    m_audioCodecCombo->setEnabled(isCustom);
    m_videoBitrateSpin->setEnabled(isCustom);
    m_audioBitrateSpin->setEnabled(isCustom);

    if (!isCustom && index < presetList.size()) {
        const auto &p = presetList[index];
        int vcIdx = m_videoCodecCombo->findData(p.videoCodec);
        if (vcIdx >= 0) m_videoCodecCombo->setCurrentIndex(vcIdx);
        int acIdx = m_audioCodecCombo->findData(p.audioCodec);
        if (acIdx >= 0) m_audioCodecCombo->setCurrentIndex(acIdx);
        m_videoBitrateSpin->setValue(p.videoBitrate);
        m_audioBitrateSpin->setValue(p.audioBitrate);
    }

    if (m_hdrWarningLabel) {
        const bool presetIsHdr = (!isCustom
                                  && index < presetList.size()
                                  && presetList[index].hdr10);
        m_hdrWarningLabel->setVisible(presetIsHdr && !m_sourceIsHdr);
    }

    // ProRes does not support HW encoding — disable combo and force "none"
    const bool isProRes = (!isCustom
                           && index < presetList.size()
                           && presetList[index].proresProfile >= 0);
    if (isProRes) {
        m_hwEncoderCombo->setEnabled(false);
        m_config.hwEncoder = "none";
        m_config.useHardwareAccel = false;
    } else {
        // Re-enable only if there is at least one HW encoder available
        auto hwEncoders = CodecDetector::hwAccelVideoEncoders();
        m_hwEncoderCombo->setEnabled(!hwEncoders.isEmpty());
        m_config.hwEncoder = m_hwEncoderCombo->currentData().toString();
        m_config.useHardwareAccel = (m_config.hwEncoder != "none");
    }

    updateSummary();
}

void ExportDialog::setSourceIsHdr(bool hdr)
{
    m_sourceIsHdr = hdr;
    if (hdr) {
        const auto presetList = presets();
        for (int i = 0; i < presetList.size(); ++i) {
            if (presetList[i].hdr10) {
                m_presetCombo->setCurrentIndex(i);
                break;
            }
        }
    } else {
        onPresetChanged(m_presetCombo->currentIndex());
    }
}

void ExportDialog::onBrowseOutput()
{
    QString ext = defaultExtension();
    QString filter;
    if (ext == "mp4") filter = "MP4 (*.mp4)";
    else if (ext == "mkv") filter = "MKV (*.mkv)";
    else if (ext == "webm") filter = "WebM (*.webm)";
    else filter = "All Files (*)";

    QString path = QFileDialog::getSaveFileName(this, "Export Video", QString(), filter);
    if (!path.isEmpty())
        m_outputEdit->setText(path);
}

void ExportDialog::onExport()
{
    if (m_outputEdit->text().isEmpty()) {
        onBrowseOutput();
        if (m_outputEdit->text().isEmpty()) return;
    }

    m_config.outputPath = m_outputEdit->text();
    m_config.videoCodec = m_videoCodecCombo->currentData().toString();
    m_config.audioCodec = m_audioCodecCombo->currentData().toString();
    m_config.container = defaultExtension();
    m_config.videoBitrate = m_videoBitrateSpin->value();
    m_config.audioBitrate = m_audioBitrateSpin->value();
    m_config.hwEncoder = m_hwEncoderCombo->currentData().toString();
    m_config.useHardwareAccel = (m_config.hwEncoder != "none");
    m_config.width = m_projectConfig.width;
    m_config.height = m_projectConfig.height;
    m_config.fps = m_projectConfig.fps;

    const auto presetList = presets();
    int idx = m_presetCombo->currentIndex();
    if (idx < presetList.size()) {
        m_config.maxFileSizeMB = presetList[idx].maxFileSizeMB;
        m_config.hdr10 = presetList[idx].hdr10;
        m_config.proresProfile = presetList[idx].proresProfile;
    } else {
        m_config.hdr10 = false;
        m_config.proresProfile = -1;
    }

    accept();
}

QString ExportDialog::defaultExtension() const
{
    QString vc = m_videoCodecCombo->currentData().toString();
    if (vc == "libvpx-vp9") return "webm";
    if (vc.startsWith("prores")) return "mov";
    if (vc == "libx265") return "mkv";
    return "mp4";
}

void ExportDialog::updateSummary()
{
    QString vc = m_videoCodecCombo->currentData().toString();
    QString codecName;
    if (vc == "libx264") codecName = "H.264";
    else if (vc == "libx265") codecName = "H.265";
    else if (vc == "libsvtav1") codecName = "AV1";
    else if (vc == "libvpx-vp9") codecName = "VP9";

    QString hwInfo = QString("HW encode: %1 (%2)")
        .arg(m_config.hwEncoder.isEmpty() ? "none" : m_config.hwEncoder)
        .arg(m_hwEncoderCombo->currentText());

    m_summaryLabel->setText(QString("%1x%2 %3fps | %4 %5kbps | %6 %7kbps | .%8 | %9")
        .arg(m_projectConfig.width).arg(m_projectConfig.height).arg(m_projectConfig.fps)
        .arg(codecName).arg(m_videoBitrateSpin->value())
        .arg(m_audioCodecCombo->currentText()).arg(m_audioBitrateSpin->value())
        .arg(defaultExtension())
        .arg(hwInfo));
}
