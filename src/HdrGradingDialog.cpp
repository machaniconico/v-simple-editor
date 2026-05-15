#include "HdrGradingDialog.h"
#include "HdrGrading.h"

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

namespace {
constexpr int kPreviewPx = 320;
}

HdrGradingDialog::HdrGradingDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("HDR Grading / Tone Map"));
    setModal(false);

    // --- Preview row (before / after) ---
    m_beforeView = new QLabel(tr("(no image)"), this);
    m_afterView  = new QLabel(tr("(not graded)"), this);

    for (QLabel *lbl : { m_beforeView, m_afterView }) {
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setMinimumSize(kPreviewPx, kPreviewPx);
        lbl->setFrameShape(QFrame::Box);
    }

    auto *previewRow = new QHBoxLayout;
    previewRow->addWidget(m_beforeView);
    previewRow->addWidget(m_afterView);

    // --- Controls row ---
    m_tfCombo = new QComboBox(this);
    m_tfCombo->addItem(tr("SDR (Rec.709)"),
                       static_cast<int>(hdr::TransferFunction::SDR_Rec709));
    m_tfCombo->addItem(tr("PQ (Rec.2100 / HDR10)"),
                       static_cast<int>(hdr::TransferFunction::PQ_Rec2100));
    m_tfCombo->addItem(tr("HLG (Rec.2100)"),
                       static_cast<int>(hdr::TransferFunction::HLG_Rec2100));

    m_opCombo = new QComboBox(this);
    m_opCombo->addItem(tr("Reinhard"),
                       static_cast<int>(hdr::ToneMapOperator::Reinhard));
    m_opCombo->addItem(tr("ACES Filmic"),
                       static_cast<int>(hdr::ToneMapOperator::AcesFilmic));
    m_opCombo->setCurrentIndex(1); // default ACES filmic

    auto *browseBtn = new QPushButton(tr("Browse..."), this);
    auto *applyBtn  = new QPushButton(tr("Apply"), this);

    auto *controlRow = new QHBoxLayout;
    controlRow->addWidget(new QLabel(tr("Transfer:"), this));
    controlRow->addWidget(m_tfCombo);
    controlRow->addWidget(new QLabel(tr("Operator:"), this));
    controlRow->addWidget(m_opCombo);
    controlRow->addStretch(1);
    controlRow->addWidget(browseBtn);
    controlRow->addWidget(applyBtn);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(previewRow);
    mainLayout->addLayout(controlRow);

    connect(browseBtn, &QPushButton::clicked,
            this, &HdrGradingDialog::onBrowseClicked);
    connect(applyBtn, &QPushButton::clicked,
            this, &HdrGradingDialog::onApplyClicked);
}

void HdrGradingDialog::onBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open HDR / image file"),
        QString(),
        tr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.exr *.hdr *.bmp);;All files (*)"));

    if (path.isEmpty())
        return;

    QImage img(path);
    if (img.isNull()) {
        m_beforeView->setText(tr("(failed to load)"));
        return;
    }

    m_source = img;
    m_beforeView->setPixmap(
        QPixmap::fromImage(img).scaled(kPreviewPx, kPreviewPx,
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
    m_afterView->setText(tr("(not graded)"));
}

void HdrGradingDialog::onApplyClicked()
{
    if (m_source.isNull()) {
        m_afterView->setText(tr("(no source image)"));
        return;
    }

    const auto tf = static_cast<hdr::TransferFunction>(
        m_tfCombo->currentData().toInt());
    const auto op = static_cast<hdr::ToneMapOperator>(
        m_opCombo->currentData().toInt());

    const QImage result = hdr::toneMapHdrToSdr(m_source, tf, op);
    if (result.isNull()) {
        m_afterView->setText(tr("(grading failed)"));
        return;
    }

    m_afterView->setPixmap(
        QPixmap::fromImage(result).scaled(kPreviewPx, kPreviewPx,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
}
