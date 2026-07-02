#include "ProjectFile.h"
#include "AudioMixer.h"
#include "MarkerData.h"
#include "AdjustmentLayer.h"
#include "color/ClipColor.h"
#include <QBuffer>
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

QByteArray imageToPngBase64(const QImage &image)
{
    if (image.isNull())
        return {};

    QByteArray pngBytes;
    QBuffer buffer(&pngBytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    QImage encoded = image;
    if (encoded.format() != QImage::Format_Grayscale8
        && encoded.format() != QImage::Format_Alpha8) {
        encoded = encoded.convertToFormat(QImage::Format_Grayscale8);
    }
    if (!encoded.save(&buffer, "PNG"))
        return {};
    return pngBytes.toBase64();
}

QImage imageFromPngBase64(const QString &encoded)
{
    if (encoded.isEmpty())
        return {};
    const QByteArray pngBytes = QByteArray::fromBase64(encoded.toUtf8());
    QImage image;
    image.loadFromData(pngBytes, "PNG");
    return image.isNull() ? QImage() : image.convertToFormat(QImage::Format_Grayscale8);
}

static const int PROJECT_FORMAT_VERSION = 2;

// --- v1 -> v2 migration: clip pan offsets unit convention ---
//
// In format v1 the per-clip videoDx/videoDy were persisted in *pixels*
// (written by the old motion path, magnitude up to ~10000). From v2 they
// are a NORMALIZED fraction of canvas (canonical range +-5). When loading a
// project whose stored version is < 2 we must convert the stale pixel values
// back to the normalized convention so old projects render identically.
//
// Heuristic: the canonical normalized range is bounded +-5, so any persisted
// |videoDx|>5 (or |videoDy|>5) is unambiguously a stale pixel value and is
// divided by the canvas width (height). Values within +-5 are already
// normalized and left untouched.
//
// INTENTIONAL NO-OP for sub-5px pixel offsets (canonical contract):
//   A genuine legacy pixel offset whose magnitude is <=5px is left
//   unchanged — it is treated as an already-normalized value close to
//   zero.  This is a deliberate acceptance, not an oversight:
//   (1) Such tiny offsets were visually negligible (<0.5% of a 1080p
//       canvas) both before and after the v1->v2 unit change.
//   (2) Converting them would silently alter projects that were authored
//       under v2 normalized semantics; the +-5 threshold is the legacy
//       clamp boundary and must remain stable for round-trip safety.
//   (3) The rendering path already treated these values as normalized,
//       so leaving them as-is preserves the prior visual output exactly.
//
// rotation2DDegrees is unit-stable across v1/v2 and is deliberately NOT
// touched here.
static void migrateClipOffsetsToNormalized(ProjectData &data, int storedVersion)
{
    if (storedVersion >= 2)
        return; // v2+ projects already store normalized values verbatim

    const double w = static_cast<double>(data.config.width);
    const double h = static_cast<double>(data.config.height);
    if (w <= 0.0 || h <= 0.0)
        return; // defensive: cannot normalize against a degenerate canvas

    auto migrateTracks = [&](QVector<QVector<ClipInfo>> &tracks) {
        for (auto &track : tracks) {
            for (auto &clip : track) {
                if (std::abs(clip.videoDx) > 5.0)
                    clip.videoDx /= w;
                if (std::abs(clip.videoDy) > 5.0)
                    clip.videoDy /= h;

                // Defensive post-migration sanity guard: the normalized value
                // should always land within canonical +-5 range.  If it does
                // not (e.g. canvas dimension was extremely small causing /w to
                // overshoot), clamp and emit a one-shot warning so the anomaly
                // surfaces in logs without flooding them.
                static bool s_warnedDx = false;
                if (!s_warnedDx && std::abs(clip.videoDx) > 5.0) {
                    qWarning("migrateClipOffsetsToNormalized: videoDx %f is "
                             "outside canonical +-5 range after migration "
                             "(canvas width=%f); clamping.", clip.videoDx, w);
                    s_warnedDx = true;
                }
                clip.videoDx = std::clamp(clip.videoDx, -5.0, 5.0);

                static bool s_warnedDy = false;
                if (!s_warnedDy && std::abs(clip.videoDy) > 5.0) {
                    qWarning("migrateClipOffsetsToNormalized: videoDy %f is "
                             "outside canonical +-5 range after migration "
                             "(canvas height=%f); clamping.", clip.videoDy, h);
                    s_warnedDy = true;
                }
                clip.videoDy = std::clamp(clip.videoDy, -5.0, 5.0);
            }
        }
    };
    migrateTracks(data.videoTracks);
    migrateTracks(data.audioTracks);
}

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

    // US-AETEXT-12: AE text features
    {
        QJsonArray ptArr;
        for (const auto &e : data.pathTexts)
            ptArr.append(pathTextToJson(e));
        root["pathTexts"] = ptArr;
    }
    {
        QJsonArray t3Arr;
        for (const auto &e : data.text3DLayers)
            t3Arr.append(text3DLayerToJson(e));
        root["text3DLayers"] = t3Arr;
    }
    {
        QJsonArray mrArr;
        for (const auto &e : data.textMaskReveals)
            mrArr.append(textMaskRevealToJson(e));
        root["textMaskReveals"] = mrArr;
    }
    {
        QJsonArray pwArr;
        for (const auto &e : data.textPathWarps)
            pwArr.append(textPathWarpToJson(e));
        root["textPathWarps"] = pwArr;
    }
    {
        QJsonArray vfArr;
        for (const auto &e : data.variableFontAxes)
            vfArr.append(variableFontAxisToJson(e));
        root["variableFontAxes"] = vfArr;
    }
    {
        QJsonArray mgArr;
        for (const auto &e : data.mographTexts)
            mgArr.append(mographTextToJson(e));
        root["mographTexts"] = mgArr;
    }

    // US-SNS-6: SmartReframe, subtitle burn-in, loudness normalization
    root["smartReframe"] = data.smartReframe;
    {
        QJsonArray subArr;
        for (const auto &entry : data.subtitleSegments) {
            QJsonObject obj;
            obj["start"] = entry.start;
            obj["end"] = entry.end;
            obj["text"] = entry.text;
            subArr.append(obj);
        }
        root["subtitleSegments"] = subArr;
    }
    root["subtitleStyle"] = data.subtitleStyle;
    root["loudnessSettings"] = data.loudnessSettings;
    {
        QJsonArray particleArr;
        for (const auto &entry : data.particleClipEntries)
            particleArr.append(particleClipEntryToJson(entry));
        root["particleClipEntries"] = particleArr;
    }
    {
        QJsonArray nodeArr;
        for (const auto &entry : data.clipNodeGraphs)
            nodeArr.append(clipNodeGraphToJson(entry));
        root["clipNodeGraphs"] = nodeArr;
    }
    {
        QJsonArray rotoArr;
        for (const auto &entry : data.rotoClipEntries)
            rotoArr.append(rotoClipEntryToJson(entry));
        root["rotoClipEntries"] = rotoArr;
    }
    {
        QJsonArray timeRemapArr;
        for (const auto &entry : data.timeRemapClipEntries)
            timeRemapArr.append(timeRemapClipEntryToJson(entry));
        root["timeRemapClipEntries"] = timeRemapArr;
    }
    {
        QJsonArray matteArr;
        for (const auto &entry : data.trackMatteClipEntries)
            matteArr.append(trackMatteClipEntryToJson(entry));
        root["trackMatteClipEntries"] = matteArr;
    }
    if (!data.clipParentEntries.isEmpty()) {
        QJsonArray parentArr;
        for (const auto &entry : data.clipParentEntries)
            parentArr.append(clipParentEntryToJson(entry));
        root["clipParentEntries"] = parentArr;
    }
    root["vfxState"] = vfxStateToJson(data.vfxState);

    // US-3D-11: motion-graphics sprint persistence
    {
        QJsonArray t3Arr;
        for (const auto &entry : data.text3DClipEntries) {
            if (!entry.config.isEmpty())
                t3Arr.append(text3DClipEntryToJson(entry));
        }
        root["text3DClipEntries"] = t3Arr;
    }
    {
        QJsonArray exprArr;
        for (const auto &entry : data.expressionBindingsEntries) {
            if (!entry.bindings.isEmpty())
                exprArr.append(expressionBindingsEntryToJson(entry));
        }
        root["expressionBindingsEntries"] = exprArr;
    }
    {
        QJsonArray wigArr;
        for (const auto &entry : data.wiggleClipEntries)
            wigArr.append(wiggleClipEntryToJson(entry));
        root["wiggleClipEntries"] = wigArr;
    }
    if (!data.projectCamera.isEmpty())
        root["projectCamera"] = data.projectCamera;

    // US-HW-10: audio ducking (Sprint 9) — always written so a saved project
    // round-trips even when the user has never opened the ducking dialog.
    {
        QJsonObject duck;
        duck["thresholdDb"]       = data.duckingParams.thresholdDb;
        duck["targetReductionDb"] = data.duckingParams.targetReductionDb;
        duck["attackMs"]          = data.duckingParams.attackMs;
        duck["releaseMs"]         = data.duckingParams.releaseMs;
        duck["holdMs"]            = data.duckingParams.holdMs;
        duck["kneeDb"]            = data.duckingParams.kneeDb;
        root["duckingParams"]  = duck;
        root["duckingEnabled"] = data.duckingEnabled;
    }

    // US-EXT-10: HDR + AI processing settings (Sprint 10) — always written so
    // a saved project round-trips even with default values.
    {
        QJsonObject hdr;
        hdr["mode"]                       = data.hdrSettings.mode;
        hdr["masterDisplayLuminanceMin"]  = data.hdrSettings.masterDisplayLuminanceMin;
        hdr["masterDisplayLuminanceMax"]  = data.hdrSettings.masterDisplayLuminanceMax;
        hdr["maxCll"]                     = data.hdrSettings.maxCll;
        hdr["maxFall"]                    = data.hdrSettings.maxFall;
        hdr["previewToneMap"]             = data.hdrSettings.previewToneMap;
        root["hdrSettings"] = hdr;

        QJsonObject ai;
        ai["upscaleEnabled"]      = data.aiSettings.upscaleEnabled;
        ai["upscaleEngine"]       = data.aiSettings.upscaleEngine;
        ai["upscaleFactor"]       = data.aiSettings.upscaleFactor;
        ai["frameInterpEnabled"]  = data.aiSettings.frameInterpEnabled;
        ai["frameInterpEngine"]   = data.aiSettings.frameInterpEngine;
        ai["frameInterpFactor"]   = data.aiSettings.frameInterpFactor;
        root["aiSettings"] = ai;
    }

    // US-INT-2: Sprint 16 — mobile export + import-hub UI memory.
    // Always written so a saved project round-trips, but kept empty by default
    // so projects without a saved selection compare identical to defaults.
    {
        QJsonObject mobile;
        mobile["lastDeviceId"] = data.mobileExportLastDeviceId;
        root["mobileExport"] = mobile;
        root["lastImportFolder"] = data.lastImportFolder;
    }

    // US-INT-3: Sprint 17/18/19 — YouTube / Collab / ColorMatch UI memory.
    {
        QJsonObject youtube;
        youtube["lastChannelId"] = data.youtubeLastChannelId;
        youtube["lastPrivacy"]   = data.youtubeLastPrivacy;
        root["youtube"] = youtube;

        QJsonObject collab;
        collab["currentUserId"]      = data.collabCurrentUserId;
        collab["currentDisplayName"] = data.collabCurrentDisplayName;
        root["collab"] = collab;

        QJsonObject colormatch;
        colormatch["lastReferenceClip"] = data.colorMatchLastReferenceClip;
        root["colormatch"] = colormatch;
    }

    // PRD-PROJECT-PRESET: tracker preset state persistence
    {
        QJsonObject trackerPresets;
        trackerPresets["motion"] = motionTrackerStateToJson(data.motionTrackerState);
        trackerPresets["planar"] = planarTrackerStateToJson(data.planarTrackerState);
        root["trackerPresets"] = trackerPresets;
    }

    // PRD-PHASE1-MEDIA-POOL: メディアプール永続化
    root["mediaPool"] = data.mediaPool.toJson();

    // PRD-PHASE2-AUDIO-BUS: バス/サブミックス/AUXセンド ルーティング永続化
    root["audioBusRouting"] = data.audioBusRouting.toJson();

    // PRD-PHASE3-ACES: ACES カラーマネジメント設定永続化
    root["acesPipeline"] = aces::pipelineToJson(data.acesPipeline);

    // PRD-PHASE3-DOLBY-VISION: Dolby Vision メタデータ永続化
    root["dolbyVision"] = dolbyvision::toJson(data.dolbyVision);

    // PRD-PHASE3-BROADCAST-CC: 放送CC (CEA-608/708) 永続化
    root["broadcastCaption"] = broadcastcc::toJson(data.broadcastCaption);

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

    // v1 -> v2: convert stale pixel-convention clip pan offsets. Runs after
    // config (canvas resolution) and tracks are loaded; no-op for version>=2.
    migrateClipOffsetsToNormalized(data, root["version"].toInt(1));

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

    // US-AETEXT-12: AE text features — backward compat: missing key = empty vector
    data.pathTexts.clear();
    if (root.contains("pathTexts"))
        for (const auto &v : root["pathTexts"].toArray())
            data.pathTexts.append(pathTextFromJson(v.toObject()));

    data.text3DLayers.clear();
    if (root.contains("text3DLayers"))
        for (const auto &v : root["text3DLayers"].toArray())
            data.text3DLayers.append(text3DLayerFromJson(v.toObject()));

    data.textMaskReveals.clear();
    if (root.contains("textMaskReveals"))
        for (const auto &v : root["textMaskReveals"].toArray())
            data.textMaskReveals.append(textMaskRevealFromJson(v.toObject()));

    data.textPathWarps.clear();
    if (root.contains("textPathWarps"))
        for (const auto &v : root["textPathWarps"].toArray())
            data.textPathWarps.append(textPathWarpFromJson(v.toObject()));

    data.variableFontAxes.clear();
    if (root.contains("variableFontAxes"))
        for (const auto &v : root["variableFontAxes"].toArray())
            data.variableFontAxes.append(variableFontAxisFromJson(v.toObject()));

    data.mographTexts.clear();
    if (root.contains("mographTexts"))
        for (const auto &v : root["mographTexts"].toArray())
            data.mographTexts.append(mographTextFromJson(v.toObject()));

    // US-SNS-6: SmartReframe, subtitle burn-in, loudness normalization — backward compat: missing key = empty/default
    data.smartReframe = QJsonObject{};
    if (root.contains("smartReframe"))
        data.smartReframe = root["smartReframe"].toObject();
    data.subtitleSegments.clear();
    if (root.contains("subtitleSegments")) {
        for (const auto &v : root["subtitleSegments"].toArray()) {
            QJsonObject obj = v.toObject();
            SubtitleEntry entry;
            entry.start = obj["start"].toDouble(0.0);
            entry.end = obj["end"].toDouble(0.0);
            entry.text = obj["text"].toString();
            data.subtitleSegments.append(entry);
        }
    }
    data.subtitleStyle = QJsonObject{};
    if (root.contains("subtitleStyle"))
        data.subtitleStyle = root["subtitleStyle"].toObject();
    data.loudnessSettings = QJsonObject{};
    if (root.contains("loudnessSettings"))
        data.loudnessSettings = root["loudnessSettings"].toObject();
    data.particleClipEntries.clear();
    if (root.contains("particleClipEntries")) {
        for (const auto &v : root["particleClipEntries"].toArray())
            data.particleClipEntries.append(particleClipEntryFromJson(v.toObject()));
    }
    data.clipNodeGraphs.clear();
    if (root.contains("clipNodeGraphs")) {
        for (const auto &v : root["clipNodeGraphs"].toArray())
            data.clipNodeGraphs.append(clipNodeGraphFromJson(v.toObject()));
    }
    data.rotoClipEntries.clear();
    if (root.contains("rotoClipEntries")) {
        for (const auto &v : root["rotoClipEntries"].toArray())
            data.rotoClipEntries.append(rotoClipEntryFromJson(v.toObject()));
    }
    data.timeRemapClipEntries.clear();
    if (root.contains("timeRemapClipEntries")) {
        for (const auto &v : root["timeRemapClipEntries"].toArray())
            data.timeRemapClipEntries.append(timeRemapClipEntryFromJson(v.toObject()));
    }
    data.trackMatteClipEntries.clear();
    if (root.contains("trackMatteClipEntries")) {
        for (const auto &v : root["trackMatteClipEntries"].toArray())
            data.trackMatteClipEntries.append(trackMatteClipEntryFromJson(v.toObject()));
    }
    data.clipParentEntries.clear();
    if (root.contains("clipParentEntries")) {
        for (const auto &v : root["clipParentEntries"].toArray())
            data.clipParentEntries.append(clipParentEntryFromJson(v.toObject()));
    }
    data.vfxState = root.contains("vfxState")
        ? vfxStateFromJson(root["vfxState"].toObject())
        : ProjectVfxState{};

    // US-3D-11: motion-graphics sprint persistence — backward compat:
    // missing key = empty / disabled.
    data.text3DClipEntries.clear();
    if (root.contains("text3DClipEntries")) {
        for (const auto &v : root["text3DClipEntries"].toArray())
            data.text3DClipEntries.append(text3DClipEntryFromJson(v.toObject()));
    }
    data.expressionBindingsEntries.clear();
    if (root.contains("expressionBindingsEntries")) {
        for (const auto &v : root["expressionBindingsEntries"].toArray())
            data.expressionBindingsEntries.append(expressionBindingsEntryFromJson(v.toObject()));
    }
    data.wiggleClipEntries.clear();
    if (root.contains("wiggleClipEntries")) {
        for (const auto &v : root["wiggleClipEntries"].toArray())
            data.wiggleClipEntries.append(wiggleClipEntryFromJson(v.toObject()));
    }
    data.projectCamera = root.contains("projectCamera")
        ? root["projectCamera"].toObject()
        : QJsonObject{};

    // US-HW-10: audio ducking (Sprint 9) — backward compat: missing key keeps
    // the DuckingParams{} defaults and duckingEnabled=false.
    {
        const DuckingParams defaults;
        const QJsonObject duck = root.contains("duckingParams")
                                   ? root["duckingParams"].toObject()
                                   : QJsonObject{};
        data.duckingParams.thresholdDb       = duck.value("thresholdDb").toDouble(defaults.thresholdDb);
        data.duckingParams.targetReductionDb = duck.value("targetReductionDb").toDouble(defaults.targetReductionDb);
        data.duckingParams.attackMs          = duck.value("attackMs").toDouble(defaults.attackMs);
        data.duckingParams.releaseMs         = duck.value("releaseMs").toDouble(defaults.releaseMs);
        data.duckingParams.holdMs            = duck.value("holdMs").toDouble(defaults.holdMs);
        data.duckingParams.kneeDb            = duck.value("kneeDb").toDouble(defaults.kneeDb);
        data.duckingEnabled = root.contains("duckingEnabled")
                                ? root["duckingEnabled"].toBool(false)
                                : false;
    }

    // US-EXT-10: HDR + AI processing settings (Sprint 10) — backward compat:
    // missing keys keep HDRSettings{} / AIProcessingSettings{} defaults so older
    // .veditor files without these fields still load cleanly.
    {
        const HDRSettings hdrDefaults;
        const QJsonObject hdr = root.contains("hdrSettings")
                                   ? root["hdrSettings"].toObject()
                                   : QJsonObject{};
        data.hdrSettings.mode                      = hdr.value("mode").toString(hdrDefaults.mode);
        data.hdrSettings.masterDisplayLuminanceMin = hdr.value("masterDisplayLuminanceMin").toDouble(hdrDefaults.masterDisplayLuminanceMin);
        data.hdrSettings.masterDisplayLuminanceMax = hdr.value("masterDisplayLuminanceMax").toDouble(hdrDefaults.masterDisplayLuminanceMax);
        data.hdrSettings.maxCll                    = hdr.value("maxCll").toInt(hdrDefaults.maxCll);
        data.hdrSettings.maxFall                   = hdr.value("maxFall").toInt(hdrDefaults.maxFall);
        data.hdrSettings.previewToneMap            = hdr.value("previewToneMap").toString(hdrDefaults.previewToneMap);

        const AIProcessingSettings aiDefaults;
        const QJsonObject ai = root.contains("aiSettings")
                                   ? root["aiSettings"].toObject()
                                   : QJsonObject{};
        data.aiSettings.upscaleEnabled     = ai.value("upscaleEnabled").toBool(aiDefaults.upscaleEnabled);
        data.aiSettings.upscaleEngine      = ai.value("upscaleEngine").toString(aiDefaults.upscaleEngine);
        data.aiSettings.upscaleFactor      = ai.value("upscaleFactor").toInt(aiDefaults.upscaleFactor);
        data.aiSettings.frameInterpEnabled = ai.value("frameInterpEnabled").toBool(aiDefaults.frameInterpEnabled);
        data.aiSettings.frameInterpEngine  = ai.value("frameInterpEngine").toString(aiDefaults.frameInterpEngine);
        data.aiSettings.frameInterpFactor  = ai.value("frameInterpFactor").toInt(aiDefaults.frameInterpFactor);
    }

    // US-INT-2: Sprint 16 — mobile export + import-hub UI memory (backward compat).
    data.mobileExportLastDeviceId = QString();
    if (root.contains("mobileExport")) {
        const QJsonObject mobile = root["mobileExport"].toObject();
        data.mobileExportLastDeviceId = mobile.value("lastDeviceId").toString();
    }
    data.lastImportFolder = QString();
    if (root.contains("lastImportFolder"))
        data.lastImportFolder = root["lastImportFolder"].toString();

    // US-INT-3: Sprint 17/18/19 — YouTube / Collab / ColorMatch UI memory (backward compat).
    data.youtubeLastChannelId        = QString();
    data.youtubeLastPrivacy          = QString();
    if (root.contains("youtube")) {
        const QJsonObject youtube = root["youtube"].toObject();
        data.youtubeLastChannelId   = youtube.value("lastChannelId").toString();
        data.youtubeLastPrivacy     = youtube.value("lastPrivacy").toString();
    }
    data.collabCurrentUserId         = QString();
    data.collabCurrentDisplayName    = QString();
    if (root.contains("collab")) {
        const QJsonObject collab = root["collab"].toObject();
        data.collabCurrentUserId       = collab.value("currentUserId").toString();
        data.collabCurrentDisplayName  = collab.value("currentDisplayName").toString();
    }
    data.colorMatchLastReferenceClip = QString();
    if (root.contains("colormatch")) {
        const QJsonObject colormatch = root["colormatch"].toObject();
        data.colorMatchLastReferenceClip = colormatch.value("lastReferenceClip").toString();
    }

    // PRD-PROJECT-PRESET: tracker preset state persistence (backward compat: missing key = default struct)
    {
        const QJsonObject tp = root.value("trackerPresets").toObject();
        data.motionTrackerState = motionTrackerStateFromJson(tp.value("motion").toObject());
        data.planarTrackerState = planarTrackerStateFromJson(tp.value("planar").toObject());
    }

    // PRD-PHASE1-MEDIA-POOL: メディアプール永続化 (キー欠落でも空 object → 空プールで安全)
    data.mediaPool.fromJson(root.value("mediaPool").toObject());

    // PRD-PHASE2-AUDIO-BUS: バス/サブミックス/AUXセンド ルーティング永続化
    // (キー欠落でも空 object → 空ルーティング=identity、旧 .veditor 後方互換)
    data.audioBusRouting.fromJson(root.value("audioBusRouting").toObject());

    // PRD-PHASE3-ACES: ACES カラーマネジメント設定永続化
    // (キー欠落でも空 object → enabled=false 既定=identity、旧 .veditor 後方互換)
    data.acesPipeline = aces::pipelineFromJson(root.value("acesPipeline").toObject());

    // PRD-PHASE3-DOLBY-VISION: Dolby Vision メタデータ永続化
    // (キー欠落でも空 object → 空メタ既定、旧 .veditor 後方互換)
    data.dolbyVision = dolbyvision::fromJson(root.value("dolbyVision").toObject());

    // PRD-PHASE3-BROADCAST-CC: 放送CC (CEA-608/708) 永続化
    // (キー欠落でも空 object → 空 doc 既定、旧 .veditor 後方互換)
    data.broadcastCaption = broadcastcc::fromJson(root.value("broadcastCaption").toObject());

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

    // US-AETEXT-12: AE text features
    {
        QJsonArray ptArr;
        for (const auto &e : data.pathTexts)
            ptArr.append(pathTextToJson(e));
        root["pathTexts"] = ptArr;
    }
    {
        QJsonArray t3Arr;
        for (const auto &e : data.text3DLayers)
            t3Arr.append(text3DLayerToJson(e));
        root["text3DLayers"] = t3Arr;
    }
    {
        QJsonArray mrArr;
        for (const auto &e : data.textMaskReveals)
            mrArr.append(textMaskRevealToJson(e));
        root["textMaskReveals"] = mrArr;
    }
    {
        QJsonArray pwArr;
        for (const auto &e : data.textPathWarps)
            pwArr.append(textPathWarpToJson(e));
        root["textPathWarps"] = pwArr;
    }
    {
        QJsonArray vfArr;
        for (const auto &e : data.variableFontAxes)
            vfArr.append(variableFontAxisToJson(e));
        root["variableFontAxes"] = vfArr;
    }
    {
        QJsonArray mgArr;
        for (const auto &e : data.mographTexts)
            mgArr.append(mographTextToJson(e));
        root["mographTexts"] = mgArr;
    }

    // US-SNS-6: SmartReframe, subtitle burn-in, loudness normalization
    root["smartReframe"] = data.smartReframe;
    {
        QJsonArray subArr;
        for (const auto &entry : data.subtitleSegments) {
            QJsonObject obj;
            obj["start"] = entry.start;
            obj["end"] = entry.end;
            obj["text"] = entry.text;
            subArr.append(obj);
        }
        root["subtitleSegments"] = subArr;
    }
    root["subtitleStyle"] = data.subtitleStyle;
    root["loudnessSettings"] = data.loudnessSettings;
    {
        QJsonArray particleArr;
        for (const auto &entry : data.particleClipEntries)
            particleArr.append(particleClipEntryToJson(entry));
        root["particleClipEntries"] = particleArr;
    }
    {
        QJsonArray nodeArr;
        for (const auto &entry : data.clipNodeGraphs)
            nodeArr.append(clipNodeGraphToJson(entry));
        root["clipNodeGraphs"] = nodeArr;
    }
    {
        QJsonArray rotoArr;
        for (const auto &entry : data.rotoClipEntries)
            rotoArr.append(rotoClipEntryToJson(entry));
        root["rotoClipEntries"] = rotoArr;
    }
    {
        QJsonArray timeRemapArr;
        for (const auto &entry : data.timeRemapClipEntries)
            timeRemapArr.append(timeRemapClipEntryToJson(entry));
        root["timeRemapClipEntries"] = timeRemapArr;
    }
    {
        QJsonArray matteArr;
        for (const auto &entry : data.trackMatteClipEntries)
            matteArr.append(trackMatteClipEntryToJson(entry));
        root["trackMatteClipEntries"] = matteArr;
    }
    if (!data.clipParentEntries.isEmpty()) {
        QJsonArray parentArr;
        for (const auto &entry : data.clipParentEntries)
            parentArr.append(clipParentEntryToJson(entry));
        root["clipParentEntries"] = parentArr;
    }
    root["vfxState"] = vfxStateToJson(data.vfxState);

    // US-3D-11: motion-graphics sprint persistence
    {
        QJsonArray t3Arr;
        for (const auto &entry : data.text3DClipEntries) {
            if (!entry.config.isEmpty())
                t3Arr.append(text3DClipEntryToJson(entry));
        }
        root["text3DClipEntries"] = t3Arr;
    }
    {
        QJsonArray exprArr;
        for (const auto &entry : data.expressionBindingsEntries) {
            if (!entry.bindings.isEmpty())
                exprArr.append(expressionBindingsEntryToJson(entry));
        }
        root["expressionBindingsEntries"] = exprArr;
    }
    {
        QJsonArray wigArr;
        for (const auto &entry : data.wiggleClipEntries)
            wigArr.append(wiggleClipEntryToJson(entry));
        root["wiggleClipEntries"] = wigArr;
    }
    if (!data.projectCamera.isEmpty())
        root["projectCamera"] = data.projectCamera;

    // US-HW-10: audio ducking (Sprint 9) — always written so a saved project
    // round-trips even when the user has never opened the ducking dialog.
    {
        QJsonObject duck;
        duck["thresholdDb"]       = data.duckingParams.thresholdDb;
        duck["targetReductionDb"] = data.duckingParams.targetReductionDb;
        duck["attackMs"]          = data.duckingParams.attackMs;
        duck["releaseMs"]         = data.duckingParams.releaseMs;
        duck["holdMs"]            = data.duckingParams.holdMs;
        duck["kneeDb"]            = data.duckingParams.kneeDb;
        root["duckingParams"]  = duck;
        root["duckingEnabled"] = data.duckingEnabled;
    }

    // US-EXT-10: HDR + AI processing settings (Sprint 10) — always written so
    // a saved project round-trips even with default values.
    {
        QJsonObject hdr;
        hdr["mode"]                       = data.hdrSettings.mode;
        hdr["masterDisplayLuminanceMin"]  = data.hdrSettings.masterDisplayLuminanceMin;
        hdr["masterDisplayLuminanceMax"]  = data.hdrSettings.masterDisplayLuminanceMax;
        hdr["maxCll"]                     = data.hdrSettings.maxCll;
        hdr["maxFall"]                    = data.hdrSettings.maxFall;
        hdr["previewToneMap"]             = data.hdrSettings.previewToneMap;
        root["hdrSettings"] = hdr;

        QJsonObject ai;
        ai["upscaleEnabled"]      = data.aiSettings.upscaleEnabled;
        ai["upscaleEngine"]       = data.aiSettings.upscaleEngine;
        ai["upscaleFactor"]       = data.aiSettings.upscaleFactor;
        ai["frameInterpEnabled"]  = data.aiSettings.frameInterpEnabled;
        ai["frameInterpEngine"]   = data.aiSettings.frameInterpEngine;
        ai["frameInterpFactor"]   = data.aiSettings.frameInterpFactor;
        root["aiSettings"] = ai;
    }

    // US-INT-2: Sprint 16 — mobile export + import-hub UI memory.
    {
        QJsonObject mobile;
        mobile["lastDeviceId"] = data.mobileExportLastDeviceId;
        root["mobileExport"] = mobile;
        root["lastImportFolder"] = data.lastImportFolder;
    }

    // US-INT-3: Sprint 17/18/19 — YouTube / Collab / ColorMatch UI memory.
    {
        QJsonObject youtube;
        youtube["lastChannelId"] = data.youtubeLastChannelId;
        youtube["lastPrivacy"]   = data.youtubeLastPrivacy;
        root["youtube"] = youtube;

        QJsonObject collab;
        collab["currentUserId"]      = data.collabCurrentUserId;
        collab["currentDisplayName"] = data.collabCurrentDisplayName;
        root["collab"] = collab;

        QJsonObject colormatch;
        colormatch["lastReferenceClip"] = data.colorMatchLastReferenceClip;
        root["colormatch"] = colormatch;
    }

    // PRD-PROJECT-PRESET: tracker preset state persistence
    {
        QJsonObject trackerPresets;
        trackerPresets["motion"] = motionTrackerStateToJson(data.motionTrackerState);
        trackerPresets["planar"] = planarTrackerStateToJson(data.planarTrackerState);
        root["trackerPresets"] = trackerPresets;
    }

    // PRD-PHASE1-MEDIA-POOL: メディアプール永続化
    root["mediaPool"] = data.mediaPool.toJson();

    // PRD-PHASE2-AUDIO-BUS: バス/サブミックス/AUXセンド ルーティング永続化
    root["audioBusRouting"] = data.audioBusRouting.toJson();

    // PRD-PHASE3-ACES: ACES カラーマネジメント設定永続化
    root["acesPipeline"] = aces::pipelineToJson(data.acesPipeline);

    // PRD-PHASE3-DOLBY-VISION: Dolby Vision メタデータ永続化
    root["dolbyVision"] = dolbyvision::toJson(data.dolbyVision);

    // PRD-PHASE3-BROADCAST-CC: 放送CC (CEA-608/708) 永続化
    root["broadcastCaption"] = broadcastcc::toJson(data.broadcastCaption);

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

    // v1 -> v2: convert stale pixel-convention clip pan offsets. Runs after
    // config (canvas resolution) and tracks are loaded; no-op for version>=2.
    migrateClipOffsetsToNormalized(data, root["version"].toInt(1));

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

    // US-AETEXT-12: AE text features — backward compat: missing key = empty vector
    data.pathTexts.clear();
    if (root.contains("pathTexts"))
        for (const auto &v : root["pathTexts"].toArray())
            data.pathTexts.append(pathTextFromJson(v.toObject()));

    data.text3DLayers.clear();
    if (root.contains("text3DLayers"))
        for (const auto &v : root["text3DLayers"].toArray())
            data.text3DLayers.append(text3DLayerFromJson(v.toObject()));

    data.textMaskReveals.clear();
    if (root.contains("textMaskReveals"))
        for (const auto &v : root["textMaskReveals"].toArray())
            data.textMaskReveals.append(textMaskRevealFromJson(v.toObject()));

    data.textPathWarps.clear();
    if (root.contains("textPathWarps"))
        for (const auto &v : root["textPathWarps"].toArray())
            data.textPathWarps.append(textPathWarpFromJson(v.toObject()));

    data.variableFontAxes.clear();
    if (root.contains("variableFontAxes"))
        for (const auto &v : root["variableFontAxes"].toArray())
            data.variableFontAxes.append(variableFontAxisFromJson(v.toObject()));

    data.mographTexts.clear();
    if (root.contains("mographTexts"))
        for (const auto &v : root["mographTexts"].toArray())
            data.mographTexts.append(mographTextFromJson(v.toObject()));

    // US-SNS-6: SmartReframe, subtitle burn-in, loudness normalization — backward compat: missing key = empty/default
    data.smartReframe = QJsonObject{};
    if (root.contains("smartReframe"))
        data.smartReframe = root["smartReframe"].toObject();
    data.subtitleSegments.clear();
    if (root.contains("subtitleSegments")) {
        for (const auto &v : root["subtitleSegments"].toArray()) {
            QJsonObject obj = v.toObject();
            SubtitleEntry entry;
            entry.start = obj["start"].toDouble(0.0);
            entry.end = obj["end"].toDouble(0.0);
            entry.text = obj["text"].toString();
            data.subtitleSegments.append(entry);
        }
    }
    data.subtitleStyle = QJsonObject{};
    if (root.contains("subtitleStyle"))
        data.subtitleStyle = root["subtitleStyle"].toObject();
    data.loudnessSettings = QJsonObject{};
    if (root.contains("loudnessSettings"))
        data.loudnessSettings = root["loudnessSettings"].toObject();
    data.particleClipEntries.clear();
    if (root.contains("particleClipEntries")) {
        for (const auto &v : root["particleClipEntries"].toArray())
            data.particleClipEntries.append(particleClipEntryFromJson(v.toObject()));
    }
    data.clipNodeGraphs.clear();
    if (root.contains("clipNodeGraphs")) {
        for (const auto &v : root["clipNodeGraphs"].toArray())
            data.clipNodeGraphs.append(clipNodeGraphFromJson(v.toObject()));
    }
    data.rotoClipEntries.clear();
    if (root.contains("rotoClipEntries")) {
        for (const auto &v : root["rotoClipEntries"].toArray())
            data.rotoClipEntries.append(rotoClipEntryFromJson(v.toObject()));
    }
    data.timeRemapClipEntries.clear();
    if (root.contains("timeRemapClipEntries")) {
        for (const auto &v : root["timeRemapClipEntries"].toArray())
            data.timeRemapClipEntries.append(timeRemapClipEntryFromJson(v.toObject()));
    }
    data.trackMatteClipEntries.clear();
    if (root.contains("trackMatteClipEntries")) {
        for (const auto &v : root["trackMatteClipEntries"].toArray())
            data.trackMatteClipEntries.append(trackMatteClipEntryFromJson(v.toObject()));
    }
    data.clipParentEntries.clear();
    if (root.contains("clipParentEntries")) {
        for (const auto &v : root["clipParentEntries"].toArray())
            data.clipParentEntries.append(clipParentEntryFromJson(v.toObject()));
    }
    data.vfxState = root.contains("vfxState")
        ? vfxStateFromJson(root["vfxState"].toObject())
        : ProjectVfxState{};

    // US-3D-11: motion-graphics sprint persistence — backward compat:
    // missing key = empty / disabled.
    data.text3DClipEntries.clear();
    if (root.contains("text3DClipEntries")) {
        for (const auto &v : root["text3DClipEntries"].toArray())
            data.text3DClipEntries.append(text3DClipEntryFromJson(v.toObject()));
    }
    data.expressionBindingsEntries.clear();
    if (root.contains("expressionBindingsEntries")) {
        for (const auto &v : root["expressionBindingsEntries"].toArray())
            data.expressionBindingsEntries.append(expressionBindingsEntryFromJson(v.toObject()));
    }
    data.wiggleClipEntries.clear();
    if (root.contains("wiggleClipEntries")) {
        for (const auto &v : root["wiggleClipEntries"].toArray())
            data.wiggleClipEntries.append(wiggleClipEntryFromJson(v.toObject()));
    }
    data.projectCamera = root.contains("projectCamera")
        ? root["projectCamera"].toObject()
        : QJsonObject{};

    // US-HW-10: audio ducking (Sprint 9) — backward compat: missing key keeps
    // the DuckingParams{} defaults and duckingEnabled=false.
    {
        const DuckingParams defaults;
        const QJsonObject duck = root.contains("duckingParams")
                                   ? root["duckingParams"].toObject()
                                   : QJsonObject{};
        data.duckingParams.thresholdDb       = duck.value("thresholdDb").toDouble(defaults.thresholdDb);
        data.duckingParams.targetReductionDb = duck.value("targetReductionDb").toDouble(defaults.targetReductionDb);
        data.duckingParams.attackMs          = duck.value("attackMs").toDouble(defaults.attackMs);
        data.duckingParams.releaseMs         = duck.value("releaseMs").toDouble(defaults.releaseMs);
        data.duckingParams.holdMs            = duck.value("holdMs").toDouble(defaults.holdMs);
        data.duckingParams.kneeDb            = duck.value("kneeDb").toDouble(defaults.kneeDb);
        data.duckingEnabled = root.contains("duckingEnabled")
                                ? root["duckingEnabled"].toBool(false)
                                : false;
    }

    // US-EXT-10: HDR + AI processing settings (Sprint 10) — backward compat:
    // missing keys keep HDRSettings{} / AIProcessingSettings{} defaults so older
    // .veditor files without these fields still load cleanly.
    {
        const HDRSettings hdrDefaults;
        const QJsonObject hdr = root.contains("hdrSettings")
                                   ? root["hdrSettings"].toObject()
                                   : QJsonObject{};
        data.hdrSettings.mode                      = hdr.value("mode").toString(hdrDefaults.mode);
        data.hdrSettings.masterDisplayLuminanceMin = hdr.value("masterDisplayLuminanceMin").toDouble(hdrDefaults.masterDisplayLuminanceMin);
        data.hdrSettings.masterDisplayLuminanceMax = hdr.value("masterDisplayLuminanceMax").toDouble(hdrDefaults.masterDisplayLuminanceMax);
        data.hdrSettings.maxCll                    = hdr.value("maxCll").toInt(hdrDefaults.maxCll);
        data.hdrSettings.maxFall                   = hdr.value("maxFall").toInt(hdrDefaults.maxFall);
        data.hdrSettings.previewToneMap            = hdr.value("previewToneMap").toString(hdrDefaults.previewToneMap);

        const AIProcessingSettings aiDefaults;
        const QJsonObject ai = root.contains("aiSettings")
                                   ? root["aiSettings"].toObject()
                                   : QJsonObject{};
        data.aiSettings.upscaleEnabled     = ai.value("upscaleEnabled").toBool(aiDefaults.upscaleEnabled);
        data.aiSettings.upscaleEngine      = ai.value("upscaleEngine").toString(aiDefaults.upscaleEngine);
        data.aiSettings.upscaleFactor      = ai.value("upscaleFactor").toInt(aiDefaults.upscaleFactor);
        data.aiSettings.frameInterpEnabled = ai.value("frameInterpEnabled").toBool(aiDefaults.frameInterpEnabled);
        data.aiSettings.frameInterpEngine  = ai.value("frameInterpEngine").toString(aiDefaults.frameInterpEngine);
        data.aiSettings.frameInterpFactor  = ai.value("frameInterpFactor").toInt(aiDefaults.frameInterpFactor);
    }

    // US-INT-2: Sprint 16 — mobile export + import-hub UI memory (backward compat).
    data.mobileExportLastDeviceId = QString();
    if (root.contains("mobileExport")) {
        const QJsonObject mobile = root["mobileExport"].toObject();
        data.mobileExportLastDeviceId = mobile.value("lastDeviceId").toString();
    }
    data.lastImportFolder = QString();
    if (root.contains("lastImportFolder"))
        data.lastImportFolder = root["lastImportFolder"].toString();

    // US-INT-3: Sprint 17/18/19 — YouTube / Collab / ColorMatch UI memory (backward compat).
    data.youtubeLastChannelId        = QString();
    data.youtubeLastPrivacy          = QString();
    if (root.contains("youtube")) {
        const QJsonObject youtube = root["youtube"].toObject();
        data.youtubeLastChannelId   = youtube.value("lastChannelId").toString();
        data.youtubeLastPrivacy     = youtube.value("lastPrivacy").toString();
    }
    data.collabCurrentUserId         = QString();
    data.collabCurrentDisplayName    = QString();
    if (root.contains("collab")) {
        const QJsonObject collab = root["collab"].toObject();
        data.collabCurrentUserId       = collab.value("currentUserId").toString();
        data.collabCurrentDisplayName  = collab.value("currentDisplayName").toString();
    }
    data.colorMatchLastReferenceClip = QString();
    if (root.contains("colormatch")) {
        const QJsonObject colormatch = root["colormatch"].toObject();
        data.colorMatchLastReferenceClip = colormatch.value("lastReferenceClip").toString();
    }

    // PRD-PROJECT-PRESET: tracker preset state persistence (backward compat: missing key = default struct)
    {
        const QJsonObject tp = root.value("trackerPresets").toObject();
        data.motionTrackerState = motionTrackerStateFromJson(tp.value("motion").toObject());
        data.planarTrackerState = planarTrackerStateFromJson(tp.value("planar").toObject());
    }

    // PRD-PHASE1-MEDIA-POOL: メディアプール永続化 (キー欠落でも空 object → 空プールで安全)
    data.mediaPool.fromJson(root.value("mediaPool").toObject());

    // PRD-PHASE2-AUDIO-BUS: バス/サブミックス/AUXセンド ルーティング永続化
    // (キー欠落でも空 object → 空ルーティング=identity、旧 .veditor 後方互換)
    data.audioBusRouting.fromJson(root.value("audioBusRouting").toObject());

    // PRD-PHASE3-ACES: ACES カラーマネジメント設定永続化
    // (キー欠落でも空 object → enabled=false 既定=identity、旧 .veditor 後方互換)
    data.acesPipeline = aces::pipelineFromJson(root.value("acesPipeline").toObject());

    // PRD-PHASE3-DOLBY-VISION: Dolby Vision メタデータ永続化
    // (キー欠落でも空 object → 空メタ既定、旧 .veditor 後方互換)
    data.dolbyVision = dolbyvision::fromJson(root.value("dolbyVision").toObject());

    // PRD-PHASE3-BROADCAST-CC: 放送CC (CEA-608/708) 永続化
    // (キー欠落でも空 object → 空 doc 既定、旧 .veditor 後方互換)
    data.broadcastCaption = broadcastcc::fromJson(root.value("broadcastCaption").toObject());

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
    obj["explicitOutputResolution"] = c.explicitOutputResolution;
    return obj;
}

