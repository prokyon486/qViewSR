// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit_document.h"
#include "memory_budget.h"
#include "raster_contours.h"

#include <QDomDocument>
#include <QHash>
#include <QPainterPathStroker>
#include <QPainter>
#include <QSet>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace Vector {
namespace {
constexpr qsizetype MaxSvgBytes = 1024 * 1024 * 1024;
constexpr qsizetype MaxHistoryBytes = 256 * 1024 * 1024;
constexpr int MaxSegments = 32000000;
constexpr int MaxPaths = 5000000;
constexpr double MaxCoordinate = 1.e9;
constexpr auto SvgNamespace = "http://www.w3.org/2000/svg";

bool validPoint(QPointF p) {
    return std::isfinite(p.x()) && std::isfinite(p.y())
        && std::abs(p.x()) <= MaxCoordinate && std::abs(p.y()) <= MaxCoordinate;
}
double length(QPointF p) { return std::hypot(p.x(), p.y()); }
bool same(QPointF a, QPointF b) { return length(a - b) < 1.e-8; }
QString number(double v) { return QString::number(v == 0 ? 0 : v, 'g', 15); }
QString pointText(QPointF p) { return number(p.x()) + QLatin1Char(' ') + number(p.y()); }
QString tag(QDomElement e) { return e.tagName().section(QLatin1Char(':'), -1); }
QString inherited(QDomElement e, const QString& attribute, const QString& fallback = {}) {
    for (; !e.isNull(); e = e.parentNode().toElement())
        if (e.hasAttribute(attribute)) return e.attribute(attribute);
    return fallback;
}
int layerIndex(Layer layer) { return layer == Layer::Lines ? 0 : 1; }
bool paintVisible(QDomElement element) {
    const QString visibility = inherited(element, QStringLiteral("visibility"), QStringLiteral("visible")).trimmed().toLower();
    if (visibility == QStringLiteral("hidden") || visibility == QStringLiteral("collapse")) return false;
    for (; !element.isNull(); element = element.parentNode().toElement())
        if (element.attribute(QStringLiteral("display")).trimmed().toLower() == QStringLiteral("none")) return false;
    return true;
}

struct Segment {
    QPointF c1, c2, end;
    bool cubic = false;
    bool touched = false;
};
struct Subpath {
    QPointF start;
    QVector<Segment> segments;
    bool closed = false;
    bool startTouched = false;
};
struct Shape {
    QDomElement element;
    Layer layer = Layer::Lines;
    QPointF offset;
    QVector<Subpath> subpaths;
    QPainterPath geometry;
    double strokeWidth = 1.;
    bool deleted = false;
    bool visible = true;
};

class Scanner {
public:
    explicit Scanner(const QString& s) : text(s) {}
    void spaces() {
        while (at < text.size() && (text[at].isSpace() || text[at] == QLatin1Char(','))) ++at;
    }
    bool end() { spaces(); return at == text.size(); }
    bool command(QChar* out) {
        spaces();
        if (at < text.size() && text[at].isLetter()) { *out = text[at++]; return true; }
        return false;
    }
    bool value(double* out) {
        spaces();
        const qsizetype begin = at;
        if (at < text.size() && (text[at] == QLatin1Char('-') || text[at] == QLatin1Char('+'))) ++at;
        int digits = 0;
        while (at < text.size() && text[at].isDigit()) { ++at; ++digits; }
        if (at < text.size() && text[at] == QLatin1Char('.')) {
            ++at;
            while (at < text.size() && text[at].isDigit()) { ++at; ++digits; }
        }
        if (!digits) return false;
        if (at < text.size() && (text[at] == QLatin1Char('e') || text[at] == QLatin1Char('E'))) {
            ++at;
            if (at < text.size() && (text[at] == QLatin1Char('-') || text[at] == QLatin1Char('+'))) ++at;
            int exponentDigits = 0;
            while (at < text.size() && text[at].isDigit()) { ++at; ++exponentDigits; }
            if (!exponentDigits) return false;
        }
        bool ok = false;
        *out = text.mid(begin, at - begin).toDouble(&ok);
        return ok && std::isfinite(*out) && std::abs(*out) <= MaxCoordinate;
    }
    bool point(QPointF* out) {
        double x, y;
        if (!value(&x) || !value(&y)) return false;
        *out = QPointF(x, y);
        return true;
    }
private:
    const QString& text;
    qsizetype at = 0;
};

bool parsePath(const QString& data, QVector<Subpath>* paths, int* total, QString* error) {
    Scanner scan(data);
    QChar command;
    QPointF current;
    while (!scan.end()) {
        QChar next;
        if (scan.command(&next)) command = next;
        if (command.isNull()) { *error = QStringLiteral("SVGのパス命令が不正です。"); return false; }
        const bool relative = command.isLower();
        const QChar upper = command.toUpper();
        if (upper == QLatin1Char('Z')) {
            if (paths->isEmpty()) { *error = QStringLiteral("SVGの閉曲線が不正です。"); return false; }
            paths->last().closed = true;
            current = paths->last().start;
            command = QChar();
            continue;
        }
        if (upper != QLatin1Char('M') && upper != QLatin1Char('L') && upper != QLatin1Char('C')
            && upper != QLatin1Char('H') && upper != QLatin1Char('V')) {
            *error = QStringLiteral("このSVGには編集に未対応のパス命令（%1）が含まれています。").arg(command);
            return false;
        }
        Segment segment;
        if (upper == QLatin1Char('C')) {
            segment.cubic = true;
            if (!scan.point(&segment.c1) || !scan.point(&segment.c2) || !scan.point(&segment.end)) {
                *error = QStringLiteral("SVGのベジエ曲線の数値が不正です。"); return false;
            }
            if (relative) { segment.c1 += current; segment.c2 += current; segment.end += current; }
        } else if (upper == QLatin1Char('H') || upper == QLatin1Char('V')) {
            double v;
            if (!scan.value(&v)) { *error = QStringLiteral("SVGの座標が不正です。"); return false; }
            segment.end = current;
            if (upper == QLatin1Char('H')) segment.end.setX(v + (relative ? current.x() : 0));
            else segment.end.setY(v + (relative ? current.y() : 0));
        } else {
            if (!scan.point(&segment.end)) { *error = QStringLiteral("SVGの座標が不正です。"); return false; }
            if (relative) segment.end += current;
        }
        if (!validPoint(segment.end) || (segment.cubic && (!validPoint(segment.c1) || !validPoint(segment.c2)))) {
            *error = QStringLiteral("SVGの座標が編集可能な範囲を超えています。"); return false;
        }
        if (++*total > MaxSegments) { *error = QStringLiteral("編集可能な点数（3200万区間）を超えています。"); return false; }
        if (upper == QLatin1Char('M')) {
            Subpath subpath;
            subpath.start = segment.end;
            paths->append(subpath);
            command = relative ? QLatin1Char('l') : QLatin1Char('L');
        } else {
            if (paths->isEmpty() || paths->last().closed) {
                *error = QStringLiteral("SVGのパス開始位置が不正です。"); return false;
            }
            paths->last().segments.append(segment);
        }
        current = segment.end;
    }
    return true;
}

bool translateOnly(QDomElement e, QPointF* offset) {
    static const QRegularExpression translate(QStringLiteral(R"(^\s*translate\s*\(([^()]*)\)\s*$)"));
    for (; !e.isNull(); e = e.parentNode().toElement()) {
        const QString text = e.attribute(QStringLiteral("transform"));
        if (text.isEmpty()) continue;
        const auto match = translate.match(text);
        if (!match.hasMatch()) return false;
        const QString args = match.captured(1);
        Scanner scan(args);
        double x = 0, y = 0;
        if (!scan.value(&x)) return false;
        if (!scan.end() && !scan.value(&y)) return false;
        if (!scan.end()) return false;
        *offset += QPointF(x, y);
        if (!validPoint(*offset)) return false;
    }
    return true;
}

void updateGeometry(Shape& shape) {
    QPainterPath path;
    path.setFillRule(inherited(shape.element, QStringLiteral("fill-rule")) == QStringLiteral("evenodd")
                         ? Qt::OddEvenFill : Qt::WindingFill);
    for (const auto& sub : shape.subpaths) {
        path.moveTo(sub.start + shape.offset);
        for (const auto& segment : sub.segments) {
            if (segment.cubic) path.cubicTo(segment.c1 + shape.offset, segment.c2 + shape.offset, segment.end + shape.offset);
            else path.lineTo(segment.end + shape.offset);
        }
        if (sub.closed) path.closeSubpath();
    }
    shape.geometry = path;
}
void serialize(Shape& shape) {
    QString data;
    for (const auto& sub : shape.subpaths) {
        data += QLatin1Char('M') + pointText(sub.start);
        for (const auto& segment : sub.segments) {
            if (segment.cubic)
                data += QLatin1Char('C') + pointText(segment.c1) + QLatin1Char(' ') + pointText(segment.c2) + QLatin1Char(' ') + pointText(segment.end);
            else data += QLatin1Char('L') + pointText(segment.end);
        }
        if (sub.closed) data += QLatin1Char('Z');
    }
    shape.element.setAttribute(QStringLiteral("d"), data);
    updateGeometry(shape);
}

QPointF endpoint(const Subpath& sub, int vertex) {
    return vertex == 0 ? sub.start : sub.segments[vertex - 1].end;
}
void makeCubic(QPointF from, Segment& segment) {
    if (segment.cubic) return;
    segment.c1 = from + (segment.end - from) / 3.;
    segment.c2 = from + (segment.end - from) * (2. / 3.);
    segment.cubic = true;
}
void clearTouched(Shape& shape) {
    for (auto& sub : shape.subpaths) {
        sub.startTouched = false;
        for (auto& segment : sub.segments) segment.touched = false;
    }
}

bool finitePath(const QPainterPath& path) {
    if (path.elementCount() > 1000000) return false;
    for (int i = 0; i < path.elementCount(); ++i)
        if (!validPoint(QPointF(path.elementAt(i).x, path.elementAt(i).y))) return false;
    return true;
}
int shapeSegments(const Shape& shape) {
    int count = 0;
    for (const auto& sub : shape.subpaths) count += 1 + sub.segments.size();
    return count;
}
QRectF segmentBounds(QPointF start, const Segment& segment, QPointF offset) {
    QPainterPath path(start + offset);
    if (segment.cubic) path.cubicTo(segment.c1 + offset, segment.c2 + offset, segment.end + offset);
    else path.lineTo(segment.end + offset);
    return path.controlPointRect().adjusted(-1.e-7, -1.e-7, 1.e-7, 1.e-7);
}
void splitSegmentAt(QPointF from, Segment source, double t, Segment* left, Segment* right) {
    makeCubic(from, source);
    const auto interpolate = [t](QPointF first, QPointF last) { return first + (last - first) * t; };
    const QPointF a = interpolate(from, source.c1);
    const QPointF b = interpolate(source.c1, source.c2);
    const QPointF c = interpolate(source.c2, source.end);
    const QPointF ab = interpolate(a, b);
    const QPointF bc = interpolate(b, c);
    const QPointF middle = interpolate(ab, bc);
    *left = {a, ab, middle, true, false};
    *right = {bc, c, source.end, true, false};
}
void splitSegment(QPointF from, Segment source, Segment* left, Segment* right) {
    splitSegmentAt(from, source, .5, left, right);
}
struct ClippedSegment { QPointF start; Segment segment; bool keep; double begin = 0., end = 1.; };
bool clipSegment(QPointF from, const Segment& segment, QPointF offset, const QPainterPath& region,
                 QVector<ClippedSegment>* output, int* work, int depth = 0, double begin = 0., double end = 1.) {
    if (++*work > 2000000) return false;
    const QRectF bounds = segmentBounds(from, segment, offset);
    if (!region.intersects(bounds)) { output->append({from, segment, true, begin, end}); return true; }
    if (region.contains(bounds)) { output->append({from, segment, false, begin, end}); return true; }
    if (qMax(bounds.width(), bounds.height()) <= .1 || depth >= 40) {
        // Only the parameter at the cut is approximate (<= .1 source pixel).
        // Every retained cubic's coordinates remain an exact de Casteljau subcurve.
        Segment left, right;
        splitSegment(from, segment, &left, &right);
        output->append({from, segment, !region.contains(left.end + offset), begin, end});
        return true;
    }
    Segment left, right;
    splitSegment(from, segment, &left, &right);
    const double middle = (begin + end) / 2.;
    return clipSegment(from, left, offset, region, output, work, depth + 1, begin, middle)
        && clipSegment(left.end, right, offset, region, output, work, depth + 1, middle, end);
}
ClippedSegment retainedInterval(QPointF from, const Segment& source, double begin, double end) {
    Segment portion = source, discarded;
    if (end < 1.) splitSegmentAt(from, source, end, &portion, &discarded);
    QPointF start = from;
    if (begin > 0.) {
        Segment prefix;
        splitSegmentAt(from, portion, begin / end, &prefix, &portion);
        start = prefix.end;
    }
    return {start, portion, true, begin, end};
}

bool pathToSubpaths(const QPainterPath& path, QPointF offset, QVector<Subpath>* output, QString* error) {
    // Qt can leave a lone MoveTo in an empty boolean result. It is not a
    // surviving face and must not prevent complete erasure from removing it.
    if (path.isEmpty()) return true;
    if (path.elementCount() > MaxSegments * 3LL) {
        *error = QStringLiteral("塗りの境界が編集可能な点数を超えています。"); return false;
    }
    auto finish = [&] {
        if (!output->isEmpty() && !output->last().segments.isEmpty())
            output->last().closed = same(output->last().start, output->last().segments.last().end);
    };
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        const QPointF point(element.x - offset.x(), element.y - offset.y());
        if (!validPoint(point)) { *error = QStringLiteral("塗りの境界座標が範囲外です。"); return false; }
        if (element.isMoveTo()) {
            finish();
            Subpath sub; sub.start = point; output->append(sub);
        } else if (element.isLineTo()) {
            if (output->isEmpty()) return false;
            Segment segment; segment.end = point; output->last().segments.append(segment);
        } else if (element.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
            if (output->isEmpty()) return false;
            const auto c2 = path.elementAt(++i), end = path.elementAt(++i);
            const QPointF second(c2.x - offset.x(), c2.y - offset.y()), endpoint(end.x - offset.x(), end.y - offset.y());
            if (!validPoint(second) || !validPoint(endpoint)) { *error = QStringLiteral("塗りの境界座標が範囲外です。"); return false; }
            output->last().segments.append({point, second, endpoint, true, false});
        } else { *error = QStringLiteral("塗りの境界を変換できません。"); return false; }
    }
    finish();
    return true;
}
QVector<QVector<Subpath>> fillComponents(const QVector<Subpath>& loops, QString* error) {
    if (loops.size() > 4096) { *error = QStringLiteral("分離した色面が多すぎます。短いストロークに分けてください。"); return {}; }
    QVector<QPainterPath> paths;
    QVector<QRectF> boxes;
    paths.reserve(loops.size()); boxes.reserve(loops.size());
    for (const auto& loop : loops) {
        QPainterPath path(loop.start);
        for (const auto& segment : loop.segments) {
            if (segment.cubic) path.cubicTo(segment.c1, segment.c2, segment.end);
            else path.lineTo(segment.end);
        }
        path.closeSubpath();
        // Handles may extend outside an enclosing contour even when the entire
        // curve lies inside it. Containment must use true curve extrema.
        boxes.append(path.boundingRect()); paths.append(path);
    }
    QVector<int> parents(loops.size(), -1), depths(loops.size(), 0), components(loops.size(), -1);
    int work = 0;
    for (int i = 0; i < loops.size(); ++i) {
        double smallest = std::numeric_limits<double>::max();
        for (int j = 0; j < loops.size(); ++j) {
            if (++work > 4000000) { *error = QStringLiteral("色面の分離処理が複雑すぎます。短いストロークに分けてください。"); return {}; }
            if (i == j || !boxes[j].contains(boxes[i])) continue;
            const double area = boxes[j].width() * boxes[j].height();
            const double ownArea = boxes[i].width() * boxes[i].height();
            if (area > ownArea && area < smallest && paths[j].contains(loops[i].start)) { parents[i] = j; smallest = area; }
        }
    }
    QVector<QVector<Subpath>> result;
    for (int i = 0; i < loops.size(); ++i) {
        for (int parent = parents[i]; parent >= 0; parent = parents[parent]) ++depths[i];
        if (depths[i] % 2 == 0) { components[i] = result.size(); result.append({loops[i]}); }
    }
    for (int i = 0; i < loops.size(); ++i)
        if (depths[i] % 2 && parents[i] >= 0) result[components[parents[i]]].append(loops[i]);
    return result;
}

