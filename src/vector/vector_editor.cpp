// SPDX-License-Identifier: GPL-3.0-or-later
#include "vector_editor.h"
#include "memory_budget.h"
#include "generated_svg_renderer.h"
#include "qvgraphicsview.h"
#include "sr/color_pipeline.h"

#include <QAbstractSpinBox>
#include <QAbstractSlider>
#include <QApplication>
#include <QComboBox>
#include <QColorSpace>
#include <QKeyEvent>
#include <QHash>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextEdit>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>
#include <exception>

namespace Vector {
namespace {
constexpr qsizetype AsyncHistoryThreshold = 8 * 1024 * 1024;

struct HistoryResult {
    bool changed = false;
    QString error;
};

struct PickedColor {
    QColor color;
    QString error;
};

PickedColor sampleSvgColor(const QByteArray& svg, const QPointF& point)
{
    PickedColor result;
    GeneratedSvgRenderer renderer(svg);
    if (!renderer.isValid()) { result.error = renderer.errorString(); return result; }
    QImage pixel(3, 3, QImage::Format_ARGB32_Premultiplied);
    pixel.setColorSpace(QColorSpace::SRgb);
    pixel.fill(Qt::transparent);
    // Sample the composited source SVG, never monitor-profile screenshot RGB.
    if (!renderer.render(pixel, QTransform::fromTranslate(1.5 - point.x(), 1.5 - point.y()), &result.error)) return result;
    result.color = pixel.pixelColor(1, 1);
    return result;
}

QPointF position(const QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

bool editingKey(const QKeyEvent* event)
{
    const int key = event->key();
    if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Space
        || key == Qt::Key_Delete || key == Qt::Key_Backspace || key == Qt::Key_Escape
        || key == Qt::Key_Return || key == Qt::Key_Enter)
        return true;
    return (event->modifiers() & Qt::ControlModifier)
        && (key == Qt::Key_Z || key == Qt::Key_Y);
}
}

Editor::Editor(QVGraphicsView* view) : QObject(view), view_(view)
{
    setObjectName(QStringLiteral("vectorEditor"));
    qApp->installEventFilter(this);
    connect(view, &QVGraphicsView::vectorEditorOverlay, this, &Editor::paint,
            Qt::DirectConnection);
    connect(&view->getImageCore(), &QVImageCore::sourceChanging, this, &Editor::end);
    brushPreviewTimer_.setSingleShot(true);
    brushPreviewTimer_.setInterval(75);
    connect(&brushPreviewTimer_, &QTimer::timeout, this, &Editor::updateBrushPreview);
}

Editor::~Editor()
{
    if (qApp) qApp->removeEventFilter(this);
}

bool Editor::begin(const QByteArray& svg, QString* error)
{
    if (!view_) return false;
    const auto memoryError = EditDocument::memoryError(quint64(svg.size()), availableMemoryBytes());
    if (!memoryError.isEmpty()) {
        if (error) *error = memoryError;
        return false;
    }
    auto prepared = std::make_shared<EditDocument>();
    if (!prepared->load(svg, error)) return false;
    return beginPrepared(svg, std::move(prepared), error);
}

bool Editor::beginPrepared(const QByteArray& svg, std::shared_ptr<EditDocument> prepared,
                           QString* error)
{
    if (!view_) return false;
    // Validate before touching an active document. svg() is the already cached
    // input byte array here; adoption never reparses the SVG on the GUI thread.
    if (!prepared || prepared->size().isEmpty() || svg.isEmpty() || prepared->svg() != svg) {
        if (error) *error = QStringLiteral("準備した編集データがSVGと一致しません。");
        return false;
    }
    cancelGesture();
    // Leaving and re-entering edit mode keeps the current undo history. A new
    // generated result or another file starts its own history through reset().
    if (document_.size().isEmpty() || document_.svg() != svg) {
        document_ = std::move(*prepared);
        *prepared = EditDocument();
    }
    if (active_) end();
    active_ = true;
    selected_ = -1;
    spaceDown_ = false;
    savedDragMode_ = view_->dragMode();
    savedMouseTracking_ = view_->viewport()->hasMouseTracking();
    view_->setDragMode(QGraphicsView::NoDrag);
    view_->viewport()->setMouseTracking(true);
    view_->setProperty("vectorEditing", true);
    emit view_->cancelSlideshow();
    updateCursor();
    view_->viewport()->update();
    if (error) error->clear();
    emit stateChanged();
    return true;
}

void Editor::end()
{
    // Forced exits (source change or SR) keep the controller's last committed
    // SVG and ignore an in-flight history operation when it eventually ends.
    ++historyRevision_;
    historyBusy_ = false;
    historySvg_.clear();
    if (!active_) return;
    cancelGesture();
    cancelPen();
    active_ = false;
    spaceDown_ = false;
    selected_ = -1;
    if (view_) {
        view_->setProperty("vectorEditing", false);
        view_->setDragMode(savedDragMode_);
        view_->viewport()->setMouseTracking(savedMouseTracking_);
        view_->viewport()->setCursor(Qt::ArrowCursor);
        view_->viewport()->update();
    }
    emit stateChanged();
}

void Editor::reset()
{
    end();
    document_ = EditDocument();
    selected_ = -1;
    emit stateChanged();
}

QByteArray Editor::svg() const
{
    return historyBusy_ ? historySvg_ : document_.svg();
}

void Editor::setLayer(EditDocument::Layer layer)
{
    if (historyBusy_) return;
    if (layer_ == layer) return;
    cancelGesture();
    cancelPen(true);
    layer_ = layer;
    selected_ = -1;
    if (layer != EditDocument::Layer::Lines) addLine_ = false;
    updateCursor();
    if (view_) view_->viewport()->update();
    emit stateChanged();
}

void Editor::setTool(Tool tool)
{
    if (historyBusy_ || (tool_ == tool && !addLine_)) return;
    cancelGesture();
    cancelPen(true);
    tool_ = tool;
    addLine_ = false;
    selected_ = -1;
    updateCursor();
    if (view_) view_->viewport()->update();
    emit stateChanged();
}

bool Editor::layerVisible(EditDocument::Layer layer) const
{
    if (historyBusy_) return layer == EditDocument::Layer::Lines ? historyLinesVisible_ : historyFillVisible_;
    return document_.layerVisible(layer);
}

void Editor::setLayerVisible(EditDocument::Layer layer, bool visible)
{
    if (!active_ || historyBusy_ || document_.layerVisible(layer) == visible) return;
    cancelGesture();
    if (!visible && layer == EditDocument::Layer::Lines) cancelPen(true);
    if (layer == layer_) selected_ = -1;
    document_.beginEdit();
    if (document_.setLayerVisible(layer, visible)) {
        document_.commitEdit();
        notifyChanged();
    } else document_.cancelEdit();
}

void Editor::cancelPen(bool explain)
{
    if (penAnchors_.isEmpty()) return;
    penAnchors_.clear();
    if (view_) view_->viewport()->update();
    emit stateChanged();
    if (explain) emit message(QStringLiteral("未確定の線を取り消しました。"));
}

void Editor::removeLastPenAnchor()
{
    if (penAnchors_.isEmpty()) return;
    if (gesture_ == Gesture::PenAnchor) gesture_ = Gesture::None;
    penAnchors_.removeLast();
    if (view_) view_->viewport()->update();
    emit stateChanged();
}

void Editor::updatePenTangent(const QPointF& viewportPoint)
{
    if (penAnchors_.isEmpty()) return;
    auto& anchor = penAnchors_.last();
    const QPointF delta = QLineF(mapping().map(anchor.point), viewportPoint).length() >= 2.
        ? toDocument(viewportPoint) - anchor.point : QPointF();
    anchor.incoming = anchor.point - delta;
    anchor.outgoing = anchor.point + delta;
}

void Editor::finishPen()
{
    if (!active_ || historyBusy_ || penAnchors_.isEmpty()) return;
    if (penAnchors_.size() < 2) {
        emit message(QStringLiteral("線を確定するには、2点以上を置いてください。"));
        return;
    }
    if (!document_.layerVisible(EditDocument::Layer::Lines)) return;
    if (gesture_ == Gesture::PenAnchor) gesture_ = Gesture::None;
    document_.beginEdit();
    const int path = document_.addBezierPath(penAnchors_, color_, lineWidth_);
    if (path < 0) {
        document_.cancelEdit();
        emit message(QStringLiteral("線を追加できませんでした。点の数や座標の範囲を確認してください。"));
        return;
    }
    selected_ = path;
    penAnchors_.clear();
    document_.commitEdit();
    notifyChanged();
    emit message(QStringLiteral("線を確定しました。選択ツールで各点やベジエハンドルを調整できます。"));
}

void Editor::setAddLine(bool enabled)
{
    if (historyBusy_) return;
    cancelGesture();
    cancelPen(true);
    tool_ = Tool::Select;
    addLine_ = enabled;
    if (enabled) {
        layer_ = EditDocument::Layer::Lines;
        selected_ = -1;
    }
    updateCursor();
    if (view_) view_->viewport()->update();
    emit stateChanged();
}

void Editor::setColor(const QColor& color)
{
    if (historyBusy_ || !color.isValid()) return;
    color_ = color;
    if (!active_ || selected_ < 0 || tool_ != Tool::Select) { if (view_) view_->viewport()->update(); return; }
    cancelGesture();
    document_.beginEdit();
    if (document_.setColor(selected_, color)) {
        document_.commitEdit();
        notifyChanged();
    } else document_.cancelEdit();
}

void Editor::setBrushRadius(double radius)
{
    if (std::isfinite(radius)) brushRadius_ = qBound(0.25, radius, 10000.);
    if (view_) view_->viewport()->update();
}

void Editor::setLineWidth(double width)
{
    if (std::isfinite(width)) lineWidth_ = qBound(0.1, width, 1000.);
}

void Editor::deleteSelection()
{
    if (!active_ || historyBusy_ || selected_ < 0) return;
    cancelGesture();
    document_.beginEdit();
    if (document_.remove(selected_)) {
        document_.commitEdit();
        selected_ = -1;
        notifyChanged();
    } else document_.cancelEdit();
}

void Editor::undo()
{
    changeHistory(false);
}

void Editor::redo()
{
    changeHistory(true);
}

void Editor::changeHistory(bool redo)
{
    if (!active_ || historyBusy_) return;
    if (hasPendingPen()) {
        if (!redo) removeLastPenAnchor();
        else emit message(QStringLiteral("線を確定するか、Escapeで取り消してから履歴を操作してください。"));
        return;
    }
    cancelGesture();
    if (redo ? !document_.canRedo() : !document_.canUndo()) return;
    const auto currentSvg = document_.svg();
    const auto memoryError = EditDocument::memoryError(quint64(currentSvg.size()), availableMemoryBytes());
    if (!memoryError.isEmpty()) { emit message(memoryError); return; }
    selected_ = -1;
    if (currentSvg.size() <= AsyncHistoryThreshold) {
        if (redo ? document_.redo() : document_.undo()) notifyChanged();
        return;
    }

    // The history operation reparses a snapshot. Move sole ownership of all
    // DOM/geometry state to the worker so no GUI read races with that parser.
    historyLinesVisible_ = document_.layerVisible(EditDocument::Layer::Lines);
    historyFillVisible_ = document_.layerVisible(EditDocument::Layer::Fill);
    auto model = std::make_shared<EditDocument>(std::move(document_));
    document_ = EditDocument();
    historySvg_ = currentSvg;
    historyBusy_ = true;
    const auto revision = ++historyRevision_;
    updateCursor();
    view_->viewport()->update();
    emit stateChanged();
    emit message(redo ? QStringLiteral("やり直しを処理中…") : QStringLiteral("取り消しを処理中…"));
    auto* watcher = new QFutureWatcher<HistoryResult>(this);
    connect(watcher, &QFutureWatcher<HistoryResult>::finished, this, [this, watcher, model, revision] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (revision != historyRevision_ || !historyBusy_) return;
        document_ = std::move(*model);
        *model = EditDocument();
        historyBusy_ = false;
        historySvg_.clear();
        updateCursor();
        if (view_) view_->viewport()->update();
        if (result.changed) notifyChanged();
        else emit stateChanged();
        if (!result.error.isEmpty()) emit message(result.error);
    });
    watcher->setFuture(QtConcurrent::run([model, redo] {
        HistoryResult result;
        try {
            result.changed = redo ? model->redo() : model->undo();
            if (!result.changed) result.error = QStringLiteral("編集履歴を復元できませんでした。現在の編集結果を保持しています。");
        } catch (const std::exception&) {
            result.error = QStringLiteral("編集履歴の復元中にエラーが発生しました。現在の編集結果を保持しています。");
        } catch (...) {
            result.error = QStringLiteral("編集履歴の復元中にエラーが発生しました。現在の編集結果を保持しています。");
        }
        return result;
    }));
}

