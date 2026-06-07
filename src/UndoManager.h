#pragma once

#include <QObject>
#include <QVector>
#include <QStack>
#include <QStringList>
#include <functional>
#include "Timeline.h"

// ABI note: UndoManager does NOT persist across sessions, so adding fields
// here is always backward-compatible — no migration needed. New fields
// default to -1 (no selection) and any stale undo entry that lacks them will
// simply clear the selection on that axis during restore.
struct TimelineState {
    // One ClipInfo vector per VIDEO row (videoTracks[0] = V1, [1] = V2, ...)
    // and per AUDIO row (audioTracks[0] = A1, [1] = A2, ...). Earlier
    // single-track versions only stored V1+A1 here, so undo silently
    // dropped any clip that lived on V2/V3 — this multi-track layout
    // restores the whole timeline state.
    QVector<QVector<ClipInfo>> videoTracks;
    QVector<QVector<ClipInfo>> audioTracks;
    // Legacy V1-relative selection (kept for source compatibility).
    int selectedClip = -1;
    // V2+ track-aware selection: (track index, clip index) — -1 means
    // no selection. Populated by currentState() and honoured by
    // restoreState() so Ctrl+Z doesn't clear a selection on V2/V3.
    int selectedVideoTrackIndex = -1;
    int selectedVideoClipIndex = -1;
    int selectedAudioTrackIndex = -1;
    int selectedAudioClipIndex = -1;
    double playheadPos = 0.0;
    QVector<double> audioTrackGains;
    // スナップショット時のプロジェクト出力ジオメトリ。SNS プリセット(プロジェクトを
    // 9:16 にリサイズする)適用後の Ctrl+Z が、クリップの fit だけでなく**元の
    // プロジェクトサイズも**復元できるよう捕捉する。これが無いと undo は fit を戻すが
    // プロジェクトは 9:16 のまま残り、元アスペクトのクリップが 9:16 を充填=縦伸び。
    // projectWidth <= 0 = 未捕捉(レガシー/空ベースライン): restore はサイズをスキップ。
    int projectWidth = -1;
    int projectHeight = -1;
    bool projectExplicitOutput = false;
};

class UndoManager : public QObject
{
    Q_OBJECT

public:
    explicit UndoManager(QObject *parent = nullptr);

    void saveState(const TimelineState &state, const QString &description);
    bool canUndo() const { return m_undoStack.size() > 1; }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

    TimelineState undo();
    TimelineState redo();

    QString undoDescription() const;
    QString redoDescription() const;

    int currentIndex() const { return m_undoStack.size() - 1; }

    QStringList historyDescriptions() const;

    bool jumpTo(int index);

    void clear();

signals:
    void stateChanged();
    void historyChanged();
    void stateJumpRequested(const TimelineState &state);

private:
    struct Entry {
        TimelineState state;
        QString description;
    };

    QStack<Entry> m_undoStack;
    QStack<Entry> m_redoStack;
    static constexpr int MAX_UNDO = 100;
};
