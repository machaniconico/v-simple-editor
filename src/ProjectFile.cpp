#include "ProjectFile.h"
#include "AudioMixer.h"
#include "MarkerData.h"
#include "AdjustmentLayer.h"
#include <QFile>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

// --- Timeline marker serialization (Premiere/Resolve parity) ---
// Free, externally-linked helpers so a follow-up story can wire MainWindow
// to round-trip markers through .veditor save/load without expanding the
// scope of *this* story (ProjectFile.h is locked, ProjectData currently has
// no markers field). Each Marker is serialized as
//   { id, timeUs, label, color: "#RRGGBB", note }
// per spec acceptance #5. Schema mirrors MarkerData.h's markerToJsonObject /
// markerFromJsonObject so a future story can swap to those directly with no
// JSON-format churn.
QJsonArray projectFileMarkersToJson(const QVector<Marker> &markers)
{
    return markersToJsonArray(markers);
}

QVector<Marker> projectFileMarkersFromJson(const QJsonArray &arr)
{
    return markersFromJsonArray(arr);
}

// --- Adjustment layer serialization (Premiere/Photoshop parity) ---
// Same rationale as the marker helpers above: ProjectFile.h is locked for
// this story so ProjectData has no `adjustmentLayers` field yet. The schema
// key is still reserved in save/toJsonString below so loaders never trip
// the "missing version" branch on a project that grew the field later.
// A follow-up MainWindow story will wire the helpers into the round-trip.
QJsonArray projectFileAdjustmentLayersToJson(const QVector<AdjustmentLayer> &layers)
{
    return adjustmentLayersToJsonArray(layers);
}

QVector<AdjustmentLayer> projectFileAdjustmentLayersFromJson(const QJsonArray &arr)
{
    return adjustmentLayersFromJsonArray(arr);
}

static const int PROJECT_FORMAT_VERSION = 1;

