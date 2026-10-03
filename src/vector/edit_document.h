// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QColor>
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

// Edits the generated SVG without flattening its layers or replacing other paths.
// All public points and lengths are in the SVG's source-image coordinates.
class EditDocument {
public:
    using Layer = Vector::Layer;
    using Handle = Vector::Handle;
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
    int hitTest(QPointF point, double tolerance, Layer layer) const;
    QPainterPath path(int index) const;
    QVector<Handle> handles(int index) const;
    QColor color(int index) const;
    bool moveHandle(int index, Handle handle, QPointF point);
    int addLine(QPointF from, QPointF to, QColor color, double width);
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
