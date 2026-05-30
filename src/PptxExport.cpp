// PptxExport: PowerPoint (.pptx) 書き出しの純粋エンジン実装。
// パート構成・ZIP 規約は src/PptxExport.h を参照。

#include "PptxExport.h"

#include <array>

#include <QXmlStreamWriter>
#include <QBuffer>
#include <QList>

namespace pptxexport {

namespace {

// ============================================================================
// OOXML パートの XML 生成
//   ユーザーテキストは必ず QXmlStreamWriter::writeCharacters で書き出し、
//   '<' '&' '>' '"' の自動エスケープに任せる (手書き連結で埋め込まない)。
// ============================================================================

// QXmlStreamWriter を XML 宣言 + 自動整形なしで初期化するヘルパ。
struct XmlBuilder {
    QByteArray  data;
    QBuffer     buffer;
    QXmlStreamWriter w;

    XmlBuilder() : buffer(&data) {
        buffer.open(QIODevice::WriteOnly);
        w.setDevice(&buffer);
        w.setAutoFormatting(false);
        w.writeStartDocument(QStringLiteral("1.0"));
    }
    QByteArray finish() {
        w.writeEndDocument();
        buffer.close();
        return data;
    }
};

// "[Content_Types].xml"
QByteArray buildContentTypes(int slideCount, int imageCount)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("Types"));
    w.writeDefaultNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/content-types"));

    auto def = [&](const QString& ext, const QString& type) {
        w.writeStartElement(QStringLiteral("Default"));
        w.writeAttribute(QStringLiteral("Extension"), ext);
        w.writeAttribute(QStringLiteral("ContentType"), type);
        w.writeEndElement();
    };
    auto over = [&](const QString& part, const QString& type) {
        w.writeStartElement(QStringLiteral("Override"));
        w.writeAttribute(QStringLiteral("PartName"), part);
        w.writeAttribute(QStringLiteral("ContentType"), type);
        w.writeEndElement();
    };

    def(QStringLiteral("rels"),
        QStringLiteral("application/vnd.openxmlformats-package.relationships+xml"));
    def(QStringLiteral("xml"), QStringLiteral("application/xml"));

