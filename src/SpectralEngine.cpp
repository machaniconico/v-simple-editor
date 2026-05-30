// SpectralEngine.cpp — SP-1 純粋 DSP エンジンの実装。
//
// 自前 radix-2 Cooley-Tukey FFT + Hann 窓 STFT/iSTFT + 時間周波数領域減衰。
// 外部 FFT 依存無し。SpectralEngine.h のヘッダコメントに方式を記載。

#include "SpectralEngine.h"

#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace spectral {

namespace {

// n が 2 のべき乗か (n>0)。
bool isPowerOfTwo(std::size_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

// 0..1 にクランプ。
double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

struct NormalizedRegion {
    double tStartSec = 0.0;
    double tEndSec = 0.0;
    double fLowHz = 0.0;
    double fHighHz = 0.0;
    double attenuation = 1.0;
};

} // namespace

// ---------------------------------------------------------------------------
// fft — in-place radix-2 Cooley-Tukey。
// ---------------------------------------------------------------------------
void fft(std::vector<std::complex<double>>& data, bool inverse)
{
    const std::size_t n = data.size();
    if (n < 2) return;             // 0/1 点は変換不要
    if (!isPowerOfTwo(n)) return;  // radix-2 のみ対応

    // --- bit-reversal permutation ---
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    // --- butterfly ---
    // 順変換は exp(-2πi/len)、逆変換は exp(+2πi/len)。
    const double sign = inverse ? 1.0 : -1.0;
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = sign * 2.0 * M_PI / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            const std::size_t half = len >> 1;
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<double> u = data[i + k];
                const std::complex<double> v = data[i + k + half] * w;
                data[i + k] = u + v;
                data[i + k + half] = u - v;
                w *= wlen;
            }
        }
    }

    // --- 逆変換は 1/N 正規化 ---
    if (inverse) {
        const double inv = 1.0 / static_cast<double>(n);
        for (auto& c : data) {
            c *= inv;
        }
    }
}

// ---------------------------------------------------------------------------
// hannWindow
// ---------------------------------------------------------------------------
std::vector<double> hannWindow(int n)
{
    std::vector<double> w;
    if (n <= 0) return w;
    w.resize(static_cast<std::size_t>(n));
    if (n == 1) {
        w[0] = 1.0;
        return w;
    }
    // 周期的 Hann (DFT 用): 0.5 - 0.5*cos(2*pi*i/n)。
    for (int i = 0; i < n; ++i) {
        w[static_cast<std::size_t>(i)] =
            0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n));
    }
    return w;
}

// ---------------------------------------------------------------------------
// stft
// ---------------------------------------------------------------------------
Stft stft(const std::vector<double>& samples, int sampleRate, int fftSize, int hopSize)
{
    Stft out;
    if (sampleRate <= 0 || fftSize <= 0 || hopSize <= 0 ||
        !isPowerOfTwo(static_cast<std::size_t>(fftSize)))
        return out;

    out.fftSize = fftSize;
    out.hopSize = hopSize;
    out.sampleRate = sampleRate;

    const std::vector<double> win = hannWindow(fftSize);
    const std::size_t n = samples.size();

    // 空入力でも 0 フレームの有効な Stft を返す。
    if (n == 0) return out;

    // フレーム数: 最後の hop 位置が信号内に入るまで。端は 0 パディング。
    // start = f*hop が n 未満である限りフレームを作る (末尾フレームはパディング)。
    for (std::size_t start = 0; start < n; start += static_cast<std::size_t>(hopSize)) {
        std::vector<std::complex<double>> frame(static_cast<std::size_t>(fftSize),
                                                std::complex<double>(0.0, 0.0));
        for (int k = 0; k < fftSize; ++k) {
            const std::size_t idx = start + static_cast<std::size_t>(k);
            const double s = (idx < n) ? samples[idx] : 0.0;
            frame[static_cast<std::size_t>(k)] =
                std::complex<double>(s * win[static_cast<std::size_t>(k)], 0.0);
        }
        fft(frame, false);
        out.frames.push_back(std::move(frame));
    }

    return out;
}

// ---------------------------------------------------------------------------
// istft — overlap-add + 窓二乗和正規化。
// ---------------------------------------------------------------------------
std::vector<double> istft(const Stft& s, int originalLength)
{
    std::vector<double> result;
    if (s.fftSize <= 0 || s.hopSize <= 0 || s.frames.empty() ||
        !isPowerOfTwo(static_cast<std::size_t>(s.fftSize)))
        return result;

    const int fftSize = s.fftSize;
    const int hopSize = s.hopSize;
    const std::vector<double> win = hannWindow(fftSize);

    // 出力に必要な総長 (最終フレーム末尾まで)。
    const std::size_t numFrames = s.frames.size();
    const std::size_t fullLen =
        static_cast<std::size_t>(hopSize) * (numFrames - 1) + static_cast<std::size_t>(fftSize);

    std::vector<double> acc(fullLen, 0.0);     // overlap-add 累積
    std::vector<double> wsum(fullLen, 0.0);    // 各位置の窓二乗和

    for (std::size_t f = 0; f < numFrames; ++f) {
        std::vector<std::complex<double>> frame = s.frames[f];
        // サイズ不整合フレームは安全に skip。
        if (frame.size() != static_cast<std::size_t>(fftSize)) continue;
        fft(frame, true);  // 逆変換 (1/N 正規化込み)

        const std::size_t start = static_cast<std::size_t>(hopSize) * f;
        for (int k = 0; k < fftSize; ++k) {
            const std::size_t idx = start + static_cast<std::size_t>(k);
            if (idx >= fullLen) break;
            const double wk = win[static_cast<std::size_t>(k)];
            // 解析窓を掛けて合成 (analysis-synthesis 同窓)。実部のみ採用。
            acc[idx] += frame[static_cast<std::size_t>(k)].real() * wk;
            wsum[idx] += wk * wk;
        }
    }

    // 窓二乗和で正規化 (COLA を満たさない hop でも振幅一貫)。極小は 0 出力。
    const double eps = 1e-9;
    for (std::size_t i = 0; i < fullLen; ++i) {
        result.push_back(wsum[i] > eps ? acc[i] / wsum[i] : 0.0);
    }

    // 元長にトリム。
    if (originalLength >= 0 && static_cast<std::size_t>(originalLength) <= result.size()) {
        result.resize(static_cast<std::size_t>(originalLength));
    }
    return result;
}

