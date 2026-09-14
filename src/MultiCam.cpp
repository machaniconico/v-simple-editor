#include "MultiCam.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QtMath>

#include <algorithm>

// ===========================================================================
// MultiCamSession (legacy / advanced — referenced by MainWindow).
// Audio synchronization shares the dialog's file-based correlation helper.
// ===========================================================================

MultiCamSession::MultiCamSession(QObject *parent)
    : QObject(parent)
{
}

void MultiCamSession::addSource(const QString &filePath, const QString &label)
{
    CameraSource src;
    src.filePath = filePath;
    src.label = label.isEmpty()
                  ? QStringLiteral("Camera %1").arg(m_sources.size() + 1)
                  : label;
    src.syncOffset = 0.0;
    src.duration = 0.0;
    src.isActive = true;
    m_sources.append(src);
    emit sourcesChanged();
}

void MultiCamSession::removeSource(int index)
{
    if (index < 0 || index >= m_sources.size())
        return;
    m_sources.remove(index);

    // Drop cuts that referenced the removed camera; reindex the rest so
    // that indices > index shift down by one.
    for (int i = m_cuts.size() - 1; i >= 0; --i) {
        if (m_cuts[i].cameraIndex == index)
            m_cuts.remove(i);
        else if (m_cuts[i].cameraIndex > index)
            --m_cuts[i].cameraIndex;
    }
    emit sourcesChanged();
    emit cutsChanged();
}

void MultiCamSession::setSyncOffset(int sourceIndex, double offset)
{
    if (sourceIndex < 0 || sourceIndex >= m_sources.size())
        return;
    m_sources[sourceIndex].syncOffset = offset;
    emit sourcesChanged();
}

multicam::AudioSyncReport MultiCamSession::autoSyncByAudio()
{
    QStringList paths;
    paths.reserve(m_sources.size());
    for (const CameraSource &src : m_sources)
        paths.append(src.filePath);
    const multicam::AudioSyncReport report = multicam::estimateOffsetsForFiles(paths);
    for (int i = 0; i < m_sources.size(); ++i)
        m_sources[i].syncOffset = static_cast<double>(report.offsetsUs[i]) / 1000000.0;
    emit syncCompleted();
    return report;
}

void MultiCamSession::switchToCamera(int cameraIndex, double time)
{
    if (cameraIndex < 0 || cameraIndex >= m_sources.size())
        return;
    // Append a zero-length switch marker; generateEditList() expands
    // these into real segments by pairing consecutive switches.
    CameraCut c;
    c.cameraIndex = cameraIndex;
    c.startTime = time;
    c.endTime = time;
    m_cuts.append(c);
    sortCuts();
    emit cutsChanged();
}

void MultiCamSession::addCut(int cameraIndex, double startTime, double endTime)
{
    if (cameraIndex < 0 || cameraIndex >= m_sources.size())
        return;
    if (endTime < startTime)
        std::swap(startTime, endTime);
    CameraCut c;
    c.cameraIndex = cameraIndex;
    c.startTime = startTime;
    c.endTime = endTime;
    m_cuts.append(c);
    sortCuts();
    emit cutsChanged();
}

void MultiCamSession::removeCut(int index)
{
    if (index < 0 || index >= m_cuts.size())
        return;
    m_cuts.remove(index);
    emit cutsChanged();
}

int MultiCamSession::activeCameraAt(double time) const
{
    int active = -1;
    double bestStart = -1.0;
    for (const auto &c : m_cuts) {
        if (c.startTime <= time && c.startTime >= bestStart) {
            bestStart = c.startTime;
            active = c.cameraIndex;
        }
    }
    if (active < 0 && !m_sources.isEmpty())
        active = 0;
    return active;
}

int MultiCamSession::gridColumns() const
{
    const int n = m_sources.size();
    if (n <= 1) return 1;
    if (n <= 4) return 2;
    return 3;
}

int MultiCamSession::gridRows() const
{
    const int n = m_sources.size();
    if (n <= 0) return 0;
    if (n <= 2) return 1;
    if (n <= 4) return 2;
    return 2;
}

