"""Conservative, optional geometric cleanup of AutoTrace centerline paths.

All tolerances are in source-image pixels. The original XML tree is never
modified. Only absolute M/L/C/Z output is emitted, so line colors can be sampled
*after* cleanup by color_paths. Open endpoints, sharp corners and graph junctions
are fixed; uncertain fits retain their original geometry.
"""
from collections import Counter
import copy
import math

import numpy as np

from color_paths import NS, parse_segments

CORRECTION_VERSION = 'geometry-2'
_EPS = 1e-9


def _point_key(point):
    return tuple(np.round(point, 5))


def _unit(vector):
    length = np.linalg.norm(vector)
    return vector / length if length > _EPS else np.zeros(2)


def _tangent(segment, end=False):
    points = segment['points']
    candidates = np.diff(points, axis=0)
    if end:
        candidates = candidates[::-1]
    for vector in candidates:
        if np.linalg.norm(vector) > _EPS:
            return _unit(vector)
    return np.zeros(2)


def _corner(before, after):
    a, b = _tangent(before, True), _tangent(after)
    return np.linalg.norm(a) < .5 or np.linalg.norm(b) < .5 or float(a @ b) < math.cos(math.radians(28))


def _distance_to_segments(points, polyline):
    """Exact point-to-polyline distances, with bounded temporary allocations."""
    if len(polyline) == 1:
        return np.linalg.norm(points - polyline[0], axis=1)
    starts = polyline[:-1]
    delta = np.diff(polyline, axis=0)
    norm = np.maximum(np.sum(delta * delta, axis=1), _EPS)
    distances = []
    for offset in range(0, len(points), 64):
        difference = points[offset:offset + 64, None] - starts
        t = np.clip(np.sum(difference * delta, axis=2) / norm, 0., 1.)
        distances.extend(np.sqrt(np.min(np.sum((difference - t[..., None] * delta)**2, axis=2), axis=1)))
    return np.asarray(distances)


def _flatten_cubic(points, error, depth=0):
    chord = points[-1] - points[0]
    length = np.linalg.norm(chord)
    if length > _EPS:
        relative = points[1:3] - points[0]
        perpendicular = np.abs(chord[0]*relative[:, 1] - chord[1]*relative[:, 0]) / length
        projection = (points[1:3] - points[0]) @ chord / (length * length)
        flat = max(perpendicular) <= error and min(projection) >= 0 and max(projection) <= 1
    else:
        flat = max(np.linalg.norm(points[1:] - points[0], axis=1)) <= error
    if flat or depth >= 18:
        return [points[-1]]
    a, b, c = (points[:-1] + points[1:]) / 2
    d, e = (a + b) / 2, (b + c) / 2
    middle = (d + e) / 2
    return (_flatten_cubic(np.array([points[0], a, d, middle]), error, depth + 1)
            + _flatten_cubic(np.array([middle, e, c, points[-1]]), error, depth + 1))


def _flatten(segments, error):
    result = [segments[0]['points'][0]]
    for segment in segments:
        if segment['command'] == 'L':
            result.append(segment['points'][-1])
        else:
            result.extend(_flatten_cubic(segment['points'], error))
    result = np.asarray(result)
    keep = np.r_[True, np.linalg.norm(np.diff(result, axis=0), axis=1) > _EPS]
    return result[keep]


def _segment(command, points):
    return {'command': command, 'points': np.asarray(points, dtype=float), 'close': False}


def _format(value):
    return format(float(value), '.10g')


def _serialize(segments, closed):
    start = segments[0]['points'][0]
    parts = ['M' + ' '.join(map(_format, start))]
    for segment in segments:
        if segment.get('close'):
            continue
        parts.append(segment['command'] + ' '.join(map(_format, segment['points'][1:].ravel())))
    if closed:
        parts.append('Z')
    return ''.join(parts)


def _within_error(original, replacement, tolerance):
    if len(original) > 8192 or len(replacement) > 8192:
        return False
    return (np.max(_distance_to_segments(original, replacement), initial=0) <= tolerance
            and np.max(_distance_to_segments(replacement, original), initial=0) <= tolerance)


def _ellipse_segments(center, axes, basis, angle, direction, tolerance):
    # Four cubics suffice for most circles; more arcs keep the cubic
    # approximation error sub-pixel even for very large source coordinates.
    count = 4
    while max(axes) * .000273 * (4 / count)**6 > tolerance * .08:
        count *= 2
    step = direction * 2 * math.pi / count
    handle = 4 / 3 * math.tan(step / 4)
    result = []
    def point(t):
        return center + basis @ (axes * np.array([math.cos(t), math.sin(t)]))
    def tangent(t):
        return basis @ (axes * np.array([-math.sin(t), math.cos(t)]))
    for i in range(count):
        a, b = angle + i * step, angle + (i + 1) * step
        start, end = point(a), point(b)
        result.append(_segment('C', [start, start + handle * tangent(a), end - handle * tangent(b), end]))
    result[-1]['points'][-1] = result[0]['points'][0]
    return result


