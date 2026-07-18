#pragma once

#include "Keyframe.h"

#include <QString>
#include <QStringList>

namespace motionpreset {

QStringList presetIds();
QString displayName(const QString &presetId);
QString presetIdForDisplayName(const QString &name);

// Applies one built-in motion preset in clip-local seconds. Existing canonical
// motion tracks are replaced so repeated application is deterministic.
void applyPreset(KeyframeManager &km, const QString &presetId,
                 double clipStartSec, double clipDurationSec);

} // namespace motionpreset
