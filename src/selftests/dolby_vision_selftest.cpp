#include <QDebug>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

#include "../DolbyVisionMetadata.h"

namespace {

// 浮動小数の許容誤差比較。
bool approxEqual(double a, double b, double eps)
{
    return std::fabs(a - b) <= eps;
}

bool relativeClose(double actual, double expected, double rel)
{
    const double tolerance = std::max(1e-9, std::fabs(expected) * rel);
    return std::fabs(actual - expected) <= tolerance;
}

// テスト用の代表的 Dolby Vision メタデータを組む。
dolbyvision::DolbyVisionMetadata buildSampleMeta()
{
    using namespace dolbyvision;
    DolbyVisionMetadata meta;
    meta.profile = 81;
    meta.level = 6;
    meta.title = QStringLiteral("DV & <Test> \"Sequence\"");
    meta.l6.maxCll = 1200;
    meta.l6.maxFall = 380;
    meta.l6.masteringMaxNits = 1000;
    meta.l6.masteringMinNits = 0;

    // ショット1: 暗いショット (L2 trim 付き)
    DvShot s1;
    s1.startSec = 0.0;
    s1.endSec = 2.0;
    s1.l1.minNits = 0.0;
    s1.l1.avgNits = 50.0;
    s1.l1.maxNits = 200.0;
    L2Trim t1;
    t1.targetNits = 100;
    t1.slope = 0.1;
    t1.offset = -0.05;
    t1.power = 1.0;
    s1.trims.push_back(t1);
    s1.l5.left = 0;
    s1.l5.right = 0;
    s1.l5.top = 132;
    s1.l5.bottom = 132;
    meta.shots.push_back(s1);

    // ショット2: 明るいショット (trim なし)
    DvShot s2;
    s2.startSec = 2.0;
    s2.endSec = 5.5;
    s2.l1.minNits = 1.0;
    s2.l1.avgNits = 400.0;
    s2.l1.maxNits = 4000.0;
    meta.shots.push_back(s2);

    return meta;
}

} // namespace

