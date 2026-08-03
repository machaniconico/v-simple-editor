#include "VoiceIsolationDialog.h"

#include "libavcore/AudioExtract.h"

#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QDir>
#include <QEventLoop>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QtGlobal>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

QDoubleSpinBox *makeDoubleSpin(QWidget *parent,
                               double minimum,
                               double maximum,
                               double step,
                               int decimals,
                               double value)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setValue(value);
    spin->setKeyboardTracking(false);
    return spin;
}

void bindSliderAndSpin(QSlider *slider,
                       QDoubleSpinBox *spin,
                       QObject *owner,
                       std::function<void()> changed)
{
    QObject::connect(slider, &QSlider::valueChanged, owner,
                     [spin, changed](int value) {
        const QSignalBlocker blocker(spin);
        spin->setValue(value / 100.0);
        changed();
    });
    QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), owner,
                     [slider, changed](double value) {
        const QSignalBlocker blocker(slider);
        slider->setValue(qRound(value * 100.0));
        changed();
    });
}

QByteArray toPcm16(const QVector<float> &samples)
{
    QByteArray pcm;
    if (samples.size() > std::numeric_limits<int>::max() / 2)
        return pcm;
    pcm.resize(samples.size() * 2);
    for (int i = 0; i < samples.size(); ++i) {
        double value = std::isfinite(samples[i])
            ? static_cast<double>(samples[i]) : 0.0;
        value = std::clamp(value, -1.0, 1.0);
        const qint16 sample = static_cast<qint16>(std::lround(value * 32767.0));
        pcm[i * 2] = static_cast<char>(sample & 0xff);
        pcm[i * 2 + 1] = static_cast<char>((sample >> 8) & 0xff);
    }
    return pcm;
}

} // namespace

