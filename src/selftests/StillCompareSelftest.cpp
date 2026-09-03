#include "../StillCompare.h"
#include "../StillStore.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool isColor(const QImage &image, int x, int y, const QColor &color)
{
    return x >= 0 && y >= 0 && x < image.width() && y < image.height()
        && image.pixelColor(x, y) == color;
}

bool isSolid(const QImage &image, const QColor &color)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (!isColor(image, x, y, color))
                return false;
        }
    }
    return true;
}

} // namespace

int runStillCompareSelftest()
{
    int passed = 0;
    int failed = 0;
    const auto check = [&](int gate, bool ok) {
        if (ok) {
            ++passed;
            std::fprintf(stderr, "PASS G%d\n", gate);
        } else {
            ++failed;
            std::fprintf(stderr, "FAIL G%d\n", gate);
        }
    };

    const QColor displayColor(220, 30, 30, 255);
    const QColor stillColor(20, 80, 230, 255);

    // G1: half wipes keep display on the leading side and still on the trailing side.
    {
        QImage display(10, 4, QImage::Format_ARGB32);
        QImage still(display.size(), QImage::Format_ARGB32);
        display.fill(displayColor);
        still.fill(stillColor);
        const QImage result = stillcompare::apply(
            display, still, stillcompare::Mode::WipeHorizontal, 0.5);
        bool ok = result.size() == display.size();
        for (int y = 0; y < result.height() && ok; ++y) {
            for (int x = 0; x < result.width(); ++x) {
                const QColor expected = x < 5 ? displayColor : stillColor;
                if (!isColor(result, x, y, expected)) {
                    ok = false;
                    break;
                }
            }
        }
        const QImage vertical = stillcompare::apply(
            display, still, stillcompare::Mode::WipeVertical, 0.5);
        for (int y = 0; y < vertical.height() && ok; ++y) {
            for (int x = 0; x < vertical.width(); ++x) {
                const QColor expected = y < 2 ? displayColor : stillColor;
                if (!isColor(vertical, x, y, expected)) {
                    ok = false;
                    break;
                }
            }
        }
        check(1, ok);
    }

    // G2: the endpoint positions produce complete display / complete still.
    {
        QImage display(8, 6, QImage::Format_ARGB32);
        QImage still(display.size(), QImage::Format_ARGB32);
        display.fill(displayColor);
        still.fill(stillColor);
        const QImage atZero = stillcompare::apply(
            display, still, stillcompare::Mode::WipeHorizontal, 0.0);
        const QImage atOne = stillcompare::apply(
            display, still, stillcompare::Mode::WipeHorizontal, 1.0);
        check(2, isSolid(atZero, displayColor) && isSolid(atOne, stillColor));
    }

    // G3: side-by-side mode shows each complete source in its own panel.
    {
        QImage display(12, 4, QImage::Format_ARGB32);
        QImage still(display.size(), QImage::Format_ARGB32);
        display.fill(displayColor);
        still.fill(stillColor);
        const QImage result = stillcompare::apply(
            display, still, stillcompare::Mode::SplitSideBySide, 0.5);
        check(3, result.size() == display.size()
                     && isColor(result, 2, 2, displayColor)
                     && isColor(result, 9, 2, stillColor));
    }

    // G4: a differently shaped still is letterboxed into display dimensions.
    {
        QImage display(8, 6, QImage::Format_ARGB32);
        QImage still(2, 4, QImage::Format_ARGB32);
        display.fill(displayColor);
        still.fill(stillColor);
        const QImage result = stillcompare::apply(
            display, still, stillcompare::Mode::WipeHorizontal, 1.0);
        check(4, result.size() == display.size()
                     && isColor(result, 0, 0, QColor(Qt::black))
                     && isColor(result, 3, 3, stillColor)
                     && isColor(result, 7, 5, QColor(Qt::black)));
    }

    // G5: StillStore persists metadata and PNG, removes both cleanly, and does
    // not drop the index entry when deleting the PNG fails.
    {
        QTemporaryDir temporary;
        stillstore::StillStore store;
        store.setBaseDirOverride(temporary.path());
        QImage image(7, 5, QImage::Format_ARGB32);
        image.fill(QColor(15, 25, 35, 255));
        stillstore::Still saved;
        QString error;
        const bool savedOk = temporary.isValid()
            && store.save(image, QStringLiteral("テストプロジェクト"),
                          QStringLiteral("ラベル"), &saved, &error);
        const QVector<stillstore::Still> listed = store.list(&error);
        const bool listedOk = savedOk && error.isEmpty() && listed.size() == 1
            && listed.front().id == saved.id
            && listed.front().timestamp.isValid()
            && listed.front().projectName == QStringLiteral("テストプロジェクト")
            && listed.front().label == QStringLiteral("ラベル")
            && QFile::exists(temporary.filePath(QStringLiteral("index.json")))
            && QFile::exists(saved.filePath);
        const bool removedOk = listedOk && store.remove(saved.id, &error)
            && error.isEmpty() && store.list(&error).isEmpty()
            && !QFile::exists(saved.filePath);

        stillstore::Still blocked;
        const bool blockedSaved = removedOk
            && store.save(image, QStringLiteral("削除失敗テスト"),
                          QString(), &blocked, &error)
            && QFile::remove(blocked.filePath)
            && QDir().mkpath(blocked.filePath);
        QString removeError;
        const bool removeRejected = blockedSaved
            && !store.remove(blocked.id, &removeError)
            && !removeError.isEmpty();
        QString relistError;
        const QVector<stillstore::Still> afterRejectedRemove =
            store.list(&relistError);
        const bool indexPreserved = removeRejected && relistError.isEmpty()
            && afterRejectedRemove.size() == 1
            && afterRejectedRemove.front().id == blocked.id;
        const bool cleanedUp = !blockedSaved
            || (QDir(blocked.filePath).removeRecursively()
                && store.remove(blocked.id, &error));

        QFile invalidIndex(temporary.filePath(QStringLiteral("index.json")));
        const bool invalidIndexReady = cleanedUp
            && invalidIndex.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && invalidIndex.write("{") == 1;
        invalidIndex.close();
        stillstore::Still rejected;
        QString saveError;
        const bool saveRejected = invalidIndexReady
            && !store.save(image, QStringLiteral("一覧失敗テスト"),
                           QString(), &rejected, &saveError)
            && !saveError.isEmpty();
        const QStringList orphanPngs = QDir(temporary.path()).entryList(
            QStringList{QStringLiteral("still-*.png")}, QDir::Files);
        check(5, savedOk && listedOk && removedOk && blockedSaved
                     && removeRejected && indexPreserved && cleanedUp
                     && saveRejected && orphanPngs.isEmpty());
    }

    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