QPainterPath Editor::brushRegion() const
{
    QPainterPathStroker stroker;
    stroker.setWidth(strokeRadius_ * 2.);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    QPainterPath region = stroker.createStroke(eraserTrace_);
    region.setFillRule(Qt::WindingFill);
    // A click has no segments for QPainterPathStroker to expand.
    if (eraserTrace_.elementCount() <= 1)
        region.addEllipse(pressDocument_, strokeRadius_, strokeRadius_);
    return region;
}

void Editor::updateBrushPreview()
{
    if (!active_ || historyBusy_ || gesture_ != Gesture::Brush || strokeLayer_ != EditDocument::Layer::Fill) return;
    QString error;
    QRectF bounds;
    // Source resolution is sufficient when zoomed in. Zoomed-out previews need
    // only viewport resolution; the model additionally caps this at two MP.
    const double scale = qMin(1., 1. / sourceTolerance(1.));
    QImage preview = document_.rasterBrushPreview(brushRegion(), strokeColor_, strokeErase_, &bounds, &error, scale);
    if (!preview.isNull() && !brushDisplayIcc_.isEmpty() && brushDisplayIcc_ != Sr::srgbProfile())
        preview = Sr::convert(preview, Sr::Profile{Sr::srgbProfile(), QStringLiteral("sRGB"), {}, false}, brushDisplayIcc_, &error);
    if (!preview.isNull()) {
        brushPreview_ = preview;
        brushPreviewBounds_ = bounds;
    } else if (!error.isEmpty() && !previewErrorShown_) {
        previewErrorShown_ = true;
        emit message(error);
    }
    if (view_) view_->viewport()->update();
}

