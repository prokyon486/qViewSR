// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QSize>
#include <QVector>
#include <memory>

namespace Vector {
enum class Layer { Lines, Fill };
struct Handle {
    int subpath = 0;
    int index = 0;
    enum Kind { Anchor, Control1, Control2 };
    Kind kind = Anchor;
    QPointF point;
};
struct BezierAnchor {
    QPointF point;
    QPointF incoming;
    QPointF outgoing;
};

// Edits the generated SVG without flattening its layers or replacing other paths.
// All public points and lengths are in the SVG's source-image coordinates.
class EditDocument {
public:
    using Layer = Vector::Layer;
    using Handle = Vector::Handle;
    using BezierAnchor = Vector::BezierAnchor;
    EditDocument();
    ~EditDocument();
    EditDocument(EditDocument&&) noexcept;
    EditDocument& operator=(EditDocument&&) noexcept;
    // Includes the second parsed document temporarily required by Undo/Redo.
    // Call with currently available memory before starting a document load.
    static QString memoryError(quint64 serializedBytes, quint64 availableBytes);
    bool load(const QByteArray& svg, QString* error = nullptr);
    QByteArray svg() const;
    QSize size() const;
    QRectF viewBox() const;
    int pathCount() const;
    Layer layer(int index) const;
    bool layerVisible(Layer layer) const;
    bool setLayerVisible(Layer layer, bool visible);
    int hitTest(QPointF point, double tolerance, Layer layer) const;
    QPainterPath path(int index) const;
    QVector<Handle> handles(int index) const;
    QColor color(int index) const;
    bool moveHandle(int index, Handle handle, QPointF point);
    int addLine(QPointF from, QPointF to, QColor color, double width);
    int addBezierPath(const QVector<BezierAnchor>& anchors, QColor color, double width, bool closed = false);
    // Deletes complete paths intersected by the swept circular brush.
    int erasePaths(QPointF from, QPointF to, double radius, Layer layer);
    int eraseLineSegments(const QPainterPath& sweptRegion, QString* error = nullptr);
    int applyRasterFillBrush(const QPainterPath& sweptRegion, QColor color, bool erase, QString* error = nullptr);
    QImage rasterBrushPreview(const QPainterPath& sweptRegion, QColor color, bool erase,
                             QRectF* bounds, QString* error = nullptr, double outputScale = 1.) const;
    bool remove(int index);
    bool setColor(int index, QColor color);
    bool deformFill(int index, QPointF center, QPointF delta, double radius, bool smooth);

    void beginEdit();
    void commitEdit();
    void cancelEdit();
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};
}
