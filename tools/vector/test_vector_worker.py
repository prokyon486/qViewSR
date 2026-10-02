"""Run with the isolated vector Python: python tools/vector/test_vector_worker.py."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw

from color_paths import color_paths, parse_segments
from vector_worker import NS, validate_svg

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
        cp = subprocess.run(self.command(*extra), capture_output=True, text=True, timeout=60)
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

    def test_corrupt_cache_regenerates_svg(self):
        first = self.worker()
        Path(first['lines_svg']).write_text('incomplete <svg')
        result = self.worker()
        self.assertEqual(result['cache_hits'], ['fill'])

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

    def test_unsupported_alpha_and_size_fail_cleanly(self):
        image = Image.open(self.input).convert('RGBA')
        image.putpixel((0, 0), (0, 0, 0, 127)); image.save(self.input)
        cp = subprocess.run(self.command(), capture_output=True, text=True)
        self.assertNotEqual(cp.returncode, 0)
        self.assertIn('半透明', cp.stderr)
        self.assertEqual(cp.stdout, '')
        for size in [(4097, 1), (2001, 2000)]:
            Image.new('RGB', size).save(self.input)
            cp = subprocess.run(self.command(), capture_output=True, text=True)
            self.assertNotEqual(cp.returncode, 0)
            self.assertIn('400万画素', cp.stderr)

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
