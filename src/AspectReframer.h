#pragma once
#include <QImage>
#include <QPointF>
#include <QSize>
#include <QRectF>
#include <QString>
#include <QStringList>

namespace reframe {

enum class Mode {
    CenterCrop,
    LetterBox,
    SmartCenterFollow,
    SkinToneFocus,
    Manual
};

struct ReframeParams {
    QSize   sourceSize;
    QSize   targetSize;
    Mode    mode                    = Mode::CenterCrop;
    QPointF manualOffsetNormalized  = QPointF(0.5, 0.5);
    double  zoom                    = 1.0;
};

struct ReframeResult {
    QRectF  cropRectNormalized;
    QImage  previewImage;
    bool    success = false;
    QString error;
};

QRectF      computeCropRect(const QImage& source, const ReframeParams& params);
ReframeResult applyReframe(const QImage& source, const ReframeParams& params);
QStringList availableModes();
QString     modeDisplayName(Mode m);
Mode        modeFromString(const QString& s);

} // namespace reframe