int runDolbyVisionSelftest()
{
    using namespace dolbyvision;

    qInfo().noquote() << "[dolby-vision] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[dolby-vision] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[dolby-vision] FAIL" << name << ":" << msg; };

    // G1: nitsToPq の端点・代表値・単調性。
    {
        const double pq0 = nitsToPq(0.0);
        const double pqPeak = nitsToPq(kPqPeakNits);
        const double pqLow = nitsToPq(10.0);
        const double pqMid = nitsToPq(100.0);
        const double pqHigh = nitsToPq(1000.0);
        const bool ends = pq0 == 0.0 && pqPeak == 1.0;
        const bool canonical = approxEqual(pqMid, 0.5080, 5e-4)
            && approxEqual(pqHigh, 0.7523, 7e-4);
        const bool monotonic = pq0 < pqLow && pqLow < pqMid && pqMid < pqHigh && pqHigh < pqPeak;
        if (ends && canonical && monotonic) {
            pass("G1 nitsToPq endpoints/canonical/monotonic");
        } else {
            fail("G1 nitsToPq", QStringLiteral("pq0=%1 peak=%2 low=%3 pq100=%4 pq1000=%5")
                    .arg(pq0).arg(pqPeak).arg(pqLow).arg(pqMid).arg(pqHigh));
        }
    }

    // G2: PQ round-trip (0.01..10000 nits は 0.1% 以内)。
    {
        bool ok = true;
        QString detail;
        const double pqSamples[] = { 0.1, 0.3, 0.508, 0.75, 0.9 };
        for (double x : pqSamples) {
            const double back = nitsToPq(pqToNits(x));
            if (!approxEqual(back, x, 1e-4)) {
                ok = false;
                detail = QStringLiteral("nitsToPq(pqToNits(%1))=%2").arg(x).arg(back);
                break;
            }
        }
        const double nitsSamples[] = { 0.01, 0.1, 1.0, 100.0, 1000.0, 4000.0, kPqPeakNits };
        for (double n : nitsSamples) {
            const double back = pqToNits(nitsToPq(n));
            if (!relativeClose(back, n, 0.001)) {
                ok = false;
                detail = QStringLiteral("pqToNits(nitsToPq(%1))=%2").arg(n).arg(back);
                break;
            }
        }
        if (ok) {
            pass("G2 PQ round-trip both directions");
        } else {
            fail("G2 round-trip", detail);
        }
    }

    // G3: 100 / 1000 nits の PQ 値が (0,1) 範囲かつ順序通り。
    {
        const double pq100 = nitsToPq(100.0);
        const double pq1000 = nitsToPq(1000.0);
        const bool ranged = pq100 > 0.0 && pq100 < 1.0 && pq1000 > 0.0 && pq1000 < 1.0;
        const bool ordered = pq100 < pq1000;
        const bool canonical = approxEqual(pq100, 0.508078, 5e-6)
            && approxEqual(pq1000, 0.751827, 5e-6)
            && nitsToPq(10000.0) == 1.0;
        if (ranged && ordered && canonical) {
            pass("G3 canonical PQ values");
        } else {
            fail("G3 pq range", QStringLiteral("pq100=%1 pq1000=%2").arg(pq100).arg(pq1000));
        }
    }

    // G4: nitsToPq12bit の範囲・端点・単調性。
    {
        const int q0 = nitsToPq12bit(0.0);
        const int q100 = nitsToPq12bit(100.0);
        const int q1000 = nitsToPq12bit(1000.0);
        const int qPeak = nitsToPq12bit(kPqPeakNits);
        const bool ranged = q0 >= 0 && qPeak <= 4095 && q100 >= 0 && q1000 <= 4095;
        const bool ends = q0 == 0 && qPeak == 4095;
        const bool monotonic = q0 < q100 && q100 < q1000 && q1000 < qPeak;
        const bool canonical = q100 == 2081 && q1000 == 3079;
        if (ranged && ends && monotonic && canonical) {
            pass("G4 nitsToPq12bit range/endpoints/monotonic");
        } else {
            fail("G4 pq12bit", QStringLiteral("q0=%1 q100=%2 q1000=%3 qPeak=%4")
                    .arg(q0).arg(q100).arg(q1000).arg(qPeak));
        }
    }

    const DolbyVisionMetadata meta = buildSampleMeta();
    const QString xml = toDolbyVisionXml(meta, 24.0);

    // G5: 生成 XML に必須要素が含まれる。
    {
        const bool ok = xml.contains(QStringLiteral("<DolbyLabsMDF version=\"4.0.2\" xmlns=\"http://www.dolby.com/schemas/dvmd/4_0_2\">"))
            && !xml.contains(QStringLiteral("DolbyLabMDF"))
            && xml.contains(QStringLiteral("<Title>DV &amp; &lt;Test&gt; &quot;Sequence&quot;</Title>"))
            && xml.contains(QStringLiteral("<Record inFrame=\"0\" outFrame=\"48\"/>"))
            && xml.contains(QStringLiteral("<Record inFrame=\"48\" outFrame=\"132\"/>"))
            && xml.contains(QStringLiteral("<Shot>"))
            && xml.contains(QStringLiteral("Level1"))
            && xml.contains(QStringLiteral("Level6"))
            && xml.contains(QStringLiteral("MaxCLL"));
        if (ok) {
            pass("G5 XML contains required elements");
        } else {
            fail("G5 xml elements", QStringLiteral("len=%1").arg(xml.size()));
        }
    }

    // G6: L1 を変えると XML 出力 (PQ12bit) が変わる。
    {
        DolbyVisionMetadata m2 = meta;
        m2.shots[0].l1.maxNits = meta.shots[0].l1.maxNits + 500.0; // L1 Max を上げる
        const QString xml2 = toDolbyVisionXml(m2, 24.0);
        // 元 / 変更後それぞれの PQ12bit 値が XML に現れることを確認。
        const int origQ = nitsToPq12bit(meta.shots[0].l1.maxNits);
        const int newQ = nitsToPq12bit(m2.shots[0].l1.maxNits);
        const bool changed = (xml != xml2)
            && xml.contains(QStringLiteral("maxPQ=\"%1\"").arg(origQ))
            && xml2.contains(QStringLiteral("maxPQ=\"%1\"").arg(newQ));
        if (changed && origQ != newQ) {
            pass("G6 L1 change reflected in XML PQ12bit");
        } else {
            fail("G6 l1 reflect", QStringLiteral("origQ=%1 newQ=%2 differ=%3")
                    .arg(origQ).arg(newQ).arg(xml != xml2));
        }
    }

    // G7: L6 の MaxCLL / MaxFALL / MasteringDisplay が XML に出る。
    {
        const bool ok = xml.contains(QStringLiteral("<MaxCLL>1200</MaxCLL>"))
            && xml.contains(QStringLiteral("<MaxFALL>380</MaxFALL>"))
            && xml.contains(QStringLiteral("MasteringDisplay"))
            && xml.contains(QString::number(meta.l6.masteringMaxNits))
            && xml.contains(QString::number(meta.l6.masteringMinNits));
        if (ok) {
            pass("G7 L6 CLL/FALL/MasteringDisplay in XML");
        } else {
            fail("G7 l6 xml", QStringLiteral("maxCll=%1 maxFall=%2")
                    .arg(meta.l6.maxCll).arg(meta.l6.maxFall));
        }
    }

    // G8: 複数 shot が XML に列挙される (shots.size() 個の <Shot> 要素)。
    {
        int count = 0;
        int from = 0;
        const QString token = QStringLiteral("<Shot>");
        while ((from = xml.indexOf(token, from)) >= 0) {
            ++count;
            from += token.size();
        }
        if (count == meta.shots.size()) {
            pass("G8 XML enumerates all shots");
        } else {
            fail("G8 shot count", QStringLiteral("xmlShots=%1 metaShots=%2")
                    .arg(count).arg(meta.shots.size()));
        }
    }

    // G9: validate 正常系 true / 異常系 false + errorOut。
    {
        QString err;
        const bool okValid = validate(meta, &err);

        // 異常1: profile 不正
        DolbyVisionMetadata badProfile = meta;
        badProfile.profile = 42;
        QString errProfile;
        const bool profileRejected = !validate(badProfile, &errProfile) && !errProfile.isEmpty();

        // 異常2: L1 min > max
        DolbyVisionMetadata badL1 = meta;
        badL1.shots[0].l1.minNits = 9999.0;
        badL1.shots[0].l1.maxNits = 10.0;
        QString errL1;
        const bool l1Rejected = !validate(badL1, &errL1) && !errL1.isEmpty();

        // 異常3: shot 時間逆転
        DolbyVisionMetadata badShot = meta;
        badShot.shots[1].startSec = 9.0;
        badShot.shots[1].endSec = 3.0;
        QString errShot;
        const bool shotRejected = !validate(badShot, &errShot) && !errShot.isEmpty();

        DolbyVisionMetadata badL1Range = meta;
        badL1Range.shots[0].l1.maxNits = 10001.0;
        QString errL1Range;
        const bool l1RangeRejected = !validate(badL1Range, &errL1Range) && !errL1Range.isEmpty();

        DolbyVisionMetadata badL6 = meta;
        badL6.l6.maxFall = badL6.l6.maxCll + 1;
        QString errL6;
        const bool l6Rejected = !validate(badL6, &errL6) && !errL6.isEmpty();

        DolbyVisionMetadata badL6Range = meta;
        badL6Range.l6.maxCll = 10001;
        QString errL6Range;
        const bool l6RangeRejected = !validate(badL6Range, &errL6Range) && !errL6Range.isEmpty();

        DolbyVisionMetadata badL2 = meta;
        badL2.shots[0].trims[0].slope = 1.5;
        QString errL2;
        const bool l2Rejected = !validate(badL2, &errL2) && !errL2.isEmpty();

        if (okValid && profileRejected && l1Rejected && shotRejected
            && l1RangeRejected && l6Rejected && l6RangeRejected && l2Rejected) {
            pass("G9 validate accepts valid, rejects invalid with errorOut");
        } else {
            fail("G9 validate",
                 QStringLiteral("valid=%1 profile=%2 l1=%3 shot=%4 l1Range=%5 l6=%6 l6Range=%7 l2=%8 err=%9")
                    .arg(okValid).arg(profileRejected).arg(l1Rejected).arg(shotRejected)
                    .arg(l1RangeRejected).arg(l6Rejected).arg(l6RangeRejected)
                    .arg(l2Rejected).arg(err));
        }
    }

    // G10: toJson/fromJson round-trip。
    {
        const QJsonObject obj = toJson(meta);
        const DolbyVisionMetadata back = fromJson(obj);
        const bool profileOk = back.profile == meta.profile;
        const bool levelOk = back.level == meta.level;
        const bool titleOk = back.title == meta.title;
        const bool l6Ok = back.l6.maxCll == meta.l6.maxCll
            && back.l6.maxFall == meta.l6.maxFall
            && back.l6.masteringMaxNits == meta.l6.masteringMaxNits
            && back.l6.masteringMinNits == meta.l6.masteringMinNits;
        const bool shotsOk = back.shots.size() == meta.shots.size();
        bool fieldsOk = shotsOk;
        if (shotsOk) {
            fieldsOk = approxEqual(back.shots[0].l1.maxNits, meta.shots[0].l1.maxNits, 1e-6)
                && approxEqual(back.shots[0].startSec, meta.shots[0].startSec, 1e-6)
                && approxEqual(back.shots[1].endSec, meta.shots[1].endSec, 1e-6)
                && back.shots[0].trims.size() == meta.shots[0].trims.size()
                && back.shots[0].l5.top == meta.shots[0].l5.top
                && back.shots[0].l5.bottom == meta.shots[0].l5.bottom
                && back.shots[0].trims[0].targetNits == meta.shots[0].trims[0].targetNits
                && approxEqual(back.shots[0].trims[0].slope, meta.shots[0].trims[0].slope, 1e-6)
                && approxEqual(back.shots[0].trims[0].offset, meta.shots[0].trims[0].offset, 1e-6)
                && approxEqual(back.shots[0].trims[0].power, meta.shots[0].trims[0].power, 1e-6);
        }
        if (profileOk && levelOk && titleOk && l6Ok && shotsOk && fieldsOk) {
            pass("G10 toJson/fromJson round-trip");
        } else {
            fail("G10 json round-trip", QStringLiteral("profile=%1 level=%2 title=%3 l6=%4 shots=%5 fields=%6")
                    .arg(profileOk).arg(levelOk).arg(titleOk).arg(l6Ok).arg(shotsOk).arg(fieldsOk));
        }
    }

    // G11: dolbyVision キー欠落相当の空 object 読み込みは既定メタデータに戻る。
    {
        const DolbyVisionMetadata missing = fromJson(QJsonObject{});
        const bool ok = missing.profile == 81
            && missing.level == 6
            && missing.title.isEmpty()
            && missing.shots.isEmpty()
            && missing.l6.maxCll == 0
            && missing.l6.maxFall == 0
            && missing.l6.masteringMaxNits == 1000
            && missing.l6.masteringMinNits == 0;
        if (ok) {
            pass("G11 missing dolbyVision defaults");
        } else {
            fail("G11 missing defaults",
                 QStringLiteral("profile=%1 level=%2 shots=%3 maxCll=%4")
                    .arg(missing.profile).arg(missing.level)
                    .arg(missing.shots.size()).arg(missing.l6.maxCll));
        }
    }

    qInfo().noquote().nospace() << "[dolby-vision] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
