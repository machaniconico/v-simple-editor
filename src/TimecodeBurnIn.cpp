#include "TimecodeBurnIn.h"

#include "EdlExport.h"
#include "Timeline.h"

#include <QDate>
#include <QColor>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QStringList>
#include <QtGlobal>

#include <cmath>

namespace {

constexpr int kMinFontSizePct = 1;
constexpr int kMaxFontSizePct = 20;

double normalizedFrameRate(double frameRate)
{
    return std::isfinite(frameRate) && frameRate > 0.0 ? frameRate : 30.0;
}

} // namespace

QJsonObject TimecodeBurnInSettings::toJson() const
{
    return QJsonObject{
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("position"), positionName(position)},
        {QStringLiteral("fontSizePct"), fontSizePct},
        {QStringLiteral("showFrames"), showFrames},
        {QStringLiteral("dropFrame"), dropFrame},
        {QStringLiteral("prefix"), prefix},
        {QStringLiteral("showClipName"), showClipName},
        {QStringLiteral("showDate"), showDate},
        {QStringLiteral("opacity"), opacity}
    };
}

TimecodeBurnInSettings TimecodeBurnInSettings::fromJson(
    const QJsonObject &object)
{
    TimecodeBurnInSettings settings;
    settings.enabled = object.value(QStringLiteral("enabled"))
                           .toBool(settings.enabled);

    Position parsedPosition = settings.position;
    if (positionFromName(object.value(QStringLiteral("position")).toString(),
                         &parsedPosition)) {
        settings.position = parsedPosition;
    }

    settings.fontSizePct = qBound(
        kMinFontSizePct,
        object.value(QStringLiteral("fontSizePct")).toInt(settings.fontSizePct),
        kMaxFontSizePct);
    settings.showFrames = object.value(QStringLiteral("showFrames"))
                              .toBool(settings.showFrames);
    settings.dropFrame = object.value(QStringLiteral("dropFrame"))
                             .toBool(settings.dropFrame);
    settings.prefix = object.value(QStringLiteral("prefix"))
                          .toString(settings.prefix);
    settings.showClipName = object.value(QStringLiteral("showClipName"))
                                .toBool(settings.showClipName);
    settings.showDate = object.value(QStringLiteral("showDate"))
                            .toBool(settings.showDate);
    const double opacityValue = object.value(QStringLiteral("opacity"))
                                    .toDouble(settings.opacity);
    settings.opacity = std::isfinite(opacityValue)
        ? qBound(0.0, opacityValue, 1.0)
        : TimecodeBurnInSettings{}.opacity;
    return settings;
}

QString TimecodeBurnInSettings::positionName(Position position)
{
    switch (position) {
    case TopLeft:      return QStringLiteral("topLeft");
    case TopCenter:    return QStringLiteral("topCenter");
    case TopRight:     return QStringLiteral("topRight");
    case BottomLeft:   return QStringLiteral("bottomLeft");
    case BottomCenter: return QStringLiteral("bottomCenter");
    case BottomRight:  return QStringLiteral("bottomRight");
    }
    return QStringLiteral("bottomCenter");
}

bool TimecodeBurnInSettings::positionFromName(const QString &name,
                                               Position *position)
{
    Position parsed;
    if (name == QLatin1String("topLeft"))
        parsed = TopLeft;
    else if (name == QLatin1String("topCenter"))
        parsed = TopCenter;
    else if (name == QLatin1String("topRight"))
        parsed = TopRight;
    else if (name == QLatin1String("bottomLeft"))
        parsed = BottomLeft;
    else if (name == QLatin1String("bottomCenter"))
        parsed = BottomCenter;
    else if (name == QLatin1String("bottomRight"))
        parsed = BottomRight;
    else
        return false;

    if (position)
        *position = parsed;
    return true;
}

TimecodeBurnInRenderer::TimecodeBurnInRenderer(
    const TimecodeBurnInSettings &settings)
    : m_settings(settings)
{
}

void TimecodeBurnInRenderer::setSettings(
    const TimecodeBurnInSettings &settings)
{
    m_settings = settings;
}