void Editor::finishBrushStroke()
{
    if (gesture_ != Gesture::Brush) return;
    brushPreviewTimer_.stop();
    const auto region = brushRegion();
    const auto layer = strokeLayer_;
    const auto color = strokeColor_;
    const bool erase = strokeErase_;
    gesture_ = Gesture::None;
    eraserTrace_ = {};
    brushPreview_ = {};
    brushPreviewBounds_ = {};
    selected_ = -1;
    const auto currentSvg = document_.svg();
    const auto memoryError = EditDocument::memoryError(quint64(currentSvg.size()), availableMemoryBytes());
    if (!memoryError.isEmpty()) {
        emit message(memoryError);
        if (view_) view_->viewport()->update();
        return;
    }
    const auto apply = [region, layer, color, erase](EditDocument& model) {
        HistoryResult result;
        model.beginEdit();
        const int changes = layer == EditDocument::Layer::Lines
            ? model.eraseLineSegments(region, &result.error)
            : model.applyRasterFillBrush(region, color, erase, &result.error);
        if (changes < 0) model.cancelEdit();
        else {
            model.commitEdit();
            result.changed = changes > 0;
        }
        return result;
    };
    const QRectF bounds = region.boundingRect().intersected(document_.viewBox());
    const bool asynchronous = currentSvg.size() > AsyncHistoryThreshold || bounds.width() * bounds.height() > 1000000.;
    if (!asynchronous) {
        HistoryResult result;
        try { result = apply(document_); }
        catch (...) {
            try { document_.cancelEdit(); } catch (...) {}
            result.error = QStringLiteral("ブラシ処理中にエラーが発生しました。直前の編集結果を保持しています。");
        }
        updateCursor();
        if (result.changed) notifyChanged();
        else { if (view_) view_->viewport()->update(); emit stateChanged(); }
        if (!result.error.isEmpty()) emit message(result.error);
        return;
    }

    historyLinesVisible_ = document_.layerVisible(EditDocument::Layer::Lines);
    historyFillVisible_ = document_.layerVisible(EditDocument::Layer::Fill);
    auto model = std::make_shared<EditDocument>(std::move(document_));
    document_ = EditDocument();
    historySvg_ = currentSvg;
    historyBusy_ = true;
    const auto revision = ++historyRevision_;
    updateCursor();
    if (view_) view_->viewport()->update();
    emit stateChanged();
    emit message(layer == EditDocument::Layer::Lines ? QStringLiteral("主線の消しゴム処理を確定しています…")
                                                    : QStringLiteral("色面の境界をベクターへ戻しています…"));
    auto* watcher = new QFutureWatcher<HistoryResult>(this);
    connect(watcher, &QFutureWatcher<HistoryResult>::finished, this, [this, watcher, model, revision] {
        const auto result = watcher->result(); watcher->deleteLater();
        if (revision != historyRevision_ || !historyBusy_) return;
        document_ = std::move(*model); *model = EditDocument();
        historyBusy_ = false; historySvg_.clear();
        updateCursor();
        if (view_) view_->viewport()->update();
        if (result.changed) notifyChanged(); else emit stateChanged();
        if (!result.error.isEmpty()) emit message(result.error);
    });
    watcher->setFuture(QtConcurrent::run([model, apply] {
        try { return apply(*model); }
        catch (...) {
            try { model->cancelEdit(); } catch (...) {}
            return HistoryResult{false, QStringLiteral("ブラシ処理中にエラーが発生しました。直前の編集結果を保持しています。")};
        }
    }));
}

