#include <QDebug>
#include <QString>

#include <vector>
#include <complex>
#include <cmath>

#include "../SpectralEngine.h"

// spectral-edit selftest (SP-2) — SpectralEngine 純粋 DSP エンジンの単体ゲート群。
// QObject / QWidget を一切持たない自前 radix-2 FFT / STFT / スペクトル編集なので
// QApplication 不要 (needsQApplication=false、--selftest=spectral-edit)。
// FFT/iFFT round-trip / Hann 窓 / 単一正弦のピーク / STFT-iSTFT COLA round-trip /
// 空 region の忠実再構成 / 帯域選択除去 / 時間方向限定 / attenuation=1 無変化 /
// binToHz<->hzToBin 往復整合 / 実信号性を検証する。依存は標準ライブラリのみ。

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// 浮動小数の許容誤差比較。
bool nearly(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) < eps;
}

// サンプル列の合計エネルギー (Σ x[i]^2)。
double energy(const std::vector<double>& x)
{
    double e = 0.0;
    for (double v : x) {
        e += v * v;
    }
    return e;
}

// ある周波数成分のエネルギーを FFT で抽出する。x を 2 のべき乗長で FFT して
// 目標周波数のビン (とその対称ビン) の振幅二乗和を返す。
double bandEnergyAt(const std::vector<double>& x, int sampleRate, double targetHz, int fftSize)
{
    std::vector<std::complex<double>> buf(static_cast<size_t>(fftSize), std::complex<double>(0.0, 0.0));
    const int n = std::min<int>(fftSize, static_cast<int>(x.size()));
    for (int i = 0; i < n; ++i) {
        buf[static_cast<size_t>(i)] = std::complex<double>(x[static_cast<size_t>(i)], 0.0);
    }
    spectral::fft(buf, false);
    const int k = spectral::hzToBin(targetHz, fftSize, sampleRate);
    const int mirror = (k == 0) ? 0 : (fftSize - k);
    double e = std::norm(buf[static_cast<size_t>(k)]);
    if (mirror != k) {
        e += std::norm(buf[static_cast<size_t>(mirror)]);
    }
    return e;
}

// 正弦波 sin(2*pi*f*t) を len サンプル生成する。
std::vector<double> sine(double freqHz, int sampleRate, int len, double amp = 1.0)
{
    std::vector<double> s(static_cast<size_t>(len), 0.0);
    for (int i = 0; i < len; ++i) {
        s[static_cast<size_t>(i)] = amp * std::sin(2.0 * M_PI * freqHz * i / sampleRate);
    }
    return s;
}

} // namespace