constexpr qint64 MaxRasterBrushPixels = 64LL * 1024 * 1024;
QRect brushRoi(const QPainterPath& region, QSize size, QString* error, bool final) {
    if (!finitePath(region)) { *error = QStringLiteral("ブラシの座標または点数が範囲外です。"); return {}; }
    if (region.isEmpty()) return {};
    const auto box = region.controlPointRect().adjusted(-2, -2, 2, 2).intersected(QRectF(QPointF(), QSizeF(size)));
    if (box.isEmpty()) return {};
    const QRect roi = box.toAlignedRect().intersected(QRect(QPoint(), size));
    const qint64 pixels = qint64(roi.width()) * roi.height();
    if (final && pixels > MaxRasterBrushPixels) {
        *error = QStringLiteral("1回の塗り操作の範囲が64メガピクセルを超えます。短いストロークに分けてください。"); return {};
    }
    if (final) {
        // Mask, contour work, path copies and previews can overlap temporarily.
        const quint64 required = quint64(pixels) * 96 + 64ULL * 1024 * 1024;
        const quint64 available = availableMemoryBytes();
        if ((!available && pixels > 1024 * 1024) || (available && required > (available / 10) * 7)) {
            *error = QStringLiteral("塗りの境界を再計算する空きメモリーが不足しています。短いストロークに分けてください。"); return {};
        }
    }
    return roi;
}
QTransform roiTransform(QRect roi, QSize pixels) {
    QTransform transform;
    transform.scale(double(pixels.width()) / roi.width(), double(pixels.height()) / roi.height());
    transform.translate(-roi.x(), -roi.y());
    return transform;
}
QImage geometryMask(const QPainterPath& path, const QPainterPath& erase, QRect roi) {
    QImage mask(roi.size(), QImage::Format_ARGB32_Premultiplied);
    if (mask.isNull()) return {};
    mask.fill(Qt::transparent);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(roiTransform(roi, roi.size()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(path);
    if (!erase.isEmpty()) {
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.drawPath(erase);
    }
    return mask;
}
double opacityAttribute(QDomElement element, const QString& name) {
    bool valid = false;
    const double opacity = inherited(element, name, QStringLiteral("1")).toDouble(&valid);
    return valid && std::isfinite(opacity) ? qBound(0., opacity, 1.) : 1.;
}
QColor paintColor(QDomElement element, const QString& attribute, const QString& fallback) {
    const QString text = inherited(element, attribute, fallback);
    QColor color = text == QStringLiteral("none") ? QColor(Qt::transparent) : QColor(text);
    if (!color.isValid()) color = Qt::transparent;
    color.setAlphaF(color.alphaF() * opacityAttribute(element, attribute + QStringLiteral("-opacity")));
    return color;
}
}

struct EditDocument::Private {
    QDomDocument document;
    QVector<Shape> shapes;
    QVector<int> paintOrder;
    QVector<QDomElement> groups[2];
    bool visible[2] = {true, true};
    QSize size;
    mutable QByteArray bytes;
    QByteArray before;
    QList<QByteArray> undo, redo;
    bool editing = false;
    bool dirty = false;
    QSet<int> touchedShapes;
    mutable QVector<int> previewCandidates;
    mutable QRectF previewCandidateBounds;
    mutable bool previewCandidatesValid = false;
    int segments = 0;

    bool read(const QByteArray& input, QString* error) {
        if (input.size() > MaxSvgBytes) { *error = QStringLiteral("SVGが編集上限（1 GiB）を超えています。"); return false; }
        if (input.contains("<!DOCTYPE") || input.contains("<!ENTITY")) {
            *error = QStringLiteral("外部定義を含むSVGは編集できません。"); return false;
        }
        if (!document.setContent(input)) { *error = QStringLiteral("SVGを読み込めません。"); return false; }
        const auto root = document.documentElement();
        if (tag(root) != QStringLiteral("svg")) { *error = QStringLiteral("SVG画像ではありません。"); return false; }
        bool widthOk = false, heightOk = false;
        const double width = root.attribute(QStringLiteral("width")).toDouble(&widthOk);
        const double height = root.attribute(QStringLiteral("height")).toDouble(&heightOk);
        if (!widthOk || !heightOk || width < 1 || height < 1 || width > 1000000 || height > 1000000
            || !std::isfinite(width) || !std::isfinite(height) || width != std::floor(width) || height != std::floor(height)) {
            *error = QStringLiteral("SVGの画像サイズが不正、または編集範囲外です。"); return false;
        }
        size = QSize(int(width), int(height));
        if (root.hasAttribute(QStringLiteral("viewBox"))) {
            const QString text = root.attribute(QStringLiteral("viewBox"));
            Scanner view(text);
            double x, y, w, h;
            if (!view.value(&x) || !view.value(&y) || !view.value(&w) || !view.value(&h) || !view.end()
                || x != 0 || y != 0 || w != width || h != height) {
                *error = QStringLiteral("このSVGの座標系は編集に対応していません。"); return false;
            }
        }
        std::function<bool(QDomElement, int, int)> visit = [&](QDomElement element, int currentLayer, int depth) {
            if (depth > 64) { *error = QStringLiteral("SVGの階層が深すぎます。"); return false; }
            const QString id = element.attribute(QStringLiteral("id"));
            if (id == QStringLiteral("lines")) currentLayer = 0;
            else if (id == QStringLiteral("fill")) currentLayer = 1;
            if (tag(element) == QStringLiteral("g")
                && (id == QStringLiteral("lines") || id == QStringLiteral("fill")))
                groups[currentLayer].append(element);
            if (currentLayer >= 0 && tag(element) == QStringLiteral("path")) {
                if (shapes.size() >= MaxPaths) { *error = QStringLiteral("編集可能なパス数（500万）を超えています。"); return false; }
                Shape shape;
                shape.element = element;
                shape.layer = currentLayer == 0 ? Layer::Lines : Layer::Fill;
                shape.visible = paintVisible(element);
                if (!translateOnly(element, &shape.offset)) {
                    *error = QStringLiteral("移動以外の座標変換を含むSVGは編集できません。"); return false;
                }
                if (!parsePath(element.attribute(QStringLiteral("d")), &shape.subpaths, &segments, error)) return false;
                for (const auto& sub : shape.subpaths) {
                    if (!validPoint(sub.start + shape.offset)) {
                        *error = QStringLiteral("SVGの座標が編集可能な範囲を超えています。"); return false;
                    }
                    for (const auto& seg : sub.segments) {
                        if (!validPoint(seg.end + shape.offset)
                            || (seg.cubic && (!validPoint(seg.c1 + shape.offset) || !validPoint(seg.c2 + shape.offset)))) {
                            *error = QStringLiteral("SVGの座標が編集可能な範囲を超えています。"); return false;
                        }
                    }
                }
                bool ok = false;
                const double stroke = inherited(element, QStringLiteral("stroke-width"), QStringLiteral("1")).toDouble(&ok);
                shape.strokeWidth = ok && std::isfinite(stroke) && stroke >= 0 ? stroke : 1.;
                updateGeometry(shape);
                paintOrder.append(shapes.size());
                shapes.append(shape);
            }
            for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
                if (!visit(child, currentLayer, depth + 1)) return false;
            return true;
        };
        if (!visit(root, -1, 0)) return false;
        for (int layer = 0; layer < 2; ++layer) {
            if (groups[layer].isEmpty()) continue;
            visible[layer] = std::any_of(groups[layer].cbegin(), groups[layer].cend(), paintVisible);
        }
        bytes = input;
        return true;
    }
    bool valid(int index) const { return index >= 0 && index < shapes.size() && !shapes[index].deleted; }
    QByteArray data() const {
        if (bytes.isEmpty()) bytes = document.toByteArray(-1);
        return bytes;
    }
    void changed(bool boundsChanged = true) {
        // Mark before mutating the DOM/geometry: an allocation failure during
        // the mutation must still let cancelEdit restore the saved document.
        bytes.clear();
        dirty = true;
        if (boundsChanged) previewCandidatesValid = false;
    }
    void resetTouched() {
        for (int index : touchedShapes)
            if (index >= 0 && index < shapes.size()) clearTouched(shapes[index]);
        touchedShapes.clear();
    }
    bool editableFill(int index) const {
        return visible[1] && valid(index) && shapes[index].visible && shapes[index].layer == Layer::Fill;
    }
    const QVector<int>& nearbyShapes(const QRectF& bounds, double scale) const {
        if (previewCandidatesValid && previewCandidateBounds.contains(bounds)) return previewCandidates;
        // Most pointer moves stay in this padded region. Reuse its candidates
        // instead of scanning a million paths twice for every preview frame.
        const double padding = qMax(128., 128. / qMax(scale, .000001));
        previewCandidateBounds = bounds.adjusted(-padding, -padding, padding, padding);
        previewCandidates.clear();
        for (int index : paintOrder) {
            const auto& shape = shapes[index];
            if (shape.deleted || !shape.visible || !visible[layerIndex(shape.layer)]) continue;
            // Fill hit tests also use a narrow outline at zero tolerance.
            const double width = shape.strokeWidth;
            if (shape.geometry.controlPointRect().adjusted(-width, -width, width, width).intersects(previewCandidateBounds))
                previewCandidates.append(index);
        }
        previewCandidatesValid = true;
        return previewCandidates;
    }
    void trimHistory() {
        qsizetype total = 0;
        for (const auto& item : undo) total += item.size();
        while ((total > MaxHistoryBytes || undo.size() > 40) && undo.size() > 1) total -= undo.takeFirst().size();
    }
};

EditDocument::EditDocument() : d(std::make_unique<Private>()) {}
EditDocument::~EditDocument() = default;
EditDocument::EditDocument(EditDocument&&) noexcept = default;
EditDocument& EditDocument::operator=(EditDocument&&) noexcept = default;
QString EditDocument::memoryError(quint64 serializedBytes, quint64 availableBytes) {
    constexpr quint64 MiB = 1024ULL * 1024;
    if (serializedBytes > quint64(MaxSvgBytes))
        return QStringLiteral("SVGが編集上限（1 GiB）を超えています。");
    if (!availableBytes) {
        if (serializedBytes <= 64 * MiB) return {};
        return QStringLiteral("使用可能なメモリー量を取得できないため、64 MiBを超えるSVGは編集できません。");
    }
    // The serialized cap makes the multiplication safe. A 203 MiB document
    // with 1.06 million paths measured 10.7 GiB at the peak of an undo load.
    const quint64 required = serializedBytes * 64 + 128 * MiB;
    const quint64 budget = (availableBytes / 10) * 7 + ((availableBytes % 10) * 7) / 10;
    if (required <= budget) return {};
    const auto asMiB = [](quint64 bytes) { return bytes / MiB + (bytes % MiB != 0); };
    return QStringLiteral("ベクター編集用の空きメモリーが不足しています。追加見積り %1 MiB / "
                          "使用可能な目安 %2 MiB（現在の空き %3 MiB の70%）。"
                          "他のアプリを閉じてから再試行してください。")
        .arg(asMiB(required)).arg(asMiB(budget)).arg(asMiB(availableBytes));
}
bool EditDocument::load(const QByteArray& svg, QString* error) {
    auto next = std::make_unique<Private>();
    QString message;
    if (!next->read(svg, &message)) { if (error) *error = message; return false; }
    d = std::move(next);
    if (error) error->clear();
    return true;
}
QByteArray EditDocument::svg() const { return d->data(); }
QSize EditDocument::size() const { return d->size; }
QRectF EditDocument::viewBox() const { return QRectF(QPointF(), QSizeF(d->size)); }
int EditDocument::pathCount() const { return d->shapes.size(); }
int EditDocument::segmentCount() const { return d->segments; }
Layer EditDocument::layer(int index) const { return d->valid(index) ? d->shapes[index].layer : Layer::Lines; }
bool EditDocument::layerVisible(Layer layer) const { return d->visible[layerIndex(layer)]; }
bool EditDocument::setLayerVisible(Layer layer, bool visible) {
    const int slot = layerIndex(layer);
    if (d->visible[slot] == visible || d->document.documentElement().isNull()) return false;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    d->changed();
    if (d->groups[slot].isEmpty()) {
        // Keep an empty hidden layer in the document so newly added paths and
        // undo/redo retain the selected visibility state.
        auto group = d->document.createElementNS(QString::fromLatin1(SvgNamespace), QStringLiteral("g"));
        group.setAttribute(QStringLiteral("id"), layer == Layer::Lines ? QStringLiteral("lines") : QStringLiteral("fill"));
        auto root = d->document.documentElement();
        QDomElement before;
        if (layer == Layer::Fill) {
            for (auto child = root.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
                if (child.attribute(QStringLiteral("id")) == QStringLiteral("lines")) { before = child; break; }
        }
        if (before.isNull()) root.appendChild(group); else root.insertBefore(group, before);
        d->groups[slot].append(group);
    }
    for (auto& group : d->groups[slot]) {
        if (visible) {
            group.removeAttribute(QStringLiteral("display"));
            if (group.attribute(QStringLiteral("visibility")) == QStringLiteral("hidden")
                || group.attribute(QStringLiteral("visibility")) == QStringLiteral("collapse"))
                group.setAttribute(QStringLiteral("visibility"), QStringLiteral("visible"));
        } else group.setAttribute(QStringLiteral("display"), QStringLiteral("none"));
    }
    d->visible[slot] = visible;
    for (auto& shape : d->shapes)
        if (!shape.deleted && shape.layer == layer) shape.visible = paintVisible(shape.element);
    if (automatic) commitEdit();
    return true;
}
QPainterPath EditDocument::path(int index) const { return d->valid(index) ? d->shapes[index].geometry : QPainterPath(); }
int EditDocument::hitTest(QPointF point, double tolerance, Layer layer) const {
    if (!layerVisible(layer) || !validPoint(point) || !std::isfinite(tolerance) || tolerance < 0 || tolerance > MaxCoordinate) return -1;
    const double radius = qMax(tolerance, .0001);
    const QRectF query(point - QPointF(radius, radius), QSizeF(radius * 2, radius * 2));
    const auto& nearby = d->nearbyShapes(query, qMin(1., 7. / radius));
    for (int order = nearby.size() - 1; order >= 0; --order) {
        const int i = nearby[order];
        const auto& shape = d->shapes[i];
        if (shape.deleted || !shape.visible || shape.layer != layer) continue;
        const double width = qMax(shape.strokeWidth, tolerance * 2);
        if (!shape.geometry.controlPointRect().adjusted(-width, -width, width, width).contains(point)) continue;
        if (layer == Layer::Fill && shape.geometry.contains(point)) return i;
        QPainterPathStroker stroke;
        stroke.setWidth(width);
        stroke.setCapStyle(Qt::RoundCap);
        stroke.setJoinStyle(Qt::RoundJoin);
        if (stroke.createStroke(shape.geometry).contains(point)) return i;
    }
    return -1;
}
QVector<Handle> EditDocument::handles(int index) const {
    QVector<Handle> result;
    if (!d->valid(index)) return result;
    const auto& shape = d->shapes[index];
    for (int s = 0; s < shape.subpaths.size(); ++s) {
        const auto& sub = shape.subpaths[s];
        result.append({s, 0, Handle::Anchor, sub.start + shape.offset});
        for (int i = 0; i < sub.segments.size(); ++i) {
            auto segment = sub.segments[i];
            // Virtual straight-line controls make an L segment bendable without
            // changing its original SVG until the user actually drags a handle.
            makeCubic(endpoint(sub, i), segment);
            result.append({s, i, Handle::Control1, segment.c1 + shape.offset});
            result.append({s, i, Handle::Control2, segment.c2 + shape.offset});
            if (!(sub.closed && i == sub.segments.size() - 1 && same(segment.end, sub.start)))
                result.append({s, i + 1, Handle::Anchor, segment.end + shape.offset});
        }
    }
    return result;
}
QColor EditDocument::color(int index) const {
    if (!d->valid(index)) return {};
    const auto& shape = d->shapes[index];
    const QString attribute = shape.layer == Layer::Lines ? QStringLiteral("stroke") : QStringLiteral("fill");
    const QString value = inherited(shape.element, attribute, shape.layer == Layer::Fill ? QStringLiteral("black") : QStringLiteral("none"));
    QColor result = value == QStringLiteral("none") ? QColor(Qt::transparent) : QColor(value);
    bool ok = false;
    const double alpha = inherited(shape.element, attribute + QStringLiteral("-opacity"), QStringLiteral("1")).toDouble(&ok);
    if (result.isValid() && ok && std::isfinite(alpha)) result.setAlphaF(qBound(0., alpha, 1.));
    return result;
}
void EditDocument::beginEdit() {
    if (d->editing) return;
    d->before = d->data();
    d->editing = true;
    d->dirty = false;
    d->resetTouched();
}
void EditDocument::commitEdit() {
    if (!d->editing) return;
    // The caller can serialize the committed result on a worker. A transaction
    // itself must not rebuild hundreds of MiB of unrelated SVG on the GUI.
    if (d->dirty) {
        d->undo.append(d->before);
        d->redo.clear();
        d->trimHistory();
    }
    d->before.clear();
    d->editing = false;
    d->dirty = false;
    d->resetTouched();
}
void EditDocument::cancelEdit() {
    if (!d->editing) return;
    if (!d->dirty) {
        d->before.clear();
        d->editing = false;
        d->resetTouched();
        return;
    }
    auto next = std::make_unique<Private>();
    QString error;
    if (next->read(d->before, &error)) {
        next->undo = std::move(d->undo);
        next->redo = std::move(d->redo);
        d = std::move(next);
    }
}
bool EditDocument::canUndo() const { return !d->undo.isEmpty(); }
bool EditDocument::canRedo() const { return !d->redo.isEmpty(); }
bool EditDocument::undo() {
    if (d->editing) cancelEdit();
    if (!canUndo()) return false;
    auto next = std::make_unique<Private>();
    QString error;
    if (!next->read(d->undo.last(), &error)) return false;
    next->undo = d->undo;
    next->undo.removeLast();
    next->redo = d->redo;
    next->redo.append(d->data());
    d = std::move(next);
    return true;
}
bool EditDocument::redo() {
    if (d->editing) cancelEdit();
    if (!canRedo()) return false;
    auto next = std::make_unique<Private>();
    QString error;
    if (!next->read(d->redo.last(), &error)) return false;
    next->redo = d->redo;
    next->redo.removeLast();
    next->undo = d->undo;
    next->undo.append(d->data());
    d = std::move(next);
    return true;
}

bool EditDocument::moveHandle(int index, Handle handle, QPointF point) {
    if (!d->valid(index) || !validPoint(point)) return false;
    auto& shape = d->shapes[index];
    if (handle.subpath < 0 || handle.subpath >= shape.subpaths.size()) return false;
    auto sub = shape.subpaths[handle.subpath];
    const QPointF local = point - shape.offset;
    if (!validPoint(local) || handle.index < 0) return false;
    if (handle.kind == Handle::Anchor) {
        if (handle.index > sub.segments.size()) return false;
    } else if ((handle.kind != Handle::Control1 && handle.kind != Handle::Control2)
               || handle.index >= sub.segments.size()) return false;
    if (handle.kind == Handle::Anchor && same(local, endpoint(sub, handle.index))) return false;
    if (handle.kind != Handle::Anchor) {
        auto current = sub.segments[handle.index];
        makeCubic(endpoint(sub, handle.index), current);
        if (same(local, handle.kind == Handle::Control1 ? current.c1 : current.c2)) return false;
    }
    if (handle.kind == Handle::Anchor) {
        const bool seam = sub.closed && !sub.segments.isEmpty() && same(sub.segments.last().end, sub.start);
        const QPointF old = endpoint(sub, handle.index);
        const QPointF delta = local - old;
        int vertex = handle.index;
        if (seam && vertex == sub.segments.size()) vertex = 0;
        if (vertex == 0) {
            sub.start = local;
            if (!sub.segments.isEmpty() && sub.segments[0].cubic) sub.segments[0].c1 += delta;
            if (seam) {
                sub.segments.last().end = local;
                if (sub.segments.last().cubic) sub.segments.last().c2 += delta;
            }
        } else {
            auto& previous = sub.segments[vertex - 1];
            previous.end = local;
            if (previous.cubic) previous.c2 += delta;
            if (vertex < sub.segments.size() && sub.segments[vertex].cubic) sub.segments[vertex].c1 += delta;
        }
    } else {
        auto& segment = sub.segments[handle.index];
        makeCubic(endpoint(sub, handle.index), segment);
        if (handle.kind == Handle::Control1) segment.c1 = local;
        else segment.c2 = local;
    }
    if (!validPoint(sub.start)) return false;
    for (const auto& segment : sub.segments)
        if (!validPoint(segment.end) || !validPoint(segment.end + shape.offset)
            || (segment.cubic && (!validPoint(segment.c1) || !validPoint(segment.c2)
                || !validPoint(segment.c1 + shape.offset) || !validPoint(segment.c2 + shape.offset)))) return false;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    d->changed();
    shape.subpaths[handle.subpath] = std::move(sub);
    serialize(shape);
    if (automatic) commitEdit();
    return true;
}
int EditDocument::addLine(QPointF from, QPointF to, QColor color, double width) {
    return addBezierPath({{from, from, from + (to - from) / 3.},
                          {to, from + (to - from) * (2. / 3.), to}}, color, width);
}
int EditDocument::addBezierPath(const QVector<BezierAnchor>& anchors, QColor color, double width, bool closed) {
    const qsizetype requiredSegments = anchors.size() + (closed ? 1 : 0);
    if (anchors.size() < 2 || !color.isValid() || !std::isfinite(width) || width <= 0 || width > 10000
        || d->shapes.size() >= MaxPaths || requiredSegments > MaxSegments - d->segments
        || d->document.documentElement().isNull()) return -1;
    QDomElement group = d->groups[0].isEmpty() ? QDomElement() : d->groups[0].last();
    QPointF offset;
    if (!group.isNull() && !translateOnly(group, &offset)) return -1;
    for (const auto& anchor : anchors) {
        if (!validPoint(anchor.point) || !validPoint(anchor.incoming) || !validPoint(anchor.outgoing)
            || !validPoint(anchor.point - offset) || !validPoint(anchor.incoming - offset)
            || !validPoint(anchor.outgoing - offset)) return -1;
    }
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    d->changed();
    if (group.isNull()) {
        group = d->document.createElementNS(QString::fromLatin1(SvgNamespace), QStringLiteral("g"));
        group.setAttribute(QStringLiteral("id"), QStringLiteral("lines"));
        d->document.documentElement().appendChild(group);
        d->groups[0].append(group);
    }
    Shape shape;
    shape.element = d->document.createElementNS(QString::fromLatin1(SvgNamespace), QStringLiteral("path"));
    group.appendChild(shape.element);
    shape.layer = Layer::Lines;
    shape.visible = paintVisible(shape.element);
    shape.offset = offset;
    shape.strokeWidth = width;
    shape.element.setAttribute(QStringLiteral("fill"), QStringLiteral("none"));
    shape.element.setAttribute(QStringLiteral("stroke"), color.name(QColor::HexRgb));
    shape.element.setAttribute(QStringLiteral("stroke-opacity"), number(color.alphaF()));
    shape.element.setAttribute(QStringLiteral("stroke-width"), number(width));
    shape.element.setAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("round"));
    shape.element.setAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("round"));
    Subpath sub;
    sub.start = anchors[0].point - offset;
    sub.closed = closed;
    const int segmentCount = int(anchors.size()) - (closed ? 0 : 1);
    sub.segments.reserve(segmentCount);
    for (int i = 0; i < segmentCount; ++i) {
        const int next = (i + 1) % int(anchors.size());
        Segment segment{anchors[i].outgoing - offset, anchors[next].incoming - offset,
                        anchors[next].point - offset, true, false};
        if (same(anchors[i].outgoing, anchors[i].point)
            && same(anchors[next].incoming, anchors[next].point)) {
            // Two clicked anchors describe an exact straight segment. Give it
            // visible handles instead of hiding both under higher-priority
            // anchor hit targets, so the user can bend it after finishing.
            const auto from = anchors[i].point - offset;
            segment.c1 = from + (segment.end - from) / 3.;
            segment.c2 = from + (segment.end - from) * (2. / 3.);
        }
        sub.segments.append(segment);
    }
    shape.subpaths.append(sub);
    serialize(shape);
    const int index = d->shapes.size();
    d->paintOrder.append(index);
    d->shapes.append(shape);
    d->segments += int(requiredSegments);
    if (automatic) commitEdit();
    return index;
}
int EditDocument::erasePaths(QPointF from, QPointF to, double radius, Layer layer) {
    if (!layerVisible(layer) || !validPoint(from) || !validPoint(to)
        || !std::isfinite(radius) || radius <= 0 || radius > MaxCoordinate) return 0;
    QPainterPath brush;
    if (same(from, to)) brush.addEllipse(from, radius, radius);
    else {
        QPainterPath sweep;
        sweep.moveTo(from);
        sweep.lineTo(to);
        QPainterPathStroker stroke;
        stroke.setWidth(radius * 2.);
        stroke.setCapStyle(Qt::RoundCap);
        brush = stroke.createStroke(sweep);
    }
    const QRectF brushBounds = brush.controlPointRect();
    QVector<int> matches;
    const auto& nearby = d->nearbyShapes(brushBounds, 1.);
    for (int i : nearby) {
        const auto& shape = d->shapes[i];
        if (shape.deleted || !shape.visible || shape.layer != layer) continue;
        const double halfWidth = layer == Layer::Lines ? shape.strokeWidth / 2. : 0.;
        const QRectF bounds = shape.geometry.controlPointRect().adjusted(-halfWidth, -halfWidth, halfWidth, halfWidth);
        if (!bounds.intersects(brushBounds)) continue;
        QPainterPath painted = shape.geometry;
        if (layer == Layer::Lines) {
            // Intersect a stroke, not the implicitly closed interior of an open
            // bezier: an eraser inside a U-shaped line must not erase its rim.
            QPainterPathStroker stroke;
            stroke.setWidth(qMax(shape.strokeWidth, 1.e-6));
            stroke.setCapStyle(Qt::RoundCap);
            stroke.setJoinStyle(Qt::RoundJoin);
            painted = stroke.createStroke(shape.geometry);
        }
        if (painted.intersects(brush)) matches.append(i);
    }
    if (matches.isEmpty()) return 0;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    for (int index : matches) remove(index);
    if (automatic) commitEdit();
    return matches.size();
}
int EditDocument::eraseLineSegments(const QPainterPath& sweptRegion, QString* error) {
    QString message;
    if (error) error->clear();
    if (!finitePath(sweptRegion)) {
        if (error) *error = QStringLiteral("消しゴムの座標または点数が範囲外です。"); return -1;
    }
    if (!layerVisible(Layer::Lines) || sweptRegion.isEmpty()) return 0;
    QVector<QPair<int, Shape>> candidates;
    QHash<double, QPainterPath> expandedRegions;
    int work = 0;
    qint64 totalSegments = d->segments;
    for (int index = 0; index < d->shapes.size(); ++index) {
        const auto& source = d->shapes[index];
        if (source.deleted || !source.visible || source.layer != Layer::Lines) continue;
        const double half = source.strokeWidth / 2.;
        if (!source.geometry.controlPointRect().adjusted(-half, -half, half, half)
                .intersects(sweptRegion.controlPointRect())) continue;
        if (!expandedRegions.contains(source.strokeWidth)) {
            QPainterPathStroker stroker;
            stroker.setWidth(qMax(source.strokeWidth, 1.e-6));
            stroker.setCapStyle(Qt::RoundCap); stroker.setJoinStyle(Qt::RoundJoin);
            expandedRegions.insert(source.strokeWidth, sweptRegion.united(stroker.createStroke(sweptRegion)));
        }
        const auto& region = expandedRegions[source.strokeWidth];
        Shape result = source;
        result.subpaths.clear();
        bool changed = false;
        for (const auto& sub : source.subpaths) {
            auto segments = sub.segments;
            if (sub.closed && !segments.isEmpty() && !same(segments.last().end, sub.start)) {
                Segment closing; closing.end = sub.start; segments.append(closing);
            }
            QVector<ClippedSegment> clipped;
            QPointF from = sub.start;
            bool clippedOk = true;
            for (const auto& segment : segments) {
                QVector<ClippedSegment> pieces;
                if (!clipSegment(from, segment, source.offset, region, &pieces, &work)) { clippedOk = false; break; }
                for (int piece = 0; piece < pieces.size();) {
                    const int first = piece;
                    while (piece + 1 < pieces.size() && pieces[piece + 1].keep == pieces[first].keep) ++piece;
                    if (pieces[first].keep)
                        clipped.append(retainedInterval(from, segment, pieces[first].begin, pieces[piece].end));
                    else clipped.append({from, segment, false, pieces[first].begin, pieces[piece].end});
                    ++piece;
                }
                from = segment.end;
            }
            if (!clippedOk) {
                if (error) *error = QStringLiteral("消しゴム処理の点数が多すぎます。短いストロークに分けてください。"); return -1;
            }
            const bool removed = std::any_of(clipped.cbegin(), clipped.cend(), [](const ClippedSegment& item) { return !item.keep; });
            if (!removed) { result.subpaths.append(sub); continue; }
            changed = true;
            QVector<Subpath> pieces;
            bool continuing = false;
            for (const auto& item : clipped) {
                if (!item.keep) { continuing = false; continue; }
                if (!continuing) { Subpath piece; piece.start = item.start; pieces.append(piece); }
                pieces.last().segments.append(item.segment);
                continuing = true;
            }
            if (sub.closed && pieces.size() > 1
                && same(pieces.last().segments.last().end, pieces.first().start)) {
                Subpath joined = pieces.takeLast();
                joined.segments += pieces.takeFirst().segments;
                pieces.prepend(joined);
            }
            result.subpaths += pieces;
        }
        if (!changed) continue;
        totalSegments += shapeSegments(result) - shapeSegments(source);
        if (totalSegments > MaxSegments) {
            if (error) *error = QStringLiteral("消しゴムで分割した線が3200万区間を超えます。"); return -1;
        }
        candidates.append({index, std::move(result)});
    }
    if (candidates.isEmpty()) return 0;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    d->changed();
    for (auto& candidate : candidates) {
        if (candidate.second.subpaths.isEmpty()) remove(candidate.first);
        else {
            d->shapes[candidate.first] = std::move(candidate.second);
            serialize(d->shapes[candidate.first]);
        }
    }
    d->segments = int(totalSegments);
    if (automatic) commitEdit();
    return candidates.size();
}
int EditDocument::applyRasterFillBrush(const QPainterPath& sweptRegion, QColor color, bool erase,
                                     QString* error, int selectedFill) {
    if (error) error->clear();
    if (erase && !d->editableFill(selectedFill)) {
        if (error) *error = QStringLiteral("消しゴムで消去する色面を選択してください。"); return -1;
    }
    if (!layerVisible(Layer::Fill)) return 0;
    if (!color.isValid()) { if (error) *error = QStringLiteral("塗りの色が不正です。"); return -1; }
    QString message;
    const QRect roi = brushRoi(sweptRegion, d->size, &message, true);
    if (!message.isEmpty()) { if (error) *error = message; return -1; }
    if (roi.isEmpty()) return 0;
    QVector<QPair<int, Shape>> candidates;
    qint64 totalSegments = d->segments;
    int addedPaths = 0;
    if (erase) {
        // The selected id is authoritative; a stroke never modifies an
        // overlapping or underlying face as an accidental second target.
        const int index = selectedFill;
        {
            const auto& source = d->shapes[index];
            if (!source.geometry.controlPointRect().intersects(QRectF(roi))
                || !source.geometry.intersects(sweptRegion)) return 0;
            const QRect pathRoi = roi.intersected(source.geometry.controlPointRect().adjusted(-2, -2, 2, 2).toAlignedRect());
            QPainterPath clip; clip.addRect(QRectF(pathRoi));
            // Only touched paths enter this operation. Keep their area outside
            // the raster patch; Qt may flatten intersected boundary cubics.
            QPainterPath outside;
            const bool internalPatch = source.geometry.fillRule() == Qt::OddEvenFill
                && source.geometry.contains(QRectF(pathRoi));
            if (internalPatch) {
                // Preserve existing outer Beziers for a wholly internal patch.
                // This shortcut is restricted to evenodd geometry: overlapping
                // winding subpaths must retain their nonzero winding semantics.
                const auto eraseMask = geometryMask(sweptRegion, {}, pathRoi);
                if (eraseMask.isNull()) { if (error) *error = QStringLiteral("塗り操作の画像領域を確保できません。"); return -1; }
                const auto hole = traceRasterMask(eraseMask, pathRoi.topLeft(), &message);
                if (!message.isEmpty()) { if (error) *error = message; return -1; }
                outside = source.geometry;
                outside.addPath(hole);
            } else {
                const QImage mask = geometryMask(source.geometry, sweptRegion, pathRoi);
                if (mask.isNull()) { if (error) *error = QStringLiteral("塗り操作の画像領域を確保できません。"); return -1; }
                auto inside = traceRasterMask(mask, pathRoi.topLeft(), &message);
                if (!message.isEmpty()) { if (error) *error = message; return -1; }
                outside = source.geometry.subtracted(clip).united(inside);
            }
            outside.setFillRule(Qt::OddEvenFill);
            Shape result = source;
            result.subpaths.clear();
            if (!pathToSubpaths(outside, source.offset, &result.subpaths, &message)) {
                if (error) *error = message.isEmpty() ? QStringLiteral("塗りの境界を復元できません。") : message; return -1;
            }
            // An internal erase only introduces a hole and cannot disconnect
            // the region. Keep the compound shape intact, including any XOR
            // overlap between its original evenodd subpaths.
            const auto components = internalPatch ? QVector<QVector<Subpath>>{result.subpaths}
                                                   : fillComponents(result.subpaths, &message);
            if (!message.isEmpty()) { if (error) *error = message; return -1; }
            totalSegments -= shapeSegments(source);
            if (components.isEmpty()) { result.subpaths.clear(); candidates.append({index, result}); }
            for (int component = 0; component < components.size(); ++component) {
                result.subpaths = components[component];
                totalSegments += shapeSegments(result);
                candidates.append({component == 0 ? index : -index - 2, result});
                if (component > 0) ++addedPaths;
            }
            if (totalSegments > MaxSegments) { if (error) *error = QStringLiteral("塗りの境界が3200万区間を超えます。"); return -1; }
            if (d->shapes.size() + addedPaths > MaxPaths) { if (error) *error = QStringLiteral("分割した色面が500万パスを超えます。"); return -1; }
        }
        if (candidates.isEmpty()) return 0;
    } else {
        if (color.alpha() == 0) return 0;
        const QImage mask = geometryMask(sweptRegion, {}, roi);
        if (mask.isNull()) { if (error) *error = QStringLiteral("塗り操作の画像領域を確保できません。"); return -1; }
        const auto traced = traceRasterMask(mask, roi.topLeft(), &message);
        if (!message.isEmpty()) { if (error) *error = message; return -1; }
        if (traced.isEmpty()) return 0;
        Shape shape;
        shape.layer = Layer::Fill;
        if (!d->groups[1].isEmpty() && !translateOnly(d->groups[1].last(), &shape.offset)) {
            if (error) *error = QStringLiteral("塗りレイヤーの座標変換に対応していません。"); return -1;
        }
        if (!pathToSubpaths(traced, shape.offset, &shape.subpaths, &message)) {
            if (error) *error = message.isEmpty() ? QStringLiteral("塗りの境界を復元できません。") : message; return -1;
        }
        totalSegments += shapeSegments(shape);
        if (totalSegments > MaxSegments || d->shapes.size() >= MaxPaths) {
            if (error) *error = QStringLiteral("塗りの追加が編集可能な点数・パス数を超えます。"); return -1;
        }
        candidates.append({-1, std::move(shape)});
    }
    // All masks and contours succeeded before the first DOM mutation. Failure
    // leaves every layer, path and undo history exactly as it was.
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    d->changed();
    for (auto& candidate : candidates) {
        if (candidate.first < -1) {
            const int sourceIndex = -candidate.first - 2;
            auto& shape = candidate.second;
            const auto sourceElement = d->shapes[sourceIndex].element;
            shape.element = sourceElement.cloneNode(true).toElement();
            shape.element.removeAttribute(QStringLiteral("id"));
            shape.element.setAttribute(QStringLiteral("fill-rule"), QStringLiteral("evenodd"));
            sourceElement.parentNode().insertAfter(shape.element, sourceElement);
            serialize(shape);
            d->paintOrder.insert(d->paintOrder.indexOf(sourceIndex) + 1, d->shapes.size());
            d->shapes.append(std::move(shape));
        } else if (candidate.first == -1) {
            QDomElement group;
            if (d->groups[1].isEmpty()) {
                group = d->document.createElementNS(QString::fromLatin1(SvgNamespace), QStringLiteral("g"));
                group.setAttribute(QStringLiteral("id"), QStringLiteral("fill"));
                auto root = d->document.documentElement();
                QDomNode before;
                for (auto child = root.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
                    if (child.attribute(QStringLiteral("id")) == QStringLiteral("lines")) { before = child; break; }
                if (before.isNull()) root.appendChild(group); else root.insertBefore(group, before);
                d->groups[1].append(group);
            } else group = d->groups[1].last();
            auto& shape = candidate.second;
            shape.element = d->document.createElementNS(QString::fromLatin1(SvgNamespace), QStringLiteral("path"));
            shape.element.setAttribute(QStringLiteral("fill"), color.name(QColor::HexRgb));
            shape.element.setAttribute(QStringLiteral("fill-opacity"), number(color.alphaF()));
            shape.element.setAttribute(QStringLiteral("stroke"), QStringLiteral("none"));
            group.appendChild(shape.element);
            shape.visible = paintVisible(shape.element);
            shape.element.setAttribute(QStringLiteral("fill-rule"), QStringLiteral("evenodd"));
            serialize(shape);
            d->paintOrder.append(d->shapes.size());
            d->shapes.append(std::move(shape));
        } else if (candidate.second.subpaths.isEmpty()) remove(candidate.first);
        else {
            d->shapes[candidate.first] = std::move(candidate.second);
            auto& shape = d->shapes[candidate.first];
            shape.element.setAttribute(QStringLiteral("fill-rule"), QStringLiteral("evenodd"));
            serialize(shape);
        }
    }
    d->segments = int(totalSegments);
    if (automatic) commitEdit();
    return candidates.size();
}
QImage EditDocument::rasterBrushPreview(const QPainterPath& sweptRegion, QColor color, bool erase,
                                       QRectF* bounds, QString* error, double outputScale, int selectedFill) const {
    if (error) error->clear();
    if (bounds) *bounds = {};
    if (erase && !d->editableFill(selectedFill)) {
        if (error) *error = QStringLiteral("消しゴムで消去する色面を選択してください。"); return {};
    }
    if (!color.isValid() || !std::isfinite(outputScale) || outputScale <= 0) {
        if (error) *error = QStringLiteral("ブラシプレビューの色・倍率が不正です。"); return {};
    }
    QString message;
    QRect roi = brushRoi(sweptRegion, d->size, &message, false);
    if (!message.isEmpty()) { if (error) *error = message; return {}; }
    if (erase) roi = roi.intersected(d->shapes[selectedFill].geometry.controlPointRect().adjusted(-2, -2, 2, 2).toAlignedRect());
    if (roi.isEmpty()) return {};
    const double scale = qMin(outputScale, qMin(1., std::sqrt((2. * 1024 * 1024) / (double(roi.width()) * roi.height()))));
    const QSize pixels(qMax(1, int(std::floor(roi.width() * scale))), qMax(1, int(std::floor(roi.height() * scale))));
    auto makeImage = [&] { QImage image(pixels, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); return image; };
    QImage result = makeImage(), fill = makeImage(), lines = makeImage();
    QImage selected = erase ? makeImage() : QImage();
    if (result.isNull() || fill.isNull() || lines.isNull() || (erase && selected.isNull())) {
        if (error) *error = QStringLiteral("ブラシプレビューの画像領域を確保できません。"); return {};
    }
    const auto transform = roiTransform(roi, pixels);
    const auto& nearby = d->nearbyShapes(QRectF(roi), scale);
    auto paintLayer = [&](QImage& destination, Layer layer) {
        if (!layerVisible(layer)) return;
        QPainter painter(&destination);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setTransform(transform);
        for (int index : nearby) {
            const auto& shape = d->shapes[index];
            if (shape.deleted || !shape.visible || shape.layer != layer
                || !shape.geometry.controlPointRect().adjusted(-shape.strokeWidth, -shape.strokeWidth,
                        shape.strokeWidth, shape.strokeWidth).intersects(QRectF(roi))) continue;
            bool valid = false;
            const double ownOpacity = shape.element.attribute(QStringLiteral("opacity"), QStringLiteral("1")).toDouble(&valid);
            painter.setOpacity(valid ? qBound(0., ownOpacity, 1.) : 1.);
            if (layer == Layer::Fill) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(paintColor(shape.element, QStringLiteral("fill"), QStringLiteral("black")));
            } else {
                QPen pen(paintColor(shape.element, QStringLiteral("stroke"), QStringLiteral("none")), shape.strokeWidth);
                pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin);
                painter.setPen(pen); painter.setBrush(Qt::NoBrush);
            }
            if (erase && layer == Layer::Fill && index == selectedFill) {
                // Clear the brush from this one face before source-over
                // compositing it at its original position in the layer stack.
                QPainter selectedPainter(&selected);
                selectedPainter.setRenderHint(QPainter::Antialiasing);
                selectedPainter.setTransform(transform);
                selectedPainter.setPen(Qt::NoPen);
                selectedPainter.setBrush(painter.brush());
                selectedPainter.drawPath(shape.geometry);
                selectedPainter.setCompositionMode(QPainter::CompositionMode_Clear);
                selectedPainter.drawPath(sweptRegion);
                selectedPainter.end();
                painter.save(); painter.resetTransform();
                painter.drawImage(QPoint(), selected); painter.restore();
            } else painter.drawPath(shape.geometry);
        }
    };
    paintLayer(fill, Layer::Fill);
    if (layerVisible(Layer::Fill) && !erase) {
        QPainter painter(&fill);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setTransform(transform);
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        painter.drawPath(sweptRegion);
    }
    paintLayer(lines, Layer::Lines);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(transform);
    for (auto child = d->document.documentElement().firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.attribute(QStringLiteral("id")) != QStringLiteral("background") || !paintVisible(child)) continue;
        painter.fillRect(QRectF(QPointF(), QSizeF(d->size)), paintColor(child, QStringLiteral("fill"), QStringLiteral("black")));
    }
    painter.resetTransform();
    const auto layerOpacity = [&](int layer) {
        return d->groups[layer].isEmpty() ? 1. : opacityAttribute(d->groups[layer].first(), QStringLiteral("opacity"));
    };
    painter.setOpacity(layerOpacity(1)); painter.drawImage(QPoint(), fill);
    painter.setOpacity(layerOpacity(0)); painter.drawImage(QPoint(), lines);
    painter.end();
    if (bounds) *bounds = QRectF(roi);
    return result;
}
bool EditDocument::remove(int index) {
    if (!d->valid(index)) return false;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    d->changed(false);
    auto& shape = d->shapes[index];
    shape.element.parentNode().removeChild(shape.element);
    shape.deleted = true;
    if (automatic) commitEdit();
    return true;
}
bool EditDocument::setColor(int index, QColor color) {
    if (!d->valid(index) || !color.isValid()) return false;
    auto& shape = d->shapes[index];
    const QString attribute = shape.layer == Layer::Lines ? QStringLiteral("stroke") : QStringLiteral("fill");
    const QString opacity = attribute + QStringLiteral("-opacity");
    if (shape.element.attribute(attribute) == color.name(QColor::HexRgb)
        && shape.element.hasAttribute(opacity)
        && std::abs(shape.element.attribute(opacity).toDouble() - color.alphaF()) < 1.e-10) return false;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    d->changed(false);
    shape.element.setAttribute(attribute, color.name(QColor::HexRgb));
    shape.element.setAttribute(attribute + QStringLiteral("-opacity"), number(color.alphaF()));
    if (automatic) commitEdit();
    return true;
}

