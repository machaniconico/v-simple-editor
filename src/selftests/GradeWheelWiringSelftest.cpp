#include "../ColorGradingPanel.h"
#include "../EffectPreset.h"
#include "../VideoEffect.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QJsonObject>
#include <QSlider>
#include <QTimer>
#include <QVector>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace {

bool near(double a, double b, double eps = 1e-6)
{
    return std::abs(a - b) <= eps;
}

double sliderToGammaForTest(int v)
{
    if (v <= 50)
        return 0.1 * std::pow(10.0, std::clamp(v / 50.0, 0.0, 1.0));
    return std::pow(4.0, std::clamp((v - 50) / 50.0, 0.0, 1.0));
}

void pumpEvents(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

QSlider *slider(ColorGradingPanel &panel, const char *name)
{
    return panel.findChild<QSlider *>(QString::fromLatin1(name));
}

void check(int gate, const char *name, bool ok, int &passed, int &failed,
           const QString &detail = QString())
{
    if (ok) {
        ++passed;
        qInfo().noquote() << QStringLiteral("[grade-wheel-wiring] PASS G%1 %2")
            .arg(gate).arg(QString::fromLatin1(name));
        return;
    }
    ++failed;
    qCritical().noquote() << QStringLiteral("[grade-wheel-wiring] FAIL G%1 %2%3")
        .arg(gate).arg(QString::fromLatin1(name))
        .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
}

} // namespace

int runGradeWheelWiringSelftest()
{
    int argc = 1;
    char arg0[] = "grade-wheel-wiring";
    char *argv[] = {arg0, nullptr};
    std::unique_ptr<QApplication> ownedApp;
    if (!qobject_cast<QApplication *>(QCoreApplication::instance())) {
        if (QCoreApplication::instance()) {
            qCritical() << "[grade-wheel-wiring] QApplication is required";
            return 1;
        }
        qputenv("QT_QPA_PLATFORM", "offscreen");
        ownedApp = std::make_unique<QApplication>(argc, argv);
    }

    int passed = 0;
    int failed = 0;

    ColorGradingPanel panel;
    int wheelSignals = 0;
    QObject::connect(&panel, &ColorGradingPanel::colorWheelsChanged,
                     [&wheelSignals](const ColorWheels &) { ++wheelSignals; });

    QSlider *liftR = slider(panel, "lggLiftR");
    QSlider *gammaG = slider(panel, "lggGammaG");
    QSlider *gainB = slider(panel, "lggGainB");
    QSlider *wbTemperature = slider(panel, "wbTemperature");
    QSlider *wbTint = slider(panel, "wbTint");
    check(1, "test controls are discoverable",
          liftR && gammaG && gainB && wbTemperature && wbTint,
          passed, failed);
    if (failed != 0)
        return failed;

    liftR->setValue(20);
    gammaG->setValue(75);
    gainB->setValue(-30);
    wbTemperature->setValue(25);
    wbTint->setValue(-12);
    pumpEvents(40);

    const ColorCorrection cc = panel.colorCorrection();
    const double expectedGammaG = std::log2(sliderToGammaForTest(75));
    check(2, "LGG sliders write ColorCorrection fields",
          near(cc.liftR, 0.20)
              && near(cc.gammaG, expectedGammaG)
              && near(cc.gainB, -0.30),
          passed, failed,
          QStringLiteral("liftR=%1 gammaG=%2 gainB=%3")
              .arg(cc.liftR).arg(cc.gammaG).arg(cc.gainB));
    check(3, "WB sliders write ColorCorrection temperature/tint",
          near(cc.temperature, 25.0) && near(cc.tint, -12.0),
          passed, failed,
          QStringLiteral("temperature=%1 tint=%2").arg(cc.temperature).arg(cc.tint));
    check(4, "wheel edit coalesces to one debounced signal",
          wheelSignals == 1, passed, failed,
          QStringLiteral("signals=%1").arg(wheelSignals));

    QImage input(1, 1, QImage::Format_RGB888);
    input.setPixelColor(0, 0, QColor(96, 128, 160));
    const QImage rendered = VideoEffectProcessor::applyColorCorrection(input, cc);
    check(5, "ColorCorrection changes render output",
          !rendered.isNull()
              && rendered.pixelColor(0, 0) != input.pixelColor(0, 0),
          passed, failed);

    ColorCorrection presetCc;
    presetCc.liftR = 0.20;
    presetCc.gammaG = expectedGammaG;
    presetCc.gainB = -0.30;
    const QJsonObject presetJson = PresetLibrary::colorCorrectionToJson(presetCc);
    check(6, "preset JSON writes only non-zero LGG fields",
          presetJson.contains(QStringLiteral("liftR"))
              && presetJson.contains(QStringLiteral("gammaG"))
              && presetJson.contains(QStringLiteral("gainB"))
              && !presetJson.contains(QStringLiteral("liftG"))
              && !presetJson.contains(QStringLiteral("gammaR"))
              && !presetJson.contains(QStringLiteral("gainR")),
          passed, failed);

    const ColorCorrection restoredPreset =
        PresetLibrary::colorCorrectionFromJson(presetJson);
    check(7, "preset JSON missing LGG fields restore as zero",
          near(restoredPreset.liftR, 0.20)
              && near(restoredPreset.gammaG, expectedGammaG)
              && near(restoredPreset.gainB, -0.30)
              && near(restoredPreset.liftG, 0.0)
              && near(restoredPreset.gammaR, 0.0)
              && near(restoredPreset.gainR, 0.0),
          passed, failed);

    qInfo().noquote() << QStringLiteral("[grade-wheel-wiring] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
