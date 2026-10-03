// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QImage>
#include <QPainter>
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
QImage render(const QByteArray& svg) {
    QImage image(100,100,QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QSvgRenderer renderer(svg);
    QPainter painter(&image);
    renderer.render(&painter);
    return image;
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
    void layerVisibilityRendersAndUndoes();
    void hiddenEmptyLayerRemainsHiddenOnAddition();
    void multiAnchorBezierPath();
    void clickedPenSegmentsHaveEditableHandles();
    void sweptEraserIsLayerSpecificAndUndoable();
    void eraserSkipsHoleAndOpenCurveInterior();
    void partialLineEraserPreservesCurves();
    void rasterFillBrushSplitsAndPreservesPaintOrder();
    void rasterFillBrushPreviewAndBudget();
    void rasterFillErasePreservesWindingCoverage();
    void rasterFillEraseKeepsCurvedHoleWithExteriorControls();
    void rasterFillEraseKeepsEvenOddOverlaps();
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
void VectorEditDocumentTests::layerVisibilityRendersAndUndoes() {
    const QByteArray source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="fill"><path id="face" fill="red" d="M0 0L100 0L100 100L0 100Z"/></g><g id="lines"><path id="stroke" fill="none" stroke="blue" stroke-width="10" d="M0 50L100 50"/></g></svg>)";
    EditDocument document;
    QVERIFY(document.load(source));
    QVERIFY(document.layerVisible(Layer::Lines));
    QVERIFY(document.layerVisible(Layer::Fill));
    QCOMPARE(render(document.svg()).pixelColor(50,50),QColor(Qt::blue));
    QVERIFY(document.setLayerVisible(Layer::Lines,false));
    QVERIFY(!document.layerVisible(Layer::Lines));
    QCOMPARE(document.hitTest({50,50},5,Layer::Lines),-1);
    QCOMPARE(document.hitTest({50,50},0,Layer::Fill),0);
    QCOMPARE(render(document.svg()).pixelColor(50,50),QColor(Qt::red));
    const QByteArray linesHidden = document.svg();
    QVERIFY(!byId(linesHidden,"stroke").isNull());
    QVERIFY(!document.setLayerVisible(Layer::Lines,false));
    QVERIFY(document.setLayerVisible(Layer::Fill,false));
    QCOMPARE(render(document.svg()).pixelColor(50,50).alpha(),0);
    QCOMPARE(document.hitTest({50,50},0,Layer::Fill),-1);
    EditDocument reopened;
    QVERIFY(reopened.load(document.svg()));
    QVERIFY(!reopened.layerVisible(Layer::Fill));
    QVERIFY(!reopened.layerVisible(Layer::Lines));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),linesHidden);
    QVERIFY(document.layerVisible(Layer::Fill));
    QVERIFY(!document.layerVisible(Layer::Lines));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);
    QVERIFY(!document.canUndo());
    QVERIFY(document.redo());
    QCOMPARE(render(document.svg()).pixelColor(50,50),QColor(Qt::red));
}
void VectorEditDocumentTests::hiddenEmptyLayerRemainsHiddenOnAddition() {
    EditDocument document;
    const QByteArray source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="fill"/></svg>)svg";
    QVERIFY(document.load(source));
    QVERIFY(document.setLayerVisible(Layer::Lines,false));
    QCOMPARE(document.addLine({0,50},{100,50},Qt::blue,10),0);
    QVERIFY(!document.layerVisible(Layer::Lines));
    QCOMPARE(document.hitTest({50,50},5,Layer::Lines),-1);
    QCOMPARE(render(document.svg()).pixelColor(50,50).alpha(),0);
    QVERIFY(document.setLayerVisible(Layer::Lines,true));
    QCOMPARE(document.hitTest({50,50},5,Layer::Lines),0);
    QCOMPARE(render(document.svg()).pixelColor(50,50),QColor(Qt::blue));
    QVERIFY(document.undo());
    QVERIFY(!document.layerVisible(Layer::Lines));
    QCOMPARE(render(document.svg()).pixelColor(50,50).alpha(),0);
    QVERIFY(document.undo());
    QCOMPARE(document.pathCount(),0);
    QVERIFY(!document.layerVisible(Layer::Lines));
}
void VectorEditDocumentTests::multiAnchorBezierPath() {
    const QByteArray source = R"svg(<svg width="100" height="100"><g id="lines" transform="translate(10,20)"/></svg>)svg";
    EditDocument document;
    QVERIFY(document.load(source));
    const QVector<BezierAnchor> anchors = {{{10,50},{0,50},{25,50}},
                                           {{50,20},{40,20},{60,20}},
                                           {{90,50},{75,50},{100,50}}};
    QCOMPARE(document.addBezierPath(anchors,Qt::magenta,3),0);
    const auto handles = document.handles(0);
    QCOMPARE(handles.size(),7);
    QCOMPARE(handles[0].point,anchors[0].point);
    QCOMPARE(handles[1].point,anchors[0].outgoing);
    QCOMPARE(handles[2].point,anchors[1].incoming);
    QCOMPARE(handles[3].point,anchors[1].point);
    QCOMPARE(handles[4].point,anchors[1].outgoing);
    QCOMPARE(handles[5].point,anchors[2].incoming);
    QCOMPARE(handles[6].point,anchors[2].point);
    QCOMPARE(document.color(0),QColor(Qt::magenta));
    QDomDocument xml;
    QVERIFY(xml.setContent(document.svg()));
    const auto path = xml.elementsByTagName("path").at(0).toElement();
    QCOMPARE(path.attribute("d").count('C'),2);
    QVERIFY(path.attribute("d").startsWith("M0 30"));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);
    QVERIFY(!document.canUndo());
    QCOMPARE(document.addBezierPath(anchors,Qt::black,2,true),0);
    QCOMPARE(document.handles(0).size(),9);
    QCOMPARE(document.path(0).currentPosition(),anchors[0].point);
    const auto saved = document.svg();
    auto invalid = anchors;
    invalid[1].outgoing.setX(std::numeric_limits<double>::infinity());
    QCOMPARE(document.addBezierPath(invalid,Qt::black,2),-1);
    QCOMPARE(document.addBezierPath({anchors[0]},Qt::black,2),-1);
    QCOMPARE(document.svg(),saved);
}
void VectorEditDocumentTests::sweptEraserIsLayerSpecificAndUndoable() {
    const QByteArray source = R"(<svg width="100" height="100"><g id="fill"><path fill="red" d="M0 0L100 0L100 100L0 100Z"/></g><g id="lines" fill="none" stroke="black" stroke-width="2"><path d="M20 10L20 90"/><path d="M50 10L50 90"/><path d="M80 10L80 90"/></g></svg>)";
    EditDocument document;
    QVERIFY(document.load(source));
    document.beginEdit();
    // Endpoints miss the strokes; the swept area still catches both crossings.
    QCOMPARE(document.erasePaths({10,50},{60,50},2,Layer::Lines),2);
    QCOMPARE(document.erasePaths({60,50},{90,50},2,Layer::Lines),1);
    document.commitEdit();
    QVERIFY(document.path(1).isEmpty());
    QVERIFY(document.path(2).isEmpty());
    QVERIFY(document.path(3).isEmpty());
    QVERIFY(!document.path(0).isEmpty());
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);
    QVERIFY(!document.canUndo());
    QVERIFY(document.setLayerVisible(Layer::Lines,false));
    QCOMPARE(document.erasePaths({0,50},{100,50},4,Layer::Lines),0);
    QVERIFY(!document.path(1).isEmpty());
    QCOMPARE(document.erasePaths({50,50},{50,50},4,Layer::Fill),1);
    QVERIFY(document.path(0).isEmpty());
    QVERIFY(!document.path(1).isEmpty());
}
void VectorEditDocumentTests::clickedPenSegmentsHaveEditableHandles() {
    EditDocument document;
    const QByteArray source = R"(<svg width="100" height="100"><g id="lines"/></svg>)";
    QVERIFY(document.load(source));
    const QVector<BezierAnchor> anchors = {{{10,50},{10,50},{10,50}},
                                           {{40,50},{40,50},{40,50}},
                                           {{40,80},{40,80},{40,80}}};
    QCOMPARE(document.addBezierPath(anchors,Qt::black,2),0);
    const auto handles = document.handles(0);
    QCOMPARE(handles[1].point,QPointF(20,50));
    QCOMPARE(handles[2].point,QPointF(30,50));
    QCOMPARE(handles[4].point,QPointF(40,60));
    QCOMPARE(handles[5].point,QPointF(40,70));
    // The expanded controls stay on the exact original polyline.
    QCOMPARE(document.path(0).boundingRect(),QRectF(10,50,30,30));
    QVERIFY(document.moveHandle(0,handles[1],{20,20}));
    QVERIFY(document.path(0).boundingRect().top()<50);
    QVERIFY(document.undo());
    QCOMPARE(document.path(0).boundingRect(),QRectF(10,50,30,30));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);
}
void VectorEditDocumentTests::eraserSkipsHoleAndOpenCurveInterior() {
    EditDocument document;
    QVERIFY(document.load(R"(<svg width="100" height="100"><g id="fill"><path fill="red" fill-rule="evenodd" d="M0 0L100 0L100 100L0 100Z M30 30L70 30L70 70L30 70Z"/></g><g id="lines" stroke="black" fill="none" stroke-width="2"><path d="M20 20L20 80L80 80L80 20"/><path display="none" d="M0 50L100 50"/></g></svg>)"));
    const auto source = document.svg();
    QCOMPARE(document.erasePaths({50,50},{50,50},5,Layer::Fill),0);
    QCOMPARE(document.erasePaths({40,50},{60,50},5,Layer::Lines),0);
    QCOMPARE(document.svg(),source);
    QVERIFY(!document.canUndo());
    QCOMPARE(document.hitTest({50,50},2,Layer::Lines),-1);
    QCOMPARE(document.erasePaths({20,45},{20,55},2,Layer::Lines),1);
    QVERIFY(!document.path(2).isEmpty()); // independently hidden path remains intact
}
void VectorEditDocumentTests::partialLineEraserPreservesCurves() {
    const QByteArray source = R"(<svg width="100" height="100"><g id="lines" stroke="black" stroke-width="2" fill="none"><path id="curve" d="M0 50C30 10 70 90 100 50"/><path id="other" d="M0 10C30 0 70 0 100 10"/></g></svg>)";
    EditDocument document;
    QVERIFY(document.load(source));
    QPainterPath brush; brush.addEllipse(QPointF(50,50),8,8);
    QString error;
    document.beginEdit();
    QCOMPARE(document.eraseLineSegments(brush,&error),1);
    QVERIFY2(error.isEmpty(),qPrintable(error));
    document.commitEdit();
    QCOMPARE(byId(document.svg(),"curve").attribute("d").count('M'),2);
    QCOMPARE(byId(document.svg(),"curve").attribute("d").count('C'),2);
    QCOMPARE(byId(document.svg(),"other").attribute("d"),byId(source,"other").attribute("d"));
    QCOMPARE(document.hitTest({50,50},0,Layer::Lines),-1);
    const auto handles = document.handles(0);
    QCOMPARE(handles.size(),8);
    QCOMPARE(handles.first().point,QPointF(0,50));
    QCOMPARE(handles.last().point,QPointF(100,50));
    QVERIFY(std::any_of(handles.cbegin(),handles.cend(),[](const Handle& h){return h.subpath==1 && h.kind==Handle::Control1;}));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);
    QVERIFY(!document.canUndo());
    QVERIFY(document.redo());
    QCOMPARE(document.hitTest({50,50},0,Layer::Lines),-1);
    QVERIFY(document.setLayerVisible(Layer::Lines,false));
    const auto hidden = document.svg();
    QCOMPARE(document.eraseLineSegments(brush,&error),0);
    QCOMPARE(document.svg(),hidden);
}
void VectorEditDocumentTests::rasterFillBrushSplitsAndPreservesPaintOrder() {
    const QByteArray source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="fill"><path id="red" fill="red" d="M10 10L90 10L90 90L10 90Z"/><path id="blue" fill="blue" d="M65 65L80 65L80 80L65 80Z"/></g><g id="lines" stroke="black" stroke-width="2" fill="none"><path id="line" d="M0 50L100 50"/></g></svg>)";
    EditDocument document;
    QVERIFY(document.load(source));
    QPainterPath brush; brush.addRect(QRectF(45,0,10,100));
    QString error;
    QVERIFY2(document.applyRasterFillBrush(brush,Qt::black,true,&error)>0,qPrintable(error));
    QCOMPARE(document.pathCount(),4); // split red has independently selectable halves
    const auto pixels = render(document.svg());
    QCOMPARE(pixels.pixelColor(20,30),QColor(Qt::red));
    QCOMPARE(pixels.pixelColor(80,30),QColor(Qt::red));
    QCOMPARE(pixels.pixelColor(50,30).alpha(),0);
    QCOMPARE(pixels.pixelColor(50,50),QColor(Qt::black));
    QCOMPARE(pixels.pixelColor(70,70),QColor(Qt::blue));
    QCOMPARE(document.hitTest({70,70},0,Layer::Fill),1);
    QVERIFY(document.hitTest({20,30},0,Layer::Fill)!=document.hitTest({80,30},0,Layer::Fill));
    QCOMPARE(byId(document.svg(),"blue").attribute("d"),byId(source,"blue").attribute("d"));
    QCOMPARE(byId(document.svg(),"line").attribute("d"),byId(source,"line").attribute("d"));
    QVERIFY(!document.svg().contains("<image"));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);
}
void VectorEditDocumentTests::rasterFillBrushPreviewAndBudget() {
    EditDocument document;
    const QByteArray source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="fill"/><g id="lines" stroke="black" stroke-width="2" fill="none"><path d="M0 50L100 50"/></g></svg>)";
    QVERIFY(document.load(source));
    QPainterPath brush; brush.addRect(QRectF(10,10,70,70));
    QString error; QRectF bounds;
    const auto preview = document.rasterBrushPreview(brush,Qt::red,false,&bounds,&error);
    QVERIFY2(!preview.isNull(),qPrintable(error));
    QCOMPARE(preview.pixelColor(20-int(bounds.left()),20-int(bounds.top())),QColor(Qt::red));
    QCOMPARE(preview.pixelColor(50-int(bounds.left()),50-int(bounds.top())),QColor(Qt::black));
    QCOMPARE(document.svg(),source);
    QVERIFY2(document.applyRasterFillBrush(brush,Qt::red,false,&error)>0,qPrintable(error));
    QCOMPARE(render(document.svg()).pixelColor(20,20),QColor(Qt::red));
    QCOMPARE(render(document.svg()).pixelColor(50,50),QColor(Qt::black));
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);

    const QByteArray large = R"(<svg width="20000" height="20000"><g id="fill"/></svg>)";
    QVERIFY(document.load(large));
    QPainterPath huge; huge.addRect(QRectF(0,0,10000,10000));
    QCOMPARE(document.applyRasterFillBrush(huge,Qt::red,false,&error),-1);
    QVERIFY(error.contains("64"));
    QCOMPARE(document.svg(),large);
    QVERIFY(!document.canUndo());
    const auto reduced = document.rasterBrushPreview(huge,Qt::red,false,&bounds,&error);
    QVERIFY2(!reduced.isNull(),qPrintable(error));
    QVERIFY(qint64(reduced.width())*reduced.height()<=2*1024*1024);
    QVERIFY(bounds.width()>10000);
}
void VectorEditDocumentTests::rasterFillErasePreservesWindingCoverage() {
    const QByteArray source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="fill"><path id="overlap" fill="red" d="M0 0L70 0L70 100L0 100Z M30 0L100 0L100 100L30 100Z"/></g></svg>)";
    EditDocument document;
    QVERIFY(document.load(source));
    QPainterPath brush; brush.addEllipse(QPointF(10,50),4,4);
    QString error;
    QVERIFY2(document.applyRasterFillBrush(brush,Qt::black,true,&error)>0,qPrintable(error));
    const auto pixels = render(document.svg());
    QCOMPARE(pixels.pixelColor(10,50).alpha(),0);
    QCOMPARE(pixels.pixelColor(50,50),QColor(Qt::red));
    QCOMPARE(pixels.pixelColor(90,50),QColor(Qt::red));
    const QByteArray hole = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="fill"><path fill="red" d="M0 0L100 0L100 100L0 100Z M30 30L30 70L70 70L70 30Z"/></g></svg>)";
    QVERIFY(document.load(hole));
    QVERIFY2(document.applyRasterFillBrush(brush,Qt::black,true,&error)>0,qPrintable(error));
    QCOMPARE(render(document.svg()).pixelColor(50,50).alpha(),0);
    QCOMPARE(render(document.svg()).pixelColor(90,50),QColor(Qt::red));
}
void VectorEditDocumentTests::rasterFillEraseKeepsCurvedHoleWithExteriorControls() {
    const QByteArray source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="fill"><path fill="red" fill-rule="evenodd" d="M0 0L100 0L100 100L0 100Z M40 40C-100 5 200 5 60 40L60 60L40 60Z"/></g></svg>)";
    EditDocument document;
    QVERIFY(document.load(source));
    QCOMPARE(render(document.svg()).pixelColor(50,50).alpha(),0);
    QPainterPath brush; brush.addEllipse(QPointF(10,80),2,2);
    QString error;
    QVERIFY2(document.applyRasterFillBrush(brush,Qt::black,true,&error)>0,qPrintable(error));
    QCOMPARE(render(document.svg()).pixelColor(50,50).alpha(),0);
    QCOMPARE(render(document.svg()).pixelColor(10,80).alpha(),0);
    QCOMPARE(render(document.svg()).pixelColor(90,80),QColor(Qt::red));
    QCOMPARE(document.pathCount(),1);
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);
}
void VectorEditDocumentTests::rasterFillEraseKeepsEvenOddOverlaps() {
    const QByteArray source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><g id="fill"><path fill="red" fill-rule="evenodd" d="M0 0L70 0L70 100L0 100Z M30 0L100 0L100 100L30 100Z"/></g></svg>)";
    EditDocument document;
    QVERIFY(document.load(source));
    QCOMPARE(render(document.svg()).pixelColor(50,50).alpha(),0);
    QPainterPath brush; brush.addEllipse(QPointF(10,50),2,2);
    QString error;
    QVERIFY2(document.applyRasterFillBrush(brush,Qt::black,true,&error)>0,qPrintable(error));
    const auto pixels = render(document.svg());
    QCOMPARE(pixels.pixelColor(50,50).alpha(),0);
    QCOMPARE(pixels.pixelColor(10,50).alpha(),0);
    QCOMPARE(pixels.pixelColor(10,80),QColor(Qt::red));
    QCOMPARE(pixels.pixelColor(90,80),QColor(Qt::red));
    QCOMPARE(document.pathCount(),1);
    QVERIFY(document.undo());
    QCOMPARE(document.svg(),source);
}
QTEST_MAIN(VectorEditDocumentTests)
#include "tst_vectoreditdocumenttests.moc"
