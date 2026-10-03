// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "edit_document.h"
#include <QObject>
#include <QPointer>
#include <QColor>
#include <QGraphicsView>

class QVGraphicsView;
class QPainter;
class QMouseEvent;

namespace Vector {

// Interaction is kept separate from the generated SVG renderer. The document
// stays in source SVG coordinates; only overlays and hit tests use screen pixels.
class Editor : public QObject {
    Q_OBJECT
public:
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
    void setAddLine(bool enabled);
    void setColor(const QColor& color);
    void setBrushRadius(double radius);
    void setLineWidth(double width);
    void deleteSelection();
    void undo();
    void redo();

signals:
    void changed(const QByteArray& svg);
    void stateChanged();
    void message(const QString& text);

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    enum class Gesture { None, Pan, Handle, AddLine, Fill };
    QTransform mapping() const;
    QPointF toDocument(const QPointF& viewportPoint) const;
    double sourceTolerance(double pixels) const;
    bool inScope(QObject* object) const;
    bool textInput(QObject* object) const;
    bool handleAt(const QPointF& viewportPoint, EditDocument::Handle* result) const;
    bool mousePress(QMouseEvent* event);
    bool mouseMove(QMouseEvent* event);
    bool mouseRelease(QMouseEvent* event);
    void cancelGesture();
    void finishGesture();
    void notifyChanged();
    void changeHistory(bool redo);
    void updateCursor();
    void paint(QPainter* painter);

    QPointer<QVGraphicsView> view_;
    QPointer<QWidget> panel_;
    EditDocument document_;
    EditDocument::Layer layer_ = EditDocument::Layer::Lines;
    EditDocument::Handle handle_;
    bool active_ = false;
    bool historyBusy_ = false;
    quint64 historyRevision_ = 0;
    QByteArray historySvg_;
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
    QPointF pointerViewport_;
};
}