void Editor::pickColor(const QPointF& point)
{
    if (!active_ || historyBusy_) return;
    const auto bytes = document_.svg();
    const auto accept = [this](const PickedColor& result) {
        if (!result.error.isEmpty()) emit message(result.error);
        if (!result.color.isValid()) return;
        color_ = result.color;
        emit colorPicked(color_);
        emit stateChanged();
        if (view_) view_->viewport()->update();
    };
    if (bytes.size() <= AsyncHistoryThreshold) { accept(sampleSvgColor(bytes, point)); return; }
    const auto memoryError = svgMemoryError(quint64(bytes.size()));
    if (!memoryError.isEmpty()) { emit message(memoryError); return; }
    historySvg_ = bytes;
    historyLinesVisible_ = document_.layerVisible(EditDocument::Layer::Lines);
    historyFillVisible_ = document_.layerVisible(EditDocument::Layer::Fill);
    historyBusy_ = true;
    const auto revision = ++historyRevision_;
    updateCursor(); emit stateChanged();
    emit message(QStringLiteral("表示中のSVGから色を取得しています…"));
    auto* watcher = new QFutureWatcher<PickedColor>(this);
    connect(watcher, &QFutureWatcher<PickedColor>::finished, this, [this, watcher, revision, accept] {
        const auto result = watcher->result(); watcher->deleteLater();
        if (revision != historyRevision_ || !historyBusy_) return;
        historyBusy_ = false; historySvg_.clear();
        updateCursor(); emit stateChanged(); accept(result);
    });
    watcher->setFuture(QtConcurrent::run([bytes, point] {
        try { return sampleSvgColor(bytes, point); }
        catch (...) { return PickedColor{{}, QStringLiteral("SVGから色を取得できませんでした。")}; }
    }));
}

QTransform Editor::mapping() const
{
    return view_ ? view_->vectorToViewportTransform(document_.viewBox()) : QTransform();
}

QPointF Editor::toDocument(const QPointF& point) const
{
    return mapping().inverted().map(point);
}

double Editor::sourceTolerance(double pixels) const
{
    const QTransform inverse = mapping().inverted();
    const QPointF origin = inverse.map(QPointF());
    return qMax(QLineF(origin, inverse.map(QPointF(pixels, 0))).length(),
                QLineF(origin, inverse.map(QPointF(0, pixels))).length());
}

bool Editor::inScope(QObject* object) const
{
    const auto* widget = qobject_cast<QWidget*>(object);
    if (!widget || !view_) return false;
    return widget->window() == view_->window()
        || (panel_ && widget->window() == panel_->window());
}

bool Editor::textInput(QObject* object) const
{
    auto* widget = qobject_cast<QWidget*>(object);
    for (; widget; widget = widget->parentWidget()) {
        if (qobject_cast<QLineEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget)
            || qobject_cast<QTextEdit*>(widget) || qobject_cast<QPlainTextEdit*>(widget)
            || qobject_cast<QComboBox*>(widget) || qobject_cast<QAbstractSlider*>(widget)) return true;
        if (widget->isWindow()) break;
    }
    return false;
}

