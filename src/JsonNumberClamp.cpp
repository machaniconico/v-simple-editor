#include "JsonNumberClamp.h"

#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <charconv>

namespace jsonclamp {
namespace {
std::atomic<int> g_scanCount{0};
} // namespace

// US-2215: classify a number token std::from_chars (locale independent) reports as
// out of range. MSVC returns result_out_of_range with +-inf / 0 for 1e309 and 1e-400
// alike, so the value cannot tell them apart: the sign of the decimal exponent of the
// first non-zero mantissa digit does.
static bool numberTokenOverflows(const char *first, const char *last)
{
    const char *p = first;
    if (p < last && *p == '-') ++p;
    long long intDigitsAfterFirstNonZero = 0; // digits at and after the first non-zero one, left of '.'
    long long leadingFractionZeros = 0;       // zeros between '.' and the first non-zero digit
    bool seenNonZero = false, inFraction = false;
    for (; p < last && *p != 'e' && *p != 'E'; ++p) {
        if (*p == '.') { inFraction = true; continue; }
        if (!seenNonZero && *p == '0') {
            if (inFraction) ++leadingFractionZeros;
            continue;
        }
        seenNonZero = true;
        if (!inFraction) ++intDigitsAfterFirstNonZero;
    }
    long long exp = 0;
    if (p < last) {
        ++p;
        const bool neg = p < last && *p == '-';
        if (p < last && (*p == '-' || *p == '+')) ++p;
        for (; p < last && *p >= '0' && *p <= '9'; ++p)
            exp = std::min(exp * 10 + (*p - '0'), 100000000LL);
        if (neg) exp = -exp;
    }
    const long long magnitude = intDigitsAfterFirstNonZero > 0 ? exp + intDigitsAfterFirstNonZero - 1
                                                               : exp - leadingFractionZeros - 1;
    return magnitude > 0;
}

// True when [p, last) is exactly one JSON number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
static bool isJsonNumberToken(const char *p, const char *last)
{
    auto digits = [&]() {
        const char *start = p;
        while (p < last && *p >= '0' && *p <= '9') ++p;
        return p - start;
    };
    if (p < last && *p == '-') ++p;
    if (p < last && *p == '0') ++p;
    else if (p < last && *p >= '1' && *p <= '9') digits();
    else return false;
    if (p < last && *p == '.') {
        ++p;
        if (digits() == 0) return false;
    }
    if (p < last && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < last && (*p == '+' || *p == '-')) ++p;
        if (digits() == 0) return false;
    }
    return p == last;
}

QByteArray clampOutOfRangeNumbers(const QByteArray &json, int *clampedCount, QList<Substitution> *substitutionsOut)
{
    ++g_scanCount;
    QByteArray rewritten;
    QList<Substitution> substitutions;
    int clamped = 0;
    const char *const begin = json.constData();
    const char *const end = begin + json.size();
    const char *copied = begin; // start of the not yet copied tail
    for (const char *p = begin; p < end;) {
        const char c = *p;
        if (c == '"') { // skip the string, honouring backslash escapes
            for (++p; p < end && *p != '"'; ++p)
                if (*p == '\\' && p + 1 < end) ++p;
            if (p < end) ++p;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char *tokenEnd = p + 1;
            while (tokenEnd < end && ((*tokenEnd >= '0' && *tokenEnd <= '9') || *tokenEnd == '.' || *tokenEnd == '+'
                                      || *tokenEnd == '-' || *tokenEnd == 'e' || *tokenEnd == 'E'))
                ++tokenEnd;
            double value = 0.0;
            const auto res = std::from_chars(p, tokenEnd, value);
            if (res.ec == std::errc::result_out_of_range && res.ptr == tokenEnd
                && isJsonNumberToken(p, tokenEnd)) {
                if (rewritten.isEmpty())
                    rewritten.reserve(json.size() + 32);
                rewritten.append(copied, int(p - copied));
                const qsizetype substitutionStart = rewritten.size();
                const bool negative = *p == '-';
                if (numberTokenOverflows(p, tokenEnd))
                    rewritten.append(negative ? "-1.7976931348623157e308" : "1.7976931348623157e308");
                else
                    rewritten.append("0");
                substitutions.append({substitutionStart, rewritten.size(), qsizetype(tokenEnd - p)});
                copied = tokenEnd;
                ++clamped;
            }
            p = tokenEnd;
        } else {
            ++p;
        }
    }
    if (clamped > 0)
        rewritten.append(copied, int(end - copied));
    if (clampedCount)
        *clampedCount = clamped;
    if (substitutionsOut)
        *substitutionsOut = substitutions;
    return rewritten;
}

qsizetype originalOffset(qsizetype offset, const QList<Substitution> &substitutions)
{
    qsizetype shift = 0;
    for (const Substitution &s : substitutions) {
        if (s.end <= offset)
            shift += s.originalLength - (s.end - s.start);
        else if (s.start < offset) { // inside a substituted span: the token's start
            offset = s.start;
            break;
        }
    }
    return offset + shift;
}

QJsonDocument parseWithClampFallback(const QByteArray &json, QJsonParseError *err, int *clampedCount)
{
    if (clampedCount)
        *clampedCount = 0;
    QJsonParseError plainError;
    QJsonDocument doc = QJsonDocument::fromJson(json, &plainError);
    if (!doc.isNull()) {
        if (err)
            *err = plainError;
        return doc;
    }
    int clamped = 0;
    QList<Substitution> substitutions;
    const QByteArray rewritten = clampOutOfRangeNumbers(json, &clamped, &substitutions);
    if (clamped == 0) {
        if (err)
            *err = plainError;
        return doc;
    }
    QJsonParseError clampedError;
    doc = QJsonDocument::fromJson(rewritten, &clampedError);
    if (doc.isNull()) {
        clampedError.offset = int(originalOffset(clampedError.offset, substitutions));
        if (err)
            *err = clampedError;
        return doc;
    }
    qWarning("jsonclamp: clamped %d out-of-range number literal(s) to +-DBL_MAX / 0 while reading JSON", clamped);
    if (clampedCount)
        *clampedCount = clamped;
    if (err)
        *err = clampedError;
    return doc;
}

int scanCountForTest()
{
    return g_scanCount.load();
}

} // namespace jsonclamp
