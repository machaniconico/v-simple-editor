// Safe zone headless selftest.
//
// QApplication 不要。safezone:: の純粋 guides()/apply() を検証する。

#include <QDebug>
#include <QImage>
#include <QString>
#include <QVector>
#include <cmath>

#include "../SafeZone.h"

namespace {

bool near(double actual, double expected, double tolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

} // namespace

int runSafeZoneSelftest()
{
    qInfo().noquote() << "[safe-zone] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[safe-zone] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[safe-zone] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok)
            pass(name);
        else
            fail(name, msg);
    };

    // G1: Platform::None → guides() 空
    {
        const QVector<safezone::Guide> gs = safezone::guides(QSize(1080, 1920), safezone::Platform::None);
        check("G1 Platform::None produces empty guides",
              gs.isEmpty(),
              QStringLiteral("count=%1").arg(gs.size()));
    }

    // G2: TikTok(1080x1920) → TitleSafe (54,96,972,1728), ActionSafe (108,192,864,1536)
    {
        const QSize sz(1080, 1920);
        const QVector<safezone::Guide> gs = safezone::guides(sz, safezone::Platform::TikTok);

        safezone::Guide titleSafe;
        safezone::Guide actionSafe;
        bool foundTitle = false, foundAction = false;
        for (const safezone::Guide& g : gs) {
            if (g.kind == safezone::GuideKind::TitleSafe)  { titleSafe = g;  foundTitle  = true; }
            if (g.kind == safezone::GuideKind::ActionSafe) { actionSafe = g; foundAction = true; }
        }

        const double tol = 1.5; // 丸め誤差
        const bool titleOk = foundTitle
            && near(titleSafe.rect.x(),      54.0,   tol)
            && near(titleSafe.rect.y(),      96.0,   tol)
            && near(titleSafe.rect.width(),  972.0,  tol)
            && near(titleSafe.rect.height(), 1728.0, tol);
        const bool actionOk = foundAction
            && near(actionSafe.rect.x(),      108.0,  tol)
            && near(actionSafe.rect.y(),      192.0,  tol)
            && near(actionSafe.rect.width(),  864.0,  tol)
            && near(actionSafe.rect.height(), 1536.0, tol);

        check("G2 TikTok TitleSafe rect ~(54,96,972,1728)",
              titleOk,
              QStringLiteral("found=%1 x=%2 y=%3 w=%4 h=%5")
                  .arg(foundTitle)
                  .arg(foundTitle ? titleSafe.rect.x()      : -1, 0, 'f', 2)
                  .arg(foundTitle ? titleSafe.rect.y()      : -1, 0, 'f', 2)
                  .arg(foundTitle ? titleSafe.rect.width()  : -1, 0, 'f', 2)
                  .arg(foundTitle ? titleSafe.rect.height() : -1, 0, 'f', 2));
        check("G2 TikTok ActionSafe rect ~(108,192,864,1536)",
              actionOk,
              QStringLiteral("found=%1 x=%2 y=%3 w=%4 h=%5")
                  .arg(foundAction)
                  .arg(foundAction ? actionSafe.rect.x()      : -1, 0, 'f', 2)
                  .arg(foundAction ? actionSafe.rect.y()      : -1, 0, 'f', 2)
                  .arg(foundAction ? actionSafe.rect.width()  : -1, 0, 'f', 2)
                  .arg(foundAction ? actionSafe.rect.height() : -1, 0, 'f', 2));
    }

    // G3: ActionSafe ⊂ TitleSafe (action が title に内包)
    {
        const QVector<safezone::Guide> gs = safezone::guides(QSize(1080, 1920), safezone::Platform::TikTok);
        QRectF title, action;
        for (const safezone::Guide& g : gs) {
            if (g.kind == safezone::GuideKind::TitleSafe)  title  = g.rect;
            if (g.kind == safezone::GuideKind::ActionSafe) action = g.rect;
        }
        const bool contained = title.contains(action);
        check("G3 ActionSafe contained within TitleSafe",
              contained,
              QStringLiteral("title=(%1,%2,%3,%4) action=(%5,%6,%7,%8)")
                  .arg(title.x(),0,'f',1).arg(title.y(),0,'f',1)
                  .arg(title.width(),0,'f',1).arg(title.height(),0,'f',1)
                  .arg(action.x(),0,'f',1).arg(action.y(),0,'f',1)
                  .arg(action.width(),0,'f',1).arg(action.height(),0,'f',1));
    }

