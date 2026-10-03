"""Helpers for AutoTrace absolute M/L/C/Z centerline output.

Geometry is retained verbatim per segment; sampled color is a heuristic,
not semantic separation or restoration of an original vector illustration.
"""
from itertools import islice
import re
import numpy as np

TOKEN = re.compile(r'[A-Za-z]|[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?')
NS = '{http://www.w3.org/2000/svg}'


def parse_segments(d):
    """Yield drawable segments with exact original numeric strings and endpoints."""
    tokens = TOKEN.findall(d)
    i = 0
    current = start = None
    command = None
    subpath = -1
    while i < len(tokens):
        if tokens[i].isalpha():
            command = tokens[i]
            i += 1
        if command not in ('M', 'L', 'C', 'Z'):
            raise ValueError(f'Unsupported SVG command {command!r}')
        if command == 'Z':
            if current is None or start is None:
                raise ValueError('Z without M')
            vals = list(start)
            yield {'d': 'M' + ' '.join(current) + 'L' + ' '.join(vals),
                   'command': 'L', 'points': np.array([current, vals], dtype=float),
                   'subpath': subpath, 'close': True}
            current = start
            command = None
            continue
        count = 6 if command == 'C' else 2
        vals = tokens[i:i + count]
        if len(vals) != count or any(t.isalpha() for t in vals):
            raise ValueError('Incomplete SVG segment')
        i += count
        if command == 'M':
            current = start = tuple(vals)
            subpath += 1
            command = 'L'
            continue
        if current is None:
            raise ValueError('Segment without M')
        points = np.array([*current, *vals], dtype=float).reshape(-1, 2)
        yield {'d': 'M' + ' '.join(current) + command + ' '.join(vals),
               'command': command, 'points': points, 'subpath': subpath,
               'close': False}
        current = tuple(vals[-2:])


def sample_segment(segment, ts):
    points = segment['points']
    t = np.asarray(ts)[:, None]
    if segment['command'] == 'L':
        return (1 - t) * points[0] + t * points[1]
    return ((1-t)**3 * points[0] + 3*(1-t)**2*t*points[1]
            + 3*(1-t)*t**2*points[2] + t**3*points[3])


# Keep temporary coordinate/neighborhood arrays bounded independently of image
# dimensions and total path count. The source image is read-only and shared.
_COLOR_BATCH_SIZE = 4096


def _batch_colors(segments, arr):
    height, width = arr.shape[:2]
    coordinates = np.empty((len(segments), 5, 2), dtype=np.float64)
    t = np.linspace(.1, .9, 5)[None, :, None]
    for command in ('L', 'C'):
        indices = [i for i, segment in enumerate(segments) if segment['command'] == command]
        if not indices:
            continue
        points = np.stack([segments[i]['points'] for i in indices])
        if command == 'L':
            coordinates[indices] = (1-t)*points[:, 0, None, :] + t*points[:, 1, None, :]
        else:
            coordinates[indices] = ((1-t)**3*points[:, 0, None, :] + 3*(1-t)**2*t*points[:, 1, None, :]
                                    + 3*(1-t)*t**2*points[:, 2, None, :] + t**3*points[:, 3, None, :])
    if not np.isfinite(coordinates).all():
        raise ValueError('Non-finite SVG sample coordinates')
    # Match Python round's ties-to-even pixel-center convention. Clip in float
    # first so very large finite SVG coordinates cannot overflow int64.
    coordinates = np.rint(coordinates-.5)
    coordinates[:, :, 0] = np.clip(coordinates[:, :, 0], 0, width-1)
    coordinates[:, :, 1] = np.clip(coordinates[:, :, 1], 0, height-1)
    centers = coordinates.astype(np.int64).reshape(-1, 2)
    x = centers[:, 0, None] + np.tile(np.arange(-1, 2), 3)
    y = centers[:, 1, None] + np.repeat(np.arange(-1, 2), 3)
    valid = (x >= 0) & (x < width) & (y >= 0) & (y < height)
    pixels = arr[np.clip(y, 0, height-1), np.clip(x, 0, width-1)]
    if arr.shape[2] == 4:
        valid &= pixels[:, :, 3] > 127
    counts = valid.sum(axis=1)
    colors = np.full((len(centers), 3), np.nan)
    # Group by actual neighbor count. Compacting in original row-major order
    # lets argsort see exactly the old cropped/alpha-filtered array, including
    # equal luminance ties at borders. Repeating clamped pixels would change
    # which darkest three pixels supply the median and is intentionally avoided.
    for count in range(1, 10):
        indices = np.flatnonzero(counts == count)
        if not len(indices):
            continue
        rgb = pixels[indices][valid[indices]].reshape(-1, count, arr.shape[2])[:, :, :3].astype(float)
        luminance = rgb @ np.array([.2126, .7152, .0722])
        order = np.argsort(luminance, axis=1)[:, :3]
        darkest = np.take_along_axis(rgb, order[:, :, None], axis=1)
        colors[indices] = np.median(darkest, axis=1)
    colors = colors.reshape(-1, 5, 3)
    retained = np.flatnonzero(~np.isnan(colors[:, :, 0]).all(axis=1))
    rgb = np.rint(np.nanmedian(colors[retained], axis=1)).astype(int)
    return [{'d': segments[i]['d'], 'color': '#%02x%02x%02x' % tuple(color),
             'subpath': segments[i]['subpath'], 'command': segments[i]['command']}
            for i, color in zip(retained, rgb)]


def color_paths(root, source, exclude_transparent=False):
    """Return [{d, color, subpath, command}] with colors sampled per segment.

    Five interior curve samples each choose the median RGB of the darkest
    three valid pixels in a 3x3 neighborhood. Median across those samples
    limits isolated noise. Long segments spanning different colors receive
    one color, so gradients are not reconstructed. Bounded batches preserve
    those choices while avoiding thousands of tiny NumPy calls per path.
    """
    arr = np.asarray(source, dtype=np.uint8)
    if arr.ndim != 3 or arr.shape[2] not in (3, 4):
        raise ValueError('RGB or RGBA source expected')
    height, width = arr.shape[:2]
    binary_alpha = arr[:, :, 3] if arr.shape[2] == 4 else None
    # A reduction avoids allocating another image-sized boolean array.
    check_alpha = exclude_transparent and binary_alpha is not None and binary_alpha.min(initial=255) == 0

    def segments():
        for path in root.iter(NS + 'path'):
            for segment in parse_segments(path.get('d', '')):
                if check_alpha:
                    bound = np.linalg.norm(np.diff(segment['points'], axis=0), axis=1).sum()
                    points = sample_segment(segment, np.linspace(0, 1, max(2, int(np.ceil(bound/.5)) + 1)))
                    indices = np.rint(points-.5).astype(int)
                    indices[:, 0] = np.clip(indices[:, 0], 0, width-1)
                    indices[:, 1] = np.clip(indices[:, 1], 0, height-1)
                    # Keep the complete-segment transparency check. Testing only
                    # the five color samples could paint across an invisible gap.
                    if np.any(binary_alpha[indices[:, 1], indices[:, 0]] == 0):
                        continue
                yield segment

    source_segments = segments()
    result = []
    while batch := list(islice(source_segments, _COLOR_BATCH_SIZE)):
        result.extend(_batch_colors(batch, arr))
    return result
