"""Optional topology-aware cleanup of generated AutoTrace centerlines.

Distances are source-image pixels. Short isolated open strokes and small side
spurs can be discarded; uniquely facing endpoints can be joined. These are
geometric heuristics, not semantic recognition: use the original lines when a
small mark is meaningful. Closed contours, corners within retained strokes and
uncertain/dense junctions are preserved. The caller's XML tree is untouched.
"""
from collections import defaultdict
import copy
from dataclasses import dataclass
import math

import numpy as np

from color_paths import NS, parse_segments

CLEANUP_VERSION = 'topology-3'
_EPS = 1e-8
_MAX_CELL_ITEMS = 128


def _unit(vector):
    length = math.hypot(float(vector[0]), float(vector[1]))
    return np.asarray(vector, dtype=float) / length if length > _EPS else np.zeros(2)


def _outward(segments, end):
    points = segments[-1 if end else 0]['points']
    point = points[-1 if end else 0]
    others = points[-2::-1] if end else points[1:]
    for other in others:
        delta = point - other
        if np.linalg.norm(delta) > _EPS:
            return _unit(delta)
    return np.zeros(2)


def _flatten_cubic(points, output, depth=0):
    chord = points[-1] - points[0]
    length = np.linalg.norm(chord)
    if length > _EPS:
        relative = points[1:3] - points[0]
        distance = np.abs(chord[0]*relative[:, 1] - chord[1]*relative[:, 0]) / length
        projection = relative @ chord / (length*length)
        flat = max(distance) <= .2 and min(projection) >= 0 and max(projection) <= 1
    else:
        flat = max(np.linalg.norm(points[1:] - points[0], axis=1)) <= .2
    if flat or depth == 15:
        output.append(points[-1])
        return
    a, b, c = (points[:-1] + points[1:]) / 2
    d, e = (a+b)/2, (b+c)/2
    middle = (d+e)/2
    _flatten_cubic(np.array([points[0], a, d, middle]), output, depth+1)
    _flatten_cubic(np.array([middle, e, c, points[-1]]), output, depth+1)


@dataclass(slots=True)
class _Stroke:
    owner: object
    scope: tuple
    segments: list
    points: np.ndarray
    distances: np.ndarray
    length: float
    closed: bool
    tangents: tuple

    def endpoint(self, side):
        return self.points[-1 if side else 0]

    def at(self, distance):
        distance = min(self.length, max(0., distance))
        index = min(len(self.points)-2, int(np.searchsorted(self.distances, distance, side='right'))-1)
        span = self.distances[index+1]-self.distances[index]
        return self.points[index] + (self.points[index+1]-self.points[index]) * ((distance-self.distances[index])/span if span > _EPS else 0.)


class _Grid:
    """Bounded occupancy: ambiguous dense cells are skipped, not all-paired."""
    def __init__(self, size):
        self.size = size
        self.cells = {}

    def cell(self, point):
        return (math.floor(float(point[0])/self.size), math.floor(float(point[1])/self.size))

    def add(self, cell, item):
        values = self.cells.setdefault(cell, [])
        if values is not None:
            if len(values) == _MAX_CELL_ITEMS:
                self.cells[cell] = None
            else:
                values.append(item)

    def nearby(self, point, radius):
        low = self.cell(point-radius)
        high = self.cell(point+radius)
        result = set()
        for x in range(low[0], high[0]+1):
            for y in range(low[1], high[1]+1):
                values = self.cells.get((x, y), ())
                if values is None:
                    return None
                result.update(values)
        return result


def _line_cells(a, b, size):
    """Supercover DDA; include both side cells at exact grid-corner crossings."""
    x, y = math.floor(float(a[0])/size), math.floor(float(a[1])/size)
    end_x, end_y = math.floor(float(b[0])/size), math.floor(float(b[1])/size)
    dx, dy = float(b[0]-a[0]), float(b[1]-a[1])
    step_x = 1 if dx > 0 else -1 if dx < 0 else 0
    step_y = 1 if dy > 0 else -1 if dy < 0 else 0
    t_x = (((x+(step_x > 0))*size-float(a[0]))/dx) if step_x else math.inf
    t_y = (((y+(step_y > 0))*size-float(a[1]))/dy) if step_y else math.inf
    delta_x = size/abs(dx) if step_x else math.inf
    delta_y = size/abs(dy) if step_y else math.inf
    yield (x, y)
    while x != end_x or y != end_y:
        # Freeze an axis once its endpoint cell is reached. This also avoids
        # stepping beyond a boundary endpoint due to accumulated round-off.
        if x == end_x:
            t_x = math.inf
        if y == end_y:
            t_y = math.inf
        if t_x < t_y:
            x += step_x
            t_x += delta_x
        elif t_y < t_x:
            y += step_y
            t_y += delta_y
        else:
            yield (x+step_x, y)
            yield (x, y+step_y)
            x += step_x
            y += step_y
            t_x += delta_x
            t_y += delta_y
        yield (x, y)


