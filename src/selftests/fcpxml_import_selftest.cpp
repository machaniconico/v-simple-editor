#include "../FcpxmlImport.h"
#include "../FcpxmlExporter.h"
#include "../UndoManager.h"
#include "SelftestGate.h"
#include <QFile>
#include <QFileInfo>
#include <QPair>
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
    const QString oldLimit = QStringLiteral("2 トラックの上限により lane 2 を読み飛ばしました。");
    std::fprintf(stderr, "G3 parsedError=%s created=%d stateTracks=%d stateCounts=%d,%d "
        "secondLead=%.9f importedTracks=%d lane2Name=%s oldLimitWarning=%d\n",
        lanes.error.toUtf8().constData(), int(tracks), int(state.videoTracks.size()),
        state.videoTracks.isEmpty() ? -1 : int(state.videoTracks[0].size()),
        state.videoTracks.size() > 1 ? int(state.videoTracks[1].size()) : -1,
        state.videoTracks.size() > 1 && !state.videoTracks[1].isEmpty()
            ? state.videoTracks[1][0].leadInSec : -1.0,
        int(capped.videoTracks.size()),
        capped.videoTracks.size() > 2 && !capped.videoTracks[2].isEmpty()
            ? capped.videoTracks[2][0].displayName.toUtf8().constData() : "",
        int(capped.warnings.contains(oldLimit)));
    gate(3, tracks && capped.error.isEmpty() && capped.videoTracks.size() == 3
        && capped.videoTracks[2].size() == 1 && !capped.warnings.contains(oldLimit));

    const auto unsupported = fcpx::xml::parseXml(document(QStringLiteral(
        "<transition duration=\"1s\"/><clip><asset-clip ref=\"a\" duration=\"9s\"/></clip>"
        "<ref-clip ref=\"compound\"/><mc-clip ref=\"multi\"/>"
        "<asset-clip ref=\"a\" offset=\"9s\" duration=\"1s\"><filter-video/><adjust-color/></asset-clip>"),
        QStringLiteral("<media id=\"compound\"><sequence/></media>")), dir.path());
    bool warnings = unsupported.error.isEmpty() && unsupported.videoTracks[0].size() == 2
        && unsupported.unresolvedFiles == 1 && unsupported.resolvedFiles == 0
        && !QFileInfo::exists(unsupported.videoTracks[0][0].filePath);
    for (const QString &tag : {QStringLiteral("transition"), QStringLiteral("ref-clip"),
            QStringLiteral("mc-clip"), QStringLiteral("filter-video"), QStringLiteral("adjust-color"), QStringLiteral("media")})
        warnings &= unsupported.warnings.join('\n').contains(QStringLiteral(": %1 (").arg(tag));
    warnings &= !unsupported.warnings.join('\n').contains(QStringLiteral(": clip ("));
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
    const QString orderedThird = ordered.videoTracks.size() > 2 && !ordered.videoTracks[2].isEmpty()
        ? ordered.videoTracks[2][0].displayName : QString();
    const QString orderedFourth = ordered.videoTracks.size() > 3 && !ordered.videoTracks[3].isEmpty()
        ? ordered.videoTracks[3][0].displayName : QString();
    const QString negativeOneWarning = QStringLiteral(
        "lane -1 の映像は FCP では主ストーリーラインの下に合成されます。V3 に置きました。");
    const QString negativeTwoWarning = QStringLiteral(
        "lane -2 の映像は FCP では主ストーリーラインの下に合成されます。V4 に置きました。");
    const int negativeOneCount = ordered.warnings.count(negativeOneWarning);
    const int negativeTwoCount = ordered.warnings.count(negativeTwoWarning);
    std::fprintf(stderr, "G7 videoTracks=%d mainCount=%d second=%s third=%s fourth=%s "
        "negativeOneWarningCount=%d negativeTwoWarningCount=%d error=%s\n",
        int(ordered.videoTracks.size()), orderedMainCount, orderedSecond.toUtf8().constData(),
        orderedThird.toUtf8().constData(), orderedFourth.toUtf8().constData(),
        negativeOneCount, negativeTwoCount, ordered.error.toUtf8().constData());
    gate(7, ordered.error.isEmpty() && ordered.videoTracks.size() == 4 && orderedMainCount == 2
        && ordered.videoTracks[0][0].displayName == QStringLiteral("main-one")
        && ordered.videoTracks[0][1].displayName == QStringLiteral("main-two")
        && orderedSecond == QStringLiteral("plus-one")
        && ordered.videoTracks[2].size() == 1
        && ordered.videoTracks[2][0].displayName == QStringLiteral("minus-one")
        && ordered.videoTracks[3].size() == 1
        && ordered.videoTracks[3][0].displayName == QStringLiteral("minus-two")
        && negativeOneCount == 1 && negativeTwoCount == 1);

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
        "audioNames=%s,%s soundLead=%.9f sound2Lead=%.9f secondLane=%s lane2Warning=%d error=%s\n", int(audio.videoTracks.size()), audioVideoCount,
        int(audio.audioTracks.size()), audioCount,
        audioCount > 0 ? audio.audioTracks[0][0].displayName.toUtf8().constData() : "",
        audioCount > 1 ? audio.audioTracks[0][1].displayName.toUtf8().constData() : "",
        audioCount > 0 ? audio.audioTracks[0][0].leadInSec : -1.0,
        audioCount > 1 ? audio.audioTracks[0][1].leadInSec : -1.0,
        audioSecondLane.toUtf8().constData(), int(audioLane2Warning),
        audio.error.toUtf8().constData());
    gate(8, audio.error.isEmpty() && audio.videoTracks.size() == 1 && audioVideoCount == 1
        && audio.videoTracks[0][0].displayName == QStringLiteral("video")
        && audio.audioTracks.size() == 3 && audioCount == 2
        && audio.audioTracks[0][0].displayName == QStringLiteral("sound")
        && audio.audioTracks[0][1].displayName == QStringLiteral("sound2")
        && near(audio.audioTracks[0][0].leadInSec, 0.0)
        && near(audio.audioTracks[0][1].leadInSec, 1.0)
        && audioSecondLane == QStringLiteral("sound-lane1")
        && audio.audioTracks[2].size() == 1
        && audio.audioTracks[2][0].displayName == QStringLiteral("sound-lane2")
        && !audioLane2Warning);

    // A zero-duration clip does not occupy a track; a later valid lane 1 clip
    // still leaves lane 2 available for V3.
    const auto emptyLane = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"main\" lane=\"0\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"discarded\" lane=\"1\" duration=\"0s\"/>"
        "<asset-clip ref=\"a\" name=\"fill\" lane=\"1\" offset=\"2s\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"retained\" lane=\"2\" duration=\"1s\"/>")), dir.path());
    const QString emptySecond = emptyLane.videoTracks.size() > 1 && !emptyLane.videoTracks[1].isEmpty()
        ? emptyLane.videoTracks[1][0].displayName : QString();
    const QString emptyThird = emptyLane.videoTracks.size() > 2 && !emptyLane.videoTracks[2].isEmpty()
        ? emptyLane.videoTracks[2][0].displayName : QString();
    const QString emptyWarnings = emptyLane.warnings.join('\n');
    std::fprintf(stderr, "G9 videoTracks=%d second=%s third=%s skippedWarning=%d lane2Capped=%d error=%s\n",
        int(emptyLane.videoTracks.size()), emptySecond.toUtf8().constData(), emptyThird.toUtf8().constData(),
        int(emptyWarnings.contains(QStringLiteral("discarded"))),
        int(emptyWarnings.contains(QStringLiteral("上限により lane 2"))), emptyLane.error.toUtf8().constData());
    gate(9, emptyLane.error.isEmpty() && emptyLane.videoTracks.size() == 3
        && emptySecond == QStringLiteral("fill") && emptyThird == QStringLiteral("retained")
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

    // G11 detects a gap being skipped with all of its connected children.
    const auto gapChild = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"main\" duration=\"1s\"/>"
        "<gap name=\"anchor-gap\" offset=\"10s\" duration=\"5s\">"
        "<asset-clip ref=\"a\" name=\"gap-child\" lane=\"1\" offset=\"2s\" duration=\"1s\"/>"
        "</gap>")), dir.path());
    const int gapClipCount = gapChild.videoTracks.size() > 1 ? gapChild.videoTracks[1].size() : -1;
    const double gapTimelineSec = gapClipCount > 0 ? gapChild.videoTracks[1][0].leadInSec : -1.0;
    std::fprintf(stderr, "G11 clipCount=%d timelineSec=%.9f expectedTimelineSec=12 "
        "error=%s warnings=%s\n", gapClipCount, gapTimelineSec,
        gapChild.error.toUtf8().constData(), gapChild.warnings.join('|').toUtf8().constData());
    gate(11, gapChild.error.isEmpty() && gapChild.videoTracks.size() == 2
        && gapClipCount == 1 && near(gapTimelineSec, 12.0)
        && gapChild.videoTracks[1][0].displayName == QStringLiteral("gap-child"));

    // G12 catches rejection of a negative parent-relative offset and incorrect
    // source trimming when the connected audio would begin before timeline 0.
    const QString audioAsset = QStringLiteral(
        "<asset id=\"j-audio\" src=\"j.wav\" hasVideo=\"0\" hasAudio=\"1\"/>");
    const auto jCut = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"parent\" offset=\"10s\" start=\"5s\" duration=\"5s\">"
        "<asset-clip ref=\"j-audio\" name=\"j-sound\" lane=\"1\" offset=\"2s\" start=\"1s\" duration=\"2s\"/>"
        "</asset-clip>"), audioAsset), dir.path());
    const auto trimmedJCut = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"early-parent\" offset=\"1s\" start=\"5s\" duration=\"5s\">"
        "<asset-clip ref=\"j-audio\" name=\"trimmed-sound\" lane=\"1\" offset=\"2s\" start=\"1s\" duration=\"4s\"/>"
        "</asset-clip>"), audioAsset), dir.path());
    const bool jPresent = !jCut.audioTracks.isEmpty() && jCut.audioTracks[0].size() == 1;
    const bool trimmedPresent = !trimmedJCut.audioTracks.isEmpty() && trimmedJCut.audioTracks[0].size() == 1;
    const double jTimelineSec = jPresent ? jCut.audioTracks[0][0].leadInSec : -1.0;
    const double trimmedTimelineSec = trimmedPresent ? trimmedJCut.audioTracks[0][0].leadInSec : -1.0;
    const double trimmedSourceSec = trimmedPresent ? trimmedJCut.audioTracks[0][0].inPoint : -1.0;
    const double trimmedDurationSec = trimmedPresent
        ? trimmedJCut.audioTracks[0][0].outPoint - trimmedSourceSec : -1.0;
    std::fprintf(stderr, "G12 jTimelineSec=%.9f jLocalSec=2 expectedTimelineSec=7 "
        "trimmedTimelineSec=%.9f trimmedLocalSec=2 "
        "trimmedSourceSec=%.9f expectedSourceSec=3 trimmedDurationSec=%.9f "
        "warning=%s jError=%s trimmedError=%s\n", jTimelineSec, trimmedTimelineSec,
        trimmedSourceSec, trimmedDurationSec, trimmedJCut.warnings.join('|').toUtf8().constData(),
        jCut.error.toUtf8().constData(), trimmedJCut.error.toUtf8().constData());
    gate(12, jCut.error.isEmpty() && trimmedJCut.error.isEmpty() && jPresent && trimmedPresent
        && near(jTimelineSec, 7.0) && near(trimmedTimelineSec, 0.0)
        && near(trimmedSourceSec, 3.0) && near(trimmedDurationSec, 2.0)
        && trimmedJCut.warnings.join('|').contains(QStringLiteral("切り詰めました")));

    // G13 detects descendants silently dropped when an invalid parent is skipped.
    const QString descendants = QStringLiteral(
        "<asset-clip ref=\"a\" name=\"child-one\" lane=\"1\" duration=\"1s\">"
        "<asset-clip ref=\"a\" name=\"child-two\" lane=\"1\" duration=\"1s\"/>"
        "</asset-clip>");
    const auto malformedParent = fcpx::xml::parseXml(document(
        QStringLiteral("<asset-clip ref=\"a\" name=\"bad-parent\" duration=\"1/0s\">")
        + descendants + QStringLiteral("</asset-clip>"
            "<asset-clip ref=\"a\" name=\"valid\" offset=\"4s\" duration=\"1s\"/>")), dir.path());
    const auto invalidRangeParent = fcpx::xml::parseXml(document(
        QStringLiteral("<asset-clip ref=\"a\" name=\"zero-parent\" duration=\"0s\">")
        + descendants + QStringLiteral("</asset-clip>"
            "<asset-clip ref=\"a\" name=\"valid\" offset=\"4s\" duration=\"1s\"/>")), dir.path());
    auto descendantWarnings = [](const fcpx::xml::ImportResult &r) {
        int count = 0;
        for (const auto &warning : r.warnings)
            if (warning.contains(QStringLiteral("親クリップが不正"))) ++count;
        return count;
    };
    const int malformedChildWarnings = descendantWarnings(malformedParent);
    const int invalidRangeChildWarnings = descendantWarnings(invalidRangeParent);
    const QString malformedText = malformedParent.warnings.join('|');
    const QString invalidRangeText = invalidRangeParent.warnings.join('|');
    std::fprintf(stderr, "G13 malformedDescendantWarnings=%d text=%s "
        "rangeDescendantWarnings=%d rangeText=%s\n", malformedChildWarnings,
        malformedText.toUtf8().constData(), invalidRangeChildWarnings,
        invalidRangeText.toUtf8().constData());
    gate(13, malformedParent.error.isEmpty() && invalidRangeParent.error.isEmpty()
        && malformedChildWarnings == 2 && invalidRangeChildWarnings == 2
        && malformedText.contains(QStringLiteral("child-one / a"))
        && malformedText.contains(QStringLiteral("child-two / a"))
        && invalidRangeText.contains(QStringLiteral("child-one / a"))
        && invalidRangeText.contains(QStringLiteral("child-two / a")));

    // G14 distinguishes an empty spine from a spine whose clips were rejected.
    const auto noAssets = fcpx::xml::parseXml(document(QStringLiteral(
        "<gap name=\"only-gap\" offset=\"2s\" duration=\"1s\"/>")), dir.path());
    Timeline emptyTimeline;
    const bool createdEmpty = noAssets.error.isEmpty()
        && emptyTimeline.createSequence(noAssets.title, noAssets.videoTracks, noAssets.audioTracks);
    const auto emptyState = emptyTimeline.currentState();
    std::fprintf(stderr, "G14 error=%s warnings=%d warningText=%s created=%d "
        "videoTracks=%d audioTracks=%d\n", noAssets.error.toUtf8().constData(),
        int(noAssets.warnings.size()), noAssets.warnings.join('|').toUtf8().constData(),
        int(createdEmpty), int(emptyState.videoTracks.size()), int(emptyState.audioTracks.size()));
    gate(14, noAssets.error.isEmpty() && noAssets.warnings.size() == 1
        && noAssets.warnings[0].contains(QStringLiteral("読み込めるクリップがありません"))
        && noAssets.videoTracks.isEmpty() && noAssets.audioTracks.isEmpty()
        && createdEmpty && emptyState.videoTracks.size() == 1 && emptyState.videoTracks[0].isEmpty()
        && emptyState.audioTracks.size() == 1 && emptyState.audioTracks[0].isEmpty());

    // G15 detects an imported source length collapsing to the selected out point.
    const auto assetWithoutDuration = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" start=\"0s\" duration=\"4s\"/>"),
        QString(), QString()), dir.path());
    const auto tenSecondAsset = fcpx::xml::parseXml(QStringLiteral(
        "<fcpxml><resources><asset id=\"a\" src=\"offline.mov\" duration=\"10s\"/>"
        "</resources><project><sequence><spine>"
        "<asset-clip ref=\"a\" start=\"0s\" duration=\"4s\"/>"
        "</spine></sequence></project></fcpxml>"), dir.path());
    const bool hasTenSecondClip = tenSecondAsset.error.isEmpty()
        && tenSecondAsset.videoTracks.size() == 1 && tenSecondAsset.videoTracks[0].size() == 1;
    const double sourceDurationSec = hasTenSecondClip ? tenSecondAsset.videoTracks[0][0].duration : -1.0;
    const double sourceOutSec = hasTenSecondClip ? tenSecondAsset.videoTracks[0][0].outPoint : -1.0;
    const double localDurationSec = hasTenSecondClip ? tenSecondAsset.videoTracks[0][0].effectiveDuration() : -1.0;
    std::fprintf(stderr, "G15 sourceDurationSec=%.9f sourceOutSec=%.9f localDurationSec=%.9f error=%s\n",
        sourceDurationSec, sourceOutSec, localDurationSec, tenSecondAsset.error.toUtf8().constData());
    gate(15, hasTenSecondClip && near(sourceDurationSec, 10.0)
        && near(sourceOutSec, 4.0) && near(localDurationSec, 4.0));

    // G16 contrasts the same selection without asset duration: legacy fallback.
    const bool hasFallbackClip = assetWithoutDuration.error.isEmpty()
        && assetWithoutDuration.videoTracks.size() == 1 && assetWithoutDuration.videoTracks[0].size() == 1;
    const double fallbackDurationSec = hasFallbackClip ? assetWithoutDuration.videoTracks[0][0].duration : -1.0;
    const double fallbackOutSec = hasFallbackClip ? assetWithoutDuration.videoTracks[0][0].outPoint : -1.0;
    std::fprintf(stderr, "G16 sourceDurationSec=%.9f sourceOutSec=%.9f error=%s\n",
        fallbackDurationSec, fallbackOutSec, assetWithoutDuration.error.toUtf8().constData());
    gate(16, hasFallbackClip && near(fallbackDurationSec, fallbackOutSec)
        && near(fallbackOutSec, 4.0));

    // G17 detects a declared asset length shorter than its selected source out.
    const auto shortAsset = fcpx::xml::parseXml(QStringLiteral(
        "<fcpxml><resources><asset id=\"a\" src=\"offline.mov\" duration=\"2s\"/>"
        "</resources><project><sequence><spine>"
        "<asset-clip ref=\"a\" start=\"1s\" duration=\"3s\"/>"
        "</spine></sequence></project></fcpxml>"), dir.path());
    const bool hasShortAssetClip = shortAsset.error.isEmpty()
        && shortAsset.videoTracks.size() == 1 && shortAsset.videoTracks[0].size() == 1;
    const double shortSourceDurationSec = hasShortAssetClip ? shortAsset.videoTracks[0][0].duration : -1.0;
    const double shortSourceOutSec = hasShortAssetClip ? shortAsset.videoTracks[0][0].outPoint : -1.0;
    const double shortLocalSec = hasShortAssetClip ? shortAsset.videoTracks[0][0].effectiveDuration() : -1.0;
    std::fprintf(stderr, "G17 declaredSourceSec=2 sourceDurationSec=%.9f "
        "sourceOutSec=%.9f localSec=%.9f timelineSec=0 error=%s\n",
        shortSourceDurationSec, shortSourceOutSec, shortLocalSec,
        shortAsset.error.toUtf8().constData());
    gate(17, hasShortAssetClip && near(shortSourceDurationSec, 4.0)
        && near(shortSourceOutSec, 4.0) && near(shortLocalSec, 3.0));

    auto namedClip = [](const QVector<QVector<ClipInfo>> &tracks, const QString &name) -> const ClipInfo * {
        for (const auto &track : tracks)
            for (const auto &clip : track)
                if (clip.displayName == name) return &clip;
        return nullptr;
    };
    const auto manyLanes = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"main\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"one\" lane=\"1\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"two\" lane=\"2\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"three\" lane=\"3\" duration=\"1s\"/>")), dir.path());
    const auto overlapping = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"main\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"first\" lane=\"1\" duration=\"2s\"/>"
        "<asset-clip ref=\"a\" name=\"overlap\" lane=\"1\" offset=\"1s\" duration=\"1s\"/>")), dir.path());
    const bool manyPresent = manyLanes.videoTracks.size() == 4;
    std::fprintf(stderr, "G18 tracks=%d V2=%s V3=%s V4=%s oldLimitWarning=%d "
        "overlapTracks=%d overlapV3=%s error=%s|%s\n",
        int(manyLanes.videoTracks.size()),
        manyPresent && !manyLanes.videoTracks[1].isEmpty() ? manyLanes.videoTracks[1][0].displayName.toUtf8().constData() : "",
        manyPresent && !manyLanes.videoTracks[2].isEmpty() ? manyLanes.videoTracks[2][0].displayName.toUtf8().constData() : "",
        manyPresent && !manyLanes.videoTracks[3].isEmpty() ? manyLanes.videoTracks[3][0].displayName.toUtf8().constData() : "",
        int(manyLanes.warnings.join('|').contains(QStringLiteral("2 トラックの上限により"))),
        int(overlapping.videoTracks.size()),
        overlapping.videoTracks.size() > 2 && !overlapping.videoTracks[2].isEmpty()
            ? overlapping.videoTracks[2][0].displayName.toUtf8().constData() : "",
        manyLanes.error.toUtf8().constData(), overlapping.error.toUtf8().constData());
    gate(18, manyLanes.error.isEmpty() && manyPresent
        && manyLanes.videoTracks[1].size() == 1 && manyLanes.videoTracks[1][0].displayName == QStringLiteral("one")
        && manyLanes.videoTracks[2].size() == 1 && manyLanes.videoTracks[2][0].displayName == QStringLiteral("two")
        && manyLanes.videoTracks[3].size() == 1 && manyLanes.videoTracks[3][0].displayName == QStringLiteral("three")
        && !manyLanes.warnings.join('|').contains(QStringLiteral("2 トラックの上限により"))
        && overlapping.error.isEmpty() && overlapping.videoTracks.size() == 3
        && overlapping.videoTracks[1].size() == 1
        && overlapping.videoTracks[1][0].displayName == QStringLiteral("first")
        && overlapping.videoTracks[2].size() == 1
        && overlapping.videoTracks[2][0].displayName == QStringLiteral("overlap"));

    const QString anchorXml = QStringLiteral(
        "<asset-clip ref=\"a\" name=\"A\" duration=\"3s\"/>"
        "<asset-clip ref=\"a\" name=\"B\" offset=\"10s\" start=\"5s\" duration=\"5s\">"
        "<asset-clip ref=\"a\" name=\"child\" lane=\"1\" offset=\"7s\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"j-cut\" lane=\"2\" offset=\"4s\" duration=\"1s\"/>"
        "</asset-clip>");
    const auto anchored = fcpx::xml::parseXml(document(anchorXml), dir.path());
    const ClipInfo *b = namedClip(anchored.videoTracks, QStringLiteral("B"));
    const ClipInfo *anchoredChild = namedClip(anchored.videoTracks, QStringLiteral("child"));
    const ClipInfo *jChild = namedClip(anchored.videoTracks, QStringLiteral("j-cut"));
    Timeline anchoredTimeline;
    const bool anchoredCreated = anchored.error.isEmpty()
        && anchoredTimeline.createSequence(anchored.title, anchored.videoTracks, anchored.audioTracks);
    const auto anchoredState = anchoredTimeline.currentState();
    const ClipInfo *stateB = namedClip(anchoredState.videoTracks, QStringLiteral("B"));
    const ClipInfo *stateChild = namedClip(anchoredState.videoTracks, QStringLiteral("child"));
    const auto preciseAnchor = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"large-parent\" offset=\"9000000000000001001/30000s\" duration=\"1s\">"
        "<asset-clip ref=\"a\" name=\"large-child\" lane=\"1\" offset=\"1001/30000s\" duration=\"1s\"/>"
        "</asset-clip>"), QString(), QStringLiteral(
        "tcStart=\"9000000000000000000/30000s\"")), dir.path());
    const ClipInfo *largeParent = namedClip(preciseAnchor.videoTracks, QStringLiteral("large-parent"));
    const ClipInfo *largeChild = namedClip(preciseAnchor.videoTracks, QStringLiteral("large-child"));
    std::fprintf(stderr, "G19 Buid=%s childAnchor=%s childOffset=%.9f jAnchor=%s jOffset=%.9f "
        "created=%d stateUidMatch=%d stateAnchorMatch=%d largeOffset=%.12f error=%s|%s\n",
        b ? b->uid.toUtf8().constData() : "", anchoredChild ? anchoredChild->anchorUid.toUtf8().constData() : "",
        anchoredChild ? anchoredChild->anchorOffsetSec : -99.0,
        jChild ? jChild->anchorUid.toUtf8().constData() : "", jChild ? jChild->anchorOffsetSec : -99.0,
        int(anchoredCreated), int(b && stateB && stateB->uid == b->uid),
        int(b && stateChild && stateChild->anchorUid == b->uid),
        largeChild ? largeChild->anchorOffsetSec : -99.0,
        anchored.error.toUtf8().constData(), preciseAnchor.error.toUtf8().constData());
    gate(19, anchored.error.isEmpty() && b && anchoredChild && jChild && !b->uid.isEmpty()
        && anchoredChild->anchorUid == b->uid && near(anchoredChild->anchorOffsetSec, 2.0)
        && jChild->anchorUid == b->uid && near(jChild->anchorOffsetSec, -1.0)
        && anchoredCreated && stateB && stateB->uid == b->uid
        && stateChild && stateChild->anchorUid == b->uid
        && preciseAnchor.error.isEmpty() && largeParent && largeChild
        && largeChild->anchorUid == largeParent->uid
        && near(largeChild->anchorOffsetSec, 1001.0 / 30000.0));

    const auto synchronized = fcpx::xml::parseXml(document(QStringLiteral(
        "<sync-clip name=\"synchronized\" offset=\"4s\" start=\"1s\" duration=\"3s\">"
        "<audio ref=\"sync-audio\" name=\"sound\" lane=\"1\"/>"
        "<video ref=\"a\" name=\"picture\"/>"
        "</sync-clip>"), QStringLiteral(
        "<asset id=\"sync-audio\" src=\"sound.wav\" hasVideo=\"0\" hasAudio=\"1\"/>")), dir.path());
    const ClipInfo *syncPicture = namedClip(synchronized.videoTracks, QStringLiteral("picture"));
    const ClipInfo *syncSound = namedClip(synchronized.audioTracks, QStringLiteral("sound"));
    std::fprintf(stderr, "G20 videoTracks=%d audioTracks=%d pictureUid=%s soundAnchor=%s soundOffset=%.9f error=%s\n",
        int(synchronized.videoTracks.size()), int(synchronized.audioTracks.size()),
        syncPicture ? syncPicture->uid.toUtf8().constData() : "",
        syncSound ? syncSound->anchorUid.toUtf8().constData() : "",
        syncSound ? syncSound->anchorOffsetSec : -99.0, synchronized.error.toUtf8().constData());
    const auto timecodeContainer = fcpx::xml::parseXml(document(QStringLiteral(
        "<clip name=\"tc\" offset=\"10s\" start=\"3600s\" duration=\"2s\">"
        "<video ref=\"a\" name=\"tc-video\" offset=\"3600s\" start=\"3600s\" duration=\"2s\"/>"
        "<audio ref=\"sync-audio\" name=\"tc-audio\" lane=\"-1\" offset=\"3601s\" duration=\"1s\"/>"
        "</clip>"), QStringLiteral(
        "<asset id=\"sync-audio\" src=\"sound.wav\" hasVideo=\"0\" hasAudio=\"1\"/>")), dir.path());
    const ClipInfo *tcVideo = namedClip(timecodeContainer.videoTracks, QStringLiteral("tc-video"));
    const ClipInfo *tcAudio = namedClip(timecodeContainer.audioTracks, QStringLiteral("tc-audio"));
    const auto nestedContainer = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"outer\" offset=\"10s\" start=\"3s\" duration=\"20s\">"
        "<clip offset=\"5s\" start=\"3600s\" duration=\"2s\">"
        "<video ref=\"a\" name=\"nested-video\" lane=\"1\" offset=\"3600s\" start=\"3600s\" duration=\"2s\"/>"
        "</clip></asset-clip>")), dir.path());
    const ClipInfo *outer = namedClip(nestedContainer.videoTracks, QStringLiteral("outer"));
    const ClipInfo *nestedVideo = namedClip(nestedContainer.videoTracks, QStringLiteral("nested-video"));
    std::fprintf(stderr, "G20 tcVideoLead=%.9f tcVideoUid=%s tcAudioAnchor=%s tcAudioOffset=%.9f tcError=%s\n",
        tcVideo ? tcVideo->leadInSec : -1.0, tcVideo ? tcVideo->uid.toUtf8().constData() : "",
        tcAudio ? tcAudio->anchorUid.toUtf8().constData() : "",
        tcAudio ? tcAudio->anchorOffsetSec : -99.0, timecodeContainer.error.toUtf8().constData());
    std::fprintf(stderr, "G20 nestedVideoLead=%.9f nestedAnchor=%s nestedOffset=%.9f nestedError=%s\n",
        nestedVideo ? nestedVideo->leadInSec : -1.0,
        nestedVideo ? nestedVideo->anchorUid.toUtf8().constData() : "",
        nestedVideo ? nestedVideo->anchorOffsetSec : -99.0,
        nestedContainer.error.toUtf8().constData());
    gate(20, synchronized.error.isEmpty() && syncPicture && syncSound && !syncPicture->uid.isEmpty()
        && syncSound->anchorUid == syncPicture->uid && near(syncSound->anchorOffsetSec, 0.0)
        && timecodeContainer.error.isEmpty() && tcVideo && tcAudio
        && timecodeContainer.videoTracks.size() == 1 && !tcVideo->uid.isEmpty()
        && near(tcVideo->leadInSec, 10.0) && tcAudio->anchorUid == tcVideo->uid
        && near(tcAudio->anchorOffsetSec, 1.0)
        && nestedContainer.error.isEmpty() && outer && nestedVideo
        && near(nestedVideo->leadInSec, 12.0)
        && nestedVideo->anchorUid == outer->uid && near(nestedVideo->anchorOffsetSec, 2.0));

    const auto badParentChild = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"bad-parent\" duration=\"1/0s\">"
        "<asset-clip ref=\"a\" name=\"orphan\" lane=\"1\" duration=\"1s\"/>"
        "</asset-clip><asset-clip ref=\"a\" name=\"valid\" duration=\"1s\"/>")), dir.path());
    const auto badParentComponent = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"bad-parent\" duration=\"1/0s\">"
        "<video ref=\"a\" name=\"orphan-video\" lane=\"1\" duration=\"1s\"/>"
        "</asset-clip><asset-clip ref=\"a\" name=\"valid\" duration=\"1s\"/>")), dir.path());
    const auto laterInvalidPrimary = fcpx::xml::parseXml(document(QStringLiteral(
        "<sync-clip duration=\"1s\">"
        "<audio ref=\"audio-root\" name=\"early-audio\" lane=\"1\"/>"
        "<video ref=\"a\" name=\"zero-primary\" duration=\"0s\"/>"
        "</sync-clip><asset-clip ref=\"a\" name=\"valid\" offset=\"3s\" duration=\"1s\"/>"),
        QStringLiteral("<asset id=\"audio-root\" src=\"root.wav\" hasVideo=\"0\" hasAudio=\"1\"/>")), dir.path());
    const auto noLaneChild = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"root\" duration=\"2s\">"
        "<asset-clip ref=\"a\" name=\"no-lane\" duration=\"1s\"/>"
        "</asset-clip>")), dir.path());
    const auto missingAssetParent = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"missing-ref\" name=\"missing-parent\" duration=\"2s\">"
        "<asset-clip ref=\"a\" name=\"missing-parent-child\" lane=\"1\" duration=\"1s\"/>"
        "</asset-clip>")), dir.path());
    QString unanchoredXml = anchorXml;
    unanchoredXml.replace(QStringLiteral(" lane=\"1\""), QString());
    unanchoredXml.replace(QStringLiteral(" lane=\"2\""), QString());
    const auto unanchored = fcpx::xml::parseXml(document(unanchoredXml), dir.path());
    const ClipInfo *unanchoredChild = namedClip(unanchored.videoTracks, QStringLiteral("child"));
    const ClipInfo *unanchoredJCut = namedClip(unanchored.videoTracks, QStringLiteral("j-cut"));
    const auto audioRoot = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"audio-root\" name=\"audio-root\" duration=\"2s\">"
        "<asset-clip ref=\"a\" name=\"detached\" lane=\"1\" duration=\"1s\"/>"
        "</asset-clip>"), QStringLiteral(
        "<asset id=\"audio-root\" src=\"root.wav\" hasVideo=\"0\" hasAudio=\"1\"/>")), dir.path());
    const auto audioRootNoLane = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"audio-root\" name=\"audio-root\" duration=\"2s\">"
        "<asset-clip ref=\"a\" name=\"detached-no-lane\" duration=\"1s\"/>"
        "</asset-clip>"), QStringLiteral(
        "<asset id=\"audio-root\" src=\"root.wav\" hasVideo=\"0\" hasAudio=\"1\"/>")), dir.path());
    const auto overlappingMain = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"P1\" offset=\"0s\" duration=\"5s\"/>"
        "<asset-clip ref=\"a\" name=\"P2\" offset=\"4s\" duration=\"5s\">"
        "<asset-clip ref=\"a\" name=\"off-v1-child\" lane=\"1\" offset=\"0s\" duration=\"1s\"/>"
        "</asset-clip>")), dir.path());
    const auto laneTwoMain = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"V1\" offset=\"0s\" duration=\"5s\"/>"
        "<asset-clip ref=\"a\" name=\"lane-two-parent\" lane=\"2\" offset=\"4s\" duration=\"5s\">"
        "<asset-clip ref=\"a\" name=\"lane-two-child\" lane=\"1\" offset=\"0s\" duration=\"1s\"/>"
        "</asset-clip>")), dir.path());
    auto location = [](const QVector<QVector<ClipInfo>> &tracks, const QString &name) {
        for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
            double timelineSec = 0.0;
            for (const auto &clip : tracks[trackIndex]) {
                timelineSec += clip.leadInSec;
                if (clip.displayName == name) return QPair<int, double>(trackIndex, timelineSec);
                timelineSec += clip.effectiveDuration();
            }
        }
        return QPair<int, double>(-1, -1.0);
    };
    const auto p2Location = location(overlappingMain.videoTracks, QStringLiteral("P2"));
    const auto offV1Location = location(overlappingMain.videoTracks, QStringLiteral("off-v1-child"));
    const auto laneTwoParentLocation = location(laneTwoMain.videoTracks, QStringLiteral("lane-two-parent"));
    const auto laneTwoChildLocation = location(laneTwoMain.videoTracks, QStringLiteral("lane-two-child"));
    const ClipInfo *offV1Child = namedClip(overlappingMain.videoTracks, QStringLiteral("off-v1-child"));
    const ClipInfo *laneTwoChild = namedClip(laneTwoMain.videoTracks, QStringLiteral("lane-two-child"));
    const QString offV1Warning = QStringLiteral(
        "接続先の主ストーリーラインのクリップが V1 に置かれないため接続を解除しました: off-v1-child");
    const QString laneTwoWarning = QStringLiteral(
        "接続先の主ストーリーラインのクリップが V1 に置かれないため接続を解除しました: lane-two-child");
    const ClipInfo *withoutLane = namedClip(noLaneChild.videoTracks, QStringLiteral("no-lane"));
    const ClipInfo *missingParentChild = namedClip(missingAssetParent.videoTracks, QStringLiteral("missing-parent-child"));
    const ClipInfo *detached = namedClip(audioRoot.videoTracks, QStringLiteral("detached"));
    const ClipInfo *detachedNoLane = namedClip(audioRootNoLane.videoTracks, QStringLiteral("detached-no-lane"));
    const QString badParentWarning = QStringLiteral(
        "親クリップが不正なため接続クリップを読み飛ばしました: orphan / a");
    const QString badComponentWarning = QStringLiteral(
        "親クリップが不正なため接続クリップを読み飛ばしました: orphan-video / a");
    const QString earlyAudioWarning = QStringLiteral(
        "親クリップが不正なため接続クリップを読み飛ばしました: early-audio / audio-root");
    const QString audioRootWarning = QStringLiteral(
        "主ストーリーラインが音声クリップのため接続を解除しました: detached");
    const QString audioNoLaneWarning = QStringLiteral(
        "主ストーリーラインが音声クリップのため接続を解除しました: detached-no-lane");
    std::fprintf(stderr, "G21 invalidWarning=%d componentWarning=%d earlyAudioWarning=%d noLaneAnchor=%s "
        "g19ChildAnchor=%s g19JCutAnchor=%s missingParentChildAnchor=%s audioRootWarningCount=%d "
        "detachedAnchor=%s audioNoLaneWarningCount=%d detachedNoLaneAnchor=%s errors=%s|%s|%s|%s|%s|%s|%s|%s\n",
        int(badParentChild.warnings.contains(badParentWarning)),
        int(badParentComponent.warnings.contains(badComponentWarning)),
        int(laterInvalidPrimary.warnings.contains(earlyAudioWarning)),
        withoutLane ? withoutLane->anchorUid.toUtf8().constData() : "",
        unanchoredChild ? unanchoredChild->anchorUid.toUtf8().constData() : "",
        unanchoredJCut ? unanchoredJCut->anchorUid.toUtf8().constData() : "",
        missingParentChild ? missingParentChild->anchorUid.toUtf8().constData() : "",
        int(audioRoot.warnings.count(audioRootWarning)),
        detached ? detached->anchorUid.toUtf8().constData() : "",
        int(audioRootNoLane.warnings.count(audioNoLaneWarning)),
        detachedNoLane ? detachedNoLane->anchorUid.toUtf8().constData() : "",
        badParentChild.error.toUtf8().constData(), badParentComponent.error.toUtf8().constData(),
        laterInvalidPrimary.error.toUtf8().constData(), noLaneChild.error.toUtf8().constData(),
        unanchored.error.toUtf8().constData(), missingAssetParent.error.toUtf8().constData(),
        audioRoot.error.toUtf8().constData(),
        audioRootNoLane.error.toUtf8().constData());
    std::fprintf(stderr, "G21 P2TrackIndex=%d childTrackIndex=%d childAnchorUid=%s "
        "childAnchorOffsetSec=%.9f childPosition=%.9f warningCount=%d error=%s\n",
        p2Location.first, offV1Location.first,
        offV1Child ? offV1Child->anchorUid.toUtf8().constData() : "",
        offV1Child ? offV1Child->anchorOffsetSec : -99.0, offV1Location.second,
        int(overlappingMain.warnings.count(offV1Warning)), overlappingMain.error.toUtf8().constData());
    std::fprintf(stderr, "G21 laneTwoParentTrackIndex=%d childTrackIndex=%d childAnchorUid=%s "
        "childAnchorOffsetSec=%.9f childPosition=%.9f warningCount=%d error=%s\n",
        laneTwoParentLocation.first, laneTwoChildLocation.first,
        laneTwoChild ? laneTwoChild->anchorUid.toUtf8().constData() : "",
        laneTwoChild ? laneTwoChild->anchorOffsetSec : -99.0, laneTwoChildLocation.second,
        int(laneTwoMain.warnings.count(laneTwoWarning)), laneTwoMain.error.toUtf8().constData());
    gate(21, badParentChild.error.isEmpty() && badParentChild.warnings.contains(badParentWarning)
        && !namedClip(badParentChild.videoTracks, QStringLiteral("orphan"))
        && badParentComponent.error.isEmpty() && badParentComponent.warnings.contains(badComponentWarning)
        && !namedClip(badParentComponent.videoTracks, QStringLiteral("orphan-video"))
        && laterInvalidPrimary.error.isEmpty() && laterInvalidPrimary.warnings.contains(earlyAudioWarning)
        && !namedClip(laterInvalidPrimary.audioTracks, QStringLiteral("early-audio"))
        && noLaneChild.error.isEmpty() && withoutLane && withoutLane->anchorUid.isEmpty()
        && unanchored.error.isEmpty() && unanchoredChild && unanchoredChild->anchorUid.isEmpty()
        && unanchoredJCut && unanchoredJCut->anchorUid.isEmpty()
        && missingAssetParent.error.isEmpty() && missingParentChild && missingParentChild->anchorUid.isEmpty()
        && missingAssetParent.warnings.contains(QStringLiteral("未定義の素材参照を読み飛ばしました: missing-ref"))
        && audioRoot.error.isEmpty() && detached && detached->anchorUid.isEmpty()
        && audioRoot.warnings.count(audioRootWarning) == 1
        && audioRootNoLane.error.isEmpty() && detachedNoLane && detachedNoLane->anchorUid.isEmpty()
        && audioRootNoLane.warnings.count(audioNoLaneWarning) == 1
        && overlappingMain.error.isEmpty() && p2Location.first == 1
        && offV1Location.first == 2 && offV1Child && offV1Child->anchorUid.isEmpty()
        && near(offV1Child->anchorOffsetSec, 0.0) && near(offV1Location.second, 4.0)
        && overlappingMain.warnings.count(offV1Warning) == 1
        && laneTwoMain.error.isEmpty() && laneTwoParentLocation.first == 2
        && laneTwoChildLocation.first == 1 && laneTwoChild && laneTwoChild->anchorUid.isEmpty()
        && near(laneTwoChild->anchorOffsetSec, 0.0) && near(laneTwoChildLocation.second, 4.0)
        && laneTwoMain.warnings.count(laneTwoWarning) == 1);

    return tally.finish();
}
