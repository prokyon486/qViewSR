// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "edit_document.h"
#include <QObject>
#include <QPointer>
#include <QColor>
#include <QGraphicsView>
#include <QTimer>

class QVGraphicsView;
class QPainter;
class QMouseEvent;

namespace Vector {

// Interaction is kept separate from the generated SVG renderer. The document
// stays in source SVG coordinates; only overlays and hit tests use screen pixels.
class Editor : public QObject {
    Q_OBJECT
public:
    enum class Tool { Select, Pen, Eraser, DeletePaths, Eyedropper };
    Q_ENUM(Tool)
    explicit Editor(QVGraphicsView* view);
    ~Editor() override;
    bool begin(const QByteArray& svg, QString* error = nullptr);
    // A worker can parse a large SVG before handing its completed model to the
    // GUI thread. The supplied document is consumed only when it is different.
    bool beginPrepared(const QByteArray& svg, std::shared_ptr<EditDocument> prepared,
                       QString* error = nullptr);
    void end();
    // A new source/result is a different document even if its SVG bytes match.
    void reset();
    bool active() const { return active_; }
    bool isBusy() const { return historyBusy_; }
    QByteArray svg() const;
    EditDocument& document() { return document_; }
    const EditDocument& document() const { return document_; }
    int selectedPath() const { return selected_; }
    void setPanelWidget(QWidget* panel) { panel_ = panel; }
    void setLayer(EditDocument::Layer layer);
    EditDocument::Layer layer() const { return layer_; }
    void setTool(Tool tool);
    Tool tool() const { return tool_; }
    void setLayerVisible(EditDocument::Layer layer, bool visible);
    bool layerVisible(EditDocument::Layer layer) const;
    bool hasPendingPen() const { return !penAnchors_.isEmpty(); }
    void finishPen();
    void setAddLine(bool enabled);
    void setColor(const QColor& color);
    QColor currentColor() const { return color_; }
    void setBrushRadius(double radius);
    void setLineWidth(double width);
    void deleteSelection();
    void undo();
    void redo();

signals:
    void changed(const QByteArray& svg);
    void stateChanged();
    void message(const QString& text);
    void colorPicked(const QColor& color);

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    enum class Gesture { None, Pan, Handle, AddLine, Fill, PenAnchor, DeletePaths, Brush };
    struct DisplayHandle {
        EditDocument::Handle handle;
        QPointF point;
        QPointF anchor;
        bool offset = false;
    };
    QTransform mapping() const;
    QPointF toDocument(const QPointF& viewportPoint) const;
    double sourceTolerance(double pixels) const;
    bool inScope(QObject* object) const;
    bool textInput(QObject* object) const;
    QVector<DisplayHandle> displayHandles() const;
    bool handleAt(const QPointF& viewportPoint, EditDocument::Handle* result,
                  QPointF* displayOffset = nullptr) const;
    bool mousePress(QMouseEvent* event);
    bool mouseMove(QMouseEvent* event);
    bool mouseRelease(QMouseEvent* event);
    void cancelGesture();
    void finishGesture();
    void notifyChanged();
    void changeHistory(bool redo);
    bool validSelectedFill() const;
    void restoreStrokeSelection(int selectedFill);
    QPainterPath brushRegion() const;
    void updateBrushPreview();
    void finishBrushStroke();
    void pickColor(const QPointF& point);
    void cancelPen(bool explain = false);
    void updatePenTangent(const QPointF& viewportPoint);
    void removeLastPenAnchor();
    void updateCursor();
    void paint(QPainter* painter);

    QPointer<QVGraphicsView> view_;
    QPointer<QWidget> panel_;
    EditDocument document_;
    EditDocument::Layer layer_ = EditDocument::Layer::Lines;
    Tool tool_ = Tool::Select;
    QVector<EditDocument::BezierAnchor> penAnchors_;
    QPainterPath eraserTrace_;
    QTimer brushPreviewTimer_;
    QImage brushPreview_;
    QRectF brushPreviewBounds_;
    QByteArray brushDisplayIcc_;
    QColor strokeColor_;
    EditDocument::Layer strokeLayer_ = EditDocument::Layer::Lines;
    int strokeSelectedFill_ = -1;
    double strokeRadius_ = 24.;
    bool strokeErase_ = false, previewErrorShown_ = false;
    EditDocument::Handle handle_;
    bool active_ = false;
    bool historyBusy_ = false;
    quint64 historyRevision_ = 0;
    QByteArray historySvg_;
    QByteArray publishedSvg_;
    bool historyLinesVisible_ = true, historyFillVisible_ = true;
    bool addLine_ = false;
    bool spaceDown_ = false;
    bool dirtyGesture_ = false;
    bool pointerInside_ = false;
    bool savedMouseTracking_ = false;
    QGraphicsView::DragMode savedDragMode_ = QGraphicsView::NoDrag;
    Gesture gesture_ = Gesture::None;
    int selected_ = -1;
    QColor color_ = Qt::black;
    double brushRadius_ = 24.;
    double lineWidth_ = 2.;
    QPointF lastViewport_;
    QPointF lastDocument_;
    QPointF pressDocument_;
    QPointF handleOffset_;
    QPointF controlDisplayOffset_;
    QPointF pointerViewport_;
};
}
