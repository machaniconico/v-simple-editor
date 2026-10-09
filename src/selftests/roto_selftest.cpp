// --selftest=roto: carrier remapping, undo, persistence, queue restoration,
// roto::applyEntryToFrame and the MainWindow single writer (storeRotoEntry).
#include "SelftestGate.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../ProjectFile.h"
#include "../TrackMatteKey.h"
#include "../RenderQueue.h"
#include "../SubtitleTrackRenderer.h"
#include "../libavcore/Encode.h"
#include "../AcesColor.h"
#include "../MainWindow.h"
#include "../RotoApply.h"
#include "../libavcore/FrameGrab.h"
#include "../TimelineFrameRenderer.h"
#include "../SmartRender.h"
#include "../RenderInPlace.h"
#include "../VideoEffect.h"
#include <cmath>
#include <atomic>
#include <functional>
#include <QDateTime>
#include <QMutex>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QDataStream>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSignalSpy>
#include <QCoreApplication>
#include <QDropEvent>
#include <QMimeData>
#include <iostream>

namespace {

QString gSyntheticClipPath;

// lumaStep > 0 writes frame n as uniform gray of luma lumaStep * n instead of `color`.
bool writeSyntheticClip(const QString &path, const QColor &color = QColor(32, 80, 160),
                        int lumaStep = 0, int frameCount = 20)
{
    libavcore::EncodeRequest request;
    request.width = 64;
    request.height = 64;
    request.fps = 10;
    request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoBitrateBits = 600000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(64, 64, QImage::Format_RGB888);
    frame.fill(color);
    for (int i = 0; i < frameCount; ++i) {
        if (lumaStep > 0) frame.fill(QColor(lumaStep * i, lumaStep * i, lumaStep * i));
        if (!encoder.pushFrame(frame, i)) return false;
    }
    return !encoder.finalize().has_value() && QFile::exists(path);
}

ClipInfo clip(int id)
{
    ClipInfo c;
    c.filePath = gSyntheticClipPath;
    c.linkGroup = id + 1;
    c.displayName = QString::number(id);
    c.duration = 2.0;
    c.inPoint = 0.0;
    c.outPoint = 2.0;
    return c;
}

RotoClipEntry entry(int marker, bool mask = false)
{
    RotoClipEntry e;
    for (int i = 0; i < marker; ++i)
        e.path.points.append(RotoPoint{});
    RotoKeyframe frame;
    frame.frameNumber = marker;
    frame.path = e.path;
    e.keyframes.append(frame);
    if (mask) {
        e.brushMask = QImage(3, 2, QImage::Format_ARGB32);
        e.brushMask.fill(Qt::white);
    }
    return e;
}

void seed(Timeline &tl, bool matte)
{
    const QVector<QVector<ClipInfo>> video{
        {clip(100), clip(101)}, {clip(1), clip(2)}, {clip(3)}};
    const QVector<QVector<ClipInfo>> audio{{clip(100), clip(101)}};
    tl.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    QHash<QString, RotoClipEntry> rotos;
    rotos.insert(QStringLiteral("1:0"), entry(1));
    rotos.insert(QStringLiteral("1:1"), entry(2, true));
    rotos.insert(QStringLiteral("2:0"), entry(3));
    tl.setRotoEntries(rotos);
    if (matte) {
        QHash<QString, TimelineTrackMatteEntry> mattes;
        QHash<QString, QString> parents;
        for (const QString &key : rotos.keys()) {
            TimelineTrackMatteEntry m;
            m.matteType = TrackMatteType::AlphaMatte;
            m.matteSourceClipId = QStringLiteral("0:0");
            mattes.insert(key, m);
            parents.insert(key, QStringLiteral("0:0"));
        }
        tl.setTrackMatteEntries(mattes);
        tl.setClipParentEntries(parents);
    }
}

QSet<QString> keys(const QHash<QString, RotoClipEntry> &entries)
{
    QSet<QString> out;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        out.insert(it.key());
    return out;
}

template <typename T> QSet<QString> keys(const QHash<QString, T> &entries)
{
    QSet<QString> out;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        out.insert(it.key());
    return out;
}

QString show(QSet<QString> values)
{
    QStringList sorted;
    for (const QString &value : values) sorted.append(value);
    sorted.sort();
    return sorted.join(QLatin1Char(','));
}

bool invariant(const QHash<QString, RotoClipEntry> &entries)
{
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        if (it.value().clipId != it.key()) return false;
    return true;
}

QString whereMarker(const QHash<QString, RotoClipEntry> &entries, int marker)
{
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        if (it.value().path.points.size() == marker) return it.key();
    return QStringLiteral("missing");
}

QHash<QString, RotoClipEntry> restoreEntries(const QVector<RotoClipEntry> &values)
{
    QHash<QString, RotoClipEntry> out;
    for (const auto &value : values) out.insert(value.clipId, value);
    return out;
}

bool samePayload(const RotoClipEntry &a, const RotoClipEntry &b)
{
    return a.clipId == b.clipId && a.path.points.size() == b.path.points.size()
        && a.keyframes.size() == b.keyframes.size()
        && a.brushMask.size() == b.brushMask.size();
}


RotoPoint corner(double x, double y)
{
    RotoPoint p;
    p.position = p.handleIn = p.handleOut = QPointF(x, y);
    return p;
}

RotoPath rectPath(double x0, double y0, double x1, double y1, double feather = 0.0)
{
    RotoPath path;
    path.points = {corner(x0, y0), corner(x1, y0), corner(x1, y1), corner(x0, y1)};
    path.closed = true;
    path.feather = feather;
    return path;
}

// Pixel statistics of a roto result against its input. "Inside" pixels must be
// byte-identical (ARGB), "outside" pixels must be alpha 0 with RGB 0.
struct MaskStats {
    int insidePixels = 0;
    int insideDiff = 0;
    int insideMaxDiff = 0;
    int outsidePixels = 0;
    int outsideBad = 0;
    int edgePixels = 0;
    int edgeAlphaMin = 255;
    int edgeAlphaMax = 0;
};

template <typename InsideFn, typename OutsideFn>
MaskStats maskStats(const QImage &input, const QImage &output, InsideFn inside, OutsideFn outside)
{
    MaskStats stats;
    if (input.isNull() || output.size() != input.size()) return stats;
    for (int y = 0; y < input.height(); ++y) {
        for (int x = 0; x < input.width(); ++x) {
            const QRgb in = input.pixel(x, y);
            const QRgb out = output.pixel(x, y);
            const int alpha = qAlpha(out);
            if (alpha > 0 && alpha < 255) {
                ++stats.edgePixels;
                stats.edgeAlphaMin = qMin(stats.edgeAlphaMin, alpha);
                stats.edgeAlphaMax = qMax(stats.edgeAlphaMax, alpha);
            }
            if (inside(x, y)) {
                ++stats.insidePixels;
                const int diff = qMax(qMax(qAbs(qRed(in) - qRed(out)), qAbs(qGreen(in) - qGreen(out))),
                                      qMax(qAbs(qBlue(in) - qBlue(out)), qAbs(qAlpha(in) - alpha)));
                if (diff > 0) ++stats.insideDiff;
                stats.insideMaxDiff = qMax(stats.insideMaxDiff, diff);
            } else if (outside(x, y)) {
                ++stats.outsidePixels;
                if (alpha != 0 || qRed(out) != 0 || qGreen(out) != 0 || qBlue(out) != 0)
                    ++stats.outsideBad;
            }
        }
    }
    return stats;
}

bool inRect(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

void printStats(const char *label, const MaskStats &s)
{
    std::cerr << label << " inside=" << s.insidePixels << " insideDiff=" << s.insideDiff
              << " insideMaxDiff=" << s.insideMaxDiff << " outside=" << s.outsidePixels
              << " outsideBad=" << s.outsideBad << " edge=" << s.edgePixels
              << " edgeAlpha=" << (s.edgePixels ? s.edgeAlphaMin : 0) << ".."
              << s.edgeAlphaMax << '\n';
}

bool cleanMask(const MaskStats &s)
{
    return s.insidePixels > 0 && s.insideDiff == 0 && s.outsidePixels > 0 && s.outsideBad == 0;
}

// MainWindow fixture for G9-G12 (seedWindow): V1 [100, 101 (1 s gap)],
// V2 [1, 2 (1 s gap)], V3 [3], V4 [matte/parent source at 10 s]. Roto markers
// 1/2/3/4 sit on V2:0 / V2:1 / V3:0 / V1:1; the matte on the same key carries a
// marker-specific type so a roto/matte pairing shift is visible.
TrackMatteType matteTypeForMarker(int marker)
{
    switch (marker) {
    case 1: return TrackMatteType::AlphaMatte;
    case 2: return TrackMatteType::AlphaInvertedMatte;
    case 3: return TrackMatteType::LumaMatte;
    case 4: return TrackMatteType::LumaInvertedMatte;
    default: return TrackMatteType::None;
    }
}

QString layoutSignature(const Timeline *tl)
{
    QStringList parts;
    for (const auto *track : tl->videoTracks()) {
        QStringList clips;
        for (const ClipInfo &c : track->clips())
            clips.append(QStringLiteral("%1@%2+%3/%4-%5").arg(c.displayName)
                .arg(c.leadInSec, 0, 'f', 3).arg(c.effectiveDuration(), 0, 'f', 3)
                .arg(c.inPoint, 0, 'f', 3).arg(c.outPoint, 0, 'f', 3));
        parts.append(QLatin1Char('[') + clips.join(QLatin1Char(',')) + QLatin1Char(']'));
    }
    return parts.join(QLatin1Char(' '));
}

// G13-G21 (renderer roto). Per-pixel comparison in premultiplied ARGB over the
// pixels selected by `region`: diffPixels counts channel differences > 2.
struct RegionDiff {
    int pixels = 0;
    int diffPixels = 0;
    int maxDiff = 255;
};

template <typename RegionFn>
RegionDiff regionDiff(const QImage &a, const QImage &b, RegionFn region)
{
    RegionDiff d;
    if (a.isNull() || b.isNull() || a.size() != b.size()) return d;
    const QImage pa = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage pb = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    d.maxDiff = 0;
    for (int y = 0; y < pa.height(); ++y) {
        const QRgb *la = reinterpret_cast<const QRgb *>(pa.constScanLine(y));
        const QRgb *lb = reinterpret_cast<const QRgb *>(pb.constScanLine(y));
        for (int x = 0; x < pa.width(); ++x) {
            if (!region(x, y)) continue;
            ++d.pixels;
            const int diff = qMax(qMax(qAbs(qRed(la[x]) - qRed(lb[x])), qAbs(qGreen(la[x]) - qGreen(lb[x]))),
                                  qMax(qAbs(qBlue(la[x]) - qBlue(lb[x])), qAbs(qAlpha(la[x]) - qAlpha(lb[x]))));
            if (diff > 2) ++d.diffPixels;
            d.maxDiff = qMax(d.maxDiff, diff);
        }
    }
    return d;
}

std::string showDiff(const RegionDiff &d)
{
    return "pixels=" + std::to_string(d.pixels) + " diffPixels=" + std::to_string(d.diffPixels)
        + " maxDiff=" + std::to_string(d.maxDiff);
}

// Mean BT.601 luma of the opaque-flattened pixels selected by `region`
// (-1 when the image is null or the region is empty).
template <typename RegionFn>
double meanLuma(const QImage &image, RegionFn region)
{
    if (image.isNull()) return -1.0;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    double sum = 0.0;
    int count = 0;
    for (int y = 0; y < rgb.height(); ++y) {
        for (int x = 0; x < rgb.width(); ++x) {
            if (!region(x, y)) continue;
            const QRgb p = rgb.pixel(x, y);
            sum += 0.299 * qRed(p) + 0.587 * qGreen(p) + 0.114 * qBlue(p);
            ++count;
        }
    }
    return count ? sum / count : -1.0;
}

bool writeTone(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = 8 * 48000;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + frames * 2);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
        << quint32(96000) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(frames * 2);
    for (quint32 i = 0; i < frames; ++i)
        out << qint16(std::lround(8192.0 * std::sin(6.283185307179586 * 440.0 * i / 48000.0)));
    return out.status() == QDataStream::Ok && file.flush();
}

// G22-G25 (US-915) fixture: V1 [100], V2 [10 @0 s, 11 @3 s, 12 @6 s],
// V3 [20 @0 s, 21 @6 s], every clip 2 s. Dropping V2:1 on V3 at 3 s lands at
// V3 index 1. Roto markers 1..5 sit on V2:0 / V2:1 / V2:2 / V3:0 / V3:1,
// parents are V3:0 -> V2:2 and V2:1 -> V3:1, Timeline-side mattes sit on
// V2:1 / V3:1. linked: V2:1 keeps linkGroup 12 and its audio partner is on A1
// (A2 with partnerOnA2, so a drop onto V3 creates exactly one track, A3).
void seedCrossDrop(Timeline &tl, bool linked, bool partnerOnA2 = false)
{
    auto at = [](int id, double leadInSec) {
        ClipInfo c = clip(id);
        c.leadInSec = leadInSec;
        return c;
    };
    ClipInfo moved = at(11, 1.0);
    if (!linked) moved.linkGroup = 0;
    const QVector<QVector<ClipInfo>> video{
        {clip(100)}, {at(10, 0.0), moved, at(12, 1.0)}, {at(20, 0.0), at(21, 4.0)}};
    QVector<QVector<ClipInfo>> audio{{}};
    if (partnerOnA2) audio.append(QVector<ClipInfo>());
    if (linked) audio[partnerOnA2 ? 1 : 0].append(at(11, 3.0));
    tl.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    QHash<QString, RotoClipEntry> rotos;
    const QStringList rotoKeys{QStringLiteral("1:0"), QStringLiteral("1:1"),
                               QStringLiteral("1:2"), QStringLiteral("2:0"),
                               QStringLiteral("2:1")};
    for (int i = 0; i < rotoKeys.size(); ++i)
        rotos.insert(rotoKeys[i], entry(i + 1, i == 1));
    tl.setRotoEntries(rotos);
    QHash<QString, QString> parents;
    parents.insert(QStringLiteral("2:0"), QStringLiteral("1:2"));
    parents.insert(QStringLiteral("1:1"), QStringLiteral("2:1"));
    tl.setClipParentEntries(parents);
    QHash<QString, TimelineTrackMatteEntry> mattes;
    TimelineTrackMatteEntry m;
    m.matteType = TrackMatteType::AlphaMatte;
    m.matteSourceClipId = QStringLiteral("0:0");
    mattes.insert(QStringLiteral("1:1"), m);
    mattes.insert(QStringLiteral("2:1"), m);
    tl.setTrackMatteEntries(mattes);
}

// "key=marker" sorted, so a key that kept the wrong payload is visible.
QString rotoLayout(const QHash<QString, RotoClipEntry> &entries)
{
    QStringList out;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        out.append(it.key() + QLatin1Char('=') + QString::number(it.value().path.points.size()));
    out.sort();
    return out.join(QLatin1Char(','));
}

QString parentLayout(const QHash<QString, QString> &entries)
{
    QStringList out;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        out.append(it.key() + QLatin1Char('>') + it.value());
    out.sort();
    return out.join(QLatin1Char(','));
}

// Sends a synthetic QDropEvent ("srcIdx:0" payload) to dest at dropTimelineSec.
// Qt 6 resolves QDropEvent::source() only through an active QDrag, so the
// source track is supplied through TimelineTrack's selftest seam.
bool driveCrossDrop(TimelineTrack *src, int srcIdx, TimelineTrack *dest,
                    double dropTimelineSec, bool useSeam, bool *qtSourceNull)
{
    QMimeData mime;
    mime.setData(QStringLiteral("application/x-vse-clip"),
                 QByteArray::number(srcIdx) + QByteArray(":0"));
    const QPointF pos(dropTimelineSec * dest->pixelsPerSecond(), 4.0);
    const double clipDurSec = srcIdx < src->clipCount()
        ? src->clips()[srcIdx].effectiveDuration() : -1.0;
    const auto plan = dest->planDrop(dropTimelineSec, clipDurSec);
    QDropEvent event(pos, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    event.ignore();
    if (qtSourceNull) *qtSourceNull = event.source() == nullptr;
    TimelineTrack::setSelftestDropSource(useSeam ? src : nullptr);
    // QApplication::notify does not deliver a Drop outside an active QDrag, so
    // dispatch through the virtual QObject::event (QWidget::event -> dropEvent).
    static_cast<QObject *>(dest)->event(&event);
    TimelineTrack::setSelftestDropSource(nullptr);
    std::cerr << "  drop srcIdx=" << srcIdx << " timelineSec=" << dropTimelineSec
              << " x=" << pos.x() << " pps=" << dest->pixelsPerSecond()
              << " clipDurSec=" << clipDurSec << " planValid=" << plan.valid
              << " insertIdx=" << plan.insertIdx << " seam=" << useSeam
              << " accepted=" << event.isAccepted() << '\n';
    return event.isAccepted();
}

bool droppedIntoV3(const Timeline &tl)
{
    const auto v2 = tl.videoTracks()[1]->clips();
    const auto v3 = tl.videoTracks()[2]->clips();
    return v2.size() == 2 && v3.size() == 3 && v3[1].displayName == QStringLiteral("11");
}

} // namespace

int runRotoSelftest()
{
    constexpr int kRotoGateCount = 32;
    selftest::GateTally gates;
    QTemporaryDir media;
    gSyntheticClipPath = media.filePath(QStringLiteral("roto-synthetic.mp4"));
    QString unavailable;
    if (!media.isValid() || !selftest::requireEncoder(QStringLiteral("mpeg4"), &unavailable)) {
        for (int gate = 1; gate <= kRotoGateCount; ++gate)
            gates.skip(gate, media.isValid() ? "mpeg4 fixture encoder unavailable"
                                             : "temporary directory unavailable");
        return gates.finish("roto");
    }
    if (!writeSyntheticClip(gSyntheticClipPath)) {
        for (int gate = 1; gate <= kRotoGateCount; ++gate)
            gates.gate(gate, false, "mpeg4 fixture creation failed");
        return gates.finish("roto");
    }

    // G1 detects missing catch-all wiring on any index-changing operation.
    bool g1 = true;
    const QStringList operations{
        QStringLiteral("delete"), QStringLiteral("split"),
        QStringLiteral("move"), QStringLiteral("ripple"),
        QStringLiteral("range-ripple"), QStringLiteral("paste"),
        QStringLiteral("track-add"), QStringLiteral("track-remove"),
        QStringLiteral("nudge")};
    for (bool withMatte : {false, true}) {
        for (const QString &operation : operations) {
            Timeline tl;
            seed(tl, withMatte);
            if (operation == QStringLiteral("paste")) {
                auto extra = tl.rotoEntries();
                extra.insert(QStringLiteral("0:1"), entry(4));
                tl.setRotoEntries(extra);
                if (withMatte) {
                    auto mattes = tl.trackMatteEntries();
                    TimelineTrackMatteEntry m;
                    m.matteType = TrackMatteType::AlphaMatte;
                    m.matteSourceClipId = QStringLiteral("0:0");
                    mattes.insert(QStringLiteral("0:1"), m);
                    tl.setTrackMatteEntries(mattes);
                }
            }
            if (operation == QStringLiteral("nudge")) {
                auto clips = tl.videoTracks()[1]->clips();
                clips[1].leadInSec = 1.0;
                tl.videoTracks()[1]->setClips(clips);
            }
            QString error;
            bool changed = true;
            if (operation == QStringLiteral("delete"))
                changed = tl.deleteClipByIndex(false, 1, 0, false, &error);
            else if (operation == QStringLiteral("split"))
                changed = tl.splitClipByIndex(false, 1, 0, 1.0, &error);
            else if (operation == QStringLiteral("move")) {
                double settled = 0.0;
                changed = tl.moveClipByIndex(false, 1, 0, 4.0, &settled, &error);
            } else if (operation == QStringLiteral("ripple"))
                changed = tl.deleteClipByIndex(false, 1, 0, true, &error);
            else if (operation == QStringLiteral("range-ripple")) {
                changed = tl.selectClipByIndex(false, 1, 0, &error);
                if (changed) tl.rippleDeleteTimeRangeActive(0.0, 2.0);
            } else if (operation == QStringLiteral("paste")) {
                changed = tl.selectClipByIndex(false, 0, 0, &error);
                if (changed) {
                    tl.copySelectedClip();
                    tl.setPlayheadPosition(0.0);
                    tl.pasteClip();
                }
            } else if (operation == QStringLiteral("track-add")) {
                tl.addVideoTrack();
                changed = tl.videoTrackCount() == 4;
            } else if (operation == QStringLiteral("track-remove"))
                changed = tl.removeTrack(false, 1, &error);
            else if (operation == QStringLiteral("nudge")) {
                tl.videoTracks()[1]->setSelectedClip(0);
                tl.videoTracks()[2]->toggleClipSelection(0);
                tl.nudgeSelectedClips(1);
                changed = tl.videoTracks()[1]->clips()[0].leadInSec > 0.0
                    && tl.videoTracks()[2]->clips()[0].leadInSec > 0.0;
            }
            const auto rotos = tl.rotoEntries();
            const auto actual = keys(rotos);
            QSet<QString> expected{QStringLiteral("1:0"), QStringLiteral("1:1"),
                                   QStringLiteral("2:0")};
            if (operation == QStringLiteral("delete") || operation == QStringLiteral("ripple")
                || operation == QStringLiteral("range-ripple"))
                expected = {QStringLiteral("1:0"), QStringLiteral("2:0")};
            if (operation == QStringLiteral("split"))
                expected = {QStringLiteral("1:0"), QStringLiteral("1:2"), QStringLiteral("2:0")};
            if (operation == QStringLiteral("move"))
                expected = {QStringLiteral("1:1"), QStringLiteral("2:0")};
            if (operation == QStringLiteral("track-remove"))
                expected = {QStringLiteral("1:0")};
            if (operation == QStringLiteral("paste"))
                expected.insert(QStringLiteral("0:2"));
            const auto peer = operation == QStringLiteral("range-ripple")
                ? keys(tl.clipParentEntries()) : keys(tl.trackMatteEntries());
            bool movedPayload = true;
            if (operation == QStringLiteral("delete") || operation == QStringLiteral("ripple")
                || operation == QStringLiteral("range-ripple"))
                movedPayload = rotos.value(QStringLiteral("1:0")).path.points.size() == 2;
            if (operation == QStringLiteral("split"))
                movedPayload = rotos.value(QStringLiteral("1:2")).path.points.size() == 2;
            if (operation == QStringLiteral("move"))
                movedPayload = rotos.value(QStringLiteral("1:1")).path.points.size() == 1
                    && whereMarker(rotos, 2) == QStringLiteral("missing");
            if (operation == QStringLiteral("track-remove"))
                movedPayload = rotos.value(QStringLiteral("1:0")).path.points.size() == 3;
            if (operation == QStringLiteral("paste"))
                movedPayload = rotos.value(QStringLiteral("0:2")).path.points.size() == 4;
            const bool one = changed && actual == expected && invariant(rotos) && movedPayload
                && (!withMatte || actual == peer);
            std::cerr << "G1 " << (withMatte ? "matte " : "roto ")
                      << operation.toStdString() << " actual=" << show(actual).toStdString()
                      << " expected=" << show(expected).toStdString()
                      << " peer=" << show(peer).toStdString()
                      << " marker2At=" << whereMarker(rotos, 2).toStdString()
                      << " marker3At=" << whereMarker(rotos, 3).toStdString()
                      << " marker4At=" << whereMarker(rotos, 4).toStdString()
                      << " payloadOk=" << movedPayload
                      << " changed=" << changed << " error=" << error.toStdString() << '\n';
            g1 = g1 && one;
        }
    }
    // A linkGroup-0 paste duplicates X exactly. The audio track has no clips,
    // so pasteClip's insertAt clamp places X' before [B, X]. The monotonic
    // alignment must keep the original X's carriers on index 2.
    Timeline duplicate;
    ClipInfo duplicateSource = clip(51);
    duplicateSource.linkGroup = 0;
    const QVector<QVector<ClipInfo>> duplicateVideo{
        {clip(50), duplicateSource}, {clip(60)}};
    const QVector<QVector<ClipInfo>> duplicateAudio{{}};
    duplicate.restoreFromProject(duplicateVideo, duplicateAudio, 0.0, -1.0, -1.0, 100);
    QHash<QString, RotoClipEntry> duplicateRoto;
    duplicateRoto.insert(QStringLiteral("0:1"), entry(5));
    duplicate.setRotoEntries(duplicateRoto);
    TimelineTrackMatteEntry duplicateMatte;
    duplicateMatte.matteType = TrackMatteType::AlphaMatte;
    duplicateMatte.matteSourceClipId = QStringLiteral("1:0");
    QHash<QString, TimelineTrackMatteEntry> duplicateMattes;
    duplicateMattes.insert(QStringLiteral("0:1"), duplicateMatte);
    duplicate.setTrackMatteEntries(duplicateMattes);
    QHash<QString, QString> duplicateParents;
    duplicateParents.insert(QStringLiteral("0:1"), QStringLiteral("1:0"));
    duplicate.setClipParentEntries(duplicateParents);
    QString duplicateError;
    const bool selectedX = duplicate.selectClipByIndex(false, 0, 1, &duplicateError);
    if (selectedX) {
        duplicate.copySelectedClip();
        duplicate.pasteClip();
    }
    const auto duplicateClips = duplicate.videoTracks()[0]->clips();
    const auto duplicateRotoAfter = duplicate.rotoEntries();
    const auto duplicateMatteAfter = duplicate.trackMatteEntries();
    const auto duplicateParentAfter = duplicate.clipParentEntries();
    const QSet<QString> originalAtTwo{QStringLiteral("0:2")};
    const bool duplicateOrder = duplicateClips.size() == 3
        && duplicateClips[0].linkGroup == 0
        && duplicateClips[1].linkGroup == clip(50).linkGroup
        && duplicateClips[2].linkGroup == 0;
    const bool duplicatePreserved = selectedX && duplicateOrder
        && keys(duplicateRotoAfter) == originalAtTwo
        && keys(duplicateMatteAfter) == originalAtTwo
        && keys(duplicateParentAfter) == originalAtTwo
        && duplicateRotoAfter.value(QStringLiteral("0:2")).path.points.size() == 5
        && duplicateMatteAfter.value(QStringLiteral("0:2")).matteSourceClipId == QStringLiteral("1:0")
        && duplicateParentAfter.value(QStringLiteral("0:2")) == QStringLiteral("1:0")
        && invariant(duplicateRotoAfter);
    std::cerr << "G1 duplicate [B,X]->[X',B,X] selected=" << selectedX
              << " order=" << duplicateOrder
              << " roto=" << show(keys(duplicateRotoAfter)).toStdString()
              << " matte=" << show(keys(duplicateMatteAfter)).toStdString()
              << " parent=" << show(keys(duplicateParentAfter)).toStdString()
              << " expected=" << show(originalAtTwo).toStdString()
              << " marker5At=" << whereMarker(duplicateRotoAfter, 5).toStdString()
              << " error=" << duplicateError.toStdString() << '\n';
    g1 = g1 && duplicatePreserved;
    gates.gate(1, g1, "mutation key set, peer, or clipId differed");

    // G2 detects a second alignment against an already-remapped carrier.
    Timeline twice;
    seed(twice, false);
    const TrackClipSnapshot before = snapshotTrackClips(&twice);
    twice.videoTracks()[1]->removeClip(0);
    twice.remapRotoEntriesAfterMutation(before);
    const auto once = twice.rotoEntries();
    twice.remapRotoEntriesAfterMutation(before);
    const auto twiceResult = twice.rotoEntries();
    const bool repeatedPayload = samePayload(once.value(QStringLiteral("1:0")),
                                             twiceResult.value(QStringLiteral("1:0")));
    bool g2 = keys(once) == keys(twiceResult) && invariant(twiceResult)
        && once.value(QStringLiteral("1:0")).path.points.size() == 2
        && repeatedPayload;
    Timeline nested;
    seed(nested, false);
    nested.videoTracks()[1]->addClip(clip(4));
    const TrackClipSnapshot outerBefore = snapshotTrackClips(&nested);
    QString moveError;
    double settled = 0.0;
    const bool moveOne = nested.moveClipByIndex(false, 1, 1, 6.0, &settled, &moveError);
    const bool moveTwo = nested.moveClipByIndex(false, 1, 0, 8.0, &settled, &moveError);
    const auto innerResult = nested.rotoEntries();
    nested.remapRotoEntriesAfterMutation(outerBefore);
    const auto outerResult = nested.rotoEntries();
    const bool nestedPayload = samePayload(innerResult.value(QStringLiteral("1:2")),
                                           outerResult.value(QStringLiteral("1:2")))
        && samePayload(innerResult.value(QStringLiteral("2:0")),
                       outerResult.value(QStringLiteral("2:0")));
    g2 = g2 && moveOne && moveTwo && keys(innerResult) == keys(outerResult)
        && keys(innerResult) == QSet<QString>{QStringLiteral("1:2"),
            QStringLiteral("2:0")}
        && innerResult.value(QStringLiteral("1:2")).path.points.size() == 1
        && whereMarker(innerResult, 2) == QStringLiteral("missing")
        && nestedPayload && invariant(outerResult);
    std::cerr << "G2 once=" << show(keys(once)).toStdString()
              << " repeated=" << show(keys(twiceResult)).toStdString()
              << " internal=" << show(keys(innerResult)).toStdString()
              << " outer=" << show(keys(outerResult)).toStdString()
              << " moveOne=" << moveOne << " moveTwo=" << moveTwo
              << " repeatedPayload=" << repeatedPayload
              << " nestedPayload=" << nestedPayload
              << " repeatedMarker=" << twiceResult.value(QStringLiteral("1:0")).path.points.size()
              << " moved-key=1:0->1:2 marker1At="
              << whereMarker(innerResult, 1).toStdString()
              << " marker2At=" << whereMarker(innerResult, 2).toStdString()
              << " outerMarker1At=" << whereMarker(outerResult, 1).toStdString()
              << " error=" << moveError.toStdString() << '\n';
    gates.gate(2, g2, "double remap changed surviving keys");

    // G3 detects undo snapshots that omit the carrier or its mask payload.
    Timeline undo;
    seed(undo, false);
    undo.undoManager()->clear();
    undo.undoManager()->saveState(undo.currentState(), QStringLiteral("基準"));
    QString undoError;
    const bool deleted = undo.deleteClipByIndex(false, 1, 0, false, &undoError);
    const auto afterDelete = undo.rotoEntries();
    undo.undo();
    const auto afterUndo = undo.rotoEntries();
    undo.redo();
    const auto afterRedo = undo.rotoEntries();
    std::cerr << "G3 delete=" << show(keys(afterDelete)).toStdString()
              << " undo=" << show(keys(afterUndo)).toStdString()
              << " redo=" << show(keys(afterRedo)).toStdString()
              << " points=" << afterUndo.value(QStringLiteral("1:1")).path.points.size()
              << "/" << afterRedo.value(QStringLiteral("1:0")).path.points.size()
              << " keyframes=" << afterUndo.value(QStringLiteral("1:1")).keyframes.size()
              << "/" << afterRedo.value(QStringLiteral("1:0")).keyframes.size() << '\n';
    gates.gate(3, deleted && keys(afterUndo).size() == 3
        && afterUndo.value(QStringLiteral("1:1")).path.points.size() == 2
        && afterUndo.value(QStringLiteral("1:1")).keyframes.size() == 1
        && keys(afterDelete) == keys(afterRedo)
        && afterRedo.value(QStringLiteral("1:0")).path.points.size() == 2
        && invariant(afterUndo) && invariant(afterRedo), "undo or redo lost roto payload");

    // G4 detects stale value clipIds after reindexing and lost brush masks.
    QTemporaryDir temporary;
    Timeline persisted;
    seed(persisted, false);
    ProjectData data;
    data.videoTracks = persisted.allVideoTracks();
    data.audioTracks = persisted.allAudioTracks();
    data.rotoClipEntries = {persisted.rotoEntries().value(QStringLiteral("1:0")),
                            persisted.rotoEntries().value(QStringLiteral("1:1"))};
    const QString path = temporary.filePath(QStringLiteral("roto.veditor"));
    ProjectData disk, jsonData;
    const bool saved = temporary.isValid() && ProjectFile::save(path, data);
    const bool loaded = saved && ProjectFile::load(path, disk);
    const QString json = ProjectFile::toJsonString(data);
    const bool jsonLoaded = ProjectFile::fromJsonString(json, jsonData);
    Timeline diskTimeline, jsonTimeline;
    diskTimeline.setRotoEntries(restoreEntries(disk.rotoClipEntries));
    jsonTimeline.setRotoEntries(restoreEntries(jsonData.rotoClipEntries));
    const auto diskEntries = diskTimeline.rotoEntries();
    const auto jsonEntries = jsonTimeline.rotoEntries();
    const QJsonArray serialized = QJsonDocument::fromJson(json.toUtf8()).object()
        .value(QStringLiteral("rotoClipEntries")).toArray();
    QSet<QString> fields;
    for (const auto &value : serialized)
        for (const auto &key : value.toObject().keys()) fields.insert(key);
    const QSet<QString> expectedPersistedKeys = keys(restoreEntries(data.rotoClipEntries));
    const bool diskFirst = samePayload(
        diskEntries.value(QStringLiteral("1:0")), data.rotoClipEntries[0]);
    const bool diskSecond = samePayload(
        diskEntries.value(QStringLiteral("1:1")), data.rotoClipEntries[1]);
    const bool jsonFirst = samePayload(
        jsonEntries.value(QStringLiteral("1:0")), data.rotoClipEntries[0]);
    const bool jsonSecond = samePayload(
        jsonEntries.value(QStringLiteral("1:1")), data.rotoClipEntries[1]);
    bool g4 = loaded && jsonLoaded
        && expectedPersistedKeys == QSet<QString>{QStringLiteral("1:0"), QStringLiteral("1:1")}
        && keys(diskEntries) == expectedPersistedKeys
        && keys(jsonEntries) == expectedPersistedKeys
        && diskFirst && diskSecond && jsonFirst && jsonSecond
        && serialized.size() == 2
        && !serialized[0].toObject().contains(QStringLiteral("brushMaskPngBase64"))
        && serialized[1].toObject().contains(QStringLiteral("brushMaskPngBase64"))
        && fields == QSet<QString>{QStringLiteral("clipId"), QStringLiteral("path"),
            QStringLiteral("keyframes"), QStringLiteral("brushMaskPngBase64")};
    QString editError;
    const bool edited = persisted.deleteClipByIndex(false, 1, 0, false, &editError);
    ProjectData editedData = data;
    editedData.videoTracks = persisted.allVideoTracks();
    editedData.rotoClipEntries.clear();
    for (const auto &value : persisted.rotoEntries())
        editedData.rotoClipEntries.append(value);
    const QString editedPath = temporary.filePath(QStringLiteral("edited.veditor"));
    ProjectData editedLoaded;
    const bool editedSaved = ProjectFile::save(editedPath, editedData);
    const bool editedRead = editedSaved && ProjectFile::load(editedPath, editedLoaded);
    Timeline editedTimeline;
    editedTimeline.setRotoEntries(restoreEntries(editedLoaded.rotoClipEntries));
    const auto editedEntry = editedTimeline.rotoEntries().value(QStringLiteral("1:0"));
    const QJsonArray editedSerialized = QJsonDocument::fromJson(
        ProjectFile::toJsonString(editedData).toUtf8()).object()
        .value(QStringLiteral("rotoClipEntries")).toArray();
    bool editedJsonClipId = false;
    for (const auto &value : editedSerialized) {
        const auto object = value.toObject();
        if (object.value(QStringLiteral("clipId")).toString() == QStringLiteral("1:0")
            && object.contains(QStringLiteral("brushMaskPngBase64")))
            editedJsonClipId = true;
    }
    g4 = g4 && edited && editedRead && editedEntry.clipId == QStringLiteral("1:0")
        && editedEntry.path.points.size() == 2 && editedEntry.brushMask.size() == QSize(3, 2)
        && editedJsonClipId;
    std::cerr << "G4 disk=" << show(keys(diskEntries)).toStdString()
              << " json=" << show(keys(jsonEntries)).toStdString()
              << " expected=" << show(expectedPersistedKeys).toStdString()
              << " payload disk=" << diskFirst << '/' << diskSecond
              << " json=" << jsonFirst << '/' << jsonSecond
              << " fields=" << show(fields).toStdString()
              << " edited=" << show(keys(editedTimeline.rotoEntries())).toStdString()
              << " saved=" << saved << " loaded=" << loaded
              << " jsonLoaded=" << jsonLoaded << " editedRead=" << editedRead
              << " editedJsonClipId=" << editedJsonClipId
              << " clipId=" << editedEntry.clipId.toStdString()
              << " points=" << editedEntry.path.points.size()
              << " mask=" << editedEntry.brushMask.width() << 'x'
              << editedEntry.brushMask.height() << '\n';
    gates.gate(4, g4, "project roundtrip or edited clipId failed");

    // G5 detects a file-backed queue resolver that omits or fails to normalize roto.
    ProjectData queueData = data;
    queueData.rotoClipEntries[0].clipId = QStringLiteral("01:00");
    const QString queuePath = temporary.filePath(QStringLiteral("queue.veditor"));
    const bool queueSaved = ProjectFile::save(queuePath, queueData);
    RenderJob job;
    job.projectFilePath = queuePath;
    job.outputPath = temporary.filePath(QStringLiteral("queue-output.mp4"));
    job.width = 64;
    job.height = 64;
    job.codec = QStringLiteral("mpeg4");
    job.bitrateBps = 600000;
    job.endUs = 100000; // one frame is enough to exercise the file-backed resolver
    job.exportConfig.insert(QStringLiteral("fps"), 10);
    RenderQueue queue;
    queue.setProperty("captureResolvedRotoClipIdsForTest", true);
    QSignalSpy finished(&queue, &RenderQueue::jobCompletedUuid);
    if (queueSaved) {
        queue.addJob(job);
        queue.start();
        if (finished.isEmpty()) finished.wait(60000);
    }
    const QJsonObject resolvedIds =
        queue.property("resolvedRotoClipIdsForTest").toJsonObject();
    QSet<QString> resolvedKeys;
    for (const QString &key : resolvedIds.keys()) resolvedKeys.insert(key);
    const bool completed = !finished.isEmpty() && finished.first().value(1).toBool();
    const bool g5 = queueSaved && completed && resolvedIds.size() == 2
        && resolvedIds.value(QStringLiteral("1:0")).toString() == QStringLiteral("1:0")
        && resolvedIds.value(QStringLiteral("1:1")).toString() == QStringLiteral("1:1");
    std::cerr << "G5 restored=" << show(resolvedKeys).toStdString()
              << " count=" << resolvedIds.size()
              << " canonicalClipId="
              << resolvedIds.value(QStringLiteral("1:0")).toString().toStdString()
              << " completed=" << completed
              << " error=" << (finished.isEmpty() ? QStringLiteral("timeout")
                  : finished.first().value(2).toString()).toStdString() << '\n';
    gates.gate(5, g5, "RenderQueue file resolver omitted or failed to canonicalize roto");

    // G6 detects the roto-only early-return bug in the clipMoved handler.
    Timeline reordered;
    seed(reordered, false);
    const auto oldKeys = keys(reordered.rotoEntries());
    reordered.videoTracks()[1]->moveClip(0, 1);
    const auto newEntries = reordered.rotoEntries();
    const bool g6 = keys(newEntries) == oldKeys && invariant(newEntries)
        && newEntries.value(QStringLiteral("1:0")).path.points.size() == 2
        && newEntries.value(QStringLiteral("1:1")).path.points.size() == 1;
    std::cerr << "G6 from=0 to=1 before=" << show(oldKeys).toStdString()
              << " after=" << show(keys(newEntries)).toStdString()
              << " markers=" << newEntries.value(QStringLiteral("1:0")).path.points.size()
              << '/' << newEntries.value(QStringLiteral("1:1")).path.points.size() << '\n';
    gates.gate(6, g6, "clipMoved failed to reorder the roto carrier");

    // G7 detects an inverted or shifted roto mask (interior must be kept) and
    // keyframes losing priority over the single path.
    const QImage source = libavcore::grabFrameAt(gSyntheticClipPath, 0.5);
    RotoClipEntry hard;
    hard.path = rectPath(16, 16, 48, 48);
    const QImage hardOut = roto::applyEntryToFrame(hard, source, 5);
    const MaskStats hardStats = maskStats(source, hardOut,
        [](int x, int y) { return inRect(x, y, 18, 18, 46, 46); },
        [](int x, int y) { return !inRect(x, y, 14, 14, 50, 50); });
    RotoClipEntry soft;
    soft.path = rectPath(16, 16, 48, 48, 3.0);
    const QImage softOut = roto::applyEntryToFrame(soft, source, 5);
    // Three box-blur passes of radius 3 reach 9 px past the edge.
    const MaskStats softStats = maskStats(source, softOut,
        [](int x, int y) { return inRect(x, y, 26, 26, 38, 38); },
        [](int x, int y) { return !inRect(x, y, 6, 6, 58, 58); });
    RotoClipEntry keyed;
    keyed.path = rectPath(16, 16, 48, 48);
    RotoKeyframe keyedFrame;
    keyedFrame.frameNumber = 0;
    keyedFrame.path = rectPath(0, 0, 24, 64);
    keyed.keyframes.append(keyedFrame);
    const QImage keyedOut = roto::applyEntryToFrame(keyed, source, 5);
    const MaskStats keyedStats = maskStats(source, keyedOut,
        [](int x, int y) { return inRect(x, y, 2, 2, 22, 62); },
        [](int x, int y) { return inRect(x, y, 26, 18, 46, 46); });
    printStats("G7 path feather=0", hardStats);
    printStats("G7 path feather=3", softStats);
    printStats("G7 keyframes>path", keyedStats);
    std::cerr << "G7 source=" << source.width() << 'x' << source.height()
              << " format=" << int(hardOut.format()) << '\n';
    const bool g7 = source.size() == QSize(64, 64)
        && cleanMask(hardStats) && hardStats.edgePixels == 0
        && cleanMask(softStats) && softStats.edgePixels > 0
        && cleanMask(keyedStats);
    gates.gate(7, g7, "roto interior not kept, exterior not cleared, or keyframes lost priority");

    // G8 detects a dropped brush max-combine, an altered pass-through for
    // empty entries / null frames, and a preview that bypasses
    // roto::sourceFrameIndexFor (its call counter stays flat).
    QImage brush(64, 64, QImage::Format_Grayscale8);
    brush.fill(0);
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            brush.scanLine(y)[x] = 255;
    RotoClipEntry brushOnly;
    brushOnly.brushMask = brush;
    const MaskStats brushStats = maskStats(source, roto::applyEntryToFrame(brushOnly, source, 5),
        [](int x, int y) { return inRect(x, y, 8, 8, 24, 24); },
        [](int x, int y) { return !inRect(x, y, 6, 6, 26, 26); });
    RotoClipEntry pathBrush;
    pathBrush.path = rectPath(32, 32, 56, 56);
    pathBrush.brushMask = brush;
    const MaskStats unionStats = maskStats(source, roto::applyEntryToFrame(pathBrush, source, 5),
        [](int x, int y) { return inRect(x, y, 8, 8, 24, 24) || inRect(x, y, 34, 34, 54, 54); },
        [](int x, int y) { return !inRect(x, y, 6, 6, 26, 26) && !inRect(x, y, 30, 30, 58, 58); });
    printStats("G8 brush-only", brushStats);
    printStats("G8 path+brush", unionStats);
    const bool emptySame = roto::applyEntryToFrame(RotoClipEntry{}, source, 5) == source;
    const bool nullSame = roto::applyEntryToFrame(hard, QImage(), 5).isNull();
    struct IndexCase { double sec; double fps; int expected; };
    const IndexCase indexCases[] = {
        {1.0, 30.0, 30}, {2.0, 29.97, 60}, {-0.4, 30.0, 0}, {0.25, 10.0, 3}, {0.0, 24.0, 0}};
    bool indexOk = true;
    for (const auto &c : indexCases) {
        const int actual = roto::sourceFrameIndexFor(c.sec, c.fps);
        const int viaLlround = qMax(0, static_cast<int>(std::llround(c.sec * c.fps)));
        std::cerr << "G8 sourceFrameIndexFor(" << c.sec << ',' << c.fps << ")=" << actual
                  << " llround=" << viaLlround << " expected=" << c.expected << '\n';
        indexOk = indexOk && actual == viaLlround && actual == c.expected;
    }
    MainWindow window;
    Timeline *wtl = window.m_timeline;
    window.m_projectConfig.width = 64;
    window.m_projectConfig.height = 64;
    window.m_projectConfig.fps = 10;
    ClipInfo remapped = clip(70);
    remapped.timeRemapCurve.sourceFps = 10.0;
    remapped.timeRemapCurve.addKey(0.0, 0.0);
    remapped.timeRemapCurve.addKey(1.0, 1.0);
    wtl->restoreFromProject(QVector<QVector<ClipInfo>>{{remapped}},
                            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    wtl->setRotoEntries({});
    const quint64 remapBefore = roto::sourceFrameIndexForCallCount();
    const QImage remapPreview = window.buildSpecialClipComposite(0.5);
    const quint64 remapCalls = roto::sourceFrameIndexForCallCount() - remapBefore;
    wtl->restoreFromProject(QVector<QVector<ClipInfo>>{{clip(71)}},
                            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    QHash<QString, RotoClipEntry> plainRoto;
    plainRoto.insert(QStringLiteral("0:0"), hard);
    wtl->setRotoEntries(plainRoto);
    const quint64 plainBefore = roto::sourceFrameIndexForCallCount();
    const QImage plainPreview = window.buildSpecialClipComposite(0.5);
    const quint64 plainCalls = roto::sourceFrameIndexForCallCount() - plainBefore;
    wtl->setRotoEntries({});
    std::cerr << "G8 emptySame=" << emptySame << " nullSame=" << nullSame
              << " indexOk=" << indexOk << " remapPreviewCalls=" << remapCalls
              << " remapPreviewNull=" << remapPreview.isNull()
              << " plainRotoPreviewCalls=" << plainCalls
              << " plainPreviewNull=" << plainPreview.isNull() << '\n';
    const bool g8 = cleanMask(brushStats) && cleanMask(unionStats) && emptySame && nullSame
        && indexOk && remapCalls > 0 && !remapPreview.isNull()
        && plainCalls > 0 && !plainPreview.isNull();
    gates.gate(8, g8, "brush max-combine, pass-through or shared source frame index failed");

    const auto seedWindow = [](MainWindow &window) {
        Timeline *tl = window.m_timeline;
        ClipInfo gappedV1 = clip(101);
        gappedV1.leadInSec = 1.0;
        ClipInfo gappedV2 = clip(2);
        gappedV2.leadInSec = 1.0;
        ClipInfo source = clip(4);
        source.leadInSec = 10.0;
        const QVector<QVector<ClipInfo>> video{
            {clip(100), gappedV1}, {clip(1), gappedV2}, {clip(3)}, {source}};
        const QVector<QVector<ClipInfo>> audio{{}};
        tl->clearSelection();
        tl->restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
        QHash<QString, RotoClipEntry> rotos;
        rotos.insert(QStringLiteral("1:0"), entry(1));
        rotos.insert(QStringLiteral("1:1"), entry(2, true));
        rotos.insert(QStringLiteral("2:0"), entry(3));
        rotos.insert(QStringLiteral("0:1"), entry(4));
        tl->setRotoEntries(rotos);
        window.m_trackMatteClipEntries.clear();
        QHash<QString, TimelineTrackMatteEntry> carrier;
        QHash<QString, QString> parents;
        for (auto it = rotos.cbegin(); it != rotos.cend(); ++it) {
            TrackMatteClipEntry m;
            m.clipId = it.key();
            m.matteType = matteTypeForMarker(it.value().path.points.size());
            m.matteSourceClipId = QStringLiteral("3:0");
            window.m_trackMatteClipEntries.insert(it.key(), m);
            TimelineTrackMatteEntry c;
            c.matteType = m.matteType;
            c.matteSourceClipId = m.matteSourceClipId;
            carrier.insert(it.key(), c);
            parents.insert(it.key(), QStringLiteral("3:0"));
        }
        tl->setTrackMatteEntries(carrier);
        tl->setClipParentEntries(parents);
        tl->undoManager()->clear();
        tl->undoManager()->saveState(tl->currentState(), QStringLiteral("基準"));
    };

    // G9 detects a writer that bypasses the carrier, keeps a stale value
    // clipId, or skips the single undo step.
    seedWindow(window);
    UndoManager *um = wtl->undoManager();
    const quint64 serialBefore = um->saveSerial();
    RotoClipEntry stored = entry(6);
    stored.clipId = QStringLiteral("9:9");
    window.storeRotoEntry(QStringLiteral("0:0"), stored);
    const quint64 serialAfter = um->saveSerial();
    const auto afterStore = wtl->rotoEntries();
    wtl->undo();
    const auto afterStoreUndo = wtl->rotoEntries();
    wtl->redo();
    const auto afterStoreRedo = wtl->rotoEntries();
    ProjectData populated;
    window.populateProjectData(populated);
    const auto populatedEntries = restoreEntries(populated.rotoClipEntries);
    bool populatedSame = populatedEntries.size() == afterStoreRedo.size()
        && populated.rotoClipEntries.size() == afterStoreRedo.size();
    for (auto it = afterStoreRedo.cbegin(); it != afterStoreRedo.cend(); ++it)
        populatedSame = populatedSame && samePayload(populatedEntries.value(it.key()), it.value());
    const QSet<QString> seeded{QStringLiteral("0:1"), QStringLiteral("1:0"),
                               QStringLiteral("1:1"), QStringLiteral("2:0")};
    QSet<QString> withStored = seeded;
    withStored.insert(QStringLiteral("0:0"));
    std::cerr << "G9 store=" << show(keys(afterStore)).toStdString()
              << " clipId=" << afterStore.value(QStringLiteral("0:0")).clipId.toStdString()
              << " saveSerial=" << serialBefore << "->" << serialAfter
              << " undo=" << show(keys(afterStoreUndo)).toStdString()
              << " redo=" << show(keys(afterStoreRedo)).toStdString()
              << " redoPoints=" << afterStoreRedo.value(QStringLiteral("0:0")).path.points.size()
              << " populated=" << show(keys(populatedEntries)).toStdString()
              << " populatedSame=" << populatedSame << '\n';
    const bool g9 = keys(afterStore) == withStored
        && afterStore.value(QStringLiteral("0:0")).clipId == QStringLiteral("0:0")
        && afterStore.value(QStringLiteral("0:0")).path.points.size() == 6
        && serialAfter == serialBefore + 1
        && keys(afterStoreUndo) == seeded
        && keys(afterStoreRedo) == withStored
        && afterStoreRedo.value(QStringLiteral("0:0")).path.points.size() == 6
        && invariant(afterStore) && invariant(afterStoreRedo) && populatedSame;
    gates.gate(9, g9, "storeRotoEntry carrier, clipId, undo serial or populateProjectData mismatch");

    // G10 detects roto keys drifting from the MainWindow matte keys on the GUI
    // edit paths (each path remaps twice: inside Timeline and in MainWindow).
    // The text-based range ripple compares against parents (matte does not
    // follow that path, an existing defect).
    struct EditPath { const char *name; std::function<void()> run; bool parentPeer; };
    const auto select = [&](int track, int clipIndex) {
        wtl->clearSelection();
        QString error;
        wtl->selectClipByIndex(false, track, clipIndex, &error);
    };
    const QVector<EditPath> editPaths{
        // MainWindow::deleteClip only runs with a V1 (primary track) selection.
        {"deleteClip", [&] { select(1, 0); wtl->videoTracks()[0]->toggleClipSelection(0);
                             window.deleteClip(); }, false},
        {"splitClip", [&] { wtl->setPlayheadPosition(1.0); select(1, 0); window.splitClip(); }, false},
        {"rippleDelete", [&] { select(1, 0); window.rippleDelete(); }, false},
        // Timeline::pasteClip copies from / inserts into V1 (the audio track is
        // empty, so the insert lands at V1:0 and shifts the V1:1 roto).
        {"pasteClip", [&] { select(0, 0); window.copyClip(); wtl->setPlayheadPosition(0.0);
                            window.pasteClip(); }, false},
        {"moveClipHeadToPlayhead", [&] { wtl->setPlayheadPosition(6.0); select(1, 0);
                                         window.m_moveClipHeadAction->trigger(); }, false},
        {"duplicate", [&] { select(1, 0); window.m_duplicateAction->trigger(); }, false},
        {"nudge-1", [&] { select(1, 1); window.m_nudgeLeftAction->trigger(); }, false},
        {"nudge+1", [&] { select(1, 1); window.m_nudgeRightAction->trigger(); }, false},
        {"nudge-10", [&] { select(1, 1); window.m_nudgeLeft10Action->trigger(); }, false},
        {"nudge+10", [&] { select(1, 0); window.m_nudgeRight10Action->trigger(); }, false},
        {"closeAllGaps", [&] { window.m_closeAllGapsAction->trigger(); }, false},
        {"bladeAll", [&] { wtl->setPlayheadPosition(1.0); window.m_bladeAllAction->trigger(); }, false},
        {"lift", [&] { select(1, 0); window.m_liftAction->trigger(); }, false},
        {"textRangeRipple", [&] { select(1, 0); wtl->rippleDeleteTimeRangeActive(0.0, 2.0); }, true},
    };
    bool g10 = true;
    for (const auto &path : editPaths) {
        seedWindow(window);
        const QString layoutBefore = layoutSignature(wtl);
        path.run();
        const QString layoutAfter = layoutSignature(wtl);
        const auto rotos = wtl->rotoEntries();
        const QSet<QString> rotoKeys = keys(rotos);
        const QSet<QString> peerKeys = path.parentPeer
            ? keys(wtl->clipParentEntries()) : keys(window.m_trackMatteClipEntries);
        QStringList pairing;
        bool paired = true;
        if (!path.parentPeer) {
            for (auto it = rotos.cbegin(); it != rotos.cend(); ++it) {
                const int marker = it.value().path.points.size();
                const TrackMatteType matte = window.m_trackMatteClipEntries.value(it.key()).matteType;
                if (matte != matteTypeForMarker(marker)) {
                    paired = false;
                    pairing.append(QStringLiteral("%1:marker%2/matte%3").arg(it.key())
                                       .arg(marker).arg(int(matte)));
                }
            }
        }
        const bool changed = layoutBefore != layoutAfter;
        const bool one = changed && rotoKeys == peerKeys && invariant(rotos) && paired
            && rotoKeys.size() >= 2;
        std::cerr << "G10 " << path.name << " roto=" << show(rotoKeys).toStdString()
                  << " peer(" << (path.parentPeer ? "parent" : "matte") << ")="
                  << show(peerKeys).toStdString()
                  << " rotoOnly=" << show(rotoKeys - peerKeys).toStdString()
                  << " peerOnly=" << show(peerKeys - rotoKeys).toStdString()
                  << " marker1At=" << whereMarker(rotos, 1).toStdString()
                  << " marker2At=" << whereMarker(rotos, 2).toStdString()
                  << " marker3At=" << whereMarker(rotos, 3).toStdString()
                  << " marker4At=" << whereMarker(rotos, 4).toStdString()
                  << " pairingMismatch=" << pairing.join(QLatin1Char(',')).toStdString()
                  << " clipIdOk=" << invariant(rotos) << " changed=" << changed
                  << " layout=" << layoutAfter.toStdString() << '\n';
        g10 = g10 && one;
    }
    gates.gate(10, g10, "roto keys drifted from matte/parent keys on a MainWindow edit path");

    // G11 detects a project load that sets roto after the undo baseline (an
    // edit + undo would then wipe the loaded roto).
    seedWindow(window);
    ProjectData project;
    window.populateProjectData(project);
    wtl->setRotoEntries({});
    window.applyLoadedProjectData(project, QString());
    const auto afterLoad = wtl->rotoEntries();
    const bool canUndoAfterLoad = wtl->undoManager()->canUndo();
    wtl->undo();
    const auto afterLoadUndo = wtl->rotoEntries();
    window.storeRotoEntry(QStringLiteral("0:0"), entry(7));
    const auto afterLoadEdit = wtl->rotoEntries();
    wtl->undo();
    const auto afterLoadEditUndo = wtl->rotoEntries();
    std::cerr << "G11 projectRoto=" << project.rotoClipEntries.size()
              << " load=" << show(keys(afterLoad)).toStdString()
              << " canUndoAfterLoad=" << canUndoAfterLoad
              << " undo=" << show(keys(afterLoadUndo)).toStdString()
              << " edit=" << show(keys(afterLoadEdit)).toStdString()
              << " editUndo=" << show(keys(afterLoadEditUndo)).toStdString()
              << " marker2At=" << whereMarker(afterLoadEditUndo, 2).toStdString() << '\n';
    QSet<QString> loadEdited = seeded;
    loadEdited.insert(QStringLiteral("0:0"));
    const bool g11 = project.rotoClipEntries.size() == 4 && keys(afterLoad) == seeded
        && keys(afterLoadUndo) == seeded && keys(afterLoadEdit) == loadEdited
        && keys(afterLoadEditUndo) == seeded
        && whereMarker(afterLoadEditUndo, 2) == QStringLiteral("1:1")
        && invariant(afterLoadEditUndo);
    gates.gate(11, g11, "loaded roto missing from the post-load undo baseline");

    // G12 detects an empty dialog result that leaves the entry in the carrier
    // or removes it outside the undo history.
    seedWindow(window);
    const quint64 clearSerialBefore = um->saveSerial();
    window.storeRotoEntry(QStringLiteral("1:1"), RotoClipEntry{});
    const quint64 clearSerialAfter = um->saveSerial();
    const auto afterClear = wtl->rotoEntries();
    wtl->undo();
    const auto afterClearUndo = wtl->rotoEntries();
    const RotoClipEntry restored = afterClearUndo.value(QStringLiteral("1:1"));
    std::cerr << "G12 clear=" << show(keys(afterClear)).toStdString()
              << " saveSerial=" << clearSerialBefore << "->" << clearSerialAfter
              << " undo=" << show(keys(afterClearUndo)).toStdString()
              << " points=" << restored.path.points.size()
              << " mask=" << restored.brushMask.width() << 'x' << restored.brushMask.height() << '\n';
    const bool g12 = keys(afterClear) == QSet<QString>{QStringLiteral("0:1"), QStringLiteral("1:0"),
                                                       QStringLiteral("2:0")}
        && clearSerialAfter == clearSerialBefore + 1
        && keys(afterClearUndo) == seeded && restored.clipId == QStringLiteral("1:1")
        && restored.path.points.size() == 2 && restored.brushMask.size() == QSize(3, 2);
    gates.gate(12, g12, "empty roto entry not removed from the carrier or not undoable");

    // ── G13-G21 (US-908): roto in tlrender::renderFrameAt, Smart Render and
    // RenderInPlace. outSize = the 64x64 fixture resolution, no proxy.
    const QSize frameSize(64, 64);
    const auto renderAt = [&](const Timeline *tl, double timelineSec) {
        return tlrender::renderFrameAt(tl, qRound64(timelineSec * 1000000.0), frameSize);
    };
    const auto oneTrack = [](const QVector<ClipInfo> &clips) {
        return QVector<QVector<ClipInfo>>{clips};
    };
    const auto rotoAt = [](const QString &key, const RotoClipEntry &value) {
        QHash<QString, RotoClipEntry> entries;
        entries.insert(key, value);
        return entries;
    };
    RotoClipEntry hardRect;
    hardRect.path = rectPath(16, 16, 48, 48);
    const auto insideRect = [](int x, int y) { return inRect(x, y, 18, 18, 46, 46); };
    const auto outsideRect = [](int x, int y) { return !inRect(x, y, 14, 14, 50, 50); };
    const auto everyPixel = [](int, int) { return true; };
    const QString redClipPath = media.filePath(QStringLiteral("roto-synthetic-red.mp4"));
    const bool redReady = writeSyntheticClip(redClipPath, QColor(200, 60, 40));

    // G13 detects a missing, inverted or shifted roto stage on the V1 layer
    // of renderFrameAt (interior kept, exterior alpha 0 over an empty canvas).
    Timeline v1Timeline;
    v1Timeline.restoreFromProject(oneTrack({clip(130)}), QVector<QVector<ClipInfo>>{},
                                  0.0, -1.0, -1.0, 100);
    const QImage v1Plain = renderAt(&v1Timeline, 0.5);
    v1Timeline.setRotoEntries(rotoAt(QStringLiteral("0:0"), hardRect));
    const quint64 v1CallsBefore = roto::renderApplyCallCount();
    const QImage v1Roto = renderAt(&v1Timeline, 0.5);
    const quint64 v1Calls = roto::renderApplyCallCount() - v1CallsBefore;
    const MaskStats v1Stats = maskStats(v1Plain, v1Roto, insideRect, outsideRect);
    const QRgb v1PlainCorner = v1Plain.isNull() ? 0 : v1Plain.pixel(2, 2);
    printStats("G13 V1 roto vs no-entry", v1Stats);
    std::cerr << "G13 plainCorner=" << qRed(v1PlainCorner) << ',' << qGreen(v1PlainCorner) << ','
              << qBlue(v1PlainCorner) << ',' << qAlpha(v1PlainCorner)
              << " plainFormat=" << int(v1Plain.format()) << " rotoFormat=" << int(v1Roto.format())
              << " renderApplyCalls=" << v1Calls << '\n';
    gates.gate(13, cleanMask(v1Stats) && qAlpha(v1PlainCorner) == 255 && qBlue(v1PlainCorner) > 100
                   && v1Roto.format() == v1Plain.format() && v1Calls > 0,
               "V1 roto interior not kept or exterior not alpha 0 in renderFrameAt");

    // G14 detects preview/export roto drift (shifted source frame index,
    // double application, feather at another resolution): the special-
    // composite preview and renderFrameAt agree within maxDiff <= 2 on a
    // 24 fps timeline (fixture stream 10 fps) at keyframe boundary frames
    // 4 / 5 / 6. In-binary controls: re-applying the entry to the export, and
    // the export one frame later, must both exceed the threshold.
    window.m_projectConfig.width = 64;
    window.m_projectConfig.height = 64;
    window.m_projectConfig.fps = 24;
    wtl->setProjectOutputConfig(64, 64, true, 24.0);
    window.m_trackMatteClipEntries.clear();
    wtl->setTrackMatteEntries(QHash<QString, TimelineTrackMatteEntry>{});
    wtl->setClipParentEntries(QHash<QString, QString>{});
    wtl->restoreFromProject(oneTrack({clip(140)}), QVector<QVector<ClipInfo>>{},
                            0.0, -1.0, -1.0, 100);
    RotoClipEntry featherForm;
    featherForm.path = rectPath(16, 16, 48, 48, 3.0);
    RotoClipEntry keyedForm;
    RotoKeyframe key4;
    key4.frameNumber = 4;
    key4.path = rectPath(6, 8, 30, 56, 3.0);
    RotoKeyframe key5;
    key5.frameNumber = 5;
    key5.path = rectPath(34, 8, 58, 56, 3.0);
    keyedForm.keyframes = {key4, key5};
    RotoClipEntry brushForm;
    brushForm.brushMask = QImage(64, 64, QImage::Format_Grayscale8);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int dist = qMax(qMax(20 - x, x - 43), qMax(20 - y, y - 43));
            brushForm.brushMask.scanLine(y)[x] = uchar(qBound(0, 255 - 32 * qMax(0, dist), 255));
        }
    }
    struct ParityForm { const char *name; RotoClipEntry entry; };
    const QVector<ParityForm> parityForms{
        {"path", featherForm}, {"keyframes", keyedForm}, {"brushMask", brushForm}};
    bool g14 = true;
    for (const auto &form : parityForms) {
        wtl->setRotoEntries(rotoAt(QStringLiteral("0:0"), form.entry));
        for (double timelineSec : {0.4, 0.5, 0.6}) {
            const double localSec = timelineSec;   // clip starts at 0
            const double sourceSec = 0.0 + localSec; // inPoint 0, speed 1
            const int sourceFrameIndex = roto::sourceFrameIndexFor(sourceSec, 10.0);
            const QImage preview = window.buildSpecialClipComposite(timelineSec);
            const QImage exported = renderAt(wtl, timelineSec);
            const RegionDiff parity = regionDiff(preview, exported, everyPixel);
            const QImage doubled = roto::applyEntryToFrame(
                form.entry, exported.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                sourceFrameIndex);
            const RegionDiff doubledDiff = regionDiff(preview, doubled, everyPixel);
            const RegionDiff shiftedDiff = regionDiff(preview, renderAt(wtl, timelineSec + 0.1), everyPixel);
            int featherPixels = 0;
            for (int y = 0; y < exported.height(); ++y)
                for (int x = 0; x < exported.width(); ++x) {
                    const int a = qAlpha(exported.pixel(x, y));
                    if (a > 0 && a < 255) ++featherPixels;
                }
            const bool shiftMatters = std::string(form.name) == "keyframes" && sourceFrameIndex == 4;
            const bool ok = parity.pixels == 64 * 64 && parity.maxDiff <= 2 && featherPixels > 0
                && doubledDiff.maxDiff > 2 && (!shiftMatters || shiftedDiff.maxDiff > 2);
            std::cerr << "G14 form=" << form.name << " timelineSec=" << timelineSec
                      << " localSec=" << localSec << " sourceSec=" << sourceSec
                      << " sourceFrameIndex=" << sourceFrameIndex
                      << " fps stream=10 previewProject=" << window.m_projectConfig.fps
                      << " rendererProject=" << wtl->projectFrameRate()
                      << " parity " << showDiff(parity) << " featherPixels=" << featherPixels
                      << " doubleApplied " << showDiff(doubledDiff)
                      << " nextFrame " << showDiff(shiftedDiff)
                      << (shiftMatters ? " (keyframe boundary)" : "") << " ok=" << ok << '\n';
            g14 = g14 && ok;
        }
    }
    // Reversed form: reverse decode keeps the frame before the source second,
    // so preview and export both index the mask one source frame earlier
    // (sourceSec - 1/fps). Local 1.4 / 1.5 / 1.6 -> index 5 / 4 / 3 around the
    // 4|5 keyframe boundary. Control: the export 0.1 s earlier (index + 1, the
    // unadjusted index) must differ at the boundary.
    ClipInfo reversedClip = clip(141);
    reversedClip.reversed = true;
    wtl->restoreFromProject(oneTrack({reversedClip}), QVector<QVector<ClipInfo>>{},
                            0.0, -1.0, -1.0, 100);
    wtl->setRotoEntries(rotoAt(QStringLiteral("0:0"), keyedForm));
    const QVector<QPair<double, int>> reversedTimes{{1.4, 5}, {1.5, 4}, {1.6, 3}};
    for (const auto &[timelineSec, expectedIndex] : reversedTimes) {
        const double localSec = timelineSec;   // clip starts at 0
        const double sourceSec = reversedClip.sourceSecondAtLocalTime(localSec);
        const bool runsBackward = reversedClip.sourceTimeRunsBackwardAtLocalTime(localSec);
        const double previewSourceSec = runsBackward
            ? qMax(reversedClip.inPoint, sourceSec - 1.0 / 10.0) : sourceSec;
        const int previewIndex = roto::sourceFrameIndexFor(previewSourceSec, 10.0);
        const QImage preview = window.buildSpecialClipComposite(timelineSec);
        const QImage exported = renderAt(wtl, timelineSec);
        const int rendererIndex = roto::lastRenderSourceFrameIndex();
        const RegionDiff parity = regionDiff(preview, exported, everyPixel);
        const RegionDiff shiftedDiff = regionDiff(preview, renderAt(wtl, timelineSec - 0.1), everyPixel);
        const bool boundary = expectedIndex == 4;
        const bool ok = parity.pixels == 64 * 64 && parity.maxDiff <= 2
            && previewIndex == expectedIndex && rendererIndex == expectedIndex
            && (!boundary || shiftedDiff.maxDiff > 2);
        std::cerr << "G14 form=reversed timelineSec=" << timelineSec << " localSec=" << localSec
                  << " sourceSec=" << sourceSec << " runsBackward=" << runsBackward
                  << " preview sourceSec=" << previewSourceSec << " sourceFrameIndex=" << previewIndex
                  << " renderer sourceFrameIndex=" << rendererIndex << " (expected " << expectedIndex << ")"
                  << " fps stream=10 previewProject=" << window.m_projectConfig.fps
                  << " rendererProject=" << wtl->projectFrameRate()
                  << " parity " << showDiff(parity)
                  << " unadjustedIndex(+1) " << showDiff(shiftedDiff)
                  << (boundary ? " (keyframe boundary)" : "") << " ok=" << ok << '\n';
        g14 = g14 && ok;
    }
    wtl->setRotoEntries(QHash<QString, RotoClipEntry>{});
    gates.gate(14, g14, "special-composite preview and renderFrameAt roto differ (maxDiff > 2) or a control did not separate");

    // G15 detects a missing roto stage on upper-track layers: V1 hidden, V2
    // (red, entry) over V3 (blue). Interior keeps V2, exterior shows V3.
    ClipInfo redV2 = clip(151);
    redV2.filePath = redClipPath;
    Timeline upper;
    upper.restoreFromProject(QVector<QVector<ClipInfo>>{{clip(150)}, {redV2}, {clip(152)}},
                             QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    upper.videoTracks()[0]->setHidden(true);
    const QImage upperPlain = renderAt(&upper, 0.5);
    upper.videoTracks()[1]->setHidden(true);
    const QImage upperV3Only = renderAt(&upper, 0.5);
    upper.videoTracks()[1]->setHidden(false);
    upper.setRotoEntries(rotoAt(QStringLiteral("1:0"), hardRect));
    const quint64 upperCallsBefore = roto::renderApplyCallCount();
    const QImage upperRoto = renderAt(&upper, 0.5);
    const quint64 upperCalls = roto::renderApplyCallCount() - upperCallsBefore;
    const RegionDiff upperInside = regionDiff(upperPlain, upperRoto, insideRect);
    const RegionDiff upperOutside = regionDiff(upperV3Only, upperRoto, outsideRect);
    const RegionDiff upperSanity = regionDiff(upperPlain, upperV3Only, outsideRect);
    std::cerr << "G15 inside(roto vs no-entry) " << showDiff(upperInside)
              << " outside(roto vs V3 only) " << showDiff(upperOutside)
              << " sanity outside(no-entry vs V3 only) " << showDiff(upperSanity)
              << " redReady=" << redReady << " renderApplyCalls=" << upperCalls << '\n';
    gates.gate(15, redReady && upperInside.pixels > 0 && upperInside.maxDiff <= 2
                   && upperOutside.pixels > 0 && upperOutside.maxDiff <= 2
                   && upperSanity.diffPixels == upperSanity.pixels && upperCalls > 0,
               "upper-layer roto interior not kept or exterior does not reveal the lower track");

    // G16 detects roto leaking into nested-sequence layers (sequenceDepth > 0):
    // the child's 0:1 key collides with a top-level key format. Control: the
    // same clips flat on V1 with the same entry do change.
    TimelineSequence child;
    child.id = child.name = QStringLiteral("roto-child");
    ClipInfo childFirst = clip(160);
    childFirst.outPoint = 1.0;
    ClipInfo childSecond = clip(161);
    childSecond.outPoint = 1.0;
    child.videoTracks = {{childFirst, childSecond}};
    ClipInfo nestedRef = clip(162);
    nestedRef.filePath = timeline_nesting::sequenceClipFilePath(child.id);
    nestedRef.sequenceRefId = child.id;
    TimelineSequence mainSequence;
    mainSequence.id = mainSequence.name = QStringLiteral("roto-main");
    mainSequence.videoTracks = {{nestedRef}};
    Timeline nestedTimeline;
    nestedTimeline.setSequences({mainSequence, child}, mainSequence.id);
    const QImage nestedPlain = renderAt(&nestedTimeline, 1.5);
    nestedTimeline.setRotoEntries(rotoAt(QStringLiteral("0:1"), hardRect));
    const quint64 nestedCallsBefore = roto::renderApplyCallCount();
    const QImage nestedRoto = renderAt(&nestedTimeline, 1.5);
    const quint64 nestedCalls = roto::renderApplyCallCount() - nestedCallsBefore;
    Timeline flat;
    flat.restoreFromProject(oneTrack({childFirst, childSecond}), QVector<QVector<ClipInfo>>{},
                            0.0, -1.0, -1.0, 100);
    const QImage flatPlain = renderAt(&flat, 1.5);
    flat.setRotoEntries(rotoAt(QStringLiteral("0:1"), hardRect));
    const QImage flatRoto = renderAt(&flat, 1.5);
    const RegionDiff nestedDiff = regionDiff(nestedPlain, nestedRoto, everyPixel);
    const RegionDiff flatDiff = regionDiff(flatPlain, flatRoto, outsideRect);
    std::cerr << "G16 nested(entry vs none) " << showDiff(nestedDiff)
              << " nestedRenderApplyCalls=" << nestedCalls
              << " control flat outside " << showDiff(flatDiff) << '\n';
    gates.gate(16, nestedDiff.pixels == 64 * 64 && nestedDiff.maxDiff == 0 && nestedCalls == 0
                   && flatDiff.pixels > 0 && flatDiff.diffPixels == flatDiff.pixels,
               "roto applied inside a nested sequence or flat control did not change");

    // G17 detects roto applied to Echo samples: the current frame is masked
    // but the 0.2 s echo sample is not, so the exterior carries the sample and
    // export matches the preview (which also leaves samples unmasked). The
    // control masks the samples too and must differ outside.
    ClipInfo echoed = clip(170);
    echoed.effects = {VideoEffect::createEcho(0.2, 1, 1.0, 0)};
    wtl->restoreFromProject(oneTrack({echoed}), QVector<QVector<ClipInfo>>{},
                            0.0, -1.0, -1.0, 100);
    wtl->setRotoEntries(rotoAt(QStringLiteral("0:0"), hardRect));
    const QImage echoPreview = window.buildSpecialClipComposite(0.5);
    const QImage echoExport = renderAt(wtl, 0.5);
    const int echoIndex = roto::sourceFrameIndexFor(0.5, 10.0);
    const QImage echoCurrent = roto::applyEntryToFrame(
        hardRect, tlrender::detail::decodeClipFrameNativeForTest(gSyntheticClipPath, 0.5, false, 0.0),
        echoIndex);
    const tlrender::EchoFrameProvider maskedSamples = [&](double sampleSourceSec, double sampleLocalSec) {
        const QImage raw = tlrender::detail::decodeClipFrameNativeForTest(
            gSyntheticClipPath, sampleSourceSec, false, 0.0);
        return tlrender::prepareClipSourceForEcho(
            roto::applyEntryToFrame(hardRect, raw, roto::sourceFrameIndexFor(sampleSourceSec, 10.0)),
            echoed, sampleLocalSec);
    };
    const QImage echoMaskedControl = tlrender::applyClipFxStackWithEchoFromSource(
        echoCurrent, echoed, 0.5, 0.5, maskedSamples);
    const RegionDiff echoParity = regionDiff(echoPreview, echoExport, everyPixel);
    const RegionDiff echoControl = regionDiff(echoExport, echoMaskedControl, outsideRect);
    int echoOutsideVisible = 0;
    for (int y = 0; y < echoExport.height(); ++y)
        for (int x = 0; x < echoExport.width(); ++x)
            if (outsideRect(x, y) && qAlpha(echoExport.pixel(x, y)) > 0) ++echoOutsideVisible;
    wtl->setRotoEntries(QHash<QString, RotoClipEntry>{});
    std::cerr << "G17 parity(preview vs export) " << showDiff(echoParity)
              << " exteriorVisiblePixels=" << echoOutsideVisible
              << " control(masked samples) outside " << showDiff(echoControl) << '\n';
    gates.gate(17, echoParity.pixels == 64 * 64 && echoParity.maxDiff <= 2 && echoOutsideVisible > 0
                   && echoControl.diffPixels > 0,
               "Echo samples were roto'd or export differs from the preview");

    // G18 detects a missing roto stage on the transition incoming clip: a
    // CrossDissolve (blue -> red) over 1.0-2.0 s, entry on the incoming 0:1. One
    // frame before / after the 1.5 s cut the exterior changes and the interior
    // does not; 0.5 s (outgoing only) is untouched.
    ClipInfo outgoing = clip(180);
    outgoing.outPoint = 1.5;
    ClipInfo incoming = clip(181);
    incoming.filePath = redClipPath;
    incoming.inPoint = 0.5;
    outgoing.trailOut.type = incoming.leadIn.type = TransitionType::CrossDissolve;
    outgoing.trailOut.duration = incoming.leadIn.duration = 1.0;
    outgoing.trailOut.alignment = incoming.leadIn.alignment = TransitionAlignment::Center;
    Timeline dissolve;
    dissolve.restoreFromProject(oneTrack({outgoing, incoming}), QVector<QVector<ClipInfo>>{},
                                0.0, -1.0, -1.0, 100);
    bool g18 = true;
    for (double timelineSec : {1.4, 1.6, 0.5}) {
        dissolve.setRotoEntries(QHash<QString, RotoClipEntry>{});
        const QImage plain = renderAt(&dissolve, timelineSec);
        dissolve.setRotoEntries(rotoAt(QStringLiteral("0:1"), hardRect));
        const QImage masked = renderAt(&dissolve, timelineSec);
        const RegionDiff inside = regionDiff(plain, masked, insideRect);
        const RegionDiff outside = regionDiff(plain, masked, outsideRect);
        const bool overlap = timelineSec > 1.0;
        const bool ok = inside.pixels > 0 && inside.maxDiff <= 2
            && (overlap ? outside.diffPixels == outside.pixels : outside.maxDiff == 0);
        std::cerr << "G18 timelineSec=" << timelineSec << (overlap ? " (overlap)" : " (outgoing only)")
                  << " inside " << showDiff(inside) << " outside " << showDiff(outside)
                  << " ok=" << ok << '\n';
        g18 = g18 && ok;
    }
    gates.gate(18, g18 && redReady, "transition incoming roto missing or leaked outside the overlap");

    // G19 detects a file-backed RenderQueue job that drops roto (resolveTimeline
    // carrier): the decoded output's far exterior must go dark against the
    // entry-less output, the interior must match it within the mpeg4 encoder
    // tolerance (mean 8-bit luma difference <= 6). Also Smart Render pass-
    // through: eligible without roto, rejected with a roto reason with it.
    const auto innerRegion = [](int x, int y) { return inRect(x, y, 24, 24, 40, 40); };
    const auto farOutside = [](int x, int y) { return !inRect(x, y, 8, 8, 56, 56); };
    const auto exportProject = [&](bool withRoto, QString *outputPath) {
        ProjectData project;
        project.videoTracks = oneTrack({clip(190)});
        if (withRoto) {
            RotoClipEntry stored = hardRect;
            stored.clipId = QStringLiteral("0:0");
            project.rotoClipEntries = {stored};
        }
        const QString name = withRoto ? QStringLiteral("g19-roto") : QStringLiteral("g19-plain");
        const QString projectPath = temporary.filePath(name + QStringLiteral(".veditor"));
        *outputPath = temporary.filePath(name + QStringLiteral(".mp4"));
        if (!ProjectFile::save(projectPath, project)) return false;
        RenderJob renderJob;
        renderJob.projectFilePath = projectPath;
        renderJob.outputPath = *outputPath;
        renderJob.width = 64;
        renderJob.height = 64;
        renderJob.codec = QStringLiteral("mpeg4");
        renderJob.bitrateBps = 600000;
        renderJob.endUs = 1000000;
        renderJob.exportConfig.insert(QStringLiteral("fps"), 10);
        RenderQueue renderQueue;
        QSignalSpy done(&renderQueue, &RenderQueue::jobCompletedUuid);
        renderQueue.addJob(renderJob);
        renderQueue.start();
        if (done.isEmpty()) done.wait(120000);
        return !done.isEmpty() && done.first().value(1).toBool();
    };
    QString plainOutput, rotoOutput;
    const bool plainExported = exportProject(false, &plainOutput);
    const bool rotoExported = exportProject(true, &rotoOutput);
    const QImage plainFrame = plainExported ? libavcore::grabFrameAt(plainOutput, 0.5) : QImage();
    const QImage rotoFrame = rotoExported ? libavcore::grabFrameAt(rotoOutput, 0.5) : QImage();
    const double plainInner = meanLuma(plainFrame, innerRegion);
    const double rotoInner = meanLuma(rotoFrame, innerRegion);
    const double plainOuter = meanLuma(plainFrame, farOutside);
    const double rotoOuter = meanLuma(rotoFrame, farOutside);
    Timeline smartTimeline;
    ClipInfo smartClip = clip(191);
    smartTimeline.restoreFromProject(oneTrack({smartClip}), QVector<QVector<ClipInfo>>{},
                                     0.0, -1.0, -1.0, 100);
    const QString smartOutput = temporary.filePath(QStringLiteral("g19-smart.mp4"));
    const auto passThrough = [&]() {
        return smartrender::timelinePassThrough(&smartTimeline, smartOutput, QStringLiteral("mpeg4"),
            64, 64, 10.0, 0, 0, false, false, false, false, false, false, false);
    };
    // Same seam as export-meta: canStreamCopy reads the probed signature that
    // timelinePassThrough stores in the predicate clip's displayName.
    const QByteArray oldSeam = qgetenv("VEDITOR_SMARTRENDER_TEST_SEAM");
    const bool hadSeam = qEnvironmentVariableIsSet("VEDITOR_SMARTRENDER_TEST_SEAM");
    qputenv("VEDITOR_SMARTRENDER_TEST_SEAM", "1");
    const smartrender::PassThroughEligibility smartPlain = passThrough();
    smartTimeline.setRotoEntries(rotoAt(QStringLiteral("0:0"), hardRect));
    const smartrender::PassThroughEligibility smartRoto = passThrough();
    if (hadSeam) qputenv("VEDITOR_SMARTRENDER_TEST_SEAM", oldSeam);
    else qunsetenv("VEDITOR_SMARTRENDER_TEST_SEAM");
    std::cerr << "G19 exported plain=" << plainExported << " roto=" << rotoExported
              << " frame=" << rotoFrame.width() << 'x' << rotoFrame.height()
              << " inner[24,40)^2 luma plain=" << plainInner << " roto=" << rotoInner
              << " |delta|=" << std::abs(plainInner - rotoInner) << " (<= 6)"
              << " farOutside(8px border) luma plain=" << plainOuter << " roto=" << rotoOuter
              << " drop=" << (plainOuter - rotoOuter) << " (>= 30)"
              << " smart plain eligible=" << smartPlain.eligible
              << " reason='" << smartPlain.reason.toStdString() << "'"
              << " smart roto eligible=" << smartRoto.eligible
              << " reason='" << smartRoto.reason.toStdString() << "'\n";
    gates.gate(19, plainExported && rotoExported && plainInner >= 0.0 && rotoInner >= 0.0
                   && std::abs(plainInner - rotoInner) <= 6.0 && plainOuter - rotoOuter >= 30.0
                   && smartPlain.eligible && !smartRoto.eligible
                   && smartRoto.reason.contains(QStringLiteral("roto")),
               "file-backed export ignored roto or Smart Render pass-through accepted a roto timeline");

    // G20 detects the roto stage running (renderApplyCallCount moving) for a
    // timeline without roto entries; V1 + V2 + dissolve + Echo exercise every
    // insertion point. The positive control adds entries and must move it.
    ClipInfo busyFirst = outgoing;
    busyFirst.effects = {VideoEffect::createEcho(0.2, 1, 0.5, 0)};
    ClipInfo busyUpper = clip(201);
    busyUpper.filePath = redClipPath;
    Timeline busy;
    busy.restoreFromProject(QVector<QVector<ClipInfo>>{{busyFirst, incoming}, {busyUpper}},
                            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    const auto busyCalls = [&]() {
        const quint64 before = roto::renderApplyCallCount();
        bool rendered = true;
        for (double timelineSec : {0.5, 1.4, 1.6})
            rendered = rendered && !renderAt(&busy, timelineSec).isNull();
        return rendered ? roto::renderApplyCallCount() - before : quint64(~0ULL);
    };
    const quint64 busyNoEntry = busyCalls();
    QHash<QString, RotoClipEntry> busyEntries;
    busyEntries.insert(QStringLiteral("0:0"), hardRect);
    busyEntries.insert(QStringLiteral("0:1"), hardRect);
    busyEntries.insert(QStringLiteral("1:0"), hardRect);
    busy.setRotoEntries(busyEntries);
    const quint64 busyWithEntries = busyCalls();
    std::cerr << "G20 renderApplyCalls noEntries=" << busyNoEntry
              << " withEntries=" << busyWithEntries << '\n';
    gates.gate(20, busyNoEntry == 0 && busyWithEntries > 0 && busyWithEntries != quint64(~0ULL),
               "roto stage ran without entries or the counter did not move with entries");

    // G21 detects a RenderInPlace bake that drops the roto (temporary key not
    // shifted past the prefix black clip), applies it twice after the bake
    // (entry left on the source key) or loses it on undo; (d) pins the known
    // limitation that decompose does not restore the entry.
    const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    bool g21Ran = false;
    bool g21Passed = false;
    QString proresUnavailable;
    if (ffmpegPath.isEmpty()) {
        gates.skip(21, "ffmpeg.exe not found (the linked-audio prefix case needs the export mix)");
    } else if (!selftest::requireEncoder(QStringLiteral("prores_ks"), &proresUnavailable)) {
        gates.skip(21, "prores_ks encoder unavailable (a roto bake needs ProRes 4444 alpha)");
    } else {
        const QString tonePath = temporary.filePath(QStringLiteral("g21-tone.wav"));
        const bool toneReady = writeTone(tonePath);
        struct BakeResult {
            bool baked = false;
            QString path;
            QString error;
            double inPoint = -1.0;
            double inner = -1.0;
            double outer = -1.0;
            bool keyAfterBake = true;
            bool undoClip = false;
            bool undoKey = false;
            bool redoClip = false;
            bool redoKey = true;
            bool decomposeClip = false;
            bool decomposeKey = true;
        };
        const auto bake = [&](bool withRoto, double handles, bool linked) {
            BakeResult r;
            ClipInfo video = clip(210);
            video.inPoint = 0.5;
            video.outPoint = 1.5;
            video.leadInSec = linked ? 1.0 : 0.0;
            QVector<QVector<ClipInfo>> audio{{}};
            if (linked) {
                ClipInfo tone;
                tone.filePath = tonePath;
                tone.displayName = QStringLiteral("tone");
                tone.duration = 8.0;
                tone.outPoint = 1.0;
                tone.leadInSec = 0.5; // starts 0.5 s before the video
                tone.linkGroup = video.linkGroup;
                audio = {{tone}};
            }
            Timeline tl;
            tl.restoreFromProject(oneTrack({video}), audio, 0.0, -1.0, -1.0, 10);
            if (withRoto) tl.setRotoEntries(rotoAt(QStringLiteral("0:0"), hardRect));
            tl.undoManager()->clear();
            tl.saveUndoState(QStringLiteral("roto 焼き込み前"));
            renderinplace::Options options;
            options.handlesSec = handles; // codec: the default h264 (RenderInPlace accepts h264 / prores)
            options.outputDir = temporary.path();
            options.outputSize = frameSize;
            options.fps = 10.0;
            r.baked = renderinplace::renderClipInPlace(tl, 0, 0, options, &r.path, &r.error);
            if (!r.baked) return r;
            const ClipInfo replaced = tl.videoTracks()[0]->clips()[0];
            r.inPoint = replaced.inPoint;
            const QImage frame = libavcore::grabFrameAt(r.path, replaced.inPoint + 0.5);
            r.inner = meanLuma(frame, innerRegion);
            r.outer = meanLuma(frame, farOutside);
            const QString key = QStringLiteral("0:0");
            r.keyAfterBake = tl.rotoEntries().contains(key);
            tl.undo();
            r.undoClip = tl.videoTracks()[0]->clips()[0].filePath == gSyntheticClipPath;
            r.undoKey = tl.rotoEntries().contains(key);
            tl.redo();
            r.redoClip = tl.videoTracks()[0]->clips()[0].filePath == r.path;
            r.redoKey = tl.rotoEntries().contains(key);
            renderinplace::decomposeRenderInPlace(tl, 0, 0);
            r.decomposeClip = tl.videoTracks()[0]->clips()[0].filePath == gSyntheticClipPath;
            r.decomposeKey = tl.rotoEntries().contains(key);
            return r;
        };
        const BakeResult control = bake(false, 0.0, false);
        struct BakeCase { const char *name; double handles; bool linked; double expectedInPoint; };
        bool g21 = toneReady && control.baked && control.inner >= 0.0 && control.outer >= 0.0;
        std::cerr << "G21 control(no roto) baked=" << control.baked << " inner=" << control.inner
                  << " outer=" << control.outer << " error=" << control.error.toStdString()
                  << " toneReady=" << toneReady << '\n';
        for (const BakeCase &c : {BakeCase{"handles0/prefixCount0", 0.0, false, 0.0},
                                  BakeCase{"handles0.2+linkedAudio/prefixCount1", 0.2, true, 0.5}}) {
            const BakeResult r = bake(true, c.handles, c.linked);
            const bool a = r.baked && std::abs(r.inPoint - c.expectedInPoint) <= 1e-6
                && std::abs(r.inner - control.inner) <= 6.0 && control.outer - r.outer >= 30.0;
            const bool b = r.baked && !r.keyAfterBake;
            const bool cUndo = r.baked && r.undoClip && r.undoKey && r.redoClip && !r.redoKey;
            const bool d = r.baked && r.decomposeClip && !r.decomposeKey;
            std::cerr << "G21 case=" << c.name << " baked=" << r.baked
                      << " error=" << r.error.toStdString()
                      << " replacementInPoint=" << r.inPoint << " (expected " << c.expectedInPoint << ")"
                      << " (a) inner=" << r.inner << " vs control " << control.inner
                      << " outer=" << r.outer << " vs control " << control.outer << " -> " << a
                      << " (b) keyAfterBake=" << r.keyAfterBake << " -> " << b
                      << " (c) undoClip=" << r.undoClip << " undoKey=" << r.undoKey
                      << " redoClip=" << r.redoClip << " redoKey=" << r.redoKey << " -> " << cUndo
                      << " (d) decomposeClip=" << r.decomposeClip << " decomposeKey=" << r.decomposeKey
                      << " -> " << d << '\n';
            g21 = g21 && a && b && cUndo && d;
        }

        renderinplace::Options bakeOptions;
        bakeOptions.outputDir = temporary.path();
        bakeOptions.outputSize = frameSize;
        bakeOptions.fps = 10.0;
        const auto alphaMismatch = [](const QImage &a, const QImage &b) {
            if (a.isNull() || b.isNull() || a.size() != b.size()) return -1;
            int mismatched = 0;
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x)
                    if (qAbs(qAlpha(a.pixel(x, y)) - qAlpha(b.pixel(x, y))) > 16) ++mismatched;
            return mismatched;
        };

        // (e) An opaque red V2 under the roto'd V1 clip shows through the
        // exterior before the bake and must still show after it (alpha kept:
        // ProRes 4444 .mov, not an opaque h264 file flattened onto black).
        {
            ClipInfo video = clip(211);
            video.inPoint = 0.5;
            video.outPoint = 1.5;
            ClipInfo under = clip(212);
            under.filePath = redClipPath;
            Timeline tl;
            tl.restoreFromProject(QVector<QVector<ClipInfo>>{{video}, {under}},
                                  QVector<QVector<ClipInfo>>{{}}, 0.0, -1.0, -1.0, 10);
            tl.setRotoEntries(rotoAt(QStringLiteral("0:0"), hardRect));
            const QImage before = renderAt(&tl, 0.5);
            QString path, error;
            const bool baked = renderinplace::renderClipInPlace(tl, 0, 0, bakeOptions, &path, &error);
            const QImage after = baked ? renderAt(&tl, 0.5) : QImage();
            const RegionDiff exterior = regionDiff(before, after, farOutside);
            const RegionDiff interior = regionDiff(before, after, innerRegion);
            const double beforeOuter = meanLuma(before, farOutside);
            const double afterOuter = meanLuma(after, farOutside);
            const double beforeInner = meanLuma(before, innerRegion);
            const double afterInner = meanLuma(after, innerRegion);
            const bool e = redReady && baked && path.endsWith(QStringLiteral(".mov"))
                && beforeOuter >= 50.0 && exterior.pixels > 0 && exterior.maxDiff <= 2
                && std::abs(afterOuter - beforeOuter) <= 2.0
                && afterInner >= 0.0 && std::abs(afterInner - beforeInner) <= 6.0
                && !tl.rotoEntries().contains(QStringLiteral("0:0"));
            std::cerr << "G21 case=V2-under-roto baked=" << baked << " error=" << error.toStdString()
                      << " output=" << QFileInfo(path).suffix().toStdString()
                      << " (e) farOutside luma before=" << beforeOuter << " (V2 red, >= 50) after=" << afterOuter
                      << " exterior " << showDiff(exterior) << " (maxDiff <= 2)"
                      << " inner luma before=" << beforeInner << " after=" << afterInner
                      << " interior " << showDiff(interior) << " (|luma delta| <= 6) -> " << e << '\n';
            g21 = g21 && e;
        }

        // (f) Animated roto with handles over a synthetic time-remap curve
        // (inPoint 0, handles 0.2 -> isolated inPoint < 0): the curve's
        // sourceFps must be the stream rate (10), not the 30 Hz default, so the
        // baked mask matches the source timeline around the 4|5 keyframe
        // boundary (local 0.3 / 0.4 / 0.5 / 0.6 -> source frames 3 / 4 / 5 / 6).
        {
            RotoClipEntry hardKeyed;
            RotoKeyframe left;
            left.frameNumber = 4;
            left.path = rectPath(6, 8, 30, 56);
            RotoKeyframe right;
            right.frameNumber = 5;
            right.path = rectPath(34, 8, 58, 56);
            hardKeyed.keyframes = {left, right};
            ClipInfo video = clip(213);
            video.inPoint = 0.0;
            video.outPoint = 1.5;
            Timeline tl;
            tl.restoreFromProject(oneTrack({video}), QVector<QVector<ClipInfo>>{{}}, 0.0, -1.0, -1.0, 10);
            tl.setRotoEntries(rotoAt(QStringLiteral("0:0"), hardKeyed));
            const QVector<double> times{0.3, 0.4, 0.5, 0.6};
            QVector<QImage> before;
            for (double timelineSec : times) before.append(renderAt(&tl, timelineSec));
            renderinplace::Options options = bakeOptions;
            options.handlesSec = 0.2;
            QString path, error;
            const bool baked = renderinplace::renderClipInPlace(tl, 0, 0, options, &path, &error);
            const double inPoint = baked ? tl.videoTracks()[0]->clips()[0].inPoint : -1.0;
            const int boundarySanity = alphaMismatch(before[1], before[2]);
            bool f = baked && std::abs(inPoint - 0.2) <= 1e-6 && boundarySanity > 0;
            std::cerr << "G21 case=keyframes+handles0.2/syntheticCurve baked=" << baked
                      << " error=" << error.toStdString() << " replacementInPoint=" << inPoint
                      << " (expected 0.2) sanity source key4-vs-key5 alphaMismatch=" << boundarySanity
                      << " (> 0)\n";
            for (int i = 0; i < times.size(); ++i) {
                const QImage after = baked ? renderAt(&tl, times[i]) : QImage();
                const int mismatched = alphaMismatch(before[i], after);
                const RegionDiff diff = regionDiff(before[i], after, everyPixel);
                const bool ok = mismatched == 0;
                std::cerr << "G21 (f) timelineSec=" << times[i] << " localSec=" << times[i]
                          << " sourceSec=" << times[i]
                          << " sourceFrameIndex=" << roto::sourceFrameIndexFor(times[i], 10.0)
                          << " baked-vs-source alphaMismatch(>16)=" << mismatched << " (== 0) "
                          << showDiff(diff) << " -> " << ok << '\n';
                f = f && ok;
            }
            g21 = g21 && f;
        }
        g21Ran = true;
        g21Passed = gates.gate(21, g21, "RenderInPlace roto bake, source-key removal, undo/redo or decompose limitation mismatch");
    }

    // ── G22-G25 (US-915): cross-track drop through TimelineTrack::dropEvent.
    // Map: 1:1 -> 2:1 (moved), 1:2 -> 1:1 (src shift), 2:1 -> 2:2 (dest shift),
    // 1:0 / 2:0 fixed. The matte carrier is intentionally not remapped.
    const QString crossRotoBefore = QStringLiteral("1:0=1,1:1=2,1:2=3,2:0=4,2:1=5");
    const QString crossRotoAfter = QStringLiteral("1:0=1,1:1=3,2:0=4,2:1=2,2:2=5");
    const QString crossParentBefore = QStringLiteral("1:1>2:1,2:0>1:2");
    const QString crossParentAfter = QStringLiteral("2:0>1:1,2:1>2:2");
    const QString crossMatte = QStringLiteral("1:1,2:1");
    std::cerr << "G22 map V2:1(1:1)->V3:1(2:1) V2:2(1:2)->V2:1(1:1) V3:1(2:1)->V3:2(2:2)"
                 " fixed V2:0(1:0) V3:0(2:0)\n";
    auto crossMapped = [&](const Timeline &tl, const char *label) {
        const auto rotos = tl.rotoEntries();
        const QString roto = rotoLayout(rotos);
        const QString parent = parentLayout(tl.clipParentEntries());
        const QString matte = show(keys(tl.trackMatteEntries()));
        const bool ok = droppedIntoV3(tl) && roto == crossRotoAfter && invariant(rotos)
            && parent == crossParentAfter && matte == crossMatte;
        std::cerr << label << " moved=" << droppedIntoV3(tl)
                  << " roto=" << roto.toStdString() << " (expected " << crossRotoAfter.toStdString() << ")"
                  << " clipIdOk=" << invariant(rotos)
                  << " parent=" << parent.toStdString() << " (expected " << crossParentAfter.toStdString() << ")"
                  << " matte=" << matte.toStdString() << " (unchanged " << crossMatte.toStdString() << ")"
                  << " marker2At=" << whereMarker(rotos, 2).toStdString() << " -> " << ok << '\n';
        return ok;
    };

    // G22 detects an unlinked cross-track drop that leaves roto / parent keys on
    // the pre-drop indices (HEAD emitted no signal for linkGroup 0).
    {
        Timeline tl;
        seedCrossDrop(tl, false);
        const QString rotoPre = rotoLayout(tl.rotoEntries());
        const QString parentPre = parentLayout(tl.clipParentEntries());
        bool qtSourceNull = false;
        const bool accepted = driveCrossDrop(tl.videoTracks()[1], 1, tl.videoTracks()[2], 3.0,
                                             true, &qtSourceNull);
        std::cerr << "G22 before roto=" << rotoPre.toStdString()
                  << " parent=" << parentPre.toStdString()
                  << " linkGroup=" << tl.videoTracks()[2]->clips().value(1).linkGroup
                  << " accepted=" << accepted << " qtSourceNull=" << qtSourceNull << '\n';
        const bool mapped = crossMapped(tl, "G22 after");
        gates.gate(22, rotoPre == crossRotoBefore && parentPre == crossParentBefore
                           && accepted && mapped,
                   "unlinked cross-track drop did not remap roto / parent keys");
    }

    // G23 detects the linked path re-aligning roto / parent after the audio
    // partner move (handleCrossTrackLinkedDrop) and losing the explicit map.
    {
        Timeline tl;
        seedCrossDrop(tl, true);
        TimelineTrack *dest = tl.videoTracks()[2];
        QString rotoAtSignal, parentAtSignal;
        QObject::connect(dest, &TimelineTrack::clipMovedAcrossTracks, &tl,
            [&](TimelineTrack *, int, int) {
                rotoAtSignal = rotoLayout(tl.rotoEntries());
                parentAtSignal = parentLayout(tl.clipParentEntries());
            });
        QSignalSpy linkedSpy(dest, &TimelineTrack::crossTrackDropped);
        const bool accepted = driveCrossDrop(tl.videoTracks()[1], 1, dest, 3.0, true, nullptr);
        int partnerTrack = -1;
        for (int t = 0; t < tl.audioTracks().size(); ++t)
            for (const ClipInfo &c : tl.audioTracks()[t]->clips())
                if (c.linkGroup == clip(11).linkGroup) partnerTrack = t;
        const bool mapped = crossMapped(tl, "G23 after-partner");
        const bool stable = rotoAtSignal == rotoLayout(tl.rotoEntries())
            && parentAtSignal == parentLayout(tl.clipParentEntries());
        std::cerr << "G23 accepted=" << accepted << " crossTrackDropped=" << linkedSpy.count()
                  << " partnerAudioTrack=" << partnerTrack << " (expected 2, from 0)"
                  << " rotoAtSignal=" << rotoAtSignal.toStdString()
                  << " parentAtSignal=" << parentAtSignal.toStdString()
                  << " stableAfterPartnerMove=" << stable << '\n';
        gates.gate(23, accepted && linkedSpy.count() == 1 && partnerTrack == 2 && mapped && stable,
                   "linked cross-track drop map differs or partner move changed roto / parent keys");
    }

    // G24 (US-1006): a cross-track drop pushes exactly one undo entry, taken at
    // the end of TimelineTrack::dropEvent (interactionCompleted). The gate checks
    // the undo description and saveSerial +1, then that undo returns to the
    // pre-drop keys and redo re-applies the map. HEAD printed undoPoint=none and
    // saveSerial 3->3 here (this gate then FAILs).
    {
        Timeline tl;
        seedCrossDrop(tl, false);
        tl.undoManager()->clear();
        tl.undoManager()->saveState(tl.currentState(), QStringLiteral("基準"));
        const quint64 serialBefore = tl.undoManager()->saveSerial();
        const bool accepted = driveCrossDrop(tl.videoTracks()[1], 1, tl.videoTracks()[2], 3.0,
                                             true, nullptr);
        const quint64 serialAfter = tl.undoManager()->saveSerial();
        const QString undoPoint = tl.undoManager()->undoDescription();
        const bool pointOk = serialAfter == serialBefore + 1
            && undoPoint == QStringLiteral("クリップをトラック間移動");
        tl.undo();
        const QString rotoUndo = rotoLayout(tl.rotoEntries());
        const QString parentUndo = parentLayout(tl.clipParentEntries());
        const int v2Undo = tl.videoTracks()[1]->clipCount();
        tl.redo();
        const bool redoMapped = crossMapped(tl, "G24 redo");
        const bool undoOk = rotoUndo == crossRotoBefore && parentUndo == crossParentBefore && v2Undo == 3;
        std::cerr << "G24 undoPoint=" << (undoPoint.isEmpty() ? std::string("none") : undoPoint.toStdString())
                  << " saveSerial " << serialBefore << "->" << serialAfter << " (expected +1)"
                  << " accepted=" << accepted << " undo roto=" << rotoUndo.toStdString()
                  << " parent=" << parentUndo.toStdString() << " v2Clips=" << v2Undo
                  << " pointOk=" << pointOk << " undoOk=" << undoOk << '\n';
        gates.gate(24, accepted && pointOk && undoOk && redoMapped,
                   "drop pushed no single undo point, or undo / redo did not restore and re-apply the roto / parent keys");
    }

    // G25 negative controls: (a) without the new signal connection the same drop
    // keeps the pre-drop keys, so the G22 predicate fails; (b) without a source
    // the drop is ignored, so G22 really goes through dropEvent's source check.
    // The production mutation (connection removed) is logged separately in
    // .omc/state/ralph4-results/US-915-mutation.txt.
    {
        Timeline tl;
        seedCrossDrop(tl, false);
        QObject::disconnect(tl.videoTracks()[2], &TimelineTrack::clipMovedAcrossTracks, &tl, nullptr);
        const bool accepted = driveCrossDrop(tl.videoTracks()[1], 1, tl.videoTracks()[2], 3.0,
                                             true, nullptr);
        const QString roto = rotoLayout(tl.rotoEntries());
        const bool predicate = crossMapped(tl, "G25 disconnected");
        const bool stale = accepted && droppedIntoV3(tl) && roto == crossRotoBefore && !predicate;
        Timeline noSource;
        seedCrossDrop(noSource, false);
        const bool ignored = !driveCrossDrop(noSource.videoTracks()[1], 1,
                                             noSource.videoTracks()[2], 3.0, false, nullptr);
        const bool untouched = ignored && noSource.videoTracks()[1]->clipCount() == 3
            && rotoLayout(noSource.rotoEntries()) == crossRotoBefore;
        std::cerr << "G25 disconnected accepted=" << accepted << " roto=" << roto.toStdString()
                  << " marker2At=" << whereMarker(tl.rotoEntries(), 2).toStdString()
                  << " g22Predicate=" << predicate << " staleDetected=" << stale
                  << " | noSource ignored=" << ignored << " untouched=" << untouched << '\n';
        gates.gate(25, stale && untouched, "negative control did not discriminate");
    }

    // ── G26 (US-913): the renderer's roto fps probe is cached. G26 detects
    // roto::probeVideoStreamFps reopening the media on every rendered roto frame
    // (open-count delta == frames instead of <= 1) or a cached fps that differs
    // from a real probe. Control: changing the file's mtime re-keys the cache, so
    // the next probe really opens (open delta 1) and gives the uncached value.
    {
        const QString fpsClipPath = media.filePath(QStringLiteral("roto-g26-fps.mp4"));
        const bool fpsReady = writeSyntheticClip(fpsClipPath, QColor(40, 160, 90));
        ClipInfo fpsClip = clip(260);
        fpsClip.filePath = fpsClipPath;
        Timeline fpsTimeline;
        fpsTimeline.restoreFromProject(oneTrack({fpsClip}), QVector<QVector<ClipInfo>>{},
                                       0.0, -1.0, -1.0, 100);
        fpsTimeline.setRotoEntries(rotoAt(QStringLiteral("0:0"), hardRect));
        const quint64 opensBefore = roto::probeVideoStreamFpsOpenCount();
        const quint64 appliesBefore = roto::renderApplyCallCount();
        int frames = 0;
        double lastSec = 0.0;
        for (int i = 0; i < 6; ++i) {
            lastSec = 0.12 + 0.2 * i;
            if (!renderAt(&fpsTimeline, lastSec).isNull())
                ++frames;
        }
        const quint64 openDelta = roto::probeVideoStreamFpsOpenCount() - opensBefore;
        const quint64 applyDelta = roto::renderApplyCallCount() - appliesBefore;
        const int lastIndex = roto::lastRenderSourceFrameIndex();
        const quint64 opensBeforeCached = roto::probeVideoStreamFpsOpenCount();
        const double cachedFps = roto::probeVideoStreamFps(fpsClipPath);
        const quint64 cachedOpens = roto::probeVideoStreamFpsOpenCount() - opensBeforeCached;

        QFile touch(fpsClipPath);
        const QDateTime mtime = QFileInfo(fpsClipPath).lastModified();
        const bool touched = touch.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)
            && touch.setFileTime(mtime.addSecs(-60), QFileDevice::FileModificationTime);
        touch.close();
        const quint64 opensBeforeUncached = roto::probeVideoStreamFpsOpenCount();
        const double uncachedFps = roto::probeVideoStreamFps(fpsClipPath);
        const quint64 uncachedOpens = roto::probeVideoStreamFpsOpenCount() - opensBeforeUncached;
        const long long expectedLastIndex = std::llround(lastSec * uncachedFps);

        std::cerr << "G26 frames=" << frames << " openDelta=" << openDelta
                  << " renderApplyDelta=" << applyDelta << " cachedFps=" << cachedFps
                  << " cachedProbeOpens=" << cachedOpens << " uncachedFps=" << uncachedFps
                  << " uncachedProbeOpens=" << uncachedOpens << " touched=" << touched
                  << " lastSourceFrameIndex=" << lastIndex << " expected=" << expectedLastIndex
                  << " (timelineSec=localSec=sourceSec=" << lastSec << ") fpsReady=" << fpsReady
                  << '\n';
        gates.gate(26, fpsReady && frames >= 5 && openDelta <= 1
                       && applyDelta == quint64(frames) && cachedOpens == 0
                       && touched && uncachedOpens == 1 && std::abs(uncachedFps - 10.0) < 0.01
                       && cachedFps == uncachedFps && lastIndex == expectedLastIndex,
                   "roto fps probe reopened the media per frame or the cached fps differs from a real probe");
    }

    // ── G27-G28 (US-1003): RenderInPlace bake over the synthetic time-remap
    // curve (inPoint 0.13, handles 0.2 -> isolated inPoint < 0). Fixture: source
    // frame n is uniform gray of luma 20*n (n = 0..12, 10 fps); a frame number
    // is identified from the mean luma inside the opaque roto rect (+-6). The
    // trim is 0.03 s off the source frame grid, so no source second lands on a
    // frame start (the fixture's stored pts sit a hair below n/10). Roto keys:
    // source frame 5 = left rect, 6 = right rect; timeline 0.4 / 0.5 s map to
    // roto frames 6 / 5 (reversed) and 5 / 6 (forward), across the key boundary.
    // G27 also bakes a reversed time-remap clip with handles 0.2 and an
    // untrimmed out point (outPoint 0 = media end 1.3 s; curve srcTime
    // 0.23 + t, so sourceSec = 1.07 - t, roto frames 6 / 5 at 0.4 / 0.5 s).
    // G27 also bakes a reversed tail clip (inPoint 0.16, outPoint 0.96, handles
    // 0.2, 30 fps bake, roto keys 1 = left / 2 = right) at timeline 22/30 and
    // 23/30 s (sourceSec 0.2267 / 0.1933, the last source frame): the source
    // timeline shows frame 2 and roto frame llround(1.6) = 2 (right rect).
    // G27 also bakes a reversed identity-slope time-remap tail clip whose curve
    // sourceFps (30) differs from the stream rate (10): inPoint 0.16, outPoint
    // 0.95, handles 0.2, 60 fps bake, roto keys 5 = left / 6 = right (30 fps
    // frames). At the last two retained ticks 46/60 and 47/60 s (sourceSec
    // 0.1833 / 0.1667) the source timeline shows frame 2 (first frame at or
    // after inPoint) and roto frame llround(0.16 * 30) = 5 (left rect).
    // G27 also bakes a reversed clip with a descending time-remap curve, whose
    // source time runs forward (inPoint 0.16, outPoint 0.96, keys (0, 0.8) /
    // (0.8, 0), sourceFps 10, handles 0.2, 40 fps bake, roto keys 1 = left /
    // 2 = right). At 1/40 and 3/40 s (sourceSec 0.185 / 0.235, inside the
    // first 1/sourceFps after inPoint) the source timeline samples forward:
    // frames 2 / 3 and roto frame llround(sourceSec * 10) = 2 (right rect).
    // G27 detects a reversed bake that samples one source frame later than the
    // source timeline in the picture and the mask (a2d552e bakes n+1: FAIL),
    // and a tail bake that reads the lowered isolated inPoint (frame 1 or roto
    // frame 1 = left rect: FAIL) or picks the tail frame on the sourceFps grid
    // instead of the stream timestamps (remap tail: frame 1: FAIL), or that
    // applies the tail rule to forward-running samples (descending: frame 1,
    // roto frame 1 = left rect: FAIL).
    // G28 (control) detects a fix that also moves forward bakes: G21 stays PASS
    // and the forward counterpart of the G27 clip bakes the source frames.
    QString rampProresUnavailable;
    if (!selftest::requireEncoder(QStringLiteral("prores_ks"), &rampProresUnavailable)) {
        gates.skip(27, "prores_ks encoder unavailable (a roto bake needs ProRes 4444 alpha)");
        gates.skip(28, "prores_ks encoder unavailable (a roto bake needs ProRes 4444 alpha)");
    } else {
        const QString rampPath = media.filePath(QStringLiteral("roto-g27-luma-ramp.mp4"));
        const bool rampReady = writeSyntheticClip(rampPath, QColor(), 20, 13);
        RotoClipEntry rampKeyed;
        RotoKeyframe key5;
        key5.frameNumber = 5;
        key5.path = rectPath(6, 8, 30, 56);
        RotoKeyframe key6;
        key6.frameNumber = 6;
        key6.path = rectPath(34, 8, 58, 56);
        rampKeyed.keyframes = {key5, key6};
        // rect: 0 = left (key 5), 1 = right (key 6), -1 = neither opaque.
        struct FrameId {
            int rect = -1;
            double luma = -1.0;
            int frame = -1;
            int innerAlphaMin = -1;
            int outerAlphaMax = -1;
        };
        const auto identify = [](const QImage &image) {
            FrameId id;
            if (image.isNull()) return id;
            const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
            // Inner = the rect shrunk by 3 px, outer = beyond the rect grown by 3 px.
            const int inner[2][4] = {{9, 11, 27, 53}, {37, 11, 55, 53}};
            const int outer[2][4] = {{3, 5, 33, 59}, {31, 5, 61, 59}};
            double alphaMean[2] = {0.0, 0.0};
            for (int r = 0; r < 2; ++r) {
                int count = 0;
                for (int y = inner[r][1]; y < inner[r][3]; ++y)
                    for (int x = inner[r][0]; x < inner[r][2]; ++x, ++count)
                        alphaMean[r] += qAlpha(argb.pixel(x, y));
                alphaMean[r] /= count;
            }
            const int r = alphaMean[1] > alphaMean[0] ? 1 : 0;
            if (alphaMean[r] <= 128.0) return id;
            id.rect = r;
            const auto in = [&](int x, int y) { return inRect(x, y, inner[r][0], inner[r][1], inner[r][2], inner[r][3]); };
            const auto out = [&](int x, int y) { return !inRect(x, y, outer[r][0], outer[r][1], outer[r][2], outer[r][3]); };
            id.luma = meanLuma(argb, in);
            const int n = int(std::lround(id.luma / 20.0));
            id.frame = std::abs(id.luma - 20.0 * n) <= 6.0 ? n : -1;
            id.innerAlphaMin = 255;
            id.outerAlphaMax = 0;
            for (int y = 0; y < argb.height(); ++y) {
                for (int x = 0; x < argb.width(); ++x) {
                    if (in(x, y)) id.innerAlphaMin = qMin(id.innerAlphaMin, qAlpha(argb.pixel(x, y)));
                    if (out(x, y)) id.outerAlphaMax = qMax(id.outerAlphaMax, qAlpha(argb.pixel(x, y)));
                }
            }
            return id;
        };
        const QVector<double> rampTimes{0.4, 0.5};
        const QVector<double> tailTimes{22.0 / 30.0, 23.0 / 30.0};
        const QVector<double> remapTailTimes{46.0 / 60.0, 47.0 / 60.0};
        const QVector<double> descendingTimes{1.0 / 40.0, 3.0 / 40.0};
        RotoClipEntry tailKeyed;
        RotoKeyframe key1 = key5;
        key1.frameNumber = 1;
        RotoKeyframe key2 = key6;
        key2.frameNumber = 2;
        tailKeyed.keyframes = {key1, key2};
        struct RampBake {
            QVector<double> times;
            bool baked = false;
            QString error;
            double inPoint = -1.0;
            ClipInfo original;
            FrameId source[2];
            FrameId bakedId[2];
        };
        // remap && tail = the identity-slope remap tail clip at sourceFps 30;
        // descending (with tail, remap false) = the descending-curve tail clip.
        const auto bakeRamp = [&](bool reversed, double handlesSec, bool remap, bool tail,
                                  bool descending = false) {
            RampBake r;
            const bool remapTail = remap && tail;
            r.times = descending ? descendingTimes : remapTail ? remapTailTimes : tail ? tailTimes : rampTimes;
            ClipInfo video = clip(270);
            video.filePath = rampPath;
            video.duration = 1.3;
            video.inPoint = tail ? 0.16 : 0.13;
            video.outPoint = remapTail ? 0.95 : tail ? 0.96 : 1.13;
            video.reversed = reversed;
            if (descending) {
                video.timeRemapCurve.keys = {{0.0, 0.8}, {0.8, 0.0}};
                video.timeRemapCurve.sourceFps = 10.0;
            } else if (remapTail) {
                video.timeRemapCurve.keys = {{0.0, 0.0}, {0.79, 0.79}};
                video.timeRemapCurve.sourceFps = 30.0;
            } else if (remap) {
                video.outPoint = 0.0;
                video.timeRemapCurve.keys = {{0.0, 0.23}, {0.8, 1.03}};
                video.timeRemapCurve.sourceFps = 10.0;
            }
            r.original = video;
            Timeline tl;
            tl.restoreFromProject(oneTrack({video}), QVector<QVector<ClipInfo>>{{}}, 0.0, -1.0, -1.0, 10);
            tl.setRotoEntries(rotoAt(QStringLiteral("0:0"), tail && !remapTail ? tailKeyed : rampKeyed));
            for (int i = 0; i < r.times.size(); ++i)
                r.source[i] = identify(renderAt(&tl, r.times[i]));
            renderinplace::Options options;
            options.handlesSec = handlesSec;
            options.outputDir = temporary.path();
            options.outputSize = frameSize;
            options.fps = descending ? 40.0 : remapTail ? 60.0 : tail ? 30.0 : 10.0;
            QString path;
            r.baked = renderinplace::renderClipInPlace(tl, 0, 0, options, &path, &r.error);
            if (!r.baked) return r;
            r.inPoint = tl.videoTracks()[0]->clips()[0].inPoint;
            for (int i = 0; i < r.times.size(); ++i)
                r.bakedId[i] = identify(libavcore::grabFrameAt(path, r.inPoint + r.times[i]));
            return r;
        };
        // Expected source frames / rects are literals (sanity band): reversed
        // shows the last frame before 1.13 - t (remap: 1.07 - t), forward the
        // first frame at 0.13 + t; the rect follows the roto frame llround(sourceSec * 10)
        // (one frame earlier when reversed); the reversed tail clamps the roto
        // second to inPoint 0.16 (roto frame 2; remap tail at 30 fps: frame 5);
        // the descending clip runs forward (first frame at 0.16 + t, no offset).
        const auto rampMatches = [&](const char *label, const RampBake &r, double handlesSec,
                                     const int (&expectedFrame)[2], const int (&expectedRect)[2]) {
            bool ok = rampReady && r.baked && std::abs(r.inPoint - handlesSec) <= 1e-6;
            std::cerr << label << " rampReady=" << rampReady << " baked=" << r.baked
                      << " error=" << r.error.toStdString() << " replacementInPoint=" << r.inPoint
                      << " (expected " << handlesSec << ")\n";
            for (int i = 0; i < r.times.size(); ++i) {
                const double timelineSec = r.times[i];
                const double localSec = timelineSec;
                const double sourceSec = r.original.sourceSecondAtLocalTime(localSec);
                const FrameId &s = r.source[i];
                const FrameId &b = r.bakedId[i];
                const bool sourceSane = s.frame == expectedFrame[i] && s.rect == expectedRect[i];
                const bool same = b.frame == s.frame && b.rect == s.rect
                    && b.innerAlphaMin == 255 && b.outerAlphaMax == 0;
                std::cerr << label << " timelineSec=" << timelineSec << " localSec=" << localSec
                          << " sourceSec=" << sourceSec << " expectedFrame=" << expectedFrame[i]
                          << " expectedRect=" << expectedRect[i]
                          << " sourceFrame=" << s.frame << " (luma " << s.luma << ", rect " << s.rect
                          << ") bakedFrame=" << b.frame << " (luma " << b.luma << ", rect " << b.rect
                          << ") baked innerAlphaMin=" << b.innerAlphaMin << " (== 255) outerAlphaMax="
                          << b.outerAlphaMax << " (== 0) -> sourceSane=" << sourceSane
                          << " same=" << same << '\n';
                ok = ok && sourceSane && same;
            }
            return ok;
        };
        const bool g27Handles = rampMatches("G27 reversed+handles0.2", bakeRamp(true, 0.2, false, false), 0.2,
                                            {7, 6}, {1, 0});
        const bool g27Remap = rampMatches("G27 reversed+remap+handles0.2", bakeRamp(true, 0.2, true, false), 0.2,
                                          {6, 5}, {1, 0});
        const bool g27Tail = rampMatches("G27 reversed+handles0.2+tail30fps", bakeRamp(true, 0.2, false, true), 0.2,
                                         {2, 2}, {1, 1});
        const bool g27RemapTail = rampMatches("G27 reversed+remap30+handles0.2+tail60fps",
                                              bakeRamp(true, 0.2, true, true), 0.2, {2, 2}, {0, 0});
        const bool g27Descending = rampMatches("G27 reversed+descendingRemap+handles0.2+tail40fps",
                                               bakeRamp(true, 0.2, false, true, true), 0.2, {2, 3}, {1, 1});
        std::cerr << "G27 handles=" << g27Handles << " remap=" << g27Remap << " tail=" << g27Tail
                  << " remapTail=" << g27RemapTail << " descending=" << g27Descending << '\n';
        gates.gate(27, g27Handles && g27Remap && g27Tail && g27RemapTail && g27Descending,
                   "reversed bake picture or roto frame differs from the source timeline");
        if (!g21Ran) {
            gates.skip(28, "ffmpeg.exe not found (G21 did not run)");
        } else {
            const bool forward = rampMatches("G28 forward+handles0.2", bakeRamp(false, 0.2, false, false), 0.2,
                                             {6, 7}, {0, 1});
            std::cerr << "G28 G21=" << (g21Passed ? "PASS" : "FAIL") << " forward=" << forward << '\n';
            gates.gate(28, g21Passed && forward, "forward bake control: G21 failed or the forward bake moved");
        }
    }
    // ── G29-G31 (US-1004): roto / alpha media keep their alpha through the
    // grade stage (gradeClipNativeFrame) and the non-geometric FX pack
    // (applyClipFxPack). Fixture: V1 hidden, V2 = the blue synthetic clip with
    // saturation +50 and Vignette (or ChromaKey), roto hardRect on V2.
    {
        ColorCorrection saturated;
        saturated.saturation = 50.0;
        VideoEffect vignette;
        vignette.type = VideoEffectType::Vignette;
        vignette.param1 = 0.6;
        vignette.param2 = 0.5;
        const auto gradedClip = [&](int id, const QVector<VideoEffect> &effects) {
            ClipInfo c = clip(id);
            c.colorCorrection = saturated;
            c.effects = effects;
            return c;
        };
        const auto seedV2 = [&](Timeline *tl, const ClipInfo &v2, bool withRoto) {
            tl->restoreFromProject(QVector<QVector<ClipInfo>>{{clip(290)}, {v2}},
                                   QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
            tl->videoTracks()[0]->setHidden(true);
            tl->setRotoEntries(withRoto ? rotoAt(QStringLiteral("1:0"), hardRect)
                                        : QHash<QString, RotoClipEntry>{});
        };
        const auto pixelText = [](const QImage &image, int x, int y) {
            if (image.isNull()) return std::string("null");
            const QRgb p = image.pixel(x, y);
            return std::to_string(qRed(p)) + ',' + std::to_string(qGreen(p)) + ','
                + std::to_string(qBlue(p)) + ',' + std::to_string(qAlpha(p));
        };
        // {min, max} alpha over the region ({256, -1} for a null image).
        const auto alphaRange = [](const QImage &image, auto region) {
            std::pair<int, int> range{256, -1};
            if (image.isNull()) return range;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    if (!region(x, y)) continue;
                    const int a = qAlpha(image.pixel(x, y));
                    range.first = qMin(range.first, a);
                    range.second = qMax(range.second, a);
                }
            return range;
        };

        // G29 detects a grade / FX stage that refills the roto exterior (HEAD:
        // the RGB888 stages return alpha 255 outside the mask). Inside must
        // equal the same grade / FX without roto, and the grade must have run
        // (differs from the ungraded clip). Sub-case roto + saturation +
        // ChromaKey (soft key ~40 away from the graded colour): exterior alpha 0,
        // interior = the key result (partial alpha) of the no-roto render.
        Timeline g29Tl;
        seedV2(&g29Tl, clip(292), false);
        const QImage g29Raw = renderAt(&g29Tl, 0.5);
        const ClipInfo fxClip = gradedClip(291, {vignette});
        seedV2(&g29Tl, fxClip, false);
        const QImage g29Plain = renderAt(&g29Tl, 0.5);
        seedV2(&g29Tl, fxClip, true);
        const quint64 g29ClampBefore = roto::clampAlphaCallCountForTest();
        const QImage g29Roto = renderAt(&g29Tl, 0.5);
        const quint64 g29Clamps = roto::clampAlphaCallCountForTest() - g29ClampBefore;
        const RegionDiff g29Inside = regionDiff(g29Plain, g29Roto, insideRect);
        const RegionDiff g29Graded = regionDiff(g29Raw, g29Plain, insideRect);
        const auto g29Outside = alphaRange(g29Roto, outsideRect);
        const auto g29PlainAlpha = alphaRange(g29Plain, everyPixel);
        std::cerr << "G29 saturation+vignette inside(32,32) roto=" << pixelText(g29Roto, 32, 32)
                  << " noRoto=" << pixelText(g29Plain, 32, 32) << " raw=" << pixelText(g29Raw, 32, 32)
                  << " outside(4,4) roto=" << pixelText(g29Roto, 4, 4)
                  << " noRoto=" << pixelText(g29Plain, 4, 4)
                  << " outsideAlpha=" << g29Outside.first << ".." << g29Outside.second << " (== 0)"
                  << " noRotoAlpha=" << g29PlainAlpha.first << ".." << g29PlainAlpha.second
                  << " inside vs noRoto " << showDiff(g29Inside) << " (<= 2)"
                  << " graded vs raw " << showDiff(g29Graded) << " (> 2)"
                  << " clampAlphaCallCountForTest=" << g29Clamps << " (> 0)\n";
        const bool g29Main = g29Outside.second == 0 && g29Inside.pixels > 0 && g29Inside.maxDiff <= 2
            && g29Graded.maxDiff > 2 && g29PlainAlpha.first == 255 && g29Clamps > 0;

        const QImage ccOnly = [&] {
            seedV2(&g29Tl, gradedClip(294, {}), false);
            return renderAt(&g29Tl, 0.5);
        }();
        const QRgb graded = ccOnly.isNull() ? 0 : ccOnly.pixel(32, 32);
        VideoEffect chromaKey;
        chromaKey.type = VideoEffectType::ChromaKey;
        chromaKey.keyColor = QColor(qRed(graded), qBound(0, qGreen(graded) + 40, 255), qBlue(graded));
        chromaKey.param1 = 10.0;
        chromaKey.param2 = 60.0;
        const ClipInfo keyClip = gradedClip(293, {chromaKey});
        seedV2(&g29Tl, keyClip, false);
        const QImage keyPlain = renderAt(&g29Tl, 0.5);
        seedV2(&g29Tl, keyClip, true);
        const quint64 keyClampBefore = roto::clampAlphaCallCountForTest();
        const QImage keyRoto = renderAt(&g29Tl, 0.5);
        const quint64 keyClamps = roto::clampAlphaCallCountForTest() - keyClampBefore;
        const RegionDiff keyInside = regionDiff(keyPlain, keyRoto, insideRect);
        const auto keyOutside = alphaRange(keyRoto, outsideRect);
        const auto keyInsideAlpha = alphaRange(keyRoto, insideRect);
        std::cerr << "G29 saturation+chromaKey key=" << chromaKey.keyColor.red() << ','
                  << chromaKey.keyColor.green() << ',' << chromaKey.keyColor.blue()
                  << " inside(32,32) roto=" << pixelText(keyRoto, 32, 32)
                  << " noRoto=" << pixelText(keyPlain, 32, 32)
                  << " outside(4,4) roto=" << pixelText(keyRoto, 4, 4)
                  << " noRoto=" << pixelText(keyPlain, 4, 4)
                  << " outsideAlpha=" << keyOutside.first << ".." << keyOutside.second << " (== 0)"
                  << " insideAlpha=" << keyInsideAlpha.first << ".." << keyInsideAlpha.second
                  << " (partial key, within 40..215)"
                  << " inside vs noRoto " << showDiff(keyInside) << " (<= 2)"
                  << " clampAlphaCallCountForTest=" << keyClamps << " (> 0)\n";
        const bool g29Key = keyOutside.second == 0 && keyInside.pixels > 0 && keyInside.maxDiff <= 2
            && keyInsideAlpha.first >= 40 && keyInsideAlpha.second <= 215 && keyClamps > 0;

        // Sub-case roto + saturation + Vignette + a DISABLED Flip: the disabled
        // pixel-moving effect does not run (applyEffect returns its input), so
        // it must not restart the carried alpha from the opaque Vignette output.
        VideoEffect disabledFlip;
        disabledFlip.type = VideoEffectType::Flip;
        disabledFlip.enabled = false;
        const ClipInfo flipClip = gradedClip(295, {vignette, disabledFlip});
        seedV2(&g29Tl, flipClip, false);
        const QImage flipPlain = renderAt(&g29Tl, 0.5);
        seedV2(&g29Tl, flipClip, true);
        const QImage flipRoto = renderAt(&g29Tl, 0.5);
        const RegionDiff flipInside = regionDiff(flipPlain, flipRoto, insideRect);
        const auto flipOutside = alphaRange(flipRoto, outsideRect);
        std::cerr << "G29 saturation+vignette+disabledFlip inside(32,32) roto=" << pixelText(flipRoto, 32, 32)
                  << " noRoto=" << pixelText(flipPlain, 32, 32)
                  << " outside(4,4) roto=" << pixelText(flipRoto, 4, 4)
                  << " noRoto=" << pixelText(flipPlain, 4, 4)
                  << " outsideAlpha=" << flipOutside.first << ".." << flipOutside.second << " (== 0)"
                  << " inside vs noRoto " << showDiff(flipInside) << " (<= 2)\n";
        const bool g29DisabledFlip = flipOutside.second == 0 && flipInside.pixels > 0 && flipInside.maxDiff <= 2;
        std::cerr << "G29 main=" << g29Main << " key=" << g29Key << " disabledFlip=" << g29DisabledFlip << '\n';
        gates.gate(29, g29Main && g29Key && g29DisabledFlip,
                   "roto exterior not alpha 0 after grade / non-geometric FX, or interior differs from the unmasked grade");

        // G30 detects preview/export drift of the clamped alpha: the special-
        // composite preview (G14 procedure, 64x64, no proxy) and renderFrameAt
        // agree within maxDiff <= 2 (premultiplied ARGB) and the preview keeps
        // the exterior at alpha 0 (HEAD: 255). Control: the no-roto export must
        // differ from the preview.
        wtl->setTrackMatteEntries(QHash<QString, TimelineTrackMatteEntry>{});
        wtl->setClipParentEntries(QHash<QString, QString>{});
        bool g30 = true;
        struct ParityCase { const char *name; ClipInfo clip; };
        for (const ParityCase &pc : {ParityCase{"saturation+vignette", fxClip},
                                     ParityCase{"saturation+chromaKey", keyClip}}) {
            seedV2(wtl, pc.clip, false);
            const QImage noRotoExport = renderAt(wtl, 0.5);
            seedV2(wtl, pc.clip, true);
            const QImage preview = window.buildSpecialClipComposite(0.5);
            const QImage exported = renderAt(wtl, 0.5);
            const RegionDiff parity = regionDiff(preview, exported, everyPixel);
            const RegionDiff control = regionDiff(preview, noRotoExport, everyPixel);
            const auto previewOutside = alphaRange(preview, outsideRect);
            const bool ok = parity.pixels == 64 * 64 && parity.maxDiff <= 2
                && previewOutside.second == 0 && control.maxDiff > 2;
            std::cerr << "G30 " << pc.name << " parity " << showDiff(parity) << " (<= 2)"
                      << " preview outsideAlpha=" << previewOutside.first << ".." << previewOutside.second
                      << " (== 0) preview(4,4)=" << pixelText(preview, 4, 4)
                      << " export(4,4)=" << pixelText(exported, 4, 4)
                      << " preview(32,32)=" << pixelText(preview, 32, 32)
                      << " export(32,32)=" << pixelText(exported, 32, 32)
                      << " control noRotoExport " << showDiff(control) << " (> 2) ok=" << ok << '\n';
            g30 = g30 && ok;
        }
        wtl->setRotoEntries(QHash<QString, RotoClipEntry>{});
        gates.gate(30, g30, "preview and renderFrameAt differ on roto + grade / FX alpha, or the preview exterior is opaque");

        // G31 (a) detects a clamp on opaque layers: no roto + saturation +
        // Vignette renders alpha 255 everywhere with clampAlphaCallCountForTest
        // unchanged. (b) detects alpha media losing its alpha in the grade / FX
        // stage (HEAD: 255): a QImage ARGB frame with an 8 px alpha-0 border
        // through applyClipFxStackFromSource keeps the border at alpha 0 and the
        // core equal to the same stage on the opaque frame.
        Timeline g31Tl;
        seedV2(&g31Tl, fxClip, false);
        const quint64 g31ClampBefore = roto::clampAlphaCallCountForTest();
        const QImage g31Opaque = renderAt(&g31Tl, 0.5);
        const quint64 g31Clamps = roto::clampAlphaCallCountForTest() - g31ClampBefore;
        const auto g31Alpha = alphaRange(g31Opaque, everyPixel);
        const RegionDiff g31Graded = regionDiff(g29Raw, g31Opaque, everyPixel);
        std::cerr << "G31a noRoto saturation+vignette alpha=" << g31Alpha.first << ".." << g31Alpha.second
                  << " (== 255) graded vs raw " << showDiff(g31Graded) << " (> 2)"
                  << " clampAlphaCallCountForTest=" << g31Clamps << " (== 0)\n";
        const bool g31a = g31Alpha.first == 255 && g31Alpha.second == 255 && g31Graded.maxDiff > 2
            && g31Clamps == 0;

        const auto core = [](int x, int y) { return inRect(x, y, 8, 8, 56, 56); };
        const auto border = [&core](int x, int y) { return !core(x, y); };
        QImage opaqueMedia(64, 64, QImage::Format_ARGB32);
        opaqueMedia.fill(QColor(32, 80, 160));
        QImage alphaMedia(64, 64, QImage::Format_ARGB32);
        alphaMedia.fill(Qt::transparent);
        for (int y = 8; y < 56; ++y)
            for (int x = 8; x < 56; ++x)
                alphaMedia.setPixel(x, y, qRgba(32, 80, 160, 255));
        bool g31b = true;
        struct MediaCase { const char *name; ClipInfo clip; };
        for (const MediaCase &mc : {MediaCase{"saturation", gradedClip(295, {})},
                                    MediaCase{"saturation+vignette", gradedClip(296, {vignette})}}) {
            const quint64 clampBefore = roto::clampAlphaCallCountForTest();
            const QImage out = tlrender::applyClipFxStackFromSource(alphaMedia, mc.clip, 0.5);
            const quint64 clamps = roto::clampAlphaCallCountForTest() - clampBefore;
            const QImage ref = tlrender::applyClipFxStackFromSource(opaqueMedia, mc.clip, 0.5);
            const auto borderAlpha = alphaRange(out, border);
            const auto coreAlpha = alphaRange(out, core);
            const RegionDiff coreDiff = regionDiff(out, ref, core);
            const RegionDiff graded = regionDiff(opaqueMedia, ref, core);
            const bool ok = borderAlpha.second == 0 && coreAlpha.first == 255 && coreDiff.pixels > 0
                && coreDiff.maxDiff <= 2 && graded.maxDiff > 2 && clamps > 0;
            std::cerr << "G31b alphaMedia " << mc.name << " border(2,2)=" << pixelText(out, 2, 2)
                      << " core(32,32)=" << pixelText(out, 32, 32) << " opaqueRef(32,32)=" << pixelText(ref, 32, 32)
                      << " borderAlpha=" << borderAlpha.first << ".." << borderAlpha.second << " (== 0)"
                      << " coreAlpha=" << coreAlpha.first << ".." << coreAlpha.second << " (== 255)"
                      << " core vs opaqueRef " << showDiff(coreDiff) << " (<= 2)"
                      << " graded vs source " << showDiff(graded) << " (> 2)"
                      << " clampAlphaCallCountForTest=" << clamps << " (> 0) ok=" << ok << '\n';
            g31b = g31b && ok;
        }
        gates.gate(31, g31a && g31b,
                   "opaque layer clamped, or alpha media border not alpha 0 after grade / FX");
    }

    // ── G32 (US-1006): one undo for a cross-track drop. (a) unlinked drop:
    // undo returns the main clip to V2:1 with the pre-drop clip identities
    // (ClipKeyId::sameAs) and roto / parent / matte keys, redo re-applies the
    // map. (b) linked drop into V3 creates A2 / A3 for the partner (structural
    // path): still exactly one save point, and undo restores the track counts,
    // the clip identities and every carrier key. Detects HEAD's missing undo
    // point (G32 FAILs: serial delta 0, undo does not return to pre-drop).
    // Control: the pre-drop identity layout differs from the post-drop layout,
    // so an undo that did nothing would FAIL the sameAs comparison.
    {
        auto identities = [](const Timeline &t) {
            QVector<ClipKeyId> out;
            for (TimelineTrack *tr : t.videoTracks())
                for (const ClipInfo &c : tr->clips())
                    out.append(ClipKeyId{c.filePath, c.linkGroup, c.inPoint});
            for (TimelineTrack *tr : t.audioTracks())
                for (const ClipInfo &c : tr->clips())
                    out.append(ClipKeyId{c.filePath, c.linkGroup, c.inPoint});
            return out;
        };
        auto sameIdentities = [](const QVector<ClipKeyId> &a, const QVector<ClipKeyId> &b) {
            if (a.size() != b.size()) return false;
            for (int i = 0; i < a.size(); ++i)
                if (!a[i].sameAs(b[i])) return false;
            return true;
        };
        auto trackLayout = [](const Timeline &t) {
            QStringList out;
            for (TimelineTrack *tr : t.videoTracks()) out.append(QStringLiteral("V%1").arg(tr->clipCount()));
            for (TimelineTrack *tr : t.audioTracks()) out.append(QStringLiteral("A%1").arg(tr->clipCount()));
            return out.join(QLatin1Char(','));
        };
        auto carriers = [](const Timeline &t) {
            return rotoLayout(t.rotoEntries()) + QStringLiteral(" | ")
                + parentLayout(t.clipParentEntries()) + QStringLiteral(" | ")
                + show(keys(t.trackMatteEntries()));
        };

        Timeline a;
        seedCrossDrop(a, false);
        const QVector<ClipKeyId> idBeforeA = identities(a);
        const QString carriersBeforeA = carriers(a);
        a.undoManager()->clear();
        a.undoManager()->saveState(a.currentState(), QStringLiteral("基準"));
        const quint64 serialA0 = a.undoManager()->saveSerial();
        const bool acceptedA = driveCrossDrop(a.videoTracks()[1], 1, a.videoTracks()[2], 3.0, true, nullptr);
        const quint64 serialA1 = a.undoManager()->saveSerial();
        const QVector<ClipKeyId> idDroppedA = identities(a);
        const bool differsA = !sameIdentities(idBeforeA, idDroppedA);
        const bool undone = a.undoManager()->canUndo();
        a.undo();
        std::cerr << "G32a undo carriers=" << carriers(a).toStdString() << " (expected "
                  << carriersBeforeA.toStdString() << ")\n";
        const int v2ClipsAfterUndo = a.videoTracks()[1]->clipCount();
        const QString v2At1NameAfterUndo = v2ClipsAfterUndo > 1
            ? a.videoTracks()[1]->clips()[1].displayName : QString();
        const bool identitiesBackA = sameIdentities(idBeforeA, identities(a));
        const bool backA = v2ClipsAfterUndo == 3
            && v2At1NameAfterUndo == QStringLiteral("11")
            && identitiesBackA
            && rotoLayout(a.rotoEntries()) == crossRotoBefore
            && parentLayout(a.clipParentEntries()) == crossParentBefore
            && carriers(a) == carriersBeforeA;
        a.redo();
        const bool redoA = crossMapped(a, "G32a redo");
        std::cerr << "G32a accepted=" << acceptedA << " saveSerial " << serialA0 << "->" << serialA1
                  << " canUndo=" << undone << " identitiesDiffer=" << differsA
                  << " v2ClipsAfterUndo=" << v2ClipsAfterUndo << " (expected 3) v2At1NameAfterUndo="
                  << v2At1NameAfterUndo.toStdString() << " (expected 11) identitiesBack=" << identitiesBackA
                  << " backAtV2:1=" << backA << " carriersBefore=" << carriersBeforeA.toStdString()
                  << " redoMapped=" << redoA << '\n';
        const bool gateA = acceptedA && serialA1 == serialA0 + 1 && differsA && backA && redoA;

        // AC2(b) reads the V3 of "V2 -> nonexistent V3" as "V3 exists, its paired A3
        // does not": dropEvent needs an existing destination TimelineTrack, so
        // nothing in the same drop creates V3. The partner starts on A2 and the
        // drop creates exactly A3. An external-state hook pair mirrors
        // MainWindow::collectExternalTrackState (live Timeline mattes), and a matte
        // added after the baseline save (no undo entry) must survive the undo.
        Timeline b;
        seedCrossDrop(b, true, true);
        b.setExternalTrackStateHooks(
            [&b]() {
                QJsonArray mattes;
                const auto entries = b.trackMatteEntries();
                for (auto it = entries.cbegin(); it != entries.cend(); ++it)
                    mattes.append(QJsonObject{{"clipId", it.key()},
                        {"source", it.value().matteSourceClipId}, {"type", int(it.value().matteType)}});
                return QJsonObject{{"mattes", mattes}};
            },
            [&b](const QJsonObject &state) {
                QHash<QString, TimelineTrackMatteEntry> parsed;
                for (const auto &value : state.value("mattes").toArray()) {
                    const auto obj = value.toObject();
                    TimelineTrackMatteEntry e;
                    e.matteSourceClipId = obj.value("source").toString();
                    e.matteType = static_cast<TrackMatteType>(obj.value("type").toInt());
                    parsed.insert(obj.value("clipId").toString(), e);
                }
                b.setTrackMatteEntries(parsed);
            });
        const QVector<ClipKeyId> idBeforeB = identities(b);
        const QString layoutBeforeB = trackLayout(b);
        const int audioTracksBeforeB = b.audioTracks().size();
        b.undoManager()->clear();
        b.undoManager()->saveState(b.currentState(), QStringLiteral("基準"));
        {
            auto withMatte = b.trackMatteEntries();
            TimelineTrackMatteEntry late;
            late.matteType = TrackMatteType::AlphaMatte;
            late.matteSourceClipId = QStringLiteral("0:0");
            withMatte.insert(QStringLiteral("1:2"), late);
            b.setTrackMatteEntries(withMatte);
        }
        const QString carriersBeforeB = carriers(b);
        const bool lateMatteSet = b.trackMatteEntries().contains(QStringLiteral("1:2"));
        auto partnerOn = [&b](int track) {
            if (track < 0 || track >= b.audioTracks().size()) return false;
            for (const ClipInfo &c : b.audioTracks()[track]->clips())
                if (c.linkGroup == clip(11).linkGroup) return true;
            return false;
        };
        const bool partnerStartA2 = partnerOn(1);
        const quint64 serialB0 = b.undoManager()->saveSerial();
        const bool acceptedB = driveCrossDrop(b.videoTracks()[1], 1, b.videoTracks()[2], 3.0, true, nullptr);
        const quint64 serialB1 = b.undoManager()->saveSerial();
        const int audioTracksAfterB = b.audioTracks().size();
        const QString layoutAfterB = trackLayout(b);
        const bool partnerA3After = partnerOn(2);
        const bool partnerA2After = partnerOn(1);
        b.undo();
        const bool partnerBackA2 = partnerOn(1) && !partnerOn(2);
        const bool lateMatteBack = b.trackMatteEntries().contains(QStringLiteral("1:2"));
        const bool identitiesBackB = sameIdentities(idBeforeB, identities(b));
        const bool backB = b.audioTracks().size() == audioTracksBeforeB
            && trackLayout(b) == layoutBeforeB
            && identitiesBackB
            && carriers(b) == carriersBeforeB
            && lateMatteBack && partnerBackA2;
        std::cerr << "G32b accepted=" << acceptedB << " saveSerial " << serialB0 << "->" << serialB1
                  << " (expected +1) audioTracks " << audioTracksBeforeB << "->" << audioTracksAfterB
                  << " (expected 2->3) partnerStartA2=" << partnerStartA2
                  << " partnerOnA3AfterDrop=" << partnerA3After << " partnerOnA2AfterDrop=" << partnerA2After
                  << " (expected 1/0) layout " << layoutBeforeB.toStdString() << " -> "
                  << layoutAfterB.toStdString() << " undo: tracks=" << trackLayout(b).toStdString()
                  << " audioTracks=" << b.audioTracks().size() << " partnerBackOnA2=" << partnerBackA2
                  << " identitiesBack=" << identitiesBackB
                  << " lateMatte1:2SetBefore=" << lateMatteSet << " lateMatte1:2AfterUndo=" << lateMatteBack
                  << " carriers=" << carriers(b).toStdString()
                  << " (expected " << carriersBeforeB.toStdString() << ") backB=" << backB << '\n';
        const bool gateB = acceptedB && serialB1 == serialB0 + 1 && partnerStartA2 && lateMatteSet
            && audioTracksAfterB == 3 && partnerA3After && !partnerA2After && backB;
        gates.gate(32, gateA && gateB,
                   "cross-track drop not undone in one step (unlinked, or linked with track creation)");
    }

    return gates.finish("roto");
}