def _geometry_index(strokes, cell_size):
    grid = _Grid(cell_size)
    for identity, stroke in enumerate(strokes):
        for index, (a, b) in enumerate(zip(stroke.points[:-1], stroke.points[1:])):
            for cell in _line_cells(a, b, cell_size):
                grid.add(cell, (identity, index))
    return grid


def _intersection(a, b, c, d):
    """Return an intersection point, or True for collinear overlap."""
    r, delta = b-a, d-c
    cross = lambda u, v: float(u[0]*v[1]-u[1]*v[0])
    denominator = cross(r, delta)
    offset = c-a
    if abs(denominator) > _EPS:
        t, u = cross(offset, delta)/denominator, cross(offset, r)/denominator
        if -_EPS <= t <= 1+_EPS and -_EPS <= u <= 1+_EPS:
            return a+max(0., min(1., t))*r
        return None
    if abs(cross(offset, r)) > _EPS:
        return None
    length2 = float(r @ r)
    if length2 <= _EPS:
        return None
    t0, t1 = sorted((float((c-a) @ r)/length2, float((d-a) @ r)/length2))
    low, high = max(0., t0), min(1., t1)
    if high < low-_EPS:
        return None
    if high-low > _EPS:
        return True
    return a+((low+high)/2)*r


def _bridge_crosses(geometry, strokes, identity, side, other, other_side, removed):
    first, second = strokes[identity], strokes[other]
    a, b = first.endpoint(side), second.endpoint(other_side)
    bridge = _bridge(a, first.tangents[side], b, second.tangents[other_side])
    if not bridge:
        return False
    points = [a]
    _flatten_cubic(bridge[0]['points'], points)
    allowed = {(identity, 0 if side == 0 else len(first.points)-2): [a]}
    allowed.setdefault((other, 0 if other_side == 0 else len(second.points)-2), []).append(b)
    for start, end in zip(points[:-1], points[1:]):
        candidates = set()
        for x, y in _line_cells(start, end, geometry.size):
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    values = geometry.cells.get((x+dx, y+dy), ())
                    if values is None:
                        return True
                    candidates.update(values)
        for node, index in candidates:
            if node in removed:
                continue
            left, right = strokes[node].points[index:index+2]
            intersection = _intersection(start, end, left, right)
            if intersection is None:
                continue
            if intersection is not True and any(np.linalg.norm(intersection-point) <= _EPS
                                                 for point in allowed.get((node, index), ())):
                continue
            return True
    return False


def _reject_crossing_bridges(strokes, links, size):
    """Discard both ambiguous new bridges if they cross one another."""
    proposals = []
    index = _Grid(size)
    for key, target in sorted(links.items()):
        if key >= target:
            continue
        first, second = strokes[key[0]], strokes[target[0]]
        a, b = first.endpoint(key[1]), second.endpoint(target[1])
        bridge = _bridge(a, first.tangents[key[1]], b, second.tangents[target[1]])
        points = [a]
        if bridge:
            _flatten_cubic(bridge[0]['points'], points)
        identity = len(proposals)
        proposals.append((key, target, points))
        for segment, (start, end) in enumerate(zip(points[:-1], points[1:])):
            for cell in _line_cells(start, end, size):
                index.add(cell, (identity, segment))
    rejected = set()
    for identity, (_, _, points) in enumerate(proposals):
        for start, end in zip(points[:-1], points[1:]):
            candidates = set()
            for x, y in _line_cells(start, end, size):
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        values = index.cells.get((x+dx, y+dy), ())
                        if values is None:
                            rejected.add(identity)
                        else:
                            candidates.update(values)
            for other, segment in candidates:
                if other <= identity:
                    continue
                left, right = proposals[other][2][segment:segment+2]
                if _intersection(start, end, left, right) is not None:
                    rejected.update((identity, other))
    for identity in rejected:
        key, target, _ = proposals[identity]
        del links[key]
        del links[target]
    return len(rejected)


