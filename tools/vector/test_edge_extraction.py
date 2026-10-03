"""Color-boundary quality and invariance without an external tracing runtime."""
import unittest
from unittest import mock

import numpy as np
from PIL import Image, ImageDraw

from edge_extraction import extract_color_edges, _workers, AUTO_EDGE_THREADS


def extract(values, threshold=12, alpha=None, **kwargs):
    image = values if isinstance(values, Image.Image) else Image.fromarray(values)
    alpha = Image.new('L', image.size, 255) if alpha is None else alpha
    return extract_color_edges(image, alpha, threshold, **kwargs)


class ColorEdgeTests(unittest.TestCase):
    def test_equal_luminance_colors_still_have_one_boundary(self):
        # Pillow's 8-bit grayscale is identical for both colors (76), so a
        # grayscale edge detector cannot recover this boundary.
        image = Image.new('RGB', (96, 72), (255, 0, 0))
        ImageDraw.Draw(image).rectangle((48, 0, 95, 71), fill=(0, 130, 0))
        self.assertEqual(len(set(image.convert('L').getdata())), 1)
        mask = extract(image)
        self.assertTrue(np.all(mask.sum(axis=1) == 1))
        self.assertTrue(np.all(np.abs(np.nonzero(mask)[1] - 48) <= 1))

    def test_filled_black_white_logo_boundary_not_just_narrow_ink(self):
        image = Image.new('RGB', (128, 128), 'white')
        ImageDraw.Draw(image).rectangle((28, 24, 100, 104), fill='black')
        mask = extract(image)
        self.assertEqual(int(mask[64].sum()), 2)
        self.assertEqual(int(mask[:, 64].sum()), 2)
        self.assertTrue(np.all(np.abs(np.nonzero(mask[64])[0] - [28, 101]) <= 1))
        self.assertFalse(mask[32:96, 36:92].any(), 'flat black interiors are not edges')
        self.assertFalse(mask[:16].any(), 'no artificial border against image padding')

    def test_antialiased_transition_has_one_rail(self):
        values = np.zeros((80, 96, 3), dtype=np.uint8)
        values[:, 50:] = 255
        for x, color in enumerate([0, 32, 96, 159, 223, 255], 45):
            values[:, x] = color
        mask = extract(values)
        self.assertTrue(np.all(mask.sum(axis=1) == 1))
        self.assertTrue(np.all((np.nonzero(mask)[1] >= 46) & (np.nonzero(mask)[1] <= 50)))

    def test_soft_diagonal_is_a_connected_thin_staircase(self):
        yy, xx = np.indices((128, 128))
        values = np.clip((xx - yy) * 85 + 128, 0, 255).astype(np.uint8)
        mask = extract(np.repeat(values[..., None], 3, axis=2))
        ys, xs = np.nonzero(mask[8:-8, 8:-8])
        self.assertGreater(len(ys), 100)
        self.assertLessEqual(len(ys), 224)
        self.assertLessEqual(int(np.max(np.abs(xs - ys))), 1)
        # Every interior pixel has an 8-connected neighbor on the following row.
        for y in range(8, 119):
            x = np.nonzero(mask[y])[0]
            self.assertTrue(any(mask[y + 1, max(0, p - 1):p + 2].any() for p in x))

    def test_low_contrast_texture_is_not_promoted(self):
        random = np.random.default_rng(8102)
        values = random.integers(124, 133, (128, 160, 3), dtype=np.uint8)
        self.assertFalse(extract(values).any())

    def test_visible_color_boundary_and_alpha_rim_ignore_invisible_rgb(self):
        values = np.zeros((128, 128, 3), dtype=np.uint8)
        values[:, 64:] = [250, 120, 20]
        alpha = np.full((128, 128), 255, dtype=np.uint8)
        alpha[70:] = 0
        # Arbitrary invisible RGB must not create contours or change either
        # the color contour or the separately detected transparency silhouette.
        alternate = values.copy()
        alternate[70:] = np.random.default_rng(55).integers(0, 256, (58, 128, 3), dtype=np.uint8)
        mask = extract(values, alpha=Image.fromarray(alpha))
        np.testing.assert_array_equal(mask, extract(alternate, alpha=Image.fromarray(alpha)))
        self.assertEqual(int(mask[:64].sum()), 64)
        self.assertFalse(mask[67:69].any())
        self.assertTrue(mask[69].all())
        self.assertFalse(mask[70:].any())

    def test_uniform_visible_patch_outlines_alpha_hole_on_visible_side(self):
        image = Image.new('RGB', (96, 96), '#a04030')
        alpha = Image.new('L', image.size, 255)
        ImageDraw.Draw(alpha).ellipse((24, 20, 72, 80), fill=0)
        image.paste('white', mask=Image.eval(alpha, lambda value: 255 - value))
        mask = extract(image, alpha=alpha)
        self.assertGreater(int(mask.sum()), 100)
        self.assertFalse(mask[np.asarray(alpha) == 0].any())
        self.assertFalse(mask[:18].any())
        self.assertFalse(mask[83:].any())
        self.assertFalse(extract(Image.new('RGB', (16, 16), 'black'),
                                 alpha=Image.new('L', (16, 16), 0)).any())

    def test_black_and_white_transparent_logos_have_identical_silhouettes(self):
        alpha = Image.new('L', (128, 128), 0)
        ImageDraw.Draw(alpha).rectangle((24, 28, 103, 99), fill=255)
        expected = np.zeros((128, 128), dtype=bool)
        expected[28:100, 24] = expected[28:100, 103] = True
        expected[28, 24:104] = expected[99, 24:104] = True
        for foreground in ('black', 'white'):
            image = Image.new('RGB', alpha.size, foreground)
            for background in ('black', 'white', '#04fd62'):
                variant = image.copy()
                variant.paste(background, mask=Image.eval(alpha, lambda value: 255 - value))
                with self.subTest(foreground=foreground, background=background):
                    np.testing.assert_array_equal(extract(variant, alpha=alpha), expected)

    def test_alpha_silhouette_does_not_add_artificial_image_frame(self):
        image = Image.new('RGB', (64, 48), 'white')
        self.assertFalse(extract(image).any())
        alpha = Image.new('L', image.size, 0)
        ImageDraw.Draw(alpha).rectangle((0, 0, 63, 24), fill=255)
        expected = np.zeros((48, 64), dtype=bool)
        expected[24] = True
        np.testing.assert_array_equal(extract(image, alpha=alpha), expected)

    def test_tile_size_and_worker_schedule_are_pixel_identical(self):
        random = np.random.default_rng(744)
        values = random.integers(0, 256, (171, 203, 3), dtype=np.uint8)
        # Strong boundaries and weak connecting segments cross tile seams, as
        # does an alpha hole. Tiles smaller than the halo test the full radius.
        values[30:140, 30:172] = [170, 60, 30]
        values[32:138, 31:170] = [155, 45, 15]
        values[55:120, 80:120] = [10, 120, 180]
        alpha = Image.new('L', (203, 171), 255)
        ImageDraw.Draw(alpha).rectangle((120, 25, 169, 54), fill=0)
        expected = extract(values, alpha=alpha, tile_size=512, workers=1)
        for tile_size, workers in ((9, 1), (17, 4), (32, 16), (64, 2)):
            with self.subTest(tile_size=tile_size, workers=workers):
                actual = extract(values, alpha=alpha, tile_size=tile_size, workers=workers)
                np.testing.assert_array_equal(actual, expected)

    def test_narrow_images_and_invalid_arguments(self):
        for size in ((1, 1), (1, 12), (12, 1), (2, 2)):
            self.assertFalse(extract(Image.new('RGB', size, '#345678')).any())
        image, alpha = Image.new('RGB', (8, 8)), Image.new('L', (8, 8))
        for threshold in (0, -1, float('nan'), float('inf')):
            with self.assertRaises(ValueError):
                extract_color_edges(image, alpha, threshold)
        for kwargs in ({'tile_size': 0}, {'workers': 0}, {'tile_size': 1.5}):
            with self.assertRaises(ValueError):
                extract_color_edges(image, alpha, 12, **kwargs)
        with self.assertRaises(ValueError):
            extract_color_edges(image.convert('L'), alpha, 12)
        with self.assertRaises(ValueError):
            extract_color_edges(image, Image.new('L', (4, 4)), 12)

    def test_cpu_affinity_and_small_jobs_bound_parallelism(self):
        with mock.patch('edge_extraction.os.sched_getaffinity', return_value={2, 3}):
            self.assertEqual(_workers(100), 2)
            self.assertEqual(_workers(1), 1)
        with mock.patch('edge_extraction.os.sched_getaffinity', side_effect=OSError), \
             mock.patch('edge_extraction.os.cpu_count', return_value=64):
            self.assertEqual(_workers(100), AUTO_EDGE_THREADS)


if __name__ == '__main__':
    unittest.main()