bool ProjectFile::save(const QString &filePath, const ProjectData &data)
{
    QJsonObject root;
    root["version"] = PROJECT_FORMAT_VERSION;
    root["config"] = configToJson(data.config);
    root["videoTracks"] = tracksToJson(data.videoTracks);
    root["audioTracks"] = tracksToJson(data.audioTracks);
    root["playheadPos"] = data.playheadPos;
    root["markIn"] = data.markIn;
    root["markOut"] = data.markOut;
    root["zoomLevel"] = data.zoomLevel;

    // Audio mixer
    {
        QJsonObject am;
        QJsonArray teArr;
        for (const auto &s : data.trackEqStates) {
            if (s.enabled || !s.isDefault())
                teArr.append(trackEqToJson(s));
        }
        am["trackEq"] = teArr;
        am["compressor"] = compressorToJson(data.masterCompressor);
        am["autoDuck"] = autoDuckToJson(data.autoDuck);
        root["audioMixer"] = am;
    }

    // UI
    {
        QJsonObject ui;
        ui["audioMetersDockVisible"] = data.audioMetersDockVisible;
        root["ui"] = ui;
    }

    // US-FEAT-A: overlay persistence
    {
        QJsonArray ovArr;
        for (const auto &o : data.overlays)
            ovArr.append(overlayToJson(o));
        root["overlays"] = ovArr;
    }

    // Timeline markers — schema reserved here so loaders never see a
    // missing key and trip the "unknown project version" branch. ProjectData
    // does not yet carry a markers field (out of this story's scope), so the
    // array stays empty until a follow-up wires MainWindow through the
    // projectFileMarkersToJson helper. Round-trip is verified via the
    // helper's own tests; reading is tolerant on the load side.
    root["markers"] = QJsonArray{};

    // Adjustment layers — schema reserved with the same rationale as
    // markers. ProjectData has no `adjustmentLayers` field yet (ProjectFile.h
    // is locked for this story); a follow-up will wire MainWindow through
    // projectFileAdjustmentLayersToJson() once the UI lands.
    root["adjustmentLayers"] = QJsonArray{};

    // US-MOCHA-2: planar tracks — skip empty / all-identity tracks on save
    // to keep project files compact (forward-compat guard).
    {
        QJsonArray ptArr;
        for (const auto &t : data.planarTracks) {
            if (!planartrack::shouldSkipOnSave(t)) {
                ptArr.append(planarTrackToJson(t));
            }
        }
        root["planarTracks"] = ptArr;
    }

    // US-BRUSH-5: brush animations
    {
        QJsonArray baArr;
        for (const auto &entry : data.brushAnimations) {
            QJsonObject obj;
            obj["clipId"] = entry.clipId;
            obj["brushData"] = entry.brushData;
            baArr.append(obj);
        }
        root["brushAnimations"] = baArr;
    }

    QJsonDocument doc(root);
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

bool ProjectFile::load(const QString &filePath, ProjectData &data)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isNull()) return false;

    QJsonObject root = doc.object();
    if (!root.contains("version")) return false;

    data.config = configFromJson(root["config"].toObject());
    data.videoTracks = tracksFromJson(root["videoTracks"].toArray());
    data.audioTracks = tracksFromJson(root["audioTracks"].toArray());
    data.playheadPos = root["playheadPos"].toDouble();
    data.markIn = root["markIn"].toDouble(-1.0);
    data.markOut = root["markOut"].toDouble(-1.0);
    data.zoomLevel = root["zoomLevel"].toInt(10);

    // Audio mixer (tolerate missing keys)
    data.trackEqStates.clear();
    if (root.contains("audioMixer")) {
        QJsonObject am = root["audioMixer"].toObject();
        if (am.contains("trackEq")) {
            for (const auto &v : am["trackEq"].toArray())
                data.trackEqStates.append(trackEqFromJson(v.toObject()));
        }
        if (am.contains("compressor"))
            data.masterCompressor = compressorFromJson(am["compressor"].toObject());
        if (am.contains("autoDuck"))
            data.autoDuck = autoDuckFromJson(am["autoDuck"].toObject());
    }

    // UI (tolerate missing keys)
    if (root.contains("ui")) {
        QJsonObject ui = root["ui"].toObject();
        if (ui.contains("audioMetersDockVisible"))
            data.audioMetersDockVisible = ui["audioMetersDockVisible"].toBool(true);
    }

    // US-FEAT-A: overlay persistence — backward compat: missing key = empty list
    data.overlays.clear();
    if (root.contains("overlays")) {
        for (const auto &v : root["overlays"].toArray())
            data.overlays.append(overlayFromJson(v.toObject()));
    }

    // US-MOCHA-2: planar tracks — backward compat: missing key = empty vector
    data.planarTracks.clear();
    if (root.contains("planarTracks")) {
        for (const auto &v : root["planarTracks"].toArray())
            data.planarTracks.append(planarTrackFromJson(v.toObject()));
    }

    // US-BRUSH-5: brush animations — backward compat: missing key = empty list
    data.brushAnimations.clear();
    if (root.contains("brushAnimations")) {
        for (const auto &v : root["brushAnimations"].toArray()) {
            QJsonObject obj = v.toObject();
            BrushAnimationEntry entry;
            entry.clipId = obj["clipId"].toString();
            entry.brushData = obj["brushData"].toObject();
            data.brushAnimations.append(entry);
        }
    }

    return true;
}