QVector<Editor::DisplayHandle> Editor::displayHandles() const
{
    QVector<DisplayHandle> result;
    if (selected_ < 0 || layer_ != EditDocument::Layer::Lines || !document_.layerVisible(layer_)) return result;
    const auto handles = document_.handles(selected_);
    const auto transform = mapping();
    const auto key = [](int subpath, int index) {
        return (quint64(quint32(subpath)) << 32) | quint32(index);
    };
    QHash<quint64, QPointF> anchors;
    for (const auto& handle : handles)
        if (handle.kind == EditDocument::Handle::Anchor)
            anchors.insert(key(handle.subpath, handle.index), transform.map(handle.point));

    struct CoincidentPair { int first = -1; int second = -1; };
    QHash<quint64, CoincidentPair> coincident;
    result.reserve(handles.size());
    for (const auto& handle : handles) {
        DisplayHandle display{handle, transform.map(handle.point), {}, false};
        if (handle.kind != EditDocument::Handle::Anchor) {
            const int startIndex = handle.index;
            // A closed subpath omits its duplicate final anchor in handles().
            const int endIndex = anchors.contains(key(handle.subpath, handle.index + 1)) ? handle.index + 1 : 0;
            const int anchorIndex = handle.kind == EditDocument::Handle::Control1 ? startIndex : endIndex;
            const int neighborIndex = handle.kind == EditDocument::Handle::Control1 ? endIndex : startIndex;
            const auto anchorKey = key(handle.subpath, anchorIndex);
            display.anchor = anchors.value(anchorKey, display.point);
            if (QLineF(display.anchor, display.point).length() < .5) {
                QPointF direction = anchors.value(key(handle.subpath, neighborIndex), display.anchor) - display.anchor;
                double length = std::hypot(direction.x(), direction.y());
                if (length < .00001) {
                    direction = handle.kind == EditDocument::Handle::Control1 ? QPointF(1, -1) : QPointF(-1, -1);
                    length = std::sqrt(2.);
                }
                display.point = display.anchor + direction * (14. / length);
                display.offset = true;
                auto& pair = coincident[anchorKey];
                if (pair.first < 0) pair.first = result.size();
                else pair.second = result.size();
            }
        }
        result.append(display);
    }
    // At a cusp both neighboring segments can point in the same direction.
    // Separate their two proxy controls without scanning other handles.
    for (auto it = coincident.cbegin(); it != coincident.cend(); ++it) {
        const auto pair = it.value();
        if (pair.first < 0 || pair.second < 0) continue;
        auto& first = result[pair.first];
        auto& second = result[pair.second];
        if (QLineF(first.point, second.point).length() >= 10.) continue;
        for (auto* display : {&first, &second}) {
            const QPointF direction = display->point - display->anchor;
            const double sine = display->handle.kind == EditDocument::Handle::Control1 ? -.5 : .5;
            constexpr double cosine = .8660254037844386;
            display->point = display->anchor + QPointF(direction.x() * cosine - direction.y() * sine,
                                                      direction.x() * sine + direction.y() * cosine);
        }
    }
    // Keep a proxy under the pointer during its drag. The actual SVG handle
    // moves by the pointer delta, with no geometry jump when the drag starts.
    if (gesture_ == Gesture::Handle && !controlDisplayOffset_.isNull()) {
        for (auto& display : result) {
            if (display.handle.kind != handle_.kind || display.handle.index != handle_.index
                || display.handle.subpath != handle_.subpath) continue;
            display.point = transform.map(display.handle.point + controlDisplayOffset_);
            display.offset = true;
        }
    }
    return result;
}

bool Editor::handleAt(const QPointF& point, EditDocument::Handle* result, QPointF* displayOffset) const
{
    if (selected_ < 0 || layer_ != EditDocument::Layer::Lines || !document_.layerVisible(layer_)) return false;
    double distance = 8.;
    bool found = false;
    for (const auto& display : displayHandles()) {
        const double next = QLineF(point, display.point).length();
        if (next < distance || (next == distance && display.handle.kind == EditDocument::Handle::Anchor)) {
            distance = next;
            *result = display.handle;
            if (displayOffset) *displayOffset = display.offset ? toDocument(display.point) - display.handle.point : QPointF();
            found = true;
        }
    }
    return found;
}

bool Editor::mousePress(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) {
        if (tool_ == Tool::Pen && layer_ == EditDocument::Layer::Lines) finishPen();
        return true;
    }
    if (event->button() != Qt::LeftButton) return true;
    cancelGesture();
    view_->setFocus(Qt::MouseFocusReason);
    pointerInside_ = true;
    pointerViewport_ = lastViewport_ = position(event);
    pressDocument_ = lastDocument_ = toDocument(lastViewport_);
    if (spaceDown_) {
        gesture_ = Gesture::Pan;
        updateCursor();
        return true;
    }
    if (tool_ == Tool::Eyedropper) {
        pickColor(lastDocument_);
        return true;
    }
    if (!document_.layerVisible(layer_)) {
        selected_ = -1;
        emit message(QStringLiteral("編集するレイヤーを表示してください。"));
        emit stateChanged();
        return true;
    }
    if (tool_ == Tool::Pen && layer_ == EditDocument::Layer::Lines) {
        selected_ = -1;
        penAnchors_.append({lastDocument_, lastDocument_, lastDocument_});
        gesture_ = Gesture::PenAnchor;
    } else if (tool_ == Tool::Eraser || (tool_ == Tool::Pen && layer_ == EditDocument::Layer::Fill)) {
        selected_ = -1;
        gesture_ = Gesture::Brush;
        eraserTrace_ = QPainterPath(lastDocument_);
        strokeLayer_ = layer_; strokeErase_ = tool_ == Tool::Eraser;
        strokeRadius_ = brushRadius_; strokeColor_ = color_;
        brushPreview_ = {}; brushPreviewBounds_ = {}; previewErrorShown_ = false;
        if (strokeLayer_ == EditDocument::Layer::Fill) {
            brushDisplayIcc_ = view_->getLoadedPixmap().toImage().colorSpace().iccProfile();
            brushPreviewTimer_.start(0);
        }
    } else if (tool_ == Tool::DeletePaths) {
        selected_ = -1;
        document_.beginEdit();
        gesture_ = Gesture::DeletePaths;
        eraserTrace_ = QPainterPath(lastDocument_);
        dirtyGesture_ = document_.erasePaths(lastDocument_, lastDocument_, brushRadius_, layer_) > 0;
    } else if (addLine_ && layer_ == EditDocument::Layer::Lines) {
        document_.beginEdit();
        gesture_ = Gesture::AddLine;
        selected_ = -1;
    } else if (layer_ == EditDocument::Layer::Lines) {
        if (!handleAt(lastViewport_, &handle_, &controlDisplayOffset_)) {
            selected_ = document_.hitTest(lastDocument_, sourceTolerance(7.), layer_);
            if (selected_ >= 0 && !handleAt(lastViewport_, &handle_, &controlDisplayOffset_)) {
                view_->viewport()->update();
                emit stateChanged();
                return true;
            }
        }
        if (selected_ >= 0) {
            document_.beginEdit();
            gesture_ = Gesture::Handle;
            handleOffset_ = handle_.point - lastDocument_;
        }
    } else {
        selected_ = document_.hitTest(lastDocument_, sourceTolerance(5.), layer_);
        if (selected_ >= 0) {
            document_.beginEdit();
            gesture_ = Gesture::Fill;
        }
    }
    updateCursor();
    view_->viewport()->update();
    emit stateChanged();
    return true;
}

