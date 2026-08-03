#pragma once

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>

#include "ObjectRemoval.h"

namespace objremoval {

struct ObjectRemovalDialogContext {
    QString clipLabel;
    QString sourcePath;
    int frameCount = 0;
    int defaultFrame = 0;
    double fps = 30.0;
    bool clipMaskAvailable = false;
    bool rotoMaskAvailable = false;
    bool maskTrackingAvailable = false;
    bool backgroundAlignmentAvailable = false;

    std::function<QImage(int)> frameFetcher;
    std::function<QImage(int, const QSize &)> clipMaskFetcher;
    std::function<QImage(int, const QSize &)> rotoMaskFetcher;
    // Used only to follow the mask shape, not to align temporal background
    // samples.
    std::function<QPointF(int)> maskTrackingOffsetFetcher;
    // Optional background/plane tracking displacement for temporal sampling.
    // It is intentionally separate from maskTrackingOffsetFetcher.
    std::function<QPointF(int)> backgroundAlignmentOffsetFetcher;
    // paths are the written PNG sequence in ascending frame order.
    std::function<bool(const QStringList &, double, QString *)> sequenceImporter;
};

} // namespace objremoval

class QLabel;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;

class ObjectRemovalDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ObjectRemovalDialog(QWidget *parent = nullptr);

    void setContext(const objremoval::ObjectRemovalDialogContext &context);
    objremoval::ObjectRemovalParams params() const;

private slots:
    void onPreviewClicked();
    void onApplyClicked();
    void onCancelClicked();

private:
    QImage fetchFrame(int frameIndex);
    QImage fetchMask(int frameIndex, const QSize &size) const;
    QImage processFrame(int frameIndex);
    void setBusy(bool busy);
    void showImage(QLabel *label, const QImage &image,
                   const QString &emptyText) const;
    QString outputDirectory(QString *error) const;

    objremoval::ObjectRemovalDialogContext m_context;
    QHash<int, QImage> m_frameCache;

    QLabel *m_clipValue = nullptr;
    QSpinBox *m_startSpin = nullptr;
    QSpinBox *m_endSpin = nullptr;
    QSpinBox *m_previewFrameSpin = nullptr;
    QComboBox *m_maskCombo = nullptr;
    QSpinBox *m_temporalRadiusSpin = nullptr;
    QSpinBox *m_temporalStrideSpin = nullptr;
    QCheckBox *m_trackingCheck = nullptr;
    QSpinBox *m_dilateSpin = nullptr;
    QSpinBox *m_featherSpin = nullptr;
    QDoubleSpinBox *m_trustSpin = nullptr;
    QSpinBox *m_spatialRadiusSpin = nullptr;

    QLabel *m_beforeView = nullptr;
    QLabel *m_afterView = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_previewButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    bool m_busy = false;
    bool m_cancelRequested = false;
};
