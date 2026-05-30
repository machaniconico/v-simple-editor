#pragma once

// SpectralEngine — iZotope RX 風のスペクトル編集を支える純粋 DSP エンジン (SP-1)。
//
// 既存の AudioRestoration.cpp の "spectralNoiseGate" は名前に反して実体は時間領域
// ブロック RMS ゲートだった。本エンジンは STFT (短時間フーリエ変換) ベースの本物の
// スペクトル編集を提供する。外部 FFT 依存 (kissfft / fftw 等) は一切追加せず、
// 自前の radix-2 Cooley-Tukey FFT を実装する。
//
// QObject / QWidget を一切持たない純粋 DSP なので QApplication 不要で headless
// 単体テストできる (selftest `--selftest=spectral-edit`, needsQApplication=false)。
// 依存は <vector> / <complex> / <cmath> の標準ライブラリのみ。Qt 依存は無い。
//
// アルゴリズム概要:
//   - fft(): in-place radix-2 Cooley-Tukey。サイズは 2 のべき乗のみ。inverse=true
//     で逆変換し 1/N 正規化まで含める (fft→ifft で round-trip が単位的)。
//   - stft(): mono サンプル列を Hann 窓 + hop で区切り、各フレームを FFT して
//     複素スペクトルのフレーム列を作る。端は 0 パディング。
//   - istft(): overlap-add で逆変換し、各サンプル位置での窓二乗和で割って正規化
//     (COLA 条件を満たす Hann + hop=fftSize/4 等で忠実再構成)。
//   - attenuateRegions(): 時間周波数領域の矩形 (SpectralRegion) に入るビンの複素
//     スペクトルに attenuation 係数を乗算する (複素数への係数乗算なので位相は保持
//     される)。実信号性を保つため対称ビン (N-k) も同じ係数で処理する。
//   - applySpectralEdit(): stft → attenuateRegions → istft の一括ヘルパー。
//     regions が空なら入力をほぼ忠実に再構成する (round-trip)。
//
// COLA / 正規化方式:
//   overlap-add 後に「各出力サンプル位置に重なった解析窓 (Hann) の二乗和」で割る。
//   これにより hop が COLA を満たさない場合でも振幅一貫性が保たれる。窓二乗和が
//   極小 (端の未カバー領域) の位置はゼロ除算を避けるため出力 0 とする。

#include <vector>
#include <complex>
#include <cmath>

namespace spectral {

// ---------------------------------------------------------------------------
// 基本 DSP プリミティブ
// ---------------------------------------------------------------------------

// in-place radix-2 Cooley-Tukey FFT。data.size() は 2 のべき乗でなければならない
// (そうでなければ何もしない)。inverse=false で順変換、inverse=true で逆変換し
// 1/N 正規化まで含める (順→逆で恒等変換)。
void fft(std::vector<std::complex<double>>& data, bool inverse);

// 長さ n の周期的 Hann 窓 (w[i] = 0.5 - 0.5*cos(2*pi*i/n))。n<=0 なら空。
std::vector<double> hannWindow(int n);

// ---------------------------------------------------------------------------
// STFT コンテナ
// ---------------------------------------------------------------------------

// 短時間フーリエ変換の結果。frames[f][k] = フレーム f のビン k の複素スペクトル
// (k は 0..fftSize-1 の全ビン、対称分も含む)。
struct Stft {
    int fftSize = 0;
    int hopSize = 0;
    int sampleRate = 0;
    std::vector<std::vector<std::complex<double>>> frames;
};

// mono サンプル列 → STFT フレーム列。Hann 窓 + hop で区切り、端は 0 パディング。
// fftSize は 2 のべき乗、hopSize>0 を想定 (不正なら空の Stft)。
Stft stft(const std::vector<double>& samples, int sampleRate, int fftSize, int hopSize);

// STFT フレーム列 → サンプル列。overlap-add + 窓二乗和正規化で再構成し、
// originalLength にトリム (パディング分を除去)。originalLength<0 なら全長を返す。
std::vector<double> istft(const Stft& s, int originalLength);

// ---------------------------------------------------------------------------
// スペクトル編集
// ---------------------------------------------------------------------------

// 時間周波数領域の矩形減衰指定。attenuation: 0=完全除去, 1=変化なし。
// 矩形に入るビンの複素スペクトルに attenuation を乗算する。
struct SpectralRegion {
    double tStartSec = 0.0;
    double tEndSec = 0.0;
    double fLowHz = 0.0;
    double fHighHz = 0.0;
    double attenuation = 0.0;   // [0,1] にクランプして適用
};

// Stft の各フレーム f (時刻 = f*hop/sr) / 各ビン k (周波数 = binToHz(k)) が
// いずれかの region 矩形に入れば、その複素スペクトルに region.attenuation を
// 乗算する。実信号性を維持するため対称ビン (fftSize-k) も同じ係数で処理する。
// 複数 region が重なる場合は順に乗算される (係数が積算される)。
void attenuateRegions(Stft& s, const std::vector<SpectralRegion>& regions);

// stft → attenuateRegions → istft の一括ヘルパー。regions が空なら入力をほぼ
// 忠実に再構成する (round-trip)。出力長は samples.size() に一致する。
std::vector<double> applySpectralEdit(const std::vector<double>& samples,
                                      int sampleRate, int fftSize, int hopSize,
                                      const std::vector<SpectralRegion>& regions);

// ---------------------------------------------------------------------------
// ビン <-> 周波数 ヘルパー
// ---------------------------------------------------------------------------

// ビン k (0..fftSize-1) の中心周波数 [Hz] = k * sampleRate / fftSize。
double binToHz(int k, int fftSize, int sampleRate);

// 周波数 hz に最も近いビン番号 (0..fftSize-1 にクランプ、四捨五入)。
int hzToBin(double hz, int fftSize, int sampleRate);

} // namespace spectral