int runSpectralEditSelftest()
{
    qInfo().noquote() << "[spectral-edit] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[spectral-edit] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[spectral-edit] FAIL" << name << ":" << msg; };

    // --- G1: fft -> ifft round-trip (相対誤差 < 1e-9) -------------------------
    {
        // 決定論的な擬似乱数 (LCG) で複素配列を作る。
        const int N = 256;
        std::vector<std::complex<double>> orig(static_cast<size_t>(N));
        unsigned long long seed = 0x12345678ULL;
        auto next = [&]() {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<double>((seed >> 11) & 0xFFFFFFFFULL) / static_cast<double>(0xFFFFFFFFULL) - 0.5;
        };
        for (int i = 0; i < N; ++i) {
            orig[static_cast<size_t>(i)] = std::complex<double>(next(), next());
        }
        std::vector<std::complex<double>> work = orig;
        spectral::fft(work, false);
        spectral::fft(work, true);
        double maxErr = 0.0;
        double normRef = 0.0;
        for (int i = 0; i < N; ++i) {
            maxErr = std::max(maxErr, std::abs(work[static_cast<size_t>(i)] - orig[static_cast<size_t>(i)]));
            normRef = std::max(normRef, std::abs(orig[static_cast<size_t>(i)]));
        }
        const double relErr = (normRef > 0.0) ? (maxErr / normRef) : maxErr;
        if (relErr < 1e-9) {
            pass("G1 fft->ifft round-trip identity");
        } else {
            fail("G1 fft round-trip", QStringLiteral("relErr=%1").arg(relErr, 0, 'e', 6));
        }
    }

    // --- G2: hannWindow の長さ / 端 0 / 中央最大 / 対称性 ----------------------
    {
        const int N = 64;
        const std::vector<double> w = spectral::hannWindow(N);
        bool ok = static_cast<int>(w.size()) == N;
        // 周期 Hann は w[0]=0、両端は対称 (w[i]==w[N-i] for i=1..N-1)。
        if (ok) ok = nearly(w[0], 0.0, 1e-12);
        // 中央 (N/2) で最大 ~1.0。
        if (ok) ok = nearly(w[static_cast<size_t>(N / 2)], 1.0, 1e-9);
        // 中央が他の全てのサンプル以上であること。
        for (int i = 0; ok && i < N; ++i) {
            if (w[static_cast<size_t>(i)] > w[static_cast<size_t>(N / 2)] + 1e-12) ok = false;
        }
        // 周期 Hann の対称性 w[i] == w[N-i]。
        for (int i = 1; ok && i < N; ++i) {
            if (!nearly(w[static_cast<size_t>(i)], w[static_cast<size_t>(N - i)], 1e-9)) ok = false;
        }
        // 縮退ケース: n<=0 で空、n==1 で {1.0}。
        if (ok) ok = spectral::hannWindow(0).empty();
        if (ok) {
            const std::vector<double> w1 = spectral::hannWindow(1);
            ok = (w1.size() == 1) && nearly(w1[0], 1.0, 1e-12);
        }
        if (ok) {
            pass("G2 hannWindow length/edges/peak/symmetry");
        } else {
            fail("G2 hannWindow", QStringLiteral("size=%1").arg(static_cast<int>(w.size())));
        }
    }

    // --- G3: 単一正弦の FFT がそのビンに明確なピーク -------------------------
    {
        const int sr = 48000;
        const int fftSize = 4096;
        // 1000Hz is not bin-centered at 4096pt/48kHz (bin 85.33) and
        // correctly leaks into adjacent bins. Use the nearest centered bin
        // so this gate validates FFT bin dominance rather than leakage math.
        const int expectBin = spectral::hzToBin(1000.0, fftSize, sr);
        const double freq = spectral::binToHz(expectBin, fftSize, sr);
        const std::vector<double> s = sine(freq, sr, fftSize);
        std::vector<std::complex<double>> buf(static_cast<size_t>(fftSize));
        for (int i = 0; i < fftSize; ++i) {
            buf[static_cast<size_t>(i)] = std::complex<double>(s[static_cast<size_t>(i)], 0.0);
        }
        spectral::fft(buf, false);
        // 期待ビンの振幅が、無関係ビン (期待ビンと対称ビン以外) の最大より圧倒的に大きい。
        const int mirror = fftSize - expectBin;
        const double peakMag = std::abs(buf[static_cast<size_t>(expectBin)]);
        double otherMax = 0.0;
        for (int k = 0; k < fftSize; ++k) {
            if (k == expectBin || k == mirror) continue;
            otherMax = std::max(otherMax, std::abs(buf[static_cast<size_t>(k)]));
        }
        // 整合チェック: binToHz(expectBin) が freq に近い。
        const double binHz = spectral::binToHz(expectBin, fftSize, sr);
        const bool ok = (peakMag > otherMax * 10.0) && nearly(binHz, freq, 1e-12);
        if (ok) {
            pass("G3 single-sine FFT peak at expected bin");
        } else {
            fail("G3 sine peak", QStringLiteral("freq=%1 peak=%2 otherMax=%3 binHz=%4")
                    .arg(freq).arg(peakMag).arg(otherMax).arg(binHz));
        }
    }

    // --- G4: STFT -> iSTFT round-trip (中央部 RMS 誤差小、COLA 確認) ----------
    {
        const int sr = 48000;
        const int fftSize = 1024;
        const int hop = fftSize / 4;
        // 2 正弦の合成信号。
        std::vector<double> s = sine(440.0, sr, 8192, 0.6);
        const std::vector<double> s2 = sine(1320.0, sr, 8192, 0.3);
        for (size_t i = 0; i < s.size(); ++i) s[i] += s2[i];

        const spectral::Stft st = spectral::stft(s, sr, fftSize, hop);
        const std::vector<double> rec = spectral::istft(st, static_cast<int>(s.size()));

        bool ok = rec.size() == s.size();
        if (ok) {
            // 端 (fftSize 分) を除いた中央部で RMS 誤差を測る (端は窓カバレッジが薄い)。
            double sumSqErr = 0.0, sumSqRef = 0.0;
            int count = 0;
            for (int i = fftSize; i + fftSize < static_cast<int>(s.size()); ++i) {
                const double d = rec[static_cast<size_t>(i)] - s[static_cast<size_t>(i)];
                sumSqErr += d * d;
                sumSqRef += s[static_cast<size_t>(i)] * s[static_cast<size_t>(i)];
                ++count;
            }
            const double rmsErr = (count > 0) ? std::sqrt(sumSqErr / count) : 1.0;
            const double rmsRef = (count > 0) ? std::sqrt(sumSqRef / count) : 0.0;
            // 中央部の相対 RMS 誤差が 1% 未満なら COLA 再構成成功。
            const double relRms = (rmsRef > 0.0) ? (rmsErr / rmsRef) : rmsErr;
            ok = (relRms < 0.01);
            if (!ok) {
                fail("G4 stft round-trip", QStringLiteral("relRms=%1").arg(relRms));
            }
        } else {
            fail("G4 stft round-trip", QStringLiteral("recSize=%1 expected=%2").arg(static_cast<int>(rec.size())).arg(static_cast<int>(s.size())));
        }
        if (ok) {
            pass("G4 STFT->iSTFT COLA round-trip (center RMS)");
        }
    }

    // --- G5: applySpectralEdit で regions 空 -> 入力をほぼ忠実に再構成 --------
    {
        const int sr = 48000;
        const int fftSize = 1024;
        const int hop = fftSize / 4;
        std::vector<double> s = sine(880.0, sr, 8192, 0.7);
        const std::vector<spectral::SpectralRegion> noRegions;
        const std::vector<double> out = spectral::applySpectralEdit(s, sr, fftSize, hop, noRegions);

        bool ok = out.size() == s.size();
        if (ok) {
            double sumSqErr = 0.0, sumSqRef = 0.0;
            int count = 0;
            for (int i = fftSize; i + fftSize < static_cast<int>(s.size()); ++i) {
                const double d = out[static_cast<size_t>(i)] - s[static_cast<size_t>(i)];
                sumSqErr += d * d;
                sumSqRef += s[static_cast<size_t>(i)] * s[static_cast<size_t>(i)];
                ++count;
            }
            const double rmsErr = (count > 0) ? std::sqrt(sumSqErr / count) : 1.0;
            const double rmsRef = (count > 0) ? std::sqrt(sumSqRef / count) : 0.0;
            const double relRms = (rmsRef > 0.0) ? (rmsErr / rmsRef) : rmsErr;
            ok = (relRms < 0.01);
            if (!ok) fail("G5 empty regions", QStringLiteral("relRms=%1").arg(relRms));
        } else {
            fail("G5 empty regions", QStringLiteral("outSize=%1 expected=%2").arg(static_cast<int>(out.size())).arg(static_cast<int>(s.size())));
        }
        if (ok) {
            pass("G5 applySpectralEdit empty regions faithful round-trip");
        }
    }

    // --- G6: 帯域選択除去 (5000Hz を除去・500Hz は保持) ----------------------
    {
        const int sr = 48000;
        const int fftSize = 1024;
        const int hop = fftSize / 4;
        const int len = 16384;
        std::vector<double> s = sine(500.0, sr, len, 0.5);
        const std::vector<double> hi = sine(5000.0, sr, len, 0.5);
        for (int i = 0; i < len; ++i) s[static_cast<size_t>(i)] += hi[static_cast<size_t>(i)];

        // 全時間にわたり 4500-5500Hz を完全除去 (attenuation=0)。
        spectral::SpectralRegion r;
        r.tStartSec = 0.0;
        r.tEndSec = static_cast<double>(len) / sr;     // 全長カバー
        r.fLowHz = 4500.0;
        r.fHighHz = 5500.0;
        r.attenuation = 0.0;
        const std::vector<spectral::SpectralRegion> regions = { r };
        const std::vector<double> out = spectral::applySpectralEdit(s, sr, fftSize, hop, regions);

        const int measFft = 8192;
        // 中央部 (端の窓減衰を避ける) を測定窓に使う。
        std::vector<double> centerIn(s.begin() + 4096, s.begin() + 4096 + measFft);
        std::vector<double> centerOut(out.begin() + 4096, out.begin() + 4096 + measFft);

        const double hiBefore = bandEnergyAt(centerIn, sr, 5000.0, measFft);
        const double hiAfter = bandEnergyAt(centerOut, sr, 5000.0, measFft);
        const double loBefore = bandEnergyAt(centerIn, sr, 500.0, measFft);
        const double loAfter = bandEnergyAt(centerOut, sr, 500.0, measFft);

        // 5000Hz は大幅減衰 (10% 未満)、500Hz は概ね保持 (80% 以上)。
        const bool hiKilled = (hiBefore > 0.0) && (hiAfter < hiBefore * 0.1);
        const bool loKept = (loBefore > 0.0) && (loAfter > loBefore * 0.8);
        if (hiKilled && loKept) {
            pass("G6 band-selective removal (5kHz killed, 500Hz kept)");
        } else {
            fail("G6 band removal", QStringLiteral("hiBefore=%1 hiAfter=%2 loBefore=%3 loAfter=%4")
                    .arg(hiBefore).arg(hiAfter).arg(loBefore).arg(loAfter));
        }
    }

    // --- G7: 時間方向の限定 (前半だけ除去・後半は不変) -----------------------
    {
        const int sr = 48000;
        const int fftSize = 1024;
        const int hop = fftSize / 4;
        const int len = 24000;            // 0.5 秒
        std::vector<double> s = sine(2000.0, sr, len, 0.6);

        const double halfSec = static_cast<double>(len) / sr / 2.0;
        spectral::SpectralRegion r;
        r.tStartSec = 0.0;
        r.tEndSec = halfSec;              // 前半のみ
        r.fLowHz = 1500.0;
        r.fHighHz = 2500.0;
        r.attenuation = 0.0;
        const std::vector<spectral::SpectralRegion> regions = { r };
        const std::vector<double> out = spectral::applySpectralEdit(s, sr, fftSize, hop, regions);

        bool ok = out.size() == s.size();
        if (ok) {
            // 前半中央部のエネルギーは大幅減、後半中央部はほぼ保持。
            const int q = len / 4;        // 前半の中央付近
            const int q3 = (len * 3) / 4; // 後半の中央付近
            const int win = 4096;
            std::vector<double> frontIn(s.begin() + (q - win / 2), s.begin() + (q - win / 2) + win);
            std::vector<double> frontOut(out.begin() + (q - win / 2), out.begin() + (q - win / 2) + win);
            std::vector<double> backIn(s.begin() + (q3 - win / 2), s.begin() + (q3 - win / 2) + win);
            std::vector<double> backOut(out.begin() + (q3 - win / 2), out.begin() + (q3 - win / 2) + win);

            const double frontEnergyIn = energy(frontIn);
            const double frontEnergyOut = energy(frontOut);
            const double backEnergyIn = energy(backIn);
            const double backEnergyOut = energy(backOut);

            const bool frontKilled = (frontEnergyIn > 0.0) && (frontEnergyOut < frontEnergyIn * 0.2);
            const bool backKept = (backEnergyIn > 0.0) && (backEnergyOut > backEnergyIn * 0.8);
            ok = frontKilled && backKept;
            if (!ok) {
                fail("G7 time-limited", QStringLiteral("frontIn=%1 frontOut=%2 backIn=%3 backOut=%4")
                        .arg(frontEnergyIn).arg(frontEnergyOut).arg(backEnergyIn).arg(backEnergyOut));
            }
        } else {
            fail("G7 time-limited", QStringLiteral("outSize=%1").arg(static_cast<int>(out.size())));
        }
        if (ok) {
            pass("G7 time-limited removal (front killed, back preserved)");
        }
    }

    // --- G8: attenuation=1.0 は無変化 (空 region 同等の round-trip) ----------
    {
        const int sr = 48000;
        const int fftSize = 1024;
        const int hop = fftSize / 4;
        const int len = 8192;
        std::vector<double> s = sine(1000.0, sr, len, 0.5);
        const std::vector<double> mid = sine(3000.0, sr, len, 0.4);
        for (int i = 0; i < len; ++i) s[static_cast<size_t>(i)] += mid[static_cast<size_t>(i)];

        spectral::SpectralRegion r;
        r.tStartSec = 0.0;
        r.tEndSec = static_cast<double>(len) / sr;
        r.fLowHz = 0.0;
        r.fHighHz = static_cast<double>(sr) / 2.0;     // 全帯域
        r.attenuation = 1.0;                            // 無変化
        const std::vector<spectral::SpectralRegion> regions = { r };

        const std::vector<double> outAtten = spectral::applySpectralEdit(s, sr, fftSize, hop, regions);
        const std::vector<spectral::SpectralRegion> noRegions;
        const std::vector<double> outNone = spectral::applySpectralEdit(s, sr, fftSize, hop, noRegions);

        bool ok = (outAtten.size() == outNone.size()) && (outAtten.size() == s.size());
        if (ok) {
            double maxDiff = 0.0;
            for (size_t i = 0; i < outAtten.size(); ++i) {
                maxDiff = std::max(maxDiff, std::fabs(outAtten[i] - outNone[i]));
            }
            // attenuation=1 は乗算係数 1 なので空 region と完全一致するはず。
            ok = (maxDiff < 1e-9);
            if (!ok) fail("G8 attenuation=1", QStringLiteral("maxDiff=%1").arg(maxDiff, 0, 'e', 6));
        } else {
            fail("G8 attenuation=1", QStringLiteral("sizes mismatch"));
        }
        if (ok) {
            pass("G8 attenuation=1.0 leaves signal unchanged");
        }
    }

    // --- G9: binToHz / hzToBin の往復整合 ------------------------------------
    {
        const int sr = 48000;
        const int fftSize = 2048;
        bool ok = true;
        // 各ビンの中心周波数 -> hzToBin で同じビンに戻る。
        for (int k = 0; ok && k <= fftSize / 2; ++k) {
            const double hz = spectral::binToHz(k, fftSize, sr);
            const int back = spectral::hzToBin(hz, fftSize, sr);
            if (back != k) {
                ok = false;
                fail("G9 bin<->hz", QStringLiteral("k=%1 hz=%2 back=%3").arg(k).arg(hz).arg(back));
            }
        }
        // 縮退ケース: 不正パラメータで 0。
        if (ok) ok = nearly(spectral::binToHz(5, 0, sr), 0.0, 1e-12);
        if (ok) ok = (spectral::hzToBin(1000.0, fftSize, 0) == 0);
        // クランプ: 過大な hz はナイキスト相当のビン上限にクランプ。
        if (ok) {
            const int clamped = spectral::hzToBin(1.0e9, fftSize, sr);
            ok = (clamped >= 0 && clamped <= fftSize - 1);
        }
        if (ok) {
            pass("G9 binToHz<->hzToBin round-trip consistency");
        }
    }

    // --- G10: 実信号性 (実入力 -> 出力が実数、虚部相当の漏れが無い) ----------
    {
        const int sr = 48000;
        const int fftSize = 1024;
        const int hop = fftSize / 4;
        const int len = 8192;
        std::vector<double> s = sine(750.0, sr, len, 0.5);
        const std::vector<double> b = sine(4000.0, sr, len, 0.5);
        for (int i = 0; i < len; ++i) s[static_cast<size_t>(i)] += b[static_cast<size_t>(i)];

        // 4000Hz を半分に減衰。対称ビンが同じ係数で処理されないと出力に虚部漏れが
        // 出て NaN/inf や非実数的な暴れが生じる。ここでは出力が有限実数で、かつ
        // round-trip 後に istft 経由の実数列として安定していることを確認する。
        spectral::SpectralRegion r;
        r.tStartSec = 0.0;
        r.tEndSec = static_cast<double>(len) / sr;
        r.fLowHz = 3500.0;
        r.fHighHz = 4500.0;
        r.attenuation = 0.5;
        const std::vector<spectral::SpectralRegion> regions = { r };
        const std::vector<double> out = spectral::applySpectralEdit(s, sr, fftSize, hop, regions);

        bool ok = out.size() == s.size();
        // 全サンプルが有限 (NaN/inf でない)。istft の出力は real() なので実数だが、
        // 対称ビン処理が崩れると振幅が異常値になる。中央部の最大絶対値が入力の
        // 妥当な範囲 (元振幅合計 ~1.0 の数倍以内) に収まることを確認する。
        if (ok) {
            double maxAbs = 0.0;
            for (int i = fftSize; ok && i + fftSize < static_cast<int>(out.size()); ++i) {
                const double v = out[static_cast<size_t>(i)];
                if (!std::isfinite(v)) { ok = false; break; }
                maxAbs = std::max(maxAbs, std::fabs(v));
            }
            // 入力ピーク ~1.0 に対し、減衰後の出力中央部は ~1.0 を僅かに超える程度。
            // 対称ビンが壊れていると倍率がずれて大きく外れる。3.0 を上限ガードとする。
            if (ok) ok = (maxAbs < 3.0);
            if (!ok) fail("G10 real-valued", QStringLiteral("maxAbs=%1").arg(maxAbs));
        } else {
            fail("G10 real-valued", QStringLiteral("outSize=%1").arg(static_cast<int>(out.size())));
        }
        if (ok) {
            pass("G10 real input -> finite real output (symmetric bin handling)");
        }
    }

    qInfo().noquote() << "[spectral-edit] selftest done passed=" << passed << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