QString ProjectFile::toJsonString(const ProjectData &data)
{
    QJsonObject root;
    root["version"] = PROJECT_FORMAT_VERSION;
    root["config"] = configToJson(data.config);
    root["videoTracks"] = tracksToJson(data.videoTracks);
    root["audioTracks"] = tracksToJson(data.audioTracks);
    root["playheadPos"] = data.playheadPos;
    root["markIn"] = data.markIn;
    root["markOut"] = data.markOut;
    root["zoomLevel"] = data.zoomLevel;

    // Audio mixer
    {
        QJsonObject am;
        QJsonArray teArr;
        for (const auto &s : data.trackEqStates) {
            if (s.enabled || !s.isDefault())
                teArr.append(trackEqToJson(s));
        }
        am["trackEq"] = teArr;
        am["compressor"] = compressorToJson(data.masterCompressor);
        am["autoDuck"] = autoDuckToJson(data.autoDuck);
        root["audioMixer"] = am;
    }

    // UI
    {
        QJsonObject ui;
        ui["audioMetersDockVisible"] = data.audioMetersDockVisible;
        root["ui"] = ui;
    }

    // US-FEAT-A: overlay persistence
    {
        QJsonArray ovArr;
        for (const auto &o : data.overlays)
            ovArr.append(overlayToJson(o));
        root["overlays"] = ovArr;
    }

    // Timeline markers — see save() above for rationale (schema reserved).
    root["markers"] = QJsonArray{};

    // Adjustment layers — see save() above for rationale (schema reserved).
    root["adjustmentLayers"] = QJsonArray{};

    // US-MOCHA-2: planar tracks — skip empty / all-identity tracks on save
    {
        QJsonArray ptArr;
        for (const auto &t : data.planarTracks) {
            if (!planartrack::shouldSkipOnSave(t)) {
                ptArr.append(planarTrackToJson(t));
            }
        }
        root["planarTracks"] = ptArr;
    }

    // US-BRUSH-5: brush animations
    {
        QJsonArray baArr;
        for (const auto &entry : data.brushAnimations) {
            QJsonObject obj;
            obj["clipId"] = entry.clipId;
            obj["brushData"] = entry.brushData;
            baArr.append(obj);
        }
        root["brushAnimations"] = baArr;
    }

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

bool ProjectFile::fromJsonString(const QString &json, ProjectData &data)
{
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isNull()) return false;

    QJsonObject root = doc.object();
    if (!root.contains("version")) return false;

    data.config = configFromJson(root["config"].toObject());
    data.videoTracks = tracksFromJson(root["videoTracks"].toArray());
    data.audioTracks = tracksFromJson(root["audioTracks"].toArray());
    data.playheadPos = root["playheadPos"].toDouble();
    data.markIn = root["markIn"].toDouble(-1.0);
    data.markOut = root["markOut"].toDouble(-1.0);
    data.zoomLevel = root["zoomLevel"].toInt(10);

    // Audio mixer (tolerate missing keys)
    data.trackEqStates.clear();
    if (root.contains("audioMixer")) {
        QJsonObject am = root["audioMixer"].toObject();
        if (am.contains("trackEq")) {
            for (const auto &v : am["trackEq"].toArray())
                data.trackEqStates.append(trackEqFromJson(v.toObject()));
        }
        if (am.contains("compressor"))
            data.masterCompressor = compressorFromJson(am["compressor"].toObject());
        if (am.contains("autoDuck"))
            data.autoDuck = autoDuckFromJson(am["autoDuck"].toObject());
    }

    // UI (tolerate missing keys)
    if (root.contains("ui")) {
        QJsonObject ui = root["ui"].toObject();
        if (ui.contains("audioMetersDockVisible"))
            data.audioMetersDockVisible = ui["audioMetersDockVisible"].toBool(true);
    }

    // US-FEAT-A: overlay persistence — backward compat: missing key = empty list
    data.overlays.clear();
    if (root.contains("overlays")) {
        for (const auto &v : root["overlays"].toArray())
            data.overlays.append(overlayFromJson(v.toObject()));
    }

    // US-MOCHA-2: planar tracks — backward compat: missing key = empty vector
    data.planarTracks.clear();
    if (root.contains("planarTracks")) {
        for (const auto &v : root["planarTracks"].toArray())
            data.planarTracks.append(planarTrackFromJson(v.toObject()));
    }

    // US-BRUSH-5: brush animations — backward compat: missing key = empty list
    data.brushAnimations.clear();
    if (root.contains("brushAnimations")) {
        for (const auto &v : root["brushAnimations"].toArray()) {
            QJsonObject obj = v.toObject();
            BrushAnimationEntry entry;
            entry.clipId = obj["clipId"].toString();
            entry.brushData = obj["brushData"].toObject();
            data.brushAnimations.append(entry);
        }
    }

    return true;
}

