// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit_document.h"

#include <QDomDocument>
#include <QPainterPathStroker>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <functional>

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
void clearTouched(QVector<Shape>& shapes) {
    for (auto& shape : shapes) for (auto& sub : shape.subpaths) {
        sub.startTouched = false;
        for (auto& segment : sub.segments) segment.touched = false;
    }
}
}

struct EditDocument::Private {
    QDomDocument document;
    QVector<Shape> shapes;
    QSize size;
    mutable QByteArray bytes;
    QByteArray before;
    QList<QByteArray> undo, redo;
    bool editing = false;
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
            if (currentLayer >= 0 && tag(element) == QStringLiteral("path")) {
                if (shapes.size() >= MaxPaths) { *error = QStringLiteral("編集可能なパス数（500万）を超えています。"); return false; }
                Shape shape;
                shape.element = element;
                shape.layer = currentLayer == 0 ? Layer::Lines : Layer::Fill;
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
                shapes.append(shape);
            }
            for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
                if (!visit(child, currentLayer, depth + 1)) return false;
            return true;
        };
        if (!visit(root, -1, 0)) return false;
        bytes = input;
        return true;
    }
    bool valid(int index) const { return index >= 0 && index < shapes.size() && !shapes[index].deleted; }
    QByteArray data() const {
        if (bytes.isEmpty()) bytes = document.toByteArray(-1);
        return bytes;
    }
    void changed() { bytes.clear(); }
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
Layer EditDocument::layer(int index) const { return d->valid(index) ? d->shapes[index].layer : Layer::Lines; }
QPainterPath EditDocument::path(int index) const { return d->valid(index) ? d->shapes[index].geometry : QPainterPath(); }
int EditDocument::hitTest(QPointF point, double tolerance, Layer layer) const {
    if (!validPoint(point) || !std::isfinite(tolerance) || tolerance < 0 || tolerance > MaxCoordinate) return -1;
    for (int i = d->shapes.size() - 1; i >= 0; --i) {
        const auto& shape = d->shapes[i];
        if (shape.deleted || shape.layer != layer) continue;
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
    clearTouched(d->shapes);
}
void EditDocument::commitEdit() {
    if (!d->editing) return;
    if (d->data() != d->before) {
        d->undo.append(d->before);
        d->redo.clear();
        d->trimHistory();
    }
    d->before.clear();
    d->editing = false;
    clearTouched(d->shapes);
}
void EditDocument::cancelEdit() {
    if (!d->editing) return;
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
    shape.subpaths[handle.subpath] = std::move(sub);
    serialize(shape);
    d->changed();
    if (automatic) commitEdit();
    return true;
}
int EditDocument::addLine(QPointF from, QPointF to, QColor color, double width) {
    if (!validPoint(from) || !validPoint(to) || !color.isValid() || !std::isfinite(width) || width <= 0 || width > 10000
        || d->shapes.size() >= MaxPaths || d->segments + 2 > MaxSegments || d->document.documentElement().isNull()) return -1;
    QDomElement group;
    const auto groups = d->document.elementsByTagName(QStringLiteral("g"));
    for (int i = 0; i < groups.size(); ++i)
        if (groups.at(i).toElement().attribute(QStringLiteral("id")) == QStringLiteral("lines")) { group = groups.at(i).toElement(); break; }
    QPointF offset;
    if ((!group.isNull() && !translateOnly(group, &offset))
        || !validPoint(from - offset) || !validPoint(to - offset)) return -1;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    if (group.isNull()) {
        group = d->document.createElementNS(QString::fromLatin1(SvgNamespace), QStringLiteral("g"));
        group.setAttribute(QStringLiteral("id"), QStringLiteral("lines"));
        d->document.documentElement().appendChild(group);
    }
    Shape shape;
    shape.element = d->document.createElementNS(QString::fromLatin1(SvgNamespace), QStringLiteral("path"));
    group.appendChild(shape.element);
    shape.layer = Layer::Lines;
    shape.offset = offset;
    shape.strokeWidth = width;
    shape.element.setAttribute(QStringLiteral("fill"), QStringLiteral("none"));
    shape.element.setAttribute(QStringLiteral("stroke"), color.name(QColor::HexRgb));
    shape.element.setAttribute(QStringLiteral("stroke-opacity"), number(color.alphaF()));
    shape.element.setAttribute(QStringLiteral("stroke-width"), number(width));
    shape.element.setAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("round"));
    Subpath sub;
    sub.start = from - shape.offset;
    Segment segment;
    segment.end = to - shape.offset;
    makeCubic(sub.start, segment);
    sub.segments.append(segment);
    shape.subpaths.append(sub);
    serialize(shape);
    const int index = d->shapes.size();
    d->shapes.append(shape);
    d->segments += 2;
    d->changed();
    if (automatic) commitEdit();
    return index;
}
bool EditDocument::remove(int index) {
    if (!d->valid(index)) return false;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    auto& shape = d->shapes[index];
    shape.element.parentNode().removeChild(shape.element);
    shape.deleted = true;
    d->changed();
    if (automatic) commitEdit();
    return true;
}
bool EditDocument::setColor(int index, QColor color) {
    if (!d->valid(index) || !color.isValid()) return false;
    const bool automatic = !d->editing;
    if (automatic) beginEdit();
    auto& shape = d->shapes[index];
    const QString attribute = shape.layer == Layer::Lines ? QStringLiteral("stroke") : QStringLiteral("fill");
    shape.element.setAttribute(attribute, color.name(QColor::HexRgb));
    shape.element.setAttribute(attribute + QStringLiteral("-opacity"), number(color.alphaF()));
    d->changed();
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
    d->shapes[index] = std::move(candidate);
    serialize(d->shapes[index]);
    d->segments += added;
    d->changed();
    if (automatic) commitEdit();
    return true;
}
}
