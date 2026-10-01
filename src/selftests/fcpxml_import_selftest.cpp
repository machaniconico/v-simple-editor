#include "../FcpxmlImport.h"
#include "../FcpxmlExporter.h"
#include "../UndoManager.h"
#include "SelftestGate.h"
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

int runFcpxmlImportSelftest()
{
    selftest::GateTally tally;
    auto gate = [&](int n, bool ok) {
        tally.gate(n, ok);
    };
    auto near = [](double a, double b) { return std::abs(a - b) < 1e-10; };
    QTemporaryDir dir;
    const QVector<fcpx::xml::ClipEntry> source{
        {dir.filePath(QStringLiteral("missing & one.mov")), 2.0, 3.0, 1.0, QStringLiteral("一 & <素材>")},
        {dir.filePath(QStringLiteral("missing two.mov")), 7.0, 2.0, 4.0, QStringLiteral("二")}};
    fcpx::xml::ExporterConfig cfg; cfg.projectName = QStringLiteral("往復");
    const QString path = dir.filePath(QStringLiteral("roundtrip.fcpxml"));
    bool roundtrip = dir.isValid() && fcpx::xml::exportProject(path, source, cfg);
    const auto imported = fcpx::xml::readFcpxml(path);
    roundtrip &= imported.error.isEmpty() && imported.title == cfg.projectName
        && imported.videoTracks.size() == 1 && imported.videoTracks[0].size() == source.size();
    double position = 0.0;
    for (int i = 0; i < qMin(imported.videoTracks[0].size(), source.size()); ++i) {
        const auto &c = imported.videoTracks[0][i]; const auto &e = source[i];
        position += c.leadInSec;
        roundtrip &= c.filePath == e.filePath && c.displayName == e.name
            && near(position, e.offset) && near(c.outPoint - c.inPoint, e.duration)
            && near(c.inPoint, e.startInSource);
        std::fprintf(stderr, "G1 clip=%d pathMatch=%d nameMatch=%d offset=%.9f expectedOffset=%.9f "
            "duration=%.9f expectedDuration=%.9f inPoint=%.9f expectedInPoint=%.9f\n",
            i, int(c.filePath == e.filePath), int(c.displayName == e.name), position, e.offset,
            c.outPoint - c.inPoint, e.duration, c.inPoint, e.startInSource);
        position += c.effectiveDuration();
    }
    std::fprintf(stderr, "G1 exported=%d importedError=%s title=%s videoTracks=%d "
        "clipCount=%d expectedClipCount=%d finalPosition=%.9f roundtrip=%d\n",
        int(dir.isValid()), imported.error.toUtf8().constData(), imported.title.toUtf8().constData(),
        int(imported.videoTracks.size()), int(imported.videoTracks[0].size()),
        int(source.size()), position, int(roundtrip));
    gate(1, roundtrip);

    auto document = [](const QString &clips, const QString &resources = QString(), const QString &attributes = QString()) {
        return QStringLiteral("<fcpxml><resources><asset id=\"a\" src=\"offline.mov\"/>%1</resources>"
            "<project name=\"読み込み\"><sequence %2><spine>%3</spine></sequence></project></fcpxml>")
            .arg(resources, attributes, clips);
    };
    const auto rational = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" offset=\"1001/30000s\" start=\"1001/30000s\" duration=\"1001/30000s\"/>"
        "<asset-clip ref=\"a\" offset=\"3003/30000s\" duration=\"2002/30000s\"/>")), dir.path());
    const auto cancellation = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" offset=\"9000000000000001001/30000s\" duration=\"1001/30000s\"/>"),
        QString(), QStringLiteral("tcStart=\"9000000000000000000/30000s\"")), dir.path());
    const auto malformed = fcpx::xml::parseXml(document(QStringLiteral("<asset-clip ref=\"a\" duration=\"1/0s\"/>")), dir.path());
    std::fprintf(stderr, "G2 rationalError=%s rationalClips=%d inPoint=%.12f outPoint=%.12f "
        "secondLead=%.12f cancellationError=%s cancellationClips=%d cancellationLead=%.12f "
        "expected=%.12f malformedError=%s\n", rational.error.toUtf8().constData(),
        int(rational.videoTracks[0].size()),
        rational.videoTracks[0].isEmpty() ? -1.0 : rational.videoTracks[0][0].inPoint,
        rational.videoTracks[0].isEmpty() ? -1.0 : rational.videoTracks[0][0].outPoint,
        rational.videoTracks[0].size() < 2 ? -1.0 : rational.videoTracks[0][1].leadInSec,
        cancellation.error.toUtf8().constData(), int(cancellation.videoTracks[0].size()),
        cancellation.videoTracks[0].isEmpty() ? -1.0 : cancellation.videoTracks[0][0].leadInSec,
        1001.0 / 30000.0, malformed.error.toUtf8().constData());
    gate(2, rational.error.isEmpty() && rational.videoTracks[0].size() == 2
        && near(rational.videoTracks[0][0].inPoint, 1001.0 / 30000.0)
        && near(rational.videoTracks[0][0].outPoint, 2002.0 / 30000.0)
        && near(rational.videoTracks[0][1].leadInSec, 1001.0 / 30000.0)
        && cancellation.error.isEmpty() && cancellation.videoTracks[0].size() == 1
        && near(cancellation.videoTracks[0][0].leadInSec, 1001.0 / 30000.0)
        && !malformed.error.isEmpty());

    const QString two = QStringLiteral("<asset-clip ref=\"a\" lane=\"1\" offset=\"2s\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" lane=\"0\" duration=\"3s\"/>");
    const auto lanes = fcpx::xml::parseXml(document(two), dir.path());
    Timeline laneTimeline;
    bool tracks = lanes.error.isEmpty() && laneTimeline.createSequence(lanes.title, lanes.videoTracks, lanes.audioTracks);
    const auto state = laneTimeline.currentState();
    tracks &= state.videoTracks.size() == 2 && state.videoTracks[0].size() == 1
        && state.videoTracks[1].size() == 1 && near(state.videoTracks[1][0].leadInSec, 2.0);
    const auto capped = fcpx::xml::parseXml(document(two + QStringLiteral("<asset-clip ref=\"a\" lane=\"2\" duration=\"1s\"/>")), dir.path());
    std::fprintf(stderr, "G3 parsedError=%s created=%d stateTracks=%d stateCounts=%d,%d "
        "secondLead=%.9f cappedTracks=%d lane2Warning=%d\n",
        lanes.error.toUtf8().constData(), int(tracks), int(state.videoTracks.size()),
        state.videoTracks.isEmpty() ? -1 : int(state.videoTracks[0].size()),
        state.videoTracks.size() > 1 ? int(state.videoTracks[1].size()) : -1,
        state.videoTracks.size() > 1 && !state.videoTracks[1].isEmpty()
            ? state.videoTracks[1][0].leadInSec : -1.0,
        int(capped.videoTracks.size()), int(capped.warnings.join('\n').contains(QStringLiteral("lane 2"))));
    gate(3, tracks && capped.error.isEmpty() && capped.videoTracks.size() == 2
        && capped.warnings.join('\n').contains(QStringLiteral("lane 2")));

    const auto unsupported = fcpx::xml::parseXml(document(QStringLiteral(
        "<transition duration=\"1s\"/><clip><asset-clip ref=\"a\" duration=\"9s\"/></clip>"
        "<ref-clip ref=\"compound\"/><mc-clip ref=\"multi\"/>"
        "<asset-clip ref=\"a\" duration=\"1s\"><filter-video/><adjust-color/></asset-clip>"),
        QStringLiteral("<media id=\"compound\"><sequence/></media>")), dir.path());
    bool warnings = unsupported.error.isEmpty() && unsupported.videoTracks[0].size() == 1
        && unsupported.unresolvedFiles == 1 && unsupported.resolvedFiles == 0
        && !QFileInfo::exists(unsupported.videoTracks[0][0].filePath);
    for (const QString &tag : {QStringLiteral("transition"), QStringLiteral("clip"), QStringLiteral("ref-clip"),
            QStringLiteral("mc-clip"), QStringLiteral("filter-video"), QStringLiteral("adjust-color"), QStringLiteral("media")})
        warnings &= unsupported.warnings.join('\n').contains(QStringLiteral(": %1 (").arg(tag));
    warnings &= !fcpx::xml::parseXml(QStringLiteral("<fcpxml><project>"), dir.path()).error.isEmpty()
        && !fcpx::xml::parseXml(QStringLiteral("<unrelated/>"), dir.path()).error.isEmpty()
        && !fcpx::xml::readFcpxml(dir.filePath(QStringLiteral("absent.fcpxml"))).error.isEmpty();
    std::fprintf(stderr, "G4 unsupportedError=%s importedClips=%d resolved=%d unresolved=%d "
        "warningCount=%d allChecks=%d\n", unsupported.error.toUtf8().constData(),
        int(unsupported.videoTracks[0].size()), unsupported.resolvedFiles, unsupported.unresolvedFiles,
        int(unsupported.warnings.size()), int(warnings));
    gate(4, warnings);

    Timeline timeline;
    timeline.createSequence(QStringLiteral("既存"), imported.videoTracks, imported.audioTracks);
    timeline.createSequence(QStringLiteral("別シーケンス"));
    timeline.setActiveSequence(timeline.sequenceList().first().id);
    const auto before = timeline.currentState();
    const auto active = timeline.activeSequenceId();
    const auto count = timeline.sequenceList().size();
    const auto serial = timeline.undoManager()->saveSerial();
    const auto index = timeline.undoManager()->currentIndex();
    bool undo = timeline.createSequence(imported.title, imported.videoTracks, imported.audioTracks);
    const auto created = timeline.activeSequenceId();
    undo &= timeline.sequenceList().size() == count + 1 && created != active
        && timeline.undoManager()->saveSerial() == serial + 1;
    timeline.undo();
    undo &= timeline.activeSequenceId() == active && timeline.sequenceList().size() == count
        && timeline.undoManager()->currentIndex() == index
        && timeline.currentState().clipParentEntries == before.clipParentEntries;
    timeline.redo();
    undo &= timeline.activeSequenceId() == created && timeline.sequenceList().size() == count + 1
        && timeline.currentState().videoTracks[0].size() == source.size();
    std::fprintf(stderr, "G5 initialSequences=%d createdSequences=%d saveSerialBefore=%llu "
        "saveSerialAfter=%llu initialIndex=%d currentIndex=%d undoChecks=%d\n",
        int(count), int(timeline.sequenceList().size()),
        static_cast<unsigned long long>(serial),
        static_cast<unsigned long long>(timeline.undoManager()->saveSerial()),
        int(index), int(timeline.undoManager()->currentIndex()), int(undo));
    gate(5, undo);

    const auto connected = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"anchor\" offset=\"10s\" start=\"3s\" duration=\"20s\">"
        "<asset-clip ref=\"a\" name=\"child\" lane=\"1\" offset=\"5s\" start=\"1s\" duration=\"1s\">"
        "<asset-clip ref=\"a\" name=\"grandchild\" lane=\"1\" offset=\"2s\" duration=\"1s\"/>"
        "</asset-clip></asset-clip>")), dir.path());
    const int connectedVideo = connected.videoTracks.size();
    const int connectedChildren = connectedVideo > 1 ? connected.videoTracks[1].size() : -1;
    const double childLead = connectedChildren > 0 ? connected.videoTracks[1][0].leadInSec : -1.0;
    const double grandchildLead = connectedChildren > 1 ? connected.videoTracks[1][1].leadInSec : -1.0;
    const double expectedChildLead = 10.0 + 5.0 - 3.0;
    std::fprintf(stderr, "G6 videoTracks=%d children=%d parentOffset=10 childOffset=5 parentStart=3 "
        "childLead=%.9f expected=%.9f grandchildLead=%.9f expected=0 error=%s\n",
        connectedVideo, connectedChildren, childLead, expectedChildLead, grandchildLead,
        connected.error.toUtf8().constData());
    gate(6, connected.error.isEmpty() && connectedVideo == 2 && connectedChildren == 2
        && std::abs(childLead - expectedChildLead) <= 1e-6 && std::abs(grandchildLead) <= 1e-6);

    const auto ordered = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"minus-two\" lane=\"-2\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"minus-one\" lane=\"-1\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"main-one\" lane=\"0\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"plus-one\" lane=\"1\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"main-two\" lane=\"0\" offset=\"2s\" duration=\"1s\"/>")), dir.path());
    const int orderedMainCount = ordered.videoTracks.isEmpty() ? -1 : ordered.videoTracks[0].size();
    const QString orderedSecond = ordered.videoTracks.size() > 1 && !ordered.videoTracks[1].isEmpty()
        ? ordered.videoTracks[1][0].displayName : QString();
    const QString orderedWarnings = ordered.warnings.join('\n');
    std::fprintf(stderr, "G7 videoTracks=%d mainCount=%d second=%s warningMinusTwo=%d "
        "warningMinusOne=%d error=%s\n", int(ordered.videoTracks.size()), orderedMainCount,
        orderedSecond.toUtf8().constData(), int(orderedWarnings.contains(QStringLiteral("lane -2"))),
        int(orderedWarnings.contains(QStringLiteral("lane -1"))), ordered.error.toUtf8().constData());
    gate(7, ordered.error.isEmpty() && ordered.videoTracks.size() == 2 && orderedMainCount == 2
        && ordered.videoTracks[0][0].displayName == QStringLiteral("main-one")
        && ordered.videoTracks[0][1].displayName == QStringLiteral("main-two")
        && orderedSecond == QStringLiteral("plus-one")
        && orderedWarnings.contains(QStringLiteral("lane -2"))
        && orderedWarnings.contains(QStringLiteral("lane -1")));

    const auto audio = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"video\" duration=\"1s\"/>"
        "<asset-clip ref=\"audio\" name=\"sound\" duration=\"1s\"/>"
        "<asset-clip ref=\"implicit-audio\" name=\"sound2\" offset=\"2s\" duration=\"1s\"/>"
        "<asset-clip ref=\"audio\" name=\"sound-lane1\" lane=\"1\" duration=\"1s\"/>"
        "<asset-clip ref=\"audio\" name=\"sound-lane2\" lane=\"2\" duration=\"1s\"/>"),
        QStringLiteral("<asset id=\"audio\" src=\"sound.wav\" hasVideo=\"0\" hasAudio=\"1\"/>"
            "<asset id=\"implicit-audio\" src=\"sound2.wav\" hasAudio=\"1\"/>")), dir.path());
    const int audioVideoCount = audio.videoTracks.isEmpty() ? -1 : audio.videoTracks[0].size();
    const int audioCount = audio.audioTracks.isEmpty() ? -1 : audio.audioTracks[0].size();
    const QString audioSecondLane = audio.audioTracks.size() > 1 && !audio.audioTracks[1].isEmpty()
        ? audio.audioTracks[1][0].displayName : QString();
    const bool audioLane2Warning = audio.warnings.join('\n').contains(QStringLiteral("lane 2"));
    std::fprintf(stderr, "G8 videoTracks=%d videoClips=%d audioTracks=%d audioClips=%d "
        "audioNames=%s,%s secondLane=%s lane2Warning=%d error=%s\n", int(audio.videoTracks.size()), audioVideoCount,
        int(audio.audioTracks.size()), audioCount,
        audioCount > 0 ? audio.audioTracks[0][0].displayName.toUtf8().constData() : "",
        audioCount > 1 ? audio.audioTracks[0][1].displayName.toUtf8().constData() : "",
        audioSecondLane.toUtf8().constData(), int(audioLane2Warning),
        audio.error.toUtf8().constData());
    gate(8, audio.error.isEmpty() && audio.videoTracks.size() == 1 && audioVideoCount == 1
        && audio.videoTracks[0][0].displayName == QStringLiteral("video")
        && audio.audioTracks.size() == 2 && audioCount == 2
        && audio.audioTracks[0][0].displayName == QStringLiteral("sound")
        && audio.audioTracks[0][1].displayName == QStringLiteral("sound2")
        && audioSecondLane == QStringLiteral("sound-lane1") && audioLane2Warning);

    // A lane's first valid clip cannot overlap; zero duration exercises the same empty-lane outcome.
    const auto emptyLane = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"main\" lane=\"0\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"discarded\" lane=\"1\" duration=\"0s\"/>"
        "<asset-clip ref=\"a\" name=\"retained\" lane=\"2\" duration=\"1s\"/>")), dir.path());
    const QString emptySecond = emptyLane.videoTracks.size() > 1 && !emptyLane.videoTracks[1].isEmpty()
        ? emptyLane.videoTracks[1][0].displayName : QString();
    const QString emptyWarnings = emptyLane.warnings.join('\n');
    std::fprintf(stderr, "G9 videoTracks=%d second=%s skippedWarning=%d lane2Capped=%d error=%s\n",
        int(emptyLane.videoTracks.size()), emptySecond.toUtf8().constData(),
        int(emptyWarnings.contains(QStringLiteral("discarded"))),
        int(emptyWarnings.contains(QStringLiteral("上限により lane 2"))), emptyLane.error.toUtf8().constData());
    gate(9, emptyLane.error.isEmpty() && emptyLane.videoTracks.size() == 2
        && emptySecond == QStringLiteral("retained")
        && emptyWarnings.contains(QStringLiteral("discarded"))
        && !emptyWarnings.contains(QStringLiteral("上限により lane 2")));

    const auto partlyInvalid = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"missing-ref\" name=\"missing\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"bad-range\" offset=\"2s\" duration=\"0s\"/>"
        "<asset-clip ref=\"a\" name=\"bad-origin\" offset=\"0s\" duration=\"1s\"/>"
        "<asset-clip ref=\"late\" name=\"bad-start\" offset=\"3s\" start=\"1s\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"malformed\" offset=\"3s\" duration=\"1/0s\"/>"
        "<asset-clip ref=\"a\" name=\"valid\" offset=\"4s\" duration=\"1s\"/>"),
        QStringLiteral("<asset id=\"late\" src=\"late.mov\" start=\"2s\"/>"),
        QStringLiteral("tcStart=\"1s\"")), dir.path());
    const auto allInvalid = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"missing-ref\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" duration=\"0s\"/>")), dir.path());
    const QString invalidWarnings = partlyInvalid.warnings.join('\n');
    const int retainedCount = partlyInvalid.videoTracks.isEmpty() ? -1 : partlyInvalid.videoTracks[0].size();
    std::fprintf(stderr, "G10 retained=%d missingRefWarning=%d badRangeWarning=%d "
        "badOriginWarning=%d badStartWarning=%d malformedWarning=%d "
        "partialError=%s allInvalidError=%s\n", retainedCount,
        int(invalidWarnings.contains(QStringLiteral("missing-ref"))),
        int(invalidWarnings.contains(QStringLiteral("bad-range"))),
        int(invalidWarnings.contains(QStringLiteral("bad-origin"))),
        int(invalidWarnings.contains(QStringLiteral("bad-start"))),
        int(invalidWarnings.contains(QStringLiteral("malformed"))),
        partlyInvalid.error.toUtf8().constData(), allInvalid.error.toUtf8().constData());
    gate(10, partlyInvalid.error.isEmpty() && retainedCount == 1
        && partlyInvalid.videoTracks[0][0].displayName == QStringLiteral("valid")
        && invalidWarnings.contains(QStringLiteral("missing-ref"))
        && invalidWarnings.contains(QStringLiteral("bad-range"))
        && invalidWarnings.contains(QStringLiteral("bad-origin"))
        && invalidWarnings.contains(QStringLiteral("bad-start"))
        && invalidWarnings.contains(QStringLiteral("malformed"))
        && !allInvalid.error.isEmpty());

    return tally.finish();
}