QString TimecodeBurnInRenderer::displayText(double timelineSec,
                                             double frameRate,
                                             const QString &clipName) const
{
    const double fps = normalizedFrameRate(frameRate);
    const qint64 frames = edl::secToFrames(qMax(0.0, timelineSec), fps);

    // framesToTimecode is the single owner of the EdlExport drop-frame-rate
    // test. Passing dropFrame through here deliberately avoids duplicating
    // isDropFrameRate or SMPTE frame-number arithmetic in this renderer.
    QString timecode = edl::framesToTimecode(frames, fps,
                                             m_settings.dropFrame);
    if (!m_settings.showFrames) {
        const int frameSeparator = qMax(timecode.lastIndexOf(QLatin1Char(':')),
                                        timecode.lastIndexOf(QLatin1Char(';')));
        if (frameSeparator >= 0)
            timecode.truncate(frameSeparator);
    }

    QStringList parts;
    if (!m_settings.prefix.isEmpty())
        parts.append(m_settings.prefix);
    if (m_settings.showDate)
        parts.append(QDate::currentDate().toString(Qt::ISODate));
    parts.append(timecode);
    if (m_settings.showClipName && !clipName.isEmpty())
        parts.append(clipName);
    return parts.join(QLatin1Char(' '));
}

void TimecodeBurnInRenderer::paintOnto(QPainter &painter,
                                        const QRectF &frameRect,
                                        double timelineSec,
                                        double frameRate,
                                        const QString &clipName) const
{
    if (!m_settings.enabled || m_settings.opacity <= 0.0
        || !frameRect.isValid() || frameRect.isEmpty()) {
        return;
    }

    QString text = displayText(timelineSec, frameRate, clipName);
    if (text.isEmpty())
        return;

    const int fontPixels = qMax(
        1, qRound(frameRect.height()
                  * qBound(kMinFontSizePct, m_settings.fontSizePct,
                           kMaxFontSizePct) / 100.0));
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(fontPixels);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);

    const QFontMetricsF metrics(font);
    const qreal paddingX = qMax<qreal>(2.0, fontPixels * 0.35);
    const qreal paddingY = qMax<qreal>(1.0, fontPixels * 0.20);
    const qreal margin = qMax<qreal>(4.0, frameRect.height() * 0.02);
    const qreal availableTextWidth = qMax<qreal>(
        1.0, frameRect.width() - 2.0 * (margin + paddingX));
    text = metrics.elidedText(text, Qt::ElideRight, availableTextWidth);

    const qreal boxWidth = qMin(
        frameRect.width() - 2.0 * margin,
        metrics.horizontalAdvance(text) + 2.0 * paddingX);
    const qreal boxHeight = qMin(
        frameRect.height() - 2.0 * margin,
        metrics.height() + 2.0 * paddingY);
    if (boxWidth <= 0.0 || boxHeight <= 0.0)
        return;

    qreal x = frameRect.left() + margin;
    qreal y = frameRect.top() + margin;
    switch (m_settings.position) {
    case TimecodeBurnInSettings::TopCenter:
    case TimecodeBurnInSettings::BottomCenter:
        x = frameRect.center().x() - boxWidth / 2.0;
        break;
    case TimecodeBurnInSettings::TopRight:
    case TimecodeBurnInSettings::BottomRight:
        x = frameRect.right() - margin - boxWidth;
        break;
    case TimecodeBurnInSettings::TopLeft:
    case TimecodeBurnInSettings::BottomLeft:
        break;
    }
    switch (m_settings.position) {
    case TimecodeBurnInSettings::BottomLeft:
    case TimecodeBurnInSettings::BottomCenter:
    case TimecodeBurnInSettings::BottomRight:
        y = frameRect.bottom() - margin - boxHeight;
        break;
    case TimecodeBurnInSettings::TopLeft:
    case TimecodeBurnInSettings::TopCenter:
    case TimecodeBurnInSettings::TopRight:
        break;
    }

    const QRectF box(x, y, boxWidth, boxHeight);
    painter.save();
    painter.setClipRect(frameRect);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setOpacity(qBound(0.0, m_settings.opacity, 1.0));
    painter.fillRect(box, QColor(0, 0, 0, 210));
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(box.adjusted(paddingX, paddingY,
                                  -paddingX, -paddingY),
                     Qt::AlignCenter | Qt::TextSingleLine, text);
    painter.restore();
}

QString timecodeBurnInClipNameAt(const Timeline *timeline, double timelineSec)
{
    if (!timeline || !std::isfinite(timelineSec))
        return {};

    const auto &tracks = timeline->videoTracks();
    for (int trackIndex = tracks.size() - 1; trackIndex >= 0; --trackIndex) {
        const TimelineTrack *track = tracks.at(trackIndex);
        if (!track || track->isHidden())
            continue;

        double cursor = 0.0;
        for (const ClipInfo &clip : track->clips()) {
            const double start = cursor + qMax(0.0, clip.leadInSec);
            const double end = start + qMax(0.0, clip.effectiveDuration());
            if (timelineSec >= start && timelineSec < end) {
                if (clip.isAdjustment)
                    break;
                if (!clip.displayName.isEmpty())
                    return clip.displayName;
                return QFileInfo(clip.filePath).completeBaseName();
            }
            cursor = end;
        }
    }
    return {};
}