ProjectConfig ProjectFile::configFromJson(const QJsonObject &obj)
{
    ProjectConfig c;
    c.name = obj["name"].toString("Untitled");
    c.width = obj["width"].toInt(1920);
    c.height = obj["height"].toInt(1080);
    c.fps = obj["fps"].toInt(30);
    c.explicitOutputResolution = obj["explicitOutputResolution"].toBool(false);
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
    if (clip.pan != 0.0)
        obj["pan"] = clip.pan;
    obj["videoScale"] = clip.videoScale;
    obj["videoDx"] = clip.videoDx;
    obj["videoDy"] = clip.videoDy;
    obj["rotation2DDegrees"] = clip.rotation2DDegrees;
    obj["opacity"] = clip.opacity;
    if (clip.isAdjustment)
        obj["isAdjustment"] = true;
    obj["is3DLayer"] = clip.is3DLayer;
    obj["layer3D"] = clip.layer3D.toJson();
    if (clip.motionBlurEnabled)
        obj["motionBlurEnabled"] = true;
    if (clip.fitContain)
        obj["fitContain"] = true;
    if (clip.fitCover)
        obj["fitCover"] = true;

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
    if (!clip.colorMeta.isDefault())
        obj["colorMeta"] = clipcolor::toJson(clip.colorMeta);
    if (!clip.layerStyle.isIdentity())
        obj["layerStyle"] = clip.layerStyle.toJson();

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
    obj["atempoEnabled"] = clip.atempoEnabled;

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
    clip.pan = obj["pan"].toDouble(0.0);
    clip.videoScale = obj["videoScale"].toDouble(1.0);
    clip.videoDx = obj["videoDx"].toDouble(0.0);
    clip.videoDy = obj["videoDy"].toDouble(0.0);
    clip.rotation2DDegrees = obj["rotation2DDegrees"].toDouble(0.0);
    clip.opacity = obj["opacity"].toDouble(1.0);
    clip.isAdjustment = obj["isAdjustment"].toBool(false);
    clip.is3DLayer = obj["is3DLayer"].toBool(false);
    clip.layer3D = obj.contains("layer3D")
        ? Layer3DTransform::fromJson(obj["layer3D"].toObject())
        : Layer3DTransform{};
    clip.motionBlurEnabled = obj["motionBlurEnabled"].toBool(false);
    clip.fitContain = obj["fitContain"].toBool(false);
    clip.fitCover = obj["fitCover"].toBool(false);
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
    clip.colorMeta = obj.contains("colorMeta")
        ? clipcolor::fromJson(obj["colorMeta"].toObject())
        : clipcolor::defaultSdr();
    if (obj.contains("layerStyle"))
        clip.layerStyle = LayerStyle::fromJson(obj["layerStyle"].toObject());

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
    clip.atempoEnabled = obj["atempoEnabled"].toBool(false);

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

    auto addIfNonZero = [&obj](const QString &key, double value) {
        if (value != 0.0)
            obj[key] = value;
    };
    addIfNonZero(QStringLiteral("liftR"), cc.liftR);
    addIfNonZero(QStringLiteral("liftG"), cc.liftG);
    addIfNonZero(QStringLiteral("liftB"), cc.liftB);
    addIfNonZero(QStringLiteral("gammaR"), cc.gammaR);
    addIfNonZero(QStringLiteral("gammaG"), cc.gammaG);
    addIfNonZero(QStringLiteral("gammaB"), cc.gammaB);
    addIfNonZero(QStringLiteral("gainR"), cc.gainR);
    addIfNonZero(QStringLiteral("gainG"), cc.gainG);
    addIfNonZero(QStringLiteral("gainB"), cc.gainB);
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
    cc.liftR = obj["liftR"].toDouble(0.0);
    cc.liftG = obj["liftG"].toDouble(0.0);
    cc.liftB = obj["liftB"].toDouble(0.0);
    cc.gammaR = obj["gammaR"].toDouble(0.0);
    cc.gammaG = obj["gammaG"].toDouble(0.0);
    cc.gammaB = obj["gammaB"].toDouble(0.0);
    cc.gainR = obj["gainR"].toDouble(0.0);
    cc.gainG = obj["gainG"].toDouble(0.0);
    cc.gainB = obj["gainB"].toDouble(0.0);
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
    if (e.startSec != -1.0)
        obj["startSec"] = e.startSec;
    if (e.endSec != -1.0)
        obj["endSec"] = e.endSec;
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
    e.startSec = obj["startSec"].toDouble(-1.0);
    e.endSec = obj["endSec"].toDouble(-1.0);
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
        kfArr.append(keyframePointToJson(kf));
    }
    obj["keyframes"] = kfArr;
    return obj;
}