QVector<MultiCamSession::EditSegment> MultiCamSession::generateEditList() const
{
    QVector<EditSegment> out;
    if (m_sources.isEmpty() || m_cuts.isEmpty())
        return out;

    // Cuts are sorted by sortCuts(); pair consecutive switch markers.
    for (int i = 0; i < m_cuts.size(); ++i) {
        const CameraCut &c = m_cuts[i];
        const double tlEnd =
            (i + 1 < m_cuts.size()) ? m_cuts[i + 1].startTime : totalDuration();
        EditSegment s;
        s.cameraIndex = c.cameraIndex;
        s.timelineStart = c.startTime;
        s.timelineEnd = tlEnd;
        const double off = (c.cameraIndex >= 0 && c.cameraIndex < m_sources.size())
                               ? m_sources[c.cameraIndex].syncOffset
                               : 0.0;
        s.sourceStart = c.startTime - off;
        s.sourceEnd = tlEnd - off;
        out.append(s);
    }
    return out;
}

double MultiCamSession::totalDuration() const
{
    double maxEnd = 0.0;
    for (const auto &src : m_sources) {
        const double end = src.duration - src.syncOffset;
        if (end > maxEnd) maxEnd = end;
    }
    return maxEnd;
}

void MultiCamSession::sortCuts()
{
    std::sort(m_cuts.begin(), m_cuts.end(),
              [](const CameraCut &a, const CameraCut &b) {
                  return a.startTime < b.startTime;
              });
}

// ===========================================================================
// MultiCamProject — pure-data EDL produced by MultiCamDialog.
// ===========================================================================

QJsonObject MultiCamProject::toJson() const
{
    QJsonArray angleArr;
    for (const MultiCamAngle &a : angles) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), a.id);
        o.insert(QStringLiteral("sourcePath"), a.sourcePath);
        o.insert(QStringLiteral("syncOffsetUs"),
                 static_cast<double>(a.syncOffsetUs));
        o.insert(QStringLiteral("label"), a.label);
        angleArr.append(o);
    }

    QJsonArray switchArr;
    for (const MultiCamSwitch &s : switches) {
        QJsonObject o;
        o.insert(QStringLiteral("timelineUs"),
                 static_cast<double>(s.timelineUs));
        o.insert(QStringLiteral("activeAngleId"), s.activeAngleId);
        switchArr.append(o);
    }

    QJsonObject root;
    root.insert(QStringLiteral("angles"), angleArr);
    root.insert(QStringLiteral("switches"), switchArr);
    root.insert(QStringLiteral("defaultAngleId"), defaultAngleId);
    return root;
}

MultiCamProject MultiCamProject::fromJson(const QJsonObject &o)
{
    MultiCamProject p;

    const QJsonArray angleArr = o.value(QStringLiteral("angles")).toArray();
    p.angles.reserve(angleArr.size());
    for (const QJsonValue &v : angleArr) {
        const QJsonObject ao = v.toObject();
        MultiCamAngle a;
        a.id = ao.value(QStringLiteral("id")).toInt();
        a.sourcePath = ao.value(QStringLiteral("sourcePath")).toString();
        a.syncOffsetUs = static_cast<qint64>(
            ao.value(QStringLiteral("syncOffsetUs")).toDouble());
        a.label = ao.value(QStringLiteral("label")).toString();
        p.angles.append(a);
    }

    const QJsonArray switchArr = o.value(QStringLiteral("switches")).toArray();
    p.switches.reserve(switchArr.size());
    for (const QJsonValue &v : switchArr) {
        const QJsonObject so = v.toObject();
        MultiCamSwitch s;
        s.timelineUs = static_cast<qint64>(
            so.value(QStringLiteral("timelineUs")).toDouble());
        s.activeAngleId = so.value(QStringLiteral("activeAngleId")).toInt();
        p.switches.append(s);
    }
    // Ensure invariant: switches sorted by timelineUs ascending.
    std::sort(p.switches.begin(), p.switches.end(),
              [](const MultiCamSwitch &a, const MultiCamSwitch &b) {
                  return a.timelineUs < b.timelineUs;
              });

    p.defaultAngleId = o.value(QStringLiteral("defaultAngleId")).toInt();
    return p;
}

int MultiCamProject::activeAngleAt(qint64 timelineUs) const
{
    // Walk backwards over switches (sorted ascending) and return the
    // first whose timelineUs <= timelineUs. O(n) is fine for the
    // dialog-scale switch counts (typically < 1000).
    for (int i = switches.size() - 1; i >= 0; --i) {
        if (switches[i].timelineUs <= timelineUs)
            return switches[i].activeAngleId;
    }
    return defaultAngleId;
}
