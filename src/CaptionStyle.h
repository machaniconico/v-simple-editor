#pragma once
#include <QString>
#include <QColor>
#include <QPointF>
#include <QStringList>

namespace caption {

enum class Anchor {
    TopLeft,    TopCenter,    TopRight,
    MiddleLeft, MiddleCenter, MiddleRight,
    BottomLeft, BottomCenter, BottomRight
};

struct Style {
    QString fontFamily          = "Noto Sans CJK JP";
    int     fontSizePt          = 24;
    bool    bold                = false;
    bool    italic              = false;
    QColor  textColor           = Qt::white;
    QColor  outlineColor        = Qt::black;
    double  outlineThickness    = 2.0;
    QColor  shadowColor         = QColor(0, 0, 0, 128);
    QPointF shadowOffset        = QPointF(2, 2);
    bool    background          = false;
    QColor  backgroundColor     = QColor(0, 0, 0, 128);
    Anchor  anchor              = Anchor::BottomCenter;
    QPointF anchorOffsetNormalized = QPointF(0.0, -0.08); // 下端から 8% 上
    double  maxWidthNormalized  = 0.8;                    // 横幅 80% 以内
};

Style       defaultStyle();
QStringList anchorNames();                           // 9 個の display name
QString     anchorToString(Anchor a);
Anchor      anchorFromString(const QString& s);

} // namespace caption
