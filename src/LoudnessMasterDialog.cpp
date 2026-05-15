#include "LoudnessMasterDialog.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

LoudnessMasterDialog::LoudnessMasterDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Loudness Master (EBU R128)"));
    setModal(false);

    // ---- file row: line edit + Browse ----
    m_fileEdit = new QLineEdit(this);
    m_fileEdit->setPlaceholderText(tr("Select an audio file to measure..."));

    auto *browseBtn = new QPushButton(tr("Browse..."), this);

    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_fileEdit, 1);
    fileRow->addWidget(browseBtn);

    // ---- preset combo ----
    m_presetCombo = new QComboBox(this);
    m_presetCombo->addItem(tr("YouTube (-14 LUFS)"),
                           static_cast<int>(loudness::LoudnessPreset::YouTube));
    m_presetCombo->addItem(tr("Spotify (-14 LUFS)"),
                           static_cast<int>(loudness::LoudnessPreset::Spotify));
    m_presetCombo->addItem(tr("Apple Music (-16 LUFS)"),
                           static_cast<int>(loudness::LoudnessPreset::AppleMusic));
    m_presetCombo->addItem(tr("Broadcast (-23 LUFS)"),
                           static_cast<int>(loudness::LoudnessPreset::Broadcast));
    m_presetCombo->addItem(tr("TikTok (-14 LUFS)"),
                           static_cast<int>(loudness::LoudnessPreset::TikTok));

    // ---- measure button ----
    m_measureBtn = new QPushButton(tr("Measure"), this);

    // ---- result labels ----
    m_measuredLabel = new QLabel(tr("Measured: --- LUFS"), this);
    m_gainLabel     = new QLabel(tr("Recommended gain: --- dB"), this);

    // ---- layout ----
    auto *form = new QFormLayout;
    form->addRow(tr("Audio file:"), fileRow);
    form->addRow(tr("Target preset:"), m_presetCombo);
    form->addRow(m_measureBtn);
    form->addRow(tr("Integrated loudness:"), m_measuredLabel);
    form->addRow(tr("Normalization gain:"), m_gainLabel);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(form);

    // ---- connections ----
    connect(browseBtn, &QPushButton::clicked,
            this, &LoudnessMasterDialog::onBrowseClicked);
    connect(m_measureBtn, &QPushButton::clicked,
            this, &LoudnessMasterDialog::onMeasureClicked);
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LoudnessMasterDialog::onPresetChanged);
}

void LoudnessMasterDialog::onBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select Audio File"),
        QString(),
        tr("Audio Files (*.wav *.mp3 *.flac *.aac *.m4a *.ogg *.raw *.pcm);;"
           "All Files (*)"));

    if (!path.isEmpty())
        m_fileEdit->setText(path);
}

void LoudnessMasterDialog::onMeasureClicked()
{
    const QString path = m_fileEdit->text().trimmed();
    if (path.isEmpty()) {
        m_measuredLabel->setText(tr("Measured: (no file selected)"));
        m_gainLabel->setText(tr("Recommended gain: --- dB"));
        return;
    }

    m_measuredLufs = loudness::measureIntegratedLufs(path);
    m_hasMeasured  = true;

    m_measuredLabel->setText(
        tr("Measured: %1 LUFS").arg(m_measuredLufs, 0, 'f', 1));

    const auto preset = static_cast<loudness::LoudnessPreset>(
        m_presetCombo->currentData().toInt());
    const double target = loudness::presetTargetLufs(preset);
    const double gain   = loudness::computeGainDb(m_measuredLufs, target);

    m_gainLabel->setText(
        tr("Recommended gain: %1 dB (target %2 LUFS)")
            .arg(gain, 0, 'f', 1)
            .arg(target, 0, 'f', 1));
}

void LoudnessMasterDialog::onPresetChanged(int index)
{
    Q_UNUSED(index);

    if (!m_hasMeasured)
        return;

    // Re-derive the recommended gain for the newly selected preset.
    const auto preset = static_cast<loudness::LoudnessPreset>(
        m_presetCombo->currentData().toInt());
    const double target = loudness::presetTargetLufs(preset);
    const double gain   = loudness::computeGainDb(m_measuredLufs, target);

    m_gainLabel->setText(
        tr("Recommended gain: %1 dB (target %2 LUFS)")
            .arg(gain, 0, 'f', 1)
            .arg(target, 0, 'f', 1));
}
