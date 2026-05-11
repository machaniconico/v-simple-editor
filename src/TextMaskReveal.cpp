#include "TextMaskReveal.h"
#include "TextAnimator.h"
#include "MaskSystem.h"
#include <algorithm>

TextMaskReveal::TextMaskReveal() = default;

void TextMaskReveal::setSourceText(TextAnimator* source)
{
    m_sourceText = source;
}

void TextMaskReveal::setSourcePathText(PathText* source)
{
    m_sourcePathText = source;
}

void TextMaskReveal::setMask(MaskSystem* mask)
{
    m_maskSystem = mask;
}

void TextMaskReveal::setMaskInvert(bool invert)
{
    m_maskInvert = invert;
}

void TextMaskReveal::setMaskFeatherPx(double feather)
{
    m_maskFeatherPx = qMax(0.0, feather);
}

void TextMaskReveal::setMaskExpansionPx(double expansion)
{
    m_maskExpansionPx = expansion;
}

QImage TextMaskReveal::renderFrame(const QSize& canvasSize, double time) const
{
    QImage rawImage(canvasSize, QImage::Format_ARGB32_Premultiplied);
    rawImage.fill(Qt::transparent);

    if (m_sourceText) {
        rawImage = m_sourceText->renderFrame(canvasSize, time);
    } else if (m_sourcePathText) {
    }

    if (!m_maskSystem) {
        return rawImage;
    }

    QImage maskImage = MaskSystem::generateMaskImage(m_maskSystem->masks(), canvasSize);

    if (m_maskExpansionPx != 0.0) {
        double blurAmt = std::abs(m_maskExpansionPx);
        maskImage = MaskSystem::featherMask(maskImage, blurAmt);
        int threshold = m_maskExpansionPx > 0.0 ? 64 : 192;
        for (int y = 0; y < maskImage.height(); ++y) {
            uchar* row = maskImage.scanLine(y);
            for (int x = 0; x < maskImage.width(); ++x) {
                row[x] = row[x] >= threshold ? 255 : 0;
            }
        }
    }

    if (m_maskFeatherPx > 0.0) {
        maskImage = MaskSystem::featherMask(maskImage, m_maskFeatherPx);
    }

    if (m_maskInvert) {
        for (int y = 0; y < maskImage.height(); ++y) {
            uchar* row = maskImage.scanLine(y);
            for (int x = 0; x < maskImage.width(); ++x) {
                row[x] = 255 - row[x];
            }
        }
    }

    return MaskSystem::applyMask(rawImage, maskImage);
}

QJsonObject TextMaskReveal::toJson() const
{
    QJsonObject obj;
    obj["maskInvert"] = m_maskInvert;
    obj["maskFeatherPx"] = m_maskFeatherPx;
    obj["maskExpansionPx"] = m_maskExpansionPx;
    return obj;
}

void TextMaskReveal::fromJson(const QJsonObject& obj)
{
    if (obj.contains("maskInvert")) {
        m_maskInvert = obj["maskInvert"].toBool(false);
    }
    if (obj.contains("maskFeatherPx")) {
        m_maskFeatherPx = obj["maskFeatherPx"].toDouble(0.0);
    }
    if (obj.contains("maskExpansionPx")) {
        m_maskExpansionPx = obj["maskExpansionPx"].toDouble(0.0);
    }
}
