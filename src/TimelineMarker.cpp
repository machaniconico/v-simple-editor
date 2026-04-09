#include "TimelineMarker.h"

#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

// --- TimelineMarker serialisation ---

static QString markerTypeToString(MarkerType t)
{
    switch (t) {
    case MarkerType::Standard: return QStringLiteral("standard");
    case MarkerType::Chapter:  return QStringLiteral("chapter");
    case MarkerType::Todo:     return QStringLiteral("todo");
    case MarkerType::Note:     return QStringLiteral("note");
    }
    return QStringLiteral("standard");
}

static MarkerType markerTypeFromString(const QString &s)
{
    if (s == QLatin1String("chapter"))  return MarkerType::Chapter;
    if (s == QLatin1String("todo"))     return MarkerType::Todo;
    if (s == QLatin1String("note"))     return MarkerType::Note;
    return MarkerType::Standard;
}

static QString markerColorToString(MarkerColor c)
{
    switch (c) {
    case MarkerColor::Red:    return QStringLiteral("red");
    case MarkerColor::Orange: return QStringLiteral("orange");
    case MarkerColor::Yellow: return QStringLiteral("yellow");
    case MarkerColor::Green:  return QStringLiteral("green");
    case MarkerColor::Blue:   return QStringLiteral("blue");
    case MarkerColor::Purple: return QStringLiteral("purple");
    case MarkerColor::White:  return QStringLiteral("white");
    }
    return QStringLiteral("blue");
}

static MarkerColor markerColorFromString(const QString &s)
{
    if (s == QLatin1String("red"))    return MarkerColor::Red;
    if (s == QLatin1String("orange")) return MarkerColor::Orange;
    if (s == QLatin1String("yellow")) return MarkerColor::Yellow;
    if (s == QLatin1String("green"))  return MarkerColor::Green;
    if (s == QLatin1String("purple")) return MarkerColor::Purple;
    if (s == QLatin1String("white"))  return MarkerColor::White;
    return MarkerColor::Blue;
}

QJsonObject TimelineMarker::toJson() const
{
    QJsonObject obj;
    obj["time"]    = time;
    obj["name"]    = name;
    obj["type"]    = markerTypeToString(type);
    obj["color"]   = markerColorToString(color);
    obj["comment"] = comment;
    return obj;
}

TimelineMarker TimelineMarker::fromJson(const QJsonObject &obj)
{
    TimelineMarker m;
    m.time    = obj["time"].toDouble();
    m.name    = obj["name"].toString();
    m.type    = markerTypeFromString(obj["type"].toString());
    m.color   = markerColorFromString(obj["color"].toString());
    m.comment = obj["comment"].toString();
    return m;
}

// --- MarkerManager ---

void MarkerManager::addMarker(double time, const QString &name,
                               MarkerType type, MarkerColor color)
{
    TimelineMarker m;
    m.time  = time;
    m.name  = name;
    m.type  = type;
    m.color = color;
    m_markers.append(m);
    sortMarkers();
}

void MarkerManager::removeMarker(int index)
{
    if (index >= 0 && index < m_markers.size())
        m_markers.removeAt(index);
}

void MarkerManager::clearMarkers()
{
    m_markers.clear();
}

void MarkerManager::sortMarkers()
{
    std::sort(m_markers.begin(), m_markers.end(),
        [](const TimelineMarker &a, const TimelineMarker &b) { return a.time < b.time; });
}

// --- Search / navigation ---

int MarkerManager::markerAt(double time, double tolerance) const
{
    int bestIndex = -1;
    double bestDist = tolerance;

    for (int i = 0; i < m_markers.size(); ++i) {
        double dist = std::abs(m_markers[i].time - time);
        if (dist <= bestDist) {
            bestDist = dist;
            bestIndex = i;
        }
    }
    return bestIndex;
}

QVector<TimelineMarker> MarkerManager::markersByType(MarkerType type) const
{
    QVector<TimelineMarker> result;
    for (const auto &m : m_markers)
        if (m.type == type)
            result.append(m);
    return result;
}

int MarkerManager::nextMarker(double fromTime) const
{
    for (int i = 0; i < m_markers.size(); ++i)
        if (m_markers[i].time > fromTime)
            return i;
    return -1;
}

int MarkerManager::prevMarker(double fromTime) const
{
    for (int i = m_markers.size() - 1; i >= 0; --i)
        if (m_markers[i].time < fromTime)
            return i;
    return -1;
}

// --- Chapter generation ---

