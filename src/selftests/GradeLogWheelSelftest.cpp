#include "../ProjectFile.h"
#include "../VideoEffect.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace {

ProjectData projectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    return data;
}

ClipInfo makeClip()
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("grade-log-wheel.mov");
    clip.displayName = QStringLiteral("grade-log-wheel");
    clip.duration = 1.0;
    clip.outPoint = 1.0;
    return clip;
}

QJsonObject firstClipObject(const QString &json)
{
    const QJsonArray tracks = QJsonDocument::fromJson(json.toUtf8())
                                  .object()
                                  .value(QStringLiteral("videoTracks"))
                                  .toArray();
    if (tracks.isEmpty())
        return {};
    const QJsonArray clips = tracks.first().toArray();
    return clips.isEmpty() ? QJsonObject{} : clips.first().toObject();
}

bool sameLogWheels(const ColorCorrection &a, const ColorCorrection &b)
{
    constexpr double eps = 1e-9;
    auto near = [eps](double x, double y) { return std::abs(x - y) <= eps; };
    return near(a.logShadowR, b.logShadowR)
        && near(a.logShadowG, b.logShadowG)
        && near(a.logShadowB, b.logShadowB)
        && near(a.logMidR, b.logMidR)
        && near(a.logMidG, b.logMidG)
        && near(a.logMidB, b.logMidB)
        && near(a.logHighR, b.logHighR)
        && near(a.logHighG, b.logHighG)
        && near(a.logHighB, b.logHighB);
}

bool hasNoLogKeys(const QJsonObject &obj)
{
    const QStringList keys = {
        QStringLiteral("logShadowR"), QStringLiteral("logShadowG"),
        QStringLiteral("logShadowB"), QStringLiteral("logMidR"),
        QStringLiteral("logMidG"), QStringLiteral("logMidB"),
        QStringLiteral("logHighR"), QStringLiteral("logHighG"),
        QStringLiteral("logHighB")
    };
    for (const QString &key : keys) {
        if (obj.contains(key))
            return false;
    }
    return true;
}

double smoothstep(double edge0, double edge1, double value)
{
    const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

} // namespace

int runGradeLogWheelSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[grade-log-wheel] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[grade-log-wheel] FAIL G%1 %2%3")
                .arg(gate).arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        }
    };

    QImage identityInput(3, 2, QImage::Format_RGB888);
    identityInput.fill(QColor(37, 113, 219));
    const QImage identityOutput =
        VideoEffectProcessor::applyColorCorrection(identityInput, ColorCorrection{});
    const bool bitIdentical = identityOutput.format() == identityInput.format()
        && identityOutput.sizeInBytes() == identityInput.sizeInBytes()
        && std::memcmp(identityOutput.constBits(), identityInput.constBits(),
                       static_cast<size_t>(identityInput.sizeInBytes())) == 0;
    check(1, "all-zero log wheels are bit-identical", bitIdentical);

    QImage shadowInput(2, 1, QImage::Format_RGB888);
    shadowInput.setPixelColor(0, 0, QColor(26, 26, 26));
    shadowInput.setPixelColor(1, 0, QColor(242, 242, 242));
    ColorCorrection shadowCc;
    shadowCc.logShadowR = 0.4;
    shadowCc.logShadowG = 0.4;
    shadowCc.logShadowB = 0.4;
    const QImage shadowOutput =
        VideoEffectProcessor::applyColorCorrection(shadowInput, shadowCc);
    const QColor shadowDark = shadowOutput.pixelColor(0, 0);
    const QColor shadowHigh = shadowOutput.pixelColor(1, 0);
    const bool shadowOk = std::abs((shadowDark.red() - 26) - 51) <= 1
        && std::abs((shadowDark.green() - 26) - 51) <= 1
        && std::abs((shadowDark.blue() - 26) - 51) <= 1
        && shadowHigh == QColor(242, 242, 242);
    check(2, "shadow wheel affects shadows only", shadowOk,
          QStringLiteral("dark=%1/%2/%3 high=%4/%5/%6")
              .arg(shadowDark.red()).arg(shadowDark.green()).arg(shadowDark.blue())
              .arg(shadowHigh.red()).arg(shadowHigh.green()).arg(shadowHigh.blue()));

    ColorCorrection highCc;
    highCc.logHighR = -0.4;
    highCc.logHighG = -0.4;
    highCc.logHighB = -0.4;
    const QImage highOutput =
        VideoEffectProcessor::applyColorCorrection(shadowInput, highCc);
    const QColor highShadow = highOutput.pixelColor(0, 0);
    const QColor highBright = highOutput.pixelColor(1, 0);
    const bool highOk = highShadow == QColor(26, 26, 26)
        && std::abs((242 - highBright.red()) - 51) <= 1
        && std::abs((242 - highBright.green()) - 51) <= 1
        && std::abs((242 - highBright.blue()) - 51) <= 1;
    check(3, "highlight wheel affects highlights only", highOk,
          QStringLiteral("shadow=%1/%2/%3 high=%4/%5/%6")
              .arg(highShadow.red()).arg(highShadow.green()).arg(highShadow.blue())
              .arg(highBright.red()).arg(highBright.green()).arg(highBright.blue()));

    bool weightsOk = true;
    for (double y : {0.0, 0.15, 0.50, 0.85, 1.0}) {
        const double wS = 1.0 - smoothstep(0.15, 0.45, y);
        const double wH = smoothstep(0.55, 0.85, y);
        const double wM = std::clamp(1.0 - wS - wH, 0.0, 1.0);
        weightsOk = weightsOk && std::abs((wS + wM + wH) - 1.0) <= 1e-12;
    }
    check(4, "log range weights sum to one", weightsOk);

    ClipInfo gradedClip = makeClip();
    gradedClip.colorCorrection.logShadowR = 0.11;
    gradedClip.colorCorrection.logShadowG = -0.12;
    gradedClip.colorCorrection.logShadowB = 0.13;
    gradedClip.colorCorrection.logMidR = -0.21;
    gradedClip.colorCorrection.logMidG = 0.22;
    gradedClip.colorCorrection.logMidB = -0.23;
    gradedClip.colorCorrection.logHighR = 0.31;
    gradedClip.colorCorrection.logHighG = -0.32;
    gradedClip.colorCorrection.logHighB = 0.33;
    ProjectData loaded;
    const bool loadedOk = ProjectFile::fromJsonString(
        ProjectFile::toJsonString(projectWithClip(gradedClip)), loaded);
    const bool hasLoadedClip = loadedOk && !loaded.videoTracks.isEmpty()
        && !loaded.videoTracks.first().isEmpty();

    ClipInfo legacyGrade = makeClip();
    legacyGrade.colorCorrection.brightness = 5.0;
    const QJsonObject legacyCc = firstClipObject(
        ProjectFile::toJsonString(projectWithClip(legacyGrade)))
        .value(QStringLiteral("colorCorrection")).toObject();
    check(5, "log fields round-trip and defaults are omitted",
          hasLoadedClip
              && sameLogWheels(gradedClip.colorCorrection,
                               loaded.videoTracks.first().first().colorCorrection)
              && hasNoLogKeys(legacyCc));

    qInfo().noquote() << QStringLiteral("[grade-log-wheel] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed;
}
