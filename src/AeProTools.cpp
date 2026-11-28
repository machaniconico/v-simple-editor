#include "AeProTools.h"
#include "Timeline.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QSet>
#include <QSettings>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace aepro {

// ============================================================================
// Speed Graph
// ============================================================================

SpeedCurveSummary sampleSpeedCurve(const KeyframeTrack &track,
                                   double startSec,
                                   double endSec,
                                   int sampleCount)
{
    SpeedCurveSummary out;
    out.propertyName = track.propertyName();

    double lo = startSec;
    double hi = endSec;
    if (track.hasKeyframes()) {
        for (const KeyframePoint &kf : track.keyframes())
            out.keyframeTimes.append(kf.time);
        if (hi <= lo) {
            lo = track.keyframes().first().time;
            hi = track.keyframes().last().time;
        }
    }
    if (hi <= lo)
        hi = lo + 1.0;

    out.startSec = lo;
    out.endSec = hi;

    const int n = qBound(4, sampleCount, 512);
    const double span = hi - lo;
    const double dt = qMax(1e-4, span / static_cast<double>(n - 1) * 0.25);

    out.samples.reserve(n);
    double sumSpeed = 0.0;

    for (int i = 0; i < n; ++i) {
        const double frac = static_cast<double>(i) / static_cast<double>(n - 1);
        const double t = lo + span * frac;
        const double v = track.valueAt(t);

        double speed = 0.0;
        if (i == 0) {
            const double vNext = track.valueAt(qMin(hi, t + dt));
            speed = std::abs(vNext - v) / qMax(1e-6, qMin(hi, t + dt) - t);
        } else if (i == n - 1) {
            const double vPrev = track.valueAt(qMax(lo, t - dt));
            speed = std::abs(v - vPrev) / qMax(1e-6, t - qMax(lo, t - dt));
        } else {
            const double t0 = qMax(lo, t - dt);
            const double t1 = qMin(hi, t + dt);
            speed = std::abs(track.valueAt(t1) - track.valueAt(t0)) / qMax(1e-6, t1 - t0);
        }

        SpeedSample s;
        s.timeSec = t;
        s.value = v;
        s.speed = speed;
        out.samples.append(s);

        sumSpeed += speed;
        if (speed > out.maxSpeed) {
            out.maxSpeed = speed;
            out.peakTimeSec = t;
        }
    }

    out.avgSpeed = sumSpeed / static_cast<double>(n);
    return out;
}

SpeedGraphWidget::SpeedGraphWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(460, 210);
}

void SpeedGraphWidget::setSummary(const SpeedCurveSummary &summary)
{
    m_summary = summary;
    update();
}

void SpeedGraphWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF full = rect();
    p.fillRect(full, QColor(24, 26, 32));

    const QRectF plot = full.adjusted(44.0, 18.0, -16.0, -28.0);
    if (plot.width() <= 10.0 || plot.height() <= 10.0)
        return;

    // Grid
    p.setPen(QPen(QColor(52, 56, 68), 1.0));
    for (int i = 0; i <= 4; ++i) {
        const double y = plot.top() + plot.height() * (static_cast<double>(i) / 4.0);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    p.drawRect(plot);

    if (m_summary.samples.size() < 2) {
        p.setPen(QColor(160, 166, 180));
        p.drawText(plot, Qt::AlignCenter, QStringLiteral("キーフレームのあるトラックがありません"));
        return;
    }

    const double span = qMax(1e-6, m_summary.endSec - m_summary.startSec);
    const double yMax = qMax(1e-3, m_summary.maxSpeed * 1.15);

    auto mapPt = [&](double t, double spd) -> QPointF {
        const double nx = qBound(0.0, (t - m_summary.startSec) / span, 1.0);
        const double ny = qBound(0.0, spd / yMax, 1.0);
        return QPointF(plot.left() + nx * plot.width(),
                       plot.bottom() - ny * plot.height());
    };

    // Keyframe vertical guides
    p.setPen(QPen(QColor(255, 200, 70, 120), 1.0, Qt::DashLine));
    for (double kt : m_summary.keyframeTimes) {
        if (kt < m_summary.startSec - 1e-6 || kt > m_summary.endSec + 1e-6)
            continue;
        const double x = mapPt(kt, 0.0).x();
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }

    // Velocity curve + fill
    QPainterPath strokePath;
    QPainterPath fillPath;
    const QPointF firstPt = mapPt(m_summary.samples.first().timeSec, m_summary.samples.first().speed);
    strokePath.moveTo(firstPt);
    fillPath.moveTo(QPointF(firstPt.x(), plot.bottom()));
    fillPath.lineTo(firstPt);

    for (int i = 1; i < m_summary.samples.size(); ++i) {
        const QPointF pt = mapPt(m_summary.samples[i].timeSec, m_summary.samples[i].speed);
        strokePath.lineTo(pt);
        fillPath.lineTo(pt);
    }
    fillPath.lineTo(QPointF(mapPt(m_summary.samples.last().timeSec, 0.0).x(), plot.bottom()));
    fillPath.closeSubpath();

    QLinearGradient grad(plot.topLeft(), plot.bottomLeft());
    grad.setColorAt(0.0, QColor(80, 210, 255, 110));
    grad.setColorAt(1.0, QColor(80, 210, 255, 12));
    p.fillPath(fillPath, grad);

    p.setPen(QPen(QColor(95, 220, 255), 2.2));
    p.drawPath(strokePath);

    // Peak marker
    const QPointF peakPt = mapPt(m_summary.peakTimeSec, m_summary.maxSpeed);
    p.setBrush(QColor(255, 120, 90));
    p.setPen(Qt::NoPen);
    p.drawEllipse(peakPt, 4.0, 4.0);

    // Axis labels
    p.setPen(QColor(180, 186, 200));
    p.drawText(QRectF(4.0, plot.top() - 6.0, 38.0, 16.0), Qt::AlignRight | Qt::AlignVCenter,
               QString::number(yMax, 'f', 1));
    p.drawText(QRectF(4.0, plot.bottom() - 8.0, 38.0, 16.0), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("0.0"));
    p.drawText(QRectF(plot.left(), plot.bottom() + 4.0, 80.0, 18.0), Qt::AlignLeft,
               QStringLiteral("%1s").arg(m_summary.startSec, 0, 'f', 2));
    p.drawText(QRectF(plot.right() - 80.0, plot.bottom() + 4.0, 80.0, 18.0), Qt::AlignRight,
               QStringLiteral("%1s").arg(m_summary.endSec, 0, 'f', 2));
}

SpeedGraphDialog::SpeedGraphDialog(const KeyframeManager &manager,
                                   const QString &clipLabel,
                                   double clipDurationSec,
                                   QWidget *parent)
    : QDialog(parent)
    , m_clipDurationSec(qMax(0.1, clipDurationSec))
{
    setWindowTitle(QStringLiteral("スピードグラフ (Velocity / Speed Graph)"));
    setMinimumWidth(520);

    for (const KeyframeTrack &tr : manager.tracks()) {
        if (tr.hasKeyframes())
            m_tracks.append(tr);
    }

    auto *root = new QVBoxLayout(this);
    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(QStringLiteral("対象: %1").arg(clipLabel), this));
    topRow->addStretch();
    topRow->addWidget(new QLabel(QStringLiteral("トラック:"), this));
    m_trackCombo = new QComboBox(this);
    for (const KeyframeTrack &tr : m_tracks)
        m_trackCombo->addItem(tr.propertyName());
    topRow->addWidget(m_trackCombo);
    root->addLayout(topRow);

    m_graph = new SpeedGraphWidget(this);
    root->addWidget(m_graph, 1);

    m_statsLabel = new QLabel(this);
    root->addWidget(m_statsLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_trackCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SpeedGraphDialog::selectTrackIndex);

    if (!m_tracks.isEmpty()) {
        selectTrackIndex(0);
    } else {
        m_statsLabel->setText(QStringLiteral("キーフレームが設定されたプロパティがありません。"));
    }
}

SpeedCurveSummary SpeedGraphDialog::currentSummary() const
{
    return m_graph ? m_graph->summary() : SpeedCurveSummary{};
}

int SpeedGraphDialog::trackCount() const
{
    return m_tracks.size();
}

