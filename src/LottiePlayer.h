#pragma once

#include <QColor>
#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

class QLabel;
class QLineEdit;
class QSlider;
class Timeline;

namespace lottie {

struct Keyframe {
    double frame = 0.0;
    QVector<double> startVal;
    QVector<double> endVal;
    QPointF outTangent = QPointF(0.33, 0.33);
    QPointF inTangent = QPointF(0.67, 0.67);
    bool hold = false;
};

struct AnimatedProp {
    bool animated = false;
    QVector<double> staticVal;
    QVector<Keyframe> keyframes;

    QVector<double> evaluate(double frame) const;
    double evaluateScalar(double frame, double fallback = 0.0) const;
};

struct BezierData {
    QVector<QPointF> vertices;
    QVector<QPointF> inTangents;
    QVector<QPointF> outTangents;
    bool closed = false;
};

struct ShapeItem {
    enum class Kind {
        Group,
        Rect,
        Ellipse,
        Polystar,
        Path,
        Fill,
        Stroke,
        Transform
    };
    Kind kind = Kind::Group;
    QString name;

    // Rect / Ellipse
    AnimatedProp position;
    AnimatedProp size;
    AnimatedProp roundness;

    // Polystar
    int starType = 1; // 1 = star, 2 = polygon
    AnimatedProp points;
    AnimatedProp outerRadius;
    AnimatedProp innerRadius;
    AnimatedProp rotation;

    // Path
    BezierData path;

    // Fill / Stroke
    AnimatedProp color; // RGB in [0..1]
    AnimatedProp opacity; // [0..100]
    AnimatedProp strokeWidth;
    int lineCap = 2; // 1=butt, 2=round, 3=square

    // Transform
    AnimatedProp anchor;
    AnimatedProp scale; // percentage [100, 100]

    // Group children
    QVector<ShapeItem> children;
};

struct Layer {
    int index = 0;
    int parentIndex = -1;
    int type = 4; // 1=Solid, 3=Null, 4=Shape
    QString name;
    double inFrame = 0.0;
    double outFrame = 60.0;

    // Solid layer fields
    QColor solidColor = Qt::transparent;
    double solidWidth = 0.0;
    double solidHeight = 0.0;

    // Layer transform
    AnimatedProp anchor;
    AnimatedProp position;
    AnimatedProp scale;
    AnimatedProp rotation;
    AnimatedProp opacity;

    // Shape layer items
    QVector<ShapeItem> shapes;
};

struct LottieDocument {
    QString name;
    QString sourcePath;
    double fps = 30.0;
    double inFrame = 0.0;
    double outFrame = 60.0;
    double width = 512.0;
    double height = 512.0;
    QVector<Layer> layers;
    bool valid = false;
    QString error;

    double durationSec() const {
        return (fps > 0.0 && outFrame > inFrame) ? (outFrame - inFrame) / fps : 0.0;
    }
};

// Parse Bodymovin/Lottie v5 JSON from memory or disk.
LottieDocument parseLottieJson(const QByteArray &jsonBytes, const QString &sourceLabel = {});
LottieDocument loadLottieFile(const QString &filePath);

// Render a parsed Lottie document at `timeSec` onto a transparent RGBA8888 QImage.
QImage renderLottieFrame(const LottieDocument &doc, double timeSec, QSize outSize);

// SSOT helper called by ExternalGenerator for `renderer=lottie`.
QImage renderLottieFileAtTime(const QString &filePath, double timeSec, QSize outSize);
QString buildLottieGeneratorUri(const QString &filePath, double fps = 30.0);

// Insert a Lottie `.json` clip onto the timeline video track in 1 Undo step.
bool insertLottieClip(Timeline *timeline,
                      const QString &filePath,
                      int videoTrackIdx = 0,
                      QString *errorOut = nullptr);

// Clear internal parsed document cache (useful for selftests).
void clearDocumentCache();

class LottieImportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LottieImportDialog(Timeline *timeline,
                                QWidget *parent = nullptr,
                                const QString &initialFilePath = {});

    QString selectedFilePath() const;
    bool isLoaded() const { return m_doc.valid; }

private:
    void browseFile();
    void loadPath(const QString &path);
    void updatePreview();
    void onInsertAndAccept();

    Timeline *m_timeline = nullptr;
    LottieDocument m_doc;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_infoLabel = nullptr;
    QLabel *m_previewLabel = nullptr;
    QSlider *m_scrubSlider = nullptr;
};

} // namespace lottie
