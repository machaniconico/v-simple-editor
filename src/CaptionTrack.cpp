#include "CaptionTrack.h"

#include <algorithm>

#include <QTextBoundaryFinder>

namespace caption {

void Track::addClip(const Clip& c)
{
    m_clips.append(c);
}

void Track::insertClipAt(int index, const Clip& c)
{
    if (index < 0 || index > m_clips.size())
        return;
    m_clips.insert(index, c);
}

void Track::removeClipAt(int index)
{
    if (index < 0 || index >= m_clips.size())
        return;
    m_clips.removeAt(index);
}

void Track::updateClip(int index, const Clip& c)
{
    if (index < 0 || index >= m_clips.size())
        return;
    m_clips[index] = c;
}

void Track::clear()
{
    m_clips.clear();
}

QList<Clip> Track::clips() const
{
    return m_clips;
}

Clip Track::clipAt(int index) const
{
    if (index < 0 || index >= m_clips.size())
        return Clip{};
    return m_clips[index];
}

int Track::clipCount() const
{
    return m_clips.size();
}

QList<Clip> Track::clipsAtTime(qint64 timeMs) const
{
    QList<Clip> result;
    for (const Clip& c : m_clips) {
        if (c.startMs <= timeMs && timeMs < c.endMs)
            result.append(c);
    }
    return result;
}

void Track::sortByStart()
{
    std::stable_sort(m_clips.begin(), m_clips.end(),
        [](const Clip& a, const Clip& b) { return a.startMs < b.startMs; });
}

namespace {

// words の text を連結する。語が空白を持たない場合、ASCII 同士の境目にだけ半角スペースを入れる。
QString joinWordTexts(const QList<Word>& words)
{
    QString out;
    for (const Word& w : words) {
        if (!out.isEmpty() && !w.text.isEmpty()) {
            const QChar prev = out.back();
            const QChar next = w.text.front();
            if (!prev.isSpace() && !next.isSpace() && prev.unicode() < 0x80 && next.unicode() < 0x80)
                out += QLatin1Char(' ');
        }
        out += w.text;
    }
    return out.trimmed();
}

} // namespace

std::optional<std::pair<Clip, Clip>> splitClip(const Clip& clip, qint64 atMs)
{
    if (!(clip.startMs < atMs && atMs < clip.endMs))
        return std::nullopt;

    Clip a = clip;
    Clip b = clip;
    a.endMs = atMs;
    b.startMs = atMs;
    a.words.clear();
    b.words.clear();

    if (!clip.words.isEmpty()) {
        // 語は中点が分割点より前なら a、そうでなければ b。境界をまたぐ語は側の範囲へ切り詰める。
        for (const Word& w : clip.words) {
            Word moved = w;
            const bool toA = (w.startMs + w.endMs) < 2 * atMs;
            Clip& side = toA ? a : b;
            moved.startMs = std::max(moved.startMs, side.startMs);
            moved.endMs = std::min(moved.endMs, side.endMs);
            side.words.append(moved);
        }
        // words がある clip は文字数比に落とさない (words と text がずれるため)。片側が空なら分割不可。
        if (a.words.isEmpty() || b.words.isEmpty())
            return std::nullopt;
        a.text = joinWordTexts(a.words);
        b.text = joinWordTexts(b.words);
        if (a.text.isEmpty() || b.text.isEmpty())
            return std::nullopt;
    } else {
        const QString text = clip.text.trimmed();
        if (text.size() < 2)
            return std::nullopt;
        const double ratio = static_cast<double>(atMs - clip.startMs)
                           / static_cast<double>(clip.endMs - clip.startMs);
        int pos = std::clamp(static_cast<int>(text.size() * ratio + 0.5), 1,
                             static_cast<int>(text.size()) - 1);
        // サロゲートペアや結合文字列を割らないよう書記素境界へ寄せる。
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
        finder.setPosition(pos);
        if (!finder.isAtBoundary()) {
            int next = finder.toNextBoundary();
            if (next < 0 || next >= text.size()) {
                finder.setPosition(pos);
                next = finder.toPreviousBoundary();
            }
            pos = next;
        }
        if (pos < 1 || pos > text.size() - 1)
            return std::nullopt;
        a.text = text.left(pos).trimmed();
        b.text = text.mid(pos).trimmed();
        if (a.text.isEmpty() || b.text.isEmpty())
            return std::nullopt;
    }
    return std::make_pair(a, b);
}

std::optional<Clip> mergeClips(const Clip& a, const Clip& b)
{
    if (a.endMs > b.startMs || a.actor != b.actor)
        return std::nullopt;

    Clip m = a;
    m.endMs = b.endMs;
    m.text = a.text + QLatin1Char(' ') + b.text;
    m.words = a.words + b.words;
    return m;
}

} // namespace caption
