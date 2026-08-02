#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace vfxfootage {

// Metadata discovered from one file in the user's VFX footage directory.
// A file is kept in the result when probing fails; durationSeconds == 0 then
// means that placement should make one best-effort probe before inserting it.
struct FootageItem {
    QString filePath;
    QString displayName;
    QString category;
    QStringList tags;
    double durationSeconds = 0.0;
    QSize frameSize;
};

// Directory and file handling for the VFX footage library. This class is
// deliberately independent from EffectLibraryModel so the scan and pixel
// operations remain headless and directly testable.
class VfxFootageLibrary
{
public:
    static QString defaultDirectory();
    static bool isSupportedVideoFile(const QString &filePath);
    static QString inferCategory(const QString &fileName);
    static QStringList inferTags(const QString &fileName,
                                 const QString &category = QString());

    // Missing directories and unreadable files are safe: the former returns
    // an empty list and the latter remains in the result with empty metadata.
    static QVector<FootageItem> scan(const QString &directory);

    static double probeDurationSeconds(const QString &filePath,
                                       QSize *frameSize = nullptr);
    static QImage representativeFrame(const QString &filePath,
                                       double *durationSeconds = nullptr,
                                       QSize *frameSize = nullptr);

    // threshold == 0 is a strict no-op. Otherwise pixels whose luma is <=
    // threshold become RGB=0 while their alpha is preserved.
    static QImage applyBlackLevel(const QImage &source, int threshold);

    // gain == 1 is a strict no-op. RGB channels are multiplied and clamped;
    // alpha is preserved.
    static QImage applyIntensity(const QImage &source, double gain);

    // Shared compositor route used by the VFX selftest and by callers that
    // need a deterministic CPU reference for a footage overlay.
    static QImage screenComposite(const QImage &base, const QImage &overlay,
                                  double opacity = 1.0);
};

} // namespace vfxfootage
