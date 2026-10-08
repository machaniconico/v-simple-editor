#include "../Timeline.h"
#include "../UndoManager.h"
#include "../ProjectFile.h"

#include <QLabel>
#include <cstdio>

int runTrackManageSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    Timeline timeline;
    timeline.addVideoTrack();
    timeline.addAudioTrack();
    const QJsonObject defaults = timeline.trackFlagsToJson();
    auto baseline = [&] {
        timeline.undoManager()->clear();
        timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("Baseline"));
    };
    bool g1 = true;
    for (const QString &kind : {QStringLiteral("video"), QStringLiteral("audio")})
        for (const QJsonValue &value : defaults.value(kind).toArray())
            g1 = g1 && !value.toObject().contains(QStringLiteral("name"))
                && !value.toObject().contains(QStringLiteral("color"));
    timeline.setTrackCustomName(false, 1, QStringLiteral("映像"));
    timeline.setTrackColor(false, 1, QColor(QStringLiteral("#d94b4b")));
    timeline.setTrackCustomName(true, 0, QStringLiteral("音声"));
    timeline.setTrackColor(true, 0, QColor(QStringLiteral("#4b78d1")));
    const QJsonObject expected = timeline.trackFlagsToJson();
    ProjectData project;
    project.videoTracks = timeline.allVideoTracks();
    project.audioTracks = timeline.allAudioTracks();
    ProjectData loaded;
    g1 = ProjectFile::fromJsonString(ProjectFile::toJsonString(project), loaded) && g1;
    Timeline restored;
    restored.restoreFromProject(loaded.videoTracks, loaded.audioTracks, 0.0, -1.0, -1.0, 100);
    g1 = g1 && restored.trackFlagsToJson() == expected;
    restored.setTrackCustomName(false, 1, QStringLiteral("変更"));
    restored.undo();
    g1 = g1 && restored.trackFlagsToJson() == expected;
    restored.applyTrackFlagsFromJson(defaults);
    g1 = g1 && restored.trackFlagsToJson() == defaults;
    gate(1, g1);

    timeline.applyTrackFlagsFromJson(defaults);
    baseline();
    bool g2 = true;
    for (bool audio : {false, true}) {
        const quint64 serial = timeline.undoManager()->saveSerial();
        g2 = timeline.setTrackCustomName(audio, 1, QStringLiteral("名前")) && g2;
        g2 = g2 && timeline.undoManager()->saveSerial() == serial + 1;
        timeline.undo();
        g2 = g2 && timeline.trackAt(audio, 1)->customName.isEmpty();
        timeline.redo();
        g2 = g2 && timeline.trackAt(audio, 1)->customName == QStringLiteral("名前");
        const quint64 colorSerial = timeline.undoManager()->saveSerial();
        g2 = timeline.setTrackColor(audio, 1, QColor(QStringLiteral("#123456"))) && g2;
        g2 = g2 && timeline.undoManager()->saveSerial() == colorSerial + 1;
        timeline.undo();
        g2 = g2 && !timeline.trackAt(audio, 1)->color.isValid()
            && timeline.trackAt(audio, 1)->customName == QStringLiteral("名前");
        timeline.redo();
        g2 = g2 && timeline.trackAt(audio, 1)->color == QColor(QStringLiteral("#123456"));
    }
    int namedHeaders = 0, coloredHeaders = 0;
    for (const auto *label : timeline.findChildren<QLabel *>(QStringLiteral("timelineTrackName")))
        if (label->text() == QStringLiteral("名前")) ++namedHeaders;
    for (const auto *stripe : timeline.findChildren<QWidget *>(QStringLiteral("timelineTrackColorStripe")))
        if (stripe->width() == 6 && stripe->styleSheet().contains(QStringLiteral("#123456"))) ++coloredHeaders;
    g2 = g2 && namedHeaders == 2 && coloredHeaders == 2;
    gate(2, g2);

    const QJsonObject before = timeline.trackFlagsToJson();
    const auto state = timeline.currentState();
    const quint64 serial = timeline.undoManager()->saveSerial();
    QString error;
    bool g3 = !timeline.setTrackCustomName(false, -1, QStringLiteral("bad"), &error)
        && !error.isEmpty();
    error.clear();
    g3 = !timeline.setTrackColor(true, timeline.audioTrackCount(), QColor(Qt::red), &error)
        && !error.isEmpty() && g3;
    g3 = g3 && timeline.trackFlagsToJson() == before
        && timeline.undoManager()->saveSerial() == serial
        && timeline.currentState().videoTracks.size() == state.videoTracks.size()
        && timeline.currentState().audioTracks.size() == state.audioTracks.size();
    gate(3, g3);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