def _near_geometry(grid, strokes, point, radius, exclude=(), removed=()):
    candidates = grid.nearby(point, radius+.21)
    if candidates is None:
        return None
    result = {}
    for identity, index in candidates:
        if identity in exclude or identity in removed:
            continue
        stroke = strokes[identity]
        a, b = stroke.points[index:index+2]
        delta = b-a
        norm = float(delta @ delta)
        t = min(1., max(0., float((point-a) @ delta)/norm)) if norm > _EPS else 0.
        projected = a+t*delta
        distance = float(np.linalg.norm(point-projected))
        if distance <= radius+.2:
            entry = (distance, index, t, projected)
            if identity not in result or entry[:3] < result[identity][:3]:
                result[identity] = entry
    return result


def _support_point(strokes, endpoints, identity, position, direction, distance,
                   max_gap, removed, exclude):
    """Follow a unique gentle continuation when the main stroke is fragmented."""
    visited = set(exclude)
    aligned, facing = math.cos(math.radians(30.)), math.cos(math.radians(35.))
    for _ in range(64):
        if identity in visited:
            return None
        visited.add(identity)
        stroke = strokes[identity]
        if stroke.closed:
            return stroke.at((position+direction*distance) % stroke.length) if stroke.length >= distance*4 else None
        available = position if direction < 0 else stroke.length-position
        if available >= distance:
            return stroke.at(position+direction*distance)
        distance -= available
        side = int(direction > 0)
        point, tangent = stroke.endpoint(side), stroke.tangents[side]
        nearby = endpoints.nearby(point, max_gap)
        if nearby is None:
            return None
        matches = []
        for other, other_side in sorted(nearby):
            if other in visited or other in removed:
                continue
            target = strokes[other]
            if target.scope != stroke.scope:
                continue
            next_point = target.endpoint(other_side)
            gap = float(np.linalg.norm(next_point-point))
            other_tangent = target.tangents[other_side]
            if gap > max_gap or float(tangent @ other_tangent) > -aligned:
                continue
            if gap > _EPS and (float(tangent @ ((next_point-point)/gap)) < facing
                               or float(other_tangent @ ((point-next_point)/gap)) < facing):
                continue
            matches.append((other, other_side, gap))
        if len(matches) != 1:
            return None
        identity, incoming, gap = matches[0]
        next_point = strokes[identity].endpoint(incoming)
        if gap >= distance:
            return point+(next_point-point)*(distance/gap)
        distance -= gap
        direction = -1 if incoming else 1
        position = strokes[identity].length if incoming else 0.
    return None


def _format(point):
    return ' '.join(format(float(value), '.17g') for value in point)


def _serialize(segments, closed):
    text = ['M'+_format(segments[0]['points'][0])]
    for segment in segments:
        if not segment.get('close'):
            text.append(segment['command']+_format(segment['points'][1:].ravel()))
    if closed:
        text.append('Z')
    return ''.join(text)


def _oriented(stroke, reversed_):
    if not reversed_:
        return stroke.segments[:]
    return [dict(command=segment['command'], points=segment['points'][::-1], close=False)
            for segment in stroke.segments[::-1]]


def _bridge(a, tangent_a, b, tangent_b):
    length = float(np.linalg.norm(b-a))
    if length < _EPS:
        return []
    # Both input tangents point away from their respective open ends.
    return [dict(command='C', points=np.array([a, a+tangent_a*length/3,
                                              b+tangent_b*length/3, b]), close=False)]