// --- Config ---

QJsonObject ProjectFile::configToJson(const ProjectConfig &c)
{
    QJsonObject obj;
    obj["name"] = c.name;
    obj["width"] = c.width;
    obj["height"] = c.height;
    obj["fps"] = c.fps;
    return obj;
}

ProjectConfig ProjectFile::configFromJson(const QJsonObject &obj)
{
    ProjectConfig c;
    c.name = obj["name"].toString("Untitled");
    c.width = obj["width"].toInt(1920);
    c.height = obj["height"].toInt(1080);
    c.fps = obj["fps"].toInt(30);
    return c;
}

// --- Clip ---

QJsonObject ProjectFile::clipToJson(const ClipInfo &clip)
{
    QJsonObject obj;
    obj["filePath"] = clip.filePath;
    obj["displayName"] = clip.displayName;
    obj["duration"] = clip.duration;
    obj["inPoint"] = clip.inPoint;
    obj["outPoint"] = clip.outPoint;
    obj["speed"] = clip.speed;
    obj["volume"] = clip.volume;
    obj["videoScale"] = clip.videoScale;
    obj["videoDx"] = clip.videoDx;
    obj["videoDy"] = clip.videoDy;
    obj["rotation2DDegrees"] = clip.rotation2DDegrees;
    obj["opacity"] = clip.opacity;
    obj["is3DLayer"] = clip.is3DLayer;
    obj["layer3D"] = clip.layer3D.toJson();

    if (!clip.volumeEnvelope.isEmpty()) {
        QJsonArray envArr;
        for (const auto &p : clip.volumeEnvelope) {
            QJsonObject pt;
            pt["t"] = p.time;
            pt["g"] = p.gain;
            envArr.append(pt);
        }
        obj["volumeEnvelope"] = envArr;
    }

    if (!clip.colorCorrection.isDefault())
        obj["colorCorrection"] = colorCorrectionToJson(clip.colorCorrection);

    if (!clip.effects.isEmpty()) {
        QJsonArray fxArr;
        for (const auto &e : clip.effects)
            fxArr.append(effectToJson(e));
        obj["effects"] = fxArr;
    }

    if (clip.keyframes.hasAnyKeyframes())
        obj["keyframes"] = keyframeManagerToJson(clip.keyframes);

    if (clip.leadIn.type != TransitionType::None)
        obj["leadIn"] = transitionToJson(clip.leadIn);
    if (clip.trailOut.type != TransitionType::None)
        obj["trailOut"] = transitionToJson(clip.trailOut);

    // US-INT-2 Phase A: persist non-identity speed ramps. Identity ramps
    // are skipped to keep project files compact and forward-compatible
    // (older builds without the field load fine via SpeedRamp::identity()
    // default in clipFromJson).
    if (!clip.speedRamp.isIdentity())
        obj["speedRamp"] = clip.speedRamp.toJson();

    return obj;
}

