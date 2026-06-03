// ClipColor headless selftest.
//
// QApplication 不要・QtCore のみ。clipcolor:: の純粋モデルを JSON round-trip、
// token 逆写像、default SDR 判定、describe 非空性で検証する。

#include <QDebug>
#include <QJsonObject>
#include <QString>

#include "../color/ClipColor.h"

int runClipColorSelftest()
{
    qInfo().noquote() << "[clip-color] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[clip-color] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[clip-color] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok)
            pass(name);
        else
            fail(name, msg);
    };

    const clipcolor::ColorMeta sdr = clipcolor::defaultSdr();
    clipcolor::ColorMeta hdr;
    hdr.primaries = clipcolor::Primaries::Rec2020;
    hdr.transfer = clipcolor::Transfer::PQ;
    hdr.bitDepth = 10;
    hdr.isHdr = true;

    check("G1 defaultSdr is Rec709/sRGB/8-bit SDR",
          sdr.primaries == clipcolor::Primaries::Rec709
          && sdr.transfer == clipcolor::Transfer::sRGB
          && sdr.bitDepth == 8
          && !sdr.isHdr);

    check("G2 defaultSdr().isDefault()",
          sdr.isDefault());

    check("G3 Rec2020/PQ/10/HDR is non-default",
          !hdr.isDefault());

    check("G4 JSON round-trip defaultSdr",
          clipcolor::fromJson(clipcolor::toJson(sdr)) == sdr);

    check("G5 JSON round-trip HDR meta",
          clipcolor::fromJson(clipcolor::toJson(hdr)) == hdr);

    {
        const clipcolor::Primaries values[] = {
            clipcolor::Primaries::Rec709,
            clipcolor::Primaries::Rec2020,
            clipcolor::Primaries::DisplayP3,
            clipcolor::Primaries::ACEScg
        };
        bool ok = true;
        for (clipcolor::Primaries p : values)
            ok = ok && clipcolor::primariesFromToken(clipcolor::primariesToken(p)) == p;
        check("G6 all Primaries token->fromToken inverse",
              ok);
    }

    {
        const clipcolor::Transfer values[] = {
            clipcolor::Transfer::sRGB,
            clipcolor::Transfer::Rec709,
            clipcolor::Transfer::PQ,
            clipcolor::Transfer::HLG,
            clipcolor::Transfer::Linear
        };
        bool ok = true;
        for (clipcolor::Transfer t : values)
            ok = ok && clipcolor::transferFromToken(clipcolor::transferToken(t)) == t;
        check("G7 all Transfer token->fromToken inverse",
              ok);
    }

    check("G8 garbage token uses fallback",
          clipcolor::primariesFromToken(QStringLiteral("bogus"), clipcolor::Primaries::ACEScg)
              == clipcolor::Primaries::ACEScg
          && clipcolor::transferFromToken(QStringLiteral("bogus"), clipcolor::Transfer::HLG)
              == clipcolor::Transfer::HLG);

    check("G9 fromJson({}) fills defaultSdr",
          clipcolor::fromJson(QJsonObject()) == sdr);

    check("G10 describe is non-empty for SDR and HDR",
          !clipcolor::describe(sdr).isEmpty()
          && !clipcolor::describe(hdr).isEmpty());

    {
        clipcolor::ColorMeta bitDepthDiff = sdr;
        bitDepthDiff.bitDepth = 10;
        clipcolor::ColorMeta hdrFlagDiff = sdr;
        hdrFlagDiff.isHdr = true;
        check("G11 operator==/!= distinguish bitDepth and isHdr",
              sdr == clipcolor::defaultSdr()
              && sdr != bitDepthDiff
              && sdr != hdrFlagDiff);
    }

    {
        const QJsonObject json = clipcolor::toJson(hdr);
        check("G12 toJson emits stable tokens",
              json.value(QStringLiteral("primaries")).toString() == QStringLiteral("rec2020")
              && json.value(QStringLiteral("transfer")).toString() == QStringLiteral("pq"));
    }

    qInfo().noquote() << "[clip-color] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
