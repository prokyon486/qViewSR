// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QSvgRenderer>
#include "vector/edit_document.h"
#include <limits>

using namespace Vector;
namespace {
const QByteArray Example = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="160" viewBox="0 0 200 160"><rect id="background" width="200" height="160" fill="white"/><g id="fill"><path id="area" fill="#abcdef" fill-rule="evenodd" transform="translate(10,20)" d="M0 0L100 0L100 100L0 100Z M30 30L70 30L70 70L30 70Z"/><path id="top" fill="#112233" d="M100 10L130 10L130 30L100 30Z"/></g><g id="lines" fill="none" stroke-width="2" opacity="0.85"><path id="ink" stroke="#123456" data-custom="keep" d="m10 10 c10 0 20 10 30 10 l10 0"/><path id="untouched" stroke="#654321" d="M10 100C20 90 30 90 40 100"/></g></svg>)svg";
QDomElement byId(const QByteArray& svg, const QString& id) {
    QDomDocument doc;
    doc.setContent(svg);
    const auto nodes = doc.elementsByTagName(QStringLiteral("path"));
    for (int i = 0; i < nodes.size(); ++i) if (nodes.at(i).toElement().attribute("id") == id) return nodes.at(i).toElement();
    return {};
}
}
class VectorEditDocumentTests : public QObject {
    Q_OBJECT
private slots:
    void loadAndHitTest();
    void bezierAndUndo();
    void straightLineCanBend();
    void closedSeamMovesTogether();
    void additionsColorAndRemoval();
    void cancelAndAtomicFailures();
    void fillDeformationPreservesHoles();
    void fillLongEdgeSubdivides();
    void unsupportedAndBounds();
    void editMemoryBoundaries();
    void realGeneratedSvg();
};
void VectorEditDocumentTests::loadAndHitTest() {
    EditDocument document;
    QString error;
    QVERIFY2(document.load(Example, &error), qPrintable(error));
    QCOMPARE(document.size(), QSize(200,160));
    QCOMPARE(document.svg(), Example);
    QCOMPARE(document.pathCount(), 4);
    QCOMPARE(document.layer(0), Layer::Fill);
    QCOMPARE(document.hitTest({110,20}, 0, Layer::Fill), 1);
    QCOMPARE(document.hitTest({20,40}, 0, Layer::Fill), 0);
    QCOMPARE(document.hitTest({60,70}, 0, Layer::Fill), -1); // evenodd hole
    QCOMPARE(document.hitTest({12,10}, 3, Layer::Lines), 2);
    QCOMPARE(document.color(2), QColor("#123456"));
    const auto handles = document.handles(2);
    QCOMPARE(handles.size(), 7);
    QCOMPARE(handles[0].point, QPointF(10,10));
    QCOMPARE(handles[1].point, QPointF(20,10));
    QCOMPARE(handles[2].point, QPointF(30,20));
    QCOMPARE(handles[3].point, QPointF(40,20));
}
void VectorEditDocumentTests::bezierAndUndo() {
    EditDocument document;
    QVERIFY(document.load(Example));
    const QString untouched = byId(Example,"untouched").attribute("d");
    document.beginEdit();
    QVERIFY(document.moveHandle(2, {0,0,Handle::Control1,{}}, {20,30}));
    QVERIFY(document.moveHandle(2, {0,1,Handle::Anchor,{}}, {45,25}));
    document.commitEdit();
    QVERIFY(document.canUndo());
    auto handles = document.handles(2);
    QCOMPARE(handles[1].point,QPointF(20,30));
    QCOMPARE(handles[2].point,QPointF(35,25));
    QCOMPARE(handles[3].point,QPointF(45,25));
    QCOMPARE(byId(document.svg(),"untouched").attribute("d"), untouched);
    QCOMPARE(byId(document.svg(),"ink").attribute("data-custom"), QString("keep"));
    const QByteArray edited = document.svg();
    QSvgRenderer renderer(edited);
    QVERIFY(renderer.isValid());
    QVERIFY(document.undo());
    QCOMPARE(document.svg(), Example);
    QVERIFY(!document.canUndo());
    QVERIFY(document.redo());
    QCOMPARE(document.svg(), edited);
}
void VectorEditDocumentTests::closedSeamMovesTogether() {
    EditDocument document;
    QVERIFY(document.load(R"(<svg width="100" height="100"><g id="lines"><path stroke="black" d="M10 10C30 10 30 40 10 40C0 40 0 10 10 10Z"/></g></svg>)"));
    QVERIFY(document.moveHandle(0, {0,0,Handle::Anchor,{}}, {15,20}));
    auto handles = document.handles(0);
    QCOMPARE(handles.size(), 6);
    QCOMPARE(handles[0].point, QPointF(15,20));
    QCOMPARE(handles[1].point, QPointF(35,20));
    QCOMPARE(handles.last().point, QPointF(5,20));
    QCOMPARE(document.path(0).currentPosition(), QPointF(15,20));
}
void VectorEditDocumentTests::straightLineCanBend() {
    EditDocument document;
    QVERIFY(document.load(Example));
    const auto original = document.svg();
    const auto points = document.handles(2);
    QCOMPARE(points[4].kind, Handle::Control1);
    QCOMPARE(points[4].index, 1);
    QCOMPARE(document.svg(), original);
    QVERIFY(document.moveHandle(2, points[4], {43,40}));
    const QString data = byId(document.svg(),"ink").attribute("d");
    QCOMPARE(data.count('C'),2);
    QCOMPARE(document.handles(2)[4].point,QPointF(43,40));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),original);
}
void VectorEditDocumentTests::additionsColorAndRemoval() {
    EditDocument document;
    QVERIFY(document.load(Example));
    const int index = document.addLine({1,1},{50,50}, QColor(10,20,30,128), 3.5);
    QCOMPARE(index,4);
    QCOMPARE(document.handles(index).size(),4);
    QCOMPARE(document.color(index), QColor(10,20,30,128));
    QVERIFY(document.setColor(0, QColor("red")));
    QCOMPARE(document.color(0), QColor("red"));
    QCOMPARE(byId(document.svg(),"area").attribute("transform"), QString("translate(10,20)"));
    QVERIFY(document.remove(2));
    QVERIFY(byId(document.svg(),"ink").isNull());
    QVERIFY(document.path(2).isEmpty());
    QVERIFY(!document.remove(2));
    QVERIFY(document.undo());
    QVERIFY(!byId(document.svg(),"ink").isNull());
    QVERIFY(document.undo());
    QCOMPARE(document.color(0), QColor("#abcdef"));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(), Example);
}
void VectorEditDocumentTests::cancelAndAtomicFailures() {
    EditDocument document;
    QVERIFY(document.load(Example));
    document.beginEdit();
    QVERIFY(document.setColor(2, Qt::red));
    QVERIFY(document.moveHandle(2, {0,0,Handle::Anchor,{}},{5,5}));
    document.cancelEdit();
    QCOMPARE(document.svg(), Example);
    QVERIFY(!document.canUndo());
    QVERIFY(!document.load("<broken>"));
    QCOMPARE(document.svg(), Example);
    QVERIFY(!document.moveHandle(2, {0,0,Handle::Anchor,{}},{std::numeric_limits<double>::quiet_NaN(),4}));
    QVERIFY(!document.moveHandle(2, {90,0,Handle::Anchor,{}},{4,4}));
    QVERIFY(!document.deformFill(2,{5,5},{1,1},10,false));
    QCOMPARE(document.svg(),Example);
}
void VectorEditDocumentTests::fillDeformationPreservesHoles() {
    EditDocument document;
    QVERIFY(document.load(Example));
    const QString untouched = byId(document.svg(),"top").attribute("d");
    document.beginEdit();
    QVERIFY(document.deformFill(0, {110,60},{12,0},20,false));
    QVERIFY(document.deformFill(0, {115,60},{4,0},20,false));
    QVERIFY(document.deformFill(0, {115,60},{0,0},20,true));
    document.commitEdit();
    QVERIFY(document.path(0).contains({114,60}));
    QVERIFY(!document.path(0).contains({60,70}));
    QCOMPARE(byId(document.svg(),"top").attribute("d"), untouched);
    QCOMPARE(byId(document.svg(),"area").attribute("fill-rule"),QString("evenodd"));
    const auto handles = document.handles(0);
    int holeAnchors = 0;
    for (const auto& handle : handles) if (handle.subpath == 1 && handle.kind == Handle::Anchor) ++holeAnchors;
    QCOMPARE(holeAnchors,4);
    QVERIFY(document.svg().contains('C'));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(), Example);
    QVERIFY(!document.canUndo());
}
void VectorEditDocumentTests::fillLongEdgeSubdivides() {
    EditDocument document;
    QVERIFY(document.load(R"(<svg width="10000" height="10000"><g id="fill"><path fill="red" d="M0 0L10000 0L10000 10000L0 10000Z"/></g></svg>)"));
    document.beginEdit();
    QVERIFY(document.deformFill(0,{0,5000},{-20,0},100,false));
    QVERIFY(document.deformFill(0,{0,5000},{},100,true));
    document.commitEdit();
    QVERIFY(document.path(0).contains({-10,5000}));
    QVERIFY(!document.path(0).contains({-10,4500}));
    QVERIFY(document.path(0).contains({500,5000}));
    QVERIFY(document.handles(0).size() < 150);
}
void VectorEditDocumentTests::unsupportedAndBounds() {
    EditDocument document;
    QString error;
    QVERIFY(!document.load(R"(<svg width="100" height="100"><g id="lines"><path d="M1 2A5 5 0 0 0 3 3"/></g></svg>)", &error));
    QVERIFY(error.contains("未対応"));
    QVERIFY(!document.load(R"svg(<svg width="100" height="100"><g id="lines" transform="scale(2)"><path d="M1 2L3 3"/></g></svg>)svg", &error));
    QVERIFY(!document.load(R"(<svg width="100" height="100"><g id="lines"><path d="M1 2L1e999 3"/></g></svg>)", &error));
    QVERIFY(!document.load(R"(<svg width="100" height="100" viewBox="10 0 100 100"/>)", &error));
    QVERIFY(!document.load(R"(<!DOCTYPE svg [<!ENTITY a "b">]><svg width="100" height="100"/>)", &error));
    QVERIFY(document.load(R"(<svg width="10001" height="12000" viewBox="0 0 10001 12000"><g id="lines"/></svg>)"));
    QCOMPARE(document.addLine({0,0},{5,5}, Qt::black,0),-1);
    QCOMPARE(document.addLine({0,0},{5,5}, Qt::black,2),0);
}
void VectorEditDocumentTests::realGeneratedSvg() {
    const QString source = qEnvironmentVariable("QVIEWSR_TEST_EDIT_SVG");
    if (source.isEmpty()) QSKIP("Set QVIEWSR_TEST_EDIT_SVG to verify a real generated SVG.");
    QFile file(source);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray original = file.readAll();
    EditDocument document;
    QString error;
    QVERIFY2(document.load(original,&error),qPrintable(error));
    QVERIFY(document.pathCount()>0);
    const auto points = document.handles(document.pathCount()-1);
    QVERIFY(!points.isEmpty());
    document.beginEdit();
    QVERIFY(document.moveHandle(document.pathCount()-1, points[0], points[0].point+QPointF(1,1)));
    document.commitEdit();
    QSvgRenderer render(document.svg());
    QVERIFY(render.isValid());
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),original);
}
void VectorEditDocumentTests::editMemoryBoundaries() {
    constexpr quint64 MiB = 1024ULL * 1024;
    const quint64 bytes = 203 * MiB;
    const quint64 required = bytes * 64 + 128 * MiB;
    const quint64 enoughAvailable = (required * 10 + 6) / 7;
    QVERIFY(EditDocument::memoryError(bytes, enoughAvailable).isEmpty());
    QVERIFY(!EditDocument::memoryError(bytes, enoughAvailable - 1).isEmpty());
    QVERIFY(EditDocument::memoryError(1024 * MiB, 128ULL * 1024 * MiB).isEmpty());
    QVERIFY(!EditDocument::memoryError(1024 * MiB + 1, std::numeric_limits<quint64>::max()).isEmpty());
    QVERIFY(EditDocument::memoryError(64 * MiB, 0).isEmpty());
    QVERIFY(!EditDocument::memoryError(64 * MiB + 1, 0).isEmpty());
    QVERIFY(EditDocument::memoryError(bytes, std::numeric_limits<quint64>::max()).isEmpty());
    QVERIFY(!EditDocument::memoryError(std::numeric_limits<quint64>::max(), std::numeric_limits<quint64>::max()).isEmpty());
}
QTEST_MAIN(VectorEditDocumentTests)
#include "tst_vectoreditdocumenttests.moc"