VoiceIsolationDialog::VoiceIsolationDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("音声分離（スピーチ強調）"));
    setModal(true);
    resize(720, 680);
    setMinimumWidth(640);

    m_sourceLabel = new QLabel(tr("音声ソース未設定"), this);
    m_sourceLabel->setWordWrap(true);
    m_sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sourceLabel->setMinimumHeight(34);

    m_rangeStartSpin = makeDoubleSpin(this, 0.0, 0.0, 0.1, 3, 0.0);
    m_rangeEndSpin = makeDoubleSpin(this, 0.0, 0.0, 0.1, 3, 0.0);
    m_rangeStartSpin->setSuffix(tr(" 秒"));
    m_rangeEndSpin->setSuffix(tr(" 秒"));
    connect(m_rangeStartSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::onRangeChanged);
    connect(m_rangeEndSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::onRangeChanged);

    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(tr("音声のみ"), static_cast<int>(voiceiso::OutputMode::VoiceOnly));
    m_modeCombo->addItem(tr("背景のみ"), static_cast<int>(voiceiso::OutputMode::BackgroundOnly));
    m_modeCombo->addItem(tr("ミックス"), static_cast<int>(voiceiso::OutputMode::Mix));
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);

    m_strengthSlider = new QSlider(Qt::Horizontal, this);
    m_strengthSlider->setRange(0, 100);
    m_strengthSlider->setSingleStep(5);
    m_strengthSlider->setValue(80);
    m_strengthSpin = makeDoubleSpin(this, 0.0, 1.0, 0.05, 2, 0.8);
    bindSliderAndSpin(m_strengthSlider, m_strengthSpin, this,
                      [this]() { invalidateAnalysis(); });

    m_voiceGainSpin = makeDoubleSpin(this, -60.0, 24.0, 0.5, 1, 0.0);
    m_voiceGainSpin->setSuffix(tr(" dB"));
    m_backgroundGainSpin = makeDoubleSpin(this, -60.0, 24.0, 0.5, 1, -18.0);
    m_backgroundGainSpin->setSuffix(tr(" dB"));
    m_lowHzSpin = makeDoubleSpin(this, 0.0, 24000.0, 10.0, 0, 80.0);
    m_lowHzSpin->setSuffix(tr(" Hz"));
    m_highHzSpin = makeDoubleSpin(this, 0.0, 24000.0, 100.0, 0, 8000.0);
    m_highHzSpin->setSuffix(tr(" Hz"));
    m_noiseLearnSpin = makeDoubleSpin(this, 0.0, 5.0, 0.1, 1, 0.5);
    m_noiseLearnSpin->setSuffix(tr(" 秒"));
    m_adaptiveNoiseCheck = new QCheckBox(tr("適応ノイズ床を使用"), this);
    m_adaptiveNoiseCheck->setChecked(true);
    m_harmonicSlider = new QSlider(Qt::Horizontal, this);
    m_harmonicSlider->setRange(0, 100);
    m_harmonicSlider->setSingleStep(5);
    m_harmonicSlider->setValue(50);
    m_harmonicSpin = makeDoubleSpin(this, 0.0, 1.0, 0.05, 2, 0.5);
    bindSliderAndSpin(m_harmonicSlider, m_harmonicSpin, this,
                      [this]() { invalidateAnalysis(); });
    m_smoothingSlider = new QSlider(Qt::Horizontal, this);
    m_smoothingSlider->setRange(0, 100);
    m_smoothingSlider->setSingleStep(5);
    m_smoothingSlider->setValue(70);
    m_smoothingSpin = makeDoubleSpin(this, 0.0, 1.0, 0.05, 2, 0.7);
    bindSliderAndSpin(m_smoothingSlider, m_smoothingSpin, this,
                      [this]() { invalidateAnalysis(); });

    auto *sourceGroup = new QGroupBox(tr("対象クリップと範囲"), this);
    auto *sourceForm = new QFormLayout(sourceGroup);
    sourceForm->addRow(tr("クリップ:"), m_sourceLabel);
    auto *rangeRow = new QHBoxLayout;
    rangeRow->addWidget(m_rangeStartSpin);
    rangeRow->addWidget(new QLabel(tr("から"), sourceGroup));
    rangeRow->addWidget(m_rangeEndSpin);
    rangeRow->addWidget(new QLabel(tr("まで"), sourceGroup));
    rangeRow->addStretch(1);
    sourceForm->addRow(tr("処理範囲:"), rangeRow);

    auto *separationGroup = new QGroupBox(tr("分離"), this);
    auto *separationForm = new QFormLayout(separationGroup);
    auto *strengthRow = new QHBoxLayout;
    strengthRow->addWidget(m_strengthSlider, 1);
    strengthRow->addWidget(m_strengthSpin);
    separationForm->addRow(tr("分離強度:"), strengthRow);
    separationForm->addRow(tr("出力モード:"), m_modeCombo);
    separationForm->addRow(tr("音声ゲイン:"), m_voiceGainSpin);
    separationForm->addRow(tr("背景ゲイン:"), m_backgroundGainSpin);
    auto *bandRow = new QHBoxLayout;
    bandRow->addWidget(m_lowHzSpin);
    bandRow->addWidget(new QLabel(tr("から"), separationGroup));
    bandRow->addWidget(m_highHzSpin);
    bandRow->addStretch(1);
    separationForm->addRow(tr("音声帯域:"), bandRow);

    auto *analysisGroup = new QGroupBox(tr("解析と音声らしさ"), this);
    auto *analysisForm = new QFormLayout(analysisGroup);
    analysisForm->addRow(tr("ノイズ学習:"), m_noiseLearnSpin);
    analysisForm->addRow(QString(), m_adaptiveNoiseCheck);
    auto *harmonicRow = new QHBoxLayout;
    harmonicRow->addWidget(m_harmonicSlider, 1);
    harmonicRow->addWidget(m_harmonicSpin);
    analysisForm->addRow(tr("倍音構造:"), harmonicRow);
    auto *smoothingRow = new QHBoxLayout;
    smoothingRow->addWidget(m_smoothingSlider, 1);
    smoothingRow->addWidget(m_smoothingSpin);
    analysisForm->addRow(tr("時間平滑化:"), smoothingRow);

    m_ratioLabel = new QLabel(tr("推定音声比率: 未解析"), this);
    m_ratioLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel = new QLabel(tr("対象を設定すると解析できます。"), this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setMinimumHeight(32);

    m_analyzeButton = new QPushButton(tr("解析"), this);
    m_previewButton = new QPushButton(tr("プレビュー再生"), this);
    m_applyButton = new QPushButton(tr("適用"), this);
    m_applyButton->setDefault(true);
    auto *closeButton = new QPushButton(tr("閉じる"), this);
    connect(m_analyzeButton, &QPushButton::clicked,
            this, &VoiceIsolationDialog::onAnalyzeClicked);
    connect(m_previewButton, &QPushButton::clicked,
            this, &VoiceIsolationDialog::onPreviewClicked);
    connect(m_applyButton, &QPushButton::clicked,
            this, &VoiceIsolationDialog::onApplyClicked);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    auto *actionRow = new QHBoxLayout;
    actionRow->addWidget(m_analyzeButton);
    actionRow->addWidget(m_previewButton);
    actionRow->addStretch(1);
    actionRow->addWidget(m_applyButton);
    actionRow->addWidget(closeButton);

    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(1.0);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOutput);
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &VoiceIsolationDialog::onPlayerStateChanged);
    m_previewFile.setAutoRemove(true);
    m_previewFile.setFileTemplate(
        QDir::tempPath() + QStringLiteral("/v-simple-editor-voice-XXXXXX.wav"));

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->addWidget(sourceGroup);
    layout->addWidget(separationGroup);
    layout->addWidget(analysisGroup);
    layout->addWidget(m_ratioLabel);
    layout->addWidget(m_statusLabel);
    layout->addLayout(actionRow);

    connect(m_voiceGainSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_backgroundGainSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_lowHzSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_highHzSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_noiseLearnSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VoiceIsolationDialog::invalidateAnalysis);
    connect(m_adaptiveNoiseCheck, &QCheckBox::toggled,
            this, &VoiceIsolationDialog::invalidateAnalysis);

    updateActionState();
}