def cleanup_lines(root, min_length=6., join_distance=4., branch_strength=50):
    """Return ``(new_root, stats)``; bypass this function to keep raw lines.

    ``min_length`` controls isolated strokes and the maximum short-spur length;
    zero disables both kinds of deletion. ``join_distance`` limits
    gaps between mutually unique, tangent-aligned endpoints. ``branch_strength``
    (0..100) controls removal of tiny perpendicular side spurs; zero disables it.
    Set both distances to zero and branch strength to zero for an exact no-op.
    Geometry with transforms or unsupported commands is retained untouched.
    """
    parameters = (min_length, join_distance, branch_strength)
    if any(not math.isfinite(value) for value in parameters) or min_length < 0 or join_distance < 0 or not 0 <= branch_strength <= 100:
        raise ValueError('Cleanup distances must be finite and nonnegative; branch strength must be 0..100')
    result = copy.deepcopy(root)
    stats = dict(strokes=0, isolated_removed=0, spurs_removed=0, joins=0,
                 closed_gaps=0, ambiguous_endpoints=0)
    if not any(parameters):
        return result, stats
    parents = {child: parent for parent in result.iter() for child in parent}
    transformed = set()
    def visit(element, inherited=False):
        inherited = inherited or bool(element.get('transform'))
        if inherited:
            transformed.add(element)
        for child in element:
            visit(child, inherited)
    visit(result)
    strokes = []
    owners = defaultdict(list)
    for path in result.iter(NS+'path'):
        if path in transformed:
            continue
        try:
            segments = list(parse_segments(path.get('d', '')))
            if not segments or any(not np.isfinite(segment['points']).all() or np.max(np.abs(segment['points'])) > 1e7 for segment in segments):
                continue
        except (ValueError, TypeError, OverflowError):
            continue
        subpaths = []
        for segment in segments:
            if not subpaths or segment['subpath'] != subpaths[-1][-1]['subpath']:
                subpaths.append([])
            subpaths[-1].append(segment)
        scope = (id(parents.get(path)), tuple(sorted((key, value) for key, value in path.attrib.items() if key != 'd')))
        for subpath in subpaths:
            points = [subpath[0]['points'][0]]
            for segment in subpath:
                if segment['command'] == 'C':
                    _flatten_cubic(segment['points'], points)
                else:
                    points.append(segment['points'][-1])
            points = np.asarray(points)
            keep = np.r_[True, np.linalg.norm(np.diff(points, axis=0), axis=1) > _EPS]
            points = points[keep]
            if len(points) < 2:
                # A zero-length dot may be a meaningful round-capped mark.
                # Preserve it without disabling cleanup of every sibling in
                # AutoTrace's single, potentially very large path element.
                points = np.repeat(points, 2, axis=0)
            distances = np.r_[0., np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
            closed = bool(subpath[-1].get('close')) or (distances[-1] > _EPS and np.linalg.norm(points[0]-points[-1]) < _EPS)
            identity = len(strokes)
            strokes.append(_Stroke(path, scope, subpath, points, distances, float(distances[-1]), closed,
                                   (_outward(subpath, False), _outward(subpath, True))))
            owners[path].append(identity)
    stats['strokes'] = len(strokes)
    if not strokes:
        return result, stats
    size = max(2., join_distance, min_length*.5)
    geometry = _geometry_index(strokes, size)
    support_endpoints = _Grid(size)
    for identity, stroke in enumerate(strokes):
        if not stroke.closed and stroke.length > _EPS:
            for side in (0, 1):
                support_endpoints.add(support_endpoints.cell(stroke.endpoint(side)), (identity, side))
    contact = min(.75, max(.2, join_distance*.25))
    isolation_radius = max(1., join_distance, min_length*.5)
    removed = set()
    for identity, stroke in enumerate(strokes):
        if stroke.closed or stroke.length <= _EPS or stroke.length >= min_length:
            continue
        # Interior samples prevent deleting a short crossing whose endpoints
        # both happen to lie away from the supporting line.
        count = max(2, int(math.ceil(stroke.length/max(.5, isolation_radius*.5)))+1)
        isolated = True
        for distance in np.linspace(0., stroke.length, count):
            nearby = _near_geometry(geometry, strokes, stroke.at(distance), isolation_radius, (identity,))
            if nearby is None or nearby:
                isolated = False
                break
        if isolated:
            removed.add(identity)
    stats['isolated_removed'] = len(removed)
    if branch_strength:
        maximum = min_length*(.5+branch_strength/100.)
        ratio = .15+branch_strength*.0015
        perpendicular = math.cos(math.radians(75.-branch_strength*.15))
        smooth = math.cos(math.radians(25.))
        for identity, stroke in enumerate(strokes):
            if identity in removed or stroke.closed or stroke.length <= _EPS or stroke.length > maximum:
                continue
            required = max(2.*min_length, stroke.length/ratio)
            for side in (0, 1):
                anchor, tip = stroke.endpoint(side), stroke.endpoint(1-side)
                nearby = _near_geometry(geometry, strokes, anchor, contact, (identity,), removed)
                tip_nearby = _near_geometry(geometry, strokes, tip, contact, (identity,), removed)
                if nearby is None or tip_nearby is None or not nearby or tip_nearby:
                    continue
                arms = []
                branch = -stroke.tangents[side]
                for other, (_, index, t, projection) in sorted(nearby.items()):
                    main = strokes[other]
                    position = main.distances[index]+t*(main.distances[index+1]-main.distances[index])
                    local = _unit(main.points[index+1]-main.points[index])
                    for direction in (-1, 1):
                        # At a path endpoint this direction has no geometry of
                        # its own. The adjoining stroke supplies the other arm.
                        available = position if direction < 0 else main.length-position
                        if not main.closed and available <= contact:
                            continue
                        supported = _support_point(strokes, support_endpoints, other, position,
                                                   direction, required, max(contact, join_distance),
                                                   removed, (identity,))
                        if supported is not None:
                            arm = _unit(supported-projection)
                            if float(arm @ (local*direction)) >= smooth:
                                arms.append(arm)
                # More than two supported directions is a meaningful or
                # ambiguous junction, not a simple side spur.
                good_pair = (len(arms) == 2 and float(arms[0] @ arms[1]) <= -smooth
                             and all(abs(float(arm @ branch)) <= perpendicular for arm in arms))
                if good_pair:
                    removed.add(identity)
                    stats['spurs_removed'] += 1
                    break
    links = {}
    if join_distance > 0:
        endpoints = _Grid(size)
        for identity, stroke in enumerate(strokes):
            if identity not in removed and not stroke.closed and stroke.length > _EPS:
                for side in (0, 1):
                    endpoints.add(endpoints.cell(stroke.endpoint(side)), (identity, side))
        candidates = {}
        facing = math.cos(math.radians(35.))
        aligned = math.cos(math.radians(30.))
        for identity, stroke in enumerate(strokes):
            if identity in removed or stroke.closed or stroke.length <= _EPS:
                continue
            for side in (0, 1):
                key = (identity, side)
                a = stroke.endpoint(side)
                nearby = endpoints.nearby(a, join_distance)
                if nearby is None:
                    stats['ambiguous_endpoints'] += 1
                    continue
                matches = []
                for other, other_side in sorted(nearby):
                    if key == (other, other_side):
                        continue
                    target = strokes[other]
                    if stroke.scope != target.scope:
                        continue
                    b = target.endpoint(other_side)
                    distance = float(np.linalg.norm(b-a))
                    if distance > join_distance or (other == identity and stroke.length < join_distance*8):
                        continue
                    u, v = stroke.tangents[side], target.tangents[other_side]
                    if float(u @ v) > -aligned:
                        continue
                    if distance > _EPS and (float(u @ ((b-a)/distance)) < facing or float(v @ ((a-b)/distance)) < facing):
                        continue
                    # Do not bridge over a retained branch/crossing. Query the
                    # gap as well as both endpoints, at sub-pixel intervals.
                    blocked = False
                    for point in np.linspace(a, b, max(2, int(math.ceil(distance/.5))+1)):
                        nearby_geometry = _near_geometry(geometry, strokes, point, contact, (identity, other), removed)
                        if nearby_geometry is None or nearby_geometry:
                            blocked = True
                            break
                    if not blocked and not _bridge_crosses(geometry, strokes, identity, side, other, other_side, removed):
                        matches.append((other, other_side))
                if len(matches) == 1:
                    candidates[key] = matches[0]
                elif len(matches) > 1:
                    stats['ambiguous_endpoints'] += 1
        for key, target in sorted(candidates.items()):
            if candidates.get(target) == key:
                links[key] = target
        stats['ambiguous_endpoints'] += 2*_reject_crossing_bridges(strokes, links, size)
        stats['joins'] = len(links)//2
    # Walk chains/cycles using their smallest original stroke as a stable
    # anchor. Reversed cubics swap their handles as well as their endpoints.
    replacements = {}
    consumed = set(removed)
    for identity, stroke in enumerate(strokes):
        if identity in consumed:
            continue
        component = set()
        pending = [identity]
        while pending:
            node = pending.pop()
            if node in component:
                continue
            component.add(node)
            pending.extend(links[(node, side)][0] for side in (0, 1) if (node, side) in links)
        if len(component) == 1 and not any((identity, side) in links for side in (0, 1)):
            replacements[identity] = None
            consumed.add(identity)
            continue
        free = sorted((node, side) for node in component for side in (0, 1) if (node, side) not in links)
        start = free[0] if free else (min(component), 0)
        current = start
        output = []
        closed = False
        while True:
            node, incoming = current
            member = strokes[node]
            output.extend(_oriented(member, bool(incoming)))
            consumed.add(node)
            outgoing = (node, 1-incoming)
            target = links.get(outgoing)
            if target is None:
                break
            next_member = strokes[target[0]]
            output.extend(_bridge(member.endpoint(1-incoming), member.tangents[1-incoming],
                                  next_member.endpoint(target[1]), next_member.tangents[target[1]]))
            if target == start:
                closed = True
                stats['closed_gaps'] += 1
                break
            current = target
        replacements[min(component)] = _serialize(output, closed)
    for owner, identities in owners.items():
        if all(identity in replacements and replacements[identity] is None for identity in identities):
            continue
        output = []
        for identity in identities:
            if identity in replacements:
                data = replacements[identity]
                output.append(data if data is not None else _serialize(strokes[identity].segments, strokes[identity].closed))
        if output or owner.get('id'):
            owner.set('d', ''.join(output))
        else:
            parents[owner].remove(owner)
    return result, stats
