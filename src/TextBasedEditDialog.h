#pragma once

// TextBasedEditDialog: 文字起こし駆動編集 (Premiere テキストベース編集 / Descript 相当) の UI。
//
// トランスクリプト (caption::Clip 列) を行単位で一覧表示し、検索でヒット行を強調、
// 各行を「削除対象」にトグルできる。確定すると textedit::deletionRanges で計算した
// 削除区間列を applyDeletions シグナルで MainWindow に渡し、タイムラインへリップル削除を適用する。
//
// ロジックは純粋エンジン textedit:: に委譲し、このダイアログは表示と選択管理のみ担う。

#include <QDialog>
#include <QList>
#include <QSet>
#include <QVector>

#include "CaptionTrack.h"
#include "TextBasedEdit.h"

class QLineEdit;
class QListWidget;
class QLabel;
class QPushButton;
class QDialogButtonBox;

class TextBasedEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TextBasedEditDialog(QWidget* parent = nullptr);

    // MainWindow から現在の文字起こし結果 (字幕トラック) を渡す。
    void setTranscript(const QList<caption::Clip>& transcript);

    // 現在の削除対象 index 集合。
    QSet<int> deletedIndices() const { return m_deletedIndices; }

    // 削除対象 index から計算した昇順マージ済み削除区間列。
    QVector<textedit::TimeRange> deletionRanges() const;

signals:
    // 「タイムラインに適用 (リップル削除)」押下時に emit。MainWindow が Timeline へ適用する。
    void applyDeletions(const QVector<textedit::TimeRange>& ranges);

private slots:
    // 検索ボックスの変更でヒット行を強調する。
    void onSearchTextChanged(const QString& query);
    // 行のチェック状態変更を m_deletedIndices に反映する。
    void onItemChanged();
    // 「タイムラインに適用」押下。deletionRanges を emit して accept する。
    void onApply();

private:
    void rebuildList();
    void updateSummary();
    QString rowLabel(int index, const caption::Clip& clip) const;

    QList<caption::Clip> m_transcript;
    QSet<int>            m_deletedIndices;
    bool                 m_suppressItemChanged = false;

    QLineEdit*        m_searchEdit  = nullptr;
    QListWidget*      m_listWidget  = nullptr;
    QLabel*           m_summaryLabel = nullptr;
    QPushButton*      m_applyButton = nullptr;
    QDialogButtonBox* m_buttonBox   = nullptr;
};
