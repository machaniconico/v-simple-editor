#include <QDebug>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>

#include "../MediaPool.h"

namespace {

using mediapool::MediaPool;
using mediapool::MediaAsset;
using mediapool::MediaType;
using mediapool::SmartBin;

// テスト用の素材を組み立てる小ヘルパ (純粋ロジック、決定的)。
MediaAsset makeAsset(const QString& filePath,
                     const QString& displayName,
                     MediaType type,
                     const QStringList& tags = {},
                     const QString& comment = {})
{
    MediaAsset a;
    a.filePath = filePath;
    a.displayName = displayName;
    a.type = type;
    a.tags = tags;
    a.comment = comment;
    a.importedAtIso = QStringLiteral("2026-05-30T00:00:00Z");
    return a;
}

} // namespace

int runMediaPoolSelftest()
{
    qInfo().noquote() << "[media-pool] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[media-pool] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[media-pool] FAIL" << name << ":" << msg; };

    // G1: addAsset が新規 id を採番して assets() に入る
    {
        MediaPool pool;
        const int id = pool.addAsset(makeAsset(QStringLiteral("/clips/a.mp4"),
                                               QStringLiteral("Clip A"), MediaType::Video));
        const bool ok = id >= 1
            && pool.assets().size() == 1
            && pool.getAsset(id) != nullptr
            && pool.getAsset(id)->displayName == QStringLiteral("Clip A");
        if (ok) {
            pass("G1 addAsset assigns id and stores asset");
        } else {
            fail("G1 addAsset", QStringLiteral("id=%1 count=%2").arg(id).arg(pool.assets().size()));
        }
    }

    // G2: 同一 filePath の addAsset は新規を作らず既存 id を返す (重複排除)
    {
        MediaPool pool;
        const int id1 = pool.addAsset(makeAsset(QStringLiteral("/clips/dup.mp4"),
                                                QStringLiteral("First"), MediaType::Video));
        const int id2 = pool.addAsset(makeAsset(QStringLiteral("/clips/dup.mp4"),
                                                QStringLiteral("Second"), MediaType::Video));
        const bool ok = id1 == id2 && pool.assets().size() == 1;
        if (ok) {
            pass("G2 duplicate filePath returns existing id");
        } else {
            fail("G2 dedup", QStringLiteral("id1=%1 id2=%2 count=%3")
                    .arg(id1).arg(id2).arg(pool.assets().size()));
        }
    }

    // G3: createBin + moveAssetToBin + assetsInBin が一致
    {
        MediaPool pool;
        const int id = pool.addAsset(makeAsset(QStringLiteral("/clips/b.mp4"),
                                               QStringLiteral("Clip B"), MediaType::Video));
        const QString binId = pool.createBin(QStringLiteral("Bin1"));
        const bool moved = pool.moveAssetToBin(id, binId);
        const bool invalidMoveRejected =
            !pool.moveAssetToBin(id, QStringLiteral("missing-bin"));
        const QVector<MediaAsset> inBin = pool.assetsInBin(binId);
        const bool ok = !binId.isEmpty() && moved
            && invalidMoveRejected
            && inBin.size() == 1 && inBin.first().id == id;
        if (ok) {
            pass("G3 createBin + moveAssetToBin + assetsInBin agree");
        } else {
            fail("G3 bin move", QStringLiteral("binId=%1 moved=%2 inBin=%3")
                    .arg(binId).arg(moved).arg(inBin.size()));
        }
    }

    // G4: removeBin で配下 asset の binId が空に戻る
    {
        MediaPool pool;
        const int id = pool.addAsset(makeAsset(QStringLiteral("/clips/c.mp4"),
                                               QStringLiteral("Clip C"), MediaType::Video));
        const QString binId = pool.createBin(QStringLiteral("Bin2"));
        pool.moveAssetToBin(id, binId);
        const bool removed = pool.removeBin(binId);
        const MediaAsset* a = pool.getAsset(id);
        const bool ok = removed && a != nullptr && a->binId.isEmpty()
            && pool.assetsInBin(QString()).size() == 1;
        if (ok) {
            pass("G4 removeBin resets contained asset binId to root");
        } else {
            fail("G4 removeBin", QStringLiteral("removed=%1 binId=%2")
                    .arg(removed).arg(a ? a->binId : QStringLiteral("<null>")));
        }
    }

    // G5: search(displayName 部分) がヒット
    {
        MediaPool pool;
        const int id = pool.addAsset(makeAsset(QStringLiteral("/clips/sunset.mp4"),
                                               QStringLiteral("Sunset Beach"), MediaType::Video));
        const QVector<MediaAsset> hits = pool.search(QStringLiteral("Beach"));
        const bool ok = hits.size() == 1 && hits.first().id == id;
        if (ok) {
            pass("G5 search by displayName substring hits");
        } else {
            fail("G5 search name", QStringLiteral("hits=%1").arg(hits.size()));
        }
    }

    // G6: search が case-insensitive (大文字クエリで小文字 tag/name にヒット)
    {
        MediaPool pool;
        const int id = pool.addAsset(makeAsset(QStringLiteral("/clips/d.mp4"),
                                               QStringLiteral("ocean view"), MediaType::Video,
                                               { QStringLiteral("nature") }));
        const QVector<MediaAsset> byName = pool.search(QStringLiteral("OCEAN"));
        const QVector<MediaAsset> byTag = pool.search(QStringLiteral("NATURE"));
        const bool ok = byName.size() == 1 && byName.first().id == id
            && byTag.size() == 1 && byTag.first().id == id;
        if (ok) {
            pass("G6 search is case-insensitive for name and tag");
        } else {
            fail("G6 case-insensitive", QStringLiteral("byName=%1 byTag=%2")
                    .arg(byName.size()).arg(byTag.size()));
        }
    }

    // G7: search(tag) がヒット
    {
        MediaPool pool;
        const int idA = pool.addAsset(makeAsset(QStringLiteral("/clips/e.mp4"),
                                                QStringLiteral("Take 1"), MediaType::Video,
                                                { QStringLiteral("interview"), QStringLiteral("approved") }));
        pool.addAsset(makeAsset(QStringLiteral("/clips/f.mp4"),
                                QStringLiteral("Take 2"), MediaType::Video,
                                { QStringLiteral("broll") }));
        const QVector<MediaAsset> hits = pool.search(QStringLiteral("approved"));
        const bool ok = hits.size() == 1 && hits.first().id == idA;
        if (ok) {
            pass("G7 search by tag hits");
        } else {
            fail("G7 search tag", QStringLiteral("hits=%1").arg(hits.size()));
        }
    }

    // G8: evaluateSmartBin の typeFilter で型一致のみ返る
    {
        MediaPool pool;
        const int vid = pool.addAsset(makeAsset(QStringLiteral("/clips/v.mp4"),
                                                QStringLiteral("Video clip"), MediaType::Video));
        pool.addAsset(makeAsset(QStringLiteral("/clips/a.wav"),
                                QStringLiteral("Audio clip"), MediaType::Audio));
        SmartBin sb;
        sb.name = QStringLiteral("Only Video");
        sb.typeFilter = MediaType::Video;
        const QString sbId = pool.addSmartBin(sb);
        const QVector<MediaAsset> result = pool.evaluateSmartBin(sbId);
        const bool ok = result.size() == 1 && result.first().id == vid
            && result.first().type == MediaType::Video;
        if (ok) {
            pass("G8 evaluateSmartBin typeFilter returns only matching type");
        } else {
            fail("G8 smartbin type", QStringLiteral("sbId=%1 result=%2")
                    .arg(sbId).arg(result.size()));
        }
    }

    // G9: evaluateSmartBin の tagFilter で tag 一致のみ返る
    {
        MediaPool pool;
        const int idA = pool.addAsset(makeAsset(QStringLiteral("/clips/g.mp4"),
                                                QStringLiteral("Hero shot"), MediaType::Video,
                                                { QStringLiteral("hero") }));
        pool.addAsset(makeAsset(QStringLiteral("/clips/h.mp4"),
                                QStringLiteral("Filler"), MediaType::Video,
                                { QStringLiteral("filler") }));
        SmartBin sb;
        sb.name = QStringLiteral("Hero only");
        sb.tagFilter = QStringLiteral("hero");
        const QString sbId = pool.addSmartBin(sb);
        const QVector<MediaAsset> result = pool.evaluateSmartBin(sbId);
        const bool ok = result.size() == 1 && result.first().id == idA;
        if (ok) {
            pass("G9 evaluateSmartBin tagFilter returns only tag-matching");
        } else {
            fail("G9 smartbin tag", QStringLiteral("sbId=%1 result=%2")
                    .arg(sbId).arg(result.size()));
        }
    }

    // G10: toJson -> fromJson の round-trip で件数と代表フィールドが保存される
    {
        MediaPool pool;
        const int id = pool.addAsset(makeAsset(QStringLiteral("/clips/rt.mp4"),
                                               QStringLiteral("RoundTrip"), MediaType::Video,
                                               { QStringLiteral("keep") },
                                               QStringLiteral("note")));
        const QString binId = pool.createBin(QStringLiteral("RTBin"));
        pool.moveAssetToBin(id, binId);
        SmartBin sb;
        sb.name = QStringLiteral("RTSmart");
        sb.typeFilter = MediaType::Video;
        const QString smartId = pool.addSmartBin(sb);

        QJsonObject json = pool.toJson();
        json.remove(QStringLiteral("nextAssetId"));
        json.remove(QStringLiteral("nextBinId"));
        json.remove(QStringLiteral("nextSmartBinId"));
        MediaPool restored;
        restored.fromJson(json);

        const int nextAssetId =
            restored.addAsset(makeAsset(QStringLiteral("/clips/next.mp4"),
                                        QStringLiteral("Next"), MediaType::Video));
        const QString nextBinId = restored.createBin(QStringLiteral("NextBin"));
        const QString nextSmartId = restored.addSmartBin(sb);
        const MediaAsset* a = restored.getAsset(id);
        const bool ok = restored.assets().size() == pool.assets().size()
            + 1
            && restored.bins().size() == pool.bins().size()
            + 1
            && restored.smartBins().size() == pool.smartBins().size()
            + 1
            && a != nullptr
            && a->displayName == QStringLiteral("RoundTrip")
            && a->binId == binId
            && a->tags.contains(QStringLiteral("keep"))
            && a->comment == QStringLiteral("note")
            && a->type == MediaType::Video
            && nextAssetId != id
            && nextBinId != binId
            && nextSmartId != smartId;
        if (ok) {
            pass("G10 toJson/fromJson round-trip preserves counts and fields");
        } else {
            fail("G10 round-trip",
                 QStringLiteral("assets=%1 bins=%2 smartBins=%3 assetOk=%4")
                    .arg(restored.assets().size())
                    .arg(restored.bins().size())
                    .arg(restored.smartBins().size())
                    .arg(a != nullptr));
        }
    }

    // G11: removeAsset で消える
    {
        MediaPool pool;
        const int id = pool.addAsset(makeAsset(QStringLiteral("/clips/del.mp4"),
                                               QStringLiteral("Delete me"), MediaType::Video));
        const bool removed = pool.removeAsset(id);
        const bool ok = removed
            && pool.assets().isEmpty()
            && pool.getAsset(id) == nullptr
            && !pool.removeAsset(id); // 二重削除は false
        if (ok) {
            pass("G11 removeAsset removes the asset");
        } else {
            fail("G11 removeAsset", QStringLiteral("removed=%1 count=%2")
                    .arg(removed).arg(pool.assets().size()));
        }
    }

    qInfo().noquote().nospace() << "[media-pool] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
