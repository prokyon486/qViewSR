// SPDX-License-Identifier: GPL-3.0-or-later
#include "raster_contours.h"

#include <QVector>
#include <QtGlobal>
#include <array>
#include <cmath>
#include <new>
#include <vector>

namespace Vector {
namespace {
constexpr quint64 MaxMaskPixels = 64ULL * 1024 * 1024;
constexpr quint64 MaxBoundaryEdges = 4ULL * 1024 * 1024;
constexpr quint64 MaxPathElements = 4ULL * 1024 * 1024;
constexpr qreal CornerRadius = 0.5;
constexpr std::array<int, 4> Dx{{1, 0, -1, 0}};
constexpr std::array<int, 4> Dy{{0, 1, 0, -1}};

class MaskReader {
public:
    explicit MaskReader(const QImage& image)
        : bytes_(image.constBits()), stride_(image.bytesPerLine()),
          width_(image.width()), height_(image.height()), format_(image.format())
    {
    }

    bool foreground(int x, int y) const
    {
        if (quint32(x) >= quint32(width_) || quint32(y) >= quint32(height_))
            return false;
        const uchar* row = bytes_ + qsizetype(y) * stride_;
        switch (format_) {
        case QImage::Format_Alpha8:
            return row[x] >= 128;
        case QImage::Format_RGBA8888:
        case QImage::Format_RGBA8888_Premultiplied:
            return row[qsizetype(x) * 4 + 3] >= 128;
        default: // The caller validated native-endian ARGB32.
            return qAlpha(reinterpret_cast<const QRgb*>(row)[x]) >= 128;
        }
    }

    // Direction E/S/W/N, with foreground on the right of each directed edge.
    bool boundary(int x, int y, int direction) const
    {
        switch (direction) {
        case 0: return foreground(x, y) && !foreground(x, y - 1);
        case 1: return foreground(x - 1, y) && !foreground(x, y);
        case 2: return foreground(x - 1, y - 1) && !foreground(x - 1, y);
        default: return foreground(x, y - 1) && !foreground(x - 1, y - 1);
        }
    }

private:
    const uchar* bytes_;
    qsizetype stride_;
    int width_;
    int height_;
    QImage::Format format_;
};

bool supportedAlphaFormat(QImage::Format format)
{
    return format == QImage::Format_ARGB32 || format == QImage::Format_ARGB32_Premultiplied
        || format == QImage::Format_RGBA8888 || format == QImage::Format_RGBA8888_Premultiplied
        || format == QImage::Format_Alpha8;
}

QPainterPath fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    QPainterPath empty;
    empty.setFillRule(Qt::OddEvenFill);
    return empty;
}

bool appendContour(QPainterPath& path, const QVector<QPoint>& corners, QPoint origin)
{
    // Each corner produces a cubic (3 elements), possibly preceded by a line;
    // allow an extra move and close element. Check before growing QPainterPath.
    const quint64 estimate = quint64(path.elementCount()) + quint64(corners.size()) * 4 + 2;
    if (corners.size() < 4 || estimate > MaxPathElements)
        return false;
    const qsizetype count = corners.size();
    for (qsizetype index = 0; index < count; ++index) {
        // Convert before adding origin to avoid signed integer overflow near
        // the extremes of the source-coordinate range.
        const QPointF corner = QPointF(corners[index]) + QPointF(origin);
        const QPointF before = QPointF(corners[(index + count - 1) % count]) + QPointF(origin);
        const QPointF after = QPointF(corners[(index + 1) % count]) + QPointF(origin);
        const QPointF incoming = before - corner;
        const QPointF outgoing = after - corner;
        const qreal beforeLength = std::hypot(incoming.x(), incoming.y());
        const qreal afterLength = std::hypot(outgoing.x(), outgoing.y());
        if (beforeLength <= 0 || afterLength <= 0)
            return false;
        const qreal radius = qMin(CornerRadius, qMin(beforeLength, afterLength) / 2);
        const QPointF entry = corner + incoming * (radius / beforeLength);
        const QPointF exit = corner + outgoing * (radius / afterLength);
        if (index == 0)
            path.moveTo(entry);
        else
            path.lineTo(entry);
        // Exact quadratic-to-cubic conversion. Since traced corners are right
        // angles and radius <= half either adjacent edge, smoothing cannot
        // jump over another corner or remove a one-pixel-wide region/hole.
        path.cubicTo(entry + (corner - entry) * (2.0 / 3.0),
                     exit + (corner - exit) * (2.0 / 3.0), exit);
    }
    path.closeSubpath();
    return true;
}
}

