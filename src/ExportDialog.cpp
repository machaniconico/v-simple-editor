#include "ExportDialog.h"
#include "ExportUserPresets.h"
#include <QInputDialog>
#include <QSignalBlocker>
#include "CodecDetector.h"
#include "PremiereXmlExporter.h"
#include "YoutubeChapterGen.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QStandardItemModel>
#include <QClipboard>
#include <QGuiApplication>

namespace {
constexpr int kVideoBitrateMin = 500;
constexpr int kVideoBitrateMax = 100000;
constexpr int kAudioBitrateMin = 64;
constexpr int kAudioBitrateMax = 512;
constexpr int kCrfMin = 0;
constexpr int kCrfMax = 51;
constexpr int kAv1CrfMax = 63;

int defaultCrfFor(const QString &codec)
{
    return codec.contains("av1") ? 30 : 23;
}
}

QString ExportConfig::audioCodecForContainer(const QString &container)
{
    const QString value = container.toLower();
    if (value == "m4a") return "aac";
    if (value == "wav") return "pcm_s16le";
    if (value == "mp3") return "libmp3lame";
    return {};
}

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

    // Export Type (Video encode vs Premiere XML)
    auto *typeGroup = new QGroupBox("Export Type");
    auto *typeLayout = new QVBoxLayout(typeGroup);
    m_exportTypeCombo = new QComboBox(this);
    m_exportTypeCombo->addItem("動画ファイル (Video)", static_cast<int>(ExportType::Video));
    m_exportTypeCombo->addItem("Premiere Pro XML (FCP7)", static_cast<int>(ExportType::PremiereXml));
    typeLayout->addWidget(m_exportTypeCombo);
    m_audioOnlyCheckbox = new QCheckBox(tr("音声のみ (映像なし)"), this);
    typeLayout->addWidget(m_audioOnlyCheckbox);
    m_audioContainerCombo = new QComboBox(this);
    for (const QString &container : {QString("m4a"), QString("wav"), QString("mp3")}) {
        m_audioContainerCombo->addItem(container, container);
        if (!CodecDetector::isEncoderAvailable(ExportConfig::audioCodecForContainer(container))) {
            auto *model = qobject_cast<QStandardItemModel*>(m_audioContainerCombo->model());
            if (model) model->item(m_audioContainerCombo->count() - 1)->setEnabled(false);
        }
    }
    m_audioContainerCombo->setVisible(false);
    typeLayout->addWidget(m_audioContainerCombo);
    mainLayout->addWidget(typeGroup);

    // Preset
    auto *presetGroup = new QGroupBox("Export Preset");
    auto *presetLayout = new QVBoxLayout(presetGroup);
    m_presetCombo = new QComboBox(this);
    const auto presetList = presets();
    for (const auto &p : presetList)
        m_presetCombo->addItem(p.name);
    presetLayout->addWidget(m_presetCombo);
    auto *savePreset = new QPushButton(tr("現在の設定をプリセットに保存…"), this);
    auto *deletePreset = new QPushButton(tr("プリセットを削除"), this);
    presetLayout->addWidget(savePreset);
    presetLayout->addWidget(deletePreset);
    connect(savePreset, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("プリセットを保存"),
            tr("プリセット名:"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        for (const auto &p : presets()) {
            if (p.name == name) {
                QMessageBox::warning(this, tr("プリセットを保存"), tr("組み込みプリセットとは別の名前を指定してください"));
                return;
            }
        }
        for (const auto &p : ExportUserPresets::load()) {
            if (p.name == name && QMessageBox::question(this, tr("上書き確認"),
                tr("同名のプリセットを上書きしますか？")) != QMessageBox::Yes) return;
        }
        QString error;
        if (!ExportUserPresets::save({name, currentSettings()}, &error))
            QMessageBox::warning(this, tr("保存に失敗しました"), error);
        else reloadUserPresets(name);
    });
    connect(deletePreset, &QPushButton::clicked, this, [this]() {
        const QString name = m_presetCombo->currentData().toString();
        if (name.isEmpty()) return;
        QString error;
        if (!ExportUserPresets::remove(name, &error))
            QMessageBox::warning(this, tr("削除に失敗しました"), error);
        else { reloadUserPresets(); m_presetCombo->setCurrentIndex(0); onPresetChanged(0); }
    });
    connect(m_presetCombo, &QComboBox::currentIndexChanged, this, [this, deletePreset]() {
        deletePreset->setEnabled(!m_presetCombo->currentData().toString().isEmpty());
    });
    deletePreset->setEnabled(false);

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

    m_rateControlCombo = new QComboBox(this);
    m_rateControlCombo->addItem(tr("ビットレート"));
    m_rateControlCombo->addItem(tr("品質 (CRF)"));
    codecForm->addRow(tr("レート制御:"), m_rateControlCombo);
    m_crfSpin = new QSpinBox(this);
    m_crfSpin->setRange(kCrfMin, kCrfMax);
    m_crfSpin->setValue(defaultCrfFor(m_videoCodecCombo->currentData().toString()));
    m_crfSpin->setEnabled(false);
    codecForm->addRow(tr("品質 (CRF):"), m_crfSpin);

    m_videoBitrateSpin = new QSpinBox(this);
    m_videoBitrateSpin->setRange(kVideoBitrateMin, kVideoBitrateMax);
    m_videoBitrateSpin->setValue(10000);
    m_videoBitrateSpin->setSuffix(" kbps");
    m_videoBitrateSpin->setSingleStep(500);
    codecForm->addRow("Video Bitrate:", m_videoBitrateSpin);

    m_audioBitrateSpin = new QSpinBox(this);
    m_audioBitrateSpin->setRange(kAudioBitrateMin, kAudioBitrateMax);
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

    // Marked range
    m_markedRangeCheckbox = new QCheckBox(tr("マークした In/Out 範囲のみ書き出す"), this);
    m_markedRangeCheckbox->setChecked(false);
    mainLayout->addWidget(m_markedRangeCheckbox);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet("color: #666; font-size: 12px; padding: 4px;");
    mainLayout->addWidget(m_summaryLabel);

    // YouTube chapter generator (概要欄チャプター)
    auto *chapterGroup = new QGroupBox(tr("YouTube チャプター"), this);
    auto *chapterLayout = new QVBoxLayout(chapterGroup);
    m_chapterCheckbox = new QCheckBox(tr("YouTube 概要欄チャプターを生成"), this);
    // 空タイムラインでは生成不能なので初期は無効。setClips() で clips が
    // 入った時点で有効化する。
    m_chapterCheckbox->setEnabled(!m_clips.isEmpty());
    chapterLayout->addWidget(m_chapterCheckbox);
    m_chapterText = new QPlainTextEdit(this);
    m_chapterText->setReadOnly(true);
    m_chapterText->setPlaceholderText(tr("チェックを入れると、タイムライン上のクリップから概要欄用チャプターを生成します。"));
    m_chapterText->setMaximumHeight(140);
    m_chapterText->setVisible(false);
    chapterLayout->addWidget(m_chapterText);
    m_chapterCopyBtn = new QPushButton(tr("クリップボードにコピー"), this);
    m_chapterCopyBtn->setVisible(false);
    chapterLayout->addWidget(m_chapterCopyBtn);
    mainLayout->addWidget(chapterGroup);

    // Buttons
    auto *buttons = new QDialogButtonBox(this);
    auto *exportBtn = buttons->addButton("Export", QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    mainLayout->addWidget(buttons);

    // Connections
    connect(m_audioOnlyCheckbox, &QCheckBox::toggled, this, [this]() {
        updateAudioOnlyControls();
        const QFileInfo output(m_outputEdit->text());
        if (!m_outputEdit->text().isEmpty())
            m_outputEdit->setText(output.dir().filePath(output.completeBaseName() + "." + defaultExtension()));
    });
    connect(m_audioContainerCombo, &QComboBox::currentIndexChanged, this, [this]() {
        const QFileInfo output(m_outputEdit->text());
        if (!m_outputEdit->text().isEmpty())
            m_outputEdit->setText(output.dir().filePath(output.completeBaseName() + "." + defaultExtension()));
        updateSummary();
    });
    connect(m_exportTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ExportDialog::onExportTypeChanged);
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ExportDialog::onPresetChanged);
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::onBrowseOutput);
    connect(exportBtn, &QPushButton::clicked, this, &ExportDialog::onExport);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_videoCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        const bool av1 = m_videoCodecCombo->currentData().toString().contains("av1");
        m_crfSpin->setRange(kCrfMin, av1 ? kAv1CrfMax : kCrfMax);
        m_crfSpin->setValue(defaultCrfFor(m_videoCodecCombo->currentData().toString()));
        updateRateControlControls();
        updateSummary();
    });
    connect(m_rateControlCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateRateControlControls();
        updateSummary();
    });
    connect(m_crfSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { updateSummary(); });
    connect(m_videoBitrateSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { updateSummary(); });
    connect(m_audioBitrateSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { updateSummary(); });
    connect(m_hwEncoderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        m_config.hwEncoder = m_hwEncoderCombo->currentData().toString();
        m_config.useHardwareAccel = (m_config.hwEncoder != "none");
        updateSummary();
    });

    connect(m_chapterCheckbox, &QCheckBox::toggled, this, [this](bool on) {
        m_chapterText->setVisible(on);
        m_chapterCopyBtn->setVisible(on);
        if (on)
            regenerateChapters();
    });
    connect(m_chapterCopyBtn, &QPushButton::clicked, this, [this]() {
        if (QClipboard *cb = QGuiApplication::clipboard())
            cb->setText(m_chapterText->toPlainText());
    });

    reloadUserPresets();
    onPresetChanged(0);
    updateMarkedRangeCheckboxEnabled();
    updateSummary();
}

