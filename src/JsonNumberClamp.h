#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QList>

// Out-of-range number literals (1e400, 1e-400) make QJsonDocument::fromJson refuse the whole
// document. clampOutOfRangeNumbers rewrites them to +-DBL_MAX / 0; parseWithClampFallback does
// so only after a plain parse has failed.
namespace jsonclamp {

// Span of a substituted literal in the rewritten buffer, and the length of the original token.
struct Substitution {
    qsizetype start;
    qsizetype end;
    qsizetype originalLength;
};

// Returns the rewritten document, or an empty QByteArray when nothing was clamped (use `json`).
// String contents are never touched. Counts every call for scanCountForTest().
QByteArray clampOutOfRangeNumbers(const QByteArray &json, int *clamped = nullptr,
                                  QList<Substitution> *substitutions = nullptr);

// Maps an offset in the rewritten buffer back to the original bytes.
qsizetype originalOffset(qsizetype rewrittenOffset, const QList<Substitution> &substitutions);

// Plain fromJson first (no scan on success); only a failed parse is retried with the clamp.
// A failure that clamping cannot cure returns the plain error; one that remains after the
// clamp returns the error of the rewritten text with its offset mapped to the original bytes.
QJsonDocument parseWithClampFallback(const QByteArray &json, QJsonParseError *err = nullptr,
                                     int *clamped = nullptr);

// Number of clampOutOfRangeNumbers calls so far.
int scanCountForTest();

} // namespace jsonclamp
