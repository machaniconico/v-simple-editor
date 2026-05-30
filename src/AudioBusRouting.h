#pragma once

// AudioBusRouting — Fairlight 的なバス / サブミックス / AUX センドの
// ルーティングモデルを表現する純粋ロジックエンジン (AB-1)。
//
// QObject / QWidget を一切持たない値クラスなので QApplication 不要で headless
// 単体テストできる (selftest `--selftest=audio-bus`, needsQApplication=false)。
// Qt は QString / QVector / QHash / QJsonObject のみに依存し、外部依存は無い。
//
// モデル概要:
//   - 各トラックは「メイン出力先バス」を 1 つ持つ (未割当は -1 = マスター直結)。
//   - 各 AudioBus は gain / mute / solo を持ち、outputBusId で別バスへ
//     サブミックス (バス→バス) できる。outputBusId=-1 はマスター直結。
//   - トラックは複数の AuxSend を持てる (1 トラック → 任意バスへの並列センド)。
//
// ゲイン解決 (本エンジンの中核):
//   resolveTrackToMasterGain(t) はトラック t のメイン経路をたどり、割当バスから
//   outputBusId 連鎖でマスターまで各 AudioBus.gain を乗算した実効ゲインを返す。
//   resolveAuxSendGain(t, b) は (t,b) の aux send が sendLevel * (バス b →…→
//   マスターのバスゲイン連鎖) でマスターに届く実効ゲインを返す。
//
// mute / solo / 循環の扱い:
//   - mute: 経路上のいずれかのバスが mute なら実効ゲインは 0。
//   - solo: いずれかのバスが solo のとき、solo されているバスへ到達する経路に
//     属さないバスは実効的に 0 (solo されたバス自身とその上流/下流のうち、
//     「マスターへ向かう途中で solo バスを通過する経路」のみ生き残る)。本実装は
//     「経路上に solo バスが 1 つでも含まれていれば生存、そうでなければ 0」という
//     規則で resolve* に反映する (Fairlight の solo-in-place 相当の近似)。
//   - 循環: bus.outputBusId 連鎖に閉路がある場合、訪問済み集合でガードして
//     無限ループせず安全に打ち切る (打ち切り時の連鎖係数はそこで停止)。
//     hasCycle() で閉路の有無を判定できる。
//
// identity 保証:
//   バスが 1 つも定義されておらず全トラックが未割当のとき、
//   resolveTrackToMasterGain は全トラックで 1.0 を返す (= 既存フラットな
//   「各トラック→マスター直結」挙動と完全一致)。AudioMixer 統合時にバス機能を
//   使わなければピクセル/サンプル等価が保たれる。

#include <QString>
#include <QVector>
#include <QHash>
#include <QJsonObject>

namespace audiobus {

// 1 本のバス。outputBusId=-1 はマスター直結、他バス id ならサブミックス。
struct AudioBus {
    int id = -1;
    QString name;
    double gain = 1.0;
    bool mute = false;
    bool solo = false;
    int outputBusId = -1;   // -1 = master、他バス id = そのバスへサブミックス
};

// トラック → バスへの並列 AUX センド。preFader はフェーダー前送り (現状は
// メタ情報として保持、ゲイン解決では sendLevel のみ使用)。
struct AuxSend {
    int trackIndex = -1;
    int busId = -1;
    double sendLevel = 1.0;
    bool preFader = false;
};

// バス / サブミックス / AUX センドのルーティングを保持する値クラス。
// デフォルトコピー可・singleton 不使用。UI 層 (AB-5: AudioBusPanel) が本クラスを
// 保持し、AudioMixer 統合 (別 story) が resolve* の結果をミックスゲインに使う。
class AudioBusRouting {
public:
    AudioBusRouting() = default;

    // --- バス CRUD ---------------------------------------------------------