void SpeedGraphDialog::selectTrackIndex(int index)
{
    if (index < 0 || index >= m_tracks.size())
        return;
    const KeyframeTrack &tr = m_tracks[index];
    const double lo = tr.hasKeyframes() ? qMin(0.0, tr.keyframes().first().time) : 0.0;
    const double hi = tr.hasKeyframes() ? qMax(m_clipDurationSec, tr.keyframes().last().time) : m_clipDurationSec;
    const SpeedCurveSummary sum = sampleSpeedCurve(tr, lo, hi, 96);
    m_graph->setSummary(sum);
    m_statsLabel->setText(QStringLiteral("最大速度: %1 /s (t=%2s)   平均速度: %3 /s   キー数: %4")
                              .arg(sum.maxSpeed, 0, 'f', 2)
                              .arg(sum.peakTimeSec, 0, 'f', 2)
                              .arg(sum.avgSpeed, 0, 'f', 2)
                              .arg(tr.count()));
}

// ============================================================================
// Shy Layers
// ============================================================================

namespace {

constexpr const char *kShyGroup = "ShyLayers";

QMutex g_shyMutex;
bool g_shyLoaded = false;
bool g_shyFilterEnabled = false;
QSet<int> g_shyVideoTracks;
QSet<int> g_shyAudioTracks;

void ensureShyLoadedLocked()
{
    if (g_shyLoaded)
        return;
    g_shyVideoTracks.clear();
    g_shyAudioTracks.clear();
    QSettings settings;
    settings.beginGroup(QLatin1String(kShyGroup));
    g_shyFilterEnabled = settings.value(QStringLiteral("filterEnabled"), false).toBool();
    const QStringList vList = settings.value(QStringLiteral("videoTracks")).toStringList();
    for (const QString &s : vList) {
        bool ok = false;
        const int idx = s.toInt(&ok);
        if (ok && idx >= 0)
            g_shyVideoTracks.insert(idx);
    }
    const QStringList aList = settings.value(QStringLiteral("audioTracks")).toStringList();
    for (const QString &s : aList) {
        bool ok = false;
        const int idx = s.toInt(&ok);
        if (ok && idx >= 0)
            g_shyAudioTracks.insert(idx);
    }
    settings.endGroup();
    g_shyLoaded = true;
}

void saveShyLocked()
{
    QStringList vList;
    for (int idx : g_shyVideoTracks)
        vList.append(QString::number(idx));
    QStringList aList;
    for (int idx : g_shyAudioTracks)
        aList.append(QString::number(idx));

    QSettings settings;
    settings.beginGroup(QLatin1String(kShyGroup));
    settings.setValue(QStringLiteral("filterEnabled"), g_shyFilterEnabled);
    settings.setValue(QStringLiteral("videoTracks"), vList);
    settings.setValue(QStringLiteral("audioTracks"), aList);
    settings.endGroup();
    settings.sync();
}

} // namespace

bool isTrackShy(bool isAudio, int trackIdx)
{
    if (trackIdx < 0)
        return false;
    QMutexLocker lock(&g_shyMutex);
    ensureShyLoadedLocked();
    return isAudio ? g_shyAudioTracks.contains(trackIdx)
                   : g_shyVideoTracks.contains(trackIdx);
}

void setTrackShy(bool isAudio, int trackIdx, bool shy)
{
    if (trackIdx < 0)
        return;
    QMutexLocker lock(&g_shyMutex);
    ensureShyLoadedLocked();
    QSet<int> &target = isAudio ? g_shyAudioTracks : g_shyVideoTracks;
    if (shy)
        target.insert(trackIdx);
    else
        target.remove(trackIdx);
    saveShyLocked();
}

bool isShyFilterEnabled()
{
    QMutexLocker lock(&g_shyMutex);
    ensureShyLoadedLocked();
    return g_shyFilterEnabled;
}

void setShyFilterEnabled(bool enabled)
{
    QMutexLocker lock(&g_shyMutex);
    ensureShyLoadedLocked();
    g_shyFilterEnabled = enabled;
    saveShyLocked();
}

void clearShyState()
{
    QMutexLocker lock(&g_shyMutex);
    g_shyFilterEnabled = false;
    g_shyVideoTracks.clear();
    g_shyAudioTracks.clear();
    g_shyLoaded = true;
    QSettings settings;
    settings.remove(QLatin1String(kShyGroup));
    settings.sync();
}

