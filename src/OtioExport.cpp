#include "OtioExport.h"

#include "ClipGeometry.h"
#include "LayerCompositor.h"
#include "Timeline.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUrl>

#include <cmath>

namespace otio {
namespace {

QJsonObject rationalTime(double value, double rate)
{
    QJsonObject o;
    o[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("RationalTime.1");
    o[QStringLiteral("value")] = value;
    o[QStringLiteral("rate")] = rate;
    return o;
}

QJsonObject timeRange(double startValue, double durationValue, double rate)
{
    QJsonObject o;
    o[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("TimeRange.1");
    o[QStringLiteral("start_time")] = rationalTime(startValue, rate);
    o[QStringLiteral("duration")] = rationalTime(durationValue, rate);
    return o;
}

QJsonObject gap(const QString &name, double durationValue, double rate)
{
    QJsonObject o;
    o[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("Gap.1");
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("source_range")] = timeRange(0.0, durationValue, rate);
    return o;
}

bool hasMedia(const ClipInfo &c)
{
    return !c.filePath.isEmpty() && !c.isSequenceReference()
        && !clipgeom::isNullObjectFilePath(c.filePath)
        && c.shapes.isEmpty() && !c.isAdjustment;
}

QJsonObject clipToOtio(const ClipInfo &c, double fps)
{
    const double speed = (std::isfinite(c.speed) && c.speed > 0.0) ? c.speed : 1.0;
    const double durSec = c.effectiveDuration();
    QJsonObject o;
    o[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("Clip.1");
    o[QStringLiteral("name")] = QFileInfo(c.filePath).fileName();
    o[QStringLiteral("source_range")] =
        timeRange(c.inPoint * fps, durSec * fps, fps);

    QJsonObject ref;
    ref[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("ExternalReference.1");
    ref[QStringLiteral("target_url")] = QUrl::fromLocalFile(c.filePath).toString();
    o[QStringLiteral("media_reference")] = ref;

    QJsonArray effects;
    if (speed != 1.0) {
        QJsonObject fx;
        fx[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("LinearTimeWarp.1");
        fx[QStringLiteral("name")] = QStringLiteral("LinearTimeWarp");
        fx[QStringLiteral("effect_name")] = QStringLiteral("LinearTimeWarp");
        fx[QStringLiteral("time_scalar")] = speed;
        effects.append(fx);
    }
    o[QStringLiteral("effects")] = effects;

    QJsonObject veditor;
    veditor[QStringLiteral("volume")] = c.volume;
    veditor[QStringLiteral("blendMode")] = CompositeLayer::blendModeName(c.blendMode);
    QJsonObject meta;
    meta[QStringLiteral("veditor")] = veditor;
    o[QStringLiteral("metadata")] = meta;
    return o;
}

QJsonObject trackToOtio(const TimelineTrack *t, bool audio, double fps)
{
    QJsonObject o;
    o[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("Track.1");
    o[QStringLiteral("name")] = t->customName;
    o[QStringLiteral("kind")] = audio ? QStringLiteral("Audio") : QStringLiteral("Video");
    if (!audio && t->isHidden())
        o[QStringLiteral("enabled")] = false;

    QJsonArray children;
    for (const ClipInfo &c : t->clips()) {
        const double lead = qMax(0.0, c.leadInSec);
        if (lead > 0.0)
            children.append(gap(QString(), lead * fps, fps));
        if (hasMedia(c)) {
            children.append(clipToOtio(c, fps));
        } else {
            const QString name = !c.displayName.isEmpty()
                ? c.displayName : QFileInfo(c.filePath).fileName();
            children.append(gap(name, c.effectiveDuration() * fps, fps));
        }
    }
    o[QStringLiteral("children")] = children;
    return o;
}

} // namespace

QJsonObject timelineToOtio(const Timeline *tl, double fps, const QString &name)
{
    if (!(fps > 0.0)) fps = 30.0;

    QJsonArray tracks;
    if (tl) {
        for (const TimelineTrack *t : tl->videoTracks())
            if (t) tracks.append(trackToOtio(t, false, fps));
        for (const TimelineTrack *t : tl->audioTracks())
            if (t) tracks.append(trackToOtio(t, true, fps));
    }

    QJsonArray markers;
    if (tl) {
        for (const Marker &m : tl->markers()) {
            QJsonObject mo;
            mo[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("Marker.2");
            mo[QStringLiteral("name")] = m.label;
            mo[QStringLiteral("color")] = QStringLiteral("RED");
            mo[QStringLiteral("marked_range")] = timeRange(
                m.timelineUs / 1e6 * fps, m.durationUs / 1e6 * fps, fps);
            QJsonObject veditor;
            veditor[QStringLiteral("color")] = m.color.isValid()
                ? m.color.name(QColor::HexRgb) : QStringLiteral("#ff5050");
            QJsonObject meta;
            meta[QStringLiteral("veditor")] = veditor;
            mo[QStringLiteral("metadata")] = meta;
            markers.append(mo);
        }
    }

    QJsonObject stack;
    stack[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("Stack.1");
    stack[QStringLiteral("name")] = QStringLiteral("tracks");
    stack[QStringLiteral("children")] = tracks;
    stack[QStringLiteral("markers")] = markers;

    QJsonObject root;
    root[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("Timeline.1");
    root[QStringLiteral("name")] = name;
    root[QStringLiteral("global_start_time")] = rationalTime(0.0, fps);
    root[QStringLiteral("tracks")] = stack;
    return root;
}

bool writeOtio(const Timeline *tl, double fps, const QString &name,
               const QString &path, QString *err)
{
    const QJsonObject doc = timelineToOtio(tl, fps, name);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

} // namespace otio
