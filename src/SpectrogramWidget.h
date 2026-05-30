#pragma once

#include <QWidget>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <vector>

#include "SpectralEngine.h"

// ---------------------------------------------------------------------------
// SpectrogramWidget — SP-3
//
// iZotope RX 風のスペクトログラム表示 + 矩形領域選択ウィジェット。
//   - setSamples() で mono サンプル列を受け取り、内部で spectral::stft して
//     マグニチュード (dB) を QImage 化する (横=時間, 縦=周波数・線形軸, dB を
//     色強度に)。
//   - マウスドラッグで矩形領域 (rubber band) を選択でき、選択確定で選択矩形を
//     時間秒 × 周波数 Hz に変換する。
//   - selectedRegions() で現在の選択を spectral::SpectralRegion として返す
//     (attenuation は外部から setAttenuation() で設定)。
//
// 純粋表示 + 入力ウィジェットなので DSP 実行は SpectralEditDialog 側が行う。
// samples 未設定でも安全に空表示する。
// ---------------------------------------------------------------------------
class SpectrogramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrogramWidget(QWidget *parent = nullptr);

    // mono サンプル列をセットし、内部で STFT → スペクトログラム QImage を再生成する。
    // samples が空、または sampleRate<=0 の場合は安全に空表示にする。
    void setSamples(const std::vector<double> &samples, int sampleRate);

    // 表示に使う FFT サイズ (2 のべき乗、既定 2048)。setSamples 前後どちらで呼んでも
    // 次回再生成時に反映される。即座に再描画したい場合は setSamples を呼び直す。
    void setFftSize(int fftSize);
    int  fftSize() const { return m_fftSize; }

    // 選択矩形に適用する減衰量 [0,1] (0=完全除去, 1=無変化)。selectedRegions() の
    // 各 region.attenuation に反映される。
    void   setAttenuation(double attenuation);
    double attenuation() const { return m_attenuation; }

    // 現在の選択矩形を時間周波数領域の SpectralRegion として返す (attenuation 付き)。
    // 選択が無い、または samples 未設定なら空を返す。
    std::vector<spectral::SpectralRegion> selectedRegions() const;

    // 選択をクリアして再描画する。
    void clearSelection();

signals:
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    // スペクトログラム QImage を現在の m_samples / m_fftSize から再生成する。
    void rebuildImage();

    // ウィジェット座標 → (時間秒, 周波数Hz) 変換。描画領域 (m_plotRect) を基準に
    // 線形マッピングする。範囲外はクランプする。
    double xToTimeSec(int x) const;
    double yToFreqHz(int y) const;

    // 描画に使うプロット矩形 (ウィジェット全体、現状は contentsRect 相当)。
    QRect plotRect() const;

    std::vector<double> m_samples;
    int    m_sampleRate = 0;
    int    m_fftSize    = 2048;
    int    m_hopSize    = 512;   // fftSize/4
    double m_attenuation = 0.0;  // 既定 0 = 完全除去

    // STFT 由来のメタ情報 (座標変換に使う)。
    int    m_numFrames  = 0;     // STFT フレーム数
    double m_durationSec = 0.0;  // サンプル列の長さ [秒]
    double m_maxFreqHz   = 0.0;  // 表示上端の周波数 (= sampleRate/2)

    QImage m_image;              // 生成済みスペクトログラム (無効なら null)

    // 選択状態 (ウィジェット座標)。
    bool   m_selecting = false;
    QPoint m_dragStart;
    QPoint m_dragEnd;
    QRect  m_selectionRect;      // 確定済み選択矩形 (空なら選択なし)
    bool   m_hasSelection = false;
};
