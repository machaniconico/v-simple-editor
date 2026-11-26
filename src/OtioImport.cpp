#include "OtioImport.h"
#include "LayerCompositor.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace otio {

namespace {

double rationalTimeSec(const QJsonObject &rt, double fallbackFps)
{
    const double val = rt.value(QStringLiteral("value")).toDouble(0.0);
    const double rate = rt.value(QStringLiteral("rate")).toDouble(fallbackFps > 0.0 ? fallbackFps : 30.0);
    if (!std::isfinite(val) || !std::isfinite(rate) || rate <= 0.0)
        return 0.0;
    return val / rate;
}

double rationalTimeRate(const QJsonObject &rt, double fallbackFps)
{
    const double rate = rt.value(QStringLiteral("rate")).toDouble(0.0);
    return (std::isfinite(rate) && rate > 0.0) ? rate : fallbackFps;
}

bool schemaMatches(const QJsonObject &obj, const char *prefix)
{
    const QString s = obj.value(QStringLiteral("OTIO_SCHEMA")).toString();
    return s.startsWith(QLatin1String(prefix));
}

QString resolveTargetUrl(const QString &targetUrl)
{
    if (targetUrl.trimmed().isEmpty())
        return {};
    const QUrl url(targetUrl);
    if (url.isLocalFile())
        return url.toLocalFile();
    return targetUrl;
}

QColor otioColorFromName(const QString &name, const QJsonObject &veditorMeta)
{
    const QString hex = veditorMeta.value(QStringLiteral("color")).toString();
    if (!hex.isEmpty() && QColor::isValidColorName(hex))
        return QColor(hex);
    const QString upper = name.toUpper();
    if (upper == QStringLiteral("BLUE"))    return QColor(0x3a, 0x86, 0xff);
    if (upper == QStringLiteral("GREEN"))   return QColor(0x4c, 0xaf, 0x50);
    if (upper == QStringLiteral("YELLOW"))  return QColor(0xff, 0xc1, 0x07);
    if (upper == QStringLiteral("ORANGE"))  return QColor(0xff, 0x98, 0x00);
    if (upper == QStringLiteral("PURPLE") || upper == QStringLiteral("MAGENTA"))
        return QColor(0xab, 0x47, 0xbc);
    if (upper == QStringLiteral("CYAN"))    return QColor(0x26, 0xc6, 0xda);
    return QColor(0xff, 0x50, 0x50); // RED default
}

void parseMarkers(const QJsonArray &arr, double fallbackFps, QVector<Marker> &out)
{
    for (const QJsonValue &v : arr) {
        const QJsonObject mo = v.toObject();
        if (!schemaMatches(mo, "Marker."))
            continue;
        const QJsonObject mr = mo.value(QStringLiteral("marked_range")).toObject();
        const double startSec = qMax(0.0, rationalTimeSec(mr.value(QStringLiteral("start_time")).toObject(), fallbackFps));
        const double durSec = qMax(0.0, rationalTimeSec(mr.value(QStringLiteral("duration")).toObject(), fallbackFps));
        const QJsonObject veditor = mo.value(QStringLiteral("metadata")).toObject()
            .value(QStringLiteral("veditor")).toObject();
        Marker m;
        m.timelineUs = qRound64(startSec * 1e6);
        m.durationUs = qRound64(durSec * 1e6);
        m.label = mo.value(QStringLiteral("name")).toString();
        m.color = otioColorFromName(mo.value(QStringLiteral("color")).toString(), veditor);
        out.append(m);
    }
}

BlendMode parseBlendModeName(const QString &name)
{
    if (name.isEmpty())
        return BlendMode::Normal;
    for (int i = 0; i <= static_cast<int>(BlendMode::Lighten); ++i) {
        const auto bm = static_cast<BlendMode>(i);
        if (CompositeLayer::blendModeName(bm).compare(name, Qt::CaseInsensitive) == 0)
            return bm;
    }
    return BlendMode::Normal;
}

} // namespace

