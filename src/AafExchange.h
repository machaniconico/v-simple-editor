#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class Timeline;

namespace aaf {

struct AafExportOptions {
    double fps = 30.0;
    bool includeVideo = true;
    bool includeAudio = true;
    bool includeMarkers = true;
    QString projectName = QStringLiteral("Untitled");
};

struct AafSourceClip {
    QString mobId;
    QString displayName;
    QString filePath;
    double inPointSec = 0.0;
    double outPointSec = 0.0;
    double sourceDurationSec = 0.0;
    double leadInGapSec = 0.0;
    double speed = 1.0;
    double volume = 1.0;
    QString roleId;
};

struct AafTrack {
    int slotId = 1;
    bool isAudio = false;
    QString name;
    QVector<AafSourceClip> clips;
};

struct AafMarker {
    double timeSec = 0.0;
    QString label;
    QString colorHex;
};

struct AafImportResult {
    bool ok = false;
    QString error;
    QString projectName;
    double fps = 30.0;
    QVector<AafTrack> videoTracks;
    QVector<AafTrack> audioTracks;
    QVector<AafMarker> markers;
};

// Export Timeline into a spec-compliant Microsoft Structured Storage (OLE2 / CFB v3)
// binary `.aaf` file containing Header, MetaDictionary, and Composition Mob Graph streams.
bool writeAafFile(const Timeline *timeline,
                  const AafExportOptions &options,
                  const QString &outPath,
                  QString *errorOut = nullptr);

// Verify OLE2/CFB v3 magic + FAT + Directory structure and parse AAF Mob Graph from `.aaf`.
AafImportResult readAafFile(const QString &inPath);

// Apply parsed AAF composition onto Timeline in a single Undo step.
bool applyAafToTimeline(Timeline *timeline,
                        const AafImportResult &parsed,
                        QString *errorOut = nullptr);

class AafExchangeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AafExchangeDialog(const AafExportOptions &defaults = {},
                               QWidget *parent = nullptr);

    AafExportOptions options() const;

private:
    QLineEdit *m_nameEdit = nullptr;
    QDoubleSpinBox *m_fpsSpin = nullptr;
    QCheckBox *m_videoCheck = nullptr;
    QCheckBox *m_audioCheck = nullptr;
    QCheckBox *m_markerCheck = nullptr;
};

} // namespace aaf
