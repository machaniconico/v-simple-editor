#include "AafExchange.h"
#include "ClipRoles.h"
#include "Timeline.h"

#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QUuid>
#include <QVBoxLayout>
#include <QtGlobal>

#include <cmath>
#include <cstring>

namespace aaf {

namespace {

// Microsoft Compound File Binary (OLE2 / CFB v3) constants — standard AAF container format.
constexpr int kSectorSize = 512;
constexpr int kMiniCutoff = 4096;
constexpr quint32 kFreeSect   = 0xFFFFFFFFu;
constexpr quint32 kEndOfChain = 0xFFFFFFFEu;
constexpr quint32 kFatSect    = 0xFFFFFFFDu;
constexpr quint32 kNoStream   = 0xFFFFFFFFu;
constexpr uchar kCfbMagic[8]  = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

// AAF v1.1 Structured Storage Root CLSID (42464141-... / standard AAF SS magic).
constexpr uchar kAafRootClsid[16] = {
    0x41, 0x41, 0x46, 0x42, 0x0D, 0x00, 0x4F, 0x4D,
    0x06, 0x0E, 0x2B, 0x34, 0x01, 0x01, 0x01, 0xFF
};

void wrU16(uchar *p, quint16 v)
{
    p[0] = static_cast<uchar>(v & 0xFFu);
    p[1] = static_cast<uchar>((v >> 8) & 0xFFu);
}

void wrU32(uchar *p, quint32 v)
{
    p[0] = static_cast<uchar>(v & 0xFFu);
    p[1] = static_cast<uchar>((v >> 8) & 0xFFu);
    p[2] = static_cast<uchar>((v >> 16) & 0xFFu);
    p[3] = static_cast<uchar>((v >> 24) & 0xFFu);
}

quint16 rdU16(const uchar *p)
{
    return static_cast<quint16>(p[0] | (p[1] << 8));
}

quint32 rdU32(const uchar *p)
{
    return static_cast<quint32>(p[0])
         | (static_cast<quint32>(p[1]) << 8)
         | (static_cast<quint32>(p[2]) << 16)
         | (static_cast<quint32>(p[3]) << 24);
}

// Pad JSON bytes with trailing spaces to >= 4096 bytes so every stream is stored directly
// in regular 512-byte FAT sectors (100% OLE2 v3 compliant without needing a MiniFAT).
QByteArray padJsonToRegularStream(const QByteArray &json)
{
    if (json.size() >= kMiniCutoff)
        return json;
    QByteArray out = json;
    out.append(QByteArray(kMiniCutoff - out.size(), ' '));
    return out;
}

void writeDirEntry(uchar *entry,
                   const QString &name,
                   uchar objType,
                   quint32 leftSibling,
                   quint32 rightSibling,
                   quint32 childId,
                   quint32 startSector,
                   quint32 streamSize,
                   const uchar *clsid16 = nullptr)
{
    std::memset(entry, 0, 128);
    const int chars = qMin(31, name.size());
    for (int i = 0; i < chars; ++i)
        wrU16(entry + i * 2, name.at(i).unicode());
    wrU16(entry + chars * 2, 0);
    wrU16(entry + 64, static_cast<quint16>((chars + 1) * 2));
    entry[66] = objType; // 5=Root, 2=Stream
    entry[67] = 1;       // Color = Black
    wrU32(entry + 68, leftSibling);
    wrU32(entry + 72, rightSibling);
    wrU32(entry + 76, childId);
    if (clsid16)
        std::memcpy(entry + 80, clsid16, 16);
    wrU32(entry + 116, startSector);
    wrU32(entry + 120, streamSize);
}

QString readDirEntryName(const uchar *entry)
{
    const quint16 byteLen = rdU16(entry + 64);
    if (byteLen < 2 || byteLen > 64)
        return {};
    const int chars = (byteLen / 2) - 1;
    QString out;
    out.reserve(chars);
    for (int i = 0; i < chars; ++i)
        out.append(QChar(rdU16(entry + i * 2)));
    return out;
}

struct CfbStreamSpec {
    QString name;
    QByteArray data;
    quint32 startSector = kEndOfChain;
    quint32 sectorCount = 0;
};

QByteArray buildCfbV3Container(QVector<CfbStreamSpec> &streams)
{
    // Sector layout:
    //   Sector 0: Directory (4 entries: Root, Header, Composition, MetaDictionary)
    //   Sectors 1..N: Stream payloads
    //   Sectors N+1..N+F: FAT sectors
    quint32 nextSector = 1;
    for (auto &st : streams) {
        st.startSector = nextSector;
        st.sectorCount = static_cast<quint32>((st.data.size() + kSectorSize - 1) / kSectorSize);
        nextSector += st.sectorCount;
    }

    const quint32 dataAndDirSectors = nextSector;
    // Solve numFatSectors such that numFatSectors * 128 >= dataAndDirSectors + numFatSectors
    quint32 numFatSectors = 1;
    while (numFatSectors * 128u < dataAndDirSectors + numFatSectors)
        ++numFatSectors;

    const quint32 firstFatSector = nextSector;
    const quint32 totalSectors = firstFatSector + numFatSectors;

    // Build FAT table
    QVector<quint32> fat(numFatSectors * 128u, kFreeSect);
    fat[0] = kEndOfChain; // Directory sector 0
    for (const auto &st : streams) {
        for (quint32 i = 0; i < st.sectorCount; ++i) {
            const quint32 cur = st.startSector + i;
            fat[cur] = (i + 1 < st.sectorCount) ? (cur + 1) : kEndOfChain;
        }
    }
    for (quint32 f = 0; f < numFatSectors; ++f)
        fat[firstFatSector + f] = kFatSect;

    QByteArray file(static_cast<int>((totalSectors + 1) * kSectorSize), '\0');
    auto *base = reinterpret_cast<uchar *>(file.data());

    // 1. CFB v3 Header (512 bytes)
    std::memcpy(base, kCfbMagic, 8);
    wrU16(base + 24, 0x003Eu); // Minor version 3.62
    wrU16(base + 26, 0x0003u); // Major version 3
    wrU16(base + 28, 0xFFFEu); // Little-endian
    wrU16(base + 30, 0x0009u); // Sector shift 512
    wrU16(base + 32, 0x0006u); // Mini sector shift 64
    wrU32(base + 44, numFatSectors);
    wrU32(base + 48, 0u);      // First Directory sector = 0
    wrU32(base + 56, kMiniCutoff);
    wrU32(base + 60, kEndOfChain); // No MiniFAT
    wrU32(base + 64, 0u);
    wrU32(base + 68, kEndOfChain); // No DIFAT chain (up to 109 FAT sectors = ~7 MB)
    wrU32(base + 72, 0u);
    for (int i = 0; i < 109; ++i) {
        const quint32 secId = (static_cast<quint32>(i) < numFatSectors)
            ? (firstFatSector + static_cast<quint32>(i))
            : kFreeSect;
        wrU32(base + 76 + i * 4, secId);
    }

    // 2. Directory Sector 0 (Root + 3 streams ordered by CFB RB-tree length rule:
    //    "Header" (len 6) < "Composition" (len 11) < "MetaDictionary" (len 14))
    uchar *dir = base + kSectorSize;
    writeDirEntry(dir + 0 * 128, QStringLiteral("Root Entry"), 5,
                  kNoStream, kNoStream, 2, kEndOfChain, 0, kAafRootClsid);
    writeDirEntry(dir + 1 * 128, streams[0].name, 2, // Header (left of Composition)
                  kNoStream, kNoStream, kNoStream,
                  streams[0].startSector, static_cast<quint32>(streams[0].data.size()));
    writeDirEntry(dir + 2 * 128, streams[1].name, 2, // Composition (root child)
                  1, 3, kNoStream,
                  streams[1].startSector, static_cast<quint32>(streams[1].data.size()));
    writeDirEntry(dir + 3 * 128, streams[2].name, 2, // MetaDictionary (right of Composition)
                  kNoStream, kNoStream, kNoStream,
                  streams[2].startSector, static_cast<quint32>(streams[2].data.size()));

    // 3. Stream Data Sectors
    for (const auto &st : streams) {
        uchar *dst = base + static_cast<size_t>(st.startSector + 1) * kSectorSize;
        std::memcpy(dst, st.data.constData(), static_cast<size_t>(st.data.size()));
    }

    // 4. FAT Sectors
    for (quint32 f = 0; f < numFatSectors; ++f) {
        uchar *dst = base + static_cast<size_t>(firstFatSector + f + 1) * kSectorSize;
        for (int i = 0; i < 128; ++i)
            wrU32(dst + i * 4, fat[f * 128u + static_cast<quint32>(i)]);
    }

    return file;
}

bool readCfbStreamByName(const QByteArray &raw,
                         const QString &targetStreamName,
                         QByteArray *outData,
                         QString *errorOut)
{
    if (raw.size() < kSectorSize * 2) {
        if (errorOut) *errorOut = QStringLiteral("AAF ファイルサイズが不正です。");
        return false;
    }
    const auto *base = reinterpret_cast<const uchar *>(raw.constData());
    if (std::memcmp(base, kCfbMagic, 8) != 0) {
        if (errorOut) *errorOut = QStringLiteral("AAF OLE2/CFB マジックヘッダが一致しません。");
        return false;
    }
    const quint16 majorVer = rdU16(base + 26);
    const quint16 secShift = rdU16(base + 30);
    if (majorVer != 3 || secShift != 9) {
        if (errorOut) *errorOut = QStringLiteral("未対応の CFB バージョンです (v3 / 512B セクタのみ対応)。");
        return false;
    }

    const quint32 numFat = rdU32(base + 44);
    const quint32 firstDirSec = rdU32(base + 48);
    const quint32 maxSectors = static_cast<quint32>((raw.size() - kSectorSize) / kSectorSize);

    // Reconstruct FAT from Header DIFAT (up to 109 FAT sectors)
    QVector<quint32> fat;
    fat.reserve(static_cast<int>(qMin(numFat, 109u) * 128u));
    for (quint32 i = 0; i < qMin(numFat, 109u); ++i) {
        const quint32 fatSec = rdU32(base + 76 + i * 4);
        if (fatSec >= maxSectors) {
            if (errorOut) *errorOut = QStringLiteral("AAF FAT セクタ番号が範囲外です。");
            return false;
        }
        const uchar *secPtr = base + static_cast<size_t>(fatSec + 1) * kSectorSize;
        for (int e = 0; e < 128; ++e)
            fat.append(rdU32(secPtr + e * 4));
    }

    auto readChain = [&](quint32 startSec, quint32 exactBytes) -> QByteArray {
        QByteArray buf;
        quint32 cur = startSec;
        int guard = 0;
        while (cur < maxSectors && cur < static_cast<quint32>(fat.size()) && guard < 131072) {
            const uchar *p = base + static_cast<size_t>(cur + 1) * kSectorSize;
            buf.append(reinterpret_cast<const char *>(p), kSectorSize);
            cur = fat[cur];
            ++guard;
            if (cur == kEndOfChain || cur == kFreeSect)
                break;
        }
        if (exactBytes > 0 && static_cast<quint32>(buf.size()) > exactBytes)
            buf.truncate(static_cast<int>(exactBytes));
        return buf;
    };

    const QByteArray dirBytes = readChain(firstDirSec, 0);
    const int entryCount = dirBytes.size() / 128;
    for (int i = 0; i < entryCount; ++i) {
        const uchar *e = reinterpret_cast<const uchar *>(dirBytes.constData() + i * 128);
        if (e[66] != 2) // Stream entries only
            continue;
        const QString name = readDirEntryName(e);
        if (name == targetStreamName) {
            const quint32 startSec = rdU32(e + 116);
            const quint32 sz = rdU32(e + 120);
            *outData = readChain(startSec, sz);
            return true;
        }
    }

    if (errorOut) *errorOut = QStringLiteral("AAF ストリーム '%1' が見つかりません。").arg(targetStreamName);
    return false;
}

} // namespace

bool writeAafFile(const Timeline *timeline,
                  const AafExportOptions &options,
                  const QString &outPath,
                  QString *errorOut)
{
    if (!timeline) {
        if (errorOut) *errorOut = QStringLiteral("タイムラインが無効です。");
        return false;
    }
    const double fps = qBound(1.0, options.fps, 240.0);

    // 1. Header stream metadata (AAF v1.1 Identification)
    QJsonObject hdrObj;
    hdrObj[QStringLiteral("class")] = QStringLiteral("Header");
    hdrObj[QStringLiteral("version")] = QStringLiteral("1.1");
    hdrObj[QStringLiteral("byteOrder")] = QStringLiteral("LittleEndian");
    hdrObj[QStringLiteral("productName")] = QStringLiteral("v-simple-editor");
    hdrObj[QStringLiteral("projectName")] = options.projectName;
    hdrObj[QStringLiteral("editRate")] = fps;

    // 2. MetaDictionary stream
    QJsonObject metaObj;
    metaObj[QStringLiteral("class")] = QStringLiteral("MetaDictionary");
    metaObj[QStringLiteral("definedClasses")] = QJsonArray{
        QStringLiteral("CompositionMob"), QStringLiteral("MasterMob"),
        QStringLiteral("SourceMob"), QStringLiteral("TimelineMobSlot"),
        QStringLiteral("EventMobSlot"), QStringLiteral("Sequence"),
        QStringLiteral("Filler"), QStringLiteral("SourceClip"),
        QStringLiteral("NetworkLocator"), QStringLiteral("DescriptiveMarker")
    };

    // 3. Composition Mob Graph stream
    QJsonObject compMob;
    compMob[QStringLiteral("class")] = QStringLiteral("CompositionMob");
    compMob[QStringLiteral("mobId")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    compMob[QStringLiteral("name")] = options.projectName;
    compMob[QStringLiteral("editRate")] = fps;

    QJsonArray slotsArr;
    QJsonArray masterMobs;
    int nextSlotId = 1;

    auto emitTracks = [&](const QVector<TimelineTrack *> &tracks, bool isAudio) {
        for (int tIdx = 0; tIdx < tracks.size(); ++tIdx) {
            const TimelineTrack *tr = tracks[tIdx];
            if (!tr) continue;
            QJsonObject slotObj;
            slotObj[QStringLiteral("class")] = QStringLiteral("TimelineMobSlot");
            slotObj[QStringLiteral("slotId")] = nextSlotId++;
            slotObj[QStringLiteral("mediaKind")] = isAudio ? QStringLiteral("Sound") : QStringLiteral("Picture");
            slotObj[QStringLiteral("name")] = QStringLiteral("%1%2").arg(isAudio ? QStringLiteral("A") : QStringLiteral("V")).arg(tIdx + 1);
            slotObj[QStringLiteral("editRate")] = fps;

            QJsonArray components;
            for (const ClipInfo &c : tr->clips()) {
                if (c.leadInSec > 1e-6) {
                    QJsonObject filler;
                    filler[QStringLiteral("class")] = QStringLiteral("Filler");
                    filler[QStringLiteral("lengthSec")] = c.leadInSec;
                    filler[QStringLiteral("lengthFrames")] = static_cast<qint64>(std::llround(c.leadInSec * fps));
                    components.append(filler);
                }

                const QString mobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                const double outPt = (c.outPoint > c.inPoint) ? c.outPoint : c.duration;
                const double srcDur = qMax(c.duration, outPt);

                QJsonObject sc;
                sc[QStringLiteral("class")] = QStringLiteral("SourceClip");
                sc[QStringLiteral("sourceMobId")] = mobId;
                sc[QStringLiteral("displayName")] = c.displayName;
                sc[QStringLiteral("startSec")] = c.inPoint;
                sc[QStringLiteral("endSec")] = outPt;
                sc[QStringLiteral("lengthSec")] = qMax(0.0, outPt - c.inPoint);
                sc[QStringLiteral("startFrames")] = static_cast<qint64>(std::llround(c.inPoint * fps));
                sc[QStringLiteral("lengthFrames")] = static_cast<qint64>(std::llround(qMax(0.0, outPt - c.inPoint) * fps));
                sc[QStringLiteral("speed")] = c.speed;
                sc[QStringLiteral("volume")] = c.volume;
                const cliproles::Role role = cliproles::roleForClip(c.uid);
                if (role != cliproles::Role::None)
                    sc[QStringLiteral("role")] = cliproles::metaFor(role).id;
                components.append(sc);

                QJsonObject mm;
                mm[QStringLiteral("class")] = QStringLiteral("MasterMob");
                mm[QStringLiteral("mobId")] = mobId;
                mm[QStringLiteral("name")] = c.displayName;
                mm[QStringLiteral("sourceDurationSec")] = srcDur;
                mm[QStringLiteral("networkLocator")] = c.filePath;
                masterMobs.append(mm);
            }
            slotObj[QStringLiteral("components")] = components;
            slotsArr.append(slotObj);
        }
    };

    if (options.includeVideo)
        emitTracks(timeline->videoTracks(), false);
    if (options.includeAudio)
        emitTracks(timeline->audioTracks(), true);

    QJsonArray markersArr;
    if (options.includeMarkers) {
        for (const Marker &m : timeline->markers()) {
            QJsonObject mo;
            mo[QStringLiteral("class")] = QStringLiteral("DescriptiveMarker");
            mo[QStringLiteral("positionSec")] = static_cast<double>(m.timelineUs) / 1e6;
            mo[QStringLiteral("positionFrames")] = static_cast<qint64>(std::llround((m.timelineUs / 1e6) * fps));
            mo[QStringLiteral("comment")] = m.label;
            mo[QStringLiteral("color")] = m.color.name(QColor::HexRgb);
            markersArr.append(mo);
        }
    }

    QJsonObject compRoot;
    compRoot[QStringLiteral("compositionMob")] = compMob;
    compRoot[QStringLiteral("slots")] = slotsArr;
    compRoot[QStringLiteral("masterMobs")] = masterMobs;
    compRoot[QStringLiteral("markers")] = markersArr;

    // Assemble OLE2/CFB v3 streams (sorted: Header, Composition, MetaDictionary)
    QVector<CfbStreamSpec> streams;
    streams.append({QStringLiteral("Header"),
                    padJsonToRegularStream(QJsonDocument(hdrObj).toJson(QJsonDocument::Compact))});
    streams.append({QStringLiteral("Composition"),
                    padJsonToRegularStream(QJsonDocument(compRoot).toJson(QJsonDocument::Compact))});
    streams.append({QStringLiteral("MetaDictionary"),
                    padJsonToRegularStream(QJsonDocument(metaObj).toJson(QJsonDocument::Compact))});

    const QByteArray cfbBytes = buildCfbV3Container(streams);
    QDir().mkpath(QFileInfo(outPath).absolutePath());
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("AAF ファイルを書き込めません: %1").arg(outPath);
        return false;
    }
    return f.write(cfbBytes) == cfbBytes.size();
}

AafImportResult readAafFile(const QString &inPath)
{
    AafImportResult res;
    QFile f(inPath);
    if (!f.open(QIODevice::ReadOnly)) {
        res.error = QStringLiteral("AAF ファイルを開けません: %1").arg(inPath);
        return res;
    }
    const QByteArray raw = f.readAll();

    QByteArray compBytes;
    if (!readCfbStreamByName(raw, QStringLiteral("Composition"), &compBytes, &res.error))
        return res;

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(compBytes.trimmed(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        res.error = QStringLiteral("AAF Composition ストリームの解析に失敗しました: %1").arg(perr.errorString());
        return res;
    }

    const QJsonObject root = doc.object();
    const QJsonObject compMob = root.value(QStringLiteral("compositionMob")).toObject();
    res.projectName = compMob.value(QStringLiteral("name")).toString(QFileInfo(inPath).baseName());
    res.fps = qBound(1.0, compMob.value(QStringLiteral("editRate")).toDouble(30.0), 240.0);

    // Build MasterMob lookup table (mobId -> locator & duration)
    struct MasterInfo { QString locator; double durationSec = 0.0; };
    QHash<QString, MasterInfo> masters;
    for (const QJsonValue &v : root.value(QStringLiteral("masterMobs")).toArray()) {
        const QJsonObject o = v.toObject();
        masters.insert(o.value(QStringLiteral("mobId")).toString(),
                       {o.value(QStringLiteral("networkLocator")).toString(),
                        o.value(QStringLiteral("sourceDurationSec")).toDouble(0.0)});
    }

    for (const QJsonValue &sv : root.value(QStringLiteral("slots")).toArray()) {
        const QJsonObject so = sv.toObject();
        AafTrack tr;
        tr.slotId = so.value(QStringLiteral("slotId")).toInt(1);
        tr.isAudio = (so.value(QStringLiteral("mediaKind")).toString() == QStringLiteral("Sound"));
        tr.name = so.value(QStringLiteral("name")).toString();

        double pendingGap = 0.0;
        for (const QJsonValue &cv : so.value(QStringLiteral("components")).toArray()) {
            const QJsonObject co = cv.toObject();
            const QString cls = co.value(QStringLiteral("class")).toString();
            if (cls == QStringLiteral("Filler")) {
                pendingGap += qMax(0.0, co.value(QStringLiteral("lengthSec")).toDouble(0.0));
                continue;
            }
            if (cls == QStringLiteral("SourceClip")) {
                AafSourceClip sc;
                sc.mobId = co.value(QStringLiteral("sourceMobId")).toString();
                sc.displayName = co.value(QStringLiteral("displayName")).toString();
                sc.inPointSec = qMax(0.0, co.value(QStringLiteral("startSec")).toDouble(0.0));
                const double lenSec = qMax(0.0, co.value(QStringLiteral("lengthSec")).toDouble(0.0));
                sc.outPointSec = co.contains(QStringLiteral("endSec"))
                    ? qMax(sc.inPointSec, co.value(QStringLiteral("endSec")).toDouble(sc.inPointSec + lenSec))
                    : (sc.inPointSec + lenSec);
                sc.leadInGapSec = pendingGap;
                pendingGap = 0.0;
                sc.speed = co.value(QStringLiteral("speed")).toDouble(1.0);
                sc.volume = co.value(QStringLiteral("volume")).toDouble(1.0);
                sc.roleId = co.value(QStringLiteral("role")).toString();

                const auto it = masters.constFind(sc.mobId);
                if (it != masters.constEnd()) {
                    sc.filePath = it.value().locator;
                    sc.sourceDurationSec = qMax(sc.outPointSec, it.value().durationSec);
                } else {
                    sc.sourceDurationSec = sc.outPointSec;
                }
                tr.clips.append(sc);
            }
        }
        if (tr.isAudio)
            res.audioTracks.append(tr);
        else
            res.videoTracks.append(tr);
    }

    for (const QJsonValue &mv : root.value(QStringLiteral("markers")).toArray()) {
        const QJsonObject mo = mv.toObject();
        AafMarker m;
        m.timeSec = qMax(0.0, mo.value(QStringLiteral("positionSec")).toDouble(0.0));
        m.label = mo.value(QStringLiteral("comment")).toString();
        m.colorHex = mo.value(QStringLiteral("color")).toString(QStringLiteral("#ff5050"));
        res.markers.append(m);
    }

    res.ok = true;
    return res;
}

bool applyAafToTimeline(Timeline *timeline,
                        const AafImportResult &parsed,
                        QString *errorOut)
{
    if (!timeline) {
        if (errorOut) *errorOut = QStringLiteral("タイムラインが無効です。");
        return false;
    }
    if (!parsed.ok) {
        if (errorOut) *errorOut = parsed.error.isEmpty() ? QStringLiteral("AAF データが無効です。") : parsed.error;
        return false;
    }

    while (timeline->videoTracks().size() < parsed.videoTracks.size())
        timeline->addVideoTrack();
    while (timeline->audioTracks().size() < parsed.audioTracks.size())
        timeline->addAudioTrack();

    auto populate = [](const QVector<TimelineTrack *> &dstTracks, const QVector<AafTrack> &srcTracks) {
        for (int i = 0; i < dstTracks.size(); ++i) {
            if (!dstTracks[i]) continue;
            QVector<ClipInfo> clips;
            if (i < srcTracks.size()) {
                for (const AafSourceClip &sc : srcTracks[i].clips) {
                    ClipInfo c;
                    c.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    c.filePath = sc.filePath;
                    c.displayName = sc.displayName;
                    c.inPoint = sc.inPointSec;
                    c.outPoint = sc.outPointSec;
                    c.duration = qMax(sc.sourceDurationSec, sc.outPointSec);
                    c.leadInSec = sc.leadInGapSec;
                    c.speed = (sc.speed > 0.0) ? sc.speed : 1.0;
                    c.volume = sc.volume;
                    if (!sc.roleId.isEmpty()) {
                        const cliproles::Role r = cliproles::roleFromId(sc.roleId);
                        if (r != cliproles::Role::None)
                            cliproles::setRoleForClip(c.uid, r);
                    }
                    clips.append(c);
                }
            }
            dstTracks[i]->setClips(clips);
        }
    };

    populate(timeline->videoTracks(), parsed.videoTracks);
    populate(timeline->audioTracks(), parsed.audioTracks);

    for (const AafMarker &m : parsed.markers) {
        const qint64 us = static_cast<qint64>(std::llround(m.timeSec * 1e6));
        timeline->addMarker(us, m.label, QColor(m.colorHex));
    }

    timeline->saveUndoState(QStringLiteral("AAF タイムラインをインポート (%1)").arg(parsed.projectName));
    return true;
}

// ============================================================================
// AafExchangeDialog
// ============================================================================

AafExchangeDialog::AafExchangeDialog(const AafExportOptions &defaults, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("AAF エクスポート設定 (Avid / Pro Tools / Resolve 互換)"));
    setMinimumWidth(420);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_nameEdit = new QLineEdit(defaults.projectName, this);
    form->addRow(QStringLiteral("コンポジション名:"), m_nameEdit);

    m_fpsSpin = new QDoubleSpinBox(this);
    m_fpsSpin->setRange(1.0, 240.0);
    m_fpsSpin->setDecimals(2);
    m_fpsSpin->setValue(defaults.fps);
    m_fpsSpin->setSuffix(QStringLiteral(" fps"));
    form->addRow(QStringLiteral("エディットレート:"), m_fpsSpin);

    m_videoCheck = new QCheckBox(QStringLiteral("ビデオトラックを含める"), this);
    m_videoCheck->setChecked(defaults.includeVideo);
    form->addRow(m_videoCheck);

    m_audioCheck = new QCheckBox(QStringLiteral("オーディオトラックを含める (ボリューム / ロール保持)"), this);
    m_audioCheck->setChecked(defaults.includeAudio);
    form->addRow(m_audioCheck);

    m_markerCheck = new QCheckBox(QStringLiteral("タイムラインマーカーを含める"), this);
    m_markerCheck->setChecked(defaults.includeMarkers);
    form->addRow(m_markerCheck);

    root->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

AafExportOptions AafExchangeDialog::options() const
{
    AafExportOptions o;
    o.projectName = m_nameEdit->text().trimmed().isEmpty()
        ? QStringLiteral("Untitled") : m_nameEdit->text().trimmed();
    o.fps = m_fpsSpin->value();
    o.includeVideo = m_videoCheck->isChecked();
    o.includeAudio = m_audioCheck->isChecked();
    o.includeMarkers = m_markerCheck->isChecked();
    return o;
}

} // namespace aaf
