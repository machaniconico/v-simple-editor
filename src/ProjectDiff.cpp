#include "ProjectDiff.h"
#include "ProjectFile.h"

#include <QJsonDocument>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>

namespace projdiff {
namespace {
QString valueText(const QJsonValue &value)
{
    if (value.isUndefined()) return {};
    if (value.isString()) return value.toString();
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

bool equal(const QJsonValue &a, const QJsonValue &b, double eps)
{
    if (a.isDouble() && b.isDouble())
        return a.toDouble() == b.toDouble() || std::abs(a.toDouble() - b.toDouble()) <= eps;
    if (a.type() != b.type()) return false;
    if (a.isObject()) {
        const auto x = a.toObject(), y = b.toObject();
        if (x.keys() != y.keys()) return false;
        for (auto it = x.constBegin(); it != x.constEnd(); ++it)
            if (!equal(it.value(), y.value(it.key()), eps)) return false;
        return true;
    }
    if (a.isArray()) {
        const auto x = a.toArray(), y = b.toArray();
        if (x.size() != y.size()) return false;
        for (int i = 0; i < x.size(); ++i)
            if (!equal(x.at(i), y.at(i), eps)) return false;
        return true;
    }
    return a == b;
}

QJsonObject transition(const Transition &t)
{
    return {{"type", int(t.type)}, {"duration", t.duration},
            {"alignment", int(t.alignment)}, {"easing", int(t.easing)},
            {"softness", t.softness}, {"borderWidth", t.borderWidth},
            {"borderColor", t.borderColor.name(QColor::HexArgb)}};
}

// Use the persistence schema for clip properties; only the declared three
// collections enter this projection. No widget, Timeline or live state is used.
QJsonObject projection(const ProjectData &data)
{
    ProjectData scoped;
    scoped.videoTracks = data.videoTracks;
    scoped.audioTracks = data.audioTracks;
    scoped.trackFlags = data.trackFlags;
    auto root = QJsonDocument::fromJson(ProjectFile::toJsonString(scoped).toUtf8()).object();
    auto complete = [&](const char *key, const ProjectTrackClips &tracks) {
        auto jsonTracks = root.value(QLatin1String(key)).toArray();
        for (int t = 0; t < tracks.size(); ++t) {
            auto clips = jsonTracks.at(t).toArray();
            for (int c = 0; c < tracks[t].size(); ++c) {
                const auto &clip = tracks[t][c];
                auto obj = clips.at(c).toObject();
                const auto &cc = clip.colorCorrection;
                QJsonObject grade{
                    {"brightness", cc.brightness},
                    {"contrast", cc.contrast},
                    {"saturation", cc.saturation},
                    {"hue", cc.hue},
                    {"temperature", cc.temperature},
                    {"tint", cc.tint},
                    {"gamma", cc.gamma},
                    {"highlights", cc.highlights},
                    {"shadows", cc.shadows},
                    {"exposure", cc.exposure},
                    {"liftR", cc.liftR},
                    {"liftG", cc.liftG},
                    {"liftB", cc.liftB},
                    {"gammaR", cc.gammaR},
                    {"gammaG", cc.gammaG},
                    {"gammaB", cc.gammaB},
                    {"gainR", cc.gainR},
                    {"gainG", cc.gainG},
                    {"gainB", cc.gainB},
                    {"logShadowR", cc.logShadowR},
                    {"logShadowG", cc.logShadowG},
                    {"logShadowB", cc.logShadowB},
                    {"logMidR", cc.logMidR},
                    {"logMidG", cc.logMidG},
                    {"logMidB", cc.logMidB},
                    {"logHighR", cc.logHighR},
                    {"logHighG", cc.logHighG},
                    {"logHighB", cc.logHighB}
                };
                QJsonArray shifts, scales;
                for (int r = 0; r < HueSatWarp::kSatRings; ++r)
                    for (int h = 0; h < HueSatWarp::kHueNodes; ++h) {
                        shifts.append(cc.hueSatWarp.hueShiftDeg[r][h]);
                        scales.append(cc.hueSatWarp.satScale[r][h]);
                    }
                grade.insert("hueSatWarp", QJsonObject{{"hueShift", shifts}, {"satScale", scales}});
                obj.insert("colorCorrection", grade);
                const auto &hsl = clip.hslSecondary;
                obj.insert("hslSecondary", QJsonObject{
                    {"enabled", hsl.enabled},
                    {"hueCenter", hsl.hueCenter},
                    {"hueRange", hsl.hueRange},
                    {"satMin", hsl.satMin},
                    {"satMax", hsl.satMax},
                    {"lumaMin", hsl.lumaMin},
                    {"lumaMax", hsl.lumaMax},
                    {"softness", hsl.softness},
                    {"liftR", hsl.liftR},
                    {"liftG", hsl.liftG},
                    {"liftB", hsl.liftB},
                    {"gammaR", hsl.gammaR},
                    {"gammaG", hsl.gammaG},
                    {"gammaB", hsl.gammaB},
                    {"gainR", hsl.gainR},
                    {"gainG", hsl.gainG},
                    {"gainB", hsl.gainB}
                });
                // Fields omitted at default by persistence still have numeric
                // values for structural comparisons (including near zero).
                obj.insert("pan", clip.pan);
                obj.insert("lutIntensity", clip.lutIntensity);
                obj.insert("vfxIntensity", clip.vfxIntensity);
                obj.insert("vfxBlackLevel", clip.vfxBlackLevel);
                obj.insert("visible", clip.visible);
                obj.insert("layerMaterial", clip.material.toJson());
                obj.insert("layerStyle", clip.layerStyle.toJson());
                obj.insert("colorMeta", clipcolor::toJson(clip.colorMeta));
                obj.insert("speedRamp", clip.speedRamp.toJson());
                QJsonArray curves;
                for (const auto &channel : clip.colorCurves.editorPointsOrIdentity()) {
                    QJsonArray points;
                    for (const auto &point : channel)
                        points.append(QJsonArray{point.x(), point.y()});
                    curves.append(points);
                }
                obj.insert("colorCurves", curves);
                obj.insert("leadIn", transition(clip.leadIn));
                obj.insert("trailOut", transition(clip.trailOut));
                QJsonArray effects;
                for (const auto &fx : clip.effects)
                    effects.append(QJsonObject{{"type", int(fx.type)}, {"enabled", fx.enabled},
                        {"param1", fx.param1}, {"param2", fx.param2}, {"param3", fx.param3},
                        {"keyColor", fx.keyColor.name(QColor::HexArgb)},
                        {"startSec", fx.startSec}, {"endSec", fx.endSec}});
                obj.insert("effects", effects);
                obj.remove("leadInSec"); // compared as resolved timeline start
                clips[c] = obj;
            }
            jsonTracks[t] = clips;
        }
        root.insert(QLatin1String(key), jsonTracks);
    };
    complete("videoTracks", data.videoTracks);
    complete("audioTracks", data.audioTracks);
    return root;
}

struct Entry {
    int track;
    int clip;
    double start;
    const ClipInfo *info;
    QJsonObject json;
};

QVector<Entry> flatten(const ProjectTrackClips &tracks, const QJsonArray &json)
{
    QVector<Entry> result;
    for (int t = 0; t < tracks.size(); ++t) {
        double start = 0.0;
        for (int c = 0; c < tracks[t].size(); ++c) {
            const auto &clip = tracks[t][c];
            start += qMax(0.0, clip.leadInSec);
            result.append({t, c, start, &clip, json.at(t).toArray().at(c).toObject()});
            start += clip.effectiveDuration();
        }
    }
    return result;
}

QString clipPath(const QString &kind, const Entry &entry)
{
    return QStringLiteral("%1[%2].clips[%3]").arg(kind).arg(entry.track).arg(entry.clip);
}
} // namespace

QString typeName(Change::Type type)
{
    switch (type) {
    case Change::Added: return QStringLiteral("Added");
    case Change::Removed: return QStringLiteral("Removed");
    case Change::Moved: return QStringLiteral("Moved");
    case Change::Trimmed: return QStringLiteral("Trimmed");
    case Change::PropertyChanged: return QStringLiteral("PropertyChanged");
    case Change::EffectsChanged: return QStringLiteral("EffectsChanged");
    case Change::TransitionChanged: return QStringLiteral("TransitionChanged");
    case Change::TrackFlagChanged: return QStringLiteral("TrackFlagChanged");
    }
    return {};
}

QVector<Change> diff(const ProjectData &a, const ProjectData &b, double timeEps)
{
    const double eps = std::isfinite(timeEps) ? qMax(0.0, timeEps) : 1e-3;
    const auto aj = projection(a), bj = projection(b);
    QVector<Change> changes;
    auto append = [&](Change::Type type, const QString &path,
                      const QJsonValue &before, const QJsonValue &after) {
        changes.append({type, path, valueText(before), valueText(after)});
    };
    auto compareTracks = [&](const QString &kind, const ProjectTrackClips &at,
                             const ProjectTrackClips &bt) {
        const QString key = kind + QStringLiteral("Tracks");
        const auto old = flatten(at, aj.value(key).toArray());
        const auto now = flatten(bt, bj.value(key).toArray());
        QVector<int> matches(old.size(), -1);
        QVector<bool> used(now.size(), false);
        // Greedy global nearest pairs ensure exact source-in matches win over
        // a nearby trimmed duplicate. Never use clip indices or IDs as identity.
        struct Pair { int a; int b; double distance; };
        QVector<Pair> pairs;
        for (int i = 0; i < old.size(); ++i)
            for (int j = 0; j < now.size(); ++j)
                if (old[i].info->filePath == now[j].info->filePath) {
                    double distance = std::abs(old[i].info->inPoint - now[j].info->inPoint);
                    if (!std::isfinite(distance)) distance = std::numeric_limits<double>::max();
                    pairs.append({i, j, distance});
                }
        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair &x, const Pair &y) {
            return x.distance < y.distance;
        });
        for (const auto &p : pairs)
            if (matches[p.a] < 0 && !used[p.b]) { matches[p.a] = p.b; used[p.b] = true; }
        for (int i = 0; i < old.size(); ++i) {
            if (matches[i] < 0) {
                append(Change::Removed, clipPath(kind, old[i]), old[i].json, QJsonValue::Undefined);
                continue;
            }
            const auto &x = old[i];
            const auto &y = now[matches[i]];
            const QString path = clipPath(kind, y);
            if (x.track != y.track || !equal(x.start, y.start, eps))
                append(Change::Moved, path + ".start",
                       QJsonObject{{"track", x.track}, {"start", x.start}},
                       QJsonObject{{"track", y.track}, {"start", y.start}});
            QJsonObject trimBefore, trimAfter;
            for (const auto *field : {"inPoint", "outPoint", "duration"}) {
                trimBefore.insert(QLatin1String(field), x.json.value(QLatin1String(field)));
                trimAfter.insert(QLatin1String(field), y.json.value(QLatin1String(field)));
            }
            if (!equal(trimBefore, trimAfter, eps))
                append(Change::Trimmed, path + ".trim", trimBefore, trimAfter);
            QStringList keys = x.json.keys() + y.json.keys();
            keys.removeDuplicates();
            keys.sort();
            for (const auto &field : keys) {
                if (field == "duration" || field == "inPoint" || field == "outPoint") continue;
                if (equal(x.json.value(field), y.json.value(field), eps)) continue;
                const auto type = field == "effects" ? Change::EffectsChanged
                    : (field == "leadIn" || field == "trailOut") ? Change::TransitionChanged
                    : Change::PropertyChanged;
                append(type, path + "." + field, x.json.value(field), y.json.value(field));
            }
        }
        for (int j = 0; j < now.size(); ++j)
            if (!used[j]) append(Change::Added, clipPath(kind, now[j]), QJsonValue::Undefined, now[j].json);
        // Preserve empty track additions/removals as structural changes too.
        for (int t = qMin(at.size(), bt.size()); t < qMax(at.size(), bt.size()); ++t) {
            const bool added = t >= at.size();
            if ((added ? bt[t] : at[t]).isEmpty())
                append(added ? Change::Added : Change::Removed,
                       QStringLiteral("%1[%2]").arg(kind).arg(t),
                       added ? QJsonValue(QJsonValue::Undefined) : QJsonValue(QJsonArray{}),
                       added ? QJsonValue(QJsonArray{}) : QJsonValue(QJsonValue::Undefined));
        }
        const auto af = aj.value("trackFlags").toObject().value(kind).toArray();
        const auto bf = bj.value("trackFlags").toObject().value(kind).toArray();
        for (int t = 0; t < qMax(af.size(), bf.size()); ++t) {
            const auto x = t < af.size() ? af.at(t).toObject() : QJsonObject{};
            const auto y = t < bf.size() ? bf.at(t).toObject() : QJsonObject{};
            for (const auto *flag : {"locked", "muted", "solo", "hidden"}) {
                const auto name = QLatin1String(flag);
                if (x.value(name).toBool() != y.value(name).toBool())
                    append(Change::TrackFlagChanged,
                           QStringLiteral("trackFlags.%1[%2].%3").arg(kind).arg(t).arg(name),
                           x.value(name).toBool(), y.value(name).toBool());
            }
        }
    };
    compareTracks(QStringLiteral("video"), a.videoTracks, b.videoTracks);
    compareTracks(QStringLiteral("audio"), a.audioTracks, b.audioTracks);
    return changes;
}
} // namespace projdiff
