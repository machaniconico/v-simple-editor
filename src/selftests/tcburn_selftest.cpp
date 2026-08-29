#include "../TimecodeBurnIn.h"

#include <QImage>
#include <QIODevice>
#include <QPainter>
#include <QRegularExpression>
#include <QTextStream>

#include <array>
#include <cstddef>
#include <cstring>
#include <cstdio>

namespace {

bool imagesEqual(const QImage &left, const QImage &right)
{
    if (left.size() != right.size() || left.format() != right.format()
        || left.sizeInBytes() != right.sizeInBytes()) {
        return false;
    }
    return std::memcmp(left.constBits(), right.constBits(),
                       static_cast<std::size_t>(left.sizeInBytes())) == 0;
}

QRect changedBounds(const QImage &before, const QImage &after)
{
    QRect bounds;
    bool changed = false;
    for (int y = 0; y < after.height(); ++y) {
        for (int x = 0; x < after.width(); ++x) {
            if (before.pixel(x, y) == after.pixel(x, y))
                continue;
            const QRect pixelRect(x, y, 1, 1);
            bounds = changed ? bounds.united(pixelRect) : pixelRect;
            changed = true;
        }
    }
    return bounds;
}

} // namespace

int runTcburnSelftest()
{
    QTextStream err(stderr, QIODevice::WriteOnly);
    int passed = 0;
    int failed = 0;
    const auto pass = [&](const QString &gate, const QString &description) {
        ++passed;
        err << "PASS " << gate << ' ' << description << '\n';
        err.flush();
    };
    const auto fail = [&](const QString &gate, const QString &description) {
        ++failed;
        err << "FAIL " << gate << ' ' << description << '\n';
        err.flush();
    };

    {
        QImage image(480, 270, QImage::Format_ARGB32);
        image.fill(qRgba(31, 47, 63, 255));
        const QImage before = image.copy();
        TimecodeBurnInSettings settings;
        settings.enabled = false;
        TimecodeBurnInRenderer renderer(settings);
        QPainter painter(&image);
        renderer.paintOnto(painter, QRectF(image.rect()), 12.0, 30.0,
                           QStringLiteral("clip.mov"));
        painter.end();
        imagesEqual(before, image)
            ? pass(QStringLiteral("G1"),
                   QStringLiteral("enabled=false leaves every pixel unchanged"))
            : fail(QStringLiteral("G1"),
                   QStringLiteral("disabled renderer changed pixels"));
    }

    {
        struct PositionExpectation {
            TimecodeBurnInSettings::Position position;
            int column;
            bool bottom;
        };
        const std::array<PositionExpectation, 6> expectations{{
            {TimecodeBurnInSettings::TopLeft, 0, false},
            {TimecodeBurnInSettings::TopCenter, 1, false},
            {TimecodeBurnInSettings::TopRight, 2, false},
            {TimecodeBurnInSettings::BottomLeft, 0, true},
            {TimecodeBurnInSettings::BottomCenter, 1, true},
            {TimecodeBurnInSettings::BottomRight, 2, true}
        }};

        bool allPositionsMatch = true;
        for (const PositionExpectation &expectation : expectations) {
            QImage image(600, 300, QImage::Format_ARGB32);
            image.fill(qRgba(70, 90, 110, 255));
            const QImage before = image.copy();
            TimecodeBurnInSettings settings;
            settings.enabled = true;
            settings.position = expectation.position;
            settings.fontSizePct = 6;
            settings.opacity = 1.0;
            TimecodeBurnInRenderer renderer(settings);
            QPainter painter(&image);
            renderer.paintOnto(painter, QRectF(image.rect()), 12.0, 30.0);
            painter.end();

            const QRect bounds = changedBounds(before, image);
            if (bounds.isEmpty()) {
                allPositionsMatch = false;
                break;
            }
            const int centerX = bounds.center().x();
            const int centerY = bounds.center().y();
            const bool horizontalMatch = expectation.column == 0
                ? centerX < image.width() / 3
                : (expectation.column == 1
                    ? centerX >= image.width() / 3
                        && centerX <= image.width() * 2 / 3
                    : centerX > image.width() * 2 / 3);
            const bool verticalMatch = expectation.bottom
                ? centerY > image.height() / 2
                : centerY < image.height() / 2;
            if (!horizontalMatch || !verticalMatch) {
                allPositionsMatch = false;
                break;
            }
        }
        allPositionsMatch
            ? pass(QStringLiteral("G2"),
                   QStringLiteral("all six boxes occupy their requested regions"))
            : fail(QStringLiteral("G2"),
                   QStringLiteral("one or more position boxes were misplaced"));
    }

    {
        TimecodeBurnInSettings settings;
        settings.enabled = true;
        settings.dropFrame = true;
        TimecodeBurnInRenderer renderer(settings);
        const QString text = renderer.displayText(
            60.0, 30000.0 / 1001.0);
        text.contains(QLatin1Char(';'))
            ? pass(QStringLiteral("G3"),
                   QStringLiteral("29.97 drop-frame uses a semicolon"))
            : fail(QStringLiteral("G3"),
                   QStringLiteral("drop-frame timecode lacked a semicolon"));
    }

    {
        TimecodeBurnInSettings settings;
        settings.enabled = true;
        settings.position = TimecodeBurnInSettings::TopRight;
        settings.fontSizePct = 9;
        settings.showFrames = false;
        settings.dropFrame = true;
        settings.prefix = QStringLiteral("REC A");
        settings.showClipName = true;
        settings.showDate = true;
        settings.opacity = 0.37;
        const QJsonObject encoded = settings.toJson();
        const TimecodeBurnInSettings decoded =
            TimecodeBurnInSettings::fromJson(encoded);
        decoded.toJson() == encoded
            ? pass(QStringLiteral("G4"),
                   QStringLiteral("settings JSON round-trips"))
            : fail(QStringLiteral("G4"),
                   QStringLiteral("settings JSON changed during round-trip"));
    }

    {
        TimecodeBurnInSettings settings;
        settings.enabled = true;
        settings.showFrames = false;
        TimecodeBurnInRenderer renderer(settings);
        const QString text = renderer.displayText(3723.0, 30.0);
        const QRegularExpression hhmmss(
            QStringLiteral("^\\d{2}:\\d{2}:\\d{2}$"));
        hhmmss.match(text).hasMatch() && text == QStringLiteral("01:02:03")
            ? pass(QStringLiteral("G5"),
                   QStringLiteral("showFrames=false emits HH:MM:SS only"))
            : fail(QStringLiteral("G5"),
                   QStringLiteral("frame field remained in the display text"));
    }

    err << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    err.flush();
    return failed;
}
