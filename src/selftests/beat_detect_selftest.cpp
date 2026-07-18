// Beat detection headless selftest.
//
// QApplication 不要。beatdetect:: の純粋オンセット/BPM 検出を合成クリック
// トラックで検証する。

#include <QDebug>
#include <QString>
#include <QVector>

#include <cmath>

#include "../BeatDetect.h"

namespace {

// 無音背景に等間隔の短い大振幅バーストを置いたクリックトラックを作る。
// クリック i は t = leadSec + i*intervalSec に置かれる。
QVector<float> makeClickTrack(int sampleRate, double intervalSec, int count,
                              double leadSec = 0.2, double clickLenSec = 0.02,
                              float amp = 0.9f)
{
    const double tailSec = 0.2;
    const double totalSec = leadSec + count * intervalSec + tailSec;
    const int n = std::max(1, static_cast<int>(std::round(totalSec * sampleRate)));
    QVector<float> samples(n, 0.0f);
    const int clickLen = std::max(1, static_cast<int>(std::round(clickLenSec * sampleRate)));
    for (int i = 0; i < count; ++i) {
        const double tSec = leadSec + i * intervalSec;
        const int start = static_cast<int>(std::round(tSec * sampleRate));
        for (int j = 0; j < clickLen && (start + j) < n; ++j) {
            // 矩形バースト(符号反転で DC を避ける)。
            samples[start + j] = (j % 2 == 0) ? amp : -amp;
        }
    }
    return samples;
}

bool anyBeatNear(const QVector<double>& beats, double tSec, double tolSec)
{
    for (double b : beats) {
        if (std::fabs(b - tSec) <= tolSec)
            return true;
    }
    return false;
}

} // namespace

int runBeatDetectSelftest()
{
    qInfo().noquote() << "[beat-detect] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[beat-detect] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[beat-detect] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok) pass(name); else fail(name, msg);
    };

    const int sr = 16000;
    const beatdetect::Config cfg; // 既定

    {
        // G1: 120 BPM (0.5s 間隔) クリック10個 → 検出数 10±2。
        const QVector<float> s = makeClickTrack(sr, 0.5, 10);
        const beatdetect::Result r = beatdetect::detectBeats(s, sr, cfg);
        check("G1 120BPM 10 clicks -> ~10 beats",
              r.beatTimesSec.size() >= 8 && r.beatTimesSec.size() <= 12,
              QStringLiteral("detected=%1").arg(r.beatTimesSec.size()));

        // G2: BPM 推定 120±8。
        check("G2 120BPM tempo estimate",
              std::fabs(r.bpm - 120.0) <= 8.0,
              QStringLiteral("bpm=%1").arg(r.bpm, 0, 'f', 2));

        // G3: 各クリック位置近傍に検出がある(±0.05s)。
        bool allNear = true;
        for (int i = 0; i < 10; ++i) {
            const double t = 0.2 + i * 0.5;
            if (!anyBeatNear(r.beatTimesSec, t, 0.05)) { allNear = false; break; }
        }
        check("G3 beats align to click positions (+/-50ms)", allNear,
              QStringLiteral("beats=%1").arg(r.beatTimesSec.size()));
    }

    {
        // G4: 無音のみ → ビート空・bpm 0。
        QVector<float> silence(sr * 2, 0.0f);
        const beatdetect::Result r = beatdetect::detectBeats(silence, sr, cfg);
        check("G4 silence -> no beats, bpm 0",
              r.beatTimesSec.isEmpty() && r.bpm == 0.0,
              QStringLiteral("n=%1 bpm=%2").arg(r.beatTimesSec.size()).arg(r.bpm));
    }

    {
        // G5: 入力ガード。
        const beatdetect::Result r1 = beatdetect::detectBeats({}, sr, cfg);
        QVector<float> dummy(100, 0.1f);
        const beatdetect::Result r2 = beatdetect::detectBeats(dummy, 0, cfg);
        check("G5 input guards -> empty",
              r1.beatTimesSec.isEmpty() && r1.bpm == 0.0
                  && r2.beatTimesSec.isEmpty() && r2.bpm == 0.0);
    }

    {
        // G6: デバウンス。1200BPM 相当(0.05s 間隔)でも採用間隔は 60/maxBpm 秒以上。
        const QVector<float> s = makeClickTrack(sr, 0.05, 40);
        const beatdetect::Result r = beatdetect::detectBeats(s, sr, cfg);
        const double minInterval = 60.0 / cfg.maxBpm; // 0.3s
        bool debounced = true;
        for (int i = 1; i < r.beatTimesSec.size(); ++i) {
            if (r.beatTimesSec[i] - r.beatTimesSec[i - 1] < minInterval - 1e-6) {
                debounced = false; break;
            }
        }
        check("G6 dense clicks debounced to >= 60/maxBpm", debounced,
              QStringLiteral("beats=%1 minInterval=%2")
                  .arg(r.beatTimesSec.size()).arg(minInterval, 0, 'f', 3));
    }

    {
        // G7: 100 BPM (0.6s 間隔) → bpm 100±10。
        const QVector<float> s = makeClickTrack(sr, 0.6, 8);
        const beatdetect::Result r = beatdetect::detectBeats(s, sr, cfg);
        check("G7 100BPM tempo estimate",
              std::fabs(r.bpm - 100.0) <= 10.0,
              QStringLiteral("bpm=%1 beats=%2")
                  .arg(r.bpm, 0, 'f', 2).arg(r.beatTimesSec.size()));
    }

    qInfo().noquote() << "[beat-detect] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
