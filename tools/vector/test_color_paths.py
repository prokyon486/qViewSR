"""Compatibility tests for bounded, vectorized centerline color sampling."""
import unittest
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image

from color_paths import NS, color_paths, parse_segments, sample_segment


def original_colors(root, source, exclude_transparent=False):
    """Previous scalar sampling implementation, retained as a compatibility oracle."""
    arr = np.asarray(source, dtype=np.uint8)
    height, width = arr.shape[:2]
    alpha = arr[:, :, 3] if arr.shape[2] == 4 else None
    check = exclude_transparent and alpha is not None and np.any(alpha == 0)
    result = []
    for path in root.iter(NS + 'path'):
        for segment in parse_segments(path.get('d', '')):
            if check:
                bound = np.linalg.norm(np.diff(segment['points'], axis=0), axis=1).sum()
                points = sample_segment(segment, np.linspace(0, 1, max(2, int(np.ceil(bound/.5))+1)))
                indices = np.rint(points-.5).astype(int)
                indices[:, 0] = np.clip(indices[:, 0], 0, width-1)
                indices[:, 1] = np.clip(indices[:, 1], 0, height-1)
                if np.any(alpha[indices[:, 1], indices[:, 0]] == 0):
                    continue
            samples = []
            for x, y in sample_segment(segment, np.linspace(.1, .9, 5)):
                ix, iy = int(round(x-.5)), int(round(y-.5))
                ix, iy = min(max(ix, 0), width-1), min(max(iy, 0), height-1)
                pixels = arr[max(0, iy-1):min(height, iy+2), max(0, ix-1):min(width, ix+2)].reshape(-1, arr.shape[2])
                if arr.shape[2] == 4:
                    pixels = pixels[pixels[:, 3] > 127]
                if not len(pixels):
                    continue
                rgb = pixels[:, :3].astype(float)
                luminance = rgb @ np.array([.2126, .7152, .0722])
                samples.append(np.median(rgb[np.argsort(luminance)[:3]], axis=0))
            if samples:
                rgb = np.rint(np.median(samples, axis=0)).astype(int)
                result.append({'d': segment['d'], 'color': '#%02x%02x%02x' % tuple(rgb),
                               'subpath': segment['subpath'], 'command': segment['command']})
    return result


def svg(*paths):
    root = ET.Element(NS + 'svg')
    for data in paths:
        ET.SubElement(root, NS + 'path', {'d': data})
    return root


class ColorPathsTests(unittest.TestCase):
    def test_exact_geometry_numeric_strings_and_subpath_order(self):
        root = svg('M1.00 2e0C3.0 4.00 5 6 7 8L9 8Z M20 20L21 21', 'M2 3L4 5')
        image = Image.new('RGB', (24, 24), '#924050')
        expected = original_colors(root, image)
        self.assertEqual(color_paths(root, image), expected)
        self.assertTrue(all(item['color'] == '#924050' for item in expected))
        self.assertEqual(expected[0]['d'], 'M1.00 2e0C3.0 4.00 5 6 7 8')

    def test_small_borders_transparency_and_mixed_curves_match_original(self):
        rng = np.random.default_rng(90210)
        for size in [(1, 1), (1, 7), (9, 1), (7, 9), (64, 48)]:
            for channels in [3, 4]:
                arr = rng.integers(0, 256, (size[1], size[0], channels), dtype=np.uint8)
                if channels == 4:
                    arr[:, :, 3] = rng.choice([0, 127, 128, 255], size=arr.shape[:2])
                root = svg()
                for i in range(70):
                    points = rng.uniform(-3, max(size)+3, (4, 2))
                    data = 'M' + ' '.join(map(str, points[0]))
                    data += ('L' + ' '.join(map(str, points[1]))) if i % 2 == 0 else ('C' + ' '.join(map(str, points[1:].ravel())))
                    ET.SubElement(root, NS + 'path', {'d': data})
                for exclude in [False, True]:
                    with self.subTest(size=size, channels=channels, exclude=exclude):
                        self.assertEqual(color_paths(root, arr, exclude), original_colors(root, arr, exclude))

    def test_equal_luminance_neighbor_order_and_partial_alpha(self):
        # Two different RGB values have equal Rec.709 luminance to its specified
        # decimal precision. Selection order must follow the old argsort inputs.
        palette = np.array([[246, 0, 78], [0, 81, 0], [0, 0, 0], [255, 255, 255]], dtype=np.uint8)
        rng = np.random.default_rng(743)
        rgb = palette[rng.integers(0, len(palette), size=(11, 13))]
        alpha = rng.choice([0, 127, 128, 255], size=(11, 13)).astype(np.uint8)
        root = svg(*(f'M{x} {y}L{x+1} {y+1}' for y in range(-1, 13) for x in range(-1, 15)))
        for source in [rgb, np.dstack([rgb, alpha])]:
            for exclude in [False, True]:
                self.assertEqual(color_paths(root, source, exclude), original_colors(root, source, exclude))

    def test_transparent_gap_between_color_samples_drops_whole_segment(self):
        image = Image.new('RGBA', (101, 9), '#924050')
        for y in range(image.height):
            image.putpixel((16, y), (0, 0, 0, 0))
        root = svg('M.5 4.5L100.5 4.5')
        self.assertEqual(color_paths(root, image, True), [])
        self.assertEqual(color_paths(root, image, False)[0]['color'], '#924050')

    def test_fewer_than_three_valid_neighbors_and_empty_results(self):
        root = svg('M0 0L1 1', 'M-100 -100C0 0 100 100 200 200')
        for alpha in [0, 127, 128, 255]:
            image = Image.new('RGBA', (1, 1), (200, 90, 30, alpha))
            result = color_paths(root, image)
            self.assertEqual(result, original_colors(root, image))
            self.assertEqual(len(result), 2 if alpha > 127 else 0)
        self.assertEqual(color_paths(svg(), Image.new('RGB', (1, 1))), [])
        self.assertEqual(color_paths(svg('M0 0'), Image.new('RGB', (1, 1))), [])

    def test_many_segments_cross_batch_boundaries_without_reordering(self):
        rng = np.random.default_rng(108)
        image = rng.integers(0, 256, (64, 64, 3), dtype=np.uint8)
        root = svg(''.join(f'M{i%63}.5 {i%59}.5L{(i+5)%63}.5 {(i+3)%59}.5' if i % 2
                          else f'M{i%63}.5 {i%59}.5C30 10 20 40 {(i+1)%63}.5 {(i+2)%59}.5'
                          for i in range(4201)))
        result = color_paths(root, image)
        self.assertEqual(len(result), 4201)
        self.assertEqual(result, original_colors(root, image))

    def test_large_outside_bounds_clip_before_integer_conversion(self):
        root = svg('M-1e25 -1e25L1e25 1e25', 'M1e20 -1e20C-3e20 5e20 7e20 -9e20 1e20 2e20')
        image = np.arange(8*9*3, dtype=np.uint8).reshape(8, 9, 3)
        self.assertEqual(color_paths(root, image), original_colors(root, image))

    def test_invalid_source_shape_and_nonfinite_geometry_fail(self):
        for shape in [(3, 3), (3, 3, 2), (3, 3, 5)]:
            with self.assertRaisesRegex(ValueError, 'RGB or RGBA'):
                color_paths(svg(), np.zeros(shape, dtype=np.uint8))
        with self.assertRaisesRegex(ValueError, 'Non-finite'):
            color_paths(svg('M0 0L1e309 1'), Image.new('RGB', (2, 2)))


if __name__ == '__main__':
    unittest.main(verbosity=2)
