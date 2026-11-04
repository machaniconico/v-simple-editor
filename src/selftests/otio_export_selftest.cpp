// otio-export selftest (US-2012): OtioExport JSON schema and value gates.
// G1-G6 and G8 inspect timelineToOtio's QJsonObject; G7 goes through the file.

#include "../ClipGeometry.h"
#include "../OtioExport.h"
#include "../Timeline.h"
#include "SelftestGate.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <string>

namespace {

ClipInfo mediaClip(const QString &path, double in, double out, double lead,
                   double speed = 1.0)
{
    ClipInfo c;
    c.filePath = path;
    c.displayName = QFileInfo(path).fileName();
    c.inPoint = in;
    c.outPoint = out;
    c.duration = out;
    c.leadInSec = lead;
    c.speed = speed;
    return c;
}

QJsonArray tracksOf(const QJsonObject &o)
{
    return o.value(QStringLiteral("tracks")).toObject()
        .value(QStringLiteral("children")).toArray();
}

QString schema(const QJsonValue &v)
{
    return v.toObject().value(QStringLiteral("OTIO_SCHEMA")).toString();
}

double rtValue(const QJsonObject &range, const char *key)
{
    return range.value(QLatin1String(key)).toObject()
        .value(QStringLiteral("value")).toDouble(-1.0);
}

} // namespace