bool Editor::mouseMove(QMouseEvent* event)
{
    pointerInside_ = true;
    const QPointF current = position(event);
    pointerViewport_ = current;
    if (gesture_ != Gesture::None && !event->buttons().testFlag(Qt::LeftButton)) {
        // A mouse release outside this application can be lost after focus
        // changes. Never keep mutating the document on subsequent hover moves.
        cancelGesture();
    }
    if (gesture_ == Gesture::Pan) {
        const QPoint delta = (current - lastViewport_).toPoint();
        auto* horizontal = view_->horizontalScrollBar();
        auto* vertical = view_->verticalScrollBar();
        horizontal->setValue(horizontal->value() - delta.x() * (view_->isRightToLeft() ? -1 : 1));
        vertical->setValue(vertical->value() - delta.y());
    } else if (gesture_ == Gesture::Handle) {
        const QPointF target = toDocument(current);
        if (QLineF(target, lastDocument_).length() > 0.00001)
            dirtyGesture_ |= document_.moveHandle(selected_, handle_, target + handleOffset_);
        lastDocument_ = target;
    } else if (gesture_ == Gesture::Fill) {
        const QPointF target = toDocument(current);
        const QPointF delta = target - lastDocument_;
        if (QLineF(QPointF(), delta).length() > 0.00001)
            dirtyGesture_ |= document_.deformFill(selected_, lastDocument_, delta, brushRadius_, false);
        lastDocument_ = target;
    } else if (gesture_ == Gesture::AddLine) {
        lastDocument_ = toDocument(current);
    } else if (gesture_ == Gesture::PenAnchor) {
        updatePenTangent(current);
    } else if (gesture_ == Gesture::DeletePaths) {
        const auto target = toDocument(current);
        dirtyGesture_ |= document_.erasePaths(lastDocument_, target, brushRadius_, layer_) > 0;
        eraserTrace_.lineTo(target);
        lastDocument_ = target;
    } else if (gesture_ == Gesture::Brush) {
        const auto target = toDocument(current);
        // Reduce duplicate hover samples while retaining the swept segment.
        if (QLineF(target, lastDocument_).length() > .00001) eraserTrace_.lineTo(target);
        lastDocument_ = target;
        if (strokeLayer_ == EditDocument::Layer::Fill && !brushPreviewTimer_.isActive()) brushPreviewTimer_.start(75);
    }
    lastViewport_ = current;
    view_->viewport()->update();
    return true;
}

bool Editor::mouseRelease(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return true;
    // Apply the release position too: window systems may coalesce the final
    // pointer move, particularly with a tablet or a busy large SVG preview.
    const QPointF target = toDocument(position(event));
    if (gesture_ == Gesture::Handle && QLineF(target, lastDocument_).length() > 0.00001) {
        dirtyGesture_ |= document_.moveHandle(selected_, handle_, target + handleOffset_);
    } else if (gesture_ == Gesture::Fill && QLineF(target, lastDocument_).length() > 0.00001) {
        dirtyGesture_ |= document_.deformFill(selected_, lastDocument_, target - lastDocument_, brushRadius_, false);
        lastDocument_ = target;
    } else if (gesture_ == Gesture::PenAnchor) {
        updatePenTangent(position(event));
    } else if (gesture_ == Gesture::DeletePaths) {
        dirtyGesture_ |= document_.erasePaths(lastDocument_, target, brushRadius_, layer_) > 0;
        lastDocument_ = target;
    } else if (gesture_ == Gesture::Brush) {
        if (QLineF(target, lastDocument_).length() > .00001) eraserTrace_.lineTo(target);
        lastDocument_ = target;
        finishBrushStroke();
        return true;
    } else if (gesture_ == Gesture::AddLine) {
        lastDocument_ = toDocument(position(event));
        if (QLineF(mapping().map(pressDocument_), position(event)).length() >= 2.) {
            selected_ = document_.addLine(pressDocument_, lastDocument_, color_, lineWidth_);
            dirtyGesture_ = selected_ >= 0;
            if (selected_ < 0) emit message(QStringLiteral("主線を追加できませんでした。線の数や座標の範囲を確認してください。"));
        }
    }
    finishGesture();
    return true;
}

