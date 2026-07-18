#pragma once

#include <QDialog>
#include <vector>

class QSlider;
class QComboBox;
class QPushButton;
class QLabel;
class SpectrogramWidget;

// ---------------------------------------------------------------------------
// SpectralEditDialog — SP-3
//
// iZotope RX 風スペクトル編集ダイアログ。中央に SpectrogramWidget を置き、
// 減衰量スライダ (0=完全除去 〜 100%=無変化)、FFT サイズコンボ (1024/2048/4096)、
// 「適用」「閉じる」ボタンを持つ。
//
//   - setAudio() で mono サンプル列を渡すとスペクトログラムを表示する。
//   - ユーザが矩形領域を選択し「適用」を押すと spectral::applySpectralEdit を
//     実行し、結果を processedSamples() で取得できる + applied() を emit する。
//     hop は fftSize/4。
//
// samples 未設定でも安全に動作する (適用は no-op)。
// ---------------------------------------------------------------------------
class SpectralEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SpectralEditDialog(QWidget *parent = nullptr);

    // mono サンプル列をセットしスペクトログラムを更新する。
    void setAudio(const std::vector<double> &samples, int sampleRate);

    // 「適用」実行後の処理済みサンプル列。未適用なら入力サンプルのコピーを返す。
    std::vector<double> processedSamples() const;

    // setAudio で渡されたサンプルレート。
    int sampleRate() const;

signals:
    // 「適用」で applySpectralEdit を実行し processedSamples が更新された時に emit。
    void applied();

private slots:
    void onAttenuationChanged(int value);
    void onFftSizeChanged(int index);
    void onApplyClicked();

private:
    SpectrogramWidget *m_spectrogram = nullptr;
    QSlider           *m_attenSlider = nullptr;
    QLabel            *m_attenLabel  = nullptr;
    QComboBox         *m_fftCombo    = nullptr;
    QPushButton       *m_applyBtn    = nullptr;
    QPushButton       *m_closeBtn    = nullptr;

    std::vector<double> m_samples;          // 入力サンプル
    std::vector<double> m_processedSamples; // 適用後サンプル
    int                 m_sampleRate = 0;
    int                 m_fftSize    = 2048;
};