    over(QStringLiteral("/ppt/presentation.xml"),
         QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"));
    over(QStringLiteral("/ppt/presProps.xml"),
         QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.presProps+xml"));
    over(QStringLiteral("/ppt/slideMasters/slideMaster1.xml"),
         QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml"));
    over(QStringLiteral("/ppt/slideLayouts/slideLayout1.xml"),
         QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"));
    over(QStringLiteral("/ppt/theme/theme1.xml"),
         QStringLiteral("application/vnd.openxmlformats-officedocument.theme+xml"));
    for (int i = 1; i <= slideCount; ++i) {
        over(QStringLiteral("/ppt/slides/slide%1.xml").arg(i),
             QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.slide+xml"));
    }
    for (int i = 1; i <= imageCount; ++i) {
        over(QStringLiteral("/ppt/media/image%1.png").arg(i),
             QStringLiteral("image/png"));
    }
    over(QStringLiteral("/docProps/core.xml"),
         QStringLiteral("application/vnd.openxmlformats-package.core-properties+xml"));
    over(QStringLiteral("/docProps/app.xml"),
         QStringLiteral("application/vnd.openxmlformats-officedocument.extended-properties+xml"));

    w.writeEndElement(); // Types
    return b.finish();
}

// "_rels/.rels"
QByteArray buildRootRels()
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("Relationships"));
    w.writeDefaultNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships"));

    auto rel = [&](const QString& id, const QString& type, const QString& target) {
        w.writeStartElement(QStringLiteral("Relationship"));
        w.writeAttribute(QStringLiteral("Id"), id);
        w.writeAttribute(QStringLiteral("Type"), type);
        w.writeAttribute(QStringLiteral("Target"), target);
        w.writeEndElement();
    };
    rel(QStringLiteral("rId1"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"),
        QStringLiteral("ppt/presentation.xml"));
    rel(QStringLiteral("rId2"),
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties"),
        QStringLiteral("docProps/core.xml"));
    rel(QStringLiteral("rId3"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties"),
        QStringLiteral("docProps/app.xml"));

    w.writeEndElement(); // Relationships
    return b.finish();
}

// "docProps/core.xml"
QByteArray buildCoreProps(const Deck& deck)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("cp:coreProperties"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/metadata/core-properties"),
        QStringLiteral("cp"));
    w.writeNamespace(QStringLiteral("http://purl.org/dc/elements/1.1/"), QStringLiteral("dc"));
    w.writeNamespace(QStringLiteral("http://purl.org/dc/terms/"), QStringLiteral("dcterms"));
    w.writeNamespace(QStringLiteral("http://www.w3.org/2001/XMLSchema-instance"),
                     QStringLiteral("xsi"));

    w.writeTextElement(QStringLiteral("http://purl.org/dc/elements/1.1/"),
                       QStringLiteral("title"), deck.title);
    w.writeTextElement(QStringLiteral("http://purl.org/dc/elements/1.1/"),
                       QStringLiteral("creator"), deck.author);

    w.writeEndElement(); // cp:coreProperties
    return b.finish();
}

// "docProps/app.xml"
QByteArray buildAppProps(int slideCount)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("Properties"));
    w.writeDefaultNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes"),
        QStringLiteral("vt"));

    w.writeTextElement(QStringLiteral("Application"), QStringLiteral("v-simple-editor"));
    w.writeTextElement(QStringLiteral("Slides"), QString::number(slideCount));

    w.writeEndElement(); // Properties
    return b.finish();
}

// "ppt/presentation.xml"
//   16:9 ページ (sldSz=12192000x6858000)。各 slide id は 256+i。
QByteArray buildPresentation(int slideCount)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("p:presentation"));
    w.writeNamespace(QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/main"),
                     QStringLiteral("a"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("r"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/presentationml/2006/main"),
        QStringLiteral("p"));

    // スライドマスター一覧。
    w.writeStartElement(QStringLiteral("p:sldMasterIdLst"));
    w.writeStartElement(QStringLiteral("p:sldMasterId"));
    w.writeAttribute(QStringLiteral("id"), QStringLiteral("2147483648"));
    w.writeAttribute(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("id"), QStringLiteral("rId1"));
    w.writeEndElement(); // p:sldMasterId
    w.writeEndElement(); // p:sldMasterIdLst

    // スライド一覧 (rId は presentation.xml.rels の slideN を指す: rId が後述の slideRelBase からずれる)。
    // rels では rId1=master, rId2=theme, rId3=presProps, rId4..=slideN とする。
    w.writeStartElement(QStringLiteral("p:sldIdLst"));
    for (int i = 1; i <= slideCount; ++i) {
        w.writeStartElement(QStringLiteral("p:sldId"));
        w.writeAttribute(QStringLiteral("id"), QString::number(256 + (i - 1)));
        w.writeAttribute(
            QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
            QStringLiteral("id"), QStringLiteral("rId%1").arg(3 + i));
        w.writeEndElement(); // p:sldId
    }
    w.writeEndElement(); // p:sldIdLst

    // ページサイズ (16:9) とノートサイズ。
    w.writeStartElement(QStringLiteral("p:sldSz"));
    w.writeAttribute(QStringLiteral("cx"), QStringLiteral("12192000"));
    w.writeAttribute(QStringLiteral("cy"), QStringLiteral("6858000"));
    w.writeEndElement(); // p:sldSz
    w.writeStartElement(QStringLiteral("p:notesSz"));
    w.writeAttribute(QStringLiteral("cx"), QStringLiteral("6858000"));
    w.writeAttribute(QStringLiteral("cy"), QStringLiteral("9144000"));
    w.writeEndElement(); // p:notesSz

    w.writeEndElement(); // p:presentation
    return b.finish();
}

// "ppt/_rels/presentation.xml.rels"
//   rId1=master, rId2=theme, rId3=presProps, rId(3+i)=slideN。
QByteArray buildPresentationRels(int slideCount)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("Relationships"));
    w.writeDefaultNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships"));

    auto rel = [&](const QString& id, const QString& type, const QString& target) {
        w.writeStartElement(QStringLiteral("Relationship"));
        w.writeAttribute(QStringLiteral("Id"), id);
        w.writeAttribute(QStringLiteral("Type"), type);
        w.writeAttribute(QStringLiteral("Target"), target);
        w.writeEndElement();
    };
    rel(QStringLiteral("rId1"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster"),
        QStringLiteral("slideMasters/slideMaster1.xml"));
    rel(QStringLiteral("rId2"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme"),
        QStringLiteral("theme/theme1.xml"));
    rel(QStringLiteral("rId3"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/presProps"),
        QStringLiteral("presProps.xml"));
    for (int i = 1; i <= slideCount; ++i) {
        rel(QStringLiteral("rId%1").arg(3 + i),
            QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide"),
            QStringLiteral("slides/slide%1.xml").arg(i));
    }

    w.writeEndElement(); // Relationships
    return b.finish();
}

// "ppt/presProps.xml"
QByteArray buildPresProps()
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("p:presentationPr"));
    w.writeNamespace(QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/main"),
                     QStringLiteral("a"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("r"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/presentationml/2006/main"),
        QStringLiteral("p"));
    w.writeEndElement(); // p:presentationPr
    return b.finish();
}

// 配色スキームの 1 色 (a:srgbClr) を書くヘルパ。
void writeSysOrSrgb(QXmlStreamWriter& w, const QString& tag, const QString& hex)
{
    w.writeStartElement(tag);
    w.writeStartElement(QStringLiteral("a:srgbClr"));
    w.writeAttribute(QStringLiteral("val"), hex);
    w.writeEndElement();
    w.writeEndElement();
}

// "ppt/theme/theme1.xml"
//   最小だが妥当な a:theme (clrScheme / fontScheme / fmtScheme を持つ)。
QByteArray buildTheme()
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    const QString aNs = QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/main");

    w.writeStartElement(QStringLiteral("a:theme"));
    w.writeNamespace(aNs, QStringLiteral("a"));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("Office Theme"));

    w.writeStartElement(QStringLiteral("a:themeElements"));

    // --- clrScheme ---
    w.writeStartElement(QStringLiteral("a:clrScheme"));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("Office"));
    // dk1/lt1 は sysClr、それ以外は srgbClr。
    w.writeStartElement(QStringLiteral("a:dk1"));
    w.writeStartElement(QStringLiteral("a:sysClr"));
    w.writeAttribute(QStringLiteral("val"), QStringLiteral("windowText"));
    w.writeAttribute(QStringLiteral("lastClr"), QStringLiteral("000000"));
    w.writeEndElement();
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:lt1"));
    w.writeStartElement(QStringLiteral("a:sysClr"));
    w.writeAttribute(QStringLiteral("val"), QStringLiteral("window"));
    w.writeAttribute(QStringLiteral("lastClr"), QStringLiteral("FFFFFF"));
    w.writeEndElement();
    w.writeEndElement();
    writeSysOrSrgb(w, QStringLiteral("a:dk2"), QStringLiteral("44546A"));
    writeSysOrSrgb(w, QStringLiteral("a:lt2"), QStringLiteral("E7E6E6"));
    writeSysOrSrgb(w, QStringLiteral("a:accent1"), QStringLiteral("4472C4"));
    writeSysOrSrgb(w, QStringLiteral("a:accent2"), QStringLiteral("ED7D31"));
    writeSysOrSrgb(w, QStringLiteral("a:accent3"), QStringLiteral("A5A5A5"));
    writeSysOrSrgb(w, QStringLiteral("a:accent4"), QStringLiteral("FFC000"));
    writeSysOrSrgb(w, QStringLiteral("a:accent5"), QStringLiteral("5B9BD5"));
    writeSysOrSrgb(w, QStringLiteral("a:accent6"), QStringLiteral("70AD47"));
    writeSysOrSrgb(w, QStringLiteral("a:hlink"), QStringLiteral("0563C1"));
    writeSysOrSrgb(w, QStringLiteral("a:folHlink"), QStringLiteral("954F72"));
    w.writeEndElement(); // a:clrScheme

    // --- fontScheme ---
    w.writeStartElement(QStringLiteral("a:fontScheme"));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("Office"));
    auto fontBlock = [&](const QString& tag) {
        w.writeStartElement(tag);
        w.writeStartElement(QStringLiteral("a:latin"));
        w.writeAttribute(QStringLiteral("typeface"), QStringLiteral("Calibri"));
        w.writeEndElement();
        w.writeStartElement(QStringLiteral("a:ea"));
        w.writeAttribute(QStringLiteral("typeface"), QString());
        w.writeEndElement();
        w.writeStartElement(QStringLiteral("a:cs"));
        w.writeAttribute(QStringLiteral("typeface"), QString());
        w.writeEndElement();
        w.writeEndElement();
    };
    fontBlock(QStringLiteral("a:majorFont"));
    fontBlock(QStringLiteral("a:minorFont"));
    w.writeEndElement(); // a:fontScheme

    // --- fmtScheme ---
    w.writeStartElement(QStringLiteral("a:fmtScheme"));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("Office"));

    // fillStyleLst: 単色 + 単色 + 単色 (最小)。
    w.writeStartElement(QStringLiteral("a:fillStyleLst"));
    for (int k = 0; k < 3; ++k) {
        w.writeStartElement(QStringLiteral("a:solidFill"));
        w.writeStartElement(QStringLiteral("a:schemeClr"));
        w.writeAttribute(QStringLiteral("val"), QStringLiteral("phClr"));
        w.writeEndElement();
        w.writeEndElement();
    }
    w.writeEndElement(); // a:fillStyleLst

    // lnStyleLst: 3 本の単色線。
    w.writeStartElement(QStringLiteral("a:lnStyleLst"));
    for (int k = 0; k < 3; ++k) {
        w.writeStartElement(QStringLiteral("a:ln"));
        w.writeAttribute(QStringLiteral("w"), QStringLiteral("6350"));
        w.writeAttribute(QStringLiteral("cap"), QStringLiteral("flat"));
        w.writeAttribute(QStringLiteral("cmpd"), QStringLiteral("sng"));
        w.writeAttribute(QStringLiteral("algn"), QStringLiteral("ctr"));
        w.writeStartElement(QStringLiteral("a:solidFill"));
        w.writeStartElement(QStringLiteral("a:schemeClr"));
        w.writeAttribute(QStringLiteral("val"), QStringLiteral("phClr"));
        w.writeEndElement();
        w.writeEndElement();
        w.writeStartElement(QStringLiteral("a:prstDash"));
        w.writeAttribute(QStringLiteral("val"), QStringLiteral("solid"));
        w.writeEndElement();
        w.writeEndElement(); // a:ln
    }
    w.writeEndElement(); // a:lnStyleLst

    // effectStyleLst: 3 つの空エフェクト。
    w.writeStartElement(QStringLiteral("a:effectStyleLst"));
    for (int k = 0; k < 3; ++k) {
        w.writeStartElement(QStringLiteral("a:effectStyle"));
        w.writeStartElement(QStringLiteral("a:effectLst"));
        w.writeEndElement();
        w.writeEndElement();
    }
    w.writeEndElement(); // a:effectStyleLst

    // bgFillStyleLst: 3 つの単色。
    w.writeStartElement(QStringLiteral("a:bgFillStyleLst"));
    for (int k = 0; k < 3; ++k) {
        w.writeStartElement(QStringLiteral("a:solidFill"));
        w.writeStartElement(QStringLiteral("a:schemeClr"));
        w.writeAttribute(QStringLiteral("val"), QStringLiteral("phClr"));
        w.writeEndElement();
        w.writeEndElement();
    }
    w.writeEndElement(); // a:bgFillStyleLst

    w.writeEndElement(); // a:fmtScheme

    w.writeEndElement(); // a:themeElements
    w.writeEndElement(); // a:theme
    return b.finish();
}

