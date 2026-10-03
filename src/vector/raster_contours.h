// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QPainterPath>
#include <QPoint>
#include <QString>

namespace Vector {

// Convert alpha >= 128 to vector contours in source-image coordinates. No
// bitmap is embedded. Pixel-edge tracing retains holes, disconnected regions
// and one-pixel features; contours use OddEvenFill. Straight runs are exact.
// Corners are rounded with quadratic Beziers (emitted as cubics), at most
// 0.177 source pixels from the original corner, inside a 0.5-pixel local box.
// ARGB32, ARGB32_Premultiplied, RGBA8888[_Premultiplied] and Alpha8 are read
// directly. Images without an alpha channel are entirely foreground. Other
// alpha formats are rejected rather than making an unbudgeted image copy.
// Limits: 64 Mi pixels, 4 Mi boundary edges, 4 Mi output path elements. The
// scan needs one visited bit per pixel; no all-edge graph/hash is allocated.
// Empty masks/null images return an empty path and clear error. Any failure
// also returns an empty path, with a nonempty error when error is provided.
QPainterPath traceRasterMask(const QImage& mask, QPoint origin = {}, QString* error = nullptr);

}
