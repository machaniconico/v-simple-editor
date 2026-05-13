#include "CaptionStyle.h"

namespace caption {

Style defaultStyle()
{
    return Style{};
}

QStringList anchorNames()
{
    return QStringList{
        "上左",   "上中央",   "上右",
        "中左",   "中央",     "中右",
        "下左",   "下中央",   "下右"
    };
}

QString anchorToString(Anchor a)
{
    switch (a) {
    case Anchor::TopLeft:      return "TopLeft";
    case Anchor::TopCenter:    return "TopCenter";
    case Anchor::TopRight:     return "TopRight";
    case Anchor::MiddleLeft:   return "MiddleLeft";
    case Anchor::MiddleCenter: return "MiddleCenter";
    case Anchor::MiddleRight:  return "MiddleRight";
    case Anchor::BottomLeft:   return "BottomLeft";
    case Anchor::BottomCenter: return "BottomCenter";
    case Anchor::BottomRight:  return "BottomRight";
    }
    return "BottomCenter";
}

Anchor anchorFromString(const QString& s)
{
    if (s == "TopLeft")      return Anchor::TopLeft;
    if (s == "TopCenter")    return Anchor::TopCenter;
    if (s == "TopRight")     return Anchor::TopRight;
    if (s == "MiddleLeft")   return Anchor::MiddleLeft;
    if (s == "MiddleCenter") return Anchor::MiddleCenter;
    if (s == "MiddleRight")  return Anchor::MiddleRight;
    if (s == "BottomLeft")   return Anchor::BottomLeft;
    if (s == "BottomCenter") return Anchor::BottomCenter;
    if (s == "BottomRight")  return Anchor::BottomRight;
    return Anchor::BottomCenter;
}

} // namespace caption
