"""Geometric invariants for optional centerline correction (no GPU/NCS needed)."""
import math
import unittest
import xml.etree.ElementTree as ET

import numpy as np

from color_paths import NS, parse_segments
from line_correction import correct_paths, _flatten, _distance_to_segments


def svg(*paths):
    root = ET.Element(NS + 'svg', {'viewBox': '0 0 1000 1000'})
    for data in paths:
        ET.SubElement(root, NS + 'path', {'d': data, 'stroke': '#123456', 'fill': 'none'})
    return root


def polyline(points, closed=False):
    data = 'M' + ' '.join(map(str, points[0]))
    data += ''.join('L' + ' '.join(map(str, point)) for point in points[1:])
    return data + ('Z' if closed else '')


def geometry(root, index=0):
    return list(parse_segments(list(root.iter(NS+'path'))[index].get('d')))


def outline(axes=(40., 40.), angle=0., count=128, noise=0., center=(100., 100.)):
    theta = np.linspace(0, 2*math.pi, count, endpoint=False)
    # Deterministic small radial noise, less than one source-image pixel.
    radial = 1 + noise/max(axes)*np.sin(7*theta)
    coordinates = np.c_[axes[0]*np.cos(theta)*radial, axes[1]*np.sin(theta)*radial]
    rotation = np.array([[math.cos(angle), -math.sin(angle)], [math.sin(angle), math.cos(angle)]])
    return coordinates @ rotation.T + center


def split_cubic(points, t):
    a = (1-t)*points[:-1] + t*points[1:]
    b = (1-t)*a[:-1] + t*a[1:]
    c = (1-t)*b[0] + t*b[1]
    return np.array([points[0], a[0], b[0], c]), np.array([c, b[1], a[2], points[3]])


