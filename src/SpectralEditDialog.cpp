#include "SpectralEditDialog.h"
#include "SpectrogramWidget.h"
#include "SpectralEngine.h"

#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <utility>

SpectralEditDialog::SpectralEditDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("スペクトル編集"));
    setModal(false); // modeless
    resize(720, 480);

    // --- スペクトログラム (中央) ----------------------------------------------
    m_spectrogram = new SpectrogramWidget(this);

    // --- 減衰量スライダ -------------------------------------------------------
    // 0 = 完全除去, 100 = 無変化。SpectralRegion.attenuation = value/100。
    m_attenSlider = new QSlider(Qt::Horizontal, this);
    m_attenSlider->setRange(0, 100);
    m_attenSlider->setValue(0); // 既定 0 = 完全除去
    m_attenLabel = new QLabel(this);

    connect(m_attenSlider, &QSlider::valueChanged,
            this, &SpectralEditDialog::onAttenuationChanged);

    auto *attenRow = new QHBoxLayout;
    attenRow->addWidget(m_attenSlider, 1);
    attenRow->addWidget(m_attenLabel);

    // --- FFT サイズコンボ -----------------------------------------------------
    m_fftCombo = new QComboBox(this);
    m_fftCombo->addItem(QStringLiteral("1024"), 1024);
    m_fftCombo->addItem(QStringLiteral("2048"), 2048);
    m_fftCombo->addItem(QStringLiteral("4096"), 4096);
    m_fftCombo->setCurrentIndex(1); // 既定 2048

    connect(m_fftCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SpectralEditDialog::onFftSizeChanged);

    // --- ボタン ---------------------------------------------------------------
    m_applyBtn = new QPushButton(tr("適用"), this);
    m_closeBtn = new QPushButton(tr("閉じる"), this);

    connect(m_applyBtn, &QPushButton::clicked,
            this, &SpectralEditDialog::onApplyClicked);
    connect(m_closeBtn, &QPushButton::clicked,
            this, &QDialog::close);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(m_applyBtn);
    btnRow->addWidget(m_closeBtn);

    // --- コントロール行 -------------------------------------------------------
    auto *form = new QFormLayout;
    form->addRow(tr("減衰量 (0=完全除去〜100%=無変化):"), attenRow);
    form->addRow(tr("FFT サイズ:"), m_fftCombo);

    // --- レイアウト -----------------------------------------------------------
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_spectrogram, 1);
    mainLayout->addLayout(form);
    mainLayout->addLayout(btnRow);

    // 初期同期。
    m_spectrogram->setFftSize(m_fftSize);
    onAttenuationChanged(m_attenSlider->value());
}

void SpectralEditDialog::setAudio(const std::vector<double> &samples, int sampleRate)
{
    m_samples          = samples;
    m_sampleRate       = sampleRate;
    m_processedSamples = samples; // 未適用時は入力をそのまま返す

    if (m_spectrogram) {
        m_spectrogram->setFftSize(m_fftSize);
        m_spectrogram->setSamples(m_samples, m_sampleRate);
    }
}

std::vector<double> SpectralEditDialog::processedSamples() const
{
    return m_processedSamples;
}

int SpectralEditDialog::sampleRate() const
{
    return m_sampleRate;
}

void SpectralEditDialog::onAttenuationChanged(int value)
{
    const int clampedValue = std::clamp(value, 0, 100);
    const double atten = clampedValue / 100.0;
    if (m_spectrogram)
        m_spectrogram->setAttenuation(atten);
    if (m_attenLabel)
        m_attenLabel->setText(tr("%1%").arg(clampedValue));
}

void SpectralEditDialog::onFftSizeChanged(int index)
{
    Q_UNUSED(index);
    bool ok = false;
    const int fft = m_fftCombo ? m_fftCombo->currentData().toInt(&ok) : 0;
    if (ok && fft > 0) {
        m_fftSize = fft;
        if (m_spectrogram) {
            // FFT サイズが変わるのでスペクトログラムを再生成する。
            m_spectrogram->setFftSize(m_fftSize);
            m_spectrogram->setSamples(m_samples, m_sampleRate);
        }
    }
}

void SpectralEditDialog::onApplyClicked()
{
    // samples 未設定なら no-op (安全)。
    if (m_samples.empty() || m_sampleRate <= 0) {
        m_processedSamples = m_samples;
        qWarning("SpectralEditDialog: 音声が未設定のため適用をスキップしました。");
        emit applied();
        return;
    }

    const std::vector<spectral::SpectralRegion> regions =
        m_spectrogram ? m_spectrogram->selectedRegions()
                      : std::vector<spectral::SpectralRegion>();

    const int hopSize = std::max(1, m_fftSize / 4);

    // regions が空でも applySpectralEdit は忠実 round-trip を返す。
    std::vector<double> edited = spectral::applySpectralEdit(
        m_samples, m_sampleRate, m_fftSize, hopSize, regions);
    m_processedSamples = (!edited.empty() || m_samples.empty()) ? std::move(edited) : m_samples;

    qInfo("SpectralEditDialog: 適用完了 (fft=%d hop=%d regions=%d in=%d out=%d).",
          m_fftSize, hopSize,
          static_cast<int>(regions.size()),
          static_cast<int>(m_samples.size()),
          static_cast<int>(m_processedSamples.size()));

    emit applied();
}