    // 新しいバスを master 直結 (outputBusId=-1) で生成し、採番した id を返す。
    // id は単調増加 (削除しても再利用しない)。
    int addBus(const QString& name);

    // busId のバスを削除する。このバスを出力先にしていたバス/トラックの割当は
    // master(-1) に戻し、busId を送り先とする AuxSend も全て除去する。
    // 存在しない id なら false。
    bool removeBus(int busId);

    bool renameBus(int busId, const QString& name);

    // gain は >=0 のみ受け付ける (負値は false)。
    bool setBusGain(int busId, double gain);
    bool setBusMute(int busId, bool mute);
    bool setBusSolo(int busId, bool solo);

    const QVector<AudioBus>& buses() const { return m_buses; }

    // busId のバスを返す。存在しなければ nullptr。
    const AudioBus* bus(int busId) const;

    // --- トラック割当 ------------------------------------------------------

    // トラックのメイン出力先バスを設定する。busId=-1 で master 直結に戻す。
    // busId>=0 は存在するバスでなければ false。
    bool assignTrackToBus(int trackIndex, int busId);

    // トラックのメイン出力先バス id。未割当は -1 (master 直結相当)。
    int trackBus(int trackIndex) const;

    // --- AUX センド --------------------------------------------------------

    // aux send を追加する。同一 (trackIndex, busId) が既にあれば sendLevel /
    // preFader を上書き更新する。busId が存在しないバスでも保持はする
    // (resolve 時に無効経路として 0 になる)。
    void addAuxSend(const AuxSend& send);

    // (trackIndex, busId) の aux send を除去する。無ければ false。
    bool removeAuxSend(int trackIndex, int busId);

    const QVector<AuxSend>& auxSends() const { return m_auxSends; }

    // --- ゲイン解決 (中核) -------------------------------------------------

    // トラックのメイン経路の実効ゲイン: 割当バス→(outputBusId 連鎖)→master を
    // たどり、各 AudioBus.gain を乗算する。経路上に mute バスがあれば 0、solo
    // 規則を適用 (上記ヘッダコメント参照)。循環は訪問済み集合で安全に打ち切る。
    // 未割当トラック (master 直結) は 1.0、ただし solo がアクティブで master 直結
    // 経路が solo を通らない場合は 0。
    double resolveTrackToMasterGain(int trackIndex) const;

    // (trackIndex, busId) の aux send が master に届く実効ゲイン:
    //   sendLevel * (busId → outputBusId 連鎖 → master のバスゲイン連鎖)。
    // busId が存在しなければ 0。経路上の mute は 0、solo 規則を適用、循環は安全
    // 打ち切り。該当 aux send が無ければ 0。
    double resolveAuxSendGain(int trackIndex, int busId) const;

    // bus.outputBusId の連鎖に閉路があれば true。
    bool hasCycle() const;

    // --- 直列化 ------------------------------------------------------------

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& obj);
    void clear();

private:
    // busId の配列インデックスを返す。無ければ -1。
    int indexOfBus(int busId) const;

    // いずれかのバスが solo なら true (solo 規則の適用判定に使う)。
    bool anySolo() const;

    // startBusId から outputBusId 連鎖で master までたどり、各バスの gain 積を
    // 返す純粋ヘルパー。mute に当たれば 0。soloActive が true のとき、連鎖中に
    // solo バスを 1 つも通らなければ 0 (solo 規則)。循環は訪問済み集合で打ち切り
    // (係数は打ち切り時点の積)。startBusId=-1 (master 直結) は係数 1.0、ただし
    // soloActive のときは 0 (solo を通らないため)。
    double chainGainToMaster(int startBusId, bool soloActive) const;

    QVector<AudioBus> m_buses;
    QVector<AuxSend>  m_auxSends;
    QHash<int, int>   m_trackBus;   // trackIndex -> busId (未登録は -1 扱い)
    int m_nextBusId = 0;            // 単調増加 id カウンタ
};

} // namespace audiobus