ClipInfo ProjectFile::clipFromJson(const QJsonObject &obj)
{
    ClipInfo clip;
    clip.filePath = obj["filePath"].toString();
    clip.displayName = obj["displayName"].toString();
    clip.duration = obj["duration"].toDouble();
    clip.inPoint = obj["inPoint"].toDouble();
    clip.outPoint = obj["outPoint"].toDouble();
    clip.speed = obj["speed"].toDouble(1.0);
    clip.volume = obj["volume"].toDouble(1.0);
    clip.videoScale = obj["videoScale"].toDouble(1.0);
    clip.videoDx = obj["videoDx"].toDouble(0.0);
    clip.videoDy = obj["videoDy"].toDouble(0.0);
    clip.rotation2DDegrees = obj["rotation2DDegrees"].toDouble(0.0);
    clip.opacity = obj["opacity"].toDouble(1.0);
    clip.is3DLayer = obj["is3DLayer"].toBool(false);
    clip.layer3D = obj.contains("layer3D")
        ? Layer3DTransform::fromJson(obj["layer3D"].toObject())
        : Layer3DTransform{};
    if (!clip.is3DLayer)
        clip.layer3D.reset();

    if (obj.contains("volumeEnvelope")) {
        for (const auto &v : obj["volumeEnvelope"].toArray()) {
            const auto pt = v.toObject();
            AudioGainPoint p;
            p.time = pt["t"].toDouble(0.0);
            p.gain = pt["g"].toDouble(1.0);
            clip.volumeEnvelope.append(p);
        }
        std::sort(clip.volumeEnvelope.begin(), clip.volumeEnvelope.end(),
                  [](const AudioGainPoint &a, const AudioGainPoint &b) {
                      return a.time < b.time;
                  });
    }

    if (obj.contains("colorCorrection"))
        clip.colorCorrection = colorCorrectionFromJson(obj["colorCorrection"].toObject());

    if (obj.contains("effects")) {
        for (const auto &v : obj["effects"].toArray())
            clip.effects.append(effectFromJson(v.toObject()));
    }

    if (obj.contains("keyframes"))
        clip.keyframes = keyframeManagerFromJson(obj["keyframes"].toObject());

    if (obj.contains("leadIn"))
        clip.leadIn = transitionFromJson(obj["leadIn"].toObject());
    if (obj.contains("trailOut"))
        clip.trailOut = transitionFromJson(obj["trailOut"].toObject());

    if (obj.contains("speedRamp"))
        clip.speedRamp = speedramp::SpeedRamp::fromJson(obj["speedRamp"].toObject());

    return clip;
}

// --- Color Correction ---

QJsonObject ProjectFile::colorCorrectionToJson(const ColorCorrection &cc)
{
    QJsonObject obj;
    obj["brightness"] = cc.brightness;
    obj["contrast"] = cc.contrast;
    obj["saturation"] = cc.saturation;
    obj["hue"] = cc.hue;
    obj["temperature"] = cc.temperature;
    obj["tint"] = cc.tint;
    obj["gamma"] = cc.gamma;
    obj["highlights"] = cc.highlights;
    obj["shadows"] = cc.shadows;
    obj["exposure"] = cc.exposure;
    return obj;
}

ColorCorrection ProjectFile::colorCorrectionFromJson(const QJsonObject &obj)
{
    ColorCorrection cc;
    cc.brightness = obj["brightness"].toDouble();
    cc.contrast = obj["contrast"].toDouble();
    cc.saturation = obj["saturation"].toDouble();
    cc.hue = obj["hue"].toDouble();
    cc.temperature = obj["temperature"].toDouble();
    cc.tint = obj["tint"].toDouble();
    cc.gamma = obj["gamma"].toDouble(1.0);
    cc.highlights = obj["highlights"].toDouble();
    cc.shadows = obj["shadows"].toDouble();
    cc.exposure = obj["exposure"].toDouble();
    return cc;
}

// --- Video Effect ---

QJsonObject ProjectFile::effectToJson(const VideoEffect &e)
{
    QJsonObject obj;
    obj["type"] = static_cast<int>(e.type);
    obj["enabled"] = e.enabled;
    obj["param1"] = e.param1;
    obj["param2"] = e.param2;
    obj["param3"] = e.param3;
    obj["keyColor"] = e.keyColor.name();
    return obj;
}

VideoEffect ProjectFile::effectFromJson(const QJsonObject &obj)
{
    VideoEffect e;
    e.type = static_cast<VideoEffectType>(obj["type"].toInt());
    e.enabled = obj["enabled"].toBool(true);
    e.param1 = obj["param1"].toDouble();
    e.param2 = obj["param2"].toDouble();
    e.param3 = obj["param3"].toDouble();
    e.keyColor = QColor(obj["keyColor"].toString("#00ff00"));
    return e;
}

// --- Keyframe Track ---

