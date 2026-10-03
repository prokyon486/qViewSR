"""Gap-closing invariants, including transparent and intentional small detail."""
import unittest

import numpy as np
from PIL import Image, ImageDraw

from mask_cleanup import close_dark_gaps


class MaskCleanupTests(unittest.TestCase):
    def alpha(self, mask):
        return Image.new('L', (mask.shape[1], mask.shape[0]), 255)

    def test_off_is_exact_nonmutating_bypass(self):
        mask = np.random.default_rng(7).random((61, 83)) > .6
        original = mask.copy()
        alpha = self.alpha(mask)
        result = close_dark_gaps(mask, alpha, 0)
        np.testing.assert_array_equal(result, original)
        np.testing.assert_array_equal(mask, original)
        self.assertFalse(np.shares_memory(result, mask))
        self.assertEqual(result.dtype, np.bool_)

    def test_tiny_holes_and_open_gaps_in_band_are_closed(self):
        mask = np.zeros((15, 45), dtype=bool)
        mask[5:10, 3:42] = True
        mask[7, 8:19] = False
        mask[5:8, 24:26] = False
        result = close_dark_gaps(mask, self.alpha(mask), 1)
        self.assertTrue(result[5:10, 3:42].all())
        self.assertFalse(result[:4].any())
        self.assertFalse(result[11:].any())

    def test_close_lines_merge_only_within_selected_radius(self):
        # This intentional loss of close parallel strokes is why the UI setting
        # defaults OFF and describes its effect on fine text and double lines.
        for gap in (1, 2, 3, 4, 5):
            mask = np.zeros((25, 25), dtype=bool)
            mask[3:22, 6] = True
            mask[3:22, 7 + gap] = True
            for radius in (1, 2):
                with self.subTest(gap=gap, radius=radius):
                    result = close_dark_gaps(mask, self.alpha(mask), radius)
                    filled = bool(result[10, 7:7 + gap].all())
                    self.assertEqual(filled, gap <= radius * 2)

    def test_small_counter_and_large_round_outline(self):
        small = np.zeros((15, 15), dtype=bool)
        small[3:12, 3:12] = True
        small[6:9, 6:9] = False
        # A 3px counter survives 3x3 closing, but is deliberately lost at 5x5.
        self.assertFalse(close_dark_gaps(small, self.alpha(small), 1)[7, 7])
        self.assertTrue(close_dark_gaps(small, self.alpha(small), 2)[7, 7])
        ring = Image.new('L', (55, 55), 0)
        ImageDraw.Draw(ring).ellipse((8, 8, 46, 46), fill=255)
        ImageDraw.Draw(ring).ellipse((10, 10, 44, 44), fill=0)
        mask = np.asarray(ring) > 0
        for radius in (1, 2):
            result = close_dark_gaps(mask, self.alpha(mask), radius)
            self.assertFalse(result[27, 27])
            self.assertTrue(result[mask].all())
            self.assertLess(np.count_nonzero(result & ~mask), 40)

    def test_transparent_gap_never_becomes_a_visible_bridge(self):
        mask = np.zeros((21, 31), dtype=bool)
        mask[7:14, 3:28] = True
        opacity = np.full(mask.shape, 255, dtype=np.uint8)
        opacity[:, 14:16] = 0
        clean = mask & (opacity > 0)
        alpha = Image.fromarray(opacity)
        alpha_before = alpha.tobytes()
        original = mask.copy()
        for radius in (1, 2):
            hidden_black = close_dark_gaps(mask, alpha, radius)
            hidden_white = close_dark_gaps(clean, alpha, radius)
            np.testing.assert_array_equal(hidden_black, hidden_white)
            self.assertFalse(hidden_black[:, 14:16].any())
            self.assertTrue(hidden_black[clean].all())
        np.testing.assert_array_equal(mask, original)
        self.assertEqual(alpha.tobytes(), alpha_before)

    def test_hidden_foreground_cannot_create_a_visible_line(self):
        opacity = np.full((25, 25), 255, dtype=np.uint8)
        opacity[:, 10:13] = 0
        mask = opacity == 0
        for radius in (1, 2):
            self.assertFalse(close_dark_gaps(mask, Image.fromarray(opacity), radius).any())

    def test_visible_edge_pixels_and_degenerate_image_sizes_are_retained(self):
        for shape in ((1, 1), (1, 7), (7, 1), (2, 2), (13, 17)):
            mask = np.zeros(shape, dtype=bool)
            mask[0, :] = True
            mask[:, 0] = True
            for radius in (1, 2):
                with self.subTest(shape=shape, radius=radius):
                    result = close_dark_gaps(mask, self.alpha(mask), radius)
                    self.assertTrue(result[mask].all())
                    self.assertEqual(result.shape, shape)

    def test_invalid_radius_and_shapes_fail_before_filtering(self):
        mask = np.zeros((5, 7), dtype=bool)
        for radius in (-1, 3, .5, 1., True, None, '1'):
            with self.subTest(radius=radius), self.assertRaises(ValueError):
                close_dark_gaps(mask, self.alpha(mask), radius)
        with self.assertRaises(ValueError):
            close_dark_gaps(mask, Image.new('L', (7, 6)), 1)
        with self.assertRaises(ValueError):
            close_dark_gaps(mask.astype(np.uint8), self.alpha(mask), 1)


if __name__ == '__main__':
    unittest.main()
