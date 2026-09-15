// src/SourceMonitorDock.cpp
// SM-4: SourceMonitorDock の実装。VideoPlayer を 1 個 new して中央に置き、
// スクラブ・マークイン/アウト・挿入/上書きの UI を組む。

#include "SourceMonitorDock.h"

#include "VideoPlayer.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <limits>

namespace {

// durationSec 不明 (<=0) のときに張る暫定スクラブレンジ (1 時間)。実尺が
// durationChanged で判明したら applyDurationToSlider() で張り直す。
constexpr int kFallbackDurationMs = 60 * 60 * 1000;

double boundedPositionSeconds(double seconds, double durationSeconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return 0.0;
    if (std::isfinite(durationSeconds) && durationSeconds > 0.0)
        seconds = qMin(seconds, durationSeconds);
    const double largestSliderSecond =
        static_cast<double>(std::numeric_limits<int>::max()) / 1000.0;
    return qMin(seconds, largestSliderSecond);
}

// 秒を MM:SS.mmm 形式に整形する。負値は 0 として扱う。
QString formatSeconds(double sec)
{
    if (sec < 0.0 || std::isnan(sec))
        sec = 0.0;
    const int totalMs = static_cast<int>(std::llround(sec * 1000.0));
    const int minutes = totalMs / 60000;
    const int seconds = (totalMs / 1000) % 60;
    const int millis = totalMs % 1000;
    return QString("%1:%2.%3")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(millis, 3, 10, QChar('0'));
}

} // namespace

SourceMonitorDock::SourceMonitorDock(QWidget *parent)
    : QDockWidget(tr("ソースモニター"), parent)
{
    setObjectName("SourceMonitorDock");
    setupUI();
    updateControls();
}

void SourceMonitorDock::setupUI()
{
    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 2 個目の VideoPlayer。singleton ではないので生成可。MainWindow 固有
    // シグナルには接続せず、loadFile/seek/previewSeek のみ使う。
    m_viewer = new VideoPlayer(central);
    outer->addWidget(m_viewer, /*stretch=*/1);

    // 素材尺が判明したらスクラブレンジを補正する。
    connect(m_viewer, &VideoPlayer::durationChanged,
            this, &SourceMonitorDock::onPlayerDurationChanged);

    // スクラブバー (0..durationMs)。
    m_scrubBar = new QSlider(Qt::Horizontal, central);
    m_scrubBar->setMinimum(0);
    m_scrubBar->setMaximum(kFallbackDurationMs);
    m_scrubBar->setValue(0);
    connect(m_scrubBar, &QSlider::valueChanged,
            this, &SourceMonitorDock::onScrub);
    outer->addWidget(m_scrubBar);

    // 位置 / イン / アウト ラベル行。
    auto *labelRow = new QHBoxLayout();
    labelRow->setSpacing(8);
    m_posLabel = new QLabel(tr("位置 --:--.---"), central);
    m_inLabel = new QLabel(tr("イン --:--.---"), central);
    m_outLabel = new QLabel(tr("アウト --:--.---"), central);
    labelRow->addWidget(m_posLabel);
    labelRow->addStretch(1);
    labelRow->addWidget(m_inLabel);
    labelRow->addWidget(m_outLabel);
    outer->addLayout(labelRow);

    // マークイン / マークアウト ボタン行。
    auto *markRow = new QHBoxLayout();
    markRow->setSpacing(4);
    m_markInBtn = new QPushButton(tr("マークイン"), central);
    m_markOutBtn = new QPushButton(tr("マークアウト"), central);
    connect(m_markInBtn, &QPushButton::clicked,
            this, &SourceMonitorDock::onMarkIn);
    connect(m_markOutBtn, &QPushButton::clicked,
            this, &SourceMonitorDock::onMarkOut);
    markRow->addWidget(m_markInBtn);
    markRow->addWidget(m_markOutBtn);
    markRow->addStretch(1);
    outer->addLayout(markRow);

    // 挿入 / 上書き ボタン行。
    auto *editRow = new QHBoxLayout();
    editRow->setSpacing(4);
    m_insertBtn = new QPushButton(tr("挿入 (Insert)"), central);
    m_overwriteBtn = new QPushButton(tr("上書き (Overwrite)"), central);
    connect(m_insertBtn, &QPushButton::clicked,
            this, &SourceMonitorDock::onInsertClicked);
    connect(m_overwriteBtn, &QPushButton::clicked,
            this, &SourceMonitorDock::onOverwriteClicked);
    editRow->addWidget(m_insertBtn);
    editRow->addWidget(m_overwriteBtn);
    editRow->addStretch(1);
    outer->addLayout(editRow);

    setWidget(central);
}

void SourceMonitorDock::loadSource(const threepoint::SourceSelection &sel,
                                   double initialPositionSec)
{
    loadSource(sel.filePath, sel.durationSec, sel.displayName,
               initialPositionSec);
    // SourceSelection 側に既存マークがあれば引き継ぐ。
    if (m_loaded) {
        m_sourceInSec = boundedPositionSeconds(sel.sourceInSec, m_durationSec);
        m_sourceOutSec = boundedPositionSeconds(sel.sourceOutSec, m_durationSec);
        if (m_sourceOutSec > 0.0 && m_sourceOutSec <= m_sourceInSec)
            m_sourceOutSec = 0.0;
        updateControls();
    }
}

