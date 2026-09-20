#include "../FcpxmlImport.h"
#include "../FcpxmlExporter.h"
#include "../UndoManager.h"
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

int runFcpxmlImportSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
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
        position += c.effectiveDuration();
    }
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
    gate(2, rational.error.isEmpty() && rational.videoTracks[0].size() == 2
        && near(rational.videoTracks[0][0].inPoint, 1001.0 / 30000.0)
        && near(rational.videoTracks[0][0].outPoint, 2002.0 / 30000.0)
        && near(rational.videoTracks[0][1].leadInSec, 1001.0 / 30000.0)
        && cancellation.error.isEmpty() && cancellation.videoTracks[0].size() == 1
        && near(cancellation.videoTracks[0][0].leadInSec, 1001.0 / 30000.0)
        && !fcpx::xml::parseXml(document(QStringLiteral("<asset-clip ref=\"a\" duration=\"1/0s\"/>")), dir.path()).error.isEmpty());

    const QString two = QStringLiteral("<asset-clip ref=\"a\" lane=\"1\" offset=\"2s\" duration=\"1s\"/>"
        "<asset-clip ref=\"a\" lane=\"0\" duration=\"3s\"/>");
    const auto lanes = fcpx::xml::parseXml(document(two), dir.path());
    Timeline laneTimeline;
    bool tracks = lanes.error.isEmpty() && laneTimeline.createSequence(lanes.title, lanes.videoTracks, lanes.audioTracks);
    const auto state = laneTimeline.currentState();
    tracks &= state.videoTracks.size() == 2 && state.videoTracks[0].size() == 1
        && state.videoTracks[1].size() == 1 && near(state.videoTracks[1][0].leadInSec, 2.0);
    const auto capped = fcpx::xml::parseXml(document(two + QStringLiteral("<asset-clip ref=\"a\" lane=\"2\" duration=\"1s\"/>")), dir.path());
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
    gate(5, undo);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
