"""Thin color boundaries for tracing, without a grayscale-only assumption.

This mode follows transitions between regions, rather than the center of a dark
stroke. It deliberately stays separate from the dark-stroke extractor: combining
both masks would put a second line on either side of an existing dark stroke.
All floating-point intermediates are local to a tile. Its halo covers smoothing,
nonmaximum suppression, and the bounded weak-edge connection completely, so tile
size and scheduling never alter the resulting mask.
"""
from collections import deque
from concurrent.futures import ThreadPoolExecutor
import math
import os

import numpy as np


EDGE_VERSION = 'color-boundaries-2'
MAX_EDGE_THREADS = 16
# This NumPy stage is bandwidth-bound on the target 16-core desktop: four
# workers are as fast as sixteen with substantially fewer temporary buffers.
AUTO_EDGE_THREADS = 4
TILE_SIZE = 512
_CONNECT_STEPS = 8
_HALO = 3 + _CONNECT_STEPS + 1
_TAN_PI_8 = np.float32(math.sqrt(2) - 1)


def _workers(tile_count):
    try:
        count = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        count = os.cpu_count() or 1
    return max(1, min(AUTO_EDGE_THREADS, count, tile_count))


def _neighbors(mask):
    """8-neighborhood, excluding the center, with false outside the tile."""
    padded = np.pad(mask, 1, mode='constant')
    result = np.zeros_like(mask)
    height, width = mask.shape
    for y in range(3):
        for x in range(3):
            if (y, x) != (1, 1):
                result |= padded[y:y + height, x:x + width]
    return result


def _tile(rgb, alpha, bounds, threshold):
    from PIL import ImageFilter
    left, top, right, bottom = bounds
    width, height = rgb.size
    x0, y0 = max(0, left - _HALO), max(0, top - _HALO)
    x1, y1 = min(width, right + _HALO), min(height, bottom + _HALO)
    source = rgb.crop((x0, y0, x1, y1))
    # A 3x3 binomial filter has finite support, unlike a library-dependent
    # approximation of a Gaussian. Replication at the true image edge creates
    # no artificial transition to a padding color.
    values = np.asarray(source, dtype=np.float32)
    padded = np.pad(values, ((0, 0), (1, 1), (0, 0)), mode='edge')
    horizontal = (padded[:, :-2] + 2 * padded[:, 1:-1] + padded[:, 2:]) * .25
    del values, padded
    padded = np.pad(horizontal, ((1, 1), (0, 0), (0, 0)), mode='edge')
    smooth = (padded[:-2] + 2 * padded[1:-1] + padded[2:]) * .25
    del horizontal, padded
    padded = np.pad(smooth, ((1, 1), (1, 1), (0, 0)), mode='edge')
    del smooth
    # A vertical or horizontal sharp step has a peak equal to its RGB channel
    # contrast; existing extraction strengths (6, 12, 20) remain useful values.
    gx = (padded[1:-1, 2:] - padded[1:-1, :-2]) * np.float32(4 / 3)
    gy = (padded[2:, 1:-1] - padded[:-2, 1:-1]) * np.float32(4 / 3)
    del padded
    channel_magnitude = gx * gx + gy * gy
    channel = np.argmax(channel_magnitude, axis=2)[..., None]
    magnitude = np.sqrt(np.take_along_axis(channel_magnitude, channel, axis=2)[..., 0])
    dx = np.take_along_axis(gx, channel, axis=2)[..., 0]
    dy = np.take_along_axis(gy, channel, axis=2)[..., 0]
    del gx, gy, channel_magnitude, channel

    # Keep exactly one side of equal-strength plateaus. Use the most responsive
    # color channel for the normal: isoluminant red/green boundaries must not
    # disappear when their luminance gradients cancel out.
    padded = np.pad(magnitude, 1, mode='constant')
    horizontal = np.abs(dy) <= np.abs(dx) * _TAN_PI_8
    vertical = np.abs(dx) <= np.abs(dy) * _TAN_PI_8
    diagonal_down = (dx * dy) >= 0
    maxima = horizontal & (magnitude >= padded[1:-1, :-2]) & (magnitude > padded[1:-1, 2:])
    maxima |= vertical & (magnitude >= padded[:-2, 1:-1]) & (magnitude > padded[2:, 1:-1])
    diagonal = ~(horizontal | vertical)
    maxima |= diagonal & diagonal_down & (magnitude >= padded[:-2, :-2]) & (magnitude > padded[2:, 2:])
    maxima |= diagonal & ~diagonal_down & (magnitude >= padded[:-2, 2:]) & (magnitude > padded[2:, :-2])
    del padded, dx, dy, horizontal, vertical, diagonal, diagonal_down

    # Invisible RGB never contributes to color contours. Every sample used by
    # a color candidate and its NMS neighbors must be visible. The actual alpha
    # silhouette is added separately below, so black and white transparent
    # logos both retain their outline regardless of the hidden RGB values.
    # Binary alpha is enforced by the worker's source loader.
    alpha_tile = alpha.crop((x0, y0, x1, y1))
    visible = np.asarray(alpha_tile.filter(ImageFilter.MinFilter(7))) > 0
    maxima &= visible
    weak = maxima & (magnitude >= threshold * .5)
    retained = maxima & (magnitude >= threshold)
    del magnitude, maxima, visible
    # Connect at most eight weak pixels to a confidently detected edge. A
    # bounded radius avoids promoting an arbitrarily long chain of texture and
    # makes independent tile processing deterministic with a small halo.
    for _ in range(_CONNECT_STEPS):
        retained |= weak & _neighbors(retained)
    retained &= _neighbors(retained)
    # Take the one-pixel inner rim. Pillow replicates the true image edge, so
    # an opaque image does not acquire an artificial rectangular border. Fully
    # transparent pixels are never emitted, even for single-pixel symbols.
    opaque = np.asarray(alpha_tile) > 0
    interior = np.asarray(alpha_tile.filter(ImageFilter.MinFilter(3))) > 0
    retained |= opaque & ~interior
    return bounds, retained[top - y0:bottom - y0, left - x0:right - x0].copy()