QJsonObject ProjectFile::keyframeTrackToJson(const KeyframeTrack &track)
{
    QJsonObject obj;
    obj["property"] = track.propertyName();
    obj["defaultValue"] = track.defaultValue();

    QJsonArray kfArr;
    for (const auto &kf : track.keyframes()) {
        QJsonObject kfObj;
        kfObj["time"] = kf.time;
        kfObj["value"] = kf.value;
        kfObj["interpolation"] = static_cast<int>(kf.interpolation);
        kfArr.append(kfObj);
    }
    obj["keyframes"] = kfArr;
    return obj;
}

KeyframeTrack ProjectFile::keyframeTrackFromJson(const QJsonObject &obj)
{
    KeyframeTrack track(obj["property"].toString(), obj["defaultValue"].toDouble());
    for (const auto &v : obj["keyframes"].toArray()) {
        QJsonObject kfObj = v.toObject();
        track.addKeyframe(
            kfObj["time"].toDouble(),
            kfObj["value"].toDouble(),
            static_cast<KeyframePoint::Interpolation>(kfObj["interpolation"].toInt()));
    }
    return track;
}

// --- Keyframe Manager ---

QJsonObject ProjectFile::keyframeManagerToJson(const KeyframeManager &km)
{
    QJsonObject obj;
    QJsonArray tracksArr;
    for (const auto &t : km.tracks())
        tracksArr.append(keyframeTrackToJson(t));
    obj["tracks"] = tracksArr;
    return obj;
}

KeyframeManager ProjectFile::keyframeManagerFromJson(const QJsonObject &obj)
{
    KeyframeManager km;
    for (const auto &v : obj["tracks"].toArray())
        km.addTrack(keyframeTrackFromJson(v.toObject()));
    return km;
}

// --- Transition ---

QJsonObject ProjectFile::transitionToJson(const Transition &t)
{
    QJsonObject obj;
    obj["type"] = static_cast<int>(t.type);
    obj["duration"] = t.duration;
    obj["alignment"] = static_cast<int>(t.alignment);
    obj["easing"] = static_cast<int>(t.easing);
    return obj;
}

Transition ProjectFile::transitionFromJson(const QJsonObject &obj)
{
    Transition t;
    t.type = static_cast<TransitionType>(
        obj["type"].toInt(static_cast<int>(TransitionType::None)));
    t.duration = obj["duration"].toDouble(0.5);
    // Pre-alignment projects default to Center, the pro-NLE default and the
    // alignment that Step 3's overlap math uses when no explicit choice was
    // serialized.
    t.alignment = static_cast<TransitionAlignment>(
        obj["alignment"].toInt(static_cast<int>(TransitionAlignment::Center)));
    // Pre-easing projects default to Linear — the legacy behaviour where
    // every transition advanced its progress with no curve applied.
    t.easing = static_cast<TransitionEasing>(
        obj["easing"].toInt(static_cast<int>(TransitionEasing::Linear)));
    return t;
}

// --- Tracks ---

QJsonArray ProjectFile::tracksToJson(const QVector<QVector<ClipInfo>> &tracks)
{
    QJsonArray arr;
    for (const auto &track : tracks) {
        QJsonArray clipArr;
        for (const auto &clip : track)
            clipArr.append(clipToJson(clip));
        arr.append(clipArr);
    }
    return arr;
}

QVector<QVector<ClipInfo>> ProjectFile::tracksFromJson(const QJsonArray &arr)
{
    QVector<QVector<ClipInfo>> tracks;
    for (const auto &trackVal : arr) {
        QVector<ClipInfo> clips;
        for (const auto &clipVal : trackVal.toArray())
            clips.append(clipFromJson(clipVal.toObject()));
        tracks.append(clips);
    }
    return tracks;
}

// --- Audio Mixer: Track EQ ---

QJsonObject ProjectFile::trackEqToJson(const TrackEqState &s)
{
    QJsonObject obj;
    obj["trackIdx"] = s.trackIdx;
    obj["enabled"] = s.enabled;
    obj["low"] = s.low;
    obj["mid"] = s.mid;
    obj["high"] = s.high;
    obj["lowFreqHz"] = s.lowFreqHz;
    obj["midFreqHz"] = s.midFreqHz;
    obj["highFreqHz"] = s.highFreqHz;
    obj["qFactor"] = s.qFactor;
    return obj;
}

