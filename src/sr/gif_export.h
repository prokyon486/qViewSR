// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QImage>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>

namespace Sr {
// Frames are immutable, composed sRGB images. Timing uses Qt's loop convention.
// Writes atomically; cancellation or an error leaves any existing file intact.
QString writeGif(const QString& path, const QVector<QImage>& frames, const QVector<int>& delays,
                 int loops, int rotation, const std::atomic_bool& cancelled,
                 const std::function<void(int)>& progress = {});
}
