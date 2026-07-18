#include "../ProjectFile.h"

#include <cmath>

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

namespace {

constexpr double kEps = 1e-9;

bool near(double a, double b)
{
    return std::abs(a - b) <= kEps;
}

ProjectData makeProjectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    data.audioTracks = QVector<QVector<ClipInfo>>{};
    return data;
}

ClipInfo makeClip()
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("grade-lgg-serialization.mov");
    clip.displayName = QStringLiteral("grade-lgg-serialization");
    clip.duration = 4.0;
    clip.inPoint = 0.0;
    clip.outPoint = 4.0;
    return clip;
}

QJsonObject firstClipObject(const QString &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    const QJsonArray videoTracks = doc.object().value(QStringLiteral("videoTracks")).toArray();
    if (videoTracks.isEmpty())
        return {};
    const QJsonArray firstTrack = videoTracks.at(0).toArray();
    if (firstTrack.isEmpty())
        return {};
    return firstTrack.at(0).toObject();
}

bool sameLgg(const ColorCorrection &a, const ColorCorrection &b)
{
    return near(a.liftR, b.liftR)
        && near(a.liftG, b.liftG)
        && near(a.liftB, b.liftB)
        && near(a.gammaR, b.gammaR)
        && near(a.gammaG, b.gammaG)
        && near(a.gammaB, b.gammaB)
        && near(a.gainR, b.gainR)
        && near(a.gainG, b.gainG)
        && near(a.gainB, b.gainB);
}

bool hasNoLggKeys(const QJsonObject &obj)
{
    const QStringList lggKeys = {
        QStringLiteral("liftR"), QStringLiteral("liftG"), QStringLiteral("liftB"),
        QStringLiteral("gammaR"), QStringLiteral("gammaG"), QStringLiteral("gammaB"),
        QStringLiteral("gainR"), QStringLiteral("gainG"), QStringLiteral("gainB")
    };
    for (const QString &key : lggKeys) {
        if (obj.contains(key))
            return false;
    }
    return true;
}

QJsonObject withoutLggKeys(QJsonObject obj)
{
    const QStringList lggKeys = {
        QStringLiteral("liftR"), QStringLiteral("liftG"), QStringLiteral("liftB"),
        QStringLiteral("gammaR"), QStringLiteral("gammaG"), QStringLiteral("gammaB"),
        QStringLiteral("gainR"), QStringLiteral("gainG"), QStringLiteral("gainB")
    };
    for (const QString &key : lggKeys)
        obj.remove(key);
    return obj;
}

} // namespace

int runGradeLggSerializationSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[grade-lgg-serialization] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[grade-lgg-serialization] FAIL G%1 %2%3")
                .arg(gate).arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        }
    };

    ClipInfo lggClip = makeClip();
    lggClip.colorCorrection.liftR = 0.11;
    lggClip.colorCorrection.liftG = -0.12;
    lggClip.colorCorrection.liftB = 0.13;
    lggClip.colorCorrection.gammaR = -0.21;
    lggClip.colorCorrection.gammaG = 0.22;
    lggClip.colorCorrection.gammaB = -0.23;
    lggClip.colorCorrection.gainR = 0.31;
    lggClip.colorCorrection.gainG = -0.32;
    lggClip.colorCorrection.gainB = 0.33;

    ProjectData loadedLgg;
    const QString lggJson = ProjectFile::toJsonString(makeProjectWithClip(lggClip));
    check(1, "LGG project JSON loads", ProjectFile::fromJsonString(lggJson, loadedLgg));
    const bool hasLoadedClip = !loadedLgg.videoTracks.isEmpty()
        && !loadedLgg.videoTracks.first().isEmpty();
    check(2, "LGG 9 fields round-trip",
          hasLoadedClip
              && sameLgg(lggClip.colorCorrection,
                         loadedLgg.videoTracks.first().first().colorCorrection));

    const QJsonObject defaultClipObj =
        firstClipObject(ProjectFile::toJsonString(makeProjectWithClip(makeClip())));
    check(3, "default colorCorrection omitted",
          !defaultClipObj.contains(QStringLiteral("colorCorrection")));

    ClipInfo legacyOnlyClip = makeClip();
    legacyOnlyClip.colorCorrection.brightness = 7.0;
    legacyOnlyClip.colorCorrection.gamma = 1.15;
    const QJsonObject legacyOnlyCc =
        firstClipObject(ProjectFile::toJsonString(makeProjectWithClip(legacyOnlyClip)))
            .value(QStringLiteral("colorCorrection")).toObject();
    check(4, "zero LGG fields omitted from non-default legacy grade",
          hasNoLggKeys(legacyOnlyCc));

    QJsonObject oldRoot = QJsonDocument::fromJson(
        ProjectFile::toJsonString(makeProjectWithClip(legacyOnlyClip)).toUtf8()).object();
    QJsonArray tracks = oldRoot.value(QStringLiteral("videoTracks")).toArray();
    QJsonArray firstTrack = tracks.at(0).toArray();
    QJsonObject oldClip = firstTrack.at(0).toObject();
    oldClip[QStringLiteral("colorCorrection")] =
        withoutLggKeys(oldClip.value(QStringLiteral("colorCorrection")).toObject());
    firstTrack.replace(0, oldClip);
    tracks.replace(0, firstTrack);
    oldRoot[QStringLiteral("videoTracks")] = tracks;

    ProjectData loadedOld;
    const QString oldJson = QString::fromUtf8(QJsonDocument(oldRoot).toJson(QJsonDocument::Compact));
    const bool oldLoaded = ProjectFile::fromJsonString(oldJson, loadedOld);
    const bool oldClipLoaded = oldLoaded
        && !loadedOld.videoTracks.isEmpty()
        && !loadedOld.videoTracks.first().isEmpty();
    check(5, "old JSON missing LGG loads with defaults",
          oldClipLoaded
              && near(loadedOld.videoTracks.first().first().colorCorrection.brightness, 7.0)
              && near(loadedOld.videoTracks.first().first().colorCorrection.gamma, 1.15)
              && loadedOld.videoTracks.first().first().colorCorrection.liftR == 0.0
              && loadedOld.videoTracks.first().first().colorCorrection.liftG == 0.0
              && loadedOld.videoTracks.first().first().colorCorrection.liftB == 0.0
              && loadedOld.videoTracks.first().first().colorCorrection.gammaR == 0.0
              && loadedOld.videoTracks.first().first().colorCorrection.gammaG == 0.0
              && loadedOld.videoTracks.first().first().colorCorrection.gammaB == 0.0
              && loadedOld.videoTracks.first().first().colorCorrection.gainR == 0.0
              && loadedOld.videoTracks.first().first().colorCorrection.gainG == 0.0
              && loadedOld.videoTracks.first().first().colorCorrection.gainB == 0.0);

    qInfo().noquote() << QStringLiteral("[grade-lgg-serialization] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