ImportResult parseOtioObject(const QJsonObject &root)
{
    ImportResult res;
    if (!schemaMatches(root, "Timeline.")) {
        res.error = QStringLiteral("OTIO_SCHEMA が Timeline.1 ではありません。");
        return res;
    }

    res.name = root.value(QStringLiteral("name")).toString(QStringLiteral("OTIO Sequence"));
    res.fps = rationalTimeRate(root.value(QStringLiteral("global_start_time")).toObject(), 30.0);

    const QJsonObject stack = root.value(QStringLiteral("tracks")).toObject();
    if (!schemaMatches(stack, "Stack.")) {
        res.error = QStringLiteral("OTIO tracks が Stack.1 ではありません。");
        return res;
    }

    parseMarkers(stack.value(QStringLiteral("markers")).toArray(), res.fps, res.markers);

    QVector<ImportedTrack> rawVideoTracks;
    const QJsonArray trackArr = stack.value(QStringLiteral("children")).toArray();

    for (const QJsonValue &tv : trackArr) {
        const QJsonObject to = tv.toObject();
        if (!schemaMatches(to, "Track."))
            continue;

        ImportedTrack tr;
        tr.name = to.value(QStringLiteral("name")).toString();
        const QString kind = to.value(QStringLiteral("kind")).toString(QStringLiteral("Video"));
        tr.isAudio = (kind.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0);
        tr.hidden = !to.value(QStringLiteral("enabled")).toBool(true);

        parseMarkers(to.value(QStringLiteral("markers")).toArray(), res.fps, res.markers);

        double pendingLeadSec = 0.0;
        double pendingIncomingTransDur = 0.0;
        const QJsonArray children = to.value(QStringLiteral("children")).toArray();

        for (const QJsonValue &cv : children) {
            const QJsonObject co = cv.toObject();
            if (schemaMatches(co, "Gap.")) {
                const QJsonObject sr = co.value(QStringLiteral("source_range")).toObject();
                const double gapSec = qMax(0.0, rationalTimeSec(sr.value(QStringLiteral("duration")).toObject(), res.fps));
                pendingLeadSec += gapSec;
                continue;
            }
            if (schemaMatches(co, "Transition.")) {
                const double inOff = qMax(0.0, rationalTimeSec(co.value(QStringLiteral("in_offset")).toObject(), res.fps));
                const double outOff = qMax(0.0, rationalTimeSec(co.value(QStringLiteral("out_offset")).toObject(), res.fps));
                const double transDur = inOff + outOff;
                if (transDur > 0.0) {
                    if (!tr.clips.isEmpty()) {
                        tr.clips.last().trailOut.type = TransitionType::CrossDissolve;
                        tr.clips.last().trailOut.duration = transDur;
                    }
                    pendingIncomingTransDur = transDur;
                }
                continue;
            }
            if (!schemaMatches(co, "Clip.")) {
                res.warnings.append(QStringLiteral("未対応の OTIO 要素をスキップ: %1")
                    .arg(co.value(QStringLiteral("OTIO_SCHEMA")).toString()));
                continue;
            }

            const QJsonObject sr = co.value(QStringLiteral("source_range")).toObject();
            const double startSec = qMax(0.0, rationalTimeSec(sr.value(QStringLiteral("start_time")).toObject(), res.fps));
            const double durSec = qMax(0.0, rationalTimeSec(sr.value(QStringLiteral("duration")).toObject(), res.fps));
            if (durSec <= 0.0) {
                res.warnings.append(QStringLiteral("長さ 0 のクリップをスキップ: %1")
                    .arg(co.value(QStringLiteral("name")).toString()));
                continue;
            }

            // Parse LinearTimeWarp effect if present
            double speed = 1.0;
            bool reversed = false;
            for (const QJsonValue &ev : co.value(QStringLiteral("effects")).toArray()) {
                const QJsonObject eo = ev.toObject();
                if (schemaMatches(eo, "LinearTimeWarp.")) {
                    const double ts = eo.value(QStringLiteral("time_scalar")).toDouble(1.0);
                    if (std::isfinite(ts) && std::abs(ts) > 1e-6) {
                        reversed = (ts < 0.0);
                        speed = qBound(0.1, std::abs(ts), 10.0);
                    }
                }
            }

            // Resolve media reference (OTIO 0.15+ supports media_references map or legacy media_reference)
            QJsonObject ref = co.value(QStringLiteral("media_reference")).toObject();
            if (ref.isEmpty() && co.contains(QStringLiteral("media_references"))) {
                const QString activeKey = co.value(QStringLiteral("active_media_reference_key"))
                    .toString(QStringLiteral("DEFAULT_MEDIA"));
                ref = co.value(QStringLiteral("media_references")).toObject().value(activeKey).toObject();
            }
            QString filePath;
            if (schemaMatches(ref, "ExternalReference.")) {
                filePath = resolveTargetUrl(ref.value(QStringLiteral("target_url")).toString());
            } else {
                // MissingReference -> keep offline placeholder name
                filePath = co.value(QStringLiteral("name")).toString();
            }

            const QJsonObject veditor = co.value(QStringLiteral("metadata")).toObject()
                .value(QStringLiteral("veditor")).toObject();

            ClipInfo clip;
            clip.filePath = filePath;
            clip.displayName = co.value(QStringLiteral("name")).toString();
            if (clip.displayName.isEmpty())
                clip.displayName = QFileInfo(filePath).fileName();
            clip.inPoint = startSec;
            // source_range.duration in OtioExport is durSec (timeline duration); source span = durSec * speed
            const double sourceSpan = durSec * speed;
            clip.outPoint = startSec + sourceSpan;
            clip.duration = clip.outPoint;
            if (ref.contains(QStringLiteral("available_range"))) {
                const QJsonObject ar = ref.value(QStringLiteral("available_range")).toObject();
                const double availDur = rationalTimeSec(ar.value(QStringLiteral("duration")).toObject(), res.fps);
                if (availDur > clip.duration)
                    clip.duration = availDur;
            }
            clip.leadInSec = pendingLeadSec;
            pendingLeadSec = 0.0;
            clip.speed = speed;
            clip.reversed = reversed;
            clip.volume = qBound(0.0, veditor.value(QStringLiteral("volume")).toDouble(1.0), 4.0);
            clip.blendMode = parseBlendModeName(veditor.value(QStringLiteral("blendMode")).toString());
            if (pendingIncomingTransDur > 0.0) {
                clip.leadIn.type = TransitionType::CrossDissolve;
                clip.leadIn.duration = pendingIncomingTransDur;
                pendingIncomingTransDur = 0.0;
            }
            tr.clips.append(clip);
        }

        if (tr.isAudio)
            res.audioTracks.append(tr);
        else
            rawVideoTracks.append(tr);
    }

    // Reverse video track order back so V1 (last in OTIO Stack) becomes index 0 (frontmost).
    res.videoTracks.reserve(rawVideoTracks.size());
    for (int i = rawVideoTracks.size() - 1; i >= 0; --i)
        res.videoTracks.append(rawVideoTracks[i]);

    res.ok = true;
    return res;
}

