#include <QDebug>
#include <QString>
#include <QVector>

#include <cmath>

#include "../color/DvTimelineBuilder.h"
#include "../DolbyVisionMetadata.h"

namespace {

bool approxEqual(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) <= eps;
}

dolbyvision::DolbyVisionMetadata buildBaseMeta()
{
    dolbyvision::DolbyVisionMetadata meta;
    meta.profile = 81;
    meta.level = 6;
    meta.title = QStringLiteral("DV Timeline");
    meta.l6.maxCll = 1000;
    meta.l6.maxFall = 400;
    meta.l6.masteringMaxNits = 1000;
    meta.l6.masteringMinNits = 0;

    dolbyvision::DvShot shot;
    shot.startSec = 10.0;
    shot.endSec = 20.0;
    shot.l1.minNits = 2.0;
    shot.l1.avgNits = 80.0;
    shot.l1.maxNits = 320.0;
    dolbyvision::L2Trim trim;
    trim.targetNits = 100;
    trim.slope = 0.1;
    shot.trims.push_back(trim);
    shot.l5.top = 10;
    shot.l5.bottom = 12;
    meta.shots.push_back(shot);

    return meta;
}

QVector<dvtimeline::ShotSpan> buildSpans()
{
    QVector<dvtimeline::ShotSpan> spans;

    dvtimeline::ShotSpan a;
    a.startSec = 0.0;
    a.endSec = 1.25;
    spans.push_back(a);

    dvtimeline::ShotSpan b;
    b.startSec = 1.75;
    b.endSec = 4.0;
    b.colorMeta.isHdr = true;
    b.colorMeta.primaries = clipcolor::Primaries::Rec2020;
    b.colorMeta.transfer = clipcolor::Transfer::PQ;
    b.colorMeta.bitDepth = 10;
    spans.push_back(b);

    dvtimeline::ShotSpan c;
    c.startSec = 4.0;
    c.endSec = 6.5;
    spans.push_back(c);

    return spans;
}

} // namespace

int runDvTimelineSelftest()
{
    using namespace dolbyvision;

    qInfo().noquote() << "[dv-timeline] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[dv-timeline] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[dv-timeline] FAIL" << name << ":" << msg; };

    const DolbyVisionMetadata base = buildBaseMeta();
    const QVector<dvtimeline::ShotSpan> spans = buildSpans();
    const DolbyVisionMetadata result = dvtimeline::buildFromTimeline(spans, base);

    // G1: 3 spans -> 3 shots with matching start/end times.
    {
        bool timesOk = result.shots.size() == spans.size();
        for (int i = 0; timesOk && i < result.shots.size(); ++i) {
            timesOk = approxEqual(result.shots[i].startSec, spans[i].startSec)
                && approxEqual(result.shots[i].endSec, spans[i].endSec);
        }
        if (timesOk) {
            pass("G1 spans become matching shots");
        } else {
            fail("G1 spans",
                 QStringLiteral("shots=%1 spans=%2").arg(result.shots.size()).arg(spans.size()));
        }
    }

    // G2: L6/profile/title are preserved from base.
    {
        const bool ok = result.profile == base.profile
            && result.level == base.level
            && result.title == base.title
            && result.l6.maxCll == base.l6.maxCll
            && result.l6.maxFall == base.l6.maxFall
            && result.l6.masteringMaxNits == base.l6.masteringMaxNits
            && result.l6.masteringMinNits == base.l6.masteringMinNits;
        if (ok) {
            pass("G2 base profile title l6 preserved");
        } else {
            fail("G2 preserved fields",
                 QStringLiteral("profile=%1 level=%2 title=%3 maxCll=%4")
                    .arg(result.profile).arg(result.level).arg(result.title).arg(result.l6.maxCll));
        }
    }

    // G3: matching base shot index preserves L1; later shots get default L1 from L6.
    {
        const bool preserved = result.shots.size() >= 1
            && approxEqual(result.shots[0].l1.minNits, base.shots[0].l1.minNits)
            && approxEqual(result.shots[0].l1.avgNits, base.shots[0].l1.avgNits)
            && approxEqual(result.shots[0].l1.maxNits, base.shots[0].l1.maxNits)
            && result.shots[0].trims.size() == base.shots[0].trims.size()
            && result.shots[0].l5.top == base.shots[0].l5.top
            && result.shots[0].l5.bottom == base.shots[0].l5.bottom;
        const double defaultAvg =
            (base.l6.masteringMinNits + base.l6.masteringMaxNits) / 2.0;
        bool defaulted = result.shots.size() >= 3;
        for (int i = 1; defaulted && i < result.shots.size(); ++i) {
            defaulted = approxEqual(result.shots[i].l1.minNits, base.l6.masteringMinNits)
                && approxEqual(result.shots[i].l1.avgNits, defaultAvg)
                && approxEqual(result.shots[i].l1.maxNits, base.l6.masteringMaxNits)
                && result.shots[i].trims.isEmpty()
                && result.shots[i].l5.left == 0
                && result.shots[i].l5.right == 0
                && result.shots[i].l5.top == 0
                && result.shots[i].l5.bottom == 0;
        }
        if (preserved && defaulted) {
            pass("G3 matching shot preserves L1 otherwise default from L6");
        } else {
            fail("G3 L1",
                 QStringLiteral("preserved=%1 defaulted=%2").arg(preserved).arg(defaulted));
        }
    }

    // G4: empty spans produce zero shots.
    {
        const QVector<dvtimeline::ShotSpan> emptySpans;
        const DolbyVisionMetadata empty = dvtimeline::buildFromTimeline(emptySpans, base);
        if (empty.shots.isEmpty()) {
            pass("G4 empty spans produce no shots");
        } else {
            fail("G4 empty", QStringLiteral("shots=%1").arg(empty.shots.size()));
        }
    }

    // G5: validate passes and XML contains the DolbyLabsMDF root.
    {
        QString err;
        const bool valid = validate(result, &err);
        const QString xml = toDolbyVisionXml(result, 24.0);
        const bool xmlOk = !xml.isEmpty() && xml.contains(QStringLiteral("<DolbyLabsMDF"));
        if (valid && xmlOk) {
            pass("G5 validate and XML generation");
        } else {
            fail("G5 validate/xml",
                 QStringLiteral("valid=%1 xmlLen=%2 err=%3").arg(valid).arg(xml.size()).arg(err));
        }
    }

    qInfo().noquote().nospace() << "[dv-timeline] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