void SourceMonitorDock::loadSource(const QString &filePath, double durationSec,
                                   const QString &displayName,
                                   double initialPositionSec)
{
    if (filePath.isEmpty())
        return;

    m_filePath = filePath;
    m_displayName = displayName.isEmpty() ? filePath : displayName;
    m_durationSec = (std::isfinite(durationSec) && durationSec > 0.0)
        ? durationSec : 0.0;
    m_scrubSec = boundedPositionSeconds(initialPositionSec, m_durationSec);
    m_sourceInSec = 0.0;
    m_sourceOutSec = 0.0;
    m_loaded = true;

    if (m_viewer)
        m_viewer->loadFile(filePath);

    applyDurationToSlider();

    // 指定位置へ移動する。マッチフレームでは loadFile() 直後に同じ
    // VideoPlayer へ seek し、通常のメディアプール起動は既定値 0 秒のまま。
    const int initialPositionMs = static_cast<int>(
        std::llround(m_scrubSec * 1000.0));
    if (m_scrubBar) {
        const QSignalBlocker blocker(m_scrubBar);
        m_scrubBar->setValue(initialPositionMs);
    }
    if (m_viewer)
        m_viewer->seek(initialPositionMs);

    updateControls();
}

void SourceMonitorDock::applyDurationToSlider()
{
    if (!m_scrubBar)
        return;
    const double boundedDurationSec = boundedPositionSeconds(m_durationSec, 0.0);
    const int maxMs = (boundedDurationSec > 0.0)
        ? static_cast<int>(std::llround(boundedDurationSec * 1000.0))
        : kFallbackDurationMs;
    const QSignalBlocker blocker(m_scrubBar);
    m_scrubBar->setMaximum(maxMs > 0 ? maxMs : kFallbackDurationMs);
}

void SourceMonitorDock::onPlayerDurationChanged(double durationSeconds)
{
    // 起動時に durationSec=0 で読み込まれた素材の実尺が判明したケース。
    // 既に明示尺を持っているときは上書きしない。
    if (!m_loaded || !std::isfinite(durationSeconds) || durationSeconds <= 0.0)
        return;
    if (m_durationSec > 0.0)
        return;
    m_durationSec = durationSeconds;
    m_scrubSec = boundedPositionSeconds(m_scrubSec, m_durationSec);
    applyDurationToSlider();
    const int positionMs = static_cast<int>(std::llround(m_scrubSec * 1000.0));
    if (m_scrubBar) {
        const QSignalBlocker blocker(m_scrubBar);
        m_scrubBar->setValue(positionMs);
    }
    if (m_viewer)
        m_viewer->seek(positionMs);
    updateControls();
}

void SourceMonitorDock::onScrub(int positionMs)
{
    if (!m_loaded)
        return;
    m_scrubSec = positionMs / 1000.0;
    if (m_viewer)
        m_viewer->previewSeek(positionMs);
    updateControls();
}

void SourceMonitorDock::onMarkIn()
{
    if (!m_loaded)
        return;
    m_sourceInSec = m_scrubSec;
    // in が現 out 以上になったら out を無効化 (未マーク状態へ戻す)。
    if (m_sourceOutSec > 0.0 && m_sourceInSec >= m_sourceOutSec)
        m_sourceOutSec = 0.0;
    updateControls();
}

void SourceMonitorDock::onMarkOut()
{
    if (!m_loaded)
        return;
    // out は in より後でなければ無効。in 以下なら記録しない。
    if (m_scrubSec <= m_sourceInSec)
        return;
    m_sourceOutSec = m_scrubSec;
    updateControls();
}

threepoint::SourceSelection SourceMonitorDock::currentSelection() const
{
    threepoint::SourceSelection sel;
    sel.filePath = m_filePath;
    sel.displayName = m_displayName;
    sel.durationSec = m_durationSec;
    sel.sourceInSec = m_sourceInSec;
    sel.sourceOutSec = m_sourceOutSec; // <=0 は durationSec を終端とみなす
    return sel;
}

void SourceMonitorDock::onInsertClicked()
{
    if (!m_loaded)
        return;
    emit insertRequested(currentSelection());
}

void SourceMonitorDock::onOverwriteClicked()
{
    if (!m_loaded)
        return;
    emit overwriteRequested(currentSelection());
}

void SourceMonitorDock::updateControls()
{
    const bool enabled = m_loaded && !m_filePath.isEmpty();

    if (m_markInBtn)
        m_markInBtn->setEnabled(enabled);
    if (m_markOutBtn)
        m_markOutBtn->setEnabled(enabled);
    // 挿入/上書きは validate() が通る長さと選択範囲 (in<effectiveOut) が
    // そろったときだけ有効化する。
    bool selectionOk = false;
    if (enabled) {
        const double effOut = (m_sourceOutSec > 0.0)
            ? m_sourceOutSec
            : m_durationSec;
        selectionOk = (m_durationSec > 0.0) && (m_sourceInSec < effOut);
    }
    if (m_insertBtn)
        m_insertBtn->setEnabled(enabled && selectionOk);
    if (m_overwriteBtn)
        m_overwriteBtn->setEnabled(enabled && selectionOk);

    if (m_posLabel) {
        m_posLabel->setText(enabled
            ? tr("位置 %1").arg(formatSeconds(m_scrubSec))
            : tr("位置 --:--.---"));
    }
    if (m_inLabel) {
        m_inLabel->setText(enabled
            ? tr("イン %1").arg(formatSeconds(m_sourceInSec))
            : tr("イン --:--.---"));
    }
    if (m_outLabel) {
        const bool hasOut = enabled && m_sourceOutSec > 0.0;
        m_outLabel->setText(hasOut
            ? tr("アウト %1").arg(formatSeconds(m_sourceOutSec))
            : tr("アウト --:--.---"));
    }
}
