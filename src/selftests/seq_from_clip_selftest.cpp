#include "../Timeline.h"
#include "../UndoManager.h"
#include "../libavcore/Encode.h"

#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QStatusBar>
#include <QTemporaryDir>
#include <cstdio>

namespace {
bool writeMedia(const QString &path, const QString &wavPath = QString())
{
    if (!wavPath.isEmpty()) {
        QFile wav(wavPath);
        if (!wav.open(QIODevice::WriteOnly)) return false;
        QDataStream out(&wav);
        out.setByteOrder(QDataStream::LittleEndian);
        constexpr quint32 bytes = 48000 * 2;
        out.writeRawData("RIFF", 4);
        out << quint32(36 + bytes);
        out.writeRawData("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
            << quint32(96000) << quint16(2) << quint16(16);
        out.writeRawData("data", 4);
        out << bytes;
        for (int i = 0; i < 48000; ++i) out << qint16((i % 100 < 50) ? 1000 : -1000);
        if (out.status() != QDataStream::Ok) return false;
    }
    libavcore::EncodeRequest request;
    request.width = 64;
    request.height = 48;
    request.fps = request.fpsNum = 30;
    request.fpsDen = 1;
    request.videoCodecName = "mpeg4";
    request.videoBitrateBits = 200000;
    request.hwVendorHint = "none";
    request.outputPath = path.toStdString();
    request.audioSourcePath = wavPath.toStdString();
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(64, 48, QImage::Format_RGB888);
    frame.fill(Qt::blue);
    for (int i = 0; i < 30; ++i)
        if (!encoder.pushFrameRgb24(frame.constBits(), frame.bytesPerLine(), i)) return false;
    return !encoder.finalize().has_value();
}
}

int runSeqFromClipSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir dir;
    const QString video = dir.filePath(QStringLiteral("video.mp4"));
    const QString av = dir.filePath(QStringLiteral("av.mkv"));
    const bool fixtures = dir.isValid() && writeMedia(video)
        && writeMedia(av, dir.filePath(QStringLiteral("audio.wav")));

    Timeline timeline;
    timeline.setProjectOutputConfig(1280, 720, true, 24.0);
    ClipInfo source;
    source.filePath = video;
    source.displayName = QStringLiteral("素材");
    source.duration = 1.0;
    source.inPoint = 0.1;
    source.outPoint = 0.8;
    source.leadInSec = 4.0;
    source.speed = 2.0;
    source.linkGroup = 99;
    const QString initialId = timeline.activeSequenceId();
    const bool created = fixtures && timeline.createSequenceFromClip(source);
    const auto first = timeline.currentState();
    gate(1, created && timeline.sequenceList().size() == 2
        && timeline.activeSequenceId() != initialId
        && timeline.sequenceList().last().name == source.displayName
        && !first.videoTracks.isEmpty() && first.videoTracks[0].size() == 1
        && first.videoTracks[0][0].filePath == video
        && first.videoTracks[0][0].leadInSec == 0.0
        && first.videoTracks[0][0].inPoint == source.inPoint
        && first.videoTracks[0][0].outPoint == source.outPoint
        && first.videoTracks[0][0].speed == source.speed
        && first.audioTracks[0].isEmpty());

    const bool avCreated = fixtures && timeline.createSequenceFromMedia(av, source.displayName);
    const auto linked = timeline.currentState();
    gate(2, avCreated && !linked.videoTracks.isEmpty() && !linked.audioTracks.isEmpty()
        && linked.videoTracks[0].size() == 1 && linked.audioTracks[0].size() == 1
        && linked.videoTracks[0][0].filePath == av && linked.audioTracks[0][0].filePath == av
        && linked.videoTracks[0][0].leadInSec == 0.0 && linked.audioTracks[0][0].leadInSec == 0.0
        && linked.videoTracks[0][0].linkGroup > 0
        && linked.videoTracks[0][0].linkGroup == linked.audioTracks[0][0].linkGroup
        && timeline.sequenceList().last().name == QStringLiteral("素材 (2)"));

    QStatusBar status;
    QObject::connect(&timeline, &Timeline::statusMessageRequested, &status,
                     [&status](const QString &text, int timeout) { status.showMessage(text, timeout); });
    // Navigation itself has no undo entry; creation must still return here.
    const bool navigated = timeline.sequenceList().size() > 1
        && timeline.setActiveSequence(timeline.sequenceList()[1].id);
    const auto before = timeline.currentState();
    const QString beforeId = timeline.activeSequenceId();
    const auto beforeList = timeline.sequenceList();
    const auto serial = timeline.undoManager()->saveSerial();
    const int index = timeline.undoManager()->currentIndex();
    const bool notified = fixtures && timeline.createSequenceFromMedia(video);
    const auto after = timeline.currentState();
    gate(3, notified && status.currentMessage().contains(QStringLiteral("プロジェクト設定と異なります"))
        && status.currentMessage().contains(QStringLiteral("素材 64x48 30fps"))
        && status.currentMessage().contains(QStringLiteral("プロジェクト 1280x720 24fps"))
        && after.projectWidth == before.projectWidth && after.projectHeight == before.projectHeight
        && after.projectExplicitOutput == before.projectExplicitOutput
        && timeline.projectFrameRate() == 24.0);

    const QString createdId = timeline.activeSequenceId();
    const bool oneSave = timeline.undoManager()->saveSerial() == serial + 1;
    timeline.undo();
    const auto undone = timeline.currentState();
    const auto undoneList = timeline.sequenceList();
    bool restoredList = undoneList.size() == beforeList.size();
    for (int i = 0; restoredList && i < beforeList.size(); ++i)
        restoredList = undoneList[i].id == beforeList[i].id && undoneList[i].name == beforeList[i].name;
    const bool restored = navigated && restoredList && timeline.activeSequenceId() == beforeId
        && timeline.undoManager()->currentIndex() == index
        && undone.clipParentEntries == before.clipParentEntries
        && !undone.videoTracks.isEmpty() && undone.videoTracks[0].size() == 1
        && undone.videoTracks[0][0].filePath == video;
    timeline.redo();
    const bool redone = timeline.activeSequenceId() == createdId
        && timeline.sequenceList().size() == beforeList.size() + 1;
    Timeline fresh;
    const auto freshId = fresh.activeSequenceId();
    const auto freshSerial = fresh.undoManager()->saveSerial();
    const bool freshCreated = fixtures && fresh.createSequenceFromMedia(video)
        && fresh.undoManager()->saveSerial() == freshSerial + 1;
    fresh.undo();
    const bool freshRestored = fresh.activeSequenceId() == freshId && fresh.sequences().isEmpty()
        && fresh.sequenceList().size() == 1 && !fresh.undoManager()->canUndo();
    const auto invalidSerial = fresh.undoManager()->saveSerial();
    const bool invalidRejected = !fresh.createSequenceFromMedia(dir.filePath(QStringLiteral("missing.mp4")))
        && fresh.undoManager()->saveSerial() == invalidSerial && fresh.sequences().isEmpty();
    gate(4, notified && oneSave && restored && redone && freshCreated && freshRestored && invalidRejected);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
