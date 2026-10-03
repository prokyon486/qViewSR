// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <memory>

class QThread;

namespace Vector {

// Renderer for the application's generated SVGs. Ordinary documents retain
// Qt's rendering; long paths use librsvg without altering the saved geometry.
class GeneratedSvgRenderer {
public:
    explicit GeneratedSvgRenderer(const QByteArray& svg, bool allowExtendedPaths = true);
    ~GeneratedSvgRenderer();
    bool isValid() const;
    QRectF viewBoxF() const;
    QString errorString() const;
    bool usesExtendedRenderer() const;
    // Rendering is confined to one thread. Call from the current owner after
    // worker rendering finishes, before handing the shared renderer to the UI.
    bool moveToThread(QThread* thread, QString* error = nullptr);
    QThread* thread() const;
    bool matchesSource(const QByteArray& svg) const;
    bool render(QImage& image, const QTransform& sourceToDevice, QString* error = nullptr);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

}
