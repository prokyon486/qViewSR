// SPDX-License-Identifier: GPL-3.0-or-later
#include "vector_preview_item.h"
#include "generated_svg_renderer.h"

#include "qvgraphicsview.h"
#include "sr/color_pipeline.h"
#include <QColorSpace>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QWidget>
#include <cmath>
#include <limits>

namespace Vector {
namespace {
constexpr qint64 maxCropPixels = 64'000'000;
constexpr int maxCropSide = 32'768;

bool finiteRect(const QRectF& rect)
{
    return std::isfinite(rect.left()) && std::isfinite(rect.top())
        && std::isfinite(rect.right()) && std::isfinite(rect.bottom());
}

QRectF physicalDeviceRect(const QPaintDevice* device)
{
    // QImage/QPixmap metrics already count physical pixels, while a QWidget's
    // metrics are logical pixels and its backing store is scaled by the DPR.
    const qreal ratio = device->devType() == QInternal::Widget
        ? device->devicePixelRatioF() : 1.0;
    return QRectF(0, 0, device->width() * ratio, device->height() * ratio);
}
}

PreviewItem::PreviewItem(QVGraphicsView* view) : view_(view)
{
    setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
}

PreviewItem::~PreviewItem() = default;

void PreviewItem::clearCache()
{
    cachedImage_ = QImage();
    cachedCrop_ = QRect();
    renderFailed_ = false;
}

bool PreviewItem::setVectorPreview(const QByteArray& svg, const QByteArray& displayIcc)
{
    if (svg.isEmpty()) {
        renderer_.reset();
        svg_.clear();
        displayIcc_.clear();
        clearCache();
        update();
        return true;
    }
    if (renderer_ && svg == svg_ && displayIcc == displayIcc_)
        return true;

    if (!renderer_ || svg != svg_) {
        auto renderer = std::make_unique<GeneratedSvgRenderer>(svg);
        const QRectF box = renderer->viewBoxF();
        if (!renderer->isValid() || box.isEmpty() || !finiteRect(box)
            || box.width() > std::numeric_limits<int>::max() || box.height() > std::numeric_limits<int>::max()) {
            setVectorPreview({}, {});
            if (view_) QMetaObject::invokeMethod(view_, "vectorRenderingFailed", Qt::QueuedConnection,
                Q_ARG(QString, renderer->errorString().isEmpty()
                    ? QStringLiteral("生成SVGの描画範囲が不正です。") : renderer->errorString()));
            return false;
        }
        renderer_ = std::move(renderer);
        svg_ = svg;
    }
    displayIcc_ = displayIcc;
    clearCache();
    update();
    return true;
}

bool PreviewItem::hasVectorPreview() const
{
    return bool(renderer_);
}

QSizeF PreviewItem::vectorSourceSize() const
{
    if (!renderer_) return {};
    QSizeF size = renderer_->viewBoxF().size();
    if (view_ && view_->getImageCore().getCurrentRotation() % 180 != 0) size.transpose();
    return size;
}

void PreviewItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                        QWidget* widget)
{
    const auto rasterFallback = [&] { QGraphicsPixmapItem::paint(painter, option, widget); };
    if (!renderer_ || renderFailed_) {
        rasterFallback();
        return;
    }

    const QRectF bounds = boundingRect();
    if (bounds.isEmpty())
        return;
    const QTransform deviceTransform = painter->deviceTransform();
    bool invertible = false;
    const QTransform inverseDevice = deviceTransform.inverted(&invertible);
    if (!invertible) {
        rasterFallback();
        return;
    }

    // Never allocate the full zoomed image: only the exposed part of the item
    // that intersects the physical paint device is rasterized at its resolution.
    const QRectF exposed = (option ? option->exposedRect : bounds).intersected(bounds);
    if (exposed.isEmpty())
        return;
    QRectF visible = deviceTransform.mapRect(exposed);
    if (!finiteRect(visible)) {
        rasterFallback();
        return;
    }
    visible = visible.adjusted(-1, -1, 1, 1).intersected(physicalDeviceRect(painter->device()));
    if (painter->hasClipping())
        visible = visible.intersected(deviceTransform.mapRect(painter->clipBoundingRect()));
    if (visible.isEmpty())
        return;
    if (!finiteRect(visible) || visible.width() > maxCropSide || visible.height() > maxCropSide) {
        rasterFallback();
        return;
    }
    const QRect crop = visible.toAlignedRect();
    if (qint64(crop.width()) * crop.height() > maxCropPixels) {
        rasterFallback();
        return;
    }

    const int rotation = view_ ? view_->getImageCore().getCurrentRotation() : 0;
    if (cachedImage_.isNull() || cachedDeviceTransform_ != deviceTransform
        || cachedCrop_ != crop || cachedBounds_ != bounds || cachedRotation_ != rotation) {
        QImage image(crop.size(), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) {
            rasterFallback();
            return;
        }
        image.setColorSpace(QColorSpace::SRgb);
        image.fill(Qt::transparent);

        const QRectF box = renderer_->viewBoxF();
        const QTransform rotated = QImage::trueMatrix(QTransform().rotate(rotation),
            qMax(1, qRound(box.width())), qMax(1, qRound(box.height())));
        const QRectF rotatedBounds = rotated.mapRect(QRectF(QPointF(), box.size()));
        const QTransform svgToItem = QTransform::fromTranslate(-box.left(), -box.top())
            * rotated
            * QTransform::fromTranslate(-rotatedBounds.left(), -rotatedBounds.top())
            * QTransform::fromScale(bounds.width() / rotatedBounds.width(),
                                    bounds.height() / rotatedBounds.height())
            * QTransform::fromTranslate(bounds.left(), bounds.top());
        QString error;
        if (!renderer_->render(image, svgToItem * deviceTransform
            * QTransform::fromTranslate(-crop.left(), -crop.top()), &error)) {
            renderFailed_ = true;
            if (view_) QMetaObject::invokeMethod(view_, "vectorRenderingFailed", Qt::QueuedConnection, Q_ARG(QString, error));
            rasterFallback();
            return;
        }

        if (!displayIcc_.isEmpty()) {
            Sr::Profile source;
            source.icc = Sr::srgbProfile();
            image = Sr::convert(image, source, displayIcc_);
            if (image.isNull()) {
                rasterFallback();
                return;
            }
        }
        cachedImage_ = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        cachedDeviceTransform_ = deviceTransform;
        cachedCrop_ = crop;
        cachedBounds_ = bounds;
        cachedRotation_ = rotation;
    }

    // Cancel the complete item-to-device transform, including the device DPR,
    // so that every rasterized SVG sample lands on exactly one physical pixel.
    // Existing clipping and painter opacity remain in effect.
    painter->save();
    painter->setWorldTransform(inverseDevice * painter->worldTransform());
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter->drawImage(crop.topLeft(), cachedImage_);
    painter->restore();
}

}
