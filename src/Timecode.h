#pragma once
#include <QString>

// Two fields mean MM:SS; three mean MM:SS:FF; four mean HH:MM:SS:FF.
// A leading sign applies the parsed duration relative to currentSec.
bool parseTimecodeInput(const QString &text, double fps, double currentSec, double *outSec);
