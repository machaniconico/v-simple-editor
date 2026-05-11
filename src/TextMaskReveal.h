#pragma once

#include <QObject>
#include <QImage>
#include <QJsonObject>
#include <QSize>

class TextAnimator;
class PathText;
class MaskSystem;

class TextMaskReveal : public QObject
{
    Q_OBJECT

public:
    TextMaskReveal();

    void setSourceText(TextAnimator* source);
    void setSourcePathText(PathText* source);
    void setMask(MaskSystem* mask);

    void setMaskInvert(bool invert);
    void setMaskFeatherPx(double feather);
    void setMaskExpansionPx(double expansion);

    QImage renderFrame(const QSize& canvasSize, double time) const;

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& obj);

private:
    TextAnimator* m_sourceText = nullptr;
    PathText* m_sourcePathText = nullptr;
    MaskSystem* m_maskSystem = nullptr;

    bool m_maskInvert = false;
    double m_maskFeatherPx = 0.0;
    double m_maskExpansionPx = 0.0;
};
