// AscCdlExport headless selftest.
//
// QApplication 不要・QtCore のみ。asccdl:: 純粋エンジンが返す XML 文字列を
// 「独立した最小 XML 走査」(QString::contains / 部分文字列カウント) で検証する。
// 被テストコードの出力をそのまま期待値へ流用しない (tautological 回避)。
//
// 検証対象: fromLgg (LGG -> SOP マッピング) / buildCc (.cc) / buildCcc (.ccc) / buildCdl (.cdl)。

#include <cmath>

#include <QString>
#include <QVector>
#include <QDebug>

#include "../colorexport/AscCdlExport.h"

namespace {

// 部分文字列 needle が hay に出現する回数を数える (重なりなし)。
int countOccurrences(const QString& hay, const QString& needle)
{
    if (needle.isEmpty()) {
        return 0;
    }
    int count = 0;
    int from = 0;
    while (true) {
        const int idx = hay.indexOf(needle, from);
        if (idx < 0) {
            break;
        }
        ++count;
        from = idx + needle.size();
    }
    return count;
}

// double 近似比較。
bool nearly(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) <= eps;
}

} // namespace

int runAscCdlExportSelftest()
{
    qInfo().noquote() << "[asc-cdl-export] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[asc-cdl-export] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[asc-cdl-export] FAIL" << name << ":" << msg; };

    // 恒等 LGG: lift=0, gamma=1, gain=0, sat=1。
    const double lift0[3]  = {0.0, 0.0, 0.0};
    const double gamma1[3] = {1.0, 1.0, 1.0};
    const double gain0[3]  = {0.0, 0.0, 0.0};

    // G1: 恒等 fromLgg は slope=1 offset=0 power=1 sat=1。
    {
        const asccdl::CdlCorrection c =
            asccdl::fromLgg(lift0, 0.0, gamma1, 1.0, gain0, 0.0, 1.0, QString());
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            ok = ok && nearly(c.slope[i], 1.0)
                    && nearly(c.offset[i], 0.0)
                    && nearly(c.power[i], 1.0);
        }
        ok = ok && nearly(c.saturation, 1.0);
        if (ok) {
            pass("G1 identity fromLgg -> slope=1 offset=0 power=1 sat=1");
        } else {
            fail("G1 identity", QStringLiteral("slope=%1,%2,%3 offset=%4,%5,%6 power=%7,%8,%9 sat=%10")
                    .arg(c.slope[0]).arg(c.slope[1]).arg(c.slope[2])
                    .arg(c.offset[0]).arg(c.offset[1]).arg(c.offset[2])
                    .arg(c.power[0]).arg(c.power[1]).arg(c.power[2])
                    .arg(c.saturation));
        }
    }

    // 恒等 buildCc を G2..G4 で共有。
    const asccdl::CdlCorrection ident =
        asccdl::fromLgg(lift0, 0.0, gamma1, 1.0, gain0, 0.0, 1.0, QString());
    const QString cc = asccdl::buildCc(ident);

    // G2: "<?xml" で始まり xmlns="urn:ASC:CDL:v1.01" を含む。
    {
        const bool startsXml = cc.startsWith(QStringLiteral("<?xml"));
        const bool hasNs     = cc.contains(QStringLiteral("xmlns=\"urn:ASC:CDL:v1.01\""));
        if (startsXml && hasNs) {
            pass("G2 buildCc starts with <?xml and has xmlns urn:ASC:CDL:v1.01");
        } else {
            fail("G2 header/ns", QStringLiteral("startsXml=%1 hasNs=%2 head='%3'")
                    .arg(startsXml).arg(hasNs).arg(cc.left(40)));
        }
    }

    // G3: SOPNode/Slope/Offset/Power/SatNode/Saturation が各 1 回以上。
    {
        struct Req { const char* tag; };
        const QString tags[] = {
            QStringLiteral("<SOPNode>"), QStringLiteral("<Slope>"),
            QStringLiteral("<Offset>"),  QStringLiteral("<Power>"),
            QStringLiteral("<SatNode>"), QStringLiteral("<Saturation>")
        };
        bool ok = true;
        QString missing;
        for (const QString& t : tags) {
            if (countOccurrences(cc, t) < 1) {
                ok = false;
                missing = t;
                break;
            }
        }
        if (ok) {
            pass("G3 each SOP/Sat element present at least once");
        } else {
            fail("G3 elements", QStringLiteral("missing element: %1").arg(missing));
        }
    }

    // G4: 恒等の Slope/Offset/Power トリプル文字列が規定整形。
    {
        const bool slopeOk  = cc.contains(QStringLiteral("1.000000 1.000000 1.000000"));
        const bool offsetOk = cc.contains(QStringLiteral("0.000000 0.000000 0.000000"));
        // Power も恒等は 1.000000 トリプル (slope と同文字列のため出現数 >= 2)。
        const bool powerOk  = countOccurrences(cc, QStringLiteral("1.000000 1.000000 1.000000")) >= 2;
        if (slopeOk && offsetOk && powerOk) {
            pass("G4 identity Slope/Offset/Power formatted to 6 decimals");
        } else {
            fail("G4 triples", QStringLiteral("slopeOk=%1 offsetOk=%2 powerOk=%3")
                    .arg(slopeOk).arg(offsetOk).arg(powerOk));
        }
    }

    // G5: 小数 6 桁整形 — slope[0]=1.234567 が "1.234567" を含む。
    {
        asccdl::CdlCorrection c;
        c.slope[0] = 1.234567;
        const QString xml = asccdl::buildCc(c);
        if (xml.contains(QStringLiteral("1.234567"))) {
            pass("G5 slope 1.234567 formatted to 6 decimals");
        } else {
            fail("G5 format", QStringLiteral("missing '1.234567' in: %1")
                    .arg(xml.left(200)));
        }
    }

    // G6: id 指定で id="cc0001" を含む / id 空なら id= 属性を含まない。
    //     XML 特殊文字入り id は属性値としてエスケープされる。
    {
        asccdl::CdlCorrection withId;
        withId.id = QStringLiteral("cc0001");
        const QString xmlWith = asccdl::buildCc(withId);
        const bool hasId = xmlWith.contains(QStringLiteral("id=\"cc0001\""));

        asccdl::CdlCorrection noId; // id 空 (既定)
        const QString xmlNo = asccdl::buildCc(noId);
        // id= 属性が一切無い (xmlns 等は contains しても 'id="' の形は無い)。
        const bool noIdAttr = !xmlNo.contains(QStringLiteral("id=\""));

        asccdl::CdlCorrection escapedId;
        escapedId.id = QStringLiteral("a&b<c\"d'e");
        const QString xmlEsc = asccdl::buildCc(escapedId);
        const bool escaped = xmlEsc.contains(QStringLiteral("id=\"a&amp;b&lt;c&quot;d'e\""))
                          || xmlEsc.contains(QStringLiteral("id=\"a&amp;b&lt;c&quot;d&apos;e\""));

        if (hasId && noIdAttr && escaped) {
            pass("G6 id attribute emitted/omitted and XML-special id escaped");
        } else {
            fail("G6 id", QStringLiteral("hasId=%1 noIdAttr=%2 escaped=%3 xmlEsc=%4")
                    .arg(hasId).arg(noIdAttr).arg(escaped).arg(xmlEsc.left(160)));
        }
    }

    // G7: buildCcc はルート <ColorCorrectionCollection を持ち、N 個入力で <ColorCorrection 出現数 = N。
    {
        QVector<asccdl::CdlCorrection> list;
        for (int i = 0; i < 3; ++i) {
            asccdl::CdlCorrection c;
            c.id = QStringLiteral("c%1").arg(i);
            list.append(c);
        }
        const QString xml = asccdl::buildCcc(list);
        const bool hasRoot = xml.contains(QStringLiteral("<ColorCorrectionCollection"));
        // "<ColorCorrection" は Collection ルートにも前方一致するため、属性区切りまで含めて区別する。
        // 子要素は "<ColorCorrection" の後に '>' か ' '(id 属性) が来る。Collection は
        // "<ColorCorrectionCollection" なので "<ColorCorrection " / "<ColorCorrection>" だけ数える。
        const int childCount = countOccurrences(xml, QStringLiteral("<ColorCorrection "))
                             + countOccurrences(xml, QStringLiteral("<ColorCorrection>"));
        if (hasRoot && childCount == list.size()) {
            pass("G7 buildCcc root + child ColorCorrection count == N");
        } else {
            fail("G7 ccc", QStringLiteral("hasRoot=%1 childCount=%2 expected=%3")
                    .arg(hasRoot).arg(childCount).arg(list.size()));
        }
    }

    // G8: buildCdl はルート <ColorDecisionList と <ColorDecision を持ち、N 個入力で <ColorDecision 出現数 = N。
    {
        QVector<asccdl::CdlCorrection> list;
        for (int i = 0; i < 4; ++i) {
            list.append(asccdl::CdlCorrection());
        }
        const QString xml = asccdl::buildCdl(list);
        const bool hasRoot = xml.contains(QStringLiteral("<ColorDecisionList"));
        // "<ColorDecision" は List ルートに前方一致するため、"<ColorDecision>" のみ数える
        // (各 ColorDecision は属性なしで開く)。
        const int decCount = countOccurrences(xml, QStringLiteral("<ColorDecision>"));
        if (hasRoot && decCount == list.size()) {
            pass("G8 buildCdl root + ColorDecision count == N");
        } else {
            fail("G8 cdl", QStringLiteral("hasRoot=%1 decCount=%2 expected=%3")
                    .arg(hasRoot).arg(decCount).arg(list.size()));
        }
    }

    // G9: マッピング方向 — GLPreview 実効値に合わせる。
    //     gain+0.5 -> slope≈2 / lift+0.1 -> offset≈0.05 / gamma=2.0 -> power≈0.5。
    {
        const double liftP[3]  = {0.1, 0.1, 0.1};
        const double gamma2[3] = {2.0, 2.0, 2.0};
        const double gainP[3]  = {0.5, 0.5, 0.5};
        const asccdl::CdlCorrection c =
            asccdl::fromLgg(liftP, 0.0, gamma2, 1.0, gainP, 0.0, 1.0, QString());
        const bool slopeTwo   = nearly(c.slope[0], 2.0);
        const bool offsetHalf = nearly(c.offset[0], 0.05);
        const bool powerHalf  = nearly(c.power[0], 0.5);
        if (slopeTwo && offsetHalf && powerHalf) {
            pass("G9 mapping direction: gain+0.5->slope~2, lift+0.1->offset~0.05, gamma2->power~0.5");
        } else {
            fail("G9 mapping", QStringLiteral("slope=%1 offset=%2 power=%3")
                    .arg(c.slope[0]).arg(c.offset[0]).arg(c.power[0]));
        }
    }

    // G10: power の 0/負入力ガード — shader と同じ 1e-3 分母 floor で finite な 1000 になる。
    {
        const double gammaZero[3] = {0.0, 0.0, 0.0};
        const asccdl::CdlCorrection c =
            asccdl::fromLgg(lift0, 0.0, gammaZero, 1.0, gain0, 0.0, 1.0, QString());
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            const double p = c.power[i];
            ok = ok && nearly(p, 1000.0) && std::isfinite(p);
        }
        // 負入力も同様にガードされること。
        const double gammaNeg[3] = {-3.0, -3.0, -3.0};
        const asccdl::CdlCorrection cn =
            asccdl::fromLgg(lift0, 0.0, gammaNeg, 1.0, gain0, 0.0, 1.0, QString());
        for (int i = 0; i < 3; ++i) {
            const double p = cn.power[i];
            ok = ok && nearly(p, 1000.0) && std::isfinite(p);
        }
        if (ok) {
            pass("G10 power guard: gamma 0/negative -> power=1000, finite");
        } else {
            fail("G10 power guard", QStringLiteral("power0=%1 powerNeg=%2")
                    .arg(c.power[0]).arg(cn.power[0]));
        }
    }

    // G11: saturation 負入力は 0 にクランプ。
    {
        const asccdl::CdlCorrection c =
            asccdl::fromLgg(lift0, 0.0, gamma1, 1.0, gain0, 0.0, -0.5, QString());
        if (nearly(c.saturation, 0.0)) {
            pass("G11 negative saturation clamped to 0");
        } else {
            fail("G11 sat clamp", QStringLiteral("saturation=%1").arg(c.saturation));
        }
    }

    // G12: slope の大きな負入力は shader と同じ指数変換で正の有限値になる。
    {
        const double gainBig[3] = {-5.0, -5.0, -5.0};
        const asccdl::CdlCorrection c =
            asccdl::fromLgg(lift0, 0.0, gamma1, 1.0, gainBig, 0.0, 1.0, QString());
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            ok = ok && nearly(c.slope[i], 0.0009765625) && std::isfinite(c.slope[i]);
        }
        if (ok) {
            pass("G12 negative gain maps to positive finite shader multiplier");
        } else {
            fail("G12 slope clamp", QStringLiteral("slope=%1,%2,%3")
                    .arg(c.slope[0]).arg(c.slope[1]).arg(c.slope[2]));
        }
    }

    // G13: buildCdl は各補正を <ColorDecision><ColorCorrection>...</ColorCorrection></ColorDecision> で包む。
    {
        QVector<asccdl::CdlCorrection> list;
        list.append(asccdl::CdlCorrection());
        list.append(asccdl::CdlCorrection());
        const QString xml = asccdl::buildCdl(list);
        // ColorDecision の数だけ ColorCorrection 子要素が入れ子になる。
        const int decCount = countOccurrences(xml, QStringLiteral("<ColorDecision>"));
        const int ccCount  = countOccurrences(xml, QStringLiteral("<ColorCorrection>"))
                           + countOccurrences(xml, QStringLiteral("<ColorCorrection "));
        if (decCount == list.size() && ccCount == list.size()) {
            pass("G13 buildCdl wraps each correction in ColorDecision>ColorCorrection");
        } else {
            fail("G13 nesting", QStringLiteral("decCount=%1 ccCount=%2 expected=%3")
                    .arg(decCount).arg(ccCount).arg(list.size()));
        }
    }

    qInfo().noquote().nospace() << "[asc-cdl-export] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
