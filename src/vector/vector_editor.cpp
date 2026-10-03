// SPDX-License-Identifier: GPL-3.0-or-later
#include "vector_editor.h"
#include "memory_budget.h"
#include "qvgraphicsview.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QHash>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
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
        || key == Qt::Key_Delete || key == Qt::Key_Backspace || key == Qt::Key_Escape)
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
    layer_ = layer;
    selected_ = -1;
    if (layer != EditDocument::Layer::Lines) addLine_ = false;
    updateCursor();
    if (view_) view_->viewport()->update();
    emit stateChanged();
}

void Editor::setAddLine(bool enabled)
{
    if (historyBusy_) return;
    cancelGesture();
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
    if (!active_ || selected_ < 0) return;
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
            || qobject_cast<QComboBox*>(widget)) return true;
        if (widget->isWindow()) break;
    }
    return false;
}

bool Editor::handleAt(const QPointF& point, EditDocument::Handle* result) const
{
    if (selected_ < 0 || layer_ != EditDocument::Layer::Lines) return false;
    const auto transform = mapping();
    double distance = 8.;
    bool found = false;
    // Anchors win coincident handles, so the default straight cubic remains
    // easy to select even when one of its control handles is collapsed.
    const auto handles = document_.handles(selected_);
    for (const auto& handle : handles) {
        const double next = QLineF(point, transform.map(handle.point)).length();
        if (next < distance || (next == distance && handle.kind == EditDocument::Handle::Anchor)) {
            distance = next;
            *result = handle;
            found = true;
        }
    }
    return found;
}

bool Editor::mousePress(QMouseEvent* event)
{
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
    if (addLine_ && layer_ == EditDocument::Layer::Lines) {
        document_.beginEdit();
        gesture_ = Gesture::AddLine;
        selected_ = -1;
    } else if (layer_ == EditDocument::Layer::Lines) {
        if (!handleAt(lastViewport_, &handle_)) {
            selected_ = document_.hitTest(lastDocument_, sourceTolerance(7.), layer_);
            if (selected_ >= 0 && !handleAt(lastViewport_, &handle_)) {
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
    if (gesture_ != Gesture::None && gesture_ != Gesture::Pan) document_.cancelEdit();
    gesture_ = Gesture::None;
    dirtyGesture_ = false;
    updateCursor();
    if (view_) view_->viewport()->update();
}

void Editor::finishGesture()
{
    const bool changedDocument = dirtyGesture_;
    if (gesture_ == Gesture::Fill && changedDocument)
        document_.deformFill(selected_, lastDocument_, {}, brushRadius_, true);
    if (gesture_ != Gesture::None && gesture_ != Gesture::Pan) {
        if (changedDocument) document_.commitEdit();
        else document_.cancelEdit();
    }
    gesture_ = Gesture::None;
    dirtyGesture_ = false;
    updateCursor();
    if (changedDocument) notifyChanged();
    else if (view_) view_->viewport()->update();
}

void Editor::notifyChanged()
{
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
        : addLine_ || layer_ == EditDocument::Layer::Fill ? Qt::CrossCursor : Qt::ArrowCursor);
}

bool Editor::eventFilter(QObject* object, QEvent* event)
{
    if (!active_ || !view_) return false;
    if (object == view_->viewport()) {
        if (historyBusy_ && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick)) return true;
        switch (event->type()) {
        case QEvent::MouseButtonPress: return mousePress(static_cast<QMouseEvent*>(event));
        case QEvent::MouseMove: return mouseMove(static_cast<QMouseEvent*>(event));
        case QEvent::MouseButtonRelease: return mouseRelease(static_cast<QMouseEvent*>(event));
        case QEvent::MouseButtonDblClick: return true;
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
    if (textInput(object) || !editingKey(key)) return false;
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
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) deleteSelection();
        else if (key->key() == Qt::Key_Escape) {
            cancelGesture();
            selected_ = -1;
            view_->viewport()->update();
            emit stateChanged();
        } else if (key->key() == Qt::Key_Z) {
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
    if (selected_ >= 0) {
        painter->setPen(QPen(highlight, 1.5));
        painter->setBrush(layer_ == EditDocument::Layer::Fill ? QColor(0, 140, 255, 45) : Qt::transparent);
        painter->drawPath(transform.map(document_.path(selected_)));
        if (layer_ == EditDocument::Layer::Lines) {
            const auto handles = document_.handles(selected_);
            // Control1 belongs to the segment's starting anchor; Control2 to
            // its end anchor. Match within each subpath (compound paths may
            // reuse the same index).
            painter->setPen(QPen(QColor(50, 110, 160, 190), 1.));
            const auto anchorKey = [](int subpath, int index) {
                return (quint64(quint32(subpath)) << 32) | quint32(index);
            };
            QHash<quint64, QPointF> anchors;
            for (const auto& handle : handles)
                if (handle.kind == EditDocument::Handle::Anchor)
                    anchors.insert(anchorKey(handle.subpath, handle.index), handle.point);
            for (const auto& control : handles) {
                if (control.kind == EditDocument::Handle::Anchor) continue;
                const int anchorIndex = control.index
                    + (control.kind == EditDocument::Handle::Control2 ? 1 : 0);
                const auto anchor = anchors.constFind(anchorKey(control.subpath, anchorIndex));
                if (anchor != anchors.constEnd())
                    painter->drawLine(transform.map(anchor.value()), transform.map(control.point));
            }
            // Draw controls before anchors so an overlapping anchor stays visible.
            for (int anchorPass = 0; anchorPass < 2; ++anchorPass) {
                for (const auto& handle : handles) {
                    const bool anchor = handle.kind == EditDocument::Handle::Anchor;
                    if (anchor != bool(anchorPass)) continue;
                    const QPointF point = transform.map(handle.point);
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
    if (layer_ == EditDocument::Layer::Fill && pointerInside_ && !spaceDown_) {
        const QPointF source = toDocument(pointerViewport_);
        QPainterPath brush;
        brush.addEllipse(source, brushRadius_, brushRadius_);
        painter->setPen(QPen(QColor(255, 255, 255, 210), 3.));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(transform.map(brush));
        painter->setPen(QPen(highlight, 1., Qt::DashLine));
        painter->drawPath(transform.map(brush));
    }
    painter->restore();
}
}
