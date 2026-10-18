#include "../FcpxmlImport.h"
#include "../FcpxmlExporter.h"
#include "../ProjectFile.h"
#include "../UndoManager.h"
#include "SelftestGate.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

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
    for (const QString &tag : {QStringLiteral("filter-video"), QStringLiteral("adjust-color")})
        warnings &= unsupported.warnings.join('\n').contains(QStringLiteral(": %1 (").arg(tag));
    // Old pin: "ref-clip" and "media" skip warnings. New: neither; the empty
    // media sequence is reported as an unreadable compound clip instead.
    const bool refClipSkipWarning = unsupported.warnings.join('\n').contains(QStringLiteral(": ref-clip ("));
    const bool mediaSkipWarning = unsupported.warnings.join('\n').contains(QStringLiteral(": media ("));
    const int compoundWarning = int(unsupported.warnings.count(
        QStringLiteral("compound clip を読み込めないため読み飛ばしました: compound (読み込めるシーケンスがありません。)")));
    warnings &= !refClipSkipWarning && !mediaSkipWarning && compoundWarning == 1 && unsupported.sequences.isEmpty();
    // Old pin: an "mc-clip" skip warning. New: none; an mc-clip naming an
    // undefined multicam is reported as an undefined media reference instead.
    const bool mcClipSkipWarning = unsupported.warnings.join('\n').contains(QStringLiteral(": mc-clip ("));
    const int undefinedMulticam = int(unsupported.warnings.count(
        QStringLiteral("未定義の素材参照を読み飛ばしました: multi")));
    warnings &= !mcClipSkipWarning && undefinedMulticam == 1;
    // Old pin: a "transition" skip warning. New: no skip warning and a head FadeIn on the first clip.
    const bool transitionSkipWarning = unsupported.warnings.join('\n').contains(QStringLiteral(": transition ("));
    const bool headFade = unsupported.videoTracks[0][0].leadIn.type == TransitionType::FadeIn
        && near(unsupported.videoTracks[0][0].leadIn.duration, 1.0);
    warnings &= !transitionSkipWarning && headFade;
    warnings &= !unsupported.warnings.join('\n').contains(QStringLiteral(": clip ("));
    warnings &= !fcpx::xml::parseXml(QStringLiteral("<fcpxml><project>"), dir.path()).error.isEmpty()
        && !fcpx::xml::parseXml(QStringLiteral("<unrelated/>"), dir.path()).error.isEmpty()
        && !fcpx::xml::readFcpxml(dir.filePath(QStringLiteral("absent.fcpxml"))).error.isEmpty();
    std::fprintf(stderr, "G4 unsupportedError=%s importedClips=%d resolved=%d unresolved=%d "
        "warningCount=%d transitionSkipWarning=%d(old expected 1, new expected 0) headFade=%d "
        "refClipSkipWarning=%d(old expected 1, new expected 0) mediaSkipWarning=%d(old expected 1, new expected 0) "
        "compoundWarning=%d(old expected 0, new expected 1) mcClipSkipWarning=%d(old expected 1, new expected 0) "
        "undefinedMulticam=%d(old expected 0, new expected 1) sequences=%d allChecks=%d\n",
        unsupported.error.toUtf8().constData(),
        int(unsupported.videoTracks[0].size()), unsupported.resolvedFiles, unsupported.unresolvedFiles,
        int(unsupported.warnings.size()), int(transitionSkipWarning), int(headFade),
        int(refClipSkipWarning), int(mediaSkipWarning), compoundWarning,
        int(mcClipSkipWarning), undefinedMulticam, int(unsupported.sequences.size()),
        int(warnings));
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

    // G22-G24: spine transitions become edge transitions on both neighbouring clips.
    // The placement is the author's own fixture: no real FCP export was available.
    auto tracksEnd = [](const QVector<ClipInfo> &track) {
        double end = 0.0;
        for (const auto &c : track) end += c.leadInSec + c.effectiveDuration();
        return end;
    };
    const QString dissolve = QStringLiteral("<transition name=\"Cross Dissolve\" offset=\"3500/1000s\" duration=\"1s\"/>");
    const QString continuousClips = QStringLiteral(
        "<asset-clip ref=\"t\" name=\"A\" offset=\"0s\" duration=\"4s\"/>%1"
        "<asset-clip ref=\"t\" name=\"B\" offset=\"4s\" start=\"2s\" duration=\"3s\"/>");
    // The A side needs a known asset duration: its handle is what follows the out point.
    const QString timedAsset = QStringLiteral("<asset id=\"t\" src=\"offline.mov\" duration=\"10s\"/>");
    const auto continuous = fcpx::xml::parseXml(document(continuousClips.arg(dissolve), timedAsset), dir.path());
    const auto continuousPlain = fcpx::xml::parseXml(document(continuousClips.arg(QString()), timedAsset), dir.path());
    const ClipInfo *ca = namedClip(continuous.videoTracks, QStringLiteral("A"));
    const ClipInfo *cb = namedClip(continuous.videoTracks, QStringLiteral("B"));
    const ClipInfo *pa = namedClip(continuousPlain.videoTracks, QStringLiteral("A"));
    const ClipInfo *pb = namedClip(continuousPlain.videoTracks, QStringLiteral("B"));
    auto isDissolve = [&](const Transition &t, double d) {
        return t.type == TransitionType::CrossDissolve && near(t.duration, d)
            && t.alignment == TransitionAlignment::Center;
    };
    bool g22 = continuous.error.isEmpty() && ca && cb && pa && pb
        && continuous.videoTracks.size() == 1 && continuous.videoTracks[0].size() == 2
        && isDissolve(ca->trailOut, 1.0) && isDissolve(cb->leadIn, 1.0)
        && ca->leadIn.type == TransitionType::None && cb->trailOut.type == TransitionType::None
        && near(ca->inPoint, 0.0) && near(ca->outPoint, 4.0) && near(cb->inPoint, 2.0)
        && near(cb->outPoint, 5.0) && near(cb->leadInSec, 0.0)
        && continuous.warnings.filter(QStringLiteral("transition")).isEmpty()
        && pa->trailOut.type == TransitionType::None && pb->leadIn.type == TransitionType::None;
    // GUI canonical form: reapplying the same d/Center onto the imported clips (which already
    // carry the transitions) leaves every ClipInfo unchanged; the plain-clip result must match too.
    bool canonical = false, plainCanonical = false;
    QString canonicalLog;
    if (ca && cb) {
        auto sameEdge = [&](const Transition &x, const Transition &y) {
            return x.type == y.type && near(x.duration, y.duration) && x.alignment == y.alignment
                && x.easing == y.easing && x.softness == y.softness && x.borderWidth == y.borderWidth
                && x.borderColor == y.borderColor;
        };
        // ClipInfo serialisation (ProjectFile::clipToJson is private): one clip per project.
        auto clipJson = [](const ClipInfo &c) {
            ProjectData data;
            data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{c}};
            const auto root = QJsonDocument::fromJson(ProjectFile::toJsonString(data).toUtf8()).object();
            return QString::fromUtf8(QJsonDocument(root.value(QStringLiteral("videoTracks")).toArray())
                .toJson(QJsonDocument::Compact));
        };
        auto sameClip = [&](const ClipInfo &x, const ClipInfo &y) {
            return clipJson(x) == clipJson(y)
                && near(x.inPoint, y.inPoint) && near(x.outPoint, y.outPoint)
                && near(x.leadInSec, y.leadInSec) && near(x.duration, y.duration)
                && x.uid == y.uid && x.anchorUid == y.anchorUid
                && near(x.anchorOffsetSec, y.anchorOffsetSec)
                && sameEdge(x.leadIn, y.leadIn) && sameEdge(x.trailOut, y.trailOut);
        };
        Timeline gui;
        const QVector<ClipInfo> before = continuous.videoTracks[0];
        gui.trackAt(false, 0)->setClips(before);
        gui.trackAt(false, 0)->setSelectedClip(0);
        gui.applyTransitionToSelected(Transition{TransitionType::CrossDissolve, ca->trailOut.duration,
            TransitionAlignment::Center, ca->trailOut.easing});
        const auto after = gui.trackAt(false, 0)->clips();
        canonical = after.size() == before.size() && before.size() == 2;
        for (int i = 0; canonical && i < before.size(); ++i) canonical = sameClip(before[i], after[i]);
        for (int i = 0; i < before.size() && i < after.size(); ++i)
            canonicalLog += QStringLiteral(" clip%1 before=%2 after=%3").arg(i)
                .arg(clipJson(before[i]), clipJson(after[i]));
        Timeline plainGui;
        plainGui.trackAt(false, 0)->setClips(continuousPlain.videoTracks[0]);
        plainGui.trackAt(false, 0)->setSelectedClip(0);
        plainGui.applyTransitionToSelected(Transition{TransitionType::CrossDissolve, 1.0,
            TransitionAlignment::Center, TransitionEasing::Linear});
        const auto plainAfter = plainGui.trackAt(false, 0)->clips();
        plainCanonical = plainAfter.size() == 2 && sameEdge(plainAfter[0].trailOut, ca->trailOut)
            && sameEdge(plainAfter[1].leadIn, cb->leadIn)
            && sameEdge(plainAfter[0].leadIn, ca->leadIn) && sameEdge(plainAfter[1].trailOut, cb->trailOut);
    }
    canonical = canonical && plainCanonical;
    std::fprintf(stderr, "G22 error=%s tracks=%d A.trailOut=%d/%.9f B.leadIn=%d/%.9f A=[%.9f,%.9f] B=[%.9f,%.9f] "
        "Blead=%.9f controlA=%d controlB=%d guiCanonical=%d plainCanonical=%d%s\n", continuous.error.toUtf8().constData(),
        int(continuous.videoTracks.size()), ca ? int(ca->trailOut.type) : -1, ca ? ca->trailOut.duration : -1.0,
        cb ? int(cb->leadIn.type) : -1, cb ? cb->leadIn.duration : -1.0,
        ca ? ca->inPoint : -1.0, ca ? ca->outPoint : -1.0, cb ? cb->inPoint : -1.0, cb ? cb->outPoint : -1.0,
        cb ? cb->leadInSec : -1.0, pa ? int(pa->trailOut.type) : -1, pb ? int(pb->leadIn.type) : -1, int(canonical),
        int(plainCanonical), canonicalLog.toUtf8().constData());
    gate(22, g22 && canonical);

    const QString overlappedClips = QStringLiteral(
        "<asset-clip ref=\"a\" name=\"A\" offset=\"0s\" duration=\"5s\"/>%1"
        "<asset-clip ref=\"a\" name=\"B\" offset=\"4s\" start=\"2s\" duration=\"4s\"/>");
    const auto overlapped = fcpx::xml::parseXml(document(overlappedClips.arg(
        QStringLiteral("<transition name=\"Cross Dissolve\" offset=\"4s\" duration=\"1s\"/>"))), dir.path());
    const auto overlappedPlain = fcpx::xml::parseXml(document(overlappedClips.arg(QString())), dir.path());
    const ClipInfo *oa = namedClip(overlapped.videoTracks, QStringLiteral("A"));
    const ClipInfo *ob = namedClip(overlapped.videoTracks, QStringLiteral("B"));
    const auto overlappedMissing = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"A\" offset=\"0s\" duration=\"5s\"/>"
        "<transition name=\"Cross Dissolve\" offset=\"4s\" duration=\"1s\"/>"
        "<asset-clip ref=\"missing\" name=\"B\" offset=\"4s\" start=\"2s\" duration=\"4s\"/>")), dir.path());
    const ClipInfo *ma = namedClip(overlappedMissing.videoTracks, QStringLiteral("A"));
    const QString missingWarning = QStringLiteral("未定義の素材参照を読み飛ばしました: missing");
    // Asset with a nonzero start: handleEnd is in source coordinates and must be
    // shifted back by asset.start (old code added it: duration 205 instead of 5).
    const auto offsetAsset = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"s\" name=\"A\" offset=\"0s\" start=\"100s\" duration=\"5s\"/>"
        "<transition name=\"Cross Dissolve\" offset=\"4s\" duration=\"2s\"/>"
        "<asset-clip ref=\"s\" name=\"B\" offset=\"4s\" start=\"102s\" duration=\"4s\"/>"),
        QStringLiteral("<asset id=\"s\" src=\"offline.mov\" start=\"100s\" duration=\"5s\"/>")), dir.path());
    const ClipInfo *sA = namedClip(offsetAsset.videoTracks, QStringLiteral("A"));
    // Invalid B (clip start before asset start) must leave A untrimmed.
    const auto invalidB = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"A\" offset=\"0s\" duration=\"5s\"/>"
        "<transition name=\"Cross Dissolve\" offset=\"4s\" duration=\"1s\"/>"
        "<asset-clip ref=\"t\" name=\"B\" offset=\"4s\" start=\"0s\" duration=\"4s\"/>"),
        QStringLiteral("<asset id=\"t\" src=\"offline.mov\" start=\"10s\"/>")), dir.path());
    const ClipInfo *iA = namedClip(invalidB.videoTracks, QStringLiteral("A"));
    const QString rangeWarning = QStringLiteral("lane 0 の時刻範囲が不正なクリップを読み飛ばしました: B / t");
    const double iSpan = iA ? iA->outPoint - iA->inPoint : -1.0;
    // Negative control: P overlaps A, so placement promotes A to V2 while B stays on V1. The
    // A-B transition is then dropped; the overlap trim must be restored (A and B untrimmed).
    const auto promoted = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"P\" offset=\"0s\" duration=\"6s\"/>"
        "<asset-clip ref=\"a\" name=\"A\" offset=\"4s\" duration=\"5s\"/>"
        "<transition name=\"Cross Dissolve\" offset=\"8s\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"B\" offset=\"8s\" start=\"2s\" duration=\"4s\"/>")), dir.path());
    const ClipInfo *promA = namedClip(promoted.videoTracks, QStringLiteral("A"));
    const ClipInfo *promB = namedClip(promoted.videoTracks, QStringLiteral("B"));
    const QString promotedWarning = QStringLiteral("未対応の要素を読み飛ばしました: transition (行");
    const int promotedWarnings = int(promoted.warnings.filter(promotedWarning).size());
    const double promASpan = promA ? promA->outPoint - promA->inPoint : -1.0;
    const bool promotedOk = promoted.error.isEmpty() && promA && promB && near(promASpan, 5.0)
        && near(promB->inPoint, 2.0) && promA->trailOut.type == TransitionType::None
        && promB->leadIn.type == TransitionType::None && promotedWarnings == 1;
    // Two adjacent transitions: A is pushed to V2 by P, so only B-C survives. Every retry pass must
    // start from untrimmed entries (old code left C trimmed from pass 1: cut 11.75, C.in 2.75, d 0.5).
    const auto chained = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"P\" offset=\"0s\" duration=\"6s\"/>"
        "<asset-clip ref=\"a\" name=\"A\" offset=\"4s\" duration=\"5s\"/>"
        "<transition name=\"Cross Dissolve\" offset=\"8s\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"B\" offset=\"8s\" start=\"2s\" duration=\"4s\"/>"
        "<transition name=\"Cross Dissolve\" offset=\"11s\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"C\" offset=\"11s\" start=\"2s\" duration=\"4s\"/>")), dir.path());
    const ClipInfo *chA = namedClip(chained.videoTracks, QStringLiteral("A"));
    const ClipInfo *chB = namedClip(chained.videoTracks, QStringLiteral("B"));
    const ClipInfo *chC = namedClip(chained.videoTracks, QStringLiteral("C"));
    const int chainedShort = int(chained.warnings.filter(QStringLiteral("transition Cross Dissolve のハンドルが不足したため")).size());
    const int chainedShortAny = int(chained.warnings.filter(QStringLiteral("のハンドルが不足したため")).size());
    const int chainedSkips = int(chained.warnings.filter(promotedWarning).size());
    const double chASpan = chA ? chA->outPoint - chA->inPoint : -1.0;
    const bool chainedOk = chained.error.isEmpty() && chA && chB && chC && near(chASpan, 5.0)
        && chA->trailOut.type == TransitionType::None && near(chB->inPoint, 2.0)
        && chB->leadIn.type == TransitionType::None && near(chB->outPoint, 5.5)
        && near(chC->inPoint, 2.5) && isDissolve(chB->trailOut, 1.0) && isDissolve(chC->leadIn, 1.0)
        && chB->trailOut.alignment == TransitionAlignment::Center
        && chainedShort == 0 && chainedShortAny == 0 && chainedSkips == 1;
    std::fprintf(stderr, "G23 chained Aspan=%.9f AtrailOut=%d Bin=%.9f BleadIn=%d Bout=%.9f (cut=%.9f, old=11.75) "
        "Cin=%.9f (old=2.75) BtrailOut=%d/%.9f (old d=0.5) ClleadIn=%d/%.9f shortWarnings=%d skipWarnings=%d "
        "chainedOk=%d\n", chASpan, chA ? int(chA->trailOut.type) : -1, chB ? chB->inPoint : -1.0,
        chB ? int(chB->leadIn.type) : -1, chB ? chB->outPoint : -1.0, chB ? 8.0 + (chB->outPoint - chB->inPoint) : -1.0,
        chC ? chC->inPoint : -1.0, chB ? int(chB->trailOut.type) : -1, chB ? chB->trailOut.duration : -1.0,
        chC ? int(chC->leadIn.type) : -1, chC ? chC->leadIn.duration : -1.0, chainedShortAny, chainedSkips,
        int(chainedOk));
    const double overlappedEnd = overlapped.videoTracks.isEmpty() ? -1.0 : tracksEnd(overlapped.videoTracks[0]);
    std::fprintf(stderr, "G23 tracks=%d length=%.9f expectedLength=8 A=[%.9f,%.9f] B=[%.9f,%.9f] Blead=%.9f "
        "A.trailOut=%d/%.9f B.leadIn=%d/%.9f controlTracks=%d Aduration=%.9f "
        "missingB=[%.9f,%.9f] missingTrailOut=%d missingWarnings=%d "
        "offsetAssetAduration=%.9f (old=205) offsetAssetTrailOut=%.9f (old=2) invalidBspan=%.9f (old=4.5) "
        "invalidBtrailOut=%d invalidBwarnings=%d "
        "promotedAspan=%.9f (trimmed=4.5) promotedBin=%.9f (trimmed=2.5) promotedAtrailOut=%d promotedBleadIn=%d "
        "promotedSkipWarnings=%d promotedOk=%d\n", int(overlapped.videoTracks.size()),
        overlappedEnd, oa ? oa->inPoint : -1.0, oa ? oa->outPoint : -1.0, ob ? ob->inPoint : -1.0,
        ob ? ob->outPoint : -1.0, ob ? ob->leadInSec : -1.0, oa ? int(oa->trailOut.type) : -1,
        oa ? oa->trailOut.duration : -1.0, ob ? int(ob->leadIn.type) : -1, ob ? ob->leadIn.duration : -1.0,
        int(overlappedPlain.videoTracks.size()), oa ? oa->duration : -1.0,
        ma ? ma->inPoint : -1.0, ma ? ma->outPoint : -1.0, ma ? int(ma->trailOut.type) : -1,
        int(overlappedMissing.warnings.count(missingWarning)),
        sA ? sA->duration : -1.0, sA ? sA->trailOut.duration : -1.0, iSpan,
        iA ? int(iA->trailOut.type) : -1, int(invalidB.warnings.count(rangeWarning)),
        promASpan, promB ? promB->inPoint : -1.0, promA ? int(promA->trailOut.type) : -1,
        promB ? int(promB->leadIn.type) : -1, promotedWarnings, int(promotedOk));
    gate(23, overlapped.error.isEmpty() && oa && ob && overlapped.videoTracks.size() == 1
        && near(overlappedEnd, 8.0) && near(oa->inPoint, 0.0) && near(oa->outPoint, 4.5)
        && near(ob->inPoint, 2.5) && near(ob->outPoint, 6.0) && near(ob->leadInSec, 0.0)
        && isDissolve(oa->trailOut, 1.0) && isDissolve(ob->leadIn, 1.0)
        && overlappedPlain.videoTracks.size() == 2 && oa->duration >= 5.0 - 1e-9
        && overlappedMissing.error.isEmpty() && ma && near(ma->inPoint, 0.0) && near(ma->outPoint, 5.0)
        && ma->trailOut.type == TransitionType::None
        && overlappedMissing.warnings.count(missingWarning) == 1
        && offsetAsset.error.isEmpty() && sA && near(sA->duration, 5.0) && isDissolve(sA->trailOut, 1.0)
        && invalidB.error.isEmpty() && iA && near(iSpan, 5.0) && iA->trailOut.type == TransitionType::None
        && invalidB.warnings.count(rangeWarning) == 1 && promotedOk && chainedOk);

    const QString handleAsset = QStringLiteral("<asset id=\"h\" src=\"short.mov\" duration=\"5s\"/>");
    const QString shortWarning = QStringLiteral("transition Cross Dissolve のハンドルが不足したため長さを 0.2 秒に縮めました");
    const auto handleShort = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"h\" name=\"A\" offset=\"0s\" duration=\"49/10s\"/>"
        "<transition name=\"Cross Dissolve\" duration=\"1s\"/>"
        "<asset-clip ref=\"h\" name=\"B\" offset=\"49/10s\" start=\"3/10s\" duration=\"2s\"/>"), handleAsset), dir.path());
    const ClipInfo *sa = namedClip(handleShort.videoTracks, QStringLiteral("A"));
    const ClipInfo *sb = namedClip(handleShort.videoTracks, QStringLiteral("B"));
    const auto headTail = fcpx::xml::parseXml(document(QStringLiteral(
        "<transition name=\"Cross Dissolve\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" name=\"first\" offset=\"0s\" duration=\"3s\"/>"
        "<asset-clip ref=\"a\" name=\"last\" offset=\"3s\" duration=\"3s\"/>"
        "<transition name=\"Cross Dissolve\" duration=\"1s\"/>")), dir.path());
    const ClipInfo *first = namedClip(headTail.videoTracks, QStringLiteral("first"));
    const ClipInfo *last = namedClip(headTail.videoTracks, QStringLiteral("last"));
    const QString slideWarning = QStringLiteral("transition の名前 Slide は未対応のため Cross Dissolve に置き換えました");
    const auto slide = fcpx::xml::parseXml(document(continuousClips.arg(QStringLiteral(
        "<transition name=\"Slide\" duration=\"1s\"/>")), timedAsset), dir.path());
    const ClipInfo *slideA = namedClip(slide.videoTracks, QStringLiteral("A"));
    const ClipInfo *slideB = namedClip(slide.videoTracks, QStringLiteral("B"));
    const auto noDuration = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"A\" offset=\"0s\" duration=\"4s\"/>"
        "<transition name=\"Cross Dissolve\" duration=\"2s\"/>"
        "<asset-clip ref=\"a\" name=\"B\" offset=\"4s\" start=\"1s\" duration=\"3s\"/>")), dir.path());
    const ClipInfo *na = namedClip(noDuration.videoTracks, QStringLiteral("A"));
    const ClipInfo *nb = namedClip(noDuration.videoTracks, QStringLiteral("B"));
    const QString noDurationWarning = QStringLiteral("transition Cross Dissolve のハンドルが不足したため長さを 0 秒に縮めました");
    std::fprintf(stderr, "G24 noDuration A.trailOut=%d/%.9f B.leadIn=%d/%.9f warnings=%d\n",
        na ? int(na->trailOut.type) : -1, na ? na->trailOut.duration : -1.0,
        nb ? int(nb->leadIn.type) : -1, nb ? nb->leadIn.duration : -1.0,
        int(noDuration.warnings.count(noDurationWarning)));
    std::fprintf(stderr, "G24 shortA=%d/%.9f shortB=%d/%.9f shortWarnings=%d headFade=%d/%.9f tailFade=%d/%.9f "
        "slideA=%d slideB=%d slideWarnings=%d controlWideWarnings=%d\n",
        sa ? int(sa->trailOut.type) : -1, sa ? sa->trailOut.duration : -1.0,
        sb ? int(sb->leadIn.type) : -1, sb ? sb->leadIn.duration : -1.0,
        int(handleShort.warnings.count(shortWarning)),
        first ? int(first->leadIn.type) : -1, first ? first->leadIn.duration : -1.0,
        last ? int(last->trailOut.type) : -1, last ? last->trailOut.duration : -1.0,
        slideA ? int(slideA->trailOut.type) : -1, slideB ? int(slideB->leadIn.type) : -1,
        int(slide.warnings.count(slideWarning)), int(continuous.warnings.filter(QStringLiteral("縮め")).size()));
    gate(24, noDuration.error.isEmpty() && na && nb && na->trailOut.type == TransitionType::None
        && nb->leadIn.type == TransitionType::None && noDuration.warnings.count(noDurationWarning) == 1
        && handleShort.error.isEmpty() && sa && sb && isDissolve(sa->trailOut, 0.2)
        && isDissolve(sb->leadIn, 0.2) && handleShort.warnings.count(shortWarning) == 1
        && headTail.error.isEmpty() && first && last
        && first->leadIn.type == TransitionType::FadeIn && near(first->leadIn.duration, 1.0)
        && first->trailOut.type == TransitionType::None
        && last->trailOut.type == TransitionType::FadeOut && near(last->trailOut.duration, 1.0)
        && last->leadIn.type == TransitionType::None
        && slide.error.isEmpty() && slideA && slideB && isDissolve(slideA->trailOut, 1.0)
        && isDissolve(slideB->leadIn, 1.0) && slide.warnings.count(slideWarning) == 1
        && continuous.warnings.filter(QStringLiteral("縮め")).isEmpty());

    // G25-G28: resources/media + ref-clip become nested sequences, and
    // applyImport adds them under ids that Timeline::addSequence keeps unique.
    auto findSequence = [](const QVector<TimelineSequence> &sequences, const QString &id) -> const TimelineSequence * {
        for (const auto &sequence : sequences)
            if (sequence.id == id) return &sequence;
        return nullptr;
    };
    auto clipNames = [](const TimelineSequence *sequence) {
        QStringList names;
        if (sequence)
            for (const auto &track : sequence->videoTracks)
                for (const auto &clip : track) names.append(clip.displayName + QLatin1Char('>') + clip.sequenceRefId);
        return names.join(QLatin1Char(',')).toUtf8();
    };
    const QString compoundMedia = QStringLiteral(
        "<media id=\"cmp\" name=\"複合 クリップ\"><sequence tcStart=\"0s\"><spine>"
        "<asset-clip ref=\"a\" name=\"inner1\" duration=\"2s\"/>"
        "<asset-clip ref=\"a\" name=\"inner2\" offset=\"2s\" duration=\"3s\"/>"
        "</spine></sequence></media>");
    const QString compoundSpine = QStringLiteral(
        "<asset-clip ref=\"a\" name=\"lead\" duration=\"1s\"/>"
        "<ref-clip ref=\"cmp\" name=\"compound\" offset=\"1s\" start=\"1s\" duration=\"3s\"/>");
    const auto single = fcpx::xml::parseXml(document(compoundSpine, compoundMedia), dir.path());
    const ClipInfo *singleRef = namedClip(single.videoTracks, QStringLiteral("compound"));
    const TimelineSequence *singleNested = single.sequences.size() == 1 ? &single.sequences[0] : nullptr;
    const bool singleParsed = single.error.isEmpty() && singleNested && singleRef
        && singleNested->id == QStringLiteral("cmp") && singleNested->name == QStringLiteral("複合 クリップ")
        && singleNested->videoTracks.size() == 1 && singleNested->videoTracks[0].size() == 2
        && near(singleNested->duration(), 5.0) && single.videoTracks[0].size() == 2
        && singleRef->sequenceRefId == singleNested->id
        && singleRef->filePath == timeline_nesting::sequenceClipFilePath(QStringLiteral("cmp"))
        && near(singleRef->duration, singleNested->duration())
        && near(singleRef->inPoint, 1.0) && near(singleRef->outPoint, 4.0)
        && single.warnings.filter(QStringLiteral("ref-clip")).isEmpty();
    std::fprintf(stderr, "G25 parse error=%s sequences=%d id=%s name=%s nestedClips=%s nestedDuration=%.9f "
        "(spine sum 2+3=5) V1=%d ref=%s filePath=%s duration=%.9f in=%.9f out=%.9f refClipWarnings=%d\n",
        single.error.toUtf8().constData(), int(single.sequences.size()),
        singleNested ? singleNested->id.toUtf8().constData() : "",
        singleNested ? singleNested->name.toUtf8().constData() : "", clipNames(singleNested).constData(),
        singleNested ? singleNested->duration() : -1.0, int(single.videoTracks[0].size()),
        singleRef ? singleRef->sequenceRefId.toUtf8().constData() : "",
        singleRef ? singleRef->filePath.toUtf8().constData() : "",
        singleRef ? singleRef->duration : -1.0, singleRef ? singleRef->inPoint : -1.0,
        singleRef ? singleRef->outPoint : -1.0, int(single.warnings.filter(QStringLiteral("ref-clip")).size()));
    // A timeline without a sequence model: the first addSequence also
    // materializes the default sequence, which must not be taken as the new id.
    Timeline compoundTimeline;
    auto singleApplied = single;
    QStringList singleWarnings;
    const int modelBefore = compoundTimeline.sequences().size();
    const bool singleOk = fcpx::xml::applyImport(compoundTimeline, singleApplied, &singleWarnings);
    const auto singleStore = compoundTimeline.sequences();
    const QString singleId = singleApplied.sequences.value(0).id;
    const TimelineSequence *singleStored = findSequence(singleStore, singleId);
    const TimelineSequence *singleActive = findSequence(singleStore, compoundTimeline.activeSequenceId());
    const ClipInfo *singleActiveRef = singleActive ? namedClip(singleActive->videoTracks, QStringLiteral("compound")) : nullptr;
    const bool singleApply = singleOk && modelBefore == 0 && singleStore.size() == 3
        && singleId == QStringLiteral("cmp") && compoundTimeline.activeSequenceId() != singleId
        && singleStored && singleStored->name == QStringLiteral("複合 クリップ")
        && singleStored->videoTracks.value(0).size() == 2 && singleActive && singleActiveRef
        && singleActiveRef->sequenceRefId == singleId
        && singleActive->name == QStringLiteral("読み込み");
    QStringList storeIds;
    for (const auto &sequence : singleStore) storeIds.append(sequence.id);
    std::fprintf(stderr, "G25 apply ok=%d sequencesBefore=%d store=%s addedId=%s active=%s activeName=%s "
        "activeClips=%s storedClips=%s warnings=%d\n",
        int(singleOk), modelBefore, storeIds.join(QLatin1Char(',')).toUtf8().constData(),
        singleId.toUtf8().constData(), compoundTimeline.activeSequenceId().toUtf8().constData(),
        singleActive ? singleActive->name.toUtf8().constData() : "", clipNames(singleActive).constData(),
        clipNames(singleStored).constData(), int(singleWarnings.size()));
    gate(25, singleParsed && singleApply);

    // G26: two levels; the outer media is listed first and refers to the inner one.
    const auto twoLevels = fcpx::xml::parseXml(document(
        QStringLiteral("<ref-clip ref=\"outer\" name=\"toOuter\" duration=\"3s\"/>"),
        QStringLiteral("<media id=\"outer\" name=\"外側\"><sequence><spine>"
            "<asset-clip ref=\"a\" name=\"mid\" duration=\"1s\"/>"
            "<ref-clip ref=\"inner\" name=\"toInner\" offset=\"1s\" duration=\"2s\"/>"
            "</spine></sequence></media>"
            "<media id=\"inner\" name=\"内側\"><sequence><spine>"
            "<asset-clip ref=\"a\" name=\"deep\" duration=\"2s\"/>"
            "</spine></sequence></media>")), dir.path());
    const TimelineSequence *parsedInner = twoLevels.sequences.size() == 2 ? &twoLevels.sequences[0] : nullptr;
    const TimelineSequence *parsedOuter = twoLevels.sequences.size() == 2 ? &twoLevels.sequences[1] : nullptr;
    const ClipInfo *toInner = parsedOuter ? namedClip(parsedOuter->videoTracks, QStringLiteral("toInner")) : nullptr;
    const ClipInfo *toOuter = namedClip(twoLevels.videoTracks, QStringLiteral("toOuter"));
    const bool twoParsed = twoLevels.error.isEmpty() && parsedInner && parsedOuter && toInner && toOuter
        && parsedInner->id == QStringLiteral("inner") && parsedOuter->id == QStringLiteral("outer")
        && toInner->sequenceRefId == parsedInner->id && toOuter->sequenceRefId == parsedOuter->id
        && near(parsedInner->duration(), 2.0) && near(parsedOuter->duration(), 3.0)
        && near(toInner->duration, 2.0) && near(toOuter->duration, 3.0);
    std::fprintf(stderr, "G26 parse error=%s sequences=%d inner=%s(%s, %.9f) outer=%s(%s, %.9f) toOuter=%s\n",
        twoLevels.error.toUtf8().constData(), int(twoLevels.sequences.size()),
        parsedInner ? parsedInner->id.toUtf8().constData() : "", clipNames(parsedInner).constData(),
        parsedInner ? parsedInner->duration() : -1.0,
        parsedOuter ? parsedOuter->id.toUtf8().constData() : "", clipNames(parsedOuter).constData(),
        parsedOuter ? parsedOuter->duration() : -1.0,
        toOuter ? toOuter->sequenceRefId.toUtf8().constData() : "");
    // A sequence already named "inner" makes addSequence rename the imported one,
    // so the reference inside the outer nested sequence has to follow.
    Timeline nestedTimeline;
    TimelineSequence existingInner;
    existingInner.id = QStringLiteral("inner");
    existingInner.name = QStringLiteral("既存");
    existingInner.videoTracks = {QVector<ClipInfo>{}};
    existingInner.audioTracks = {QVector<ClipInfo>{}};
    nestedTimeline.addSequence(existingInner);
    auto twoApplied = twoLevels;
    QStringList twoWarnings;
    const bool twoOk = fcpx::xml::applyImport(nestedTimeline, twoApplied, &twoWarnings);
    const auto twoStore = nestedTimeline.sequences();
    const QString innerId = twoApplied.sequences.value(0).id, outerId = twoApplied.sequences.value(1).id;
    const TimelineSequence *storedInner = findSequence(twoStore, innerId);
    const TimelineSequence *storedOuter = findSequence(twoStore, outerId);
    const TimelineSequence *keptInner = findSequence(twoStore, QStringLiteral("inner"));
    const TimelineSequence *twoActive = findSequence(twoStore, nestedTimeline.activeSequenceId());
    const ClipInfo *storedToInner = storedOuter ? namedClip(storedOuter->videoTracks, QStringLiteral("toInner")) : nullptr;
    const ClipInfo *activeToOuter = twoActive ? namedClip(twoActive->videoTracks, QStringLiteral("toOuter")) : nullptr;
    const bool twoApply = twoOk && innerId == QStringLiteral("inner-2") && outerId == QStringLiteral("outer")
        && storedInner && storedInner->name == QStringLiteral("内側") && storedOuter && storedToInner
        && storedToInner->sequenceRefId == innerId
        && storedToInner->filePath == timeline_nesting::sequenceClipFilePath(innerId)
        && keptInner && keptInner->name == QStringLiteral("既存") && keptInner->videoTracks.value(0).isEmpty()
        && activeToOuter && activeToOuter->sequenceRefId == outerId;
    QStringList twoIds;
    for (const auto &sequence : twoStore) twoIds.append(sequence.id);
    std::fprintf(stderr, "G26 apply ok=%d store=%s innerId=%s outerId=%s storedOuter=%s kept inner=%s(%s) "
        "activeToOuter=%s\n",
        int(twoOk), twoIds.join(QLatin1Char(',')).toUtf8().constData(), innerId.toUtf8().constData(),
        outerId.toUtf8().constData(), clipNames(storedOuter).constData(),
        keptInner ? keptInner->name.toUtf8().constData() : "", clipNames(keptInner).constData(),
        activeToOuter ? activeToOuter->sequenceRefId.toUtf8().constData() : "");
    // A compound with a video and an audio clip is referenced from V1 and A1,
    // so its nested audio plays; a video-only compound stays on V1 only.
    auto refsOn = [](const QVector<QVector<ClipInfo>> &tracks) {
        QStringList refs;
        for (const auto &track : tracks)
            for (const auto &clip : track)
                if (!clip.sequenceRefId.isEmpty()) refs.append(clip.sequenceRefId);
        return refs;
    };
    const auto mixed = fcpx::xml::parseXml(document(
        QStringLiteral("<ref-clip ref=\"mix\" name=\"toMix\" duration=\"2s\"/>"),
        QStringLiteral("<asset id=\"snd\" src=\"voice.wav\" hasVideo=\"0\" hasAudio=\"1\"/>"
            "<media id=\"mix\" name=\"混在\"><sequence><spine>"
            "<asset-clip ref=\"a\" name=\"mixVideo\" duration=\"2s\">"
            "<asset-clip ref=\"snd\" name=\"mixAudio\" lane=\"-1\" duration=\"2s\"/>"
            "</asset-clip></spine></sequence></media>")), dir.path());
    const TimelineSequence *parsedMix = mixed.sequences.size() == 1 ? &mixed.sequences[0] : nullptr;
    const ClipInfo *mixAudio = parsedMix ? namedClip(parsedMix->audioTracks, QStringLiteral("mixAudio")) : nullptr;
    const QString voicePath = mixAudio ? mixAudio->filePath : QString();
    const ClipInfo *mixVideoRef = namedClip(mixed.videoTracks, QStringLiteral("toMix"));
    const ClipInfo *mixAudioRef = namedClip(mixed.audioTracks, QStringLiteral("toMix"));
    const bool mixParsed = mixed.error.isEmpty() && parsedMix && mixAudio && voicePath.endsWith(QStringLiteral("/voice.wav"))
        && mixVideoRef && mixAudioRef && mixVideoRef->sequenceRefId == QStringLiteral("mix")
        && mixAudioRef->sequenceRefId == QStringLiteral("mix") && mixAudioRef->filePath == mixVideoRef->filePath
        && near(mixAudioRef->inPoint, mixVideoRef->inPoint) && near(mixAudioRef->outPoint, mixVideoRef->outPoint)
        && near(mixAudioRef->leadInSec, mixVideoRef->leadInSec)
        && !mixVideoRef->uid.isEmpty() && mixAudioRef->uid.isEmpty() && mixAudioRef->anchorUid.isEmpty();
    Timeline mixTimeline;
    auto mixApplied = mixed;
    QStringList mixWarnings;
    const bool mixOk = fcpx::xml::applyImport(mixTimeline, mixApplied, &mixWarnings);
    const QString mixId = mixApplied.sequences.value(0).id;
    const auto mixStore = mixTimeline.sequences();
    const TimelineSequence *mixActive = findSequence(mixStore, mixTimeline.activeSequenceId());
    const QStringList mixV1 = mixActive ? refsOn(mixActive->videoTracks.mid(0, 1)) : QStringList();
    const QStringList mixA1 = mixActive ? refsOn(mixActive->audioTracks.mid(0, 1)) : QStringList();
    auto playsPath = [](const QVector<PlaybackEntry> &entries, const QString &path) {
        return std::any_of(entries.cbegin(), entries.cend(), [&](const PlaybackEntry &e) { return e.filePath == path; });
    };
    const auto mixAudioPlayback = mixTimeline.computeAudioPlaybackSequence();
    const auto mixVideoPlayback = mixTimeline.computePlaybackSequence();
    const bool mixPlays = !voicePath.isEmpty() && playsPath(mixAudioPlayback, voicePath);
    const bool mixApply = mixOk && !mixId.isEmpty() && mixV1 == QStringList{mixId} && mixA1 == QStringList{mixId}
        && mixPlays;
    // Negative control: the video-only compounds of G25 / G26 get no audio-track reference.
    const QStringList singleAudioRefs = refsOn(single.audioTracks), twoAudioRefs = refsOn(twoLevels.audioTracks);
    const QStringList outerAudioRefs = parsedOuter ? refsOn(parsedOuter->audioTracks) : QStringList{QStringLiteral("?")};
    const bool videoOnlyOk = singleAudioRefs.isEmpty() && twoAudioRefs.isEmpty() && outerAudioRefs.isEmpty()
        && refsOn(single.videoTracks) == QStringList{QStringLiteral("cmp")}
        && refsOn(twoLevels.videoTracks) == QStringList{QStringLiteral("outer")};
    std::fprintf(stderr, "G26 mixed parse error=%s sequences=%d nestedAudio=%s videoRef=%s audioRef=%s "
        "videoUid=%d audioUid=%d audioAnchor=%d | apply ok=%d id=%s V1=%s A1=%s audioPlaybackEntries=%d "
        "audioPlaysNested=%d videoPlaysNested=%d warnings=%d | video-only audioRefs single=%s twoLevels=%s outer=%s\n",
        mixed.error.toUtf8().constData(), int(mixed.sequences.size()), voicePath.toUtf8().constData(),
        mixVideoRef ? mixVideoRef->sequenceRefId.toUtf8().constData() : "",
        mixAudioRef ? mixAudioRef->sequenceRefId.toUtf8().constData() : "",
        int(mixVideoRef && !mixVideoRef->uid.isEmpty()), int(mixAudioRef && !mixAudioRef->uid.isEmpty()),
        int(mixAudioRef && !mixAudioRef->anchorUid.isEmpty()), int(mixOk), mixId.toUtf8().constData(),
        mixV1.join(QLatin1Char(',')).toUtf8().constData(), mixA1.join(QLatin1Char(',')).toUtf8().constData(),
        int(mixAudioPlayback.size()), int(mixPlays), int(playsPath(mixVideoPlayback, voicePath)), int(mixWarnings.size()),
        singleAudioRefs.join(QLatin1Char(',')).toUtf8().constData(), twoAudioRefs.join(QLatin1Char(',')).toUtf8().constData(),
        outerAudioRefs.join(QLatin1Char(',')).toUtf8().constData());
    // A mixed compound connected to a V1 clip: its V2 video and A1 audio
    // references share the anchor, so a storyline-mode Delete of an earlier V1
    // clip moves both with the host (an unanchored audio copy stays at 5 s).
    const auto connectedMix = fcpx::xml::parseXml(document(
        QStringLiteral("<asset-clip ref=\"a\" name=\"lead\" duration=\"2s\"/>"
            "<asset-clip ref=\"a\" name=\"host\" offset=\"2s\" duration=\"4s\">"
            "<ref-clip ref=\"mix\" name=\"toMixC\" lane=\"1\" offset=\"3s\" duration=\"2s\"/></asset-clip>"),
        QStringLiteral("<asset id=\"snd\" src=\"voice.wav\" hasVideo=\"0\" hasAudio=\"1\"/>"
            "<media id=\"mix\" name=\"混在\"><sequence><spine>"
            "<asset-clip ref=\"a\" name=\"mixVideo\" duration=\"2s\">"
            "<asset-clip ref=\"snd\" name=\"mixAudio\" lane=\"-1\" duration=\"2s\"/>"
            "</asset-clip></spine></sequence></media>")), dir.path());
    const ClipInfo *hostClip = namedClip(connectedMix.videoTracks, QStringLiteral("host"));
    const ClipInfo *cVideoRef = namedClip(connectedMix.videoTracks, QStringLiteral("toMixC"));
    const ClipInfo *cAudioRef = namedClip(connectedMix.audioTracks, QStringLiteral("toMixC"));
    const QString hostUid = hostClip ? hostClip->uid : QString();
    const bool connectedParsed = connectedMix.error.isEmpty() && hostClip && !hostUid.isEmpty()
        && cVideoRef && cAudioRef && cVideoRef->sequenceRefId == QStringLiteral("mix")
        && cAudioRef->sequenceRefId == QStringLiteral("mix")
        && cVideoRef->anchorUid == hostUid && cAudioRef->anchorUid == hostUid
        && near(cVideoRef->anchorOffsetSec, 3.0) && near(cAudioRef->anchorOffsetSec, cVideoRef->anchorOffsetSec)
        && cAudioRef->uid.isEmpty();
    // The same compound connected to an audio-only spine clip is detached; the
    // detach warning comes once per compound, not once per track reference.
    const auto audioHostMix = fcpx::xml::parseXml(document(
        QStringLiteral("<asset-clip ref=\"snd\" name=\"soundHost\" duration=\"4s\">"
            "<ref-clip ref=\"mix\" name=\"toMixD\" lane=\"1\" offset=\"1s\" duration=\"2s\"/></asset-clip>"),
        QStringLiteral("<asset id=\"snd\" src=\"voice.wav\" hasVideo=\"0\" hasAudio=\"1\"/>"
            "<media id=\"mix\" name=\"混在\"><sequence><spine>"
            "<asset-clip ref=\"a\" name=\"mixVideo\" duration=\"2s\">"
            "<asset-clip ref=\"snd\" name=\"mixAudio\" lane=\"-1\" duration=\"2s\"/>"
            "</asset-clip></spine></sequence></media>")), dir.path());
    const QString audioHostWarning = QStringLiteral("主ストーリーラインが音声クリップのため接続を解除しました: toMixD");
    const ClipInfo *dVideoRef = namedClip(audioHostMix.videoTracks, QStringLiteral("toMixD"));
    const ClipInfo *dAudioRef = namedClip(audioHostMix.audioTracks, QStringLiteral("toMixD"));
    const int audioHostWarnings = int(audioHostMix.warnings.count(audioHostWarning));
    const bool audioHostOk = audioHostMix.error.isEmpty() && audioHostWarnings == 1 && dVideoRef && dAudioRef
        && dVideoRef->anchorUid.isEmpty() && dAudioRef->anchorUid.isEmpty();
    auto liveStart = [](const Timeline &tl, bool audio, const QString &name) {
        for (int t = 0; t < (audio ? tl.audioTrackCount() : tl.videoTrackCount()); ++t) {
            double cursor = 0.0;
            for (const auto &clip : tl.trackAt(audio, t)->clips()) {
                cursor += clip.leadInSec;
                if (clip.displayName == name) return cursor;
                cursor += clip.effectiveDuration();
            }
        }
        return -1.0;
    };
    Timeline connectedTimeline;
    auto connectedApplied = connectedMix;
    QStringList connectedWarnings;
    const bool connectedOk = fcpx::xml::applyImport(connectedTimeline, connectedApplied, &connectedWarnings);
    connectedTimeline.setStorylineMode(true);
    connectedTimeline.undoManager()->clear();
    connectedTimeline.saveUndoState(QStringLiteral("基準")); // reconcile baseline
    const double videoBefore = liveStart(connectedTimeline, false, QStringLiteral("toMixC"));
    const double audioBefore = liveStart(connectedTimeline, true, QStringLiteral("toMixC"));
    const double hostBefore = liveStart(connectedTimeline, false, QStringLiteral("host"));
    connectedTimeline.trackAt(false, 0)->setSelectedClip(0);
    connectedTimeline.deleteSelectedClip();
    const double videoAfter = liveStart(connectedTimeline, false, QStringLiteral("toMixC"));
    const double audioAfter = liveStart(connectedTimeline, true, QStringLiteral("toMixC"));
    const double hostAfter = liveStart(connectedTimeline, false, QStringLiteral("host"));
    const bool connectedFollows = connectedOk && near(hostBefore, 2.0) && near(hostAfter, 0.0)
        && near(videoBefore, 5.0) && near(audioBefore, 5.0) && near(videoAfter, 3.0) && near(audioAfter, videoAfter);
    std::fprintf(stderr, "G26 connected mixed parse error=%s hostUid=%s videoAnchor=%s audioAnchor=%s "
        "videoOffset=%.9f audioOffset=%.9f audioUid=%s | apply ok=%d warnings=%d storyline Delete of lead: "
        "host %.6f->%.6f (expect 2->0) videoRef %.6f->%.6f (expect 5->3) audioRef %.6f->%.6f (expect 5->3) | "
        "audio host error=%s detachWarnings=%d (expect 1) videoRef=%d audioRef=%d anchors=%s|%s\n",
        connectedMix.error.toUtf8().constData(), hostUid.toUtf8().constData(),
        cVideoRef ? cVideoRef->anchorUid.toUtf8().constData() : "",
        cAudioRef ? cAudioRef->anchorUid.toUtf8().constData() : "",
        cVideoRef ? cVideoRef->anchorOffsetSec : -99.0, cAudioRef ? cAudioRef->anchorOffsetSec : -99.0,
        cAudioRef ? cAudioRef->uid.toUtf8().constData() : "", int(connectedOk), int(connectedWarnings.size()),
        hostBefore, hostAfter, videoBefore, videoAfter, audioBefore, audioAfter,
        audioHostMix.error.toUtf8().constData(), audioHostWarnings, int(dVideoRef != nullptr), int(dAudioRef != nullptr),
        dVideoRef ? dVideoRef->anchorUid.toUtf8().constData() : "", dAudioRef ? dAudioRef->anchorUid.toUtf8().constData() : "");
    gate(26, twoParsed && twoApply && mixParsed && mixApply && videoOnlyOk && connectedParsed && connectedFollows
        && audioHostOk);

    // G27 (negative controls): a cycle, an undefined media, and depth 9 (depth 8 still reads).
    const auto cycle = fcpx::xml::parseXml(document(
        QStringLiteral("<ref-clip ref=\"A\" name=\"top\" duration=\"2s\"/>"),
        QStringLiteral("<media id=\"A\" name=\"A\"><sequence><spine>"
            "<asset-clip ref=\"a\" name=\"inA\" duration=\"1s\"/>"
            "<ref-clip ref=\"B\" name=\"AtoB\" offset=\"1s\" duration=\"1s\"/></spine></sequence></media>"
            "<media id=\"B\" name=\"B\"><sequence><spine>"
            "<asset-clip ref=\"a\" name=\"inB\" duration=\"1s\"/>"
            "<ref-clip ref=\"A\" name=\"BtoA\" offset=\"1s\" duration=\"1s\"/></spine></sequence></media>")), dir.path());
    const QString cycleWarning = QStringLiteral("compound clip の循環参照を読み飛ばしました: A");
    const TimelineSequence *cycleB = cycle.sequences.size() == 2 ? &cycle.sequences[0] : nullptr;
    const TimelineSequence *cycleA = cycle.sequences.size() == 2 ? &cycle.sequences[1] : nullptr;
    const bool cycleOk = cycle.error.isEmpty() && cycle.warnings.count(cycleWarning) == 1
        && cycleB && cycleA && cycleB->id == QStringLiteral("B") && cycleA->id == QStringLiteral("A")
        && clipNames(cycleB) == QByteArray("inB>") && clipNames(cycleA) == QByteArray("inA>,AtoB>B")
        && namedClip(cycle.videoTracks, QStringLiteral("top"));
    const auto undefinedRef = fcpx::xml::parseXml(document(QStringLiteral(
        "<asset-clip ref=\"a\" name=\"real\" duration=\"1s\"/>"
        "<ref-clip ref=\"nothing\" name=\"ghost\" offset=\"1s\" duration=\"1s\"/>")), dir.path());
    const QString undefinedWarning = QStringLiteral("未定義の素材参照を読み飛ばしました: nothing");
    const bool undefinedOk = undefinedRef.error.isEmpty() && undefinedRef.warnings.count(undefinedWarning) == 1
        && undefinedRef.sequences.isEmpty() && !namedClip(undefinedRef.videoTracks, QStringLiteral("ghost"))
        && namedClip(undefinedRef.videoTracks, QStringLiteral("real"));
    auto chain = [&](int levels, const QString &spine = QStringLiteral("<ref-clip ref=\"m1\" name=\"top\" duration=\"1s\"/>")) {
        QString media;
        for (int i = 1; i <= levels; ++i) {
            const QString body = i < levels
                ? QStringLiteral("<ref-clip ref=\"m%1\" name=\"to%1\" duration=\"1s\"/>").arg(i + 1)
                : QStringLiteral("<asset-clip ref=\"a\" name=\"leaf\" duration=\"1s\"/>");
            media += QStringLiteral("<media id=\"m%1\" name=\"m%1\"><sequence><spine>%2</spine></sequence></media>")
                .arg(i).arg(body);
        }
        return fcpx::xml::parseXml(document(spine, media), dir.path());
    };
    const auto depth8 = chain(8), depth9 = chain(9);
    const QString depthWarning = QStringLiteral("compound clip の入れ子が深さ 8 を超えたため読み飛ばしました: m9");
    auto hasId = [](const fcpx::xml::ImportResult &r, const QString &id) {
        for (const auto &sequence : r.sequences) if (sequence.id == id) return true;
        return false;
    };
    const bool depthOk = depth8.error.isEmpty() && depth8.sequences.size() == 8
        && depth8.warnings.filter(QStringLiteral("深さ")).isEmpty()
        && depth8.sequences[0].id == QStringLiteral("m8") && clipNames(&depth8.sequences[0]) == QByteArray("leaf>")
        && depth9.error.isEmpty() && depth9.sequences.size() == 8 && depth9.warnings.count(depthWarning) == 1
        && !hasId(depth9, QStringLiteral("m9")) && depth9.sequences[0].id == QStringLiteral("m8")
        && clipNames(&depth9.sequences[0]).isEmpty();
    // Document order: m9 read first from the root still skips m8's reference
    // at depth 9. m8 referenced from the root as well is still at depth 8 on
    // the chain (the renderer would drop m9 there), so its reference is skipped
    // and m9, referenced from nowhere else, is not read.
    const auto m9First = chain(9, QStringLiteral("<ref-clip ref=\"m9\" name=\"top9\" duration=\"1s\"/>"
        "<ref-clip ref=\"m1\" name=\"top\" offset=\"1s\" duration=\"1s\"/>"));
    const auto m8Also = chain(9, QStringLiteral("<ref-clip ref=\"m1\" name=\"top\" duration=\"1s\"/>"
        "<ref-clip ref=\"m8\" name=\"top8\" offset=\"1s\" duration=\"1s\"/>"));
    auto indexOf = [](const fcpx::xml::ImportResult &r, const QString &id) {
        for (int i = 0; i < r.sequences.size(); ++i) if (r.sequences[i].id == id) return i;
        return -1;
    };
    auto namesOf = [&](const fcpx::xml::ImportResult &r, const QString &id) {
        const int i = indexOf(r, id);
        return i < 0 ? QByteArray("-") : clipNames(&r.sequences[i]);
    };
    const bool orderOk = m9First.error.isEmpty() && m9First.warnings.count(depthWarning) == 1
        && m9First.sequences.size() == 9 && hasId(m9First, QStringLiteral("m9"))
        && namesOf(m9First, QStringLiteral("m8")).isEmpty()
        && m8Also.error.isEmpty() && m8Also.sequences.size() == 8 && m8Also.warnings.count(depthWarning) == 1
        && m8Also.warnings.filter(QStringLiteral("深さ")).size() == 1
        && namesOf(m8Also, QStringLiteral("m8")).isEmpty() && indexOf(m8Also, QStringLiteral("m9")) < 0
        && namesOf(m8Also, QStringLiteral("m7")) == QByteArray("to8>m8");
    // Every media of a 40-chain referenced from the root as well: the chain
    // from m1 reaches m8 at depth 8 and skips m9, which the root then reads
    // at depth 1, so the chain restarts there and is cut every 8 media.
    QString allRoot;
    for (int i = 1; i <= 40; ++i)
        allRoot += QStringLiteral("<ref-clip ref=\"m%1\" name=\"top%1\" offset=\"%2s\" duration=\"1s\"/>").arg(i).arg(i - 1);
    const auto longChain = chain(40, allRoot);
    QStringList chainWarnings;
    for (int cut : {9, 17, 25, 33})
        chainWarnings.append(QStringLiteral("compound clip の入れ子が深さ 8 を超えたため読み飛ばしました: m%1").arg(cut));
    const bool chainOk = longChain.error.isEmpty() && longChain.sequences.size() == 40
        && longChain.warnings.filter(QStringLiteral("深さ")) == chainWarnings
        && namesOf(longChain, QStringLiteral("m7")) == QByteArray("to8>m8")
        && namesOf(longChain, QStringLiteral("m8")).isEmpty()
        && namesOf(longChain, QStringLiteral("m15")) == QByteArray("to16>m16")
        && namesOf(longChain, QStringLiteral("m16")).isEmpty()
        && namesOf(longChain, QStringLiteral("m31")) == QByteArray("to32>m32")
        && namesOf(longChain, QStringLiteral("m32")).isEmpty()
        && namesOf(longChain, QStringLiteral("m40")) == QByteArray("leaf>");
    // The longest chain of sequence references from the root tracks (a
    // sequence on the root is 1, a cycle is unbounded) must stay within 8,
    // the renderer's nesting limit.
    auto longestRefPath = [&](const fcpx::xml::ImportResult &r) {
        QMap<QString, int> memo;
        QSet<QString> visiting;
        std::function<int(const QString &)> pathFrom;
        auto deepest = [&](const QVector<QVector<ClipInfo>> &video, const QVector<QVector<ClipInfo>> &audio) {
            int best = 0;
            for (const auto *tracks : {&video, &audio})
                for (const auto &track : *tracks)
                    for (const auto &clip : track)
                        if (!clip.sequenceRefId.isEmpty()) best = qMax(best, 1 + pathFrom(clip.sequenceRefId));
            return best;
        };
        pathFrom = [&](const QString &id) -> int {
            if (memo.contains(id)) return memo.value(id);
            const int i = indexOf(r, id);
            if (visiting.contains(id)) return 1000;
            if (i < 0) return 0;
            visiting.insert(id);
            const int depth = deepest(r.sequences[i].videoTracks, r.sequences[i].audioTracks);
            visiting.remove(id);
            memo.insert(id, depth);
            return depth;
        };
        return deepest(r.videoTracks, r.audioTracks);
    };
    const int depth8Path = longestRefPath(depth8), depth9Path = longestRefPath(depth9);
    const int m9FirstPath = longestRefPath(m9First), m8AlsoPath = longestRefPath(m8Also);
    const int longChainPath = longestRefPath(longChain);
    const bool pathsOk = depth8Path == 8 && depth9Path == 8 && m9FirstPath == 8 && m8AlsoPath == 8
        && longChainPath == 8;
    std::fprintf(stderr, "G27 cycle error=%s cycleWarning=%d sequences=%d B=%s A=%s | undefined warning=%d "
        "sequences=%d ghost=%d real=%d | depth8 sequences=%d depthWarnings=%d innermost=%s(%s) | "
        "depth9 sequences=%d depthWarning=%d m9=%d innermost=%s(%s) error=%s | "
        "m9First sequences=%d depthWarning=%d m9=%d m8=%s error=%s | "
        "m8Also sequences=%d depthWarning=%d m9Index=%d m8Index=%d m7=%s m8=%s error=%s | "
        "longChain sequences=%d depthWarnings=%s m8=%s m16=%s m31=%s m32=%s m40=%s error=%s | "
        "longestRefPath depth8=%d depth9=%d m9First=%d m8Also=%d longChain=%d (limit 8)\n",
        cycle.error.toUtf8().constData(), int(cycle.warnings.count(cycleWarning)), int(cycle.sequences.size()),
        clipNames(cycleB).constData(), clipNames(cycleA).constData(),
        int(undefinedRef.warnings.count(undefinedWarning)), int(undefinedRef.sequences.size()),
        int(namedClip(undefinedRef.videoTracks, QStringLiteral("ghost")) != nullptr),
        int(namedClip(undefinedRef.videoTracks, QStringLiteral("real")) != nullptr),
        int(depth8.sequences.size()), int(depth8.warnings.filter(QStringLiteral("深さ")).size()),
        depth8.sequences.isEmpty() ? "" : depth8.sequences[0].id.toUtf8().constData(),
        depth8.sequences.isEmpty() ? "" : clipNames(&depth8.sequences[0]).constData(),
        int(depth9.sequences.size()), int(depth9.warnings.count(depthWarning)), int(hasId(depth9, QStringLiteral("m9"))),
        depth9.sequences.isEmpty() ? "" : depth9.sequences[0].id.toUtf8().constData(),
        depth9.sequences.isEmpty() ? "" : clipNames(&depth9.sequences[0]).constData(),
        depth9.error.toUtf8().constData(),
        int(m9First.sequences.size()), int(m9First.warnings.count(depthWarning)), int(hasId(m9First, QStringLiteral("m9"))),
        namesOf(m9First, QStringLiteral("m8")).constData(), m9First.error.toUtf8().constData(),
        int(m8Also.sequences.size()), int(m8Also.warnings.count(depthWarning)),
        indexOf(m8Also, QStringLiteral("m9")), indexOf(m8Also, QStringLiteral("m8")),
        namesOf(m8Also, QStringLiteral("m7")).constData(),
        namesOf(m8Also, QStringLiteral("m8")).constData(), m8Also.error.toUtf8().constData(),
        int(longChain.sequences.size()),
        longChain.warnings.filter(QStringLiteral("深さ")).join(QLatin1Char('|')).toUtf8().constData(),
        namesOf(longChain, QStringLiteral("m8")).constData(), namesOf(longChain, QStringLiteral("m16")).constData(),
        namesOf(longChain, QStringLiteral("m31")).constData(), namesOf(longChain, QStringLiteral("m32")).constData(),
        namesOf(longChain, QStringLiteral("m40")).constData(), longChain.error.toUtf8().constData(),
        depth8Path, depth9Path, m9FirstPath, m8AlsoPath, longChainPath);
    gate(27, cycleOk && undefinedOk && depthOk && orderOk && chainOk && pathsOk);

    // G28: the same ImportResult applied twice. The first nested sequence,
    // edited after the import, must survive; the second gets its own id.
    Timeline repeatTimeline;
    auto repeated = single;
    QStringList repeatWarnings;
    const bool firstOk = fcpx::xml::applyImport(repeatTimeline, repeated, &repeatWarnings);
    const QString firstId = repeated.sequences.value(0).id;
    auto edited = repeatTimeline.sequences();
    bool editedOk = false;
    for (auto &sequence : edited) {
        if (sequence.id != firstId || sequence.videoTracks.isEmpty() || sequence.videoTracks[0].isEmpty()) continue;
        sequence.videoTracks[0][0].displayName = QStringLiteral("編集済み");
        editedOk = true;
    }
    repeatTimeline.setSequences(edited, repeatTimeline.activeSequenceId());
    const int beforeSecond = repeatTimeline.sequences().size();
    const bool secondOk = fcpx::xml::applyImport(repeatTimeline, repeated, &repeatWarnings);
    const QString secondId = repeated.sequences.value(0).id;
    const auto repeatStore = repeatTimeline.sequences();
    const TimelineSequence *firstStored = findSequence(repeatStore, firstId);
    const TimelineSequence *secondStored = findSequence(repeatStore, secondId);
    const TimelineSequence *repeatActive = findSequence(repeatStore, repeatTimeline.activeSequenceId());
    const ClipInfo *repeatRef = repeatActive ? namedClip(repeatActive->videoTracks, QStringLiteral("compound")) : nullptr;
    const auto playback = repeatTimeline.computePlaybackSequence();
    const bool repeatOk = firstOk && secondOk && editedOk && firstId == QStringLiteral("cmp")
        && secondId != firstId && !secondId.isEmpty() && repeatStore.size() == beforeSecond + 2
        && clipNames(firstStored) == QByteArray("編集済み>,inner2>") && clipNames(secondStored) == QByteArray("inner1>,inner2>")
        && repeatRef && repeatRef->sequenceRefId == secondId
        && repeatRef->filePath == timeline_nesting::sequenceClipFilePath(secondId) && !playback.isEmpty();
    // Recorded, not gated: the store after one undo (measured: back to the
    // store before the import) is logged.
    auto idList = [](const QVector<TimelineSequence> &sequences) {
        QStringList ids;
        for (const auto &sequence : sequences) ids.append(sequence.id);
        return ids.join(QLatin1Char(',')).toUtf8();
    };
    const int beforeUndo = repeatStore.size();
    repeatTimeline.undo();
    const auto afterUndoStore = repeatTimeline.sequences();
    const QByteArray freshBeforeUndo = idList(compoundTimeline.sequences());
    compoundTimeline.undo();
    const QByteArray freshAfterUndo = idList(compoundTimeline.sequences());
    std::fprintf(stderr, "G28 first=%d id=%s second=%d id=%s storeBeforeSecond=%d storeAfterSecond=%d "
        "firstClips=%s secondClips=%s activeRef=%s playbackEntries=%d\n",
        int(firstOk), firstId.toUtf8().constData(), int(secondOk), secondId.toUtf8().constData(), beforeSecond,
        beforeUndo, clipNames(firstStored).constData(), clipNames(secondStored).constData(),
        repeatRef ? repeatRef->sequenceRefId.toUtf8().constData() : "", int(playback.size()));
    std::fprintf(stderr, "G28 undo once after the second import: sequences().size() %d -> %d (%s -> %s), "
        "first nested clips after undo=%s | undo once after an import into a new Timeline: %s -> %s (pre-import: none)\n",
        beforeUndo, int(afterUndoStore.size()), idList(repeatStore).constData(), idList(afterUndoStore).constData(),
        clipNames(findSequence(afterUndoStore, firstId)).constData(), freshBeforeUndo.constData(),
        freshAfterUndo.constData());
    gate(28, repeatOk);

    // G29-G31: mc-clip reads the spine of its active angle (mc-source srcEnable).
    const QString multicamAssets = QStringLiteral("<asset id=\"b\" src=\"offline-b.mov\"/>");
    auto multicamMedia = [&](const QString &angle1, const QString &angle2) {
        return multicamAssets + QStringLiteral("<media id=\"m\" name=\"MC\"><multicam>"
            "<mc-angle name=\"アングル1\" angleID=\"a1\">%1</mc-angle>"
            "<mc-angle name=\"アングル2\" angleID=\"a2\">%2</mc-angle></multicam></media>").arg(angle1, angle2);
    };
    const QString singleAngle1 = QStringLiteral("<asset-clip ref=\"a\" name=\"A1clip\" offset=\"0s\" duration=\"10s\"/>");
    const QString singleAngle2 = QStringLiteral("<asset-clip ref=\"b\" name=\"B2clip\" offset=\"0s\" duration=\"10s\"/>");
    const QString mcSoundAsset = QStringLiteral("<asset id=\"snd\" src=\"voice.wav\" hasVideo=\"0\" hasAudio=\"1\"/>");
    const QString mcAngle1Warning = QStringLiteral("mc-clip MCC の未使用アングルを読み飛ばしました: アングル1");
    const QString mcAngle2Warning = QStringLiteral("mc-clip MCC の未使用アングルを読み飛ばしました: アングル2");

    // G29: srcEnable=all on angle 2 -> V1 holds angle 2's media; angle 1 is reported by name.
    const auto mcSecond = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" start=\"2s\" duration=\"5s\">"
        "<mc-source angleID=\"a2\" srcEnable=\"all\"/></mc-clip>"),
        multicamMedia(singleAngle1, singleAngle2)), dir.path());
    const ClipInfo *mcSecondClip = mcSecond.videoTracks.size() == 1 && mcSecond.videoTracks[0].size() == 1
        ? &mcSecond.videoTracks[0][0] : nullptr;
    const bool mcSecondOk = mcSecond.error.isEmpty() && mcSecondClip
        && mcSecondClip->displayName == QStringLiteral("B2clip")
        && mcSecondClip->filePath.endsWith(QStringLiteral("offline-b.mov"))
        && near(mcSecondClip->inPoint, 2.0) && near(mcSecondClip->outPoint, 7.0) && near(mcSecondClip->leadInSec, 0.0)
        && mcSecond.warnings.count(mcAngle1Warning) == 1 && !mcSecond.warnings.contains(mcAngle2Warning)
        && !mcSecond.warnings.join('\n').contains(QStringLiteral(": mc-clip ("));
    std::fprintf(stderr, "G29 error=%s clips=%d name=%s in=%.9f out=%.9f lead=%.9f unusedAngle1Warning=%d "
        "unusedAngle2Warning=%d skipWarning=%d\n",
        mcSecond.error.toUtf8().constData(), mcSecond.videoTracks.isEmpty() ? -1 : int(mcSecond.videoTracks[0].size()),
        mcSecondClip ? mcSecondClip->displayName.toUtf8().constData() : "",
        mcSecondClip ? mcSecondClip->inPoint : -1.0, mcSecondClip ? mcSecondClip->outPoint : -1.0,
        mcSecondClip ? mcSecondClip->leadInSec : -1.0, int(mcSecond.warnings.count(mcAngle1Warning)),
        int(mcSecond.warnings.count(mcAngle2Warning)),
        int(mcSecond.warnings.join('\n').contains(QStringLiteral(": mc-clip ("))));
    // A spine-level lane != 0 is not placed on V1: the mc-clip is skipped with a warning.
    const QString laneDocument = document(QStringLiteral(
        "\n\n<mc-clip ref=\"m\" name=\"MCC\" lane=\"1\" offset=\"0s\" duration=\"4s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"all\"/></mc-clip>"), multicamMedia(singleAngle1, singleAngle2));
    const auto mcLane = fcpx::xml::parseXml(laneDocument, dir.path());
    const int laneLine = laneDocument.left(laneDocument.indexOf(QStringLiteral("<mc-clip"))).count(QLatin1Char('\n')) + 1;
    const QString laneWarning = QStringLiteral("未対応の要素を読み飛ばしました: mc-clip (行 %1)").arg(laneLine);
    int laneClips = 0;
    for (const auto &track : mcLane.videoTracks) laneClips += int(track.size());
    for (const auto &track : mcLane.audioTracks) laneClips += int(track.size());
    const bool laneOk = mcLane.error.isEmpty() && laneClips == 0 && mcLane.warnings.count(laneWarning) == 1
        && mcLane.warnings == QStringList{laneWarning, QStringLiteral("読み込めるクリップがありません。")};
    std::fprintf(stderr, "G29 lane=1 mc-clip: clips=%d warnings=%d firstWarning=%s expectedLine=%d\n", laneClips,
        int(mcLane.warnings.size()), mcLane.warnings.join(" | ").toUtf8().constData(), laneLine);
    gate(29, mcSecondOk && laneOk);

    // G30: the active angle is a spine of two clips and a gap. The range [1s, 7s)
    // cuts both clips and keeps the gap as lead-in.
    const auto mcSpine = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"10s\" start=\"1s\" duration=\"6s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"all\"/></mc-clip>"),
        multicamMedia(QStringLiteral(
            "<asset-clip ref=\"a\" name=\"first\" offset=\"0s\" duration=\"3s\"/><gap offset=\"3s\" duration=\"2s\"/>"
            "<asset-clip ref=\"a\" name=\"second\" offset=\"5s\" start=\"10s\" duration=\"3s\"/>"), singleAngle2)),
        dir.path());
    const bool mcSpineShape = mcSpine.error.isEmpty() && mcSpine.videoTracks.size() == 1 && mcSpine.videoTracks[0].size() == 2;
    const ClipInfo *mcFirst = mcSpineShape ? &mcSpine.videoTracks[0][0] : nullptr;
    const ClipInfo *mcSecondSpine = mcSpineShape ? &mcSpine.videoTracks[0][1] : nullptr;
    const bool mcSpineOk = mcSpineShape
        && mcFirst->displayName == QStringLiteral("first") && mcSecondSpine->displayName == QStringLiteral("second")
        && near(mcFirst->leadInSec, 10.0) && near(mcFirst->inPoint, 1.0) && near(mcFirst->outPoint, 3.0)
        && near(mcSecondSpine->leadInSec, 2.0) && near(mcSecondSpine->inPoint, 10.0) && near(mcSecondSpine->outPoint, 12.0)
        && mcSpine.warnings.count(mcAngle2Warning) == 1;
    std::fprintf(stderr, "G30 error=%s clips=%d first(lead=%.9f in=%.9f out=%.9f) second(lead=%.9f in=%.9f out=%.9f) "
        "expected first(10,1,3) second(2,10,12)\n",
        mcSpine.error.toUtf8().constData(), mcSpine.videoTracks.isEmpty() ? -1 : int(mcSpine.videoTracks[0].size()),
        mcFirst ? mcFirst->leadInSec : -1.0, mcFirst ? mcFirst->inPoint : -1.0, mcFirst ? mcFirst->outPoint : -1.0,
        mcSecondSpine ? mcSecondSpine->leadInSec : -1.0, mcSecondSpine ? mcSecondSpine->inPoint : -1.0,
        mcSecondSpine ? mcSecondSpine->outPoint : -1.0);
    // A transition after a two-clip mc-clip belongs to the LAST cut clip (fade-out there, none on the first).
    const auto mcTail = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"6s\"><mc-source angleID=\"a1\" srcEnable=\"all\"/></mc-clip>"
        "<transition duration=\"1s\"/>"),
        multicamMedia(QStringLiteral(
            "<asset-clip ref=\"a\" name=\"first\" offset=\"0s\" duration=\"3s\"/>"
            "<asset-clip ref=\"a\" name=\"second\" offset=\"3s\" duration=\"3s\"/>"), singleAngle2)), dir.path());
    const bool mcTailShape = mcTail.error.isEmpty() && mcTail.videoTracks.size() == 1 && mcTail.videoTracks[0].size() == 2;
    const ClipInfo *tailFirst = mcTailShape ? &mcTail.videoTracks[0][0] : nullptr;
    const ClipInfo *tailSecond = mcTailShape ? &mcTail.videoTracks[0][1] : nullptr;
    const bool tailOk = tailFirst && tailSecond && tailSecond->trailOut.type == TransitionType::FadeOut
        && near(tailSecond->trailOut.duration, 1.0) && tailFirst->trailOut.type == TransitionType::None;
    std::fprintf(stderr, "G30 tail transition: secondTrailOut=%d/%.9f (FadeOut=%d) firstTrailOut=%d (None=%d) warnings=%d\n",
        tailSecond ? int(tailSecond->trailOut.type) : -1, tailSecond ? tailSecond->trailOut.duration : -1.0,
        int(TransitionType::FadeOut), tailFirst ? int(tailFirst->trailOut.type) : -1, int(TransitionType::None),
        int(mcTail.warnings.size()));
    // A connected clip whose parent lies outside the cut range keeps the part inside it
    // (video [0,2) with audio [0,6), range [3,5)).
    const auto mcOutParent = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"20s\" start=\"3s\" duration=\"2s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"all\"/></mc-clip>"),
        mcSoundAsset + multicamMedia(QStringLiteral(
            "<asset-clip ref=\"a\" name=\"head\" offset=\"0s\" duration=\"2s\">"
            "<asset-clip ref=\"snd\" name=\"longAudio\" lane=\"-1\" offset=\"0s\" duration=\"6s\"/></asset-clip>"
            "<asset-clip ref=\"a\" name=\"tail\" offset=\"2s\" duration=\"4s\"/>"), singleAngle2)), dir.path());
    const ClipInfo *outParentAudio = mcOutParent.audioTracks.size() == 1 && mcOutParent.audioTracks[0].size() == 1
        ? &mcOutParent.audioTracks[0][0] : nullptr;
    const bool outParentOk = mcOutParent.error.isEmpty() && outParentAudio
        && outParentAudio->displayName == QStringLiteral("longAudio")
        && near(outParentAudio->inPoint, 3.0) && near(outParentAudio->outPoint, 5.0)
        && near(outParentAudio->leadInSec, 20.0);
    std::fprintf(stderr, "G30 out-of-range parent: error=%s audioClips=%d in=%.9f out=%.9f lead=%.9f expected (3,5,20)\n",
        mcOutParent.error.toUtf8().constData(), mcOutParent.audioTracks.isEmpty() ? -1 : int(mcOutParent.audioTracks[0].size()),
        outParentAudio ? outParentAudio->inPoint : -1.0, outParentAudio ? outParentAudio->outPoint : -1.0,
        outParentAudio ? outParentAudio->leadInSec : -1.0);
    // A clip connected to a gap in the angle spine stays, unconnected, like at top level.
    const auto mcGapChild = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"6s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"all\"/></mc-clip>"),
        mcSoundAsset + multicamMedia(QStringLiteral(
            "<asset-clip ref=\"a\" name=\"first\" offset=\"0s\" duration=\"3s\"/>"
            "<gap offset=\"3s\" duration=\"3s\">"
            "<asset-clip ref=\"snd\" name=\"gapAudio\" lane=\"-1\" offset=\"0s\" duration=\"2s\"/></gap>"), singleAngle2)),
        dir.path());
    QStringList gapAudioNames;
    for (const auto &track : mcGapChild.audioTracks) for (const auto &c : track) gapAudioNames.append(c.displayName);
    const bool gapChildOk = mcGapChild.error.isEmpty() && gapAudioNames == QStringList{QStringLiteral("gapAudio")};
    std::fprintf(stderr, "G30 gap child: error=%s audio=[%s]\n", mcGapChild.error.toUtf8().constData(),
        gapAudioNames.join(',').toUtf8().constData());
    // A transition inside the angle spine is reported once; the mcTail transition (outside the mc-clip) is not.
    const QString angleTransitionWarning = QStringLiteral("mc-angle 内の transition を読み飛ばしました (1 件)");
    const auto mcAngleTransition = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"6s\"><mc-source angleID=\"a1\" srcEnable=\"all\"/></mc-clip>"),
        multicamMedia(QStringLiteral(
            "<asset-clip ref=\"a\" name=\"first\" offset=\"0s\" duration=\"3s\"/>"
            "<transition duration=\"1s\"/>"
            "<asset-clip ref=\"a\" name=\"second\" offset=\"3s\" duration=\"3s\"/>"), singleAngle2)), dir.path());
    const bool angleTransitionOk = mcAngleTransition.error.isEmpty()
        && mcAngleTransition.warnings.count(angleTransitionWarning) == 1 && !mcTail.warnings.contains(angleTransitionWarning);
    std::fprintf(stderr, "G30 angle transition warning: count=%d tailCount=%d\n",
        int(mcAngleTransition.warnings.count(angleTransitionWarning)), int(mcTail.warnings.count(angleTransitionWarning)));
    gate(30, mcSpineOk && tailOk && outParentOk && gapChildOk && angleTransitionOk);

    // G31: negative controls. (1) an undefined angleID is skipped with a warning
    // (and the first angle is used); (2) no mc-source -> the first angle with one
    // warning; (3) different video and audio angles -> V1 plus a connected A1,
    // and no unused-angle warning because both angles are used.
    const QString mcUndefinedWarning = QStringLiteral("mc-clip MCC の未定義のアングルを読み飛ばしました: zz");
    const QString mcFallbackWarning = QStringLiteral("mc-clip MCC に有効アングルが無いため最初のアングル アングル1 を使いました");
    const auto mcMedia = multicamMedia(singleAngle1, singleAngle2);
    const auto mcUndefined = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"4s\"><mc-source angleID=\"zz\" srcEnable=\"all\"/></mc-clip>"),
        mcMedia), dir.path());
    const auto mcNoSource = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"4s\"/>"), mcMedia), dir.path());
    const auto mcSplit = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"4s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"video\"/><mc-source angleID=\"a2\" srcEnable=\"audio\"/></mc-clip>"),
        mcMedia), dir.path());
    auto firstVideoName = [](const fcpx::xml::ImportResult &r) {
        return r.videoTracks.isEmpty() || r.videoTracks[0].isEmpty() ? QString() : r.videoTracks[0][0].displayName;
    };
    const bool mcUndefinedOk = mcUndefined.error.isEmpty() && mcUndefined.warnings.count(mcUndefinedWarning) == 1
        && mcUndefined.warnings.count(mcFallbackWarning) == 1 && firstVideoName(mcUndefined) == QStringLiteral("A1clip");
    const bool noSourceOk = mcNoSource.error.isEmpty() && mcNoSource.warnings.count(mcFallbackWarning) == 1
        && !mcNoSource.warnings.contains(mcUndefinedWarning) && firstVideoName(mcNoSource) == QStringLiteral("A1clip")
        && mcNoSource.audioTracks.size() == 1 && mcNoSource.audioTracks[0].isEmpty();
    const ClipInfo *splitVideo = mcSplit.videoTracks.size() == 1 && mcSplit.videoTracks[0].size() == 1
        ? &mcSplit.videoTracks[0][0] : nullptr;
    const ClipInfo *splitAudio = mcSplit.audioTracks.size() == 1 && mcSplit.audioTracks[0].size() == 1
        ? &mcSplit.audioTracks[0][0] : nullptr;
    const bool splitOk = mcSplit.error.isEmpty() && splitVideo && splitAudio
        && splitVideo->displayName == QStringLiteral("A1clip") && !splitVideo->uid.isEmpty()
        && splitAudio->displayName == QStringLiteral("B2clip") && splitAudio->filePath.endsWith(QStringLiteral("offline-b.mov"))
        && splitAudio->anchorUid == splitVideo->uid && near(splitAudio->anchorOffsetSec, 0.0)
        && near(splitAudio->inPoint, 0.0) && near(splitAudio->outPoint, 4.0)
        && !mcSplit.warnings.contains(mcAngle1Warning) && !mcSplit.warnings.contains(mcAngle2Warning)
        && !mcSplit.warnings.contains(mcFallbackWarning);
    std::fprintf(stderr, "G31 undefined(warn=%d fallbackWarn=%d video=%s) noSource(fallbackWarn=%d video=%s audioClips=%d) "
        "split(videoClips=%d audioClips=%d audioName=%s anchorMatchesVideoUid=%d anchorOffset=%.9f unusedWarn=%d)\n",
        int(mcUndefined.warnings.count(mcUndefinedWarning)), int(mcUndefined.warnings.count(mcFallbackWarning)),
        firstVideoName(mcUndefined).toUtf8().constData(), int(mcNoSource.warnings.count(mcFallbackWarning)),
        firstVideoName(mcNoSource).toUtf8().constData(),
        mcNoSource.audioTracks.isEmpty() ? -1 : int(mcNoSource.audioTracks[0].size()),
        mcSplit.videoTracks.isEmpty() ? -1 : int(mcSplit.videoTracks[0].size()),
        mcSplit.audioTracks.isEmpty() ? -1 : int(mcSplit.audioTracks[0].size()),
        splitAudio ? splitAudio->displayName.toUtf8().constData() : "",
        int(splitAudio && splitVideo && splitAudio->anchorUid == splitVideo->uid && !splitVideo->uid.isEmpty()),
        splitAudio ? splitAudio->anchorOffsetSec : -1.0,
        int(mcSplit.warnings.contains(mcAngle1Warning) || mcSplit.warnings.contains(mcAngle2Warning)));
    // Different video and audio angles where the audio angle is a gap with a lane child at
    // offset 1s: the audio clip must be connected to the first video clip at offset 1s.
    const auto mcGapAudio = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"4s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"video\"/><mc-source angleID=\"a2\" srcEnable=\"audio\"/></mc-clip>"),
        mcSoundAsset + multicamMedia(singleAngle1, QStringLiteral(
            "<gap offset=\"0s\" duration=\"4s\"><asset-clip ref=\"snd\" name=\"gapAudioA2\" lane=\"-1\" offset=\"1s\" duration=\"2s\"/></gap>"))),
        dir.path());
    int gapAudioCount = 0;
    const ClipInfo *gapAudio = nullptr;
    for (const auto &track : mcGapAudio.audioTracks) for (const auto &c : track) { ++gapAudioCount; gapAudio = &c; }
    const ClipInfo *gapVideo = mcGapAudio.videoTracks.size() == 1 && mcGapAudio.videoTracks[0].size() == 1
        ? &mcGapAudio.videoTracks[0][0] : nullptr;
    const bool gapAudioOk = mcGapAudio.error.isEmpty() && gapAudioCount == 1 && gapAudio && gapVideo
        && gapAudio->displayName == QStringLiteral("gapAudioA2") && !gapVideo->uid.isEmpty()
        && gapAudio->anchorUid == gapVideo->uid && near(gapAudio->anchorOffsetSec, 1.0);
    std::fprintf(stderr, "G31 gap audio angle: error=[%s] audioClips=%d name=%s anchorUid=[%s] videoUid=[%s] anchorOffset=%.9f\n",
        mcGapAudio.error.toUtf8().constData(), gapAudioCount, gapAudio ? gapAudio->displayName.toUtf8().constData() : "",
        gapAudio ? gapAudio->anchorUid.toUtf8().constData() : "", gapVideo ? gapVideo->uid.toUtf8().constData() : "",
        gapAudio ? gapAudio->anchorOffsetSec : -1.0);
    // Split angles shaped as <clip> containers (video + audio children): V1 holds only a1's
    // video, the A tracks only a2's audio (no a1 audio, no a2 video as audio).
    auto containerAngle = [](const QString &prefix, const QString &ref) {
        return QStringLiteral("<clip name=\"%1clip\" offset=\"0s\" duration=\"10s\">"
            "<video ref=\"%2\" name=\"%1video\" offset=\"0s\" duration=\"10s\"/>"
            "<audio ref=\"%2\" name=\"%1audio\" lane=\"-1\" offset=\"0s\" duration=\"10s\"/></clip>").arg(prefix, ref);
    };
    const auto mcSplitClip = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"4s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"video\"/><mc-source angleID=\"a2\" srcEnable=\"audio\"/></mc-clip>"),
        multicamMedia(containerAngle(QStringLiteral("A1"), QStringLiteral("a")),
                      containerAngle(QStringLiteral("B2"), QStringLiteral("b")))), dir.path());
    QStringList splitVideoNames, splitAudioNames;
    QString splitAudioAnchor;
    for (const auto &track : mcSplitClip.videoTracks) for (const auto &c : track) splitVideoNames.append(c.displayName);
    const ClipInfo *containerVideo = mcSplitClip.videoTracks.size() == 1 && mcSplitClip.videoTracks[0].size() == 1
        ? &mcSplitClip.videoTracks[0][0] : nullptr;
    for (const auto &track : mcSplitClip.audioTracks) for (const auto &c : track) {
        splitAudioNames.append(c.displayName);
        splitAudioAnchor = c.anchorUid;
    }
    const bool containerOk = mcSplitClip.error.isEmpty() && splitVideoNames == QStringList{QStringLiteral("A1video")}
        && splitAudioNames == QStringList{QStringLiteral("B2audio")} && containerVideo && !containerVideo->uid.isEmpty()
        && splitAudioAnchor == containerVideo->uid;
    std::fprintf(stderr, "G31 container split: v1=[%s] audioTracks=[%s] anchorMatchesVideoUid=%d\n",
        splitVideoNames.join(',').toUtf8().constData(), splitAudioNames.join(',').toUtf8().constData(),
        int(containerVideo && splitAudioAnchor == containerVideo->uid));
    // A video-only angle that is a compound clip with audio must not leak that audio onto an A track.
    const QString compoundResources = mcSoundAsset + QStringLiteral(
        "<media id=\"cmpAudio\" name=\"複合\"><sequence><spine>"
        "<asset-clip ref=\"a\" name=\"cmpVideo\" offset=\"0s\" duration=\"10s\">"
        "<asset-clip ref=\"snd\" name=\"cmpSound\" lane=\"-1\" offset=\"0s\" duration=\"10s\"/>"
        "</asset-clip></spine></sequence></media>");
    const QString compoundAngle = QStringLiteral("<ref-clip ref=\"cmpAudio\" name=\"CMP\" offset=\"0s\" duration=\"10s\"/>");
    const auto mcCompoundSplit = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"4s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"video\"/><mc-source angleID=\"a2\" srcEnable=\"audio\"/></mc-clip>"),
        compoundResources + multicamMedia(compoundAngle, singleAngle2)), dir.path());
    const auto mcCompoundVideoOnly = fcpx::xml::parseXml(document(QStringLiteral(
        "<mc-clip ref=\"m\" name=\"MCC\" offset=\"0s\" duration=\"4s\">"
        "<mc-source angleID=\"a1\" srcEnable=\"video\"/></mc-clip>"),
        compoundResources + multicamMedia(compoundAngle, singleAngle2)), dir.path());
    int compoundSplitAudio = 0, compoundSplitRefAudio = 0, compoundOnlyAudio = 0;
    QString compoundSplitName, compoundSplitAnchor;
    for (const auto &track : mcCompoundSplit.audioTracks) for (const auto &c : track) {
        ++compoundSplitAudio;
        if (!c.sequenceRefId.isEmpty()) ++compoundSplitRefAudio;
        compoundSplitName = c.displayName;
        compoundSplitAnchor = c.anchorUid;
    }
    for (const auto &track : mcCompoundVideoOnly.audioTracks) compoundOnlyAudio += int(track.size());
    const ClipInfo *compoundVideo = mcCompoundSplit.videoTracks.size() == 1 && mcCompoundSplit.videoTracks[0].size() == 1
        ? &mcCompoundSplit.videoTracks[0][0] : nullptr;
    const bool compoundLeakOk = mcCompoundSplit.error.isEmpty() && compoundVideo && !compoundVideo->uid.isEmpty()
        && compoundSplitAudio == 1 && compoundSplitRefAudio == 0 && compoundSplitName == QStringLiteral("B2clip")
        && compoundSplitAnchor == compoundVideo->uid
        && mcCompoundVideoOnly.error.isEmpty() && compoundOnlyAudio == 0;
    std::fprintf(stderr, "G31 compound video-only angle: split(audioClips=%d refAudio=%d name=%s anchorMatches=%d) videoOnlyAudioClips=%d\n",
        compoundSplitAudio, compoundSplitRefAudio, compoundSplitName.toUtf8().constData(),
        int(compoundVideo && compoundSplitAnchor == compoundVideo->uid), compoundOnlyAudio);
    gate(31, mcUndefinedOk && noSourceOk && splitOk && containerOk && compoundLeakOk && gapAudioOk);

    // G32 / G33: timeline -> fcpx::buildExportClips -> buildXml -> readFcpxml. V2 holds a clip
    // connected to A (anchor offset 1.5) and a J-cut clip connected to B (anchor offset -0.5).
    auto makeClip = [](const QString &name, double in, double out, double leadIn, const QString &uid,
                       const QString &anchorUid = QString(), double anchorOffset = 0.0) {
        ClipInfo c; c.filePath = QStringLiteral("/missing/") + name + QStringLiteral(".mov");
        c.displayName = name; c.inPoint = in; c.outPoint = out; c.duration = out; c.leadInSec = leadIn;
        c.uid = uid; c.anchorUid = anchorUid; c.anchorOffsetSec = anchorOffset;
        return c;
    };
    auto exportXml = [&](const QVector<QVector<ClipInfo>> &videoTracks, bool flat) {
        Timeline exportTimeline;
        exportTimeline.createSequence(QStringLiteral("往復2"), videoTracks, QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}});
        auto entries = fcpx::buildExportClips(exportTimeline);
        if (flat) for (auto &e : entries) { e.lane = 0; e.uid.clear(); e.anchorUid.clear(); }
        return QPair<QVector<fcpx::xml::ClipEntry>, QString>(entries, fcpx::xml::buildXml(entries, fcpx::xml::ExporterConfig{}));
    };
    const QVector<QVector<ClipInfo>> connectedTracks{
        {makeClip(QStringLiteral("A"), 1.0, 5.0, 0.0, QStringLiteral("ua")),
         makeClip(QStringLiteral("B"), 1.0, 3.0, 2.0, QStringLiteral("ub"))},
        {makeClip(QStringLiteral("C"), 0.0, 2.0, 1.5, QString(), QStringLiteral("ua"), 1.5),
         makeClip(QStringLiteral("D"), 0.0, 1.0, 2.0, QString(), QStringLiteral("ub"), -0.5)}};
    const auto connectedExport = exportXml(connectedTracks, false);
    const QString connectedPath = dir.filePath(QStringLiteral("connected.fcpxml"));
    QFile connectedFile(connectedPath);
    const bool connectedWritten = connectedFile.open(QIODevice::WriteOnly) && connectedFile.write(connectedExport.second.toUtf8()) > 0;
    connectedFile.close();
    const auto connectedBack = fcpx::xml::readFcpxml(connectedPath);
    auto trackClip = [](const fcpx::xml::ImportResult &r, int track, const QString &name) -> const ClipInfo * {
        if (track >= r.videoTracks.size()) return nullptr;
        for (const auto &c : r.videoTracks[track]) if (c.displayName == name) return &c;
        return nullptr;
    };
    const ClipInfo *backA = trackClip(connectedBack, 0, QStringLiteral("A"));
    const ClipInfo *backB = trackClip(connectedBack, 0, QStringLiteral("B"));
    const ClipInfo *backC = trackClip(connectedBack, 1, QStringLiteral("C"));
    const ClipInfo *backD = trackClip(connectedBack, 1, QStringLiteral("D"));
    const bool roundtripConnectedOk = connectedWritten && connectedBack.error.isEmpty() && connectedBack.videoTracks.size() == 2
        && backA && backB && backC && backD && !backA->uid.isEmpty() && !backB->uid.isEmpty()
        && backA->uid != QStringLiteral("ua") && backB->uid != QStringLiteral("ub")
        && backC->anchorUid == backA->uid && near(backC->anchorOffsetSec, 1.5)
        && backD->anchorUid == backB->uid && near(backD->anchorOffsetSec, -0.5)
        && near(backC->leadInSec, 1.5) && near(backD->leadInSec, 2.0);
    std::fprintf(stderr, "G32 error=[%s] written=%d tracks=%d anchorC=%d offsetC=%.9f anchorD=%d offsetD=%.9f "
        "newUids=%d leadInC=%.9f leadInD=%.9f\n", connectedBack.error.toUtf8().constData(), int(connectedWritten),
        int(connectedBack.videoTracks.size()), int(backC && backA && backC->anchorUid == backA->uid),
        backC ? backC->anchorOffsetSec : -99.0, int(backD && backB && backD->anchorUid == backB->uid),
        backD ? backD->anchorOffsetSec : -99.0,
        int(backA && backB && backA->uid != QStringLiteral("ua") && backB->uid != QStringLiteral("ub")),
        backC ? backC->leadInSec : -99.0, backD ? backD->leadInSec : -99.0);
    // Control: the legacy flat shape (every clip on the spine, no lanes) loses the connections.
    const auto flatConnected = fcpx::xml::parseXml(exportXml(connectedTracks, true).second, dir.path());
    int flatAnchors = 0;
    for (const auto &track : flatConnected.videoTracks) for (const auto &c : track) if (!c.anchorUid.isEmpty()) ++flatAnchors;
    std::fprintf(stderr, "G32 flatControl anchors=%d tracks=%d\n", flatAnchors, int(flatConnected.videoTracks.size()));
    // Variant: anchor B has in-point 0, so D's relative position (0 + -0.5) is negative and the
    // exporter writes a signed offset; D must stay connected to B, never fall back to A or a gap.
    const QVector<QVector<ClipInfo>> jcutTracks{
        {makeClip(QStringLiteral("A"), 1.0, 5.0, 0.0, QStringLiteral("ua")),
         makeClip(QStringLiteral("B"), 0.0, 2.0, 2.0, QStringLiteral("ub"))},
        {makeClip(QStringLiteral("C"), 0.0, 2.0, 1.5, QString(), QStringLiteral("ua"), 1.5),
         makeClip(QStringLiteral("D"), 0.0, 1.0, 2.0, QString(), QStringLiteral("ub"), -0.5)}};
    const auto jcutBack = fcpx::xml::parseXml(exportXml(jcutTracks, false).second, dir.path());
    const ClipInfo *jcutB = trackClip(jcutBack, 0, QStringLiteral("B"));
    const ClipInfo *jcutD = trackClip(jcutBack, 1, QStringLiteral("D"));
    const bool jcutOk = jcutBack.error.isEmpty() && jcutB && jcutD && !jcutB->uid.isEmpty()
        && jcutD->anchorUid == jcutB->uid && near(jcutD->anchorOffsetSec, -0.5) && near(jcutD->leadInSec, 2.0);
    std::fprintf(stderr, "G32 jcut error=[%s] anchorD=%d offsetD=%.9f leadInD=%.9f warnings=%d\n",
        jcutBack.error.toUtf8().constData(), int(jcutD && jcutB && jcutD->anchorUid == jcutB->uid),
        jcutD ? jcutD->anchorOffsetSec : -99.0, jcutD ? jcutD->leadInSec : -99.0, int(jcutBack.warnings.size()));
    gate(32, roundtripConnectedOk && flatAnchors == 0 && jcutOk);

    // Spine children by XML structure: asset-clip / gap offsets and the lane of every gap child.
    struct SpineScan { QVector<QPair<double, double>> clips; int overlaps = 0; int gapLaneChildren = 0; int spineLaneClips = 0; bool parsed = true; };
    auto scanSpine = [](const QString &xml) {
        SpineScan scan;
        QXmlStreamReader reader(xml);
        auto seconds = [](const QString &t) {
            const QStringList parts = QString(t).remove(QLatin1Char('s')).split(QLatin1Char('/'));
            return parts.size() == 2 && parts[1].toDouble() != 0 ? parts[0].toDouble() / parts[1].toDouble() : parts[0].toDouble();
        };
        QStringList stack;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isEndElement()) { stack.removeLast(); continue; }
            if (!reader.isStartElement()) continue;
            const QString tag = reader.name().toString();
            const bool inSpine = stack.size() >= 1 && stack.last() == QLatin1String("spine");
            const bool inGap = stack.size() >= 1 && stack.last() == QLatin1String("gap")
                && stack.size() >= 2 && stack[stack.size() - 2] == QLatin1String("spine");
            if (inSpine && tag == QLatin1String("asset-clip")) {
                const double off = seconds(reader.attributes().value("offset").toString());
                scan.clips.append({off, off + seconds(reader.attributes().value("duration").toString())});
                if (reader.attributes().hasAttribute("lane")) ++scan.spineLaneClips;
            } else if (inGap && tag == QLatin1String("asset-clip") && reader.attributes().value("lane") == QLatin1String("1")) {
                ++scan.gapLaneChildren;
            }
            stack.append(tag);
        }
        scan.parsed = !reader.hasError();
        std::sort(scan.clips.begin(), scan.clips.end());
        for (int i = 1; i < scan.clips.size(); ++i)
            if (scan.clips[i].first < scan.clips[i - 1].second - 1e-9) ++scan.overlaps;
        return scan;
    };
    // V1 clips occupy 0..2 and 6..8; the unconnected V2 clip E spans 4..7 (starts in the hole, overlaps the second V1 clip).
    const QVector<QVector<ClipInfo>> unconnectedTracks{
        {makeClip(QStringLiteral("A"), 0.0, 2.0, 0.0, QStringLiteral("ua")),
         makeClip(QStringLiteral("B"), 0.0, 2.0, 4.0, QStringLiteral("ub"))},
        {makeClip(QStringLiteral("E"), 0.0, 3.0, 4.0, QString())}};
    const auto unconnectedExport = exportXml(unconnectedTracks, false);
    const auto flatExport = exportXml(unconnectedTracks, true);
    const SpineScan unconnectedScan = scanSpine(unconnectedExport.second);
    const SpineScan flatScan = scanSpine(flatExport.second);
    const SpineScan connectedScan = scanSpine(connectedExport.second);
    const QString unconnectedPath = dir.filePath(QStringLiteral("unconnected.fcpxml"));
    QFile unconnectedFile(unconnectedPath);
    const bool unconnectedWritten = unconnectedFile.open(QIODevice::WriteOnly) && unconnectedFile.write(unconnectedExport.second.toUtf8()) > 0;
    unconnectedFile.close();
    const auto unconnectedBack = fcpx::xml::readFcpxml(unconnectedPath);
    const ClipInfo *backE = trackClip(unconnectedBack, 1, QStringLiteral("E"));
    const bool unconnectedGateOk = unconnectedScan.parsed && connectedScan.parsed && flatScan.parsed
        && unconnectedScan.clips.size() == 2 && unconnectedScan.overlaps == 0 && connectedScan.overlaps == 0
        && unconnectedScan.spineLaneClips == 0 && unconnectedScan.gapLaneChildren == 1
        && flatScan.overlaps > 0 && flatScan.gapLaneChildren == 0
        && unconnectedWritten && unconnectedBack.error.isEmpty() && unconnectedBack.videoTracks.size() == 2
        && backE && backE->anchorUid.isEmpty() && near(backE->leadInSec, 4.0);
    std::fprintf(stderr, "G33 spineClips=%d overlaps=%d connectedOverlaps=%d spineLaneClips=%d gapLaneChildren=%d "
        "flatControl(overlaps=%d gapLaneChildren=%d) importedTracks=%d importedE(anchor=[%s] leadIn=%.9f)\n",
        int(unconnectedScan.clips.size()), unconnectedScan.overlaps, connectedScan.overlaps, unconnectedScan.spineLaneClips,
        unconnectedScan.gapLaneChildren, flatScan.overlaps, flatScan.gapLaneChildren,
        int(unconnectedBack.videoTracks.size()), backE ? backE->anchorUid.toUtf8().constData() : "?",
        backE ? backE->leadInSec : -99.0);
    gate(33, unconnectedGateOk);

    return tally.finish();
}
