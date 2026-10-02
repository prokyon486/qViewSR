"""Helpers for AutoTrace absolute M/L/C/Z centerline output.

Geometry is retained verbatim per segment; sampled color is a heuristic,
not semantic separation or restoration of an original vector illustration.
"""
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


def color_paths(root, source, exclude_transparent=False):
    """Return [{d, color, subpath, command}] with colors sampled per segment.

    Five interior curve samples each choose the median RGB of the darkest
    three valid pixels in a 3x3 neighborhood. Median across those samples
    limits isolated noise. Long segments spanning different colors receive
    one color, so gradients are not reconstructed.
    """
    arr = np.asarray(source, dtype=np.uint8)
    if arr.ndim != 3 or arr.shape[2] not in (3, 4):
        raise ValueError('RGB or RGBA source expected')
    height, width = arr.shape[:2]
    binary_alpha = arr[:, :, 3] if arr.shape[2] == 4 else None
    check_alpha = exclude_transparent and binary_alpha is not None and np.any(binary_alpha == 0)
    result = []
    for path in root.iter(NS + 'path'):
        for seg in parse_segments(path.get('d', '')):
            if check_alpha:
                bound = np.linalg.norm(np.diff(seg['points'], axis=0), axis=1).sum()
                points = sample_segment(seg, np.linspace(0, 1, max(2, int(np.ceil(bound/.5)) + 1)))
                indices = np.rint(points-.5).astype(int)
                indices[:, 0] = np.clip(indices[:, 0], 0, width-1)
                indices[:, 1] = np.clip(indices[:, 1], 0, height-1)
                # Do not paint source-color strokes through invisible pixels.
                # Whole segments are omitted instead of introducing raster masks.
                if np.any(binary_alpha[indices[:, 1], indices[:, 0]] == 0):
                    continue
            samples = []
            for x, y in sample_segment(seg, np.linspace(.1, .9, 5)):
                # Raster pixels have centers at x+.5/y+.5 in SVG coordinates.
                ix, iy = int(round(x-.5)), int(round(y-.5))
                ix, iy = min(max(ix, 0), width-1), min(max(iy, 0), height-1)
                pixels = arr[max(0,iy-1):min(height,iy+2),
                             max(0,ix-1):min(width,ix+2)].reshape(-1, arr.shape[2])
                if arr.shape[2] == 4:
                    pixels = pixels[pixels[:, 3] > 127]
                if not len(pixels):
                    continue
                rgb = pixels[:, :3].astype(float)
                lum = rgb @ np.array([.2126, .7152, .0722])
                samples.append(np.median(rgb[np.argsort(lum)[:3]], axis=0))
            if not samples:
                continue
            rgb = np.rint(np.median(samples, axis=0)).astype(int)
            result.append({'d': seg['d'], 'color': '#%02x%02x%02x' % tuple(rgb),
                           'subpath': seg['subpath'], 'command': seg['command']})
    return result
