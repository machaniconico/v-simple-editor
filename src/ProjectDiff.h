#pragma once

#include <QString>
#include <QVector>

struct ProjectData;

namespace projdiff {
struct Change {
    enum Type { Added, Removed, Moved, Trimmed, PropertyChanged,
                EffectsChanged, TransitionChanged, TrackFlagChanged } type;
    QString path;
    QString before;
    QString after;
};

// a is the saved/before state, b is the current/after state. Clip paths
// address b, except Removed paths, which address a and cannot be selected.
// Match equal filePath values by globally nearest inPoint, one-to-one,
// across tracks of the same kind. Ties use original track/clip order.
QVector<Change> diff(const ProjectData &a, const ProjectData &b, double timeEps = 1e-3);
QString typeName(Change::Type type);
} // namespace projdiff
