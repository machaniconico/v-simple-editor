#pragma once

#include <QDialog>
#include <QImage>
#include <QString>

#include "ColorMatchAnalyzer.h"
#include "ColorMatchLutGenerator.h"

class QPushButton;
class QLabel;
class QComboBox;

class ColorMatchDialog : public QDialog {
    Q_OBJECT
public:
    explicit ColorMatchDialog(QWidget *parent = nullptr);
    void setProjectFilePath(const QString &path);

signals:
    void lutGenerated(const QString &path);
    void lutAppliedToSelectedClip(const QString &path);

private slots:
    void onSelectReference();
    void onSelectTarget();
    void onGenerate();
    void onApplyToSelectedClip();

private:
    static QImage applyLutToImage(const QImage &img, const colormatch::lut::Lut3D &lut);
    bool generateCurrentLut(colormatch::lut::Lut3D *lut) const;
    QString projectAdjacentLutDirectory() const;
    QString automaticLutPath() const;
    void updatePreview();
    void updateGenerateButton();

    QPushButton *m_btnReference = nullptr;
    QPushButton *m_btnTarget    = nullptr;
    QLabel      *m_lblRefThumb  = nullptr;
    QLabel      *m_lblTgtThumb  = nullptr;
    QLabel      *m_lblBefore    = nullptr;
    QLabel      *m_lblAfter     = nullptr;
    QComboBox   *m_cbLutSize    = nullptr;
    QPushButton *m_btnGenerate  = nullptr;
    QPushButton *m_btnApply     = nullptr;

    QImage m_refImage;
    QImage m_tgtImage;
    QString m_projectFilePath;
};