void Editor::cancelGesture()
{
    brushPreviewTimer_.stop();
    if (gesture_ == Gesture::PenAnchor && !penAnchors_.isEmpty()) {
        penAnchors_.removeLast();
        emit stateChanged();
    } else if (gesture_ != Gesture::None && gesture_ != Gesture::Pan && gesture_ != Gesture::Brush) document_.cancelEdit();
    gesture_ = Gesture::None;
    controlDisplayOffset_ = {};
    eraserTrace_ = {};
    brushPreview_ = {}; brushPreviewBounds_ = {};
    dirtyGesture_ = false;
    updateCursor();
    if (view_) view_->viewport()->update();
}

void Editor::finishGesture()
{
    const bool changedDocument = dirtyGesture_;
    if (gesture_ == Gesture::Fill && changedDocument)
        document_.deformFill(selected_, lastDocument_, {}, brushRadius_, true);
    if (gesture_ != Gesture::None && gesture_ != Gesture::Pan && gesture_ != Gesture::PenAnchor && gesture_ != Gesture::Brush) {
        if (changedDocument) document_.commitEdit();
        else document_.cancelEdit();
    }
    gesture_ = Gesture::None;
    controlDisplayOffset_ = {};
    eraserTrace_ = {};
    dirtyGesture_ = false;
    updateCursor();
    if (changedDocument) notifyChanged();
    else if (view_) view_->viewport()->update();
}

void Editor::notifyChanged()
{
    updateCursor();
    if (view_) view_->viewport()->update();
    emit changed(document_.svg());
    emit stateChanged();
}

void Editor::updateCursor()
{
    if (!view_ || !active_) return;
    view_->viewport()->setCursor(historyBusy_ ? Qt::BusyCursor
        : gesture_ == Gesture::Pan ? Qt::ClosedHandCursor
        : spaceDown_ ? Qt::OpenHandCursor
        : tool_ == Tool::Eyedropper ? Qt::CrossCursor
        : !document_.layerVisible(layer_) ? Qt::ForbiddenCursor
        : addLine_ || tool_ != Tool::Select || layer_ == EditDocument::Layer::Fill ? Qt::CrossCursor : Qt::ArrowCursor);
}

bool Editor::eventFilter(QObject* object, QEvent* event)
{
    if (!active_ || !view_) return false;
    // MainWindow owns the context-menu popup, so intercept before the event
    // propagates from the viewport, including keyboard-triggered menus.
    if (event->type() == QEvent::ContextMenu && inScope(object)) { event->accept(); return true; }
    if (object == view_->viewport()) {
        if (historyBusy_ && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick)) return true;
        switch (event->type()) {
        case QEvent::MouseButtonPress: return mousePress(static_cast<QMouseEvent*>(event));
        case QEvent::MouseMove: return mouseMove(static_cast<QMouseEvent*>(event));
        case QEvent::MouseButtonRelease: return mouseRelease(static_cast<QMouseEvent*>(event));
        case QEvent::MouseButtonDblClick:
            if (tool_ == Tool::Pen && layer_ == EditDocument::Layer::Lines && !spaceDown_
                && document_.layerVisible(EditDocument::Layer::Lines)) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    const auto point = position(mouse);
                    // The first click of a native double click already added
                    // this anchor. Directly delivered double clicks still work.
                    if (penAnchors_.isEmpty() || QLineF(mapping().map(penAnchors_.last().point), point).length() > 2.) {
                        const auto source = toDocument(point);
                        penAnchors_.append({source, source, source});
                    }
                    finishPen();
                }
            }
            return true;
        case QEvent::Wheel: if (gesture_ != Gesture::None) return true; break;
        case QEvent::Enter: pointerInside_ = true; updateCursor(); break;
        case QEvent::Leave: pointerInside_ = false; view_->viewport()->update(); break;
        default: break;
        }
    }
    if ((event->type() == QEvent::FocusOut && (object == view_ || object == view_->viewport()))
        || (event->type() == QEvent::WindowDeactivate && inScope(object))) {
        spaceDown_ = false;
        cancelGesture();
    }
    if (!inScope(object)) return false;
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress
        && event->type() != QEvent::KeyRelease) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Menu || (key->key() == Qt::Key_F10 && key->modifiers().testFlag(Qt::ShiftModifier))) {
        key->accept(); return true;
    }
    if (qobject_cast<QAbstractSlider*>(object)) {
        const int code = key->key();
        const bool navigation = code == Qt::Key_Left || code == Qt::Key_Right
            || code == Qt::Key_Up || code == Qt::Key_Down || code == Qt::Key_Home
            || code == Qt::Key_End || code == Qt::Key_PageUp || code == Qt::Key_PageDown;
        if (navigation) {
            // Reserve these keys for the focused slider, ahead of the viewer's
            // window shortcuts, then let QAbstractSlider handle its KeyPress.
            if (event->type() == QEvent::ShortcutOverride) { key->accept(); return true; }
            return false;
        }
    }
    // Space belongs to canvas panning while the canvas has focus. On a slider
    // it must not start panning or fall through to the slideshow shortcut.
    if (key->key() == Qt::Key_Space && qobject_cast<QAbstractSlider*>(QApplication::focusWidget())) {
        key->accept(); return true;
    }
    if (textInput(object) || !editingKey(key)) return false;
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !hasPendingPen()) return false;
    if (historyBusy_) { key->accept(); return true; }
    if (event->type() == QEvent::ShortcutOverride) {
        key->accept();
        return true;
    }
    if (key->key() == Qt::Key_Space) {
        if (!key->isAutoRepeat()) {
            spaceDown_ = event->type() == QEvent::KeyPress;
            if (!spaceDown_ && gesture_ == Gesture::Pan) finishGesture();
            updateCursor();
        }
    } else if (event->type() == QEvent::KeyPress) {
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
            if (hasPendingPen()) removeLastPenAnchor();
            else deleteSelection();
        }
        else if (key->key() == Qt::Key_Escape) {
            cancelGesture();
            cancelPen();
            selected_ = -1;
            view_->viewport()->update();
            emit stateChanged();
        } else if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) finishPen();
        else if (key->key() == Qt::Key_Z) {
            if (key->modifiers() & Qt::ShiftModifier) redo();
            else undo();
        } else if (key->key() == Qt::Key_Y) redo();
    }
    key->accept();
    return true;
}