class LineCorrectionTests(unittest.TestCase):
    def assertBounded(self, before, after, tolerance=1.):
        original = _flatten(geometry(before), .02)
        corrected = _flatten(geometry(after), .02)
        deviation = max(np.max(_distance_to_segments(original, corrected), initial=0),
                        np.max(_distance_to_segments(corrected, original), initial=0))
        self.assertLessEqual(deviation, tolerance, 'geometry moved beyond source-pixel tolerance')

    def test_near_straight_cubic_becomes_line_endpoints_fixed(self):
        before = svg('M10 20C30 20.4 70 19.6 90 20')
        after, stats = correct_paths(before)
        self.assertEqual(stats['lines'], 1)
        segments = geometry(after)
        self.assertEqual(segments[0]['command'], 'L')
        np.testing.assert_array_equal(segments[0]['points'], [[10, 20], [90, 20]])
        self.assertBounded(before, after)
        self.assertEqual(list(before)[0].get('d'), 'M10 20C30 20.4 70 19.6 90 20')
        self.assertEqual(list(after)[0].get('stroke'), '#123456')

    def test_collinear_run_merges_but_sharp_corner_remains(self):
        before = svg('M0 0L20 .1L40 0L40 30L40.1 60')
        after, stats = correct_paths(before)
        segments = geometry(after)
        self.assertEqual(len(segments), 2)
        np.testing.assert_array_equal(segments[0]['points'][-1], [40, 0])
        self.assertEqual(stats['lines'], 2)
        self.assertBounded(before, after)

    def test_graph_junction_is_preserved_inside_straight_run(self):
        before = svg('M0 10L10 10L20 10L30 10', 'M10 10L10 30')
        after, _ = correct_paths(before)
        self.assertEqual(len(geometry(after)), 2)
        np.testing.assert_array_equal(geometry(after)[0]['points'][-1], [10, 10])
        self.assertEqual(list(after)[1].get('d'), list(before)[1].get('d'))

    def test_noisy_closed_circle_is_regularized(self):
        before = svg(polyline(outline(noise=.2), True))
        after, stats = correct_paths(before)
        self.assertEqual(stats['circles'], 1)
        self.assertEqual(stats['ellipses'], 0)
        self.assertEqual(sum(segment['command']=='C' for segment in geometry(after)), 4)
        self.assertBounded(before, after)

    def test_rotated_noisy_ellipse_is_regularized(self):
        before = svg(polyline(outline((70, 25), angle=.7, noise=.12), True))
        after, stats = correct_paths(before)
        self.assertEqual(stats['ellipses'], 1)
        self.assertEqual(stats['circles'], 0)
        self.assertBounded(before, after)

    def test_clockwise_ellipse_keeps_direction(self):
        before = svg(polyline(outline((70, 25), angle=.7)[::-1], True))
        after, stats = correct_paths(before)
        self.assertEqual(stats['ellipses'], 1)
        first, second = geometry(after)[0]['points'][[0, -1]]
        first_before, second_before = outline((70, 25), angle=.7)[::-1][[0, 1]]
        self.assertGreater(np.dot(second-first, second_before-first_before), 0.)
        self.assertBounded(before, after)

    def test_junction_attached_to_closed_circle_prevents_shape_snap(self):
        points = outline(noise=.2)
        first = points[0]
        before = svg(polyline(points, True), polyline([first, first+[20, 0]]))
        after, stats = correct_paths(before)
        self.assertEqual(stats['circles']+stats['ellipses'], 0)
        np.testing.assert_array_equal(geometry(after)[0]['points'][0], first)
        self.assertBounded(before, after)

    def test_open_partial_arc_stays_open(self):
        theta = np.linspace(0, 1.4*math.pi, 80)
        points = np.c_[40*np.cos(theta), 40*np.sin(theta)]+100
        before = svg(polyline(points))
        after, stats = correct_paths(before)
        self.assertEqual(stats['circles']+stats['ellipses'], 0)
        self.assertFalse(list(after)[0].get('d').endswith('Z'))
        np.testing.assert_allclose(geometry(after)[0]['points'][0], points[0], atol=1e-7)
        np.testing.assert_allclose(geometry(after)[-1]['points'][-1], points[-1], atol=1e-7)
        self.assertBounded(before, after)

    def test_closed_partial_arc_and_star_are_not_ellipses(self):
        theta = np.linspace(0, 1.4*math.pi, 80)
        arc = np.c_[40*np.cos(theta), 40*np.sin(theta)]+100
        theta2 = np.arange(20)*math.pi/10
        radii = np.tile([40., 20.], 10)
        star = np.c_[radii*np.cos(theta2), radii*np.sin(theta2)]+100
        for points in (arc, star):
            with self.subTest(points=len(points)):
                before = svg(polyline(points, True))
                after, stats = correct_paths(before)
                self.assertEqual(stats['circles']+stats['ellipses'], 0)
                self.assertBounded(before, after)

    def test_local_bump_on_round_shape_is_not_erased(self):
        points = outline()
        points[20:26] += [0, 4]
        before = svg(polyline(points, True))
        after, stats = correct_paths(before)
        self.assertEqual(stats['circles']+stats['ellipses'], 0)
        self.assertBounded(before, after)

    def test_smooth_split_cubics_fit_fewer_handles(self):
        original = np.array([[10., 40.], [30., 0.], [80., 70.], [110., 30.]])
        left, right = split_cubic(original, .5)
        pieces = list(split_cubic(left, .5)) + list(split_cubic(right, .5))
        data = 'M10 40' + ''.join('C'+' '.join(map(str, piece[1:].ravel())) for piece in pieces)
        before = svg(data)
        after, stats = correct_paths(before)
        self.assertGreater(stats['simplified_curves'], 0)
        self.assertLess(len(geometry(after)), 4)
        np.testing.assert_array_equal(geometry(after)[0]['points'][0], original[0])
        np.testing.assert_array_equal(geometry(after)[-1]['points'][-1], original[-1])
        self.assertBounded(before, after)

    def test_narrow_reversal_is_not_replaced_by_chord(self):
        before = svg('M0 0C40 0 -30 .1 10 .1')
        after, stats = correct_paths(before)
        self.assertEqual(stats['lines'], 0)
        self.assertEqual(list(after)[0].get('d'), list(before)[0].get('d'))

    def test_multiple_subpaths_do_not_join(self):
        before = svg('M0 0C10 .1 20 -.1 30 0M100 100C110 100.1 120 99.9 130 100')
        after, stats = correct_paths(before)
        segments = geometry(after)
        self.assertEqual(stats['lines'], 2)
        self.assertEqual([segment['subpath'] for segment in segments], [0, 1])
        np.testing.assert_array_equal(segments[1]['points'][0], [100, 100])

    def test_invalid_unsupported_empty_and_tiny_paths_remain_untouched(self):
        before = svg('m0 0l30 30', 'M0 0Q10 10 20 0', '', 'M0 0L0 0', 'M0 0C1e309 0 2 0 3 0')
        after, _ = correct_paths(before)
        self.assertEqual(ET.tostring(after), ET.tostring(before))
        for tolerance in (0, -1, float('nan'), float('inf')):
            with self.assertRaises(ValueError):
                correct_paths(before, tolerance)

    def test_large_raster_trace_geometry_uses_full_error_budget(self):
        # AutoTrace 0.40.0, balanced detail: Pillow 3px outlines with a 3800px
        # diameter circle and 4300x2600px ellipse. These compact trace fixtures
        # avoid allocating the 16-million-pixel raster during every unit test.
        # A too-large safety margin used to reject their 0.85-0.90px variations.
        cases = [
            ('circles', (
                'M2007 152.424'
                'C2023.54 150.252 2041.32 152 2058 152'
                'C2100.11 152 2142.05 154.036 2184 156.911'
                'C2331.28 167.003 2476.97 195.147 2618 238.975'
                'C2917.86 332.162 3197.74 502.836 3414.96 730'
                'C3548.15 869.288 3661.02 1026.76 3747.75 1199'
                'C3885.6 1472.77 3956.48 1783.64 3948.99 2090'
                'C3944.17 2286.72 3910.32 2483.71 3845.66 2670'
                'C3783.75 2848.32 3696.15 3017.43 3584.85 3170'
                'C3401.74 3421 3156.69 3625.41 2877 3760.74'
                'C2692.58 3849.97 2494.01 3908.02 2291 3934.72'
                'C2159.07 3952.07 2023.47 3955.83 1891 3943.17'
                'C1805.11 3934.96 1719.14 3924.06 1635 3904.12'
                'C1566.49 3887.88 1498.57 3869.76 1432 3846.66'
                'C1230.51 3776.71 1041.63 3673.77 874 3541.79'
                'C725.342 3424.74 594.409 3284.8 487.025 3129'
                'C278.725 2826.78 160.969 2465.68 151.985 2099'
                'C151.293 2070.74 150.32 2042.34 151.015 2014'
                'C158.61 1703.97 240.459 1397.24 391.86 1126'
                'C576.644 794.959 860.462 523.708 1199 353.248'
                'C1303.98 300.388 1414.24 258.373 1527 225.424'
                'C1633.07 194.43 1743.06 173.16 1853 162.169'
                'C1887.31 158.739 1921.64 156.441 1956 154.089'
                'C1972.85 152.936 1990.27 154.622 2007 152.424'
            )),
            ('ellipses', (
                'M2191 102.424'
                'C2208.82 100.084 2228.03 102 2246 102'
                'C2283.4 102 2320.7 102.329 2358 103.961'
                'C2392.35 105.464 2426.7 105.741 2461 108.089'
                'C2535.51 113.19 2609.74 119.954 2684 128.845'
                'C2986.95 165.117 3285.6 245.318 3563 372.691'
                'C3733.5 450.981 3896.53 552.197 4036 678.17'
                'C4139.74 771.87 4231.77 880.956 4297.22 1005'
                'C4335.61 1077.76 4364.67 1155.49 4381.79 1236'
                'C4394.95 1297.9 4401.73 1362.71 4398.96 1426'
                'C4395.81 1498.06 4384.44 1569.95 4362.97 1639'
                'C4312.53 1801.32 4215.61 1946.39 4096 2066'
                'C4075.42 2086.58 4054.99 2107.35 4033 2126.42'
                'C3997.12 2157.54 3961.02 2188.11 3923 2216.62'
                'C3813.42 2298.81 3693.46 2369.54 3569 2426.69'
                'C3294.89 2552.55 3002.79 2631.43 2704 2670.72'
                'C2532.06 2693.33 2358.35 2703.26 2185 2699.01'
                'C2125.88 2697.57 2066.97 2695.95 2008 2691.91'
                'C1757.3 2674.74 1507.27 2630.69 1267 2556.03'
                'C1031.48 2482.83 800.909 2379.68 601 2233.85'
                'C479.765 2145.41 369.757 2042.52 281.151 1921'
                'C223.718 1842.24 178.352 1755.77 145.692 1664'
                'C123.222 1600.87 108.594 1532.83 103.91 1466'
                'C100.06 1411.06 100.472 1356.76 107.285 1302'
                'C140.41 1035.77 308.734 806.85 510 638.92'
                'C545.756 609.086 582.709 580.492 621 553.975'
                'C663.076 524.838 705.862 496.917 750 470.989'
                'C807.485 437.221 866.388 405.139 927 377.309'
                'C1011.79 338.376 1097.83 302.952 1186 272.344'
                'C1383.38 203.825 1589 158.504 1796 131.282'
                'C1882.46 119.911 1969.16 113.034 2056 107.089'
                'C2087.59 104.927 2119.37 104.423 2151 103.039'
                'C2164.27 102.459 2177.81 104.156 2191 102.424'
            )),
        ]
        for kind, data in cases:
            for shape_error in (None, 2.):
                with self.subTest(shape=kind, shape_tolerance=shape_error):
                    before = svg(data)
                    after, stats = correct_paths(before, shape_tolerance=shape_error)
                    self.assertEqual(stats[kind], 1)
                    self.assertBounded(before, after, shape_error or 1.)

    def test_very_large_circle_adapts_arc_count_for_subpixel_error(self):
        # Existing curves are already accurate, but slightly uneven control
        # points are repaired without accepting the error of four huge cubics.
        from line_correction import _ellipse_segments, _serialize
        pieces = _ellipse_segments(np.array([100000., 100000.]), np.array([30000., 30000.]), np.eye(2), 0, 1, .5)
        pieces[0]['points'][1] += [.05, 0]
        before = svg(_serialize(pieces, True))
        after, stats = correct_paths(before)
        self.assertEqual(stats['circles'], 1)
        self.assertGreater(sum(segment['command']=='C' for segment in geometry(after)), 4)
        self.assertBounded(before, after)

    def test_shape_tolerance_is_independent_from_cubic_simplification(self):
        circle = svg(polyline(outline((100, 100), noise=1.5), True))
        strict, strict_stats = correct_paths(circle, shape_tolerance=1.)
        relaxed, relaxed_stats = correct_paths(circle, shape_tolerance=3.)
        self.assertEqual(strict_stats['circles'] + strict_stats['ellipses'], 0)
        self.assertEqual(relaxed_stats['circles'], 1)
        self.assertEqual(relaxed_stats['shape_tolerance'], 3.)
        self.assertEqual(relaxed_stats['shape_candidates'], 1)
        self.assertBounded(circle, strict)
        self.assertBounded(circle, relaxed, 3.)

        open_curve = svg('M0 0C5 4 10 5 15 5C20 5 25 4 30 0')
        original, _ = correct_paths(open_curve)
        high_shape_error, _ = correct_paths(open_curve, shape_tolerance=20.)
        self.assertEqual(ET.tostring(original), ET.tostring(high_shape_error))

    def test_pixel_stair_circle_and_rotated_ellipse_are_regularized(self):
        for axes, noise, key in (((40, 40), .7, 'circles'), ((100, 40), 1., 'ellipses')):
            with self.subTest(axes=axes):
                points = np.round(outline(axes, angle=.7, count=512, noise=noise))
                points = points[np.r_[True, np.linalg.norm(np.diff(points, axis=0), axis=1) > 0]]
                before = svg(polyline(points, True))
                _, old_stats = correct_paths(before)
                after, stats = correct_paths(before, shape_tolerance=2.)
                self.assertEqual(old_stats['circles'] + old_stats['ellipses'], 0)
                self.assertEqual(stats[key], 1)
                self.assertBounded(before, after, 2.)

    def test_translated_large_noisy_shapes_keep_pixel_error_bound(self):
        for axes, key in (((3000, 3000), 'circles'), ((4000, 1800), 'ellipses')):
            with self.subTest(axes=axes):
                points = outline(axes, angle=.7, count=1024, noise=3., center=(150000., -170000.))
                before = svg(polyline(points, True))
                after, stats = correct_paths(before, shape_tolerance=4.)
                self.assertEqual(stats[key], 1)
                self.assertBounded(before, after, 4.)

    def test_large_shape_tolerance_does_not_override_relative_error_cap(self):
        before = svg(polyline(outline((10, 10), count=256, noise=1.2), True))
        after, stats = correct_paths(before, shape_tolerance=100.)
        self.assertEqual(stats['circles'] + stats['ellipses'], 0)
        self.assertBounded(before, after)

    def test_relaxed_shape_fit_keeps_open_arc_and_attached_junction(self):
        points = outline((100, 100), noise=1.5)
        for before in (svg(polyline(points[:-1])),
                       svg(polyline(points, True), polyline([points[0], points[0] + [20, 0]]))):
            with self.subTest(paths=len(before)):
                after, stats = correct_paths(before, shape_tolerance=10.)
                self.assertEqual(stats['circles'] + stats['ellipses'], 0)
                np.testing.assert_allclose(geometry(after)[0]['points'][0], points[0])
                self.assertBounded(before, after)

    def test_relaxed_shape_fit_keeps_rounded_rectangle_star_and_partial_loop(self):
        theta = np.linspace(0, 2*math.pi, 256, endpoint=False)
        rounded_rectangle = np.c_[80*np.sign(np.cos(theta))*np.sqrt(np.abs(np.cos(theta))),
                                  50*np.sign(np.sin(theta))*np.sqrt(np.abs(np.sin(theta)))] + 100
        radii = np.where(np.arange(20) % 2, 50., 100.)
        star_theta = np.arange(20)*math.pi/10
        star = np.c_[radii*np.cos(star_theta), radii*np.sin(star_theta)] + 100
        partial_loop = outline((100, 100), count=128)[:90]
        for points in (rounded_rectangle, star, partial_loop):
            with self.subTest(vertices=len(points)):
                before = svg(polyline(points, True))
                after, stats = correct_paths(before, shape_tolerance=20.)
                self.assertEqual(stats['circles'] + stats['ellipses'], 0)
                self.assertBounded(before, after)

    def test_sparse_polygons_and_subdivided_edges_do_not_become_circles(self):
        for count in (4, 8, 12, 16):
            polygon = outline((100, 100), count=count)
            dense = np.concatenate([a + (b-a)*np.arange(20)[:, None]/20
                                    for a, b in zip(polygon, np.roll(polygon, -1, axis=0))])
            for points in (polygon, dense):
                with self.subTest(corners=count, samples=len(points)):
                    before = svg(polyline(points, True))
                    after, stats = correct_paths(before, shape_tolerance=20.)
                    self.assertEqual(stats['circles'] + stats['ellipses'], 0)
                    self.assertBounded(before, after)

    def test_shape_tolerance_must_be_positive_finite(self):
        for tolerance in (0., -1., float('nan'), float('inf')):
            with self.subTest(tolerance=tolerance), self.assertRaises(ValueError):
                correct_paths(svg(), shape_tolerance=tolerance)


if __name__ == '__main__':
    unittest.main()
