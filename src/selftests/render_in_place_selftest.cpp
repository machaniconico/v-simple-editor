#include "../RenderInPlace.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../ProjectFile.h"
#include "../UndoManager.h"
#include "../libavcore/Probe.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cmath>
#include <limits>

namespace {
QJsonObject clipJson(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{{clip}};
    return QJsonDocument::fromJson(ProjectFile::toJsonString(data).toUtf8()).object()
        .value("videoTracks").toArray().at(0).toArray().at(0).toObject();
}
bool videoOnly(const QString &path)
{
    AVFormatContext *context = nullptr;
    if (avformat_open_input(&context, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    bool video = false, audio = false;
    const bool probed = avformat_find_stream_info(context, nullptr) >= 0;
    if (probed) {
        for (unsigned i = 0; i < context->nb_streams; ++i) {
            video |= context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
            audio |= context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
        }
    }
    avformat_close_input(&context);
    return probed && video && !audio;
}
double mse(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull() || left.size() != right.size())
        return std::numeric_limits<double>::infinity();
    const auto a = left.convertToFormat(QImage::Format_RGB32);
    const auto b = right.convertToFormat(QImage::Format_RGB32);
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const double r = qRed(pa[x]) - qRed(pb[x]);
            const double g = qGreen(pa[x]) - qGreen(pb[x]);
            const double bl = qBlue(pa[x]) - qBlue(pb[x]);
            sum += r*r + g*g + bl*bl;
        }
    }
    return sum / (a.width() * a.height() * 3.0);
}
}

int runRenderInPlaceSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir output;
    ClipInfo original{};
    original.filePath = QFileInfo(QStringLiteral("test_assets/e2e_clip.mp4")).absoluteFilePath();
    original.displayName = QStringLiteral("焼き込みテスト");
    const auto sourceDuration = libavcore::probeDurationMicroseconds(original.filePath.toStdString());
    original.duration = sourceDuration ? double(*sourceDuration) / 1000000.0 : 0.0;
    original.inPoint = 1.0;
    original.outPoint = 2.0;
    original.effects.append(VideoEffect::createBrightnessContrast(12.0, 0.0));
    Timeline timeline;
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{original}},
                                QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
    timeline.undoManager()->clear();
    timeline.saveUndoState(QStringLiteral("テスト初期状態"));
    renderinplace::Options options;
    options.outputDir = output.path();
    options.outputSize = QSize(640, 360);
    options.fps = 30;
    const QJsonObject before = clipJson(original);
    QVector<QImage> frames;
    for (qint64 tick : {100000LL, 500000LL, 900000LL})
        frames.append(tlrender::renderFrameAt(&timeline, tick, options.outputSize));
    QString path, error;
    const quint64 serial = timeline.undoManager()->saveSerial();
    const bool baked = output.isValid() && original.duration >= 3.0
        && renderinplace::renderClipInPlace(timeline, 0, 0, options, &path, &error);
    if (!baked) std::fprintf(stderr, "%s\n", qPrintable(error));
    double worst = 0.0;
    int index = 0;
    for (qint64 tick : {100000LL, 500000LL, 900000LL})
        worst = qMax(worst, mse(frames[index++], tlrender::renderFrameAt(&timeline, tick, options.outputSize)));
    std::fprintf(stderr, "render-in-place MSE: %.6f\n", worst);
    gate(1, baked && worst < 2.0);
    const bool oneUndo = baked && timeline.undoManager()->saveSerial() == serial + 1;
    if (baked) timeline.undo();
    gate(4, oneUndo && clipJson(timeline.videoTracks()[0]->clips()[0]) == before);

    options.handlesSec = 1.0;
    const bool withHandles = baked && renderinplace::renderClipInPlace(timeline, 0, 0, options, &path, &error);
    const ClipInfo replaced = timeline.videoTracks()[0]->clips()[0];
    const auto mediaDuration = withHandles ? libavcore::probeDurationMicroseconds(path.toStdString())
                                           : std::optional<int64_t>{};
    gate(2, withHandles && videoOnly(path) && mediaDuration
        && std::abs(double(*mediaDuration) / 1000000.0 - (original.effectiveDuration() + 2.0)) <= 1.0 / options.fps
        && replaced.inPoint == 1.0 && replaced.effects.isEmpty() && replaced.speed == 1.0);
    const auto restoreSerial = timeline.undoManager()->saveSerial();
    const bool restored = withHandles && renderinplace::decomposeRenderInPlace(timeline, 0, 0);
    gate(3, restored && clipJson(timeline.videoTracks()[0]->clips()[0]) == before
        && timeline.undoManager()->saveSerial() == restoreSerial + 1);

    ClipInfo nested = replaced;
    nested.renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
    nested.renderInPlaceOriginal->renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
    ProjectData saved, loaded;
    saved.videoTracks = QVector<QVector<ClipInfo>>{{nested, original}};
    const QString projectPath = output.filePath(QStringLiteral("roundtrip.veditor"));
    const QJsonObject serialized = clipJson(nested);
    const bool roundTrip = ProjectFile::save(projectPath, saved) && ProjectFile::load(projectPath, loaded);
    const bool shape = roundTrip && loaded.videoTracks.size() == 1 && loaded.videoTracks[0].size() == 2;
    // Also exercise the reader against nested input that was not emitted by
    // our writer (which already strips the second level).
    QJsonObject injected = serialized;
    QJsonObject child = injected.value("renderInPlaceOriginal").toObject();
    child["renderInPlaceOriginal"] = before;
    injected["renderInPlaceOriginal"] = child;
    QJsonObject root = QJsonDocument::fromJson(ProjectFile::toJsonString(saved).toUtf8()).object();
    root["videoTracks"] = QJsonArray{QJsonArray{injected}};
    ProjectData external;
    const bool readNested = ProjectFile::fromJsonString(
        QString::fromUtf8(QJsonDocument(root).toJson()), external)
        && external.videoTracks.size() == 1 && external.videoTracks[0].size() == 1
        && external.videoTracks[0][0].renderInPlaceOriginal
        && !external.videoTracks[0][0].renderInPlaceOriginal->renderInPlaceOriginal;
    gate(5, shape && readNested && loaded.videoTracks[0][0].renderInPlaceOriginal
        && !loaded.videoTracks[0][0].renderInPlaceOriginal->renderInPlaceOriginal
        && clipJson(*loaded.videoTracks[0][0].renderInPlaceOriginal) == before
        && !loaded.videoTracks[0][1].renderInPlaceOriginal
        && !clipJson(original).contains("renderInPlaceOriginal")
        && !serialized.value("renderInPlaceOriginal").toObject().contains("renderInPlaceOriginal"));
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