def _shape_fit(points, circle, tolerance):
    # Normalize isotropically before least squares to avoid cancellation at
    # 10,000+ pixel coordinates. Reject small/degenerate and partial shapes.
    vertices = points[:-1]
    if len(vertices) < 12 or len(vertices) > 8192:
        return None
    origin = np.mean(vertices, axis=0)
    scale = max(np.ptp(vertices, axis=0)) / 2
    if scale < max(4., tolerance * 6):
        return None
    p = (vertices - origin) / scale
    x, y = p.T
    try:
        if circle:
            solution, _, rank, _ = np.linalg.lstsq(np.c_[2*x, 2*y, np.ones(len(p))], x*x + y*y, rcond=None)
            if rank < 3:
                return None
            c = solution[:2]
            radius2 = solution[2] + c @ c
            if radius2 <= 0:
                return None
            axes, basis = np.full(2, math.sqrt(radius2) * scale), np.eye(2)
        else:
            solution, _, rank, _ = np.linalg.lstsq(np.c_[x*x, x*y, y*y, x, y], np.ones(len(p)), rcond=None)
            if rank < 5:
                return None
            q = np.array([[solution[0], solution[1]/2], [solution[1]/2, solution[2]]])
            c = -.5 * np.linalg.solve(q, solution[3:])
            values, basis = np.linalg.eigh(q)
            level = 1 + c @ q @ c
            if min(values) <= 0 or level <= 0:
                return None
            axes = np.sqrt(level / values) * scale
            if max(axes) / min(axes) > 20:
                return None
        center = origin + c * scale
        # Fit must describe a well resolved outline, not tiny arbitrary blobs.
        if min(axes) < max(3., tolerance * 4):
            return None
        local = (points - center) @ basis / axes
        angles = np.unwrap(np.arctan2(local[:, 1], local[:, 0]))
        changes = np.diff(angles)
        total = angles[-1] - angles[0]
        if abs(abs(total) - 2*math.pi) > .01 or max(np.abs(changes)) > math.pi/4:
            return None
        direction = 1 if total > 0 else -1
        if np.min(changes * direction) < -.003:
            return None
        replacement = _ellipse_segments(center, axes, basis, angles[0], direction, tolerance)
        flat = _flatten(replacement, tolerance * .025)
        # Symmetric distance rejects stars, rounded polygons, self intersections,
        # partial loops and deviations between the measured input samples.
        # Each polyline approximates its true curve within .025*tolerance.
        # Reserve both bounds plus .01 for rounding, instead of rejecting
        # valid large raster circles merely because their trace varies by .8px.
        if not _within_error(points, flat, tolerance * .94):
            return None
        return replacement
    except (ValueError, np.linalg.LinAlgError, FloatingPointError):
        return None


def _bezier(points, t):
    t = np.asarray(t)[:, None]
    return ((1-t)**3 * points[0] + 3*(1-t)**2*t*points[1]
            + 3*(1-t)*t*t*points[2] + t**3*points[3])


def _fit_cubic(points, start_tangent, end_tangent, tolerance):
    """Endpoint-constrained cubic least squares with chord parameter refinement."""
    distances = np.linalg.norm(np.diff(points, axis=0), axis=1)
    if np.sum(distances) < _EPS:
        return None
    t = np.r_[0., np.cumsum(distances)] / np.sum(distances)
    chord = np.linalg.norm(points[-1] - points[0])
    if chord < tolerance:
        return None
    curve = None
    for _ in range(5):
        b0, b1, b2, b3 = (1-t)**3, 3*(1-t)**2*t, 3*(1-t)*t*t, t**3
        residual = points - (b0+b1)[:, None]*points[0] - (b2+b3)[:, None]*points[-1]
        a = np.stack([b1[:, None]*start_tangent, -b2[:, None]*end_tangent], axis=2).reshape(-1, 2)
        handles, _, _, _ = np.linalg.lstsq(a, residual.ravel(), rcond=None)
        if min(handles) <= _EPS or max(handles) > np.sum(distances) * 1.5:
            return None
        curve = np.array([points[0], points[0] + handles[0]*start_tangent,
                          points[-1] - handles[1]*end_tangent, points[-1]])
        evaluated = _bezier(curve, t)
        difference = evaluated - points
        if np.max(np.linalg.norm(difference, axis=1)) <= tolerance * .55:
            break
        # Newton projection is used only to improve the parameter estimate;
        # monotonic ordering is required and the final geometry is checked again.
        u = t[:, None]
        derivative = 3*((1-u)**2*(curve[1]-curve[0]) + 2*(1-u)*u*(curve[2]-curve[1]) + u*u*(curve[3]-curve[2]))
        second = 6*((1-u)*(curve[2]-2*curve[1]+curve[0]) + u*(curve[3]-2*curve[2]+curve[1]))
        denominator = np.sum(derivative*derivative + difference*second, axis=1)
        adjusted = np.clip(t - np.divide(np.sum(difference*derivative, axis=1), denominator,
                                         out=np.zeros_like(t), where=np.abs(denominator)>_EPS), 0, 1)
        adjusted[0], adjusted[-1] = 0., 1.
        if np.any(np.diff(adjusted) < 0):
            break
        t = adjusted
    candidate = [_segment('C', curve)]
    if _within_error(points, _flatten(candidate, tolerance * .06), tolerance * .78):
        return candidate
    return None