void VoiceIsolationDialog::setAudioSource(const QString &label,
                                          const QVector<float> &samples,
                                          int sampleRate)
{
    if (m_player)
        m_player->stop();
    m_samples = samples;
    m_processedSamples = samples;
    m_sampleRate = sampleRate;
    m_hasAnalysis = false;
    const double duration = (sampleRate > 0 && !samples.isEmpty())
        ? samples.size() / static_cast<double>(sampleRate) : 0.0;
    m_sourceLabel->setText(label.isEmpty() ? tr("音声ソース未設定") : label);
    m_rangeStartSpin->setRange(0.0, duration);
    m_rangeEndSpin->setRange(0.0, duration);
    {
        const QSignalBlocker startBlocker(m_rangeStartSpin);
        const QSignalBlocker endBlocker(m_rangeEndSpin);
        m_rangeStartSpin->setValue(0.0);
        m_rangeEndSpin->setValue(duration);
    }
    m_ratioLabel->setText(tr("推定音声比率: 未解析"));
    m_statusLabel->setText(samples.isEmpty() || sampleRate <= 0
                               ? tr("音声サンプルを読み込めませんでした。")
                               : tr("解析またはプレビュー再生を実行できます。"));
    updateActionState();
}

void VoiceIsolationDialog::setInitialRange(double startSeconds,
                                           double endSeconds)
{
    if (m_samples.isEmpty() || m_sampleRate <= 0)
        return;
    const double duration = m_samples.size() / static_cast<double>(m_sampleRate);
    const double start = std::clamp(startSeconds, 0.0, duration);
    const double end = std::clamp(endSeconds, start, duration);
    const QSignalBlocker startBlocker(m_rangeStartSpin);
    const QSignalBlocker endBlocker(m_rangeEndSpin);
    m_rangeStartSpin->setValue(start);
    m_rangeEndSpin->setValue(end);
}