QPainterPath traceRasterMask(const QImage& mask, QPoint origin, QString* error)
{
    if (error)
        error->clear();
    QPainterPath result;
    result.setFillRule(Qt::OddEvenFill);
    if (mask.isNull())
        return result;
    const quint64 pixels = quint64(mask.width()) * quint64(mask.height());
    if (pixels > MaxMaskPixels)
        return fail(error, QStringLiteral("塗りの編集範囲が64メガピクセルを超えています。範囲を小さくしてください。"));
    try {
        if (!mask.hasAlphaChannel()) {
            const QVector<QPoint> corners{{0, 0}, {mask.width(), 0},
                                          {mask.width(), mask.height()}, {0, mask.height()}};
            if (!appendContour(result, corners, origin))
                return fail(error, QStringLiteral("塗りの輪郭をベクターへ変換できませんでした。"));
            return result;
        }
        if (!supportedAlphaFormat(mask.format()))
            return fail(error, QStringLiteral("塗りのマスク形式が未対応です。ARGB32またはAlpha8を使用してください。"));
        const MaskReader reader(mask);
        // Every closed directed contour contains an eastward (top) edge.
        // Mark only those edges: a single bit per pixel is sufficient to
        // discover each contour once, even when components touch at corners.
        std::vector<uchar> visited(size_t((pixels + 7) / 8), uchar(0));
        const auto topEdgeSeen = [&](int x, int y) {
            const quint64 index = quint64(y) * quint64(mask.width()) + quint64(x);
            return (visited[size_t(index >> 3)] & uchar(1U << (index & 7))) != 0;
        };
        const auto markTopEdge = [&](int x, int y) {
            const quint64 index = quint64(y) * quint64(mask.width()) + quint64(x);
            visited[size_t(index >> 3)] |= uchar(1U << (index & 7));
        };
        quint64 boundaryEdges = 0;
        QVector<QPoint> corners;
        for (int y = 0; y < mask.height(); ++y) {
            for (int x = 0; x < mask.width(); ++x) {
                if (!reader.boundary(x, y, 0) || topEdgeSeen(x, y))
                    continue;
                corners.clear();
                int vertexX = x;
                int vertexY = y;
                int direction = 0;
                do {
                    if (++boundaryEdges > MaxBoundaryEdges)
                        return fail(error, QStringLiteral("塗りの輪郭が複雑すぎます。編集範囲を小さくしてください（境界辺の上限4194304）。"));
                    if (direction == 0) {
                        if (topEdgeSeen(vertexX, vertexY))
                            return fail(error, QStringLiteral("塗りの輪郭を閉じられませんでした。"));
                        markTopEdge(vertexX, vertexY);
                    }
                    const int nextX = vertexX + Dx[size_t(direction)];
                    const int nextY = vertexY + Dy[size_t(direction)];
                    int nextDirection = -1;
                    // At a checkerboard vertex, turn right first: diagonal
                    // foreground pixels stay separate instead of crossing.
                    const std::array<int, 4> choices{{(direction + 1) & 3, direction,
                                                      (direction + 3) & 3, (direction + 2) & 3}};
                    for (const int choice : choices) {
                        if (reader.boundary(nextX, nextY, choice)) {
                            nextDirection = choice;
                            break;
                        }
                    }
                    if (nextDirection < 0)
                        return fail(error, QStringLiteral("塗りの輪郭を閉じられませんでした。"));
                    if (nextDirection != direction) {
                        corners.append(QPoint(nextX, nextY));
                        if (quint64(corners.size()) * 4 + quint64(result.elementCount()) + 2 > MaxPathElements)
                            return fail(error, QStringLiteral("塗りの輪郭が複雑すぎます。編集範囲を小さくしてください（ベクター点数の上限4194304）。"));
                    }
                    vertexX = nextX;
                    vertexY = nextY;
                    direction = nextDirection;
                } while (vertexX != x || vertexY != y || direction != 0);
                if (!appendContour(result, corners, origin))
                    return fail(error, QStringLiteral("塗りの輪郭をベクターへ変換できませんでした。"));
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(error, QStringLiteral("塗りの輪郭を変換するメモリを確保できませんでした。"));
    }
    return result;
}
}
