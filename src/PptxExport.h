#pragma once
// PptxExport: PowerPoint (.pptx) 書き出しの純粋エンジン。
//
// QObject / QWidget / QApplication を一切使わない純粋・決定的なロジックのみ。
// headless (QApplication 不要) でテスト可能。UI 層はこのエンジンを呼び出すだけにする。
//
// 前提・規約:
//   - 外部依存ゼロ。zlib も使わない。PPTX は無圧縮 store ZIP (method=0) でも
//     PowerPoint / LibreOffice / Google スライドが受理する。
//   - QtCore のみ使用 (QByteArray, QString, QStringList, QXmlStreamWriter, QBuffer 等)。
//   - 生成は決定的: タイムスタンプは固定定数を使い、Date.now() 等は使わない
//     (同じ入力からは常にバイト同一の .pptx を返す)。
//
// パート構成 (PowerPoint が開ける最小 OOXML 構成):
//   [Content_Types].xml                         … 既定/上書きの MIME 宣言
//   _rels/.rels                                 … パッケージルートの関係
//   docProps/core.xml, docProps/app.xml         … コア/拡張プロパティ
//   ppt/presentation.xml (+ _rels)              … スライドマスター/スライド一覧, ページサイズ
//   ppt/presProps.xml                           … プレゼン設定 (空)
//   ppt/theme/theme1.xml                        … 配色/フォント/書式スキーム (PowerPoint は必須)
//   ppt/slideMasters/slideMaster1.xml (+ _rels) … スライドマスター
//   ppt/slideLayouts/slideLayout1.xml (+ _rels) … スライドレイアウト
//   ppt/slides/slideN.xml (+ _rels)             … 各スライド本体 (N=1..)
//   ppt/media/imageK.png                        … 画像スライドのみ
//
// ZIP 規約:
//   - すべて store (無圧縮, method=0)。Local File Header + Central Directory + EOCD。
//   - マルチバイトはリトルエンディアン。crc32 は標準多項式 0xEDB88320 のテーブル方式を自前実装。
//   - "[Content_Types].xml" を必ず先頭エントリにする (一部リーダの要件)。

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pptxexport {

// 1 枚のスライド。
// imagePng が空でなければ本文領域に画像を 1 枚配置する (将来のストーリーボード用)。
// 空ならテキスト (title + bullets) のみ。
struct Slide {
    QString    title;       // タイトルプレースホルダに入る文字列。
    QStringList bullets;    // 本文プレースホルダに各行 (a:p) として入る箇条書き。
    QString    notes;       // ノート (現状はモデル保持のみ; 本文 XML には未配置)。
    QByteArray imagePng;    // PNG バイト列。空なら画像なし。
};

// 1 つのプレゼンテーション (デッキ)。
struct Deck {
    QString        title;   // dc:title。
    QString        author;  // dc:creator。
    QVector<Slide> slides;  // 0 枚でも有効 (タイトル 1 枚で出力)。
};

// 完全な .pptx バイト列 (store ZIP) を返す。
// slides が空でもタイトル 1 枚を補い、PowerPoint が開ける有効なファイルを返す。
// 通常入力の戻り値は PK\x03\x04 (0x50 0x4B 0x03 0x04) で始まる。
// ZIP32 のサイズ/エントリ数上限を超える入力では空を返す。
QByteArray buildPptx(const Deck& deck);

} // namespace pptxexport