double VoiceIsolationDialog::rangeStartSeconds() const
{
    return m_rangeStartSpin ? m_rangeStartSpin->value() : 0.0;
}

double VoiceIsolationDialog::rangeEndSeconds() const
{
    return m_rangeEndSpin ? m_rangeEndSpin->value() : 0.0;
}

voiceiso::VoiceIsolationParams VoiceIsolationDialog::paramsFromUi() const
{
    voiceiso::VoiceIsolationParams params;
    params.mode = static_cast<voiceiso::OutputMode>(
        m_modeCombo->currentData().toInt());
    params.strength = m_strengthSpin->value();
    params.voiceGainDb = m_voiceGainSpin->value();
    params.backgroundGainDb = m_backgroundGainSpin->value();
    params.voiceLowHz = m_lowHzSpin->value();
    params.voiceHighHz = m_highHzSpin->value();
    params.noiseLearnSeconds = m_noiseLearnSpin->value();
    params.adaptiveNoise = m_adaptiveNoiseCheck->isChecked();
    params.harmonicWeight = m_harmonicSpin->value();
    params.smoothingFactor = m_smoothingSpin->value();
    return params;
}

bool VoiceIsolationDialog::processCurrentRange()
{
    if (m_samples.isEmpty() || m_sampleRate <= 0) {
        m_statusLabel->setText(tr("有効な音声サンプルがありません。"));
        return false;
    }
    const qint64 first64 = qBound<qint64>(
        0, qRound64(rangeStartSeconds() * m_sampleRate), m_samples.size() - 1);
    const qint64 end64 = qBound<qint64>(
        first64 + 1, qRound64(rangeEndSeconds() * m_sampleRate), m_samples.size());
    if (end64 <= first64) {
        m_statusLabel->setText(tr("処理範囲の終点は始点より後に設定してください。"));
        return false;
    }

    setBusy(true);
    // Give the user-visible busy state one event-loop turn before the
    // synchronous DSP work starts. User input remains blocked by setBusy().
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QVector<float> range;
    range.reserve(static_cast<qsizetype>(end64 - first64));
    for (qint64 i = first64; i < end64; ++i)
        range.append(m_samples[static_cast<int>(i)]);

    const voiceiso::VoiceIsolationParams params = paramsFromUi();
    const voiceiso::VoiceIsolationResult result = voiceiso::isolate(
        range, m_sampleRate, params);
    const int rangeLength = static_cast<int>(end64 - first64);
    if (static_cast<int>(result.output.size()) != rangeLength) {
        m_statusLabel->setText(tr("音声分離に必要なメモリを確保できませんでした。範囲を短くして再試行してください。"));
        setBusy(false);
        return false;
    }
    const QVector<double> &floor = result.noiseFloor;
    m_processedSamples = m_samples;
    for (int i = 0; i < static_cast<int>(result.output.size()); ++i)
        m_processedSamples[static_cast<int>(first64) + i] = result.output[i];
    m_hasAnalysis = true;

    double floorMean = 0.0;
    for (double value : floor)
        floorMean += value;
    if (!floor.isEmpty())
        floorMean /= floor.size();
    const double floorDb = 10.0 * std::log10(std::max(1.0e-12, floorMean));
    m_ratioLabel->setText(tr("推定音声比率: %1% | ノイズ床: %2 dBFS")
                              .arg(result.estimatedVoiceRatio * 100.0, 0, 'f', 1)
                              .arg(floorDb, 0, 'f', 1));
    m_statusLabel->setText(tr("解析完了。範囲内の処理結果をプレビューできます。"));
    setBusy(false);
    return true;
}