void ExportDialog::regenerateChapters()
{
    QList<ChapterHighlight> highlights;
    double timelinePos = 0.0;
    for (const ClipInfo &clip : m_clips) {
        timelinePos += clip.leadInSec;
        ChapterHighlight h;
        h.startSec = timelinePos;
        h.title = clip.displayName;
        highlights.append(h);

        // タイムライン上の尺は ClipInfo::effectiveDuration() が SSOT:
        // ((outPoint>0?outPoint:duration) - inPoint) / speed。
        // トリム済みクリップ (inPoint>0, outPoint==0) でも後続チャプターの
        // 開始時刻がタイムラインとずれない。
        timelinePos += clip.effectiveDuration();
    }

    if (highlights.isEmpty()) {
        m_chapterText->setPlainText(
            tr("タイムラインにクリップがありません。先に素材を配置してください。"));
        return;
    }

    m_chapterText->setPlainText(
        YoutubeChapterGen::generateChapterText(highlights, timelinePos));
}

void ExportDialog::onPresetChanged(int index)
{
    if (index < 0) return;
    const QString userName = m_presetCombo->itemData(index).toString();
    if (!userName.isEmpty()) {
        for (const auto &p : ExportUserPresets::load()) {
            if (p.name == userName) { applyPreset(p.config); return; }
        }
        return;
    }
    // Built-ins (including Custom) must not inherit hidden user-preset metadata.
    ExportConfig fresh;
    fresh.width = m_projectConfig.width;
    fresh.height = m_projectConfig.height;
    fresh.fps = m_projectConfig.fps;
    fresh.hwEncoder = m_config.hwEncoder;
    fresh.useHardwareAccel = m_config.useHardwareAccel;
    m_config = fresh;
    // Restore built-in limits before applying values, including for Custom.
    m_videoBitrateSpin->setRange(kVideoBitrateMin, kVideoBitrateMax);
    m_audioBitrateSpin->setRange(kAudioBitrateMin, kAudioBitrateMax);
    // Resolve the user preset's unset sentinel before the built-in range clamps it.
    // Keep manually entered CRF values when switching within the same codec.
    if (m_crfSpin->value() < kCrfMin)
        m_crfSpin->setValue(defaultCrfFor(m_videoCodecCombo->currentData().toString()));
    m_crfSpin->setRange(kCrfMin, m_videoCodecCombo->currentData().toString().contains("av1")
        ? kAv1CrfMax : kCrfMax);
    // Keep toggled connected so the output extension and enabled controls follow.
    m_audioOnlyCheckbox->setChecked(false);
    const auto presetList = presets();
    bool isCustom = (index >= presetList.size() - 1);

    m_videoCodecCombo->setEnabled(isCustom);
    m_audioCodecCombo->setEnabled(isCustom);
    updateRateControlControls();
    m_audioBitrateSpin->setEnabled(isCustom);

    if (!isCustom && index < presetList.size()) {
        const auto &p = presetList[index];
        ExportConfig config;
        config.videoCodec = p.videoCodec;
        config.audioCodec = p.audioCodec;
        config.videoBitrate = p.videoBitrate;
        config.audioBitrate = p.audioBitrate;
        applyPreset(config, false);
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

void ExportDialog::updateAudioOnlyControls(bool exportTypeChanged)
{
    const bool video = static_cast<ExportType>(m_exportTypeCombo->currentData().toInt()) == ExportType::Video;
    const bool audio = video && m_audioOnlyCheckbox->isChecked();
    m_audioOnlyCheckbox->setEnabled(video);
    m_audioContainerCombo->setVisible(audio);
    // Type changes retain the baseline enabled states set by onExportTypeChanged.
    // Never reapply preset values here: the user may have edited the bitrates.
    if (exportTypeChanged && !m_audioOnlyCheckbox->isChecked()) {
        updateSummary();
        return;
    }
    const auto presetList = presets();
    const int index = m_presetCombo->currentIndex();
    const bool isCustom = index >= presetList.size() - 1;
    const bool user = !m_presetCombo->currentData().toString().isEmpty();
    const bool isProRes = user ? m_config.proresProfile >= 0
        : (!isCustom && index >= 0 && presetList[index].proresProfile >= 0);
    m_presetCombo->setEnabled(video && !audio);
    m_hdrWarningLabel->setVisible(!audio && !m_sourceIsHdr
        && (user ? m_config.hdr10 : (!isCustom && index >= 0 && presetList[index].hdr10)));
    m_videoCodecCombo->setEnabled(video && !audio && isCustom);
    m_audioCodecCombo->setEnabled(video && !audio && isCustom);
    m_hwEncoderCombo->setEnabled(video && !audio && !isProRes
                                && !CodecDetector::hwAccelVideoEncoders().isEmpty());
    m_audioBitrateSpin->setEnabled(video && (audio || isCustom));
    updateRateControlControls();
    updateSummary();
}

void ExportDialog::onExportTypeChanged(int index)
{
    const auto type = static_cast<ExportType>(m_exportTypeCombo->itemData(index).toInt());
    const bool isVideo = (type == ExportType::Video);

    // Codec / preset controls are only relevant for video encode
    if (m_presetCombo) m_presetCombo->setEnabled(isVideo);
    if (m_videoCodecCombo) m_videoCodecCombo->setEnabled(isVideo);
    if (m_audioCodecCombo) m_audioCodecCombo->setEnabled(isVideo);
    updateRateControlControls();
    if (m_audioBitrateSpin) m_audioBitrateSpin->setEnabled(isVideo);
    if (m_hwEncoderCombo) m_hwEncoderCombo->setEnabled(isVideo);
    updateMarkedRangeCheckboxEnabled();
    updateAudioOnlyControls(true);
}

void ExportDialog::onBrowseOutput()
{
    const auto type = static_cast<ExportType>(m_exportTypeCombo->currentData().toInt());
    QString filter;
    if (type == ExportType::PremiereXml) {
        filter = "Premiere XML (*.xml)";
    } else {
        QString ext = defaultExtension();
        if (m_audioOnlyCheckbox->isChecked()) filter = tr("音声ファイル (*.%1)").arg(ext);
        else if (ext == "mp4") filter = "MP4 (*.mp4)";
        else if (ext == "mkv") filter = "MKV (*.mkv)";
        else if (ext == "webm") filter = "WebM (*.webm)";
        else filter = "All Files (*)";
    }

    QString path = QFileDialog::getSaveFileName(this, "Export", QString(), filter);
    if (!path.isEmpty())
        m_outputEdit->setText(path);
}

void ExportDialog::onExport()
{
    if (m_outputEdit->text().isEmpty()) {
        onBrowseOutput();
        if (m_outputEdit->text().isEmpty()) return;
    }

    const auto exportType = static_cast<ExportType>(m_exportTypeCombo->currentData().toInt());

    if (exportType == ExportType::PremiereXml) {
        // Premiere Pro XML (FCP7) export via PremiereXmlExporter
        const QString outputPath = m_outputEdit->text();

        QList<PremiereHighlight> highlights;
        for (const auto &clip : m_clips) {
            PremiereHighlight h;
            h.filePath = clip.filePath;
            h.title    = clip.displayName.isEmpty() ? QFileInfo(clip.filePath).baseName() : clip.displayName;
            h.startSec = clip.inPoint;
            h.endSec   = (clip.outPoint > 0.0) ? clip.outPoint : clip.duration;
            highlights.append(h);
        }

        PremiereVideoInfo info;
        info.width  = m_projectConfig.width;
        info.height = m_projectConfig.height;
        info.fps    = static_cast<double>(m_projectConfig.fps);

        const bool ok = PremiereXmlExporter::generateCombinedXml(
            highlights, info, outputPath,
            QStringLiteral("v-simple-editor Project"));

        if (!ok) {
            QMessageBox::warning(this, tr("Export"),
                                 tr("Premiere XML エクスポートに失敗しました"));
            return;
        }

        accept();
        return;
    }

    m_config = currentSettings();

    accept();
}

void ExportDialog::reloadUserPresets(const QString &selected)
{
    {
        const QSignalBlocker blocker(m_presetCombo);
        while (m_presetCombo->count() > presets().size())
            m_presetCombo->removeItem(m_presetCombo->count() - 1);
        const auto users = ExportUserPresets::load();
        if (!users.isEmpty()) m_presetCombo->insertSeparator(m_presetCombo->count());
        for (const auto &p : users) m_presetCombo->addItem(p.name, p.name);
        m_presetCombo->setCurrentIndex(-1);
    }
    m_presetCombo->setCurrentIndex(selected.isEmpty() ? 0 : m_presetCombo->findData(selected));
}

void ExportDialog::applyPreset(const ExportConfig &config, bool userPreset)
{
    if (!userPreset) {
        const int vcIdx = m_videoCodecCombo->findData(config.videoCodec);
        if (vcIdx >= 0) m_videoCodecCombo->setCurrentIndex(vcIdx);
        const int acIdx = m_audioCodecCombo->findData(config.audioCodec);
        if (acIdx >= 0) m_audioCodecCombo->setCurrentIndex(acIdx);
        m_videoBitrateSpin->setValue(config.videoBitrate);
        m_audioBitrateSpin->setValue(config.audioBitrate);
        return;
    }
    const QString output = m_outputEdit->text();
    const QSignalBlocker videoBlock(m_videoCodecCombo), audioBlock(m_audioCodecCombo),
        hwBlock(m_hwEncoderCombo), onlyBlock(m_audioOnlyCheckbox), containerBlock(m_audioContainerCombo),
        rateBlock(m_rateControlCombo);
    auto select = [](QComboBox *combo, const QString &value) {
        int index = combo->findData(value);
        if (index < 0) { combo->addItem(value, value); index = combo->count() - 1; }
        combo->setCurrentIndex(index);
    };
    m_config = config;
    select(m_videoCodecCombo, config.videoCodec);
    select(m_audioCodecCombo, config.audioCodec);
    select(m_hwEncoderCombo, config.hwEncoder);
    m_videoBitrateSpin->setRange(1, qMax(100000, config.videoBitrate));
    m_audioBitrateSpin->setRange(1, qMax(1536, config.audioBitrate));
    m_videoBitrateSpin->setValue(config.videoBitrate);
    m_audioBitrateSpin->setValue(config.audioBitrate);
    m_crfSpin->setRange(-1, config.videoCodec.contains("av1") ? 63 : 51);
    m_crfSpin->setValue(config.crf);
    m_rateControlCombo->setCurrentIndex(config.rateControl == ExportConfig::RateControl::Crf ? 1 : 0);
    m_audioOnlyCheckbox->setChecked(config.audioOnly);
    if (config.audioOnly) select(m_audioContainerCombo, config.container);
    m_markedRangeCheckbox->setChecked(config.exportMarkedRangeOnly);
    m_outputEdit->setText(output);
    updateAudioOnlyControls();
    updateMarkedRangeCheckboxEnabled();
    updateSummary();
}

ExportConfig ExportDialog::currentSettings() const
{
    ExportConfig c = m_config;
    const bool user = !m_presetCombo->currentData().toString().isEmpty();
    c.outputPath = m_outputEdit->text();
    c.audioOnly = m_audioOnlyCheckbox->isChecked();
    c.videoCodec = m_videoCodecCombo->currentData().toString();
    c.audioCodec = m_audioCodecCombo->currentData().toString();
    c.container = defaultExtension();
    c.videoBitrate = m_videoBitrateSpin->value();
    c.audioBitrate = m_audioBitrateSpin->value();
    c.rateControl = m_rateControlCombo->currentIndex() == 1
        ? ExportConfig::RateControl::Crf : ExportConfig::RateControl::Bitrate;
    c.crf = (user || c.rateControl == ExportConfig::RateControl::Crf) ? m_crfSpin->value() : -1;
    c.hwEncoder = m_hwEncoderCombo->currentData().toString();
    if (!user || c.hwEncoder != m_config.hwEncoder) c.useHardwareAccel = c.hwEncoder != "none";
    c.exportMarkedRangeOnly = m_markedRangeCheckbox->isChecked();
    if (!user) {
        c.width = m_projectConfig.width; c.height = m_projectConfig.height; c.fps = m_projectConfig.fps;
        const int index = m_presetCombo->currentIndex();
        const auto list = presets();
        if (index >= 0 && index < list.size()) {
            c.maxFileSizeMB = list[index].maxFileSizeMB;
            c.hdr10 = list[index].hdr10;
            c.proresProfile = list[index].proresProfile;
        }
    }
    if (c.audioOnly) c.audioCodec = ExportConfig::audioCodecForContainer(c.container);
    return c;
}

void ExportDialog::setMarkedRangeAvailable(bool hasRange)
{
    m_markedRangeStateKnown = true;
    m_markedRangeAvailable = hasRange;
    if (!hasRange && m_markedRangeCheckbox)
        m_markedRangeCheckbox->setChecked(false);
    updateMarkedRangeCheckboxEnabled();
}

void ExportDialog::updateMarkedRangeCheckboxEnabled()
{
    if (!m_markedRangeCheckbox)
        return;

    bool isVideo = true;
    if (m_exportTypeCombo) {
        const auto type =
            static_cast<ExportType>(m_exportTypeCombo->currentData().toInt());
        isVideo = (type == ExportType::Video);
    }

    const bool rangeSelectable =
        !m_markedRangeStateKnown || m_markedRangeAvailable;
    m_markedRangeCheckbox->setEnabled(isVideo && rangeSelectable);
    m_markedRangeCheckbox->setToolTip(
        rangeSelectable
            ? QString()
            : tr("有効な In/Out 範囲がマークされていません"));
}

QString ExportDialog::defaultExtension() const
{
    if (m_audioOnlyCheckbox->isChecked()) return m_audioContainerCombo->currentData().toString();
    if (!m_presetCombo->currentData().toString().isEmpty()
        && !m_config.audioOnly
        && m_videoCodecCombo->currentData().toString() == m_config.videoCodec) return m_config.container;
    QString vc = m_videoCodecCombo->currentData().toString();
    if (vc == "libvpx-vp9") return "webm";
    if (vc.startsWith("prores")) return "mov";
    if (vc == "libx265") return "mkv";
    return "mp4";
}

void ExportDialog::updateRateControlControls()
{
    const bool video = static_cast<ExportType>(m_exportTypeCombo->currentData().toInt()) == ExportType::Video
        && !m_audioOnlyCheckbox->isChecked();
    const bool supportsQuality = !m_videoCodecCombo->currentData().toString().startsWith("prores");
    if (!supportsQuality && m_rateControlCombo->currentIndex() != 0)
        m_rateControlCombo->setCurrentIndex(0);
    m_rateControlCombo->setEnabled(video && supportsQuality);
    const bool crf = m_rateControlCombo->currentIndex() == 1;
    m_videoBitrateSpin->setEnabled(video && !crf);
    m_crfSpin->setEnabled(video && crf);
}

void ExportDialog::updateSummary()
{
    if (m_audioOnlyCheckbox->isChecked()) {
        m_summaryLabel->setText(tr("音声のみ | %1 | %2 kbps | .%3")
            .arg(ExportConfig::audioCodecForContainer(defaultExtension()))
            .arg(m_audioBitrateSpin->value()).arg(defaultExtension()));
        return;
    }
    QString vc = m_videoCodecCombo->currentData().toString();
    QString codecName;
    if (vc == "libx264") codecName = "H.264";
    else if (vc == "libx265") codecName = "H.265";
    else if (vc == "libsvtav1") codecName = "AV1";
    else if (vc == "libvpx-vp9") codecName = "VP9";

    QString hwInfo = QString("HW encode: %1 (%2)")
        .arg(m_config.hwEncoder.isEmpty() ? "none" : m_config.hwEncoder)
        .arg(m_hwEncoderCombo->currentText());

    m_summaryLabel->setText(QString("%1x%2 %3fps | %4 %5 | %6 %7kbps | .%8 | %9")
        .arg(currentSettings().width).arg(currentSettings().height).arg(currentSettings().fps)
        .arg(codecName).arg(m_rateControlCombo->currentIndex() == 1
            ? QStringLiteral("CRF %1").arg(m_crfSpin->value())
            : QStringLiteral("%1kbps").arg(m_videoBitrateSpin->value()))
        .arg(m_audioCodecCombo->currentText()).arg(m_audioBitrateSpin->value())
        .arg(defaultExtension())
        .arg(hwInfo));
}
