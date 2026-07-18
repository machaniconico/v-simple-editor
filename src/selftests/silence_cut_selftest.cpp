// Silence cut headless selftest.
//
// QApplication 不要。silencecut:: の純粋 RMS silence/keep segmentation を検証する。

#include <QDebug>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

#include "../SilenceCut.h"

namespace {

QVector<float> makeTone(int sampleRate, double sec, double amp)
{
    QVector<float> samples;
    const int count = std::max(0, static_cast<int>(std::round(sampleRate * sec)));
    samples.reserve(count);
    const double freq = 440.0;
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < count; ++i) {
        const double t = i / static_cast<double>(sampleRate);
        samples.push_back(static_cast<float>(amp * std::sin(2.0 * pi * freq * t)));
    }
    return samples;
}

QVector<float> makeSilence(int sampleRate, double sec)
{
    return QVector<float>(std::max(0, static_cast<int>(std::round(sampleRate * sec))), 0.0f);
}

void appendSamples(QVector<float>& dst, const QVector<float>& src)
{
    dst.reserve(dst.size() + src.size());
    for (float sample : src)
        dst.push_back(sample);
}

bool near(double actual, double expected, double tolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

double totalDurationSec(const QVector<float>& samples, int sampleRate)
{
    return samples.size() / static_cast<double>(sampleRate);
}

double durationSum(const QVector<silencecut::Segment>& segments)
{
    double sum = 0.0;
    for (const silencecut::Segment& segment : segments)
        sum += segment.durationSec();
    return sum;
}

bool noOverlap(const QVector<silencecut::Segment>& a,
               const QVector<silencecut::Segment>& b,
               double tolerance)
{
    for (const silencecut::Segment& left : a) {
        for (const silencecut::Segment& right : b) {
            if (left.endSec > right.startSec + tolerance
                && right.endSec > left.startSec + tolerance) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int runSilenceCutSelftest()
{
    qInfo().noquote() << "[silence-cut] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[silence-cut] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[silence-cut] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok)
            pass(name);
        else
            fail(name, msg);
    };

    const int sampleRate = 48000;

    {
        const silencecut::Config cfg;
        const QVector<float> samples = makeSilence(sampleRate, 1.0);
        const QVector<silencecut::Segment> keep =
            silencecut::detectKeepSegments(samples, sampleRate, cfg);
        check("G1 all silence produces no keep segments",
              keep.isEmpty(),
              QStringLiteral("keepCount=%1").arg(keep.size()));
    }

    {
        const silencecut::Config cfg;
        const QVector<float> samples = makeTone(sampleRate, 1.0, 0.8);
        const QVector<silencecut::Segment> keep =
            silencecut::detectKeepSegments(samples, sampleRate, cfg);
        const double total = totalDurationSec(samples, sampleRate);
        const double tolerance = cfg.windowSec + cfg.paddingSec;
        check("G2 all loud tone keeps nearly all audio",
              keep.size() == 1
                  && near(keep[0].startSec, 0.0, tolerance)
                  && near(keep[0].endSec, total, tolerance),
              QStringLiteral("keepCount=%1 start=%2 end=%3 total=%4 tol=%5")
                  .arg(keep.size())
                  .arg(keep.isEmpty() ? -1.0 : keep[0].startSec, 0, 'f', 6)
                  .arg(keep.isEmpty() ? -1.0 : keep[0].endSec, 0, 'f', 6)
                  .arg(total, 0, 'f', 6)
                  .arg(tolerance, 0, 'f', 6));
    }

    {
        silencecut::Config cfg;
        cfg.thresholdDb = -40.0;
        cfg.minSilenceSec = 0.50;
        QVector<float> samples;
        appendSamples(samples, makeTone(sampleRate, 1.0, 0.8));
        appendSamples(samples, makeSilence(sampleRate, 1.0));
        appendSamples(samples, makeTone(sampleRate, 1.0, 0.8));
        const QVector<silencecut::Segment> keep =
            silencecut::detectKeepSegments(samples, sampleRate, cfg);
        const double tolerance = cfg.windowSec + cfg.paddingSec;
        check("G3 1s silence splits two tone keeps",
              keep.size() == 2
                  && near(keep[0].endSec, 1.0 + cfg.paddingSec, tolerance)
                  && near(keep[1].startSec, 2.0 - cfg.paddingSec, tolerance),
              QStringLiteral("keepCount=%1 k0End=%2 k1Start=%3 tol=%4")
                  .arg(keep.size())
                  .arg(keep.size() > 0 ? keep[0].endSec : -1.0, 0, 'f', 6)
                  .arg(keep.size() > 1 ? keep[1].startSec : -1.0, 0, 'f', 6)
                  .arg(tolerance, 0, 'f', 6));
    }

    {
        silencecut::Config cfg;
        cfg.minSilenceSec = 0.50;
        QVector<float> samples;
        appendSamples(samples, makeTone(sampleRate, 1.0, 0.8));
        appendSamples(samples, makeSilence(sampleRate, 0.2));
        appendSamples(samples, makeTone(sampleRate, 1.0, 0.8));
        const QVector<silencecut::Segment> keep =
            silencecut::detectKeepSegments(samples, sampleRate, cfg);
        const double total = totalDurationSec(samples, sampleRate);
        const double tolerance = cfg.windowSec + cfg.paddingSec;
        check("G4 short silence below minSilence does not split",
              keep.size() == 1
                  && near(keep[0].startSec, 0.0, tolerance)
                  && near(keep[0].endSec, total, tolerance),
              QStringLiteral("keepCount=%1 start=%2 end=%3 total=%4 tol=%5")
                  .arg(keep.size())
                  .arg(keep.isEmpty() ? -1.0 : keep[0].startSec, 0, 'f', 6)
                  .arg(keep.isEmpty() ? -1.0 : keep[0].endSec, 0, 'f', 6)
                  .arg(total, 0, 'f', 6)
                  .arg(tolerance, 0, 'f', 6));
    }

    {
        silencecut::Config cfg;
        cfg.paddingSec = 0.10;
        QVector<float> samples;
        appendSamples(samples, makeTone(sampleRate, 1.0, 0.8));
        appendSamples(samples, makeSilence(sampleRate, 1.0));
        const QVector<silencecut::Segment> keep =
            silencecut::detectKeepSegments(samples, sampleRate, cfg);
        const double total = totalDurationSec(samples, sampleRate);
        const double tolerance = cfg.windowSec + cfg.paddingSec;
        check("G5 padding extends keep past speech end",
              keep.size() == 1
                  && keep[0].endSec > 1.0
                  && keep[0].endSec <= total
                  && near(keep[0].endSec, 1.0 + cfg.paddingSec, tolerance),
              QStringLiteral("keepCount=%1 end=%2 total=%3 tol=%4")
                  .arg(keep.size())
                  .arg(keep.isEmpty() ? -1.0 : keep[0].endSec, 0, 'f', 6)
                  .arg(total, 0, 'f', 6)
                  .arg(tolerance, 0, 'f', 6));
    }

    {
        silencecut::Config cfg;
        cfg.paddingSec = 0.0;
        cfg.minKeepSec = 0.20;
        QVector<float> samples;
        appendSamples(samples, makeSilence(sampleRate, 1.0));
        appendSamples(samples, makeTone(sampleRate, 0.1, 0.8));
        appendSamples(samples, makeSilence(sampleRate, 1.0));
        const QVector<silencecut::Segment> keep =
            silencecut::detectKeepSegments(samples, sampleRate, cfg);
        check("G6 minKeep drops a 0.1s tone island",
              keep.isEmpty(),
              QStringLiteral("keepCount=%1 firstDuration=%2")
                  .arg(keep.size())
                  .arg(keep.isEmpty() ? 0.0 : keep[0].durationSec(), 0, 'f', 6));
    }

    {
        silencecut::Config loudThreshold;
        loudThreshold.thresholdDb = -40.0;
        silencecut::Config quietThreshold;
        quietThreshold.thresholdDb = -60.0;
        const QVector<float> samples = makeTone(sampleRate, 1.0, 0.0031622776601683794);
        const QVector<silencecut::Segment> loudKeep =
            silencecut::detectKeepSegments(samples, sampleRate, loudThreshold);
        const QVector<silencecut::Segment> quietKeep =
            silencecut::detectKeepSegments(samples, sampleRate, quietThreshold);
        const double total = totalDurationSec(samples, sampleRate);
        const double tolerance = quietThreshold.windowSec + quietThreshold.paddingSec;
        check("G7 threshold controls low-amplitude tone classification",
              loudKeep.isEmpty()
                  && quietKeep.size() == 1
                  && near(quietKeep[0].startSec, 0.0, tolerance)
                  && near(quietKeep[0].endSec, total, tolerance),
              QStringLiteral("loudKeep=%1 quietKeep=%2 quietEnd=%3 total=%4 tol=%5")
                  .arg(loudKeep.size())
                  .arg(quietKeep.size())
                  .arg(quietKeep.isEmpty() ? -1.0 : quietKeep[0].endSec, 0, 'f', 6)
                  .arg(total, 0, 'f', 6)
                  .arg(tolerance, 0, 'f', 6));
    }

    {
        const silencecut::Config cfg;
        QVector<float> samples;
        appendSamples(samples, makeTone(sampleRate, 0.8, 0.8));
        appendSamples(samples, makeSilence(sampleRate, 0.8));
        appendSamples(samples, makeTone(sampleRate, 0.8, 0.8));
        appendSamples(samples, makeSilence(sampleRate, 0.8));
        const QVector<silencecut::Segment> keep =
            silencecut::detectKeepSegments(samples, sampleRate, cfg);
        const QVector<silencecut::Segment> silence =
            silencecut::detectSilenceSegments(samples, sampleRate, cfg);
        const double total = totalDurationSec(samples, sampleRate);
        const double tolerance = cfg.windowSec;
        check("G8 keep and silence segments cover total without overlap",
              near(durationSum(keep) + durationSum(silence), total, tolerance)
                  && noOverlap(keep, silence, tolerance),
              QStringLiteral("keepSum=%1 silenceSum=%2 total=%3 tol=%4 noOverlap=%5")
                  .arg(durationSum(keep), 0, 'f', 6)
                  .arg(durationSum(silence), 0, 'f', 6)
                  .arg(total, 0, 'f', 6)
                  .arg(tolerance, 0, 'f', 6)
                  .arg(noOverlap(keep, silence, tolerance)));
    }

    {
        const silencecut::Config cfg;
        const QVector<float> empty;
        const QVector<float> samples = makeTone(sampleRate, 1.0, 0.8);
        const QVector<silencecut::Segment> emptyKeep =
            silencecut::detectKeepSegments(empty, sampleRate, cfg);
        const QVector<silencecut::Segment> emptySilence =
            silencecut::detectSilenceSegments(empty, sampleRate, cfg);
        const QVector<silencecut::Segment> badRateKeep =
            silencecut::detectKeepSegments(samples, 0, cfg);
        const QVector<silencecut::Segment> badRateSilence =
            silencecut::detectSilenceSegments(samples, 0, cfg);
        check("G9 input guards return empty segments",
              emptyKeep.isEmpty()
                  && emptySilence.isEmpty()
                  && badRateKeep.isEmpty()
                  && badRateSilence.isEmpty(),
              QStringLiteral("emptyKeep=%1 emptySilence=%2 badRateKeep=%3 badRateSilence=%4")
                  .arg(emptyKeep.size())
                  .arg(emptySilence.size())
                  .arg(badRateKeep.size())
                  .arg(badRateSilence.size()));
    }

    {
        ClipInfo src;
        src.inPoint = 0.0;
        src.outPoint = 0.0;
        src.duration = 10.0;
        src.speed = 1.0;
        src.filePath = QStringLiteral("test.mp4");
        const QVector<silencecut::Segment> keeps = { { 1.0, 3.0 }, { 5.0, 8.0 } };
        const QVector<ClipInfo> subs = silencecut::planKeepClips(src, keeps);
        check("G10 planKeepClips basic",
              subs.size() == 2
                  && subs[0].inPoint == 1.0
                  && subs[0].outPoint == 3.0
                  && subs[1].inPoint == 5.0
                  && subs[1].outPoint == 8.0
                  && subs[0].filePath == src.filePath
                  && subs[0].speed == src.speed,
              QStringLiteral("subCount=%1 first=[%2,%3] second=[%4,%5] fileCopied=%6 speedCopied=%7")
                  .arg(subs.size())
                  .arg(subs.size() > 0 ? subs[0].inPoint : -1.0, 0, 'f', 6)
                  .arg(subs.size() > 0 ? subs[0].outPoint : -1.0, 0, 'f', 6)
                  .arg(subs.size() > 1 ? subs[1].inPoint : -1.0, 0, 'f', 6)
                  .arg(subs.size() > 1 ? subs[1].outPoint : -1.0, 0, 'f', 6)
                  .arg(subs.size() > 0 && subs[0].filePath == src.filePath)
                  .arg(subs.size() > 0 && subs[0].speed == src.speed));
    }

    {
        ClipInfo src2;
        src2.inPoint = 2.0;
        src2.outPoint = 12.0;
        src2.duration = 20.0;
        src2.speed = 1.0;
        src2.filePath = QStringLiteral("test2.mp4");
        const QVector<silencecut::Segment> keeps2 = { { 0.0, 3.0 } };
        const QVector<ClipInfo> subs2 = silencecut::planKeepClips(src2, keeps2);
        check("G11 planKeepClips applies inPoint offset",
              subs2.size() == 1
                  && subs2[0].inPoint == 2.0
                  && subs2[0].outPoint == 5.0,
              QStringLiteral("subCount=%1 first=[%2,%3]")
                  .arg(subs2.size())
                  .arg(subs2.size() > 0 ? subs2[0].inPoint : -1.0, 0, 'f', 6)
                  .arg(subs2.size() > 0 ? subs2[0].outPoint : -1.0, 0, 'f', 6));
    }

    {
        ClipInfo src3;
        src3.inPoint = 0.0;
        src3.outPoint = 5.0;
        src3.duration = 10.0;
        src3.speed = 1.0;
        src3.filePath = QStringLiteral("test3.mp4");
        const QVector<silencecut::Segment> outOfRange = { { 6.0, 8.0 } };
        const QVector<ClipInfo> subs3 = silencecut::planKeepClips(src3, outOfRange);
        const QVector<ClipInfo> subs4 = silencecut::planKeepClips(src3, {});
        const QVector<silencecut::Segment> overrun = { { 3.0, 9.0 } };
        const QVector<ClipInfo> subs5 = silencecut::planKeepClips(src3, overrun);
        check("G12 planKeepClips clamps and discards empty ranges",
              subs3.isEmpty()
                  && subs4.isEmpty()
                  && subs5.size() == 1
                  && subs5[0].inPoint == 3.0
                  && subs5[0].outPoint == 5.0,
              QStringLiteral("outOfRange=%1 empty=%2 overrunCount=%3 overrun=[%4,%5]")
                  .arg(subs3.size())
                  .arg(subs4.size())
                  .arg(subs5.size())
                  .arg(subs5.size() > 0 ? subs5[0].inPoint : -1.0, 0, 'f', 6)
                  .arg(subs5.size() > 0 ? subs5[0].outPoint : -1.0, 0, 'f', 6));
    }

    qInfo().noquote() << "[silence-cut] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