TrackEqState ProjectFile::trackEqFromJson(const QJsonObject &obj)
{
    TrackEqState s;
    s.trackIdx = obj["trackIdx"].toInt(-1);
    s.enabled = obj["enabled"].toBool(false);

    // Read raw values from JSON (may be NaN, out-of-range, or malicious).
    double qRaw = obj["qFactor"].toDouble(1.0);
    double lowGainRaw = obj["low"].toDouble(0.0);
    double midGainRaw = obj["mid"].toDouble(0.0);
    double highGainRaw = obj["high"].toDouble(0.0);
    double lowFreqRaw = obj["lowFreqHz"].toDouble(200.0);
    double midFreqRaw = obj["midFreqHz"].toDouble(1000.0);
    double highFreqRaw = obj["highFreqHz"].toDouble(5000.0);

    // --- NaN guard: if ANY field is NaN, fall back to defaults entirely. ---
    if (std::isnan(qRaw) || std::isnan(lowGainRaw) || std::isnan(midGainRaw)
        || std::isnan(highGainRaw) || std::isnan(lowFreqRaw)
        || std::isnan(midFreqRaw) || std::isnan(highFreqRaw)) {
        return TrackEqState{}; // all defaults
    }

    // --- Trust-boundary clamp: enforce safe ranges before any DSP path. ---
    s.qFactor = qBound(AudioMixer::kEqMinQ, qRaw, AudioMixer::kEqMaxQ);
    s.low = qBound(AudioMixer::kEqMinGainDb, lowGainRaw, AudioMixer::kEqMaxGainDb);
    s.mid = qBound(AudioMixer::kEqMinGainDb, midGainRaw, AudioMixer::kEqMaxGainDb);
    s.high = qBound(AudioMixer::kEqMinGainDb, highGainRaw, AudioMixer::kEqMaxGainDb);
    s.lowFreqHz = qBound(AudioMixer::kEqMinFreqHz, lowFreqRaw, AudioMixer::kEqMaxFreqHz);
    s.midFreqHz = qBound(AudioMixer::kEqMinFreqHz, midFreqRaw, AudioMixer::kEqMaxFreqHz);
    s.highFreqHz = qBound(AudioMixer::kEqMinFreqHz, highFreqRaw, AudioMixer::kEqMaxFreqHz);
    return s;
}

// --- Audio Mixer: Compressor ---

QJsonObject ProjectFile::compressorToJson(const CompressorState &cs)
{
    QJsonObject obj;
    obj["thresholdDb"] = cs.thresholdDb;
    obj["ratio"] = cs.ratio;
    obj["attackMs"] = cs.attackMs;
    obj["releaseMs"] = cs.releaseMs;
    obj["makeupDb"] = cs.makeupDb;
    obj["enabled"] = cs.enabled;
    return obj;
}

CompressorState ProjectFile::compressorFromJson(const QJsonObject &obj)
{
    CompressorState cs;
    // Trust boundary: clamp + NaN-guard symmetric with US-501 (EQ) and
    // autoDuckFromJson — biquad/IIR DSP detonates on NaN inputs.
    auto sanitize = [](double v, double fallback, double lo, double hi) {
        if (std::isnan(v) || std::isinf(v)) return fallback;
        return qBound(lo, v, hi);
    };
    cs.thresholdDb = sanitize(obj["thresholdDb"].toDouble(-12.0), -12.0, -90.0, 0.0);
    cs.ratio       = sanitize(obj["ratio"].toDouble(4.0), 4.0, 1.0, 100.0);
    cs.attackMs    = sanitize(obj["attackMs"].toDouble(10.0), 10.0, 0.1, 1000.0);
    cs.releaseMs   = sanitize(obj["releaseMs"].toDouble(120.0), 120.0, 1.0, 5000.0);
    cs.makeupDb    = sanitize(obj["makeupDb"].toDouble(0.0), 0.0, -24.0, 24.0);
    cs.enabled     = obj["enabled"].toBool(false);
    return cs;
}

