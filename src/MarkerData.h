#pragma once

#include <QString>
#include <QColor>
#include <QJsonObject>
#include <QVector>
#include <QtGlobal>

struct Marker {
    int id = 0;
    qint64 timelineUs = 0;  // position in microseconds
    QString label;
    QColor color{255, 80, 80};
};

inline QJsonObject markerToJsonObject(const Marker &m)
{
    QJsonObject obj;
    obj["id"] = m.id;
    obj["timelineUs"] = QString::number(m.timelineUs);
    obj["label"] = m.label;
    obj["color"] = m.color.name();
    return obj;
}

inline Marker markerFromJsonObject(const QJsonObject &obj)
{
    Marker m;
    m.id = obj["id"].toInt(-1);
    m.timelineUs = obj["timelineUs"].toString(QStringLiteral("0")).toLongLong();
    m.label = obj["label"].toString();
    m.color = QColor(obj["color"].toString(QStringLiteral("#ff5050")));
    return m;
}
