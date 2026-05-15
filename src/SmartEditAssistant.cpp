#include "SmartEditAssistant.h"

#include "AutoEdit.h"

#include <QFileInfo>
#include <QMetaType>

#include <algorithm>
#include <utility>

namespace {

using smartedit::AnalysisConfig;
using smartedit::CutSuggestion;

AutoEditConfig toAutoEditConfig(const AnalysisConfig &config)
{
    AutoEditConfig autoConfig;
    autoConfig.silenceThreshold = config.silenceThreshold;
    autoConfig.minSilenceDuration = static_cast<double>(config.minSilenceMs) / 1000.0;
    autoConfig.sceneChangeThreshold = config.sceneChangeThreshold;
    return autoConfig;
}

qint64 toMs(double seconds)
{
    return std::max<qint64>(0, static_cast<qint64>(seconds * 1000.0 + 0.5));
}

CutSuggestion::Reason combineReason(CutSuggestion::Reason left,
                                    CutSuggestion::Reason right)
{
    if (left == right)
        return left;
    return CutSuggestion::Combined;
}

CutSuggestion buildSilenceSuggestion(const SilenceRegion &silence,
                                     const AnalysisConfig &config)
{
    const qint64 startMs = toMs(silence.startTime);
    const qint64 endMs = std::max(startMs, toMs(silence.endTime));
    const qint64 durationMs = std::max<qint64>(0, endMs - startMs);
    const qint64 baselineMs = std::max<qint64>(config.minSilenceMs, 1);

    CutSuggestion suggestion;
    suggestion.startMs = startMs;
    suggestion.endMs = endMs;
    suggestion.reason = CutSuggestion::Silence;
    suggestion.confidence = qBound(0.0,
                                   static_cast<double>(durationMs) / static_cast<double>(baselineMs),
                                   1.0);
    return suggestion;
}

CutSuggestion buildSceneSuggestion(const SceneChange &scene)
{
    const qint64 timeMs = toMs(scene.time);

    CutSuggestion suggestion;
    suggestion.startMs = timeMs;
    suggestion.endMs = timeMs;
    suggestion.reason = CutSuggestion::SceneChange;
    suggestion.confidence = qBound(0.0, scene.confidence, 1.0);
    return suggestion;
}

QVector<CutSuggestion> mergeAdjacentSuggestions(QVector<CutSuggestion> suggestions,
                                                qint64 mergeAdjacentMs)
{
    if (suggestions.isEmpty())
        return suggestions;

    std::sort(suggestions.begin(), suggestions.end(),
              [](const CutSuggestion &left, const CutSuggestion &right) {
                  if (left.startMs != right.startMs)
                      return left.startMs < right.startMs;
                  if (left.endMs != right.endMs)
                      return left.endMs < right.endMs;
                  return left.reason < right.reason;
              });

    QVector<CutSuggestion> merged;
    merged.reserve(suggestions.size());
    merged.append(suggestions.front());

    for (int i = 1; i < suggestions.size(); ++i) {
        const CutSuggestion &next = suggestions[i];
        CutSuggestion &current = merged.last();
        const qint64 gapMs = next.startMs - current.endMs;

        if (gapMs <= mergeAdjacentMs) {
            current.startMs = std::min(current.startMs, next.startMs);
            current.endMs = std::max(current.endMs, next.endMs);
            current.reason = combineReason(current.reason, next.reason);
            current.confidence = std::max(current.confidence, next.confidence);
            continue;
        }

        merged.append(next);
    }

    return merged;
}

QVector<CutSuggestion> dropShortClips(QVector<CutSuggestion> suggestions,
                                      qint64 minClipDurationMs)
{
    if (suggestions.size() < 2 || minClipDurationMs <= 0)
        return suggestions;

    QVector<CutSuggestion> filtered;
    filtered.reserve(suggestions.size());
    filtered.append(suggestions.front());

    for (int i = 1; i < suggestions.size(); ++i) {
        const CutSuggestion &next = suggestions[i];
        CutSuggestion &current = filtered.last();
        const qint64 gapMs = next.startMs - current.endMs;

        if (gapMs >= 0 && gapMs < minClipDurationMs) {
            current.endMs = std::max(current.endMs, next.endMs);
            current.reason = combineReason(current.reason, next.reason);
            current.confidence = std::max(current.confidence, next.confidence);
            continue;
        }

        filtered.append(next);
    }

    return filtered;
}

} // namespace

namespace smartedit {

Assistant::Assistant(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<smartedit::CutSuggestion>();
    qRegisterMetaType<QVector<smartedit::CutSuggestion>>();
}

QVector<CutSuggestion> Assistant::analyze(const QString &videoPath,
                                          const AnalysisConfig &config)
{
    emit analysisProgress(0);

    const QString trimmedPath = videoPath.trimmed();
    if (trimmedPath.isEmpty()) {
        const QString error = tr("SmartEdit analysis requires a video path.");
        emit analysisFailed(error);
        return {};
    }

    if (!QFileInfo::exists(trimmedPath)) {
        const QString error = tr("Video file does not exist: %1").arg(trimmedPath);
        emit analysisFailed(error);
        return {};
    }

    const AutoEditConfig autoConfig = toAutoEditConfig(config);

    emit analysisProgress(20);
    const QVector<SilenceRegion> silences =
        AutoEdit::detectSilenceFromFile(trimmedPath, autoConfig);

    emit analysisProgress(60);
    const QVector<SceneChange> scenes =
        AutoEdit::detectSceneChanges(trimmedPath, autoConfig);

    QVector<CutSuggestion> suggestions;
    suggestions.reserve(silences.size() + scenes.size());

    for (const SilenceRegion &silence : silences)
        suggestions.append(buildSilenceSuggestion(silence, config));

    for (const SceneChange &scene : scenes)
        suggestions.append(buildSceneSuggestion(scene));

    emit analysisProgress(85);
    suggestions = mergeAdjacentSuggestions(std::move(suggestions), config.mergeAdjacentMs);
    suggestions = dropShortClips(std::move(suggestions), config.minClipDurationMs);

    emit analysisProgress(100);
    emit analysisFinished(suggestions);
    return suggestions;
}

} // namespace smartedit
