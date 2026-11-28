#pragma once
#include <QString>
#include <QList>

#include <optional>
#include <utility>

namespace caption {

struct Word {
    qint64  startMs = 0;
    qint64  endMs   = 0;
    QString text;
    double  confidence = 1.0;
};

struct Clip {
    qint64  startMs = 0;
    qint64  endMs   = 0;
    QString text;
    QString actor;  // 話者識別 (optional, 空文字 OK)
    QList<Word> words; // optional absolute per-word timing

    qint64 durationMs() const { return endMs - startMs; }
    bool   isValid()    const { return endMs > startMs && !text.isEmpty(); }
};

class Track {
public:
    Track() = default;

    void addClip(const Clip& c);
    void insertClipAt(int index, const Clip& c);
    void removeClipAt(int index);
    void updateClip(int index, const Clip& c);
    void clear();

    QList<Clip> clips() const;
    Clip        clipAt(int index) const;
    int         clipCount() const;

    // 該当時刻に表示すべき clip 群 (startMs <= t < endMs)
    QList<Clip> clipsAtTime(qint64 timeMs) const;

    void sortByStart();

private:
    QList<Clip> m_clips;
};

// atMs で 2 つに分割する (startMs < atMs < endMs のときだけ)。words があれば時刻で振り分けて
// text を words から再構成し、無ければ文字数比で分ける。actor は両側に複製する。
std::optional<std::pair<Clip, Clip>> splitClip(const Clip& clip, qint64 atMs);

// 隣接 (a.endMs <= b.startMs) かつ同じ actor の 2 つを結合する。text は半角スペース 1 つで連結。
std::optional<Clip> mergeClips(const Clip& a, const Clip& b);

} // namespace caption
