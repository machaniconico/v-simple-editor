// SwsColor headless selftest.
//
// QApplication 不要。swscale 色行列 helper の enum 解決と係数差分を検証する。

#include <QDebug>
#include <QString>

#include "../color/SwsColorParams.h"

int runSwsColorSelftest()
{
    qInfo().noquote() << "[sws-color] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[sws-color] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[sws-color] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok)
            pass(name);
        else
            fail(name, msg);
    };

    check("G1 UNSPEC 1080p resolves BT709",
          swscolor::resolveColorspace(AVCOL_SPC_UNSPECIFIED, 1920, 1080)
              == AVCOL_SPC_BT709);

    check("G2 UNSPEC 480p resolves SMPTE170M",
          swscolor::resolveColorspace(AVCOL_SPC_UNSPECIFIED, 720, 480)
              == AVCOL_SPC_SMPTE170M);

    check("G3 tagged BT2020_NCL is preserved",
          swscolor::resolveColorspace(AVCOL_SPC_BT2020_NCL, 3840, 2160)
              == AVCOL_SPC_BT2020_NCL);

    check("G4 UNSPEC UHD does not infer BT2020",
          swscolor::resolveColorspace(AVCOL_SPC_UNSPECIFIED, 3840, 2160)
              == AVCOL_SPC_BT709);

    check("G5 sws coefficient ids map 709/601/2020",
          swscolor::swsCoeffsId(AVCOL_SPC_BT709) == SWS_CS_ITU709
          && swscolor::swsCoeffsId(AVCOL_SPC_SMPTE170M) == SWS_CS_ITU601
          && swscolor::swsCoeffsId(AVCOL_SPC_BT2020_NCL) == SWS_CS_BT2020);

    {
        const swscolor::SdrTags hd = swscolor::sdrTagsFor(1920, 1080);
        const swscolor::SdrTags sd = swscolor::sdrTagsFor(640, 480);
        check("G6 SDR tags choose HD BT709 / SD SMPTE170M limited",
              hd.spc == AVCOL_SPC_BT709
              && hd.range == AVCOL_RANGE_MPEG
              && sd.spc == AVCOL_SPC_SMPTE170M
              && sd.range == AVCOL_RANGE_MPEG);
    }

    check("G7 range UNSPEC resolves MPEG, JPEG is preserved",
          swscolor::resolveRange(AVCOL_RANGE_UNSPECIFIED) == AVCOL_RANGE_MPEG
          && swscolor::resolveRange(AVCOL_RANGE_JPEG) == AVCOL_RANGE_JPEG);

    {
        const int* bt709 = sws_getCoefficients(swscolor::swsCoeffsId(AVCOL_SPC_BT709));
        const int* bt601 = sws_getCoefficients(swscolor::swsCoeffsId(AVCOL_SPC_SMPTE170M));
        bool differs = bt709 && bt601;
        if (differs) {
            differs = false;
            for (int i = 0; i < 4; ++i)
                differs = differs || bt709[i] != bt601[i];
        }
        check("G8 BT709 and BT601 coefficients differ",
              differs);
    }

    qInfo().noquote() << "[sws-color] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