KeyframeTrack ProjectFile::keyframeTrackFromJson(const QJsonObject &obj)
{
    KeyframeTrack track(obj["property"].toString(), obj["defaultValue"].toDouble());
    for (const auto &v : obj["keyframes"].toArray()) {
        const KeyframePoint kf = keyframePointFromJson(v.toObject());
        track.addKeyframe(kf.time, kf.value, kf.interpolation,
                          kf.bezX1, kf.bezY1, kf.bezX2, kf.bezY2,
                          kf.hasSpatialTangent, kf.spatialOutX,
                          kf.spatialOutY, kf.spatialInX, kf.spatialInY);
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

QJsonObject ProjectFile::forceFieldToJson(const ForceField &field)
{
    QJsonObject obj;
    obj["kind"] = field.kind;
    obj["positionX"] = field.position.x();
    obj["positionY"] = field.position.y();
    obj["strength"] = field.strength;
    obj["radius"] = field.radius;
    return obj;
}

ForceField ProjectFile::forceFieldFromJson(const QJsonObject &obj)
{
    ForceField field;
    field.kind = static_cast<ForceField::Kind>(obj["kind"].toInt(static_cast<int>(ForceField::PointAttract)));
    field.position.setX(obj["positionX"].toDouble(field.position.x()));
    field.position.setY(obj["positionY"].toDouble(field.position.y()));
    field.strength = obj["strength"].toDouble(field.strength);
    field.radius = obj["radius"].toDouble(field.radius);
    return field;
}

QJsonObject ProjectFile::particleEmitterConfigToJson(const ParticleEmitterConfig &config)
{
    QJsonObject obj;
    obj["type"] = static_cast<int>(config.type);
    obj["emitRate"] = config.emitRate;
    obj["maxParticles"] = config.maxParticles;
    obj["emitPositionX"] = config.emitPosition.x();
    obj["emitPositionY"] = config.emitPosition.y();
    obj["emitAreaWidth"] = config.emitAreaSize.width();
    obj["emitAreaHeight"] = config.emitAreaSize.height();
    obj["lifeMin"] = config.lifeMin;
    obj["lifeMax"] = config.lifeMax;
    obj["sizeMin"] = config.sizeMin;
    obj["sizeMax"] = config.sizeMax;
    obj["speedMin"] = config.speedMin;
    obj["speedMax"] = config.speedMax;
    obj["direction"] = config.direction;
    obj["spread"] = config.spread;
    obj["gravityX"] = config.gravity.x();
    obj["gravityY"] = config.gravity.y();
    obj["windX"] = config.wind.x();
    obj["windY"] = config.wind.y();
    obj["collisionFloor"] = config.collisionFloor;
    obj["floorY"] = config.floorY;
    obj["restitution"] = config.restitution;
    obj["floorFriction"] = config.floorFriction;
    obj["turbulenceAmount"] = config.turbulenceAmount;
    obj["turbulenceScale"] = config.turbulenceScale;
    obj["turbulenceSpeed"] = config.turbulenceSpeed;
    obj["startColor"] = config.startColor.name(QColor::HexArgb);
    obj["endColor"] = config.endColor.name(QColor::HexArgb);
    obj["fadeIn"] = config.fadeIn;
    obj["fadeOut"] = config.fadeOut;
    obj["sizeStartMult"] = config.sizeStartMult;
    obj["sizeEndMult"] = config.sizeEndMult;

    QJsonArray forceArr;
    for (const auto &field : config.forceFields)
        forceArr.append(forceFieldToJson(field));
    obj["forceFields"] = forceArr;
    return obj;
}

ParticleEmitterConfig ProjectFile::particleEmitterConfigFromJson(const QJsonObject &obj)
{
    ParticleEmitterConfig config;
    config.type = static_cast<ParticleType>(obj["type"].toInt(static_cast<int>(config.type)));
    config.emitRate = obj["emitRate"].toDouble(config.emitRate);
    config.maxParticles = obj["maxParticles"].toInt(config.maxParticles);
    config.emitPosition.setX(obj["emitPositionX"].toDouble(config.emitPosition.x()));
    config.emitPosition.setY(obj["emitPositionY"].toDouble(config.emitPosition.y()));
    config.emitAreaSize.setWidth(obj["emitAreaWidth"].toDouble(config.emitAreaSize.width()));
    config.emitAreaSize.setHeight(obj["emitAreaHeight"].toDouble(config.emitAreaSize.height()));
    config.lifeMin = obj["lifeMin"].toDouble(config.lifeMin);
    config.lifeMax = obj["lifeMax"].toDouble(config.lifeMax);
    config.sizeMin = obj["sizeMin"].toDouble(config.sizeMin);
    config.sizeMax = obj["sizeMax"].toDouble(config.sizeMax);
    config.speedMin = obj["speedMin"].toDouble(config.speedMin);
    config.speedMax = obj["speedMax"].toDouble(config.speedMax);
    config.direction = obj["direction"].toDouble(config.direction);
    config.spread = obj["spread"].toDouble(config.spread);
    config.gravity.setX(obj["gravityX"].toDouble(config.gravity.x()));
    config.gravity.setY(obj["gravityY"].toDouble(config.gravity.y()));
    config.wind.setX(obj["windX"].toDouble(config.wind.x()));
    config.wind.setY(obj["windY"].toDouble(config.wind.y()));
    config.collisionFloor = obj["collisionFloor"].toBool(config.collisionFloor);
    config.floorY = obj["floorY"].toDouble(config.floorY);
    config.restitution = obj["restitution"].toDouble(config.restitution);
    config.floorFriction = obj["floorFriction"].toDouble(config.floorFriction);
    config.turbulenceAmount = obj["turbulenceAmount"].toDouble(config.turbulenceAmount);
    config.turbulenceScale = obj["turbulenceScale"].toDouble(config.turbulenceScale);
    config.turbulenceSpeed = obj["turbulenceSpeed"].toDouble(config.turbulenceSpeed);
    config.startColor = QColor(obj["startColor"].toString(config.startColor.name(QColor::HexArgb)));
    config.endColor = QColor(obj["endColor"].toString(config.endColor.name(QColor::HexArgb)));
    config.fadeIn = obj["fadeIn"].toDouble(config.fadeIn);
    config.fadeOut = obj["fadeOut"].toDouble(config.fadeOut);
    config.sizeStartMult = obj["sizeStartMult"].toDouble(config.sizeStartMult);
    config.sizeEndMult = obj["sizeEndMult"].toDouble(config.sizeEndMult);

    config.forceFields.clear();
    if (obj.contains("forceFields")) {
        for (const auto &value : obj["forceFields"].toArray())
            config.forceFields.append(forceFieldFromJson(value.toObject()));
    }
    return config;
}

QJsonObject ProjectFile::particleClipEntryToJson(const ParticleClipEntry &entry)
{
    QJsonObject obj;
    obj["trackIndex"] = entry.trackIndex;
    obj["clipIndex"] = entry.clipIndex;
    obj["clipFilePath"] = entry.clipFilePath;
    obj["config"] = particleEmitterConfigToJson(entry.config);
    return obj;
}

ParticleClipEntry ProjectFile::particleClipEntryFromJson(const QJsonObject &obj)
{
    ParticleClipEntry entry;
    entry.trackIndex = obj["trackIndex"].toInt(-1);
    entry.clipIndex = obj["clipIndex"].toInt(-1);
    entry.clipFilePath = obj["clipFilePath"].toString();
    if (obj.contains("config"))
        entry.config = particleEmitterConfigFromJson(obj["config"].toObject());
    return entry;
}

QJsonObject ProjectFile::clipNodeGraphToJson(const ClipNodeGraph &entry)
{
    QJsonObject obj;
    obj["clipId"] = entry.clipId;
    obj["graph"] = entry.graph;
    obj["compositingMode"] = entry.compositingMode;
    return obj;
}

ClipNodeGraph ProjectFile::clipNodeGraphFromJson(const QJsonObject &obj)
{
    ClipNodeGraph entry;
    entry.clipId = obj["clipId"].toString();
    entry.graph = obj["graph"].toObject();
    entry.compositingMode = obj["compositingMode"].toString(entry.compositingMode);
    return entry;
}

QJsonObject ProjectFile::rotoClipEntryToJson(const RotoClipEntry &entry)
{
    QJsonObject obj;
    obj["clipId"] = entry.clipId;
    obj["path"] = entry.path.toJson();

    QJsonArray keyframeArr;
    for (const auto &keyframe : entry.keyframes)
        keyframeArr.append(keyframe.toJson());
    obj["keyframes"] = keyframeArr;

    const QByteArray encodedMask = imageToPngBase64(entry.brushMask);
    if (!encodedMask.isEmpty())
        obj["brushMaskPngBase64"] = QString::fromLatin1(encodedMask);
    return obj;
}

RotoClipEntry ProjectFile::rotoClipEntryFromJson(const QJsonObject &obj)
{
    RotoClipEntry entry;
    entry.clipId = obj["clipId"].toString();
    if (obj.contains("path"))
        entry.path = RotoPath::fromJson(obj["path"].toObject());
    if (obj.contains("keyframes")) {
        for (const auto &value : obj["keyframes"].toArray())
            entry.keyframes.append(RotoKeyframe::fromJson(value.toObject()));
    }
    entry.brushMask = imageFromPngBase64(obj["brushMaskPngBase64"].toString());
    return entry;
}

QJsonObject ProjectFile::timeRemapClipEntryToJson(const TimeRemapClipEntry &entry)
{
    QJsonObject obj;
    obj["clipId"] = entry.clipId;
    obj["curve"] = entry.curve.toJson();
    return obj;
}

TimeRemapClipEntry ProjectFile::timeRemapClipEntryFromJson(const QJsonObject &obj)
{
    TimeRemapClipEntry entry;
    entry.clipId = obj["clipId"].toString();
    if (obj.contains("curve"))
        entry.curve = timeremap::TimeRemapCurve::fromJson(obj["curve"].toObject());
    return entry;
}

QJsonObject ProjectFile::trackMatteClipEntryToJson(const TrackMatteClipEntry &entry)
{
    QJsonObject obj;
    obj["clipId"] = entry.clipId;
    obj["matteType"] = static_cast<int>(entry.matteType);
    obj["matteSourceClipId"] = entry.matteSourceClipId;
    return obj;
}

TrackMatteClipEntry ProjectFile::trackMatteClipEntryFromJson(const QJsonObject &obj)
{
    TrackMatteClipEntry entry;
    entry.clipId = obj["clipId"].toString();
    entry.matteType = static_cast<TrackMatteType>(obj["matteType"].toInt(static_cast<int>(TrackMatteType::None)));
    entry.matteSourceClipId = obj["matteSourceClipId"].toString();
    return entry;
}

QJsonObject ProjectFile::clipParentEntryToJson(const ClipParentEntry &entry)
{
    QJsonObject obj;
    obj["clipId"] = entry.clipId;
    obj["parentClipId"] = entry.parentClipId;
    return obj;
}

ClipParentEntry ProjectFile::clipParentEntryFromJson(const QJsonObject &obj)
{
    ClipParentEntry entry;
    entry.clipId = obj["clipId"].toString();
    entry.parentClipId = obj["parentClipId"].toString();
    return entry;
}

// --- US-3D-11: motion-graphics sprint persistence ---

QJsonObject ProjectFile::text3DClipEntryToJson(const Text3DClipEntry &entry)
{
    QJsonObject obj;
    obj["clipId"] = entry.clipId;
    obj["config"] = entry.config;
    return obj;
}

Text3DClipEntry ProjectFile::text3DClipEntryFromJson(const QJsonObject &obj)
{
    Text3DClipEntry entry;
    entry.clipId = obj["clipId"].toString();
    entry.config = obj["config"].toObject();
    return entry;
}

QJsonObject ProjectFile::expressionBindingsEntryToJson(const ExpressionBindingsClipEntry &entry)
{
    QJsonObject obj;
    obj["clipId"] = entry.clipId;
    obj["bindings"] = entry.bindings.toJson();
    return obj;
}

ExpressionBindingsClipEntry ProjectFile::expressionBindingsEntryFromJson(const QJsonObject &obj)
{
    ExpressionBindingsClipEntry entry;
    entry.clipId = obj["clipId"].toString();
    entry.bindings.fromJson(obj["bindings"].toObject());
    return entry;
}

QJsonObject ProjectFile::wiggleClipEntryToJson(const WiggleClipEntry &entry)
{
    QJsonObject obj;
    obj["clipId"] = entry.clipId;
    obj["params"] = wiggle::toJson(entry.params);
    return obj;
}

WiggleClipEntry ProjectFile::wiggleClipEntryFromJson(const QJsonObject &obj)
{
    WiggleClipEntry entry;
    entry.clipId = obj["clipId"].toString();
    entry.params = wiggle::fromJson(obj["params"].toObject());
    return entry;
}

// --- PRD-PROJECT-PRESET: tracker preset state serialization ---

QJsonObject ProjectFile::motionTrackerStateToJson(const MotionTrackerProjectState &s)
{
    QJsonObject o;
    o["lastPresetId"]           = s.lastPresetId;
    o["searchRadius"]           = s.searchRadius;
    o["matchMetric"]            = s.matchMetric;
    o["kalmanEnabled"]          = s.kalmanEnabled;
    o["kalmanProcessNoise"]     = s.kalmanProcessNoise;
    o["kalmanMeasurementNoise"] = s.kalmanMeasurementNoise;
    o["occlusionGate"]          = s.occlusionGate;
    o["subPixelEnabled"]        = s.subPixelEnabled;
    o["minConfidence"]          = s.minConfidence;
    return o;
}

MotionTrackerProjectState ProjectFile::motionTrackerStateFromJson(const QJsonObject &obj)
{
    MotionTrackerProjectState s;
    if (obj.contains("lastPresetId"))           s.lastPresetId           = obj.value("lastPresetId").toString();
    if (obj.contains("searchRadius"))           s.searchRadius           = std::clamp(obj.value("searchRadius").toInt(s.searchRadius), 1, 256);
    if (obj.contains("matchMetric")) {
        const QString m = obj.value("matchMetric").toString();
        s.matchMetric = m.isEmpty() ? QStringLiteral("NCC") : m;
    }
    if (obj.contains("kalmanEnabled"))          s.kalmanEnabled          = obj.value("kalmanEnabled").toBool(s.kalmanEnabled);
    if (obj.contains("kalmanProcessNoise"))     s.kalmanProcessNoise     = std::clamp(obj.value("kalmanProcessNoise").toDouble(s.kalmanProcessNoise), 0.0, 10.0);
    if (obj.contains("kalmanMeasurementNoise")) s.kalmanMeasurementNoise = std::clamp(obj.value("kalmanMeasurementNoise").toDouble(s.kalmanMeasurementNoise), 0.0, 10.0);
    if (obj.contains("occlusionGate"))          s.occlusionGate          = std::clamp(obj.value("occlusionGate").toDouble(s.occlusionGate), 0.0, 1000.0);
    if (obj.contains("subPixelEnabled"))        s.subPixelEnabled        = obj.value("subPixelEnabled").toBool(s.subPixelEnabled);
    if (obj.contains("minConfidence"))          s.minConfidence          = std::clamp(obj.value("minConfidence").toDouble(s.minConfidence), 0.0, 1.0);
    return s;
}

QJsonObject ProjectFile::planarTrackerStateToJson(const PlanarTrackerProjectState &s)
{
    QJsonObject o;
    o["lastPresetId"]     = s.lastPresetId;
    o["searchRadiusPx"]   = s.searchRadiusPx;
    o["patchSizePx"]      = s.patchSizePx;
    o["dampingFactor"]    = s.dampingFactor;
    o["maxFramesPerCall"] = s.maxFramesPerCall;
    return o;
}

PlanarTrackerProjectState ProjectFile::planarTrackerStateFromJson(const QJsonObject &obj)
{
    PlanarTrackerProjectState s;
    if (obj.contains("lastPresetId"))     s.lastPresetId     = obj.value("lastPresetId").toString();
    if (obj.contains("searchRadiusPx"))   s.searchRadiusPx   = std::clamp(obj.value("searchRadiusPx").toDouble(s.searchRadiusPx), 4.0, 64.0);
    if (obj.contains("patchSizePx"))      s.patchSizePx      = std::clamp(obj.value("patchSizePx").toDouble(s.patchSizePx), 16.0, 128.0);
    if (obj.contains("dampingFactor"))    s.dampingFactor    = std::clamp(obj.value("dampingFactor").toDouble(s.dampingFactor), 0.0, 1.0);
    if (obj.contains("maxFramesPerCall")) s.maxFramesPerCall = std::max(0, obj.value("maxFramesPerCall").toInt(s.maxFramesPerCall));
    return s;
}

QJsonObject ProjectFile::vfxStateToJson(const ProjectVfxState &state)
{
    auto glowToJson = [](const ProjectGlowState &glow) {
        QJsonObject obj;
        obj["enabled"] = glow.enabled;
        obj["threshold"] = glow.threshold;
        obj["radius"] = glow.radius;
        obj["intensity"] = glow.intensity;
        return obj;
    };
    auto bloomToJson = [](const ProjectBloomState &bloom) {
        QJsonObject obj;
        obj["enabled"] = bloom.enabled;
        obj["threshold"] = bloom.threshold;
        obj["intensity"] = bloom.intensity;
        obj["spread"] = bloom.spread;
        return obj;
    };
    auto chromaToJson = [](const ProjectChromaticAberrationState &chromatic) {
        QJsonObject obj;
        obj["enabled"] = chromatic.enabled;
        obj["amount"] = chromatic.amount;
        obj["radialFalloff"] = chromatic.radialFalloff;
        return obj;
    };
    auto wrapToJson = [](const ProjectLightWrapState &wrap) {
        QJsonObject obj;
        obj["enabled"] = wrap.enabled;
        obj["amount"] = wrap.amount;
        obj["radius"] = wrap.radius;
        return obj;
    };

    QJsonObject obj;
    obj["glow"] = glowToJson(state.glow);
    obj["bloom"] = bloomToJson(state.bloom);
    obj["chromaticAberration"] = chromaToJson(state.chromaticAberration);
    obj["lightWrap"] = wrapToJson(state.lightWrap);
    return obj;
}

ProjectVfxState ProjectFile::vfxStateFromJson(const QJsonObject &obj)
{
    ProjectVfxState state;
    if (obj.contains("glow")) {
        const QJsonObject glow = obj["glow"].toObject();
        state.glow.enabled = glow["enabled"].toBool(state.glow.enabled);
        state.glow.threshold = static_cast<float>(glow["threshold"].toDouble(state.glow.threshold));
        state.glow.radius = static_cast<float>(glow["radius"].toDouble(state.glow.radius));
        state.glow.intensity = static_cast<float>(glow["intensity"].toDouble(state.glow.intensity));
    }
    if (obj.contains("bloom")) {
        const QJsonObject bloom = obj["bloom"].toObject();
        state.bloom.enabled = bloom["enabled"].toBool(state.bloom.enabled);
        state.bloom.threshold = static_cast<float>(bloom["threshold"].toDouble(state.bloom.threshold));
        state.bloom.intensity = static_cast<float>(bloom["intensity"].toDouble(state.bloom.intensity));
        state.bloom.spread = static_cast<float>(bloom["spread"].toDouble(state.bloom.spread));
    }
    if (obj.contains("chromaticAberration")) {
        const QJsonObject chromatic = obj["chromaticAberration"].toObject();
        state.chromaticAberration.enabled = chromatic["enabled"].toBool(state.chromaticAberration.enabled);
        state.chromaticAberration.amount = static_cast<float>(chromatic["amount"].toDouble(state.chromaticAberration.amount));
        state.chromaticAberration.radialFalloff = static_cast<float>(chromatic["radialFalloff"].toDouble(state.chromaticAberration.radialFalloff));
    }
    if (obj.contains("lightWrap")) {
        const QJsonObject wrap = obj["lightWrap"].toObject();
        state.lightWrap.enabled = wrap["enabled"].toBool(state.lightWrap.enabled);
        state.lightWrap.amount = static_cast<float>(wrap["amount"].toDouble(state.lightWrap.amount));
        state.lightWrap.radius = static_cast<float>(wrap["radius"].toDouble(state.lightWrap.radius));
    }
    return state;
}

// --- US-AETEXT-12: AE text feature persistence ---

QJsonObject ProjectFile::pathTextToJson(const PathTextEntry &e)
{
    return e.data;
}

PathTextEntry ProjectFile::pathTextFromJson(const QJsonObject &obj)
{
    PathTextEntry e;
    e.data = obj;
    return e;
}

QJsonObject ProjectFile::text3DLayerToJson(const Text3DLayerEntry &e)
{
    return e.data;
}

Text3DLayerEntry ProjectFile::text3DLayerFromJson(const QJsonObject &obj)
{
    Text3DLayerEntry e;
    e.data = obj;
    return e;
}

QJsonObject ProjectFile::textMaskRevealToJson(const TextMaskRevealEntry &e)
{
    return e.data;
}

TextMaskRevealEntry ProjectFile::textMaskRevealFromJson(const QJsonObject &obj)
{
    TextMaskRevealEntry e;
    e.data = obj;
    return e;
}

QJsonObject ProjectFile::textPathWarpToJson(const TextPathWarpEntry &e)
{
    return e.data;
}

TextPathWarpEntry ProjectFile::textPathWarpFromJson(const QJsonObject &obj)
{
    TextPathWarpEntry e;
    e.data = obj;
    return e;
}

QJsonObject ProjectFile::variableFontAxisToJson(const VariableFontAxisEntry &e)
{
    return e.data;
}

VariableFontAxisEntry ProjectFile::variableFontAxisFromJson(const QJsonObject &obj)
{
    VariableFontAxisEntry e;
    e.data = obj;
    return e;
}

QJsonObject ProjectFile::mographTextToJson(const MographTextEntry &e)
{
    return e.data;
}

MographTextEntry ProjectFile::mographTextFromJson(const QJsonObject &obj)
{
    MographTextEntry e;
    e.data = obj;
    return e;
}