def _cleanup_run(segments, tolerance, stats):
    if not segments:
        return []
    points = _flatten(segments, tolerance * .06)
    if len(points) < 2:
        return segments
    chord = points[-1] - points[0]
    length = np.linalg.norm(chord)
    if length > _EPS:
        positions = (points - points[0]) @ chord / length
        distance = _distance_to_segments(points, points[[0, -1]])
        # Do not erase a reversal or a hook, even when it is very narrow.
        if max(distance) <= tolerance * .78 and np.min(np.diff(positions), initial=0) >= -tolerance*.05:
            if len(segments) > 1 or segments[0]['command'] != 'L':
                stats['lines'] += 1
                stats['removed_segments'] += len(segments) - 1
                return [_segment('L', points[[0, -1]])]
            return segments
    if len(segments) < 2 or len(points) > 4096:
        return segments
    candidate = _fit_cubic(points, _tangent(segments[0]), _tangent(segments[-1], True), tolerance)
    if candidate is not None:
        stats['simplified_curves'] += 1
        stats['removed_segments'] += len(segments) - 1
        return candidate
    # Recursively try smaller runs, but never add segments or move join points.
    middle = len(segments) // 2
    return (_cleanup_run(segments[:middle], tolerance, stats)
            + _cleanup_run(segments[middle:], tolerance, stats))


def correct_paths(root, tolerance=1.0):
    """Return ``(new_root, stats)`` with optional near-geometric line cleanup.

    ``tolerance`` must be a positive finite source-pixel distance. Call this
    before source-color segmentation. Unsupported paths are retained verbatim;
    switching this feature off should bypass this function entirely.
    """
    if not math.isfinite(tolerance) or tolerance <= 0:
        raise ValueError('Line correction tolerance must be positive and finite')
    result = copy.deepcopy(root)
    stats = dict(paths=0, lines=0, circles=0, ellipses=0, simplified_curves=0, removed_segments=0)
    parsed = []
    degree = Counter()
    for path in result.iter(NS + 'path'):
        try:
            segments = list(parse_segments(path.get('d', '')))
            if any(not np.isfinite(segment['points']).all() for segment in segments):
                continue
        except (ValueError, TypeError, OverflowError):
            continue
        parsed.append((path, segments))
        for segment in segments:
            if np.linalg.norm(segment['points'][-1] - segment['points'][0]) > _EPS:
                degree[_point_key(segment['points'][0])] += 1
                degree[_point_key(segment['points'][-1])] += 1
    for path, segments in parsed:
        if not segments:
            continue
        subpaths = []
        for segment in segments:
            if not subpaths or segment['subpath'] != subpaths[-1][-1]['subpath']:
                subpaths.append([])
            subpaths[-1].append(segment)
        changed = False
        output = []
        for subpath in subpaths:
            closed = subpath[-1].get('close') or np.linalg.norm(subpath[-1]['points'][-1] - subpath[0]['points'][0]) < _EPS
            # Z at an already coincident endpoint has no geometry.
            working = [segment for segment in subpath if not (segment.get('close') and np.linalg.norm(segment['points'][-1]-segment['points'][0]) < _EPS)]
            if not working:
                output.append(_serialize(subpath, closed))
                continue
            corners = [i for i in range(1, len(working)) if _corner(working[i-1], working[i])
                       or degree[_point_key(working[i]['points'][0])] > 2]
            junction = any(degree[_point_key(segment['points'][0])] > 2 for segment in working)
            fit = None
            if closed and not corners and not junction and not _corner(working[-1], working[0]):
                points = _flatten(working, tolerance * .025)
                for circle, key in ((True, 'circles'), (False, 'ellipses')):
                    fit = _shape_fit(points, circle, tolerance)
                    if fit is not None:
                        stats[key] += 1
                        stats['removed_segments'] += max(0, len(working) - len(fit))
                        break
            if fit is None:
                fit = []
                stops = [0] + corners + [len(working)]
                for start, end in zip(stops[:-1], stops[1:]):
                    # Very long paths are corrected in bounded runs, retaining
                    # every chunk boundary as an additional fixed anchor.
                    for offset in range(start, end, 128):
                        fit.extend(_cleanup_run(working[offset:min(offset+128, end)], tolerance, stats))
            did_change = len(fit) != len(working) or any(a is not b for a, b in zip(fit, working))
            changed |= did_change
            # Retain an unmodified subpath's numeric strings when other
            # subpaths change by serializing only changed paths as a whole.
            output.append(_serialize(fit, closed))
        if changed:
            path.set('d', ''.join(output))
            stats['paths'] += 1
    return result, stats