void Editor::paint(QPainter* painter)
{
    if (!active_ || historyBusy_ || !view_) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QTransform transform = mapping();
    const QColor highlight(0, 140, 255);
    if (gesture_ == Gesture::Brush && strokeLayer_ == EditDocument::Layer::Fill && !brushPreview_.isNull()) {
        painter->save();
        painter->setTransform(transform);
        // A transparent SVG patch replaces old fill pixels. First repaint the
        // canvas underneath; CompositionMode_Source would punch an alpha hole
        // through an opaque QWidget backing store and can appear black.
        painter->fillRect(brushPreviewBounds_, view_->viewport()->palette().brush(QPalette::Window));
        painter->drawImage(brushPreviewBounds_, brushPreview_);
        painter->restore();
    }
    if (selected_ >= 0 && document_.layerVisible(layer_)) {
        painter->setPen(QPen(highlight, 1.5));
        painter->setBrush(layer_ == EditDocument::Layer::Fill ? QColor(0, 140, 255, 45) : Qt::transparent);
        painter->drawPath(transform.map(document_.path(selected_)));
        if (layer_ == EditDocument::Layer::Lines) {
            const auto handles = displayHandles();
            for (const auto& control : handles) {
                if (control.handle.kind == EditDocument::Handle::Anchor) continue;
                painter->setPen(QPen(QColor(50, 110, 160, 190), 1., control.offset ? Qt::DashLine : Qt::SolidLine));
                painter->drawLine(control.anchor, control.point);
            }
            // Draw controls before anchors so an overlapping anchor stays visible.
            for (int anchorPass = 0; anchorPass < 2; ++anchorPass) {
                for (const auto& handle : handles) {
                    const bool anchor = handle.handle.kind == EditDocument::Handle::Anchor;
                    if (anchor != bool(anchorPass)) continue;
                    const QPointF point = handle.point;
                    if (!QRectF(view_->viewport()->rect()).adjusted(-8, -8, 8, 8).contains(point)) continue;
                    painter->setPen(QPen(highlight, 1.3));
                    painter->setBrush(anchor ? highlight : QColor(Qt::white));
                    if (anchor) painter->drawRect(QRectF(point - QPointF(3.5, 3.5), QSizeF(7, 7)));
                    else painter->drawEllipse(point, 3.5, 3.5);
                }
            }
        }
    }
    if (gesture_ == Gesture::AddLine) {
        painter->setPen(QPen(highlight, 2., Qt::DashLine));
        painter->setBrush(Qt::NoBrush);
        painter->drawLine(transform.map(pressDocument_), transform.map(lastDocument_));
    }
    if (!penAnchors_.isEmpty()) {
        QPainterPath pending(penAnchors_.first().point);
        for (int i = 1; i < penAnchors_.size(); ++i)
            pending.cubicTo(penAnchors_[i-1].outgoing, penAnchors_[i].incoming, penAnchors_[i].point);
        if (gesture_ == Gesture::None && pointerInside_ && !spaceDown_) {
            const auto next = toDocument(pointerViewport_);
            pending.cubicTo(penAnchors_.last().outgoing, next, next);
        }
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(color_, qMax(.5, lineWidth_ / sourceTolerance(1.)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(transform.map(pending));
        painter->setPen(QPen(highlight, 1., Qt::DashLine));
        painter->drawPath(transform.map(pending));
        for (const auto& anchor : penAnchors_) {
            const auto point = transform.map(anchor.point);
            for (const auto& control : {anchor.incoming, anchor.outgoing}) {
                if (QLineF(control, anchor.point).length() < .00001) continue;
                painter->setPen(QPen(highlight, 1.));
                painter->drawLine(point, transform.map(control));
                painter->setBrush(Qt::white);
                painter->drawEllipse(transform.map(control), 3.5, 3.5);
            }
            painter->setBrush(highlight);
            painter->setPen(QPen(Qt::white, 1.));
            painter->drawRect(QRectF(point - QPointF(3.5, 3.5), QSizeF(7, 7)));
        }
    }
    if (gesture_ == Gesture::DeletePaths || (gesture_ == Gesture::Brush && strokeLayer_ == EditDocument::Layer::Lines)) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(230, 45, 35, 70), 2. * brushRadius_ / sourceTolerance(1.),
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(transform.map(eraserTrace_));
    }
    if (tool_ != Tool::Eyedropper
        && (layer_ == EditDocument::Layer::Fill || tool_ == Tool::Eraser || tool_ == Tool::DeletePaths) && pointerInside_
        && !spaceDown_ && document_.layerVisible(layer_)) {
        const QPointF source = toDocument(pointerViewport_);
        QPainterPath brush;
        brush.addEllipse(source, brushRadius_, brushRadius_);
        painter->setPen(QPen(QColor(255, 255, 255, 210), 3.));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(transform.map(brush));
        painter->setPen(QPen(tool_ == Tool::Eraser || tool_ == Tool::DeletePaths ? QColor(230, 45, 35) : highlight, 1., Qt::DashLine));
        painter->drawPath(transform.map(brush));
    }
    painter->restore();
}
}