QVector<int> visibleTrackIndices(int trackCount, bool isAudio)
{
    QVector<int> out;
    if (trackCount <= 0)
        return out;
    QMutexLocker lock(&g_shyMutex);
    ensureShyLoadedLocked();
    const QSet<int> &shySet = isAudio ? g_shyAudioTracks : g_shyVideoTracks;
    out.reserve(trackCount);
    for (int i = 0; i < trackCount; ++i) {
        if (!g_shyFilterEnabled || !shySet.contains(i))
            out.append(i);
    }
    return out;
}

ShyLayersDialog::ShyLayersDialog(Timeline *timeline, QWidget *parent)
    : QDialog(parent)
    , m_timeline(timeline)
{
    setWindowTitle(QStringLiteral("シャイレイヤー管理 (Shy Layers)"));
    setMinimumSize(420, 360);

    auto *root = new QVBoxLayout(this);
    auto *hint = new QLabel(
        QStringLiteral("シャイに設定したトラックは、フィルターON時に一覧をすっきり隠せます。\n"
                       "プレビューおよび書き出し映像・音声には常に反映され続けます。"),
        this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    m_masterFilterCheck = new QCheckBox(
        QStringLiteral("シャイフィルターを有効にする (シャイトラックを一覧から非表示)"), this);
    m_masterFilterCheck->setChecked(isShyFilterEnabled());
    root->addWidget(m_masterFilterCheck);

    m_trackList = new QListWidget(this);
    root->addWidget(m_trackList, 1);

    m_summaryLabel = new QLabel(this);
    root->addWidget(m_summaryLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ShyLayersDialog::saveAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_masterFilterCheck, &QCheckBox::toggled, this, &ShyLayersDialog::updateSummary);
    connect(m_trackList, &QListWidget::itemChanged, this, &ShyLayersDialog::updateSummary);

    populate();
    updateSummary();
}

int ShyLayersDialog::visibleVideoTrackCount() const
{
    if (!m_timeline)
        return 0;
    return visibleTrackIndices(m_timeline->videoTracks().size(), false).size();
}

int ShyLayersDialog::visibleAudioTrackCount() const
{
    if (!m_timeline)
        return 0;
    return visibleTrackIndices(m_timeline->audioTracks().size(), true).size();
}

void ShyLayersDialog::populate()
{
    m_trackList->blockSignals(true);
    m_trackList->clear();
    if (m_timeline) {
        const auto &vTracks = m_timeline->videoTracks();
        for (int i = 0; i < vTracks.size(); ++i) {
            auto *item = new QListWidgetItem(
                QStringLiteral("V%1  (%2 クリップ)").arg(i + 1).arg(vTracks[i] ? vTracks[i]->clips().size() : 0),
                m_trackList);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(isTrackShy(false, i) ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, false);
            item->setData(Qt::UserRole + 1, i);
        }
        const auto &aTracks = m_timeline->audioTracks();
        for (int i = 0; i < aTracks.size(); ++i) {
            auto *item = new QListWidgetItem(
                QStringLiteral("A%1  (%2 クリップ)").arg(i + 1).arg(aTracks[i] ? aTracks[i]->clips().size() : 0),
                m_trackList);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(isTrackShy(true, i) ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, true);
            item->setData(Qt::UserRole + 1, i);
        }
    }
    m_trackList->blockSignals(false);
}

void ShyLayersDialog::updateSummary()
{
    const int total = m_trackList->count();
    int shyCount = 0;
    for (int i = 0; i < total; ++i) {
        if (m_trackList->item(i)->checkState() == Qt::Checked)
            ++shyCount;
    }
    const int shown = m_masterFilterCheck->isChecked() ? (total - shyCount) : total;
    m_summaryLabel->setText(
        QStringLiteral("全 %1 トラック / シャイ設定: %2 / 一覧表示: %3 (レンダリング対象: 全 %1)")
            .arg(total).arg(shyCount).arg(shown));
}

void ShyLayersDialog::saveAndAccept()
{
    setShyFilterEnabled(m_masterFilterCheck->isChecked());
    for (int i = 0; i < m_trackList->count(); ++i) {
        const QListWidgetItem *item = m_trackList->item(i);
        const bool isAudio = item->data(Qt::UserRole).toBool();
        const int idx = item->data(Qt::UserRole + 1).toInt();
        setTrackShy(isAudio, idx, item->checkState() == Qt::Checked);
    }
    accept();
}

} // namespace aepro
