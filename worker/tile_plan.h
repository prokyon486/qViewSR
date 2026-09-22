// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace sr {
constexpr int tileWidth = 480, tileHeight = 270, scale = 4;
struct Tile { int x, y, width, height; };
inline std::vector<Tile> plan(int width, int height, int halo) {
    if (width <= 0 || height <= 0 || width > 100000 || height > 100000 || halo < 0 || halo > 64)
        throw std::runtime_error("Invalid image dimensions or halo (0..64).");
    std::vector<Tile> tiles;
    const int cw = tileWidth - 2 * halo, ch = tileHeight - 2 * halo;
    for (int y = 0; y < height; y += ch)
        for (int x = 0; x < width; x += cw)
            tiles.push_back({x, y, std::min(cw, width - x), std::min(ch, height - y)});
    return tiles;
}
inline int reflect(int position, int length) {
    if (length <= 1) return 0;
    const int period = 2 * (length - 1);
    position = (position % period + period) % period;
    return position < length ? position : period - position;
}
}
