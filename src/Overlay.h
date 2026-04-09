#pragma once

#include <QString>
#include <QColor>
#include <QFont>
#include <QRectF>
#include <QImage>
#include <QVector>

// --- Text Overlay (Telop) ---

struct TextOverlay {
    QString text;
    QFont font = QFont("Arial", 32, QFont::Bold);
    QColor color = Qt::white;
    QColor backgroundColor = QColor(0, 0, 0, 160);
    QColor outlineColor = Qt::black;
    int outlineWidth = 2;
    double x = 0.5;      // 0.0-1.0 normalized position
    double y = 0.85;     // 0.0-1.0 normalized position
    double startTime = 0.0;
    double endTime = 0.0; // 0 = until end of clip
    int alignment = Qt::AlignCenter;
    bool visible = true;
};

// --- Transition ---

enum class TransitionType {
    None,
    FadeIn,
    FadeOut,
    CrossDissolve,
    WipeLeft,
    WipeRight,
    WipeUp,
    WipeDown,
    SlideLeft,
    SlideRight
};

struct Transition {
    TransitionType type = TransitionType::None;
    double duration = 0.5; // seconds

    static QString typeName(TransitionType t) {
        switch (t) {
            case TransitionType::None:          return "None";
            case TransitionType::FadeIn:        return "Fade In";
            case TransitionType::FadeOut:        return "Fade Out";
            case TransitionType::CrossDissolve: return "Cross Dissolve";
            case TransitionType::WipeLeft:      return "Wipe Left";
            case TransitionType::WipeRight:     return "Wipe Right";
            case TransitionType::WipeUp:        return "Wipe Up";
            case TransitionType::WipeDown:      return "Wipe Down";
            case TransitionType::SlideLeft:     return "Slide Left";
            case TransitionType::SlideRight:    return "Slide Right";
        }
        return "Unknown";
    }
};

// --- Image Overlay ---

struct ImageOverlay {
    QString filePath;
    QRectF rect = QRectF(0.1, 0.1, 0.3, 0.3); // normalized x,y,w,h
    double startTime = 0.0;
    double endTime = 0.0;
    double opacity = 1.0;
    bool keepAspectRatio = true;
    bool visible = true;
};

// --- Picture in Picture ---

struct PipConfig {
    int sourceClipIndex = -1;
    QRectF rect = QRectF(0.65, 0.05, 0.3, 0.3); // bottom-right corner default
    double opacity = 1.0;
    int borderWidth = 2;
    QColor borderColor = Qt::white;
    double startTime = 0.0;
    double endTime = 0.0;
    bool visible = true;

    enum Position { TopLeft, TopRight, BottomLeft, BottomRight, Custom };
    static QRectF presetRect(Position pos) {
        switch (pos) {
            case TopLeft:     return QRectF(0.05, 0.05, 0.3, 0.3);
            case TopRight:    return QRectF(0.65, 0.05, 0.3, 0.3);
            case BottomLeft:  return QRectF(0.05, 0.65, 0.3, 0.3);
            case BottomRight: return QRectF(0.65, 0.65, 0.3, 0.3);
            default:          return QRectF(0.65, 0.05, 0.3, 0.3);
        }
    }
};

// --- Composite render layer ---

class OverlayRenderer
{
public:
    static void renderTextOverlay(QImage &frame, const TextOverlay &overlay, double currentTime);
    static void renderImageOverlay(QImage &frame, const ImageOverlay &overlay, double currentTime);
    static void renderPip(QImage &frame, const QImage &pipSource, const PipConfig &config);
    static QImage applyTransition(const QImage &from, const QImage &to, TransitionType type, double progress);
};
