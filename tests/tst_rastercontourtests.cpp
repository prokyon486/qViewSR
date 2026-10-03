// SPDX-License-Identifier: GPL-3.0-or-later
#include "vector/raster_contours.h"

#include <QtTest>
#include <QElapsedTimer>
#include <QLineF>
#include <QPainter>
#include <QStringList>
#include <array>

class RasterContourTests : public QObject {
    Q_OBJECT

    static QImage mask(const QStringList& rows)
    {
        if (rows.isEmpty())
            return {};
        QImage image(rows.first().size(), rows.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        for (int y = 0; y < image.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
                if (rows[y][x] == QLatin1Char('#'))
                    row[x] = qRgba(255, 255, 255, 255);
        }
        return image;
    }

    static QImage render(const QPainterPath& path, QSize size, QPoint origin = {})
    {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(-qreal(origin.x()), -qreal(origin.y()));
        painter.fillPath(path, Qt::white);
        painter.end();
        return image;
    }

    static bool sameThresholdedPixels(const QImage& expected, const QImage& actual)
    {
        if (expected.size() != actual.size())
            return false;
        for (int y = 0; y < expected.height(); ++y)
            for (int x = 0; x < expected.width(); ++x)
                if ((qAlpha(expected.pixel(x, y)) >= 128) != (qAlpha(actual.pixel(x, y)) >= 128))
                    return false;
        return true;
    }

    static int subpathCount(const QPainterPath& path)
    {
        int count = 0;
        for (int i = 0; i < path.elementCount(); ++i)
            count += path.elementAt(i).isMoveTo();
        return count;
    }

private slots:
    void emptyMasksAreValidAndClearPreviousErrors()
    {
        QString error = QStringLiteral("previous failure");
        QVERIFY(Vector::traceRasterMask({}, {}, &error).isEmpty());
        QVERIFY(error.isEmpty());
        const QImage image = mask({"...", "..."});
        error = QStringLiteral("previous failure");
        const auto path = Vector::traceRasterMask(image, {}, &error);
        QVERIFY(path.isEmpty());
        QVERIFY(error.isEmpty());
        QCOMPARE(path.fillRule(), Qt::OddEvenFill);
        QVERIFY(sameThresholdedPixels(image, render(path, image.size())));
    }

    void holesIslandsAndOnePixelDetailsRoundTrip()
    {
        const QImage image = mask({
            "###########",
            "#.........#",
            "#..#####..#",
            "#..#...#..#",
            "#..#.#.#..#",
            "#..#...#..#",
            "#..#####..#",
            "#.........#",
            "###########"
        });
        QString error;
        const auto path = Vector::traceRasterMask(image, {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(path.fillRule(), Qt::OddEvenFill);
        QCOMPARE(subpathCount(path), 5);
        QVERIFY(sameThresholdedPixels(image, render(path, image.size())));
        QVERIFY(path.contains(QPointF(0.5, 0.5)));
        QVERIFY(!path.contains(QPointF(1.5, 1.5)));
        QVERIFY(path.contains(QPointF(3.5, 2.5)));
        QVERIFY(!path.contains(QPointF(4.5, 3.5)));
        QVERIFY(path.contains(QPointF(5.5, 4.5)));
    }

    void diagonalContactsStaySeparate()
    {
        const QImage image = mask({"#.#", ".#.", "#.#"});
        QString error;
        const auto path = Vector::traceRasterMask(image, {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(subpathCount(path), 5);
        QVERIFY(sameThresholdedPixels(image, render(path, image.size())));
        // This point lies in the transparent pixel between two diagonal dots.
        QVERIFY(!path.contains(QPointF(1.5, 0.5)));
    }

    void smoothingProducesCurvesWithinThePixelErrorBudget()
    {
        const QImage image = mask({"#"});
        QString error;
        const auto path = Vector::traceRasterMask(image, {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        int curves = 0;
        for (int i = 0; i < path.elementCount(); ++i)
            curves += path.elementAt(i).type == QPainterPath::CurveToElement;
        QCOMPARE(curves, 4);
        QCOMPARE(path.boundingRect(), QRectF(0, 0, 1, 1));
        // Sample the actual Qt cubic path, not the tracing implementation.
        for (int i = 0; i <= 400; ++i) {
            const QPointF point = path.pointAtPercent(qreal(i) / 400);
            QVERIFY(point.x() >= 0 && point.x() <= 1);
            QVERIFY(point.y() >= 0 && point.y() <= 1);
            const qreal distanceToPixelEdge = qMin(qMin(point.x(), 1 - point.x()),
                                                   qMin(point.y(), 1 - point.y()));
            QVERIFY(distanceToPixelEdge <= 0.125001);
        }
        const std::array<QPointF, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (const auto& corner : corners) {
            qreal distance = 2;
            for (int i = 0; i <= 400; ++i)
                distance = qMin(distance, QLineF(corner, path.pointAtPercent(qreal(i) / 400)).length());
            QVERIFY(distance < 0.18);
        }
        QVERIFY(sameThresholdedPixels(image, render(path, image.size())));
    }

    void alphaThresholdAndSupportedFormatsAgree()
    {
        QImage source(4, 2, QImage::Format_ARGB32);
        const std::array<int, 8> alphas{{0, 127, 128, 255, 255, 128, 127, 0}};
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixel(x, y, qRgba(73, 155, 218, alphas[size_t(y * source.width() + x)]));
        const auto reference = Vector::traceRasterMask(source);
        const std::array<QImage::Format, 5> formats{{QImage::Format_ARGB32,
            QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA8888,
            QImage::Format_RGBA8888_Premultiplied, QImage::Format_Alpha8}};
        for (const auto format : formats) {
            const QImage image = source.convertToFormat(format);
            QVERIFY(!image.isNull());
            QString error;
            const auto path = Vector::traceRasterMask(image, {}, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(path, reference);
            const auto raster = render(path, image.size());
            QVERIFY(sameThresholdedPixels(source, raster));
            QVERIFY(qAlpha(raster.pixel(1, 0)) < 128);
            QVERIFY(qAlpha(raster.pixel(2, 0)) >= 128);
        }
        QImage opaque(3, 2, QImage::Format_RGB32);
        opaque.fill(Qt::black); // Alpha, rather than RGB brightness, defines the mask.
        QString error;
        const auto opaquePath = Vector::traceRasterMask(opaque, {}, &error);
        QVERIFY(error.isEmpty());
        QVERIFY(sameThresholdedPixels(opaque, render(opaquePath, opaque.size())));
    }

    void sourceOriginAndInputImmutability()
    {
        const QImage image = mask({"##..#", "#.#..", "###.#"});
        const QImage original = image.copy();
        const std::array<QPoint, 3> origins{{{113, -71}, {-117, 59}, {17980, 21110}}};
        for (const auto origin : origins) {
            QString error;
            const auto first = Vector::traceRasterMask(image, origin, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            const auto second = Vector::traceRasterMask(image, origin, &error);
            QCOMPARE(first, second);
            QVERIFY(sameThresholdedPixels(image, render(first, image.size(), origin)));
        }
        QCOMPARE(image, original);
    }

    void allFourByFourBinaryMasksRoundTrip()
    {
        QElapsedTimer timer;
        timer.start();
        QImage image(4, 4, QImage::Format_ARGB32_Premultiplied);
        QString error;
        for (quint32 bits = 0; bits < 65536; ++bits) {
            for (int y = 0; y < 4; ++y) {
                auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
                for (int x = 0; x < 4; ++x)
                    row[x] = (bits & (1U << (y * 4 + x))) ? qRgba(255, 255, 255, 255) : 0;
            }
            const auto path = Vector::traceRasterMask(image, {}, &error);
            if (!error.isEmpty())
                QFAIL(qPrintable(QStringLiteral("Mask %1 failed: %2").arg(bits).arg(error)));
            if (!sameThresholdedPixels(image, render(path, image.size())))
                QFAIL(qPrintable(QStringLiteral("Rendered pixels differ for binary mask %1").arg(bits)));
        }
        qInfo() << "65,536 binary masks traced and Qt-rasterized in" << timer.elapsed() << "ms";
    }

    void unsupportedAlphaFormatReportsFailure()
    {
        QImage image(1, 1, QImage::Format_RGBA64_Premultiplied);
        image.fill(Qt::white);
        QString error;
        const auto path = Vector::traceRasterMask(image, {}, &error);
        QVERIFY(path.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void pixelBudgetRejectsWithoutPartialOutput()
    {
        // Alpha8 keeps the fixture allocation below 65 MiB. No fill is needed:
        // the size check must reject the mask before reading its pixels.
        QImage image(8193, 8192, QImage::Format_Alpha8);
        QVERIFY(!image.isNull());
        QString error;
        const auto path = Vector::traceRasterMask(image, {}, &error);
        QVERIFY(path.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void boundaryBudgetRejectsWithoutPartialOutput()
    {
        QImage image(2 * 1024 * 1024, 1, QImage::Format_Alpha8);
        QVERIFY(!image.isNull());
        image.fill(255);
        QString error;
        const auto path = Vector::traceRasterMask(image, {}, &error);
        QVERIFY(path.isEmpty());
        QVERIFY2(error.contains(QStringLiteral("境界辺")), qPrintable(error));
    }

    void pathElementBudgetRejectsWithoutPartialOutput()
    {
        QImage image(1024, 1024, QImage::Format_Alpha8);
        QVERIFY(!image.isNull());
        for (int y = 0; y < image.height(); ++y) {
            auto* row = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x)
                row[x] = ((x + y) & 1) ? 255 : 0;
        }
        QString error;
        const auto path = Vector::traceRasterMask(image, {}, &error);
        QVERIFY(path.isEmpty());
        QVERIFY2(error.contains(QStringLiteral("ベクター点数")), qPrintable(error));
    }
};

QTEST_GUILESS_MAIN(RasterContourTests)
#include "tst_rastercontourtests.moc"
