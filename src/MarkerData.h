#pragma once

// Timeline markers/chapters — Premiere Pro / DaVinci Resolve parity feature.
//
// NOTE on naming: The spec for this story uses the name `TimelineMarker` for
// this struct, but the existing `src/TimelineMarker.h` already defines a
// different `TimelineMarker` (used by `MarkerManager` + `SnapEngine`). To
// avoid breaking the legacy snap-target system without expanding the file
// scope of this story, the new spec-conformant struct is named `Marker` here.
// All other field names match the spec verbatim.

#include <QString>
#include <QColor>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QtGlobal>

struct Marker {
    int id = -1;            // unique, monotonic, persists across save/load
    qint64 timelineUs = 0;  // microseconds on timeline
    QString label;          // short text shown in tooltip
    QColor color = QColor(QStringLiteral("#ff5050"));
    QString note;           // longer note (optional, multi-line)
};

// JSON helpers — header-only/inline so they can be reused from Timeline,
// ProjectFile, or any future panel without adding a translation unit. The
// color is round-tripped as `#RRGGBB` per spec acceptance #5.
inline QJsonObject markerToJsonObject(const Marker &m)
{
    QJsonObject obj;
    obj[QStringLiteral("id")]     = m.id;
    obj[QStringLiteral("timeUs")] = static_cast<double>(m.timelineUs);
    obj[QStringLiteral("label")]  = m.label;
    obj[QStringLiteral("color")]  = m.color.isValid()
        ? m.color.name(QColor::HexRgb)
        : QStringLiteral("#ff5050");
    obj[QStringLiteral("note")]   = m.note;
    return obj;
}

inline Marker markerFromJsonObject(const QJsonObject &obj)
{
    Marker m;
    m.id = obj.value(QStringLiteral("id")).toInt(-1);
    // qint64 round-trips through double via QJsonValue.toDouble; precision
    // is exact up to 2^53 us (~ 285 years), well above any timeline range.
    m.timelineUs = static_cast<qint64>(
        obj.value(QStringLiteral("timeUs")).toDouble(0.0));
    m.label = obj.value(QStringLiteral("label")).toString();
    const QString colStr = obj.value(QStringLiteral("color"))
        .toString(QStringLiteral("#ff5050"));
    QColor c(colStr);
    m.color = c.isValid() ? c : QColor(QStringLiteral("#ff5050"));
    m.note = obj.value(QStringLiteral("note")).toString();
    return m;
}

inline QJsonArray markersToJsonArray(const QVector<Marker> &markers)
{
    QJsonArray arr;
    for (const auto &m : markers)
        arr.append(markerToJsonObject(m));
    return arr;
}

inline QVector<Marker> markersFromJsonArray(const QJsonArray &arr)
{
    QVector<Marker> result;
    result.reserve(arr.size());
    for (const auto &v : arr)
        result.append(markerFromJsonObject(v.toObject()));
    return result;
}