// --- Audio Mixer: Auto Ducking ---

// US-502: suffix-with-unit naming for symmetry with compressor
QJsonObject ProjectFile::autoDuckToJson(const AutoDuckState &ad)
{
    QJsonObject obj;
    obj["thresholdDb"] = ad.thresholdDb;
    obj["ratio"] = ad.ratio;
    obj["attackMs"] = ad.attackMs;
    obj["releaseMs"] = ad.releaseMs;
    return obj;
}

AutoDuckState ProjectFile::autoDuckFromJson(const QJsonObject &obj)
{
    AutoDuckState ad;
    // Trust boundary: .veditor JSON is attacker-controllable. NaN or
    // out-of-range values reach the IIR/biquad sidechain and explode the
    // ducking math (negative attack → exp() of negative inf, etc.).
    // Symmetric with US-501's trackEqFromJson clamp+NaN-guard pattern.
    auto sanitize = [](double v, double fallback, double lo, double hi) {
        if (std::isnan(v) || std::isinf(v)) return fallback;
        return qBound(lo, v, hi);
    };
    const double rawThreshold = obj.contains("thresholdDb")
        ? obj["thresholdDb"].toDouble(-20.0) : obj["threshold"].toDouble(-20.0);
    const double rawRatio = obj["ratio"].toDouble(4.0);
    const double rawAttack = obj.contains("attackMs")
        ? obj["attackMs"].toDouble(5.0) : obj["attack"].toDouble(5.0);
    const double rawRelease = obj.contains("releaseMs")
        ? obj["releaseMs"].toDouble(250.0) : obj["release"].toDouble(250.0);
    ad.thresholdDb = sanitize(rawThreshold, -20.0, -90.0, 0.0);
    ad.ratio       = sanitize(rawRatio, 4.0, 1.0, 100.0);
    ad.attackMs    = sanitize(rawAttack, 5.0, 0.1, 1000.0);
    ad.releaseMs   = sanitize(rawRelease, 250.0, 1.0, 5000.0);
    return ad;
}

// --- US-FEAT-A: overlay persistence ---

QJsonObject ProjectFile::overlayToJson(const OverlayItem &o)
{
    QJsonObject obj;
    obj["id"] = o.id;
    obj["type"] = o.type;
    QJsonObject b;
    b["x"] = o.bounds.x();
    b["y"] = o.bounds.y();
    b["w"] = o.bounds.width();
    b["h"] = o.bounds.height();
    obj["bounds"] = b;
    obj["startTimeMs"] = o.startTimeMs;
    obj["durationMs"] = o.durationMs;
    obj["color"] = o.color;
    obj["text"] = o.text;
    obj["opacity"] = o.opacity;
    obj["trackIdx"] = o.trackIdx;
    return obj;
}

OverlayItem ProjectFile::overlayFromJson(const QJsonObject &obj)
{
    OverlayItem o;
    o.id = obj["id"].toInt(-1);
    o.type = obj["type"].toString("text");
    const QJsonObject b = obj["bounds"].toObject();
    o.bounds = QRectF(b["x"].toDouble(0.5), b["y"].toDouble(0.5),
                       b["w"].toDouble(0.3), b["h"].toDouble(0.1));
    o.startTimeMs = obj["startTimeMs"].toDouble(0.0);
    o.durationMs = obj["durationMs"].toDouble(0.0);
    o.color = obj["color"].toString("#FFFFFFFF");
    o.text = obj["text"].toString();
    o.opacity = obj["opacity"].toDouble(1.0);
    o.trackIdx = obj["trackIdx"].toInt(0);
    return o;
}

// --- US-MOCHA-2: planar track persistence ---

QJsonObject ProjectFile::planarTrackToJson(const planartrack::PlanarTrack &t)
{
    return planartrack::toJson(t);
}

planartrack::PlanarTrack ProjectFile::planarTrackFromJson(const QJsonObject &obj)
{
    planartrack::PlanarTrack t;
    planartrack::fromJson(obj, t);
    return t;
}