bool VoiceIsolationDialog::writePreviewFile(const QVector<float> &samples,
                                            QString *error)
{
    if (m_sampleRate <= 0 || samples.isEmpty()) {
        if (error)
            *error = tr("プレビュー対象が空です。");
        return false;
    }
    if (m_previewFile.fileName().isEmpty()) {
        if (!m_previewFile.open()) {
            if (error)
                *error = m_previewFile.errorString();
            return false;
        }
        m_previewFile.close();
    }
    const QByteArray pcm = toPcm16(samples);
    if (pcm.isEmpty()) {
        if (error)
            *error = tr("プレビュー用 PCM の作成に失敗しました。");
        return false;
    }
    return libavcore::writePcm16AsWav(
        m_previewFile.fileName(), pcm, m_sampleRate, 1, error);
}

void VoiceIsolationDialog::setBusy(bool busy)
{
    m_busy = busy;
    updateActionState();
    if (m_busy)
        m_statusLabel->setText(tr("解析中..."));
}

void VoiceIsolationDialog::invalidateAnalysis()
{
    if (m_busy)
        return;
    m_hasAnalysis = false;
    if (!m_samples.isEmpty())
        m_statusLabel->setText(tr("設定が変更されました。再解析してください。"));
}

void VoiceIsolationDialog::updateActionState()
{
    const bool ready = !m_samples.isEmpty() && m_sampleRate > 0;
    const bool canAct = ready && !m_busy;
    m_analyzeButton->setEnabled(canAct);
    m_previewButton->setEnabled(canAct);
    m_applyButton->setEnabled(canAct);
    m_rangeStartSpin->setEnabled(canAct);
    m_rangeEndSpin->setEnabled(canAct);
    m_modeCombo->setEnabled(canAct);
    m_strengthSlider->setEnabled(canAct);
    m_strengthSpin->setEnabled(canAct);
    m_voiceGainSpin->setEnabled(canAct);
    m_backgroundGainSpin->setEnabled(canAct);
    m_lowHzSpin->setEnabled(canAct);
    m_highHzSpin->setEnabled(canAct);
    m_noiseLearnSpin->setEnabled(canAct);
    m_adaptiveNoiseCheck->setEnabled(canAct);
    m_harmonicSlider->setEnabled(canAct);
    m_harmonicSpin->setEnabled(canAct);
    m_smoothingSlider->setEnabled(canAct);
    m_smoothingSpin->setEnabled(canAct);
}

void VoiceIsolationDialog::onAnalyzeClicked()
{
    processCurrentRange();
}

void VoiceIsolationDialog::onPreviewClicked()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->stop();
        return;
    }
    if (!processCurrentRange())
        return;
    QString error;
    if (!writePreviewFile(m_processedSamples, &error)) {
        m_statusLabel->setText(tr("プレビュー準備に失敗しました: %1").arg(error));
        return;
    }
    m_player->setSource(QUrl::fromLocalFile(m_previewFile.fileName()));
    m_player->play();
    m_statusLabel->setText(tr("プレビューを再生しています。"));
}

void VoiceIsolationDialog::onApplyClicked()
{
    if (!processCurrentRange())
        return;
    emit applied();
    accept();
}

void VoiceIsolationDialog::onRangeChanged()
{
    if (m_rangeStartSpin->value() > m_rangeEndSpin->value()) {
        const QSignalBlocker blocker(m_rangeEndSpin);
        m_rangeEndSpin->setValue(m_rangeStartSpin->value());
    }
    invalidateAnalysis();
}

void VoiceIsolationDialog::onPlayerStateChanged(QMediaPlayer::PlaybackState state)
{
    if (state == QMediaPlayer::PlayingState) {
        m_previewButton->setText(tr("プレビュー停止"));
    } else {
        m_previewButton->setText(tr("プレビュー再生"));
        if (!m_busy && m_hasAnalysis)
            m_statusLabel->setText(tr("解析完了。適用するか、設定を調整できます。"));
    }
}
