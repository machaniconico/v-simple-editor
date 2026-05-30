#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <cmath>

#include "../AudioBusRouting.h"

// audio-bus selftest (AB-2) — AudioBusRouting 純粋エンジンの単体ゲート群。
// QObject / QWidget を持たない値クラスなので QApplication 不要
// (needsQApplication=false、--selftest=audio-bus)。
// バス CRUD / トラック割当 / サブミックス連鎖 / mute / solo / AUX センド /
// removeBus の波及 / 循環ガード / setter / JSON round-trip を検証する。

namespace {

// 浮動小数の許容誤差比較。
bool nearly(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) < eps;
}

} // namespace

int runAudioBusSelftest()
{
    using audiobus::AudioBus;
    using audiobus::AudioBusRouting;
    using audiobus::AuxSend;

    qInfo().noquote() << "[audio-bus] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[audio-bus] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[audio-bus] FAIL" << name << ":" << msg; };

    // --- G1: addBus が id 採番し buses() に入る、master 直結 -------------------
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        const int idB = r.addBus(QStringLiteral("Bus B"));
        const AudioBus* a = r.bus(idA);
        const bool ok = idA >= 0
            && idB >= 0
            && idA != idB
            && r.buses().size() == 2
            && a != nullptr
            && a->name == QStringLiteral("Bus A")
            && a->outputBusId == -1
            && nearly(a->gain, 1.0);
        if (ok) {
            pass("G1 addBus assigns id and stores master-routed bus");
        } else {
            fail("G1 addBus", QStringLiteral("idA=%1 idB=%2 count=%3")
                    .arg(idA).arg(idB).arg(r.buses().size()));
        }
    }

    // --- G2: assignTrackToBus + trackBus 一致 --------------------------------
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        const bool assigned = r.assignTrackToBus(3, idA);
        const bool missing = r.assignTrackToBus(4, 9999);   // 不在バスは false
        const bool ok = assigned
            && r.trackBus(3) == idA
            && !missing
            && r.trackBus(4) == -1     // 失敗時は割当されない (未割当 = -1)
            && r.trackBus(7) == -1;    // 未設定トラックは -1
        if (ok) {
            pass("G2 assignTrackToBus and trackBus agree");
        } else {
            fail("G2 assignTrackToBus", QStringLiteral("trackBus(3)=%1 missing=%2")
                    .arg(r.trackBus(3)).arg(missing));
        }
    }

    // --- G3: トラック→バス(gain=0.5)→master = 0.5 ----------------------------
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        r.setBusGain(idA, 0.5);
        r.assignTrackToBus(0, idA);
        const double g = r.resolveTrackToMasterGain(0);
        if (nearly(g, 0.5)) {
            pass("G3 single bus gain resolves track->master = 0.5");
        } else {
            fail("G3 single bus gain", QStringLiteral("gain=%1 expected=0.5").arg(g));
        }
    }

    // --- G4: サブミックス連鎖 トラック→busA(0.5)→busB(0.5)→master = 0.25 -----
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        const int idB = r.addBus(QStringLiteral("Bus B"));
        r.setBusGain(idA, 0.5);
        r.setBusGain(idB, 0.5);
        // busA を busB へサブミックスするため fromJson 経由で outputBusId を設定。
        // (公開 setter に outputBusId 変更が無いため JSON round-trip で構成する)
        QJsonObject obj = r.toJson();
        // toJson の buses 配列から busA の outputBusId を busB に書き換える。
        QJsonArray busArr = obj.value(QStringLiteral("buses")).toArray();
        for (int i = 0; i < busArr.size(); ++i) {
            QJsonObject b = busArr.at(i).toObject();
            if (b.value(QStringLiteral("id")).toInt() == idA) {
                b.insert(QStringLiteral("outputBusId"), idB);
                busArr.replace(i, b);
            }
        }
        obj.insert(QStringLiteral("buses"), busArr);
        r.fromJson(obj);
        r.assignTrackToBus(0, idA);
        const double g = r.resolveTrackToMasterGain(0);
        if (nearly(g, 0.25)) {
            pass("G4 submix chain busA(0.5)->busB(0.5)->master = 0.25");
        } else {
            fail("G4 submix chain", QStringLiteral("gain=%1 expected=0.25").arg(g));
        }
    }

    // --- G5: 未割当トラックは 1.0 (identity)、バス全くなしでも 1.0 ------------
    {
        AudioBusRouting empty;
        const bool flatOk = nearly(empty.resolveTrackToMasterGain(0), 1.0)
            && nearly(empty.resolveTrackToMasterGain(5), 1.0);
        AudioBusRouting r;
        r.addBus(QStringLiteral("Bus A"));   // バスはあるがトラックは未割当
        const bool unassignedOk = nearly(r.resolveTrackToMasterGain(2), 1.0);
        if (flatOk && unassignedOk) {
            pass("G5 unassigned track / no bus stays identity 1.0");
        } else {
            fail("G5 identity", QStringLiteral("empty(0)=%1 unassigned(2)=%2")
                    .arg(empty.resolveTrackToMasterGain(0))
                    .arg(r.resolveTrackToMasterGain(2)));
        }
    }

    // --- G6: bus mute で resolveTrackToMasterGain=0 --------------------------
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        r.setBusGain(idA, 0.8);
        r.assignTrackToBus(0, idA);
        const double before = r.resolveTrackToMasterGain(0);
        r.setBusMute(idA, true);
        const double after = r.resolveTrackToMasterGain(0);
        if (nearly(before, 0.8) && nearly(after, 0.0)) {
            pass("G6 muted bus resolves to 0");
        } else {
            fail("G6 mute", QStringLiteral("before=%1 after=%2").arg(before).arg(after));
        }
    }

    // --- G7: solo 経路は通り、非 solo 経路は 0 -------------------------------
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));   // solo 対象
        const int idB = r.addBus(QStringLiteral("Bus B"));   // 非 solo
        r.assignTrackToBus(0, idA);
        r.assignTrackToBus(1, idB);
        r.setBusSolo(idA, true);
        const double soloTrack = r.resolveTrackToMasterGain(0);   // solo を通る → 生存
        const double otherTrack = r.resolveTrackToMasterGain(1);  // solo を通らない → 0
        const double masterDirect = r.resolveTrackToMasterGain(9); // master 直結 → 0
        if (nearly(soloTrack, 1.0) && nearly(otherTrack, 0.0) && nearly(masterDirect, 0.0)) {
            pass("G7 solo keeps solo path, mutes non-solo and master-direct paths");
        } else {
            fail("G7 solo", QStringLiteral("solo=%1 other=%2 masterDirect=%3")
                    .arg(soloTrack).arg(otherTrack).arg(masterDirect));
        }
    }

    // --- G8: addAuxSend + resolveAuxSendGain = sendLevel * バスゲイン連鎖 -----
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Aux Bus"));
        r.setBusGain(idA, 0.5);
        AuxSend send;
        send.trackIndex = 0;
        send.busId = idA;
        send.sendLevel = 0.4;
        send.preFader = false;
        r.addAuxSend(send);
        const double g = r.resolveAuxSendGain(0, idA);   // 0.4 * 0.5 = 0.2
        const double missing = r.resolveAuxSendGain(0, 9999);  // 不在バス → 0
        const double noSend = r.resolveAuxSendGain(5, idA);    // 該当 send 無し → 0
        // 上書き更新の確認: 同一 (track,bus) を再投入すると level が更新される。
        AuxSend updated = send;
        updated.sendLevel = 0.8;
        r.addAuxSend(updated);
        const double g2 = r.resolveAuxSendGain(0, idA);  // 0.8 * 0.5 = 0.4
        if (nearly(g, 0.2) && nearly(missing, 0.0) && nearly(noSend, 0.0)
            && nearly(g2, 0.4) && r.auxSends().size() == 1) {
            pass("G8 aux send gain = sendLevel * bus chain, with upsert");
        } else {
            fail("G8 aux send", QStringLiteral("g=%1 missing=%2 noSend=%3 g2=%4 count=%5")
                    .arg(g).arg(missing).arg(noSend).arg(g2).arg(r.auxSends().size()));
        }
    }

    // --- G9: removeBus 波及 (出力先/トラック割当を master 復帰 + auxSend 除去) -
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        const int idB = r.addBus(QStringLiteral("Bus B"));
        // busA を busB へサブミックスする構成を JSON 経由で作る。
        QJsonObject obj = r.toJson();
        QJsonArray busArr = obj.value(QStringLiteral("buses")).toArray();
        for (int i = 0; i < busArr.size(); ++i) {
            QJsonObject b = busArr.at(i).toObject();
            if (b.value(QStringLiteral("id")).toInt() == idA) {
                b.insert(QStringLiteral("outputBusId"), idB);
                busArr.replace(i, b);
            }
        }
        obj.insert(QStringLiteral("buses"), busArr);
        r.fromJson(obj);
        r.assignTrackToBus(0, idB);     // トラック 0 は busB へ
        AuxSend send;
        send.trackIndex = 1;
        send.busId = idB;               // aux send は busB へ
        send.sendLevel = 1.0;
        r.addAuxSend(send);

        const bool removed = r.removeBus(idB);
        const bool notFound = r.removeBus(9999);
        const AudioBus* a = r.bus(idA);
        const bool ok = removed
            && !notFound
            && r.bus(idB) == nullptr            // busB が消えた
            && a != nullptr
            && a->outputBusId == -1             // busA は master 直結に復帰
            && r.trackBus(0) == -1              // トラック割当も master 復帰
            && r.auxSends().isEmpty();          // busB 宛 auxSend は除去
        if (ok) {
            pass("G9 removeBus reroutes dependents to master and drops aux sends");
        } else {
            fail("G9 removeBus", QStringLiteral("removed=%1 outA=%2 track0=%3 aux=%4")
                    .arg(removed)
                    .arg(a ? a->outputBusId : -999)
                    .arg(r.trackBus(0))
                    .arg(r.auxSends().size()));
        }
    }

    // --- G10: hasCycle 検出 + 循環時 resolve* が無限ループしない --------------
    {
        AudioBusRouting normal;
        const int n1 = normal.addBus(QStringLiteral("N1"));
        const int n2 = normal.addBus(QStringLiteral("N2"));
        Q_UNUSED(n1);
        Q_UNUSED(n2);
        const bool normalNoCycle = !normal.hasCycle();

        // busA→busB→busA の閉路を JSON 経由で構築する。
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        const int idB = r.addBus(QStringLiteral("Bus B"));
        QJsonObject obj = r.toJson();
        QJsonArray busArr = obj.value(QStringLiteral("buses")).toArray();
        for (int i = 0; i < busArr.size(); ++i) {
            QJsonObject b = busArr.at(i).toObject();
            const int id = b.value(QStringLiteral("id")).toInt();
            if (id == idA) {
                b.insert(QStringLiteral("outputBusId"), idB);   // A→B
            } else if (id == idB) {
                b.insert(QStringLiteral("outputBusId"), idA);   // B→A (閉路)
            }
            busArr.replace(i, b);
        }
        obj.insert(QStringLiteral("buses"), busArr);
        r.fromJson(obj);
        const bool cycleDetected = r.hasCycle();
        r.assignTrackToBus(0, idA);
        // 無限ループしないことの確認: 有限時間で返れば合格 (戻り値自体は不問)。
        const double g = r.resolveTrackToMasterGain(0);
        Q_UNUSED(g);

        if (normalNoCycle && cycleDetected) {
            pass("G10 hasCycle detects A->B->A and resolve terminates");
        } else {
            fail("G10 cycle", QStringLiteral("normalNoCycle=%1 cycleDetected=%2")
                    .arg(normalNoCycle).arg(cycleDetected));
        }
    }

    // --- G11: setBusGain / setBusMute / setBusSolo の反映 ---------------------
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        const bool gNeg = r.setBusGain(idA, -0.5);   // 負値は false
        const bool gOk = r.setBusGain(idA, 0.3);
        const bool mOk = r.setBusMute(idA, true);
        const bool sOk = r.setBusSolo(idA, true);
        const bool missGain = r.setBusGain(9999, 0.5);  // 不在 id は false
        const AudioBus* a = r.bus(idA);
        const bool ok = !gNeg
            && gOk && mOk && sOk
            && !missGain
            && a != nullptr
            && nearly(a->gain, 0.3)
            && a->mute
            && a->solo;
        if (ok) {
            pass("G11 setBusGain/Mute/Solo reflect on bus state");
        } else {
            fail("G11 setters", QStringLiteral("gNeg=%1 gOk=%2 missGain=%3 gain=%4")
                    .arg(gNeg).arg(gOk).arg(missGain)
                    .arg(a ? a->gain : -999));
        }
    }

    // --- G12: toJson / fromJson round-trip -----------------------------------
    {
        AudioBusRouting r;
        const int idA = r.addBus(QStringLiteral("Bus A"));
        const int idB = r.addBus(QStringLiteral("Bus B"));
        r.setBusGain(idA, 0.6);
        r.setBusMute(idB, true);
        r.assignTrackToBus(0, idA);
        r.assignTrackToBus(2, idB);
        AuxSend send;
        send.trackIndex = 1;
        send.busId = idB;
        send.sendLevel = 0.7;
        send.preFader = true;
        r.addAuxSend(send);

        const QJsonObject obj = r.toJson();
        AudioBusRouting restored;
        restored.fromJson(obj);

        const AudioBus* a = restored.bus(idA);
        const AudioBus* b = restored.bus(idB);
        const bool ok = restored.buses().size() == 2
            && restored.auxSends().size() == 1
            && a != nullptr && nearly(a->gain, 0.6)
            && b != nullptr && b->mute
            && restored.trackBus(0) == idA
            && restored.trackBus(2) == idB
            && restored.auxSends().first().trackIndex == 1
            && restored.auxSends().first().busId == idB
            && nearly(restored.auxSends().first().sendLevel, 0.7)
            && restored.auxSends().first().preFader == true
            // resolve も round-trip 後に一致する。
            && nearly(restored.resolveTrackToMasterGain(0), r.resolveTrackToMasterGain(0));
        if (ok) {
            pass("G12 toJson/fromJson round-trips buses, aux sends and assignments");
        } else {
            fail("G12 json round-trip", QStringLiteral("buses=%1 aux=%2 track0=%3 track2=%4")
                    .arg(restored.buses().size())
                    .arg(restored.auxSends().size())
                    .arg(restored.trackBus(0))
                    .arg(restored.trackBus(2)));
        }
    }

    qInfo().noquote().nospace() << "[audio-bus] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