bool EditDocument::deformFill(int index, QPointF center, QPointF delta, double radius, bool smooth) {
    if (!d->valid(index) || d->shapes[index].layer != Layer::Fill || !validPoint(center) || !validPoint(delta)
        || !std::isfinite(radius) || radius <= 0 || radius > MaxCoordinate) return false;
    // Work on a copy so a size limit or invalid coordinate never partly edits a path.
    Shape candidate = d->shapes[index];
    center -= candidate.offset;
    const bool displace = length(delta) > 1.e-9;
    const double deltaLength = length(delta);
    if (deltaLength > radius * .5) delta *= radius * .5 / deltaLength;
    const QRectF brush(center - QPointF(radius, radius), QSizeF(radius * 2, radius * 2));
    int added = 0;
    bool changed = false;
    bool limit = false;
    for (auto& sub : candidate.subpaths) {
        if (sub.segments.isEmpty()) continue;
        if (displace) {
            QVector<Segment> refined;
            const bool explicitSeam = sub.closed && same(sub.segments.last().end, sub.start);
            auto input = sub.segments;
            if (sub.closed && !explicitSeam) {
                // An implicit closing edge also needs vertices when the brush meets it.
                QPainterPath closing;
                closing.moveTo(input.last().end);
                closing.lineTo(sub.start);
                if (closing.controlPointRect().adjusted(-radius, -radius, radius, radius).contains(center)) {
                    Segment close;
                    close.end = sub.start;
                    input.append(close);
                    ++added;
                }
            }
            std::function<void(QPointF, Segment, int)> subdivide = [&](QPointF from, Segment segment, int depth) {
                if (limit) return;
                QPainterPath bounds;
                bounds.moveTo(from);
                if (segment.cubic) bounds.cubicTo(segment.c1, segment.c2, segment.end);
                else bounds.lineTo(segment.end);
                // Give zero-width horizontal/vertical edge bounds a nonempty area.
                const bool meets = bounds.controlPointRect().adjusted(-.001, -.001, .001, .001).intersects(brush);
                const double span = segment.cubic
                    ? length(segment.c1 - from) + length(segment.c2 - segment.c1) + length(segment.end - segment.c2)
                    : length(segment.end - from);
                if (!meets || span <= radius * .4 || depth >= 12) { refined.append(segment); return; }
                if (d->segments + ++added > MaxSegments) { limit = true; return; }
                makeCubic(from, segment);
                const QPointF a = (from + segment.c1) / 2.;
                const QPointF b = (segment.c1 + segment.c2) / 2.;
                const QPointF c = (segment.c2 + segment.end) / 2.;
                const QPointF ab = (a + b) / 2.;
                const QPointF bc = (b + c) / 2.;
                const QPointF middle = (ab + bc) / 2.;
                Segment left{a, ab, middle, true, segment.touched};
                Segment right{bc, c, segment.end, true, segment.touched};
                subdivide(from, left, depth + 1);
                subdivide(middle, right, depth + 1);
            };
            QPointF from = sub.start;
            for (const auto& segment : input) {
                subdivide(from, segment, 0);
                from = segment.end;
            }
            if (limit || d->segments + added > MaxSegments) return false;
            sub.segments = std::move(refined);
            auto move = [&](QPointF& point, bool* touched) {
                const double distance = length(point - center) / radius;
                if (distance >= 1.) return;
                const double weight = std::pow(1. - distance * distance, 2);
                point += delta * weight;
                if (weight > 1.e-8) { *touched = true; changed = true; }
            };
            const bool closed = sub.closed && same(sub.segments.last().end, sub.start);
            move(sub.start, &sub.startTouched);
            for (auto& segment : sub.segments) {
                if (segment.cubic) {
                    bool controlTouched = false;
                    move(segment.c1, &controlTouched);
                    move(segment.c2, &controlTouched);
                }
                move(segment.end, &segment.touched);
            }
            if (closed) sub.segments.last().end = sub.start;
        }
        if (smooth && sub.segments.size() > 1) {
            const bool seam = sub.closed && same(sub.segments.last().end, sub.start);
            const int vertexCount = sub.segments.size() + (seam ? 0 : 1);
            const auto old = sub;
            for (int vertex = 0; vertex < vertexCount; ++vertex) {
                const bool touched = vertex == 0 ? old.startTouched : old.segments[vertex - 1].touched;
                if (!touched || (!seam && (vertex == 0 || vertex == vertexCount - 1))) continue;
                const int previousVertex = (vertex + vertexCount - 1) % vertexCount;
                const int nextVertex = (vertex + 1) % vertexCount;
                const QPointF anchor = endpoint(old, vertex);
                const QPointF previous = endpoint(old, previousVertex);
                const QPointF next = endpoint(old, nextVertex);
                const QPointF tangent = next - previous;
                const double tangentLength = length(tangent);
                if (tangentLength < 1.e-8) continue;
                // Short, symmetric tangent handles smooth the affected boundary while
                // fixing its anchors and every unrelated subpath (including holes).
                const double handleLength = qMin(qMin(length(anchor - previous), length(next - anchor)) / 3., radius * .3);
                const QPointF handle = tangent * (handleLength / tangentLength);
                const int incoming = (vertex + sub.segments.size() - 1) % sub.segments.size();
                const int outgoing = vertex % sub.segments.size();
                makeCubic(previous, sub.segments[incoming]);
                makeCubic(anchor, sub.segments[outgoing]);
                sub.segments[incoming].c2 = anchor - handle;
                sub.segments[outgoing].c1 = anchor + handle;
                changed = true;
            }
        }
        if (!validPoint(sub.start) || !validPoint(sub.start + candidate.offset)) return false;
        for (const auto& segment : sub.segments)
            if (!validPoint(segment.end) || !validPoint(segment.end + candidate.offset)
                || (segment.cubic && (!validPoint(segment.c1) || !validPoint(segment.c2)
                    || !validPoint(segment.c1 + candidate.offset) || !validPoint(segment.c2 + candidate.offset)))) return false;
    }
    if (!changed) return false;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    d->changed();
    d->shapes[index] = std::move(candidate);
    d->touchedShapes.insert(index);
    serialize(d->shapes[index]);
    d->segments += added;
    if (automatic) commitEdit();
    return true;
}
}