    // G4: TikTok に PlatformUi が >=3 個、下部帯の bottom ≈ H (5px 以内)
    {
        const int H = 1920;
        const QVector<safezone::Guide> gs = safezone::guides(QSize(1080, H), safezone::Platform::TikTok);
        int uiCount = 0;
        double bottomEdge = 0.0;
        for (const safezone::Guide& g : gs) {
            if (g.kind == safezone::GuideKind::PlatformUi) {
                ++uiCount;
                const double b = g.rect.y() + g.rect.height();
                if (b > bottomEdge)
                    bottomEdge = b;
            }
        }
        const bool countOk = uiCount >= 3;
        const bool bottomOk = near(bottomEdge, static_cast<double>(H), 5.0);
        check("G4 TikTok has >=3 PlatformUi guides",
              countOk,
              QStringLiteral("uiCount=%1").arg(uiCount));
        check("G4 TikTok bottom guide touches bottom edge",
              bottomOk,
              QStringLiteral("bottomEdge=%1 H=%2").arg(bottomEdge,0,'f',2).arg(H));
    }

    // G5: Generic → PlatformUi 0 個、TitleSafe と ActionSafe あり
    {
        const QVector<safezone::Guide> gs = safezone::guides(QSize(1080, 1920), safezone::Platform::Generic);
        int uiCount = 0;
        bool hasTitle = false, hasAction = false;
        for (const safezone::Guide& g : gs) {
            if (g.kind == safezone::GuideKind::PlatformUi) ++uiCount;
            if (g.kind == safezone::GuideKind::TitleSafe)  hasTitle  = true;
            if (g.kind == safezone::GuideKind::ActionSafe) hasAction = true;
        }
        check("G5 Generic has 0 PlatformUi guides",
              uiCount == 0,
              QStringLiteral("uiCount=%1").arg(uiCount));
        check("G5 Generic has TitleSafe and ActionSafe",
              hasTitle && hasAction,
              QStringLiteral("hasTitle=%1 hasAction=%2").arg(hasTitle).arg(hasAction));
    }

    // G6: スケール不変性 — TitleSafe.width / imageWidth ≈ 0.90 (各サイズで)
    {
        const QList<QSize> sizes = { QSize(720, 1280), QSize(540, 960) };
        for (const QSize& sz : sizes) {
            const QVector<safezone::Guide> gs = safezone::guides(sz, safezone::Platform::TikTok);
            for (const safezone::Guide& g : gs) {
                if (g.kind == safezone::GuideKind::TitleSafe) {
                    const double ratio = g.rect.width() / sz.width();
                    check(QStringLiteral("G6 TitleSafe width ratio ~0.90 at %1x%2")
                              .arg(sz.width()).arg(sz.height()).toUtf8().constData(),
                          near(ratio, 0.90, 0.001),
                          QStringLiteral("ratio=%1").arg(ratio, 0, 'f', 6));
                    break;
                }
            }
        }
    }

    // G7: apply() — None は pixel 同一、TikTok は差分あり
    {
        QImage img(100, 100, QImage::Format_ARGB32);
        img.fill(QColor(128, 64, 32, 255));

        // None → byte-identical copy
        const QImage resultNone = safezone::apply(img, safezone::Platform::None);
        const bool sameSize   = (resultNone.size() == img.size());
        const bool sameFmt    = (resultNone.format() == img.format());
        const bool samePixels = (resultNone == img);
        check("G7 apply None returns same-size same-format image",
              sameSize && sameFmt,
              QStringLiteral("sameSize=%1 sameFmt=%2").arg(sameSize).arg(sameFmt));
        check("G7 apply None returns pixel-identical image",
              samePixels,
              QStringLiteral("pixelIdentical=%1").arg(samePixels));

        // TikTok → 何か描画されて差分あり
        const QImage resultTikTok = safezone::apply(img, safezone::Platform::TikTok);
        const bool tiktokDiffs = (resultTikTok != img);
        check("G7 apply TikTok returns image with drawn guides (differs from input)",
              tiktokDiffs && resultTikTok.size() == img.size(),
              QStringLiteral("differs=%1 sameSize=%2")
                  .arg(tiktokDiffs)
                  .arg(resultTikTok.size() == img.size()));
    }

    qInfo().noquote() << "[safe-zone] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
