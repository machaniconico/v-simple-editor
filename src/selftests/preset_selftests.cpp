// src/selftests/preset_selftests.cpp
// PRD-SPLIT-MAIN-2 Phase 1: Tracker / Planar / Project preset selftests
// moved verbatim from src/main.cpp. Dispatch via SelftestRegistry.{h,cpp}.

#include "TrackerPreset.h"
#include "TrackerPresetRegistry.h"
#include "MotionTracker.h"
#if __has_include("PlanarTrackerPreset.h")
#include "PlanarTracker.h"
#include "PlanarTrackerPreset.h"
#include "PlanarTrackerPresetRegistry.h"
#define HAVE_PLANARTRACKER_PRESET 1
#endif
#include "ProjectFile.h"

#include <QString>
#include <QStringLiteral>
#include <QFile>
#include <QTemporaryFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDebug>

extern void writeLogLine(const QString& level, const QString& msg);

// US-B3-8: DLL-ready HDR routing regression gate
// US-TP-7: tracker preset application regression gate
// (VEDITOR_TRACKER_PRESET_SELFTEST=1).
//
// Validates without spawning any subprocess or QApplication::exec():
//   (1) builtinPresets().size() == 7
//   (2) findBuiltin(id) succeeds for all 7
//   (3) toJson/fromJson round-trip is lossless for all 7
//   (4) fromJson rejects 3 classes of invalid input
//   (5) applyToMotionTracker runs without crash for all 7 + nullptr safety
//   (6) Registry::allPresets().size() >= 7
//   (7) Registry CRUD: save -> list -> remove -> list
int runTrackerPresetSelftest()
{
    using tracker_preset::TrackerPreset;
    auto fail = [](const QString& msg) -> int {
        writeLogLine("CRIT", QString("TRACKER-PRESET selftest FAILED: %1").arg(msg));
        return 1;
    };
    writeLogLine("INFO", "TRACKER-PRESET selftest: preset application regression gate");

    // (1) builtinPresets().size() == 7
    const auto& builtins = tracker_preset::builtinPresets();
    if (builtins.size() != 7)
        return fail(QString("builtinPresets size != 7 (got %1)").arg(builtins.size()));
    writeLogLine("INFO", QString("TRACKER-PRESET (1): builtinPresets count = %1").arg(builtins.size()));

    // (2) findBuiltin success for all 7
    for (const auto& p : builtins) {
        if (!tracker_preset::findBuiltin(p.id).has_value())
            return fail(QString("findBuiltin failed for id=%1").arg(p.id));
    }
    writeLogLine("INFO", "TRACKER-PRESET (2): findBuiltin OK for all 7");

    // (3) toJson/fromJson round-trip
    for (const auto& p : builtins) {
        const auto j  = tracker_preset::toJson(p);
        const auto rt = tracker_preset::fromJson(j);
        if (!rt.has_value())
            return fail(QString("fromJson(toJson(p)) failed for id=%1").arg(p.id));
        const auto& q = *rt;
        if (q.id != p.id || q.displayName != p.displayName
            || q.searchRadius != p.searchRadius
            || q.kalmanEnabled != p.kalmanEnabled
            || q.matchMetric != p.matchMetric
            || q.subPixelEnabled != p.subPixelEnabled)
            return fail(QString("round-trip mismatch for id=%1").arg(p.id));
    }
    writeLogLine("INFO", "TRACKER-PRESET (3): round-trip OK for all 7");

    // (4) fromJson rejects invalid inputs
    {
        QJsonObject bad1; bad1["id"] = "x"; bad1["displayName"] = "x"; bad1["searchRadius"] = -1;
        if (tracker_preset::fromJson(bad1).has_value())
            return fail("fromJson accepted searchRadius=-1");
        QJsonObject bad2; bad2["id"] = "x"; bad2["displayName"] = "x"; bad2["matchMetric"] = "UNKNOWN";
        if (tracker_preset::fromJson(bad2).has_value())
            return fail("fromJson accepted matchMetric=UNKNOWN");
        QJsonObject bad3; bad3["id"] = "x"; bad3["displayName"] = "x"; bad3["occlusionGate"] = 2.0;
        if (tracker_preset::fromJson(bad3).has_value())
            return fail("fromJson accepted occlusionGate=2.0");
    }
    writeLogLine("INFO", "TRACKER-PRESET (4): fromJson rejects 3 invalid inputs");

    // (5) applyToMotionTracker — all 7 + nullptr safety
    MotionTracker tracker;
    for (const auto& p : builtins)
        tracker_preset::applyToMotionTracker(&tracker, p);
    tracker_preset::applyToMotionTracker(nullptr, builtins.front());
    writeLogLine("INFO", "TRACKER-PRESET (5): applyToMotionTracker OK for all 7 + nullptr");

    // (6) Registry::allPresets().size() >= 7
    const int registrySize = static_cast<int>(tracker_preset::Registry::instance().allPresets().size());
    if (registrySize < 7)
        return fail(QString("Registry::allPresets size < 7 (got %1)").arg(registrySize));
    writeLogLine("INFO", QString("TRACKER-PRESET (6): Registry size = %1").arg(registrySize));

    // (7) Registry CRUD
    TrackerPreset custom = builtins.front();
    custom.id          = QStringLiteral("__selftest-tmp-custom");
    custom.displayName = QStringLiteral("__selftest-tmp-custom");
    if (!tracker_preset::Registry::instance().saveUserPreset(custom))
        return fail("saveUserPreset failed");
    {
        bool found = false;
        for (const auto& p : tracker_preset::Registry::instance().allPresets())
            if (p.id == custom.id) { found = true; break; }
        if (!found)
            return fail("saved preset not in allPresets after save");
    }
    if (!tracker_preset::Registry::instance().removeUserPreset(custom.id))
        return fail("removeUserPreset failed");
    for (const auto& p : tracker_preset::Registry::instance().allPresets())
        if (p.id == custom.id)
            return fail("removed preset still in allPresets");
    writeLogLine("INFO", "TRACKER-PRESET (7): Registry CRUD OK");

    // (8) JSON I/O round-trip via QFile (QTemporaryFile)
    {
        for (const auto& p : builtins) {
            QTemporaryFile tmp;
            if (!tmp.open()) return fail(QString("QTemporaryFile open failed for id=%1").arg(p.id));
            const QString tmpPath = tmp.fileName();
            tmp.close();

            // toJson -> QFile write
            const auto obj = tracker_preset::toJson(p);
            QFile out(tmpPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
                return fail(QString("QFile WriteOnly failed for id=%1").arg(p.id));
            out.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
            out.close();

            // QFile read -> fromJson
            QFile in(tmpPath);
            if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
                return fail(QString("QFile ReadOnly failed for id=%1").arg(p.id));
            const QByteArray bytes = in.readAll();
            in.close();
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return fail(QString("JSON parse failed for id=%1: %2").arg(p.id, err.errorString()));
            const auto rt = tracker_preset::fromJson(doc.object());
            if (!rt.has_value())
                return fail(QString("fromJson via file failed for id=%1").arg(p.id));
            const auto& q = *rt;
            if (q.id != p.id || q.displayName != p.displayName
                || q.searchRadius != p.searchRadius
                || q.kalmanEnabled != p.kalmanEnabled
                || q.matchMetric != p.matchMetric
                || q.description != p.description)
                return fail(QString("QFile round-trip mismatch for id=%1").arg(p.id));
        }
    }
    qInfo().noquote() << "[INFO] TRACKER-PRESET (8): JSON QFile round-trip OK for all 7";

    // (9) description 非空 gate (US-MTD2-1 ゲート)
    {
        for (const auto& p : builtins) {
            if (p.description.isEmpty())
                return fail(QString("description is empty for id=%1").arg(p.id));
        }
    }
    qInfo().noquote() << "[INFO] TRACKER-PRESET (9): description non-empty for all 7";

    // (10) description JSON round-trip
    {
        TrackerPreset sample = builtins.front();
        sample.description = QStringLiteral("__selftest description ALPHA");
        const auto obj = tracker_preset::toJson(sample);
        if (!obj.contains("description") || obj["description"].toString() != sample.description)
            return fail("toJson did not include description");
        const auto rt = tracker_preset::fromJson(obj);
        if (!rt.has_value() || rt->description != sample.description)
            return fail("fromJson did not restore description");
    }
    qInfo().noquote() << "[INFO] TRACKER-PRESET (10): description JSON round-trip OK";

    writeLogLine("INFO", "TRACKER-PRESET selftest PASSED");
    return 0;
}

// (VEDITOR_PLANAR_PRESET_SELFTEST=1).
//
// Validates PlanarTrackerPreset system without spawning any subprocess or QApplication::exec():
//   (1) builtinPresets().size() == 5
//   (2) findBuiltin(id) succeeds for all 5
//   (3) toJson/fromJson round-trip is lossless for all 5
//   (4) fromJson rejects 3 classes of invalid input
//   (5) applyToPlanarTracker runs without crash for all 5 + nullptr safety
//   (6) Registry::allPresets().size() >= 5
//   (7) Registry CRUD: save -> list -> remove -> list
//   (8) JSON QFile round-trip via QTemporaryFile for all 5
//   (9) description non-empty for all 5
//   (10) description JSON round-trip
#ifdef HAVE_PLANARTRACKER_PRESET
int runPlanarPresetSelftest()
{
    using planar_tracker_preset::PlanarTrackerPreset;
    auto fail = [](const QString& msg) -> int {
        qCritical().noquote() << "[CRIT] PLANAR-PRESET selftest FAILED:" << msg;
        return 1;
    };
    qInfo().noquote() << "[INFO] PLANAR-PRESET selftest: preset application regression gate";

    // (1) builtinPresets().size() == 5
    const auto& builtins = planar_tracker_preset::builtinPresets();
    if (builtins.size() != 5)
        return fail(QString("builtinPresets size != 5 (got %1)").arg(builtins.size()));
    qInfo().noquote() << "[INFO] PLANAR-PRESET (1): builtinPresets count =" << builtins.size();

    // (2) findBuiltin OK for all 5
    for (const auto& p : builtins) {
        if (!planar_tracker_preset::findBuiltin(p.id).has_value())
            return fail(QString("findBuiltin failed for id=%1").arg(p.id));
    }
    qInfo().noquote() << "[INFO] PLANAR-PRESET (2): findBuiltin OK for all 5";

    // (3) toJson/fromJson round-trip
    for (const auto& p : builtins) {
        const auto j = planar_tracker_preset::toJson(p);
        const auto rt = planar_tracker_preset::fromJson(j);
        if (!rt.has_value())
            return fail(QString("fromJson(toJson(p)) failed for id=%1").arg(p.id));
        const auto& q = *rt;
        if (q.id != p.id || q.displayName != p.displayName
            || q.searchRadiusPx != p.searchRadiusPx
            || q.patchSizePx != p.patchSizePx
            || q.dampingFactor != p.dampingFactor
            || q.maxFramesPerCall != p.maxFramesPerCall
            || q.description != p.description)
            return fail(QString("round-trip mismatch for id=%1").arg(p.id));
    }
    qInfo().noquote() << "[INFO] PLANAR-PRESET (3): round-trip OK for all 5";

    // (4) fromJson 値域不正 → nullopt
    {
        QJsonObject bad1; bad1["id"]="x"; bad1["displayName"]="x"; bad1["searchRadiusPx"]=2.0;
        if (planar_tracker_preset::fromJson(bad1).has_value())
            return fail("fromJson accepted searchRadiusPx=2.0");
        QJsonObject bad2; bad2["id"]="x"; bad2["displayName"]="x"; bad2["patchSizePx"]=200.0;
        if (planar_tracker_preset::fromJson(bad2).has_value())
            return fail("fromJson accepted patchSizePx=200.0");
        QJsonObject bad3; bad3["id"]="x"; bad3["displayName"]="x"; bad3["dampingFactor"]=1.5;
        if (planar_tracker_preset::fromJson(bad3).has_value())
            return fail("fromJson accepted dampingFactor=1.5");
    }
    qInfo().noquote() << "[INFO] PLANAR-PRESET (4): fromJson rejects 3 invalid inputs";

    // (5) applyToPlanarTracker — 5 preset 全て + nullptr safety
    planar::Tracker tracker;
    for (const auto& p : builtins)
        planar_tracker_preset::applyToPlanarTracker(&tracker, p);
    planar_tracker_preset::applyToPlanarTracker(nullptr, builtins.front());
    qInfo().noquote() << "[INFO] PLANAR-PRESET (5): applyToPlanarTracker OK for all 5 + nullptr";

    // (6) Registry::allPresets().size() >= 5
    const int registrySize = static_cast<int>(
        planar_tracker_preset::Registry::instance().allPresets().size());
    if (registrySize < 5)
        return fail(QString("Registry size < 5 (got %1)").arg(registrySize));
    qInfo().noquote() << "[INFO] PLANAR-PRESET (6): Registry size =" << registrySize;

    // (7) Registry CRUD
    {
        PlanarTrackerPreset custom = builtins.front();
        custom.id = QStringLiteral("__selftest-planar-tmp");
        custom.displayName = QStringLiteral("__selftest-planar-tmp");
        if (!planar_tracker_preset::Registry::instance().saveUserPreset(custom))
            return fail("saveUserPreset failed");
        bool found = false;
        for (const auto& p : planar_tracker_preset::Registry::instance().allPresets())
            if (p.id == custom.id) { found = true; break; }
        if (!found) return fail("saved preset not in allPresets");
        if (!planar_tracker_preset::Registry::instance().removeUserPreset(custom.id))
            return fail("removeUserPreset failed");
        for (const auto& p : planar_tracker_preset::Registry::instance().allPresets())
            if (p.id == custom.id) return fail("removed preset still in allPresets");
    }
    qInfo().noquote() << "[INFO] PLANAR-PRESET (7): Registry CRUD OK";

    // (8) JSON QFile round-trip via QTemporaryFile
    for (const auto& p : builtins) {
        QTemporaryFile tmp;
        if (!tmp.open()) return fail(QString("QTemporaryFile open failed for id=%1").arg(p.id));
        const QString tmpPath = tmp.fileName();
        tmp.close();
        const auto obj = planar_tracker_preset::toJson(p);
        QFile out(tmpPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
            return fail(QString("QFile write open failed for id=%1").arg(p.id));
        out.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        out.close();
        QFile in(tmpPath);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
            return fail(QString("QFile read open failed for id=%1").arg(p.id));
        const QByteArray bytes = in.readAll();
        in.close();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            return fail(QString("JSON parse failed for id=%1: %2").arg(p.id, err.errorString()));
        const auto rt = planar_tracker_preset::fromJson(doc.object());
        if (!rt.has_value())
            return fail(QString("fromJson via file failed for id=%1").arg(p.id));
        const auto& q = *rt;
        if (q.id != p.id || q.searchRadiusPx != p.searchRadiusPx
            || q.patchSizePx != p.patchSizePx || q.dampingFactor != p.dampingFactor)
            return fail(QString("QFile round-trip mismatch for id=%1").arg(p.id));
    }
    qInfo().noquote() << "[INFO] PLANAR-PRESET (8): JSON QFile round-trip OK for all 5";

    // (9) description 非空
    for (const auto& p : builtins) {
        if (p.description.isEmpty())
            return fail(QString("description is empty for id=%1").arg(p.id));
    }
    qInfo().noquote() << "[INFO] PLANAR-PRESET (9): description non-empty for all 5";

    // (10) description JSON round-trip
    {
        PlanarTrackerPreset sample = builtins.front();
        sample.description = QStringLiteral("__selftest description PLANAR");
        const auto obj = planar_tracker_preset::toJson(sample);
        if (!obj.contains("description") || obj["description"].toString() != sample.description)
            return fail("toJson did not include description");
        const auto rt = planar_tracker_preset::fromJson(obj);
        if (!rt.has_value() || rt->description != sample.description)
            return fail("fromJson did not restore description");
    }
    qInfo().noquote() << "[INFO] PLANAR-PRESET (10): description JSON round-trip OK";

    qInfo().noquote() << "[INFO] PLANAR-PRESET selftest PASSED";
    return 0;
}
#endif // HAVE_PLANARTRACKER_PRESET

// runProjectPresetSelftest — argv-switch --selftest=project-preset
// (VEDITOR_PROJECT_PRESET_SELFTEST=1).
//
// Validates MotionTrackerProjectState + PlanarTrackerProjectState JSON I/O
// without spawning any subprocess or QApplication::exec():
//   gate1  - MotionTrackerProjectState defaults
//   gate2  - PlanarTrackerProjectState defaults
//   gate3  - MotionTrackerProjectState JSON round-trip
//   gate4  - PlanarTrackerProjectState JSON round-trip
//   gate5  - ProjectData (defaults) toJsonString/fromJsonString round-trip
//   gate6  - ProjectData (custom values) toJsonString/fromJsonString round-trip
//   gate7  - legacy format (no trackerPresets key) yields defaults without crash
//   gate8  - out-of-range values are clamped by fromJson
//   gate9  - JSON schema contains trackerPresets / motion / planar keys
//   gate10 - empty lastPresetId round-trip
int runProjectPresetSelftest()
{
    qInfo().noquote() << "[PROJECT-PRESET-SELFTEST] start";
    writeLogLine("INFO", "[PROJECT-PRESET-SELFTEST] start");

    int passed = 0;
    int failed = 0;
    auto pass = [&](const char* gateName) {
        qInfo().noquote() << QStringLiteral("[PROJECT-PRESET-SELFTEST] %1 PASS").arg(QString::fromLatin1(gateName));
        writeLogLine("INFO", QStringLiteral("[PROJECT-PRESET-SELFTEST] %1 PASS").arg(QString::fromLatin1(gateName)));
        ++passed;
    };
    auto fail = [&](const char* gateName, const QString& reason) {
        qCritical().noquote() << QStringLiteral("[PROJECT-PRESET-SELFTEST] %1 FAIL: %2").arg(QString::fromLatin1(gateName), reason);
        writeLogLine("CRIT", QStringLiteral("[PROJECT-PRESET-SELFTEST] %1 FAIL: %2").arg(QString::fromLatin1(gateName), reason));
        ++failed;
    };

    // gate1: MotionTrackerProjectState defaults
    {
        MotionTrackerProjectState s;
        if (s.lastPresetId.isEmpty()
            && s.searchRadius == 16
            && s.matchMetric == QStringLiteral("NCC")
            && s.kalmanEnabled == false
            && qFuzzyCompare(s.kalmanProcessNoise, 0.01)
            && qFuzzyCompare(s.kalmanMeasurementNoise, 0.1)
            && qFuzzyCompare(s.occlusionGate, 30.0)
            && s.subPixelEnabled == true
            && qFuzzyCompare(s.minConfidence, 0.5)) {
            pass("gate1-motion-defaults");
        } else {
            fail("gate1-motion-defaults", QStringLiteral("default values mismatch"));
        }
    }

    // gate2: PlanarTrackerProjectState defaults
    {
        PlanarTrackerProjectState s;
        if (s.lastPresetId.isEmpty()
            && qFuzzyCompare(s.searchRadiusPx, 16.0)
            && qFuzzyCompare(s.patchSizePx, 32.0)
            && qFuzzyCompare(s.dampingFactor, 0.3)
            && s.maxFramesPerCall == 0) {
            pass("gate2-planar-defaults");
        } else {
            fail("gate2-planar-defaults", QStringLiteral("default values mismatch"));
        }
    }

    // gate3: MotionTrackerProjectState JSON round-trip
    {
        MotionTrackerProjectState orig;
        orig.lastPresetId = QStringLiteral("custom-foo");
        orig.searchRadius = 24;
        orig.matchMetric = QStringLiteral("SSD");
        orig.kalmanEnabled = true;
        orig.kalmanProcessNoise = 0.05;
        orig.kalmanMeasurementNoise = 0.2;
        orig.occlusionGate = 60.0;
        orig.subPixelEnabled = false;
        orig.minConfidence = 0.7;

        const QJsonObject j = ProjectFile::motionTrackerStateToJson(orig);
        const MotionTrackerProjectState rt = ProjectFile::motionTrackerStateFromJson(j);

        if (rt.lastPresetId == orig.lastPresetId
            && rt.searchRadius == orig.searchRadius
            && rt.matchMetric == orig.matchMetric
            && rt.kalmanEnabled == orig.kalmanEnabled
            && qFuzzyCompare(rt.kalmanProcessNoise, orig.kalmanProcessNoise)
            && qFuzzyCompare(rt.kalmanMeasurementNoise, orig.kalmanMeasurementNoise)
            && qFuzzyCompare(rt.occlusionGate, orig.occlusionGate)
            && rt.subPixelEnabled == orig.subPixelEnabled
            && qFuzzyCompare(rt.minConfidence, orig.minConfidence)) {
            pass("gate3-motion-round-trip");
        } else {
            fail("gate3-motion-round-trip", QStringLiteral("field mismatch"));
        }
    }

    // gate4: PlanarTrackerProjectState JSON round-trip
    {
        PlanarTrackerProjectState orig;
        orig.lastPresetId = QStringLiteral("robust-motion");
        orig.searchRadiusPx = 32.0;
        orig.patchSizePx = 48.0;
        orig.dampingFactor = 0.5;
        orig.maxFramesPerCall = 100;

        const QJsonObject j = ProjectFile::planarTrackerStateToJson(orig);
        const PlanarTrackerProjectState rt = ProjectFile::planarTrackerStateFromJson(j);

        if (rt.lastPresetId == orig.lastPresetId
            && qFuzzyCompare(rt.searchRadiusPx, orig.searchRadiusPx)
            && qFuzzyCompare(rt.patchSizePx, orig.patchSizePx)
            && qFuzzyCompare(rt.dampingFactor, orig.dampingFactor)
            && rt.maxFramesPerCall == orig.maxFramesPerCall) {
            pass("gate4-planar-round-trip");
        } else {
            fail("gate4-planar-round-trip", QStringLiteral("field mismatch"));
        }
    }

    // gate5: ProjectData (defaults) toJsonString/fromJsonString round-trip
    {
        ProjectData data;
        const QString json = ProjectFile::toJsonString(data);
        ProjectData restored;
        if (!ProjectFile::fromJsonString(json, restored)) {
            fail("gate5-projectdata-defaults-round-trip", QStringLiteral("fromJsonString failed"));
        } else if (restored.motionTrackerState.searchRadius == 16
                   && restored.motionTrackerState.matchMetric == QStringLiteral("NCC")
                   && qFuzzyCompare(restored.planarTrackerState.searchRadiusPx, 16.0)
                   && qFuzzyCompare(restored.planarTrackerState.patchSizePx, 32.0)) {
            pass("gate5-projectdata-defaults-round-trip");
        } else {
            fail("gate5-projectdata-defaults-round-trip", QStringLiteral("defaults not preserved"));
        }
    }

    // gate6: ProjectData (custom values) toJsonString/fromJsonString round-trip
    {
        ProjectData data;
        data.motionTrackerState.lastPresetId = QStringLiteral("custom-a");
        data.motionTrackerState.searchRadius = 32;
        data.motionTrackerState.matchMetric = QStringLiteral("ZNCC");
        data.planarTrackerState.lastPresetId = QStringLiteral("fast-preview");
        data.planarTrackerState.searchRadiusPx = 12.0;
        data.planarTrackerState.dampingFactor = 0.2;

        const QString json = ProjectFile::toJsonString(data);
        ProjectData restored;
        const bool ok = ProjectFile::fromJsonString(json, restored);
        if (ok
            && restored.motionTrackerState.lastPresetId == QStringLiteral("custom-a")
            && restored.motionTrackerState.searchRadius == 32
            && restored.motionTrackerState.matchMetric == QStringLiteral("ZNCC")
            && restored.planarTrackerState.lastPresetId == QStringLiteral("fast-preview")
            && qFuzzyCompare(restored.planarTrackerState.searchRadiusPx, 12.0)
            && qFuzzyCompare(restored.planarTrackerState.dampingFactor, 0.2)) {
            pass("gate6-projectdata-custom-round-trip");
        } else {
            fail("gate6-projectdata-custom-round-trip", QStringLiteral("custom values lost"));
        }
    }

    // gate7: legacy format (no trackerPresets key) yields defaults without crash
    {
        const QString oldFormat = QStringLiteral("{ \"version\": \"old\", \"config\": {} }");
        ProjectData restored;
        ProjectFile::fromJsonString(oldFormat, restored);  // return value intentionally ignored
        if (restored.motionTrackerState.searchRadius == 16
            && restored.motionTrackerState.matchMetric == QStringLiteral("NCC")
            && qFuzzyCompare(restored.planarTrackerState.searchRadiusPx, 16.0)) {
            pass("gate7-legacy-format-defaults");
        } else {
            fail("gate7-legacy-format-defaults", QStringLiteral("legacy format did not yield defaults"));
        }
    }

    // gate8: out-of-range values are clamped by fromJson
    {
        QJsonObject jMotion;
        jMotion["searchRadius"] = -5;
        jMotion["minConfidence"] = 2.5;
        jMotion["occlusionGate"] = -50.0;
        const MotionTrackerProjectState m = ProjectFile::motionTrackerStateFromJson(jMotion);

        QJsonObject jPlanar;
        jPlanar["searchRadiusPx"] = 200.0;
        jPlanar["patchSizePx"] = 8.0;
        jPlanar["dampingFactor"] = 2.0;
        const PlanarTrackerProjectState p = ProjectFile::planarTrackerStateFromJson(jPlanar);

        const bool motionOK = (m.searchRadius >= 1 && m.searchRadius <= 256)
                              && (m.minConfidence >= 0.0 && m.minConfidence <= 1.0)
                              && (m.occlusionGate >= 0.0 && m.occlusionGate <= 1000.0);
        const bool planarOK = (p.searchRadiusPx >= 4.0 && p.searchRadiusPx <= 64.0)
                              && (p.patchSizePx >= 16.0 && p.patchSizePx <= 128.0)
                              && (p.dampingFactor >= 0.0 && p.dampingFactor <= 1.0);
        if (motionOK && planarOK) {
            pass("gate8-clamp");
        } else {
            fail("gate8-clamp",
                 QStringLiteral("clamp failed: m.sr=%1 m.mc=%2 m.og=%3 p.sr=%4 p.ps=%5 p.df=%6")
                     .arg(m.searchRadius).arg(m.minConfidence).arg(m.occlusionGate)
                     .arg(p.searchRadiusPx).arg(p.patchSizePx).arg(p.dampingFactor));
        }
    }

    // gate9: JSON schema contains trackerPresets / motion / planar keys
    {
        ProjectData data;
        const QString jsonStr = ProjectFile::toJsonString(data);
        const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
        const QJsonObject root = doc.object();
        if (root.contains("trackerPresets")
            && root.value("trackerPresets").toObject().contains("motion")
            && root.value("trackerPresets").toObject().contains("planar")) {
            pass("gate9-schema");
        } else {
            fail("gate9-schema", QStringLiteral("trackerPresets / motion / planar key missing"));
        }
    }

    // gate10: empty lastPresetId round-trip
    {
        MotionTrackerProjectState s;  // lastPresetId default = empty string
        const QJsonObject j = ProjectFile::motionTrackerStateToJson(s);
        const MotionTrackerProjectState rt = ProjectFile::motionTrackerStateFromJson(j);
        if (rt.lastPresetId.isEmpty()) {
            pass("gate10-empty-preset-id");
        } else {
            fail("gate10-empty-preset-id", QStringLiteral("empty lastPresetId not preserved"));
        }
    }

    const QString summary = QStringLiteral("PROJECT-PRESET-SELFTEST: %1/%2 PASS").arg(passed).arg(passed + failed);
    if (failed == 0) {
        qInfo().noquote() << summary;
    } else {
        qCritical().noquote() << summary;
    }
    writeLogLine(failed == 0 ? "INFO" : "CRIT", summary);
    return failed == 0 ? 0 : 1;
}