def extract_color_edges(rgb, alpha, threshold, *, tile_size=TILE_SIZE, workers=None):
    """Return an HxW boolean boundary mask from loaded PIL RGB/L images.

    Floating-point work is limited to 512-pixel tiles by default (roughly
    30 MiB per active tile), up to four CPU workers automatically. Only the final
    one-byte per pixel mask is image-sized, in addition to the caller's RGB and
    alpha images.
    ``tile_size`` and ``workers`` are exposed for deterministic testing; ordinary
    callers should keep the defaults. This detects color-region outlines, not
    the centerline of a narrow ink stroke.
    """
    if rgb.mode != 'RGB' or alpha.mode != 'L' or rgb.size != alpha.size:
        raise ValueError('色の境界抽出には同じ大きさのRGB画像とアルファ画像が必要です。')
    if not math.isfinite(threshold) or threshold <= 0:
        raise ValueError('色の境界抽出のしきい値は正の有限値を指定してください。')
    if not isinstance(tile_size, int) or tile_size < 1:
        raise ValueError('タイルの大きさは正の整数を指定してください。')
    if workers is not None and (not isinstance(workers, int) or workers < 1):
        raise ValueError('並列数は正の整数を指定してください。')
    # Decode before concurrent read-only crops; never race Pillow's lazy loader.
    rgb.load()
    alpha.load()
    width, height = rgb.size
    result = np.zeros((height, width), dtype=bool)
    tiles = [(x, y, min(width, x + tile_size), min(height, y + tile_size))
             for y in range(0, height, tile_size) for x in range(0, width, tile_size)]
    count = _workers(len(tiles)) if workers is None else min(workers, MAX_EDGE_THREADS, len(tiles))
    if count <= 1:
        for tile in tiles:
            (left, top, right, bottom), mask = _tile(rgb, alpha, tile, threshold)
            result[top:bottom, left:right] = mask
        return result
    with ThreadPoolExecutor(max_workers=count, thread_name_prefix='color-edge') as pool:
        # Keep queued results bounded even when the earliest tile is slow.
        pending = deque()
        for tile in tiles:
            pending.append(pool.submit(_tile, rgb, alpha, tile, threshold))
            if len(pending) >= 2 * count:
                (left, top, right, bottom), mask = pending.popleft().result()
                result[top:bottom, left:right] = mask
        for future in pending:
            (left, top, right, bottom), mask = future.result()
            result[top:bottom, left:right] = mask
    return result