int runOtioExportSelftest()
{
    selftest::GateTally tally;
    auto gate = [&](int n, bool ok, const std::string &detail) {
        tally.gate(n, ok, detail);
    };
    const QString sch = QStringLiteral("OTIO_SCHEMA");
    const QString ca = QStringLiteral("/media/a.mp4");
    const QString cb = QStringLiteral("/media/b.mp4");

    // V1: clip at 0; V2: gap 3 s then clip (in 1.5, len 2.0); A1: one clip.
    QVector<ClipInfo> v1{mediaClip(ca, 0.0, 4.0, 0.0)};
    QVector<ClipInfo> v2{mediaClip(cb, 1.5, 3.5, 3.0)};
    QVector<ClipInfo> a1{mediaClip(ca, 0.0, 1.0, 0.0)};
    Timeline tl;
    tl.restoreFromProject(QVector<QVector<ClipInfo>>{v1, v2},
                          QVector<QVector<ClipInfo>>{a1}, 0.0, -1.0, -1.0, 100);
    const QJsonObject doc = otio::timelineToOtio(&tl, 24.0, QStringLiteral("t"));
    const QJsonArray trk = tracksOf(doc);
    const QJsonArray v2c = trk.size() > 1
        ? trk.at(1).toObject().value(QStringLiteral("children")).toArray()
        : QJsonArray();

    gate(1, schema(doc) == QLatin1String("Timeline.1")
        && schema(doc.value(QStringLiteral("tracks"))) == QLatin1String("Stack.1")
        && trk.size() == 3
        && trk.at(0).toObject().value(QStringLiteral("kind")).toString() == QLatin1String("Video")
        && trk.at(1).toObject().value(QStringLiteral("kind")).toString() == QLatin1String("Video")
        && trk.at(2).toObject().value(QStringLiteral("kind")).toString() == QLatin1String("Audio")
        && v2c.size() == 2 && schema(v2c.at(0)) == QLatin1String("Gap.1")
        && schema(v2c.at(1)) == QLatin1String("Clip.1"),
        "structure tracks=" + std::to_string(trk.size()) + " v2children=" + std::to_string(v2c.size()));

    const QJsonObject sr = v2c.size() > 1
        ? v2c.at(1).toObject().value(QStringLiteral("source_range")).toObject() : QJsonObject();
    const double rate = sr.value(QStringLiteral("start_time")).toObject()
        .value(QStringLiteral("rate")).toDouble();
    gate(2, rtValue(sr, "start_time") == 36.0 && rate == 24.0 && rtValue(sr, "duration") == 48.0,
        "start=" + std::to_string(rtValue(sr, "start_time")) + " dur=" + std::to_string(rtValue(sr, "duration")));

    const QJsonObject gp = v2c.size() > 0
        ? v2c.at(0).toObject().value(QStringLiteral("source_range")).toObject() : QJsonObject();
    gate(3, rtValue(gp, "duration") == 72.0,
        "gap=" + std::to_string(rtValue(gp, "duration")));

    // G4: speed 2.0 clip (in 1, out 5) -> effectiveDuration 2 s -> 48 frames.
    {
        ClipInfo fast = mediaClip(ca, 1.0, 5.0, 0.0, 2.0);
        ClipInfo norm = mediaClip(cb, 0.0, 2.0, 0.0, 1.0);
        Timeline t4;
        t4.restoreFromProject(QVector<QVector<ClipInfo>>{{fast, norm}},
                              QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
        const QJsonArray ch = tracksOf(otio::timelineToOtio(&t4, 24.0, QStringLiteral("s")))
            .at(0).toObject().value(QStringLiteral("children")).toArray();
        const QJsonObject c0 = ch.at(0).toObject();
        const QJsonArray fx = c0.value(QStringLiteral("effects")).toArray();
        const QJsonObject c1 = ch.at(1).toObject();
        gate(4, fx.size() == 1 && schema(fx.at(0)) == QLatin1String("LinearTimeWarp.1")
            && fx.at(0).toObject().value(QStringLiteral("time_scalar")).toDouble() == 2.0
            && std::fabs(rtValue(c0.value(QStringLiteral("source_range")).toObject(), "duration")
                         - fast.effectiveDuration() * 24.0) < 1e-9
            && c1.value(QStringLiteral("effects")).toArray().isEmpty(),
            "fx=" + std::to_string(fx.size()));
    }

    // G5: markers.
    {
        Timeline t5;
        t5.restoreFromProject(QVector<QVector<ClipInfo>>{v1},
                              QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
        Marker m1; m1.id = 1; m1.timelineUs = 2000000; m1.durationUs = 0;
        m1.label = QStringLiteral("p"); m1.color = QColor(QStringLiteral("#00ff00"));
        Marker m2; m2.id = 2; m2.timelineUs = 1500000; m2.durationUs = 500000;
        m2.label = QStringLiteral("s"); m2.color = QColor(QStringLiteral("#00ff00"));
        t5.setMarkers(QVector<Marker>{m1, m2});
        const QJsonArray mk = otio::timelineToOtio(&t5, 24.0, QStringLiteral("m"))
            .value(QStringLiteral("tracks")).toObject().value(QStringLiteral("markers")).toArray();
        bool ok = mk.size() == 2;
        if (ok) {
            for (int i = 0; i < 2; ++i) {
                const QJsonObject mo = mk.at(i).toObject();
                const QJsonObject range = mo.value(QStringLiteral("marked_range")).toObject();
                const Marker &src = t5.markers().at(i);
                ok = ok && schema(mo) == QLatin1String("Marker.2")
                    && std::fabs(rtValue(range, "start_time") - src.timelineUs / 1e6 * 24.0) < 1e-9
                    && std::fabs(rtValue(range, "duration") - src.durationUs / 1e6 * 24.0) < 1e-9
                    && mo.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("veditor"))
                        .toObject().value(QStringLiteral("color")).toString() == QLatin1String("#00ff00");
            }
        }
        gate(5, ok, "markers=" + std::to_string(mk.size()));
    }

    // G6: media-less clips become named gaps; media clips are Clip.1 + media_reference.
    {
        ClipInfo seqRef = mediaClip(QStringLiteral("veditor://sequence/x"), 0.0, 2.0, 0.0);
        seqRef.sequenceRefId = QStringLiteral("x");
        seqRef.displayName = QStringLiteral("nested");
        ClipInfo nul = mediaClip(clipgeom::nullObjectFilePath(), 0.0, 2.0, 0.0);
        nul.displayName = QStringLiteral("nullobj");
        ClipInfo med = mediaClip(ca, 0.0, 2.0, 0.0);
        Timeline t6;
        t6.restoreFromProject(QVector<QVector<ClipInfo>>{{seqRef, nul, med}},
                              QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
        const QJsonArray ch = tracksOf(otio::timelineToOtio(&t6, 24.0, QStringLiteral("g")))
            .at(0).toObject().value(QStringLiteral("children")).toArray();
        bool ok = ch.size() == 3;
        if (ok) {
            const QJsonObject g0 = ch.at(0).toObject(), g1 = ch.at(1).toObject(), c = ch.at(2).toObject();
            const QJsonObject ref = c.value(QStringLiteral("media_reference")).toObject();
            ok = schema(g0) == QLatin1String("Gap.1") && g0.value(QStringLiteral("name")).toString() == QLatin1String("nested")
                && !g0.contains(QStringLiteral("media_reference"))
                && schema(g1) == QLatin1String("Gap.1") && g1.value(QStringLiteral("name")).toString() == QLatin1String("nullobj")
                && !g1.contains(QStringLiteral("media_reference"))
                && schema(c) == QLatin1String("Clip.1")
                && schema(ref) == QLatin1String("ExternalReference.1")
                && ref.value(QStringLiteral("target_url")).toString() == QUrl::fromLocalFile(ca).toString()
                && !c.contains(QStringLiteral("media_references"))
                && !c.contains(QStringLiteral("active_media_reference_key"));
        }
        gate(6, ok, "children=" + std::to_string(ch.size()));
    }

    // G7: file round trip and an unwritable path.
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("out.otio"));
        QString err;
        const bool wrote = dir.isValid() && otio::writeOtio(&tl, 24.0, QStringLiteral("t"), path, &err);
        QFile f(path);
        const bool opened = f.open(QIODevice::ReadOnly);
        const QJsonObject back = QJsonDocument::fromJson(opened ? f.readAll() : QByteArray()).object();
        QString badErr;
        const bool badOk = otio::writeOtio(&tl, 24.0, QStringLiteral("t"),
            dir.filePath(QStringLiteral("no/such/dir/x.otio")), &badErr);
        gate(7, wrote && opened && back == doc && !badOk && !badErr.isEmpty(),
            "wrote=" + std::to_string(wrote) + " badOk=" + std::to_string(badOk));
    }

    // G8: hidden track keeps its children and enabled:false; active sequence only.
    {
        Timeline t8;
        t8.restoreFromProject(QVector<QVector<ClipInfo>>{v1, v2},
                              QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
        t8.videoTracks().at(1)->setHidden(true);
        const QJsonArray tr = tracksOf(otio::timelineToOtio(&t8, 24.0, QStringLiteral("h")));
        const bool hiddenOk = tr.size() >= 2
            && tr.at(1).toObject().contains(QStringLiteral("enabled"))
            && tr.at(1).toObject().value(QStringLiteral("enabled")).toBool(true) == false
            && tr.at(1).toObject().value(QStringLiteral("children")).toArray().size() == 2
            && !tr.at(0).toObject().contains(QStringLiteral("enabled"));

        Timeline ts;
        ts.restoreFromProject(QVector<QVector<ClipInfo>>{v1},
                              QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
        const QString aId = ts.sequenceList().first().id;
        ClipInfo bclip = mediaClip(QStringLiteral("/media/only_b.mp4"), 0.0, 2.0, 0.0);
        const bool created = ts.createSequence(QStringLiteral("B"),
            QVector<QVector<ClipInfo>>{{bclip}}, QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}});
        auto firstName = [&]() {
            const QJsonArray t = tracksOf(otio::timelineToOtio(&ts, 24.0, QStringLiteral("s")));
            const QJsonArray ch = t.isEmpty() ? QJsonArray()
                : t.at(0).toObject().value(QStringLiteral("children")).toArray();
            return ch.size() == 1 ? ch.at(0).toObject().value(QStringLiteral("name")).toString() : QString();
        };
        const QString inB = firstName();
        ts.setActiveSequence(aId);
        const QString inA = firstName();
        gate(8, hiddenOk && created && inB == QLatin1String("only_b.mp4") && inA == QLatin1String("a.mp4"),
            "hidden=" + std::to_string(hiddenOk) + " B=" + inB.toStdString() + " A=" + inA.toStdString());
    }

    return tally.finish("otio-export");
}