// ---------------------------------------------------------------------------
// attenuateRegions
// ---------------------------------------------------------------------------
void attenuateRegions(Stft& s, const std::vector<SpectralRegion>& regions)
{
    if (regions.empty() || s.frames.empty() || s.fftSize <= 0 ||
        s.hopSize <= 0 || s.sampleRate <= 0)
        return;

    const int fftSize = s.fftSize;
    const int hopSize = s.hopSize;
    const int sr = s.sampleRate;
    const std::size_t numFrames = s.frames.size();
    // ナイキストまでの実ビン数。対称分は N-k で別途処理する。
    const int halfBins = fftSize / 2;
    const double nyquistHz = static_cast<double>(sr) / 2.0;
    const double lastFrameSec =
        static_cast<double>(hopSize) * static_cast<double>(numFrames - 1)
        / static_cast<double>(sr);

    std::vector<NormalizedRegion> clippedRegions;
    clippedRegions.reserve(regions.size());
    for (const SpectralRegion& r : regions) {
        if (!std::isfinite(r.tStartSec) || !std::isfinite(r.tEndSec) ||
            !std::isfinite(r.fLowHz) || !std::isfinite(r.fHighHz) ||
            !std::isfinite(r.attenuation)) {
            continue;
        }

        NormalizedRegion nr;
        nr.tStartSec = std::min(r.tStartSec, r.tEndSec);
        nr.tEndSec = std::max(r.tStartSec, r.tEndSec);
        nr.fLowHz = std::min(r.fLowHz, r.fHighHz);
        nr.fHighHz = std::max(r.fLowHz, r.fHighHz);
        nr.attenuation = clamp01(r.attenuation);

        if (nr.tEndSec < 0.0 || nr.tStartSec > lastFrameSec ||
            nr.fHighHz < 0.0 || nr.fLowHz > nyquistHz) {
            continue;
        }

        nr.tStartSec = std::max(0.0, nr.tStartSec);
        nr.tEndSec = std::min(lastFrameSec, nr.tEndSec);
        nr.fLowHz = std::max(0.0, nr.fLowHz);
        nr.fHighHz = std::min(nyquistHz, nr.fHighHz);
        if (nr.tEndSec >= nr.tStartSec && nr.fHighHz >= nr.fLowHz) {
            clippedRegions.push_back(nr);
        }
    }

    if (clippedRegions.empty())
        return;

    for (std::size_t f = 0; f < numFrames; ++f) {
        const double tSec =
            static_cast<double>(hopSize) * static_cast<double>(f) / static_cast<double>(sr);
        std::vector<std::complex<double>>& frame = s.frames[f];
        if (frame.size() != static_cast<std::size_t>(fftSize)) continue;

        for (int k = 0; k <= halfBins; ++k) {
            const double hz = binToHz(k, fftSize, sr);
            // この (時刻, 周波数) に該当する region 係数を全て積算。
            double coeff = 1.0;
            for (const NormalizedRegion& r : clippedRegions) {
                if (tSec >= r.tStartSec && tSec <= r.tEndSec
                    && hz >= r.fLowHz && hz <= r.fHighHz) {
                    coeff *= r.attenuation;
                }
            }
            if (coeff == 1.0) continue;

            frame[static_cast<std::size_t>(k)] *= coeff;
            // 対称ビン (N-k) も同じ係数で処理し実信号性を維持 (k=0 と k=N/2 は自己対称)。
            const int mirror = fftSize - k;
            if (mirror != k && mirror < fftSize) {
                frame[static_cast<std::size_t>(mirror)] *= coeff;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// applySpectralEdit
// ---------------------------------------------------------------------------
std::vector<double> applySpectralEdit(const std::vector<double>& samples,
                                      int sampleRate, int fftSize, int hopSize,
                                      const std::vector<SpectralRegion>& regions)
{
    Stft s = stft(samples, sampleRate, fftSize, hopSize);
    if (s.frames.empty()) {
        // STFT が作れない (不正パラメータ等) → 入力をそのまま返す。
        return samples;
    }
    attenuateRegions(s, regions);
    return istft(s, static_cast<int>(samples.size()));
}

// ---------------------------------------------------------------------------
// binToHz / hzToBin
// ---------------------------------------------------------------------------
double binToHz(int k, int fftSize, int sampleRate)
{
    if (fftSize <= 0 || sampleRate <= 0) return 0.0;
    const int clampedK = std::clamp(k, 0, fftSize - 1);
    return static_cast<double>(clampedK) * static_cast<double>(sampleRate)
           / static_cast<double>(fftSize);
}

int hzToBin(double hz, int fftSize, int sampleRate)
{
    if (sampleRate <= 0 || fftSize <= 0 || !std::isfinite(hz)) return 0;
    double bin = hz * static_cast<double>(fftSize) / static_cast<double>(sampleRate);
    if (!std::isfinite(bin) || bin <= 0.0) return 0;
    if (bin >= static_cast<double>(fftSize - 1)) return fftSize - 1;
    return static_cast<int>(std::lround(bin));
}

} // namespace spectral