QVector<Chapter> MarkerManager::generateChapters() const
{
    // Collect chapter-type markers (already sorted by time)
    QVector<TimelineMarker> chapterMarkers = markersByType(MarkerType::Chapter);
    if (chapterMarkers.isEmpty())
        return {};

    QVector<Chapter> chapters;
    for (int i = 0; i < chapterMarkers.size(); ++i) {
        Chapter ch;
        ch.startTime = chapterMarkers[i].time;
        ch.title     = chapterMarkers[i].name;
        ch.index     = i;

        // End time = start of next chapter, or 0 for the last one (caller must set)
        if (i + 1 < chapterMarkers.size())
            ch.endTime = chapterMarkers[i + 1].time;
        else
            ch.endTime = 0.0;

        chapters.append(ch);
    }
    return chapters;
}

// Format helper: seconds -> HH:MM:SS
static QString formatTimestamp(double seconds)
{
    int total = static_cast<int>(std::floor(seconds));
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;

    if (h > 0)
        return QString("%1:%2:%3")
            .arg(h, 2, 10, QChar('0'))
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    return QString("%1:%2")
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'));
}

QString MarkerManager::exportYouTubeChapters() const
{
    QVector<TimelineMarker> chapterMarkers = markersByType(MarkerType::Chapter);
    if (chapterMarkers.isEmpty())
        return {};

    // YouTube requires first chapter at 00:00
    bool hasZero = (std::abs(chapterMarkers.first().time) < 0.001);

    QStringList lines;
    if (!hasZero)
        lines.append(QStringLiteral("00:00 Intro"));

    for (const auto &m : chapterMarkers)
        lines.append(formatTimestamp(m.time) + QChar(' ') + m.name);

    return lines.join(QChar('\n'));
}

QString MarkerManager::exportChapterMetadata() const
{
    QVector<Chapter> chapters = generateChapters();
    if (chapters.isEmpty())
        return {};

    QStringList lines;
    lines.append(QStringLiteral(";FFMETADATA1"));

    for (const auto &ch : chapters) {
        int startMs = static_cast<int>(std::round(ch.startTime * 1000.0));
        int endMs   = (ch.endTime > 0.0)
                          ? static_cast<int>(std::round(ch.endTime * 1000.0))
                          : startMs; // caller should set last end time

        lines.append(QStringLiteral("[CHAPTER]"));
        lines.append(QStringLiteral("TIMEBASE=1/1000"));
        lines.append(QString("START=%1").arg(startMs));
        lines.append(QString("END=%1").arg(endMs));
        lines.append(QString("title=%1").arg(ch.title));
    }
    return lines.join(QChar('\n'));
}

// --- SRT import ---

// Parse SRT timestamp "HH:MM:SS,mmm" -> seconds
static double parseSrtTime(const QString &s)
{
    // Format: HH:MM:SS,mmm
    static QRegularExpression re(QStringLiteral(R"((\d+):(\d+):(\d+)[,.](\d+))"));
    auto match = re.match(s);
    if (!match.hasMatch())
        return 0.0;

    double h   = match.captured(1).toDouble();
    double m   = match.captured(2).toDouble();
    double sec = match.captured(3).toDouble();
    double ms  = match.captured(4).toDouble();
    return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
}

int MarkerManager::importFromSRT(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;

    QTextStream in(&file);
    int imported = 0;

    // SRT format: index\ntimestamp --> timestamp\ntext\n\n
    static QRegularExpression timeRe(
        QStringLiteral(R"((\d+:\d+:\d+[,.]\d+)\s*-->\s*(\d+:\d+:\d+[,.]\d+))"));

    QString line;
    while (!in.atEnd()) {
        line = in.readLine().trimmed();

        auto match = timeRe.match(line);
        if (!match.hasMatch())
            continue;

        double startTime = parseSrtTime(match.captured(1));

        // Read subtitle text (may be multi-line)
        QStringList textLines;
        while (!in.atEnd()) {
            line = in.readLine().trimmed();
            if (line.isEmpty())
                break;
            textLines.append(line);
        }

        QString text = textLines.join(QChar(' '));
        if (!text.isEmpty()) {
            addMarker(startTime, text, MarkerType::Note, MarkerColor::Yellow);
            ++imported;
        }
    }

    return imported;
}

// --- Serialisation ---

QJsonArray MarkerManager::toJson() const
{
    QJsonArray arr;
    for (const auto &m : m_markers)
        arr.append(m.toJson());
    return arr;
}

void MarkerManager::fromJson(const QJsonArray &arr)
{
    m_markers.clear();
    for (const auto &val : arr)
        m_markers.append(TimelineMarker::fromJson(val.toObject()));
    sortMarkers();
}
