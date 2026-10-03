"""Run with the isolated vector Python: python tools/vector/test_vector_worker.py."""
from contextlib import redirect_stdout, redirect_stderr
import io
import json
import hashlib
import importlib.metadata
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw

from color_paths import color_paths, parse_segments
from vector_worker import (NS, validate_svg, check_resources, load_source,
                           snapshot_source, suppress_lines, suppression_workers,
                           check_svg_resources, read_svg, save_svg, svg_root,
                           extract, STRENGTHS, main)

HERE = Path(__file__).resolve().parent
WORKER = HERE / 'vector_worker.py'


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='qviewsr-vector-test-')
        self.root = Path(self.temporary.name)
        self.input = self.root / 'source.png'
        self.cache = self.root / 'cache'
        image = Image.new('RGB', (120, 100), '#e8ccaa')
        draw = ImageDraw.Draw(image)
        draw.ellipse((10, 10, 90, 70), fill='#e890a0', outline='#403030', width=2)
        draw.line([(20, 75), (40, 50), (110, 90)], fill='#205080', width=3)
        image.save(self.input)

    def tearDown(self):
        self.temporary.cleanup()

    def command(self, *extra):
        return [sys.executable, str(WORKER), '--input', str(self.input),
                '--cache', str(self.cache), '--strength', 'balanced',
                '--detail', 'balanced', '--suppression', '35', *extra]

    def worker(self, *extra):
        cp = subprocess.run(self.command(*extra), capture_output=True, text=True, timeout=120)
        self.assertEqual(cp.returncode, 0, cp.stderr)
        result = json.loads(cp.stdout.splitlines()[-1])
        for field in ['fill_svg', 'lines_svg']:
            self.assertTrue(Path(result[field]).is_absolute())
            self.assertTrue(validate_svg(Path(result[field]), result['width'], result['height']))
        return result

    def test_independent_caches_and_source_identity(self):
        first = self.worker()
        self.assertEqual(first['cache_hits'], [])
        repeated = self.worker()
        self.assertEqual(repeated['cache_hits'], ['fill', 'lines'])
        strong = self.worker('--strength', 'strong')
        self.assertEqual(strong['fill_svg'], first['fill_svg'])
        self.assertNotEqual(strong['lines_svg'], first['lines_svg'])
        self.assertEqual(strong['cache_hits'], ['fill'])
        fine = self.worker('--detail', 'fine')
        self.assertEqual(fine['fill_svg'], first['fill_svg'])
        self.assertNotEqual(fine['lines_svg'], first['lines_svg'])
        suppressed = self.worker('--suppression', '80')
        self.assertNotEqual(suppressed['fill_svg'], first['fill_svg'])
        self.assertEqual(suppressed['lines_svg'], first['lines_svg'])
        self.assertEqual(suppressed['cache_hits'], ['suppression', 'lines'])
        image = Image.open(self.input).copy()
        image.putpixel((1, 1), (0, 0, 0)); image.save(self.input)
        changed = self.worker()
        self.assertNotEqual(Path(changed['fill_svg']).parent, Path(first['fill_svg']).parent)
        self.assertEqual(changed['cache_hits'], [])

    def test_unpatched_vectorizer_is_rejected_before_pixel_decode(self):
        import vtracer
        self.worker()
        version = importlib.metadata.version
        guards = [
            mock.patch('vector_worker.importlib.metadata.version',
                       side_effect=lambda name: '0.6.15' if name == 'vtracer' else version(name)),
            mock.patch.object(vtracer, '__qviewsr_patch__', None),
        ]
        for guard in guards:
            with guard, mock.patch.object(sys, 'argv', self.command()[1:]), \
                 mock.patch('vector_worker.load_source', side_effect=AssertionError('pixels decoded')):
                with self.assertRaisesRegex(ValueError, 'setup_vector.py'):
                    main()

    def test_complete_cache_skips_pixel_decode_and_keeps_memory_preflight(self):
        original = self.worker()
        corrected = self.worker('--correct-lines')
        for extra, expected in [((), original), (('--correct-lines',), corrected)]:
            with self.subTest(correction=bool(extra)):
                output, errors = io.StringIO(), io.StringIO()
                with mock.patch.object(sys, 'argv', self.command(*extra)[1:]), \
                     mock.patch('vector_worker.load_source', side_effect=AssertionError('source pixels decoded')), \
                     mock.patch('PIL.ImageFile.ImageFile.load', side_effect=AssertionError('image pixels decoded')), \
                     mock.patch('vector_worker.check_resources', wraps=check_resources) as preflight, \
                     redirect_stdout(output), redirect_stderr(errors):
                    main()
                result = json.loads(output.getvalue().splitlines()[-1])
                self.assertEqual(result, dict(expected, cache_hits=['fill', 'lines']))
                preflight.assert_called_once_with(120, 100)
                self.assertNotIn('入力画像を展開', errors.getvalue())
        # Cached geometry still follows the documented memory preflight policy.
        with mock.patch.object(sys, 'argv', self.command()[1:]), \
             mock.patch('vector_worker.available_memory', return_value=1024 * 1024):
            with self.assertRaisesRegex(MemoryError, '利用可能メモリ'):
                main()

    def test_incomplete_cache_decodes_and_regenerates_only_missing_layer(self):
        initial = self.worker('--correct-lines')
        for field, expected_hits in [('fill_svg', ['suppression', 'lines']),
                                     ('lines_svg', ['fill', 'raw_lines'])]:
            with self.subTest(layer=field):
                asset = Path(initial[field])
                if field == 'fill_svg':
                    asset.unlink()
                else:
                    asset.write_text('incomplete <svg')
                output = io.StringIO()
                with mock.patch.object(sys, 'argv', self.command('--correct-lines')[1:]), \
                     mock.patch('vector_worker.load_source', wraps=load_source) as decoder, \
                     redirect_stdout(output), redirect_stderr(io.StringIO()):
                    main()
                decoder.assert_called_once()
                result = json.loads(output.getvalue().splitlines()[-1])
                self.assertEqual(result, dict(initial, cache_hits=expected_hits))
                self.assertTrue(validate_svg(asset, 120, 100))

    def test_corrupt_cache_regenerates_svg(self):
        first = self.worker()
        Path(first['lines_svg']).write_text('incomplete <svg')
        result = self.worker()
        self.assertEqual(result['cache_hits'], ['fill', 'raw_lines'])

    def test_correction_switch_reuses_uncorrected_geometry(self):
        first = self.worker()
        original = Path(first['lines_svg']).read_bytes()
        corrected = self.worker('--correct-lines')
        self.assertEqual(corrected['fill_svg'], first['fill_svg'])
        self.assertNotEqual(corrected['lines_svg'], first['lines_svg'])
        self.assertEqual(corrected['cache_hits'], ['fill', 'raw_lines'])
        self.assertTrue(corrected['correct_lines'])
        self.assertIn('paths', corrected['correction_stats'])
        self.assertEqual(self.worker('--correct-lines')['cache_hits'], ['fill', 'lines'])
        restored = self.worker()
        self.assertFalse(restored['correct_lines'])
        self.assertEqual(restored['correction_stats'], {})
        self.assertEqual(restored['lines_svg'], first['lines_svg'])
        self.assertEqual(Path(restored['lines_svg']).read_bytes(), original)

    def test_binary_transparency_and_empty_source(self):
        image = Image.open(self.input).convert('RGBA')
        alpha = Image.new('L', image.size)
        ImageDraw.Draw(alpha).rectangle((5, 5, 114, 94), fill=255)
        image.putalpha(alpha); image.save(self.input)
        result = self.worker('--suppression', '0')
        self.assertGreater(len(list(ET.parse(result['fill_svg']).getroot())), 0)
        # Fully invisible RGB cannot produce a fill or a line.
        image.putalpha(0); image.save(self.input)
        blank = self.worker()
        self.assertEqual(len(list(ET.parse(blank['fill_svg']).getroot())), 0)
        self.assertEqual(len(list(ET.parse(blank['lines_svg']).getroot())), 0)

    def test_cleanup_shape_and_color_modes_have_independent_caches(self):
        source_digest = hashlib.sha256(self.input.read_bytes()).hexdigest()
        original = self.worker()
        original_bytes = Path(original['lines_svg']).read_bytes()
        options = ('--clean-lines', '--min-line-length', '8', '--join-distance', '3', '--branch-strength', '60')
        cleaned = self.worker(*options)
        self.assertEqual(cleaned['cache_hits'], ['fill', 'raw_lines'])
        self.assertTrue(cleaned['clean_lines'])
        self.assertIn('isolated_removed', cleaned['cleanup_stats'])
        fitted = self.worker(*options, '--correct-lines', '--shape-tolerance', '3')
        self.assertEqual(fitted['cache_hits'], ['fill', 'clean_lines'])
        relaxed = self.worker(*options, '--correct-lines', '--shape-tolerance', '4')
        self.assertEqual(relaxed['cache_hits'], ['fill', 'clean_lines'])
        self.assertNotEqual(fitted['lines_svg'], relaxed['lines_svg'])
        repeated = self.worker(*options, '--correct-lines', '--shape-tolerance', '4')
        self.assertEqual(repeated['cache_hits'], ['fill', 'lines'])
        resized = self.worker(*options, '--min-line-length', '10')
        self.assertEqual(resized['cache_hits'], ['fill', 'raw_lines'])
        color = self.worker('--line-mode', 'color', *options)
        self.assertEqual(color['cache_hits'], ['fill'])
        self.assertEqual(color['line_mode'], 'color')
        self.assertNotEqual(color['lines_svg'], cleaned['lines_svg'])
        self.assertGreater(len(list(ET.parse(color['lines_svg']).getroot().iter(f'{{{NS}}}path'))), 0)
        gap = self.worker('--mask-gap', '1', *options)
        self.assertEqual(gap['cache_hits'], ['fill'])
        self.assertEqual(gap['mask_gap'], 1)
        self.assertNotEqual(gap['lines_svg'], cleaned['lines_svg'])
        self.assertEqual(self.worker('--mask-gap', '1', *options)['cache_hits'], ['fill', 'lines'])
        color_gap = self.worker('--line-mode', 'color', '--mask-gap', '1', *options)
        self.assertEqual(color_gap['cache_hits'], ['fill', 'lines'])
        self.assertEqual(color_gap['mask_gap'], 0)
        self.assertEqual(color_gap['lines_svg'], color['lines_svg'])
        restored = self.worker()
        self.assertEqual(restored['lines_svg'], original['lines_svg'])
        self.assertEqual(Path(restored['lines_svg']).read_bytes(), original_bytes)
        self.assertEqual(restored['cleanup_stats'], {})
        self.assertEqual(hashlib.sha256(self.input.read_bytes()).hexdigest(),
                         source_digest)

    def test_invalid_cleanup_parameters_fail_before_processing(self):
        for option, value in (('--min-line-length', '-1'), ('--join-distance', 'inf'),
                              ('--branch-strength', '101'), ('--shape-tolerance', 'nan')):
            with self.subTest(option=option):
                cp = subprocess.run(self.command(option, value), capture_output=True, text=True, timeout=10)
                self.assertNotEqual(cp.returncode, 0)
                self.assertIn('範囲外', cp.stderr)

    def test_unsupported_alpha_fails_cleanly(self):
        image = Image.open(self.input).convert('RGBA')
        image.putpixel((0, 0), (0, 0, 0, 127)); image.save(self.input)
        cp = subprocess.run(self.command(), capture_output=True, text=True)
        self.assertNotEqual(cp.returncode, 0)
        self.assertIn('半透明', cp.stderr)
        self.assertEqual(cp.stdout, '')

    def test_full_resolution_above_old_pixel_and_side_limits(self):
        for size in [(10001, 128), (4001, 1024)]:
            with self.subTest(size=size):
                image = Image.new('RGB', size, '#e8ccaa')
                ImageDraw.Draw(image).line((10, 25, size[0]-10, 25), fill='#203040', width=3)
                image.save(self.input)
                result = self.worker('--suppression', '0')
                self.assertEqual((result['width'], result['height']), size)
                paths = ET.parse(result['lines_svg']).getroot().iter(f'{{{NS}}}path')
                segments = [segment for path in paths for segment in parse_segments(path.get('d'))]
                self.assertTrue(segments)
                self.assertGreater(max(float(segment['points'][:, 0].max()) for segment in segments), size[0]-15)

    def test_memory_preflight_and_pillow_limit_restore(self):
        with mock.patch('vector_worker.available_memory', return_value=1024 * 1024):
            with self.assertRaisesRegex(MemoryError, '利用可能メモリ'):
                load_source(self.input)
        with mock.patch('vector_worker.available_memory', return_value=64 * 1024**3):
            self.assertGreater(check_resources(10001, 10001), 10001 * 10001)
        # Pillow's fixed bomb threshold is replaced by the memory preflight,
        # including while decoding, then restored for other callers.
        previous = Image.MAX_IMAGE_PIXELS
        try:
            Image.MAX_IMAGE_PIXELS = 1
            rgb, alpha = load_source(self.input)
            self.assertEqual(rgb.size, (120, 100))
            self.assertEqual(alpha.size, rgb.size)
            self.assertEqual(Image.MAX_IMAGE_PIXELS, 1)
        finally:
            Image.MAX_IMAGE_PIXELS = previous

    def test_snapshot_hashes_exact_bytes_and_survives_source_replacement(self):
        snapshot = self.root / 'snapshot'
        original = self.input.read_bytes()
        digest = snapshot_source(self.input, snapshot)
        self.assertEqual(digest, hashlib.sha256(original).hexdigest())
        self.assertEqual(snapshot.read_bytes(), original)
        self.input.write_bytes(b'changed source')
        self.assertEqual(load_source(snapshot)[0].size, (120, 100))

    def test_svg_above_previous_size_limit_parses_and_saves_atomically(self):
        source = self.root / 'large.svg'
        with source.open('wb') as output:
            output.write(f'<svg xmlns="{NS}" viewBox="0 0 120 100">'.encode())
            padding = b' ' * (1024 * 1024)
            for _ in range(65):
                output.write(padding)
            output.write(b'<path d="M0 0L120 100"/></svg>')
        self.assertGreater(source.stat().st_size, 64 * 1024**2)
        with mock.patch('vector_worker.available_memory', return_value=8 * 1024**3):
            self.assertTrue(validate_svg(source, 120, 100))
            root = read_svg(source)
            destination = self.root / 'saved.svg'
            save_svg(root, destination, 120, 100)
            self.assertGreater(destination.stat().st_size, 64 * 1024**2)
            self.assertTrue(validate_svg(destination, 120, 100))
        self.assertEqual(root[0].get('d'), 'M0 0L120 100')

    def test_svg_resource_failure_propagates_without_parse_or_cache_rebuild(self):
        cached = self.worker()
        path = Path(cached['fill_svg'])
        with mock.patch('vector_worker.available_memory', return_value=1024 * 1024), \
             mock.patch('vector_worker.ET.parse', side_effect=AssertionError('parsed without sufficient memory')):
            with self.assertRaisesRegex(MemoryError, 'SVG.*70%'):
                validate_svg(path, 120, 100)
            with mock.patch.object(sys, 'argv', self.command()[1:]), \
                 mock.patch('vector_worker.check_resources'), \
                 mock.patch('vector_worker.load_source', side_effect=AssertionError('cache was regenerated')):
                with self.assertRaisesRegex(MemoryError, 'SVG.*70%'):
                    main()
        original = path.read_bytes()
        siblings = set(path.parent.iterdir())
        with mock.patch('vector_worker.available_memory', return_value=1024 * 1024):
            with self.assertRaisesRegex(MemoryError, 'SVG.*70%'):
                save_svg(svg_root(120, 100), path, 120, 100)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(set(path.parent.iterdir()), siblings)

    def test_svg_resource_known_unknown_and_serialized_limits(self):
        mib = 1024**2
        with mock.patch('vector_worker.available_memory', return_value=None):
            self.assertEqual(check_svg_resources(64 * mib), 32 * 64 * mib + 64 * mib)
            with self.assertRaisesRegex(MemoryError, '利用可能メモリを取得できない'):
                check_svg_resources(64 * mib + 1)
        with mock.patch('vector_worker.available_memory', return_value=64 * 1024**3):
            self.assertEqual(check_svg_resources(1024 * mib), 32 * 1024 * mib + 64 * mib)
            with self.assertRaisesRegex(MemoryError, '上限1GiB'):
                check_svg_resources(1024 * mib + 1)
        # Exact 70% boundary is allowed; one serialized byte more is refused.
        with mock.patch('vector_worker.available_memory', return_value=160 * mib):
            self.assertEqual(check_svg_resources(3 * mib // 2), 112 * mib)
            with self.assertRaisesRegex(MemoryError, '70%'):
                check_svg_resources(3 * mib // 2 + 1)

    def test_file_above_previous_128mib_limit(self):
        # A valid PNG followed by padding exercises compressed-file streaming
        # without requiring a costly 45-million-pixel uncompressed fixture.
        with self.input.open('r+b') as padded:
            padded.truncate(129 * 1024 * 1024)
        result = self.worker('--suppression', '0')
        self.assertEqual((result['width'], result['height']), (120, 100))

    def test_inpainting_matches_original_simultaneous_updates(self):
        from PIL import ImageFilter
        from concurrent.futures import ThreadPoolExecutor
        rgb = Image.open(self.input).convert('RGB')
        # Exercise clamped image edges and a transparent boundary, as well as
        # adjacent masked pixels that can race without both iteration barriers.
        ImageDraw.Draw(rgb).rectangle((0, 0, rgb.width-1, rgb.height-1), outline='#102030', width=2)
        for transparent in (False, True):
            alpha = Image.new('L', rgb.size, 255)
            if transparent:
                ImageDraw.Draw(alpha).rectangle((0, 0, 45, 55), fill=0)
            mask = extract(rgb, alpha, STRENGTHS['strong'])
            dilated = np.asarray(Image.fromarray((mask * 255).astype('uint8')).filter(ImageFilter.MaxFilter(3))) > 0
            dilated &= np.asarray(alpha) > 0
            values = np.asarray(rgb, dtype=np.float32).copy()
            values[dilated] = np.asarray(rgb.filter(ImageFilter.GaussianBlur(3)), dtype=np.float32)[dilated]
            ys, xs = np.nonzero(dilated)
            ym, yp = np.maximum(ys-1, 0), np.minimum(ys+1, rgb.height-1)
            xm, xp = np.maximum(xs-1, 0), np.minimum(xs+1, rgb.width-1)
            # Original unchunked Jacobi expression is the independent reference.
            for _ in range(160):
                values[ys, xs] = (values[ym, xs] + values[yp, xs] + values[ys, xm] + values[ys, xp]) * .25
            expected = np.uint8(np.clip(np.rint(values), 0, 255))
            with self.subTest(transparent=transparent, workers=1), \
                 mock.patch('vector_worker.ThreadPoolExecutor', side_effect=AssertionError('small job started pool')):
                np.testing.assert_array_equal(np.asarray(suppress_lines(rgb, alpha)), expected)
            # Use small chunks to cross many boundaries and exercise a real
            # thread pool without making the regression fixture need megabytes.
            for cpus in (1, 4, 16):
                with self.subTest(transparent=transparent, cpus=cpus), \
                     mock.patch('vector_worker.SUPPRESSION_CHUNK_PIXELS', 97), \
                     mock.patch('vector_worker.os.sched_getaffinity', return_value=set(range(cpus)), create=True), \
                     mock.patch('vector_worker.ThreadPoolExecutor', wraps=ThreadPoolExecutor) as pool:
                    np.testing.assert_array_equal(np.asarray(suppress_lines(rgb, alpha)), expected)
                    if cpus == 1:
                        pool.assert_not_called()
                    else:
                        pool.assert_called_once_with(max_workers=cpus)

    def test_inpainting_respects_cpu_and_chunk_limits(self):
        for cpus, chunks, expected in [(32, 100, 16), (4, 100, 4), (16, 3, 3), (1, 100, 1)]:
            with self.subTest(cpus=cpus, chunks=chunks), \
                 mock.patch('vector_worker.os.sched_getaffinity', return_value=set(range(cpus)), create=True):
                self.assertEqual(suppression_workers(chunks), expected)
        for error in (AttributeError, OSError):
            for cpus, expected in [(4, 4), (None, 1)]:
                with self.subTest(error=error, cpus=cpus), \
                     mock.patch('vector_worker.os.sched_getaffinity', side_effect=error, create=True), \
                     mock.patch('vector_worker.os.cpu_count', return_value=cpus):
                    self.assertEqual(suppression_workers(100), expected)

    def test_inpainting_empty_mask_does_not_start_pool(self):
        rgb = Image.open(self.input).convert('RGB')
        alpha = Image.new('L', rgb.size, 0)
        with mock.patch('vector_worker.ThreadPoolExecutor', side_effect=AssertionError('empty job started pool')):
            np.testing.assert_array_equal(np.asarray(suppress_lines(rgb, alpha)), np.asarray(rgb))

    def test_no_raster_and_reject_unsafe_cache(self):
        result = self.worker()
        for field in ['fill_svg', 'lines_svg']:
            path = Path(result[field]); root = ET.parse(path).getroot()
            self.assertFalse(any(e.tag.endswith('image') for e in root.iter()))
        malicious = self.root / 'bad.svg'
        malicious.write_text(f'<svg xmlns="{NS}" viewBox="0 0 120 100"><image href="remote"/></svg>')
        self.assertFalse(validate_svg(malicious, 120, 100))

    @unittest.skipUnless(sys.platform == 'linux', 'Linux parent-death signal')
    def test_autotrace_dies_when_worker_is_killed(self):
        pidfile = self.root / 'trace.pid'
        tracer = self.root / 'fake-autotrace'
        tracer.write_text('#!' + sys.executable + '\nimport os,time\nfrom pathlib import Path\n'
                          + 'Path(' + repr(str(pidfile)) + ').write_text(str(os.getpid()))\ntime.sleep(120)\n')
        tracer.chmod(0o755)
        env = dict(os.environ, QVIEWSR_AUTOTRACE=str(tracer))
        child, alive = None, False
        with subprocess.Popen(self.command('--suppression', '0'), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            try:
                deadline = time.monotonic() + 15
                while not pidfile.exists() and time.monotonic() < deadline:
                    if process.poll() is not None:
                        self.fail('worker stopped before trace launcher: ' + process.stderr.read().decode())
                    time.sleep(.02)
                self.assertTrue(pidfile.exists(), 'AutoTrace did not start')
                child = int(pidfile.read_text())
                process.kill(); process.wait(timeout=5)
                deadline = time.monotonic() + 3
                alive = True
                while time.monotonic() < deadline:
                    status = Path(f'/proc/{child}/stat')
                    try:
                        alive = status.read_text().split(') ', 1)[1].split()[0] not in ('Z', 'X')
                    except FileNotFoundError:
                        alive = False
                    if not alive:
                        break
                    time.sleep(.02)
                self.assertFalse(alive, 'AutoTrace survived worker kill')
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()
                if child and alive:
                    try:
                        os.kill(child, signal.SIGKILL)
                    except ProcessLookupError:
                        pass


class GeometryTests(unittest.TestCase):
    def test_segment_colors_retain_bezier_coordinates(self):
        root = ET.fromstring(f'<svg xmlns="{NS}"><path d="M1 2C3 4 5 6 7 8L9 8"/></svg>')
        source = Image.new('RGBA', (12, 12), '#924050')
        colored = color_paths(root, source)
        original = list(parse_segments(root[0].get('d')))
        self.assertEqual(len(colored), 2)
        for before, after in zip(original, colored):
            np.testing.assert_array_equal(before['points'], next(parse_segments(after['d']))['points'])
            self.assertEqual(after['color'], '#924050')
        source.putalpha(0)
        self.assertEqual(color_paths(root, source, exclude_transparent=True), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
