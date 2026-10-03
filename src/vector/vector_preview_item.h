// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QRect>
#include <QTransform>
#include <memory>

class QVGraphicsView;

namespace Vector {
class GeneratedSvgRenderer;

// The pixmap continues to define image geometry for the existing viewer. SVG
// painting replaces its pixels only while a vector preview is active.
class PreviewItem : public QGraphicsPixmapItem
{
public:
    explicit PreviewItem(QVGraphicsView* view);
    ~PreviewItem() override;

    // Empty SVG clears the preview. Invalid SVG clears it and returns false.
    bool setVectorPreview(const QByteArray& svg, const QByteArray& displayIcc,
                          std::shared_ptr<GeneratedSvgRenderer> renderer = {});
    bool hasVectorPreview() const;
    QSizeF vectorSourceSize() const;

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override;

private:
    void clearCache();

    QVGraphicsView* view_;
    std::shared_ptr<GeneratedSvgRenderer> renderer_;
    QByteArray svg_;
    QByteArray displayIcc_;
    QImage cachedImage_;
    QTransform cachedDeviceTransform_;
    QRect cachedCrop_;
    QRectF cachedBounds_;
    int cachedRotation_ = 0;
    bool renderFailed_ = false;
};

}