ImportResult readOtioFile(const QString &path)
{
    ImportResult res;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        res.error = QStringLiteral("OTIO ファイルを開けません: %1").arg(f.errorString());
        return res;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        res.error = QStringLiteral("OTIO JSON 解析エラー: %1").arg(perr.errorString());
        return res;
    }
    return parseOtioObject(doc.object());
}

bool applyOtioToTimeline(Timeline *timeline, const ImportResult &parsed, QString *errorOut)
{
    if (!timeline || !parsed.ok) {
        if (errorOut)
            *errorOut = parsed.error.isEmpty() ? QStringLiteral("無効な OTIO データです。") : parsed.error;
        return false;
    }

    QVector<QVector<ClipInfo>> vTracks;
    for (const auto &t : parsed.videoTracks)
        vTracks.append(t.clips);
    if (vTracks.isEmpty())
        vTracks.append(QVector<ClipInfo>{});

    QVector<QVector<ClipInfo>> aTracks;
    for (const auto &t : parsed.audioTracks)
        aTracks.append(t.clips);
    if (aTracks.isEmpty())
        aTracks.append(QVector<ClipInfo>{});

    timeline->restoreFromProject(vTracks, aTracks, 0.0, -1.0, -1.0, 100);

    // Apply track names and visibility flags
    const auto &vt = timeline->videoTracks();
    for (int i = 0; i < parsed.videoTracks.size() && i < vt.size(); ++i) {
        if (!vt[i]) continue;
        if (!parsed.videoTracks[i].name.isEmpty())
            vt[i]->customName = parsed.videoTracks[i].name;
        vt[i]->setHidden(parsed.videoTracks[i].hidden);
    }
    const auto &at = timeline->audioTracks();
    for (int i = 0; i < parsed.audioTracks.size() && i < at.size(); ++i) {
        if (!at[i]) continue;
        if (!parsed.audioTracks[i].name.isEmpty())
            at[i]->customName = parsed.audioTracks[i].name;
    }

    timeline->setMarkers(parsed.markers);
    timeline->saveUndoState(QStringLiteral("OTIO インポート (%1)").arg(parsed.name));
    return true;
}

} // namespace otio
