#pragma once
#include "FcpxmlImport.h"
#include <QString>
#include <functional>
#include <optional>

namespace uncprobe {
// \\?\UNC\srv\sh\x, //srv/sh/x, \/srv/sh/x and file://srv/sh/x -> \\srv\sh\x. Device paths
// (\\.\...), other \\?\ forms, file:///local and file://localhost are not UNC: nullopt.
std::optional<QString> normaliseUnc(const QString &path);
// "\\srv\sh" (lower case) of a UNC path in any form accepted by normaliseUnc.
QString shareRoot(const QString &path);
// True when the path exists. Called with a share root and with each normalised file of a
// share that answered, always on a pool thread. Empty = the default (QFileInfo::exists on
// Windows, always false elsewhere).
using ProbeFn = std::function<bool(const QString &path)>;
// After an FCPXML import, probes each distinct share of result.offlineSources once on a
// dedicated pool under one overall deadline. A sentinel whose share answers and whose file
// exists is relinked to its normalised path (counts and warning updated); a file missing on an
// answering share keeps the normalised path as ordinary missing media; any other sentinel is
// left alone. Returns the number of relinked sentinels.
int relinkOfflineSources(fcpx::xml::ImportResult &result, ProbeFn probe = {},
                         int deadlineMs = 3000, int maxConcurrent = 4);
} // namespace uncprobe
