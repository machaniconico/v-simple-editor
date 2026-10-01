#include "FcpxmlImport.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>

namespace fcpx::xml {
namespace {
// Nonnegative, reduced rational times. Checked arithmetic keeps all placement
// calculations exact until the final conversion into Timeline's seconds.
struct Time {
    qint64 n = 0, d = 1;
    double seconds() const { return double(n) / double(d); }
};
bool time(const QString &value, Time &out)
{
    if (!value.endsWith('s')) return false;
    const auto parts = value.chopped(1).split('/');
    if (parts.size() > 2) return false;
    bool a = false, b = true;
    const qint64 n = parts[0].toLongLong(&a);
    const qint64 d = parts.size() == 2 ? parts[1].toLongLong(&b) : 1;
    if (!a || !b || n < 0 || d <= 0) return false;
    const qint64 g = std::gcd(n, d);
    out = {n / g, d / g};
    return true;
}
bool combine(Time a, Time b, bool subtract, Time &out)
{
    const qint64 g = std::gcd(a.d, b.d), x = b.d / g, y = a.d / g;
    const auto max = std::numeric_limits<qint64>::max();
    if (a.n > max / x || b.n > max / y || a.d > max / x) return false;
    const qint64 left = a.n * x, right = b.n * y;
    if (subtract ? left < right : left > max - right) return false;
    out = {subtract ? left - right : left + right, a.d * x};
    const qint64 divisor = std::gcd(out.n, out.d);
    out.n /= divisor; out.d /= divisor;
    return true;
}
struct Asset { QString path, name, format; Time start; bool audioOnly = false; };
struct Entry { QString ref, name; int lane = 0; Time offset, duration, start; };
}
ImportResult parseXml(const QString &xml, const QString &directory)
{
    ImportResult result;
    QXmlStreamReader reader(xml);
    QMap<QString, Asset> assets;
    QMap<QString, QXmlStreamAttributes> formats;
    QVector<Entry> entries;
    QStringList stack;
    Time origin;
    QString sequenceFormat;
    bool root = false, sequence = false, spine = false;
    auto warnSkip = [&] {
        result.warnings.append(QStringLiteral("未対応の要素を読み飛ばしました: %1 (行 %2)")
            .arg(reader.name().toString()).arg(reader.lineNumber()));
        reader.skipCurrentElement();
    };
    auto readTime = [&](const QXmlStreamAttributes &a, const QString &key, Time &v, bool required = false) {
        if (!a.hasAttribute(key) && !required) return true;
        if (time(a.value(key).toString(), v)) return true;
        reader.raiseError(QStringLiteral("不正な時刻: %1").arg(key)); return false;
    };
    std::function<void(const Time *, const Time *)> readAssetClip;
    readAssetClip = [&](const Time *parentOffset, const Time *parentStart) {
        const auto a = reader.attributes();
        Entry e; e.ref = a.value("ref").toString(); e.name = a.value("name").toString();
        bool laneOk = true;
        if (a.hasAttribute("lane")) e.lane = a.value("lane").toInt(&laneOk);
        QString invalid;
        if (!laneOk) invalid = QStringLiteral("lane");
        for (const auto &key : {QStringLiteral("offset"), QStringLiteral("duration"), QStringLiteral("start")}) {
            Time *value = key == QStringLiteral("offset") ? &e.offset
                : key == QStringLiteral("duration") ? &e.duration : &e.start;
            if ((a.hasAttribute(key) || key == QStringLiteral("duration"))
                && !time(a.value(key).toString(), *value)) invalid = key;
        }
        if (parentOffset && invalid.isEmpty()) {
            Time relative, absolute;
            if (!combine(e.offset, *parentStart, true, relative)
                || !combine(*parentOffset, relative, false, absolute))
                invalid = QStringLiteral("接続位置");
            else e.offset = absolute;
        }
        if (!invalid.isEmpty()) {
            result.warnings.append(QStringLiteral("クリップの値が不正です (%1): %2 / %3")
                .arg(invalid, e.name, e.ref));
            reader.skipCurrentElement();
            return;
        }
        entries.append(e);
        while (reader.readNextStartElement()) {
            if (reader.name() == QLatin1String("asset-clip")) readAssetClip(&e.offset, &e.start);
            else warnSkip();
        }
    };
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isEndElement()) { if (!stack.isEmpty()) stack.removeLast(); continue; }
        if (!reader.isStartElement()) continue;
        const QString tag = reader.name().toString(), parent = stack.value(stack.size() - 1);
        const auto a = reader.attributes();
        if (stack.isEmpty()) {
            if (root || tag != QStringLiteral("fcpxml")) { reader.raiseError(QStringLiteral("FCPXML ではありません。")); break; }
            root = true;
        } else if (tag == "resources" && parent == "fcpxml") {
        } else if (tag == "format" && parent == "resources") {
            formats.insert(a.value("id").toString(), a);
            reader.skipCurrentElement(); continue;
        } else if (tag == "asset" && parent == "resources") {
            Asset asset;
            asset.path = a.value("src").toString(); asset.name = a.value("name").toString();
            asset.format = a.value("format").toString();
            asset.audioOnly = a.value("hasVideo") == QLatin1String("0")
                || (!a.hasAttribute("hasVideo") && a.value("hasAudio") == QLatin1String("1"));
            if (!readTime(a, QStringLiteral("start"), asset.start)) break;
            const QString id = a.value("id").toString();
            if (id.isEmpty() || assets.contains(id)) { reader.raiseError(QStringLiteral("素材 ID が不正です。")); break; }
            while (reader.readNextStartElement()) {
                if (reader.name() == QLatin1String("media-rep") && reader.attributes().value("kind") == QLatin1String("original-media")) {
                    asset.path = reader.attributes().value("src").toString(); reader.skipCurrentElement();
                } else warnSkip();
            }
            assets.insert(id, asset); continue;
        } else if ((tag == "library" && parent == "fcpxml")
                   || (tag == "event" && (parent == "library" || parent == "fcpxml"))) {
        } else if (tag == "project" && (parent == "event" || parent == "fcpxml")) {
            if (sequence) { warnSkip(); continue; }
            result.title = a.value("name").toString();
        } else if (tag == "sequence" && parent == "project" && !sequence) {
            sequence = true; sequenceFormat = a.value("format").toString();
            if (!readTime(a, QStringLiteral("tcStart"), origin)) break;
        } else if (tag == "spine" && parent == "sequence" && !spine) {
            spine = true;
        } else if (tag == "asset-clip" && parent == "spine") {
            readAssetClip(nullptr, nullptr);
            continue;
        } else { warnSkip(); continue; }
        stack.append(tag);
    }
    if (reader.hasError()) result.error = QStringLiteral("FCPXML 読み込みエラー (行 %1): %2").arg(reader.lineNumber()).arg(reader.errorString());
    else if (!root || !sequence || !spine) result.error = QStringLiteral("読み込めるシーケンスがありません。");
    if (!result.error.isEmpty()) return result;
    // Format metadata is interpreted but cannot replace global project settings
    // when importing into a new sequence of an existing project.
    QSet<QString> usedFormats;
    if (!sequenceFormat.isEmpty()) usedFormats.insert(sequenceFormat);
    for (const auto &e : entries) if (assets.contains(e.ref) && !assets[e.ref].format.isEmpty()) usedFormats.insert(assets[e.ref].format);
    for (const auto &id : usedFormats) {
        const auto f = formats.value(id);
        Time frame;
        if (f.isEmpty() || !time(f.value("frameDuration").toString(), frame) || frame.n == 0)
            result.warnings.append(QStringLiteral("未解決または不正なフォーマット: %1").arg(id));
        else result.warnings.append(QStringLiteral("素材フォーマット %1: %2×%3、フレーム時間 %4。プロジェクトの映像設定を維持します。")
            .arg(id, f.value("width").toString(), f.value("height").toString(), f.value("frameDuration").toString()));
    }
    QMap<int, QVector<Entry>> videoLanes, audioLanes;
    for (const auto &e : entries) {
        if (!assets.contains(e.ref)) {
            result.warnings.append(QStringLiteral("未定義の素材参照を読み飛ばしました: %1").arg(e.ref));
            continue;
        }
        (assets.value(e.ref).audioOnly ? audioLanes : videoLanes)[e.lane].append(e);
    }
    result.videoTracks.clear();
    result.audioTracks.clear();
    QSet<QString> found, missing;
    auto importLanes = [&](const QMap<int, QVector<Entry>> &lanes, QVector<QVector<ClipInfo>> &tracks) {
        auto laneKeys = lanes.keys();
        std::sort(laneKeys.begin(), laneKeys.end(), [](int a, int b) {
            if (a == 0 || b == 0) return a == 0 && b != 0;
            const qint64 absA = a < 0 ? -qint64(a) : qint64(a);
            const qint64 absB = b < 0 ? -qint64(b) : qint64(b);
            if (absA != absB) return absA < absB;
            return a > b;
        });
        for (int lane : laneKeys) {
            if (tracks.size() == 2) {
                result.warnings.append(QStringLiteral("2 トラックの上限により lane %1 を読み飛ばしました。").arg(lane));
                continue;
            }
            auto clips = lanes.value(lane);
            // Stable ordering via Euclidean continued fractions avoids overflowing
            // cross-products and does not round rational offsets to seconds.
            std::stable_sort(clips.begin(), clips.end(), [](const Entry &a, const Entry &b) {
                qint64 an = a.offset.n, ad = a.offset.d, bn = b.offset.n, bd = b.offset.d;
                bool reverse = false;
                for (;;) {
                    if (an / ad != bn / bd) return reverse ? an / ad > bn / bd : an / ad < bn / bd;
                    an %= ad; bn %= bd;
                    if (!an || !bn) return reverse ? an > bn : an < bn;
                    std::swap(an, ad); std::swap(bn, bd); reverse = !reverse;
                }
            });
            QVector<ClipInfo> track;
            Time end;
            for (const auto &e : clips) {
                const auto asset = assets.value(e.ref);
                Time offset, gap, start, out, nextEnd;
                if (!e.duration.n || !combine(e.offset, origin, true, offset)
                    || !combine(e.start, asset.start, true, start)
                    || !combine(start, e.duration, false, out)
                    || !combine(offset, e.duration, false, nextEnd)) {
                    result.warnings.append(QStringLiteral("lane %1 の時刻範囲が不正なクリップを読み飛ばしました: %2 / %3")
                        .arg(lane).arg(e.name, e.ref));
                    continue;
                }
                if (!combine(offset, end, true, gap)) {
                    result.warnings.append(QStringLiteral("lane %1 の重複クリップを読み飛ばしました: %2").arg(lane).arg(e.name));
                    continue;
                }
                const QUrl url(asset.path);
                QString path = url.isLocalFile() ? url.toLocalFile() : asset.path;
                if (path.isEmpty()) path = QStringLiteral("offline-%1").arg(e.ref);
                if (QDir::isRelativePath(path)) path = QDir(directory).absoluteFilePath(path);
                if (QFileInfo(path).isFile()) found.insert(path);
                else if (!missing.contains(path)) { missing.insert(path); result.warnings.append(QStringLiteral("オフライン素材: %1").arg(path)); }
                ClipInfo c; c.filePath = path; c.displayName = e.name.isEmpty() ? asset.name : e.name;
                c.inPoint = start.seconds(); c.outPoint = c.duration = out.seconds(); c.leadInSec = gap.seconds();
                track.append(c); end = nextEnd;
            }
            if (!track.isEmpty()) tracks.append(track);
        }
    };
    importLanes(videoLanes, result.videoTracks);
    importLanes(audioLanes, result.audioTracks);
    result.resolvedFiles = found.size(); result.unresolvedFiles = missing.size();
    if (result.videoTracks.isEmpty() && result.audioTracks.isEmpty()) {
        result.error = QStringLiteral("読み込めるクリップがありません。");
        return result;
    }
    if (result.videoTracks.isEmpty()) result.videoTracks.append(QVector<ClipInfo>{});
    if (result.audioTracks.isEmpty()) result.audioTracks.append(QVector<ClipInfo>{});
    return result;
}
ImportResult readFcpxml(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) { ImportResult r; r.error = QStringLiteral("FCPXML を開けません: %1").arg(filePath); return r; }
    auto result = parseXml(QString::fromUtf8(file.readAll()), QFileInfo(filePath).absolutePath());
    if (result.title.trimmed().isEmpty()) result.title = QFileInfo(filePath).completeBaseName();
    return result;
}
} // namespace fcpx::xml
