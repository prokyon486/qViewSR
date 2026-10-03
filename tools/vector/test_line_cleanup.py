"""Topology and geometry invariants for optional generated-centerline cleanup."""
import math
import unittest
import xml.etree.ElementTree as ET

import numpy as np

from color_paths import NS, parse_segments
from line_cleanup import cleanup_lines


def svg(*paths):
    root = ET.Element(NS+'svg', {'viewBox': '0 0 1000 1000'})
    for data in paths:
        ET.SubElement(root, NS+'path', {'d': data, 'stroke': '#123456', 'fill': 'none'})
    return root


def strokes(root):
    result = []
    for path in root.iter(NS+'path'):
        groups = []
        for segment in parse_segments(path.get('d', '')):
            if not groups or groups[-1][-1]['subpath'] != segment['subpath']:
                groups.append([])
            groups[-1].append(segment)
        result.extend(groups)
    return result


def path(points):
    return 'M'+' '.join(map(str, points[0]))+''.join('L'+' '.join(map(str, point)) for point in points[1:])


class LineCleanupTests(unittest.TestCase):
    def test_isolated_short_strokes_removed_but_closed_symbols_retained(self):
        before = svg('M0 0L2 0M20 20L22 20L22 22L20 22Z', 'M100 0L120 0')
        original = ET.tostring(before)
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['isolated_removed'], 1)
        self.assertEqual(len(strokes(after)), 2)
        self.assertTrue(strokes(after)[0][-1]['close'])
        self.assertEqual(ET.tostring(before), original)
        self.assertEqual(list(after)[0].get('stroke'), '#123456')

    def test_connected_short_marks_and_short_crossings_are_retained(self):
        before = svg('M0 0L3 0', 'M3 0L3 3', 'M100 0L100 10', 'M97 5L103 5')
        after, stats = cleanup_lines(before, min_length=7, branch_strength=0)
        self.assertEqual(stats['isolated_removed'], 0)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_rotated_perpendicular_spur_removed_and_mainline_preserved(self):
        angle = .61
        basis = np.array([[math.cos(angle), -math.sin(angle)], [math.sin(angle), math.cos(angle)]])
        main = np.array([[-40., 0.], [40., 0.]]) @ basis.T + 100
        spur = np.array([[0., 0.], [0., 4.]]) @ basis.T + 100
        before = svg(path(main), path(spur))
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['spurs_removed'], 1)
        self.assertEqual(list(after)[0].get('d'), list(before)[0].get('d'))
        self.assertEqual(len(strokes(after)), 1)

    def test_spur_at_fragmented_mainline_removed_and_main_arms_joined(self):
        before = svg('M-40 0L0 0M0 0L40 0M0 0L0 3')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['spurs_removed'], 1)
        self.assertEqual(stats['joins'], 1)
        self.assertEqual(len(strokes(after)), 1)
        np.testing.assert_array_equal(strokes(after)[0][0]['points'][0], [-40, 0])
        np.testing.assert_array_equal(strokes(after)[0][-1]['points'][-1], [40, 0])

    def test_short_mainline_fragments_provide_long_directional_support(self):
        before = svg(''.join(f'M{x} 0L{x+5} 0' for x in range(-40, 40, 5))+'M0 0L0 3')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['spurs_removed'], 1)
        self.assertEqual(stats['joins'], 15)
        self.assertEqual(len(strokes(after)), 1)
        np.testing.assert_array_equal(strokes(after)[0][0]['points'][0], [-40, 0])
        np.testing.assert_array_equal(strokes(after)[0][-1]['points'][-1], [40, 0])

    def test_radial_spur_removed_without_changing_closed_outline(self):
        theta = np.linspace(0., 2*math.pi, 128, endpoint=False)
        points = np.c_[40*np.cos(theta), 40*np.sin(theta)]+100
        before = svg(path(points)+'Z', 'M140 100L143 100')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['spurs_removed'], 1)
        self.assertEqual(list(after)[0].get('d'), list(before)[0].get('d'))
        self.assertTrue(strokes(after)[0][-1]['close'])

    def test_complex_junction_does_not_lose_short_branch(self):
        before = svg('M-40 0L0 0M0 0L40 0M0 0L0 40M0 0L0 -3')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['spurs_removed'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_long_t_branch_prevents_join_and_remains_present(self):
        before = svg('M-40 0L0 0M0 0L40 0M0 0L0 20')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['spurs_removed'], 0)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_corner_arm_does_not_count_as_smooth_mainline(self):
        before = svg('M-40 0L0 0L0 40', 'M0 0L3 -3')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['spurs_removed'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_nearby_aligned_chain_joined_with_endpoints_fixed(self):
        before = svg('M0 0L20 0M22 0L42 0M44 0L64 0')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['joins'], 2)
        self.assertEqual(len(strokes(after)), 1)
        np.testing.assert_array_equal(strokes(after)[0][0]['points'][0], [0, 0])
        np.testing.assert_array_equal(strokes(after)[0][-1]['points'][-1], [64, 0])
        self.assertEqual([item['command'] for item in strokes(after)[0]], ['L', 'C', 'L', 'C', 'L'])

    def test_cubic_reversal_retains_control_points(self):
        before = svg('M0 0C5 0 15 0 20 0', 'M42 0C37 4 27 0 22 0')
        after, stats = cleanup_lines(before, min_length=0, branch_strength=0)
        self.assertEqual(stats['joins'], 1)
        segments = strokes(after)[0]
        np.testing.assert_array_equal(segments[-1]['points'], [[22, 0], [27, 0], [37, 4], [42, 0]])
        np.testing.assert_array_equal(segments[0]['points'], [[0, 0], [5, 0], [15, 0], [20, 0]])

    def test_parallel_neighbors_and_perpendicular_endpoints_do_not_join(self):
        for before in (svg('M0 0L20 0', 'M0 2L20 2'), svg('M0 0L20 0', 'M22 0L22 20')):
            with self.subTest(data=ET.tostring(before)):
                after, stats = cleanup_lines(before)
                self.assertEqual(stats['joins'], 0)
                self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_crossing_inside_gap_blocks_join(self):
        before = svg('M0 0L20 0', 'M24 0L44 0', 'M22 -20L22 20')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_bridge_cannot_cross_earlier_part_of_its_own_stroke(self):
        before = svg('M22 -20L22 20L0 20L0 0L20 0', 'M24 0L44 0')
        after, stats = cleanup_lines(before, min_length=0, branch_strength=0)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_cubic_bridge_bulge_cannot_cross_a_third_stroke(self):
        before = svg('M-19.405914525519929 -4.8384379119933545L0 0',
                     'M20 0L39.405914525519933 -4.8384379119933545', 'M1 1.1L19 1.1')
        after, stats = cleanup_lines(before, min_length=0, join_distance=20, branch_strength=0)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_two_new_bridges_cannot_cross_each_other(self):
        before = svg('M-30 0L-10 0', 'M10 0L30 0', 'M0 -30L0 -10', 'M0 10L0 30')
        after, stats = cleanup_lines(before, min_length=0, join_distance=20, branch_strength=0)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_diagonal_crossing_near_grid_corner_is_detected(self):
        before = svg('M100 195L201 195', 'M209 195L300 195', 'M190 180L260 250')
        after, stats = cleanup_lines(before, min_length=0, join_distance=200, branch_strength=0)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_ambiguous_facing_endpoints_are_not_arbitrarily_selected(self):
        before = svg('M0 0L20 0', 'M23 .5L43 .5', 'M23 -.5L43 -.5')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_small_gap_in_long_circular_outline_can_close(self):
        theta = np.linspace(0., math.radians(340), 100)
        points = np.c_[10*np.cos(theta), 10*np.sin(theta)]+100
        before = svg(path(points))
        after, stats = cleanup_lines(before, min_length=0, branch_strength=0)
        self.assertEqual(stats['joins'], 1)
        self.assertEqual(stats['closed_gaps'], 1)
        self.assertTrue(strokes(after)[0][-1]['close'])
        np.testing.assert_array_equal(strokes(after)[0][0]['points'][0], points[0])

    def test_attributes_transforms_and_unsupported_paths_are_respected(self):
        before = svg('M0 0L20 0', 'M22 0L42 0', 'M100 100Q100 101 101 101')
        list(before)[1].set('stroke', '#abcdef')
        transformed = ET.SubElement(before, NS+'g', {'transform': 'scale(10)'})
        ET.SubElement(transformed, NS+'path', {'d': 'M200 200L201 200'})
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_strength_zero_disables_spur_deletion_and_zero_parameters_noop(self):
        before = svg('M-40 0L0 0M0 0L40 0M0 0L0 3M100 100L101 100')
        after, stats = cleanup_lines(before, branch_strength=0)
        self.assertEqual(stats['spurs_removed'], 0)
        self.assertEqual(stats['isolated_removed'], 1)
        after, _ = cleanup_lines(before, 0, 0, 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_degenerate_subpaths_preserved_alongside_normal_paths(self):
        before = svg('M0 0L20 0M50 50L50 50M100 100L101 100')
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['isolated_removed'], 1)
        self.assertEqual(len(strokes(after)), 2)
        np.testing.assert_array_equal(strokes(after)[1][0]['points'], [[50, 50], [50, 50]])
        self.assertFalse(strokes(after)[1][0]['close'])

    def test_deterministic_large_spatially_disjoint_input(self):
        before = svg(''.join(f'M{i*40} 0L{i*40+1} 0M{i*40} 100L{i*40+20} 100M{i*40+22} 100L{i*40+30} 100' for i in range(1200)))
        after, stats = cleanup_lines(before)
        again, again_stats = cleanup_lines(before)
        self.assertEqual(stats['isolated_removed'], 1200)
        self.assertEqual(stats['joins'], 1200)
        self.assertEqual(len(strokes(after)), 1200)
        self.assertEqual(ET.tostring(after), ET.tostring(again))
        self.assertEqual(stats, again_stats)

    def test_dense_ambiguous_cells_are_retained(self):
        before = svg(''.join(f'M0 {i*.001}L1 {i*.001}' for i in range(150)))
        after, stats = cleanup_lines(before)
        self.assertEqual(stats['isolated_removed'], 0)
        self.assertEqual(stats['joins'], 0)
        self.assertEqual(ET.tostring(after), ET.tostring(before))

    def test_invalid_parameters_rejected(self):
        for kwargs in ({'min_length': -1}, {'join_distance': float('inf')}, {'branch_strength': 101}, {'branch_strength': float('nan')}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                cleanup_lines(svg('M0 0L1 1'), **kwargs)


if __name__ == '__main__':
    unittest.main()