// "ppt/slideMasters/slideMaster1.xml"
QByteArray buildSlideMaster()
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("p:sldMaster"));
    w.writeNamespace(QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/main"),
                     QStringLiteral("a"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("r"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/presentationml/2006/main"),
        QStringLiteral("p"));

    // 共通スライド要素 (背景なし、空の spTree)。
    w.writeStartElement(QStringLiteral("p:cSld"));
    w.writeStartElement(QStringLiteral("p:spTree"));
    w.writeStartElement(QStringLiteral("p:nvGrpSpPr"));
    w.writeStartElement(QStringLiteral("p:cNvPr"));
    w.writeAttribute(QStringLiteral("id"), QStringLiteral("1"));
    w.writeAttribute(QStringLiteral("name"), QString());
    w.writeEndElement(); // p:cNvPr
    w.writeEmptyElement(QStringLiteral("p:cNvGrpSpPr"));
    w.writeEmptyElement(QStringLiteral("p:nvPr"));
    w.writeEndElement(); // p:nvGrpSpPr
    w.writeStartElement(QStringLiteral("p:grpSpPr"));
    w.writeStartElement(QStringLiteral("a:xfrm"));
    w.writeStartElement(QStringLiteral("a:off"));
    w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:ext"));
    w.writeAttribute(QStringLiteral("cx"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("cy"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:chOff"));
    w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:chExt"));
    w.writeAttribute(QStringLiteral("cx"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("cy"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeEndElement(); // a:xfrm
    w.writeEndElement(); // p:grpSpPr
    w.writeEndElement(); // p:spTree
    w.writeEndElement(); // p:cSld

    // 配色マップ。
    w.writeStartElement(QStringLiteral("p:clrMap"));
    w.writeAttribute(QStringLiteral("bg1"), QStringLiteral("lt1"));
    w.writeAttribute(QStringLiteral("tx1"), QStringLiteral("dk1"));
    w.writeAttribute(QStringLiteral("bg2"), QStringLiteral("lt2"));
    w.writeAttribute(QStringLiteral("tx2"), QStringLiteral("dk2"));
    w.writeAttribute(QStringLiteral("accent1"), QStringLiteral("accent1"));
    w.writeAttribute(QStringLiteral("accent2"), QStringLiteral("accent2"));
    w.writeAttribute(QStringLiteral("accent3"), QStringLiteral("accent3"));
    w.writeAttribute(QStringLiteral("accent4"), QStringLiteral("accent4"));
    w.writeAttribute(QStringLiteral("accent5"), QStringLiteral("accent5"));
    w.writeAttribute(QStringLiteral("accent6"), QStringLiteral("accent6"));
    w.writeAttribute(QStringLiteral("hlink"), QStringLiteral("hlink"));
    w.writeAttribute(QStringLiteral("folHlink"), QStringLiteral("folHlink"));
    w.writeEndElement(); // p:clrMap

    // レイアウト一覧 (rId1 → slideLayout1)。
    w.writeStartElement(QStringLiteral("p:sldLayoutIdLst"));
    w.writeStartElement(QStringLiteral("p:sldLayoutId"));
    w.writeAttribute(QStringLiteral("id"), QStringLiteral("2147483649"));
    w.writeAttribute(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("id"), QStringLiteral("rId1"));
    w.writeEndElement(); // p:sldLayoutId
    w.writeEndElement(); // p:sldLayoutIdLst

    w.writeEndElement(); // p:sldMaster
    return b.finish();
}

// "ppt/slideMasters/_rels/slideMaster1.xml.rels" (→ slideLayout1, theme1)
QByteArray buildSlideMasterRels()
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("Relationships"));
    w.writeDefaultNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships"));

    auto rel = [&](const QString& id, const QString& type, const QString& target) {
        w.writeStartElement(QStringLiteral("Relationship"));
        w.writeAttribute(QStringLiteral("Id"), id);
        w.writeAttribute(QStringLiteral("Type"), type);
        w.writeAttribute(QStringLiteral("Target"), target);
        w.writeEndElement();
    };
    rel(QStringLiteral("rId1"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout"),
        QStringLiteral("../slideLayouts/slideLayout1.xml"));
    rel(QStringLiteral("rId2"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme"),
        QStringLiteral("../theme/theme1.xml"));

    w.writeEndElement(); // Relationships
    return b.finish();
}

// "ppt/slideLayouts/slideLayout1.xml"
//   type="blank" の最小レイアウト。
QByteArray buildSlideLayout()
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("p:sldLayout"));
    w.writeNamespace(QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/main"),
                     QStringLiteral("a"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("r"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/presentationml/2006/main"),
        QStringLiteral("p"));
    w.writeAttribute(QStringLiteral("type"), QStringLiteral("blank"));
    w.writeAttribute(QStringLiteral("preserve"), QStringLiteral("1"));

    w.writeStartElement(QStringLiteral("p:cSld"));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("Blank"));
    w.writeStartElement(QStringLiteral("p:spTree"));
    w.writeStartElement(QStringLiteral("p:nvGrpSpPr"));
    w.writeStartElement(QStringLiteral("p:cNvPr"));
    w.writeAttribute(QStringLiteral("id"), QStringLiteral("1"));
    w.writeAttribute(QStringLiteral("name"), QString());
    w.writeEndElement(); // p:cNvPr
    w.writeEmptyElement(QStringLiteral("p:cNvGrpSpPr"));
    w.writeEmptyElement(QStringLiteral("p:nvPr"));
    w.writeEndElement(); // p:nvGrpSpPr
    w.writeStartElement(QStringLiteral("p:grpSpPr"));
    w.writeStartElement(QStringLiteral("a:xfrm"));
    w.writeStartElement(QStringLiteral("a:off"));
    w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:ext"));
    w.writeAttribute(QStringLiteral("cx"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("cy"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:chOff"));
    w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:chExt"));
    w.writeAttribute(QStringLiteral("cx"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("cy"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeEndElement(); // a:xfrm
    w.writeEndElement(); // p:grpSpPr
    w.writeEndElement(); // p:spTree
    w.writeEndElement(); // p:cSld

    w.writeStartElement(QStringLiteral("p:clrMapOvr"));
    w.writeEmptyElement(QStringLiteral("a:masterClrMapping"));
    w.writeEndElement(); // p:clrMapOvr

    w.writeEndElement(); // p:sldLayout
    return b.finish();
}

// "ppt/slideLayouts/_rels/slideLayout1.xml.rels" (→ slideMaster1)
QByteArray buildSlideLayoutRels()
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("Relationships"));
    w.writeDefaultNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships"));
    w.writeStartElement(QStringLiteral("Relationship"));
    w.writeAttribute(QStringLiteral("Id"), QStringLiteral("rId1"));
    w.writeAttribute(QStringLiteral("Type"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster"));
    w.writeAttribute(QStringLiteral("Target"),
        QStringLiteral("../slideMasters/slideMaster1.xml"));
    w.writeEndElement(); // Relationship
    w.writeEndElement(); // Relationships
    return b.finish();
}

// テキストプレースホルダ用 sp を 1 つ書く共通ヘルパ。
//   phType  : "title" / "body" (空なら ph type 属性を出さない)
//   spId    : 図形 ID
//   off/ext : 配置 (EMU)
//   paras   : 段落本文 (各要素 1 a:p)。空なら空段落 1 つ。
void writeTextSp(QXmlStreamWriter& w,
                 const QString& phType,
                 int spId,
                 const QString& name,
                 qint64 offX, qint64 offY, qint64 extCx, qint64 extCy,
                 const QStringList& paras)
{
    w.writeStartElement(QStringLiteral("p:sp"));

    // 非ビジュアルプロパティ。
    w.writeStartElement(QStringLiteral("p:nvSpPr"));
    w.writeStartElement(QStringLiteral("p:cNvPr"));
    w.writeAttribute(QStringLiteral("id"), QString::number(spId));
    w.writeAttribute(QStringLiteral("name"), name);
    w.writeEndElement(); // p:cNvPr
    w.writeStartElement(QStringLiteral("p:cNvSpPr"));
    // a:spLocks に noGrp を付ける (グループ化ロック)。
    w.writeStartElement(QStringLiteral("a:spLocks"));
    w.writeAttribute(QStringLiteral("noGrp"), QStringLiteral("1"));
    w.writeEndElement(); // a:spLocks
    w.writeEndElement(); // p:cNvSpPr
    w.writeStartElement(QStringLiteral("p:nvPr"));
    if (!phType.isEmpty()) {
        w.writeStartElement(QStringLiteral("p:ph"));
        w.writeAttribute(QStringLiteral("type"), phType);
        w.writeEndElement(); // p:ph
    }
    w.writeEndElement(); // p:nvPr
    w.writeEndElement(); // p:nvSpPr

    // 図形プロパティ (明示配置)。
    w.writeStartElement(QStringLiteral("p:spPr"));
    w.writeStartElement(QStringLiteral("a:xfrm"));
    w.writeStartElement(QStringLiteral("a:off"));
    w.writeAttribute(QStringLiteral("x"), QString::number(offX));
    w.writeAttribute(QStringLiteral("y"), QString::number(offY));
    w.writeEndElement(); // a:off
    w.writeStartElement(QStringLiteral("a:ext"));
    w.writeAttribute(QStringLiteral("cx"), QString::number(extCx));
    w.writeAttribute(QStringLiteral("cy"), QString::number(extCy));
    w.writeEndElement(); // a:ext
    w.writeEndElement(); // a:xfrm
    w.writeEndElement(); // p:spPr

    // テキスト本体。
    w.writeStartElement(QStringLiteral("p:txBody"));
    w.writeEmptyElement(QStringLiteral("a:bodyPr"));
    w.writeEmptyElement(QStringLiteral("a:lstStyle"));
    if (paras.isEmpty()) {
        w.writeEmptyElement(QStringLiteral("a:p"));
    } else {
        for (const QString& line : paras) {
            w.writeStartElement(QStringLiteral("a:p"));
            w.writeStartElement(QStringLiteral("a:r"));
            w.writeEmptyElement(QStringLiteral("a:rPr"));
            // ユーザーテキストは writeTextElement で自動エスケープ。
            w.writeStartElement(QStringLiteral("a:t"));
            w.writeCharacters(line);
            w.writeEndElement(); // a:t
            w.writeEndElement(); // a:r
            w.writeEndElement(); // a:p
        }
    }
    w.writeEndElement(); // p:txBody

    w.writeEndElement(); // p:sp
}

// 画像 p:pic を 1 つ書く (本文領域に固定配置)。
void writePic(QXmlStreamWriter& w, int spId, const QString& embedRId,
              qint64 offX, qint64 offY, qint64 extCx, qint64 extCy)
{
    w.writeStartElement(QStringLiteral("p:pic"));

    w.writeStartElement(QStringLiteral("p:nvPicPr"));
    w.writeStartElement(QStringLiteral("p:cNvPr"));
    w.writeAttribute(QStringLiteral("id"), QString::number(spId));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("Image %1").arg(spId));
    w.writeEndElement(); // p:cNvPr
    w.writeStartElement(QStringLiteral("p:cNvPicPr"));
    w.writeEmptyElement(QStringLiteral("a:picLocks"));
    w.writeEndElement(); // p:cNvPicPr
    w.writeEmptyElement(QStringLiteral("p:nvPr"));
    w.writeEndElement(); // p:nvPicPr

    w.writeStartElement(QStringLiteral("p:blipFill"));
    w.writeStartElement(QStringLiteral("a:blip"));
    w.writeAttribute(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("embed"), embedRId);
    w.writeEndElement(); // a:blip
    w.writeStartElement(QStringLiteral("a:stretch"));
    w.writeEmptyElement(QStringLiteral("a:fillRect"));
    w.writeEndElement(); // a:stretch
    w.writeEndElement(); // p:blipFill

    w.writeStartElement(QStringLiteral("p:spPr"));
    w.writeStartElement(QStringLiteral("a:xfrm"));
    w.writeStartElement(QStringLiteral("a:off"));
    w.writeAttribute(QStringLiteral("x"), QString::number(offX));
    w.writeAttribute(QStringLiteral("y"), QString::number(offY));
    w.writeEndElement(); // a:off
    w.writeStartElement(QStringLiteral("a:ext"));
    w.writeAttribute(QStringLiteral("cx"), QString::number(extCx));
    w.writeAttribute(QStringLiteral("cy"), QString::number(extCy));
    w.writeEndElement(); // a:ext
    w.writeEndElement(); // a:xfrm
    w.writeStartElement(QStringLiteral("a:prstGeom"));
    w.writeAttribute(QStringLiteral("prst"), QStringLiteral("rect"));
    w.writeEmptyElement(QStringLiteral("a:avLst"));
    w.writeEndElement(); // a:prstGeom
    w.writeEndElement(); // p:spPr

    w.writeEndElement(); // p:pic
}

// "ppt/slides/slideN.xml"
//   タイトル sp + (bullets があれば) 本文 sp + (画像があれば) p:pic。
QByteArray buildSlide(const Slide& slide)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("p:sld"));
    w.writeNamespace(QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/main"),
                     QStringLiteral("a"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("r"));
    w.writeNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/presentationml/2006/main"),
        QStringLiteral("p"));

    w.writeStartElement(QStringLiteral("p:cSld"));
    w.writeStartElement(QStringLiteral("p:spTree"));

    // グループ非ビジュアル。
    w.writeStartElement(QStringLiteral("p:nvGrpSpPr"));
    w.writeStartElement(QStringLiteral("p:cNvPr"));
    w.writeAttribute(QStringLiteral("id"), QStringLiteral("1"));
    w.writeAttribute(QStringLiteral("name"), QString());
    w.writeEndElement(); // p:cNvPr
    w.writeEmptyElement(QStringLiteral("p:cNvGrpSpPr"));
    w.writeEmptyElement(QStringLiteral("p:nvPr"));
    w.writeEndElement(); // p:nvGrpSpPr
    w.writeStartElement(QStringLiteral("p:grpSpPr"));
    w.writeStartElement(QStringLiteral("a:xfrm"));
    w.writeStartElement(QStringLiteral("a:off"));
    w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:ext"));
    w.writeAttribute(QStringLiteral("cx"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("cy"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:chOff"));
    w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:chExt"));
    w.writeAttribute(QStringLiteral("cx"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("cy"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeEndElement(); // a:xfrm
    w.writeEndElement(); // p:grpSpPr

    int spId = 2;

    // タイトル sp (常に出す)。本文上部に配置。
    writeTextSp(w, QStringLiteral("title"), spId++, QStringLiteral("Title"),
                838200, 365125, 10515600, 1325563,
                QStringList{ slide.title });

    // 本文 sp (bullets があるときのみ)。
    if (!slide.bullets.isEmpty()) {
        writeTextSp(w, QStringLiteral("body"), spId++, QStringLiteral("Body"),
                    838200, 1825625, 10515600, 4351338,
                    slide.bullets);
    }

    // 画像 (あれば本文領域に固定配置)。embed は slide rels の rId2 とする。
    if (!slide.imagePng.isEmpty()) {
        writePic(w, spId++, QStringLiteral("rId2"),
                 2743200, 1825625, 6705600, 4351338);
    }

    w.writeEndElement(); // p:spTree
    w.writeEndElement(); // p:cSld

    // 配色マップ上書き (マスター継承)。
    w.writeStartElement(QStringLiteral("p:clrMapOvr"));
    w.writeEmptyElement(QStringLiteral("a:masterClrMapping"));
    w.writeEndElement(); // p:clrMapOvr

    w.writeEndElement(); // p:sld
    return b.finish();
}

// "ppt/slides/_rels/slideN.xml.rels"
//   rId1 → slideLayout1。画像があれば rId2 → ../media/imageK.png。
QByteArray buildSlideRels(bool hasImage, int imageIndex)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("Relationships"));
    w.writeDefaultNamespace(
        QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships"));

    w.writeStartElement(QStringLiteral("Relationship"));
    w.writeAttribute(QStringLiteral("Id"), QStringLiteral("rId1"));
    w.writeAttribute(QStringLiteral("Type"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout"));
    w.writeAttribute(QStringLiteral("Target"),
        QStringLiteral("../slideLayouts/slideLayout1.xml"));
    w.writeEndElement(); // Relationship

    if (hasImage) {
        w.writeStartElement(QStringLiteral("Relationship"));
        w.writeAttribute(QStringLiteral("Id"), QStringLiteral("rId2"));
        w.writeAttribute(QStringLiteral("Type"),
            QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/image"));
        w.writeAttribute(QStringLiteral("Target"),
            QStringLiteral("../media/image%1.png").arg(imageIndex));
        w.writeEndElement(); // Relationship
    }

    w.writeEndElement(); // Relationships
    return b.finish();
}

// ============================================================================
// 自前 ZIP ライタ (store / 無圧縮, method=0)
// ============================================================================

// CRC32 (標準多項式 0xEDB88320, テーブル方式)。
const std::array<quint32, 256>& crc32Table()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[n] = c;
        }
        return t;
    }();
    return table;
}

quint32 crc32Of(const QByteArray& data)
{
    const auto& table = crc32Table();
    quint32 crc = 0xFFFFFFFFu;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData());
    const qsizetype len = data.size();
    for (qsizetype i = 0; i < len; ++i)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// リトルエンディアンでバイトを末尾に追記するヘルパ群。
void putU16(QByteArray& out, quint16 v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8) & 0xFF));
}
void putU32(QByteArray& out, quint32 v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 24) & 0xFF));
}

// ZIP エントリの記録 (Central Directory 用に保持)。
struct ZipEntry {
    QByteArray name;        // パス (UTF-8)。
    QByteArray data;        // 生データ (store)。
    quint32    crc = 0;
    quint32    localOffset = 0;  // Local File Header の先頭オフセット。
};

// 固定の DOS タイムスタンプ (決定的出力)。
//   mtime=0x0000 (00:00:00), mdate=0x0021 (1980-01-01)。
constexpr quint16 kDosTime = 0x0000;
constexpr quint16 kDosDate = 0x0021;
constexpr quint64 kZipU16Max = 0xFFFFull;
constexpr quint64 kZipU32Max = 0xFFFFFFFFull;

// store ZIP を構築する。entries は (path, data) の順序付きリスト。
QByteArray buildZip(const QList<QPair<QByteArray, QByteArray>>& parts)
{
    if (static_cast<quint64>(parts.size()) > kZipU16Max) {
        return QByteArray();
    }

    QByteArray out;
    QList<ZipEntry> records;
    records.reserve(parts.size());

    // --- Local File Headers + データ ---
    for (const auto& part : parts) {
        ZipEntry e;
        e.name        = part.first;
        e.data        = part.second;
        e.crc         = crc32Of(e.data);

        const qsizetype nameSize = e.name.size();
        const qsizetype dataSize = e.data.size();
        if (nameSize < 0 || dataSize < 0
            || static_cast<quint64>(nameSize) > kZipU16Max
            || static_cast<quint64>(dataSize) > kZipU32Max
            || static_cast<quint64>(out.size()) > kZipU32Max) {
            return QByteArray();
        }

        const quint64 localEnd = static_cast<quint64>(out.size())
                               + 30ull
                               + static_cast<quint64>(nameSize)
                               + static_cast<quint64>(dataSize);
        if (localEnd > kZipU32Max) {
            return QByteArray();
        }

        e.localOffset = static_cast<quint32>(out.size());

        putU32(out, 0x04034b50u);          // local file header signature
        putU16(out, 20);                   // version needed to extract (2.0)
        putU16(out, 0);                    // general purpose bit flag
        putU16(out, 0);                    // compression method = 0 (store)
        putU16(out, kDosTime);             // last mod file time
        putU16(out, kDosDate);             // last mod file date
        putU32(out, e.crc);                // crc-32
        putU32(out, static_cast<quint32>(dataSize)); // compressed size (== uncompressed)
        putU32(out, static_cast<quint32>(dataSize)); // uncompressed size
        putU16(out, static_cast<quint16>(nameSize)); // file name length
        putU16(out, 0);                    // extra field length
        out.append(e.name);                // file name
        out.append(e.data);                // file data

        records.append(e);
    }

    // --- Central Directory ---
    if (static_cast<quint64>(out.size()) > kZipU32Max) {
        return QByteArray();
    }
    const quint32 cdStart = static_cast<quint32>(out.size());
    for (const ZipEntry& e : records) {
        const qsizetype nameSize = e.name.size();
        const qsizetype dataSize = e.data.size();
        const quint64 cdEnd = static_cast<quint64>(out.size())
                            + 46ull
                            + static_cast<quint64>(nameSize);
        if (cdEnd > kZipU32Max) {
            return QByteArray();
        }

        putU32(out, 0x02014b50u);          // central file header signature
        putU16(out, 20);                   // version made by
        putU16(out, 20);                   // version needed to extract
        putU16(out, 0);                    // general purpose bit flag
        putU16(out, 0);                    // compression method = 0
        putU16(out, kDosTime);             // last mod file time
        putU16(out, kDosDate);             // last mod file date
        putU32(out, e.crc);                // crc-32
        putU32(out, static_cast<quint32>(dataSize)); // compressed size
        putU32(out, static_cast<quint32>(dataSize)); // uncompressed size
        putU16(out, static_cast<quint16>(nameSize)); // file name length
        putU16(out, 0);                    // extra field length
        putU16(out, 0);                    // file comment length
        putU16(out, 0);                    // disk number start
        putU16(out, 0);                    // internal file attributes
        putU32(out, 0);                    // external file attributes
        putU32(out, e.localOffset);        // relative offset of local header
        out.append(e.name);                // file name
    }
    const quint64 cdSize64 = static_cast<quint64>(out.size()) - cdStart;
    if (cdSize64 > kZipU32Max) {
        return QByteArray();
    }
    const quint32 cdSize = static_cast<quint32>(cdSize64);

    // --- End Of Central Directory ---
    putU32(out, 0x06054b50u);              // EOCD signature
    putU16(out, 0);                        // number of this disk
    putU16(out, 0);                        // disk where central directory starts
    putU16(out, static_cast<quint16>(records.size()));  // CD records on this disk
    putU16(out, static_cast<quint16>(records.size()));  // total CD records
    putU32(out, cdSize);                   // size of central directory
    putU32(out, cdStart);                  // offset of central directory
    putU16(out, 0);                        // comment length

    return out;
}

} // namespace

// ============================================================================
// 公開 API
// ============================================================================

QByteArray buildPptx(const Deck& deck)
{
    // slides が空ならタイトル 1 枚を補う (常に有効なファイルを返す)。
    QVector<Slide> slides = deck.slides;
    if (slides.isEmpty()) {
        Slide s;
        s.title = deck.title.isEmpty() ? QStringLiteral("Untitled") : deck.title;
        slides.append(s);
    }
    const int slideCount = slides.size();
    int imageCount = 0;
    for (const Slide& s : slides) {
        if (!s.imagePng.isEmpty()) {
            ++imageCount;
        }
    }

    // パートを順序付きで構築。"[Content_Types].xml" を必ず先頭にする。
    QList<QPair<QByteArray, QByteArray>> parts;
    auto add = [&](const QString& path, const QByteArray& bytes) {
        parts.append(qMakePair(path.toUtf8(), bytes));
    };

    add(QStringLiteral("[Content_Types].xml"), buildContentTypes(slideCount, imageCount));
    add(QStringLiteral("_rels/.rels"), buildRootRels());
    add(QStringLiteral("docProps/core.xml"), buildCoreProps(deck));
    add(QStringLiteral("docProps/app.xml"), buildAppProps(slideCount));
    add(QStringLiteral("ppt/presentation.xml"), buildPresentation(slideCount));
    add(QStringLiteral("ppt/_rels/presentation.xml.rels"), buildPresentationRels(slideCount));
    add(QStringLiteral("ppt/presProps.xml"), buildPresProps());
    add(QStringLiteral("ppt/theme/theme1.xml"), buildTheme());
    add(QStringLiteral("ppt/slideMasters/slideMaster1.xml"), buildSlideMaster());
    add(QStringLiteral("ppt/slideMasters/_rels/slideMaster1.xml.rels"), buildSlideMasterRels());
    add(QStringLiteral("ppt/slideLayouts/slideLayout1.xml"), buildSlideLayout());
    add(QStringLiteral("ppt/slideLayouts/_rels/slideLayout1.xml.rels"), buildSlideLayoutRels());

    // 各スライド + その rels + (画像があれば) media。
    int imageCounter = 0;
    for (int i = 0; i < slideCount; ++i) {
        const Slide& s = slides.at(i);
        const int n = i + 1;
        const bool hasImage = !s.imagePng.isEmpty();
        int imageIndex = 0;
        if (hasImage) {
            imageIndex = ++imageCounter;
            add(QStringLiteral("ppt/media/image%1.png").arg(imageIndex), s.imagePng);
        }
        add(QStringLiteral("ppt/slides/slide%1.xml").arg(n), buildSlide(s));
        add(QStringLiteral("ppt/slides/_rels/slide%1.xml.rels").arg(n),
            buildSlideRels(hasImage, imageIndex));
    }

    return buildZip(parts);
}

} // namespace pptxexport
