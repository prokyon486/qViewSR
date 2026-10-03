"""CPU-only two-layer vectorization worker. Last stdout line is its JSON result."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager, nullcontext
import hashlib
import importlib.metadata
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ALGORITHM = 'qviewsr-vector-1'
MAX_SVG_BYTES = 1024 * 1024 * 1024
UNKNOWN_MEMORY_MAX_SVG_BYTES = 64 * 1024 * 1024
SVG_WORKING_BYTES_PER_BYTE = 32
# Includes the patched vectorizer's 64-bit color sums (two sums per cluster).
WORKING_BYTES_PER_PIXEL = 190
MEMORY_HEADROOM = .70
SUPPRESSION_CHUNK_PIXELS = 262144
MAX_SUPPRESSION_THREADS = 16
NS = 'http://www.w3.org/2000/svg'
STRENGTHS = {'weak': 20, 'balanced': 12, 'strong': 6}
DETAILS = {'fine': .3, 'balanced': 1., 'simple': 3.}
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
ET.register_namespace('', NS)


def key(data):
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()[:24]


def available_memory():
    """Physical/cgroup headroom, not a fixed image dimension restriction."""
    limits = []
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            if line.startswith('MemAvailable:'):
                limits.append(int(line.split()[1]) * 1024)
                break
    except (OSError, ValueError):
        pass
    if not limits:
        try:
            pages, size = os.sysconf('SC_AVPHYS_PAGES'), os.sysconf('SC_PAGE_SIZE')
            if pages > 0 and size > 0:
                limits.append(pages * size)
        except (OSError, ValueError):
            pass
    # A desktop process or container may have a smaller cgroup allocation than
    # the host. Check ancestors too: a leaf's own memory.max can be "max".
    try:
        relative = next(line.split(':', 2)[2] for line in Path('/proc/self/cgroup').read_text().splitlines()
                        if line.startswith('0::'))
        base = Path('/sys/fs/cgroup')
        current = base / relative.lstrip('/')
        if '..' not in Path(relative).parts:
            while current == base or base in current.parents:
                try:
                    maximum = (current / 'memory.max').read_text().strip()
                    if maximum != 'max':
                        used = int((current / 'memory.current').read_text())
                        limits.append(max(0, int(maximum) - used))
                except (OSError, ValueError):
                    pass
                if current == base:
                    break
                current = current.parent
    except (OSError, ValueError, StopIteration):
        pass
    # Older distributions still expose the memory controller as cgroup v1.
    try:
        relative = next(line.split(':', 2)[2] for line in Path('/proc/self/cgroup').read_text().splitlines()
                        if 'memory' in line.split(':', 2)[1].split(','))
        base = Path('/sys/fs/cgroup/memory')
        current = base / relative.lstrip('/')
        if '..' not in Path(relative).parts:
            while current == base or base in current.parents:
                try:
                    maximum = int((current / 'memory.limit_in_bytes').read_text())
                    used = int((current / 'memory.usage_in_bytes').read_text())
                    limits.append(max(0, maximum - used))
                except (OSError, ValueError):
                    pass
                if current == base:
                    break
                current = current.parent
    except (OSError, ValueError, StopIteration, IndexError):
        pass
    return min(limits) if limits else None


def check_resources(width, height):
    if width < 1 or height < 1:
        raise ValueError('入力画像の大きさが不正です。')
    # Includes masks, floating-point inpainting, tracing, and decoded copies.
    # It is a preflight estimate; actual allocation failures are also reported.
    estimated = width * height * WORKING_BYTES_PER_PIXEL + 64 * 1024 * 1024
    available = available_memory()
    if available is not None and estimated > available * MEMORY_HEADROOM:
        raise MemoryError(f'この画像（{width}×{height}px）の処理用メモリの見積もりは約'
                          f'{estimated / 1024**3:.1f}GiBです。利用可能メモリ'
                          f'（約{available / 1024**3:.1f}GiB）が不足しています。'
                          'ほかのアプリを閉じてから再試行してください。')
    return estimated


def snapshot_source(source, destination):
    """Hash an immutable disk snapshot without keeping compressed bytes in RAM."""
    digest = hashlib.sha256()
    with source.open('rb') as incoming, destination.open('wb') as outgoing:
        before = os.fstat(incoming.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError('通常の画像ファイルを指定してください。')
        for block in iter(lambda: incoming.read(1024 * 1024), b''):
            digest.update(block)
            outgoing.write(block)
        after = os.fstat(incoming.fileno())
        fields = ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')
        if any(getattr(before, field) != getattr(after, field) for field in fields):
            raise ValueError('読み込み中に入力画像が変更されました。もう一度実行してください。')
    return digest.hexdigest()


@contextmanager
def open_image_checked(path):
    from PIL import Image
    previous = Image.MAX_IMAGE_PIXELS
    try:
        # Read only the header with Pillow's fixed pixel threshold disabled.
        # Our resource check runs before pixel decoding/allocation instead.
        Image.MAX_IMAGE_PIXELS = None
        with Image.open(path) as decoded:
            check_resources(*decoded.size)
            yield decoded
    finally:
        Image.MAX_IMAGE_PIXELS = previous


def svg_root(width, height):
    return ET.Element(f'{{{NS}}}svg', {'width': str(width), 'height': str(height),
                                      'viewBox': f'0 0 {width} {height}'})


def check_svg_resources(serialized_bytes):
    """Bound XML/geometry expansion before parsing a serialized SVG layer."""
    if serialized_bytes < 0:
        raise ValueError('SVGファイルの大きさが不正です。')
    if serialized_bytes > MAX_SVG_BYTES:
        raise MemoryError(f'SVGファイル（約{serialized_bytes / 1024**2:.1f}MiB）が'
                          '読み込み可能な上限1GiBを超えています。')
    estimated = serialized_bytes * SVG_WORKING_BYTES_PER_BYTE + 64 * 1024 * 1024
    available = available_memory()
    if available is None:
        if serialized_bytes > UNKNOWN_MEMORY_MAX_SVG_BYTES:
            raise MemoryError('利用可能メモリを取得できないため、64MiBを超えるSVGは読み込めません。')
    elif estimated > available * MEMORY_HEADROOM:
        raise MemoryError(f'SVG（約{serialized_bytes / 1024**2:.1f}MiB）の処理用メモリの見積もりは約'
                          f'{estimated / 1024**3:.1f}GiBです。利用可能メモリ'
                          f'（約{available / 1024**3:.1f}GiB）の70%を超えています。'
                          'ほかのアプリを閉じてから再試行してください。')
    return estimated


def read_svg(path):
    check_svg_resources(path.stat().st_size)
    return ET.parse(path).getroot()


def validate_svg(path, width, height):
    try:
        if not path.is_file():
            return False
        root = read_svg(path)
        if root.tag != f'{{{NS}}}svg' or root.get('viewBox') != f'0 0 {width} {height}':
            return False
        allowed = {'svg', 'g', 'path'}
        for element in root.iter():
            if element.tag.rsplit('}', 1)[-1] not in allowed:
                return False
            if any('href' in attribute or 'url(' in value or 'data:' in value
                   for attribute, value in element.attrib.items()):
                return False
        return True
    except (OSError, ET.ParseError):
        return False


def save_svg(root, destination, width, height):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, suffix='.svg', delete=False) as file:
        temp = Path(file.name)
    try:
        ET.ElementTree(root).write(temp, encoding='utf-8', xml_declaration=True)
        if not validate_svg(temp, width, height):
            raise ValueError('生成されたSVGを検証できませんでした。')
        os.replace(temp, destination)
    finally:
        temp.unlink(missing_ok=True)


def save_image(image, destination):
    with tempfile.NamedTemporaryFile(dir=destination.parent, suffix='.png', delete=False) as file:
        temp = Path(file.name)
    try:
        image.save(temp, format='PNG')
        os.replace(temp, destination)
    finally:
        temp.unlink(missing_ok=True)


def load_source(path):
    from PIL import Image, ImageCms, ImageChops
    with open_image_checked(path) as decoded:
        rgba = decoded.convert('RGBA')
        alpha = rgba.getchannel('A')
        if any(alpha.histogram()[1:255]):
            raise ValueError('半透明を含む入力にはまだ対応していません。不透明なPNG/JPEGで試してください。')
        rgb = rgba.convert('RGB')
        del rgba
        profile = decoded.info.get('icc_profile')
        if profile:
            rgb = ImageCms.profileToProfile(rgb, ImageCms.ImageCmsProfile(io.BytesIO(profile)),
                                           ImageCms.createProfile('sRGB'), outputMode='RGB')
        # Transparent RGB never contributes dark edges or sampled line colors.
        rgb.paste('white', mask=ImageChops.invert(alpha))
        return rgb, alpha


def extract(rgb, alpha, threshold):
    import numpy as np
    from PIL import ImageFilter
    gray = rgb.convert('L')
    closing = gray.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.MinFilter(9))
    delta = np.asarray(closing, dtype=np.int16) - np.asarray(gray, dtype=np.int16)
    return (delta >= threshold) & (np.asarray(alpha) > 0)


def suppression_workers(chunk_count):
    """Respect this process's CPU allocation as well as the bounded job size."""
    try:
        cpus = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        cpus = os.cpu_count() or 1
    return max(1, min(MAX_SUPPRESSION_THREADS, cpus, chunk_count))


def suppress_lines(rgb, alpha):
    import numpy as np
    from PIL import Image, ImageFilter
    large = rgb.width * rgb.height >= 10_000_000
    if large:
        print('塗りの元線の領域を検出しています。', file=sys.stderr)
    mask = extract(rgb, alpha, STRENGTHS['strong'])
    dilated = np.asarray(Image.fromarray((mask * 255).astype('uint8')).filter(ImageFilter.MaxFilter(3))) > 0
    dilated &= np.asarray(alpha) > 0
    if not dilated.any():
        return rgb.copy()
    values = np.array(rgb, dtype=np.float32)
    values[dilated] = np.asarray(rgb.filter(ImageFilter.GaussianBlur(3)), dtype=np.float32)[dilated]
    ys, xs = np.nonzero(dilated)
    del mask, dilated
    # Keep the Jacobi update simultaneous, but bound the temporary neighbor
    # arrays. Four full-size int64 index arrays are costly on large images.
    next_values = np.empty((len(ys), 3), dtype=np.float32)
    chunks = [(begin, min(begin + SUPPRESSION_CHUNK_PIXELS, len(ys)))
              for begin in range(0, len(ys), SUPPRESSION_CHUNK_PIXELS)]
    workers = suppression_workers(len(chunks))

    def update(begin, end):
        y, x = ys[begin:end], xs[begin:end]
        next_values[begin:end] = (values[np.maximum(y-1, 0), x]
            + values[np.minimum(y+1, rgb.height-1), x]
            + values[y, np.maximum(x-1, 0)]
            + values[y, np.minimum(x+1, rgb.width-1)]) * .25

    def apply(begin, end):
        # np.nonzero supplies unique coordinates, so these writes are disjoint.
        values[ys[begin:end], xs[begin:end]] = next_values[begin:end]

    with (ThreadPoolExecutor(max_workers=workers) if workers > 1 else nullcontext()) as executor:
        for iteration in range(160):
            if executor is None:
                for begin, end in chunks:
                    update(begin, end)
                values[ys, xs] = next_values
            else:
                # Finish every read of this iteration before changing values,
                # then finish every write before the next iteration can read.
                for operation in (update, apply):
                    pending = [executor.submit(operation, begin, end) for begin, end in chunks]
                    for future in pending:
                        future.result()
            if large and (iteration + 1) % 20 == 0:
                print(f'塗りの元線を補間しています（{iteration + 1}/160）。', file=sys.stderr)
    np.rint(values, out=values)
    np.clip(values, 0, 255, out=values)
    return Image.fromarray(values.astype(np.uint8), 'RGB')


def trace_lines(rgb, alpha, strength, detail, autotrace, scratch):
    import numpy as np
    from PIL import Image
    root = svg_root(*rgb.size)
    mask = extract(rgb, alpha, STRENGTHS[strength])
    if not mask.any():
        return root
    maskpath = scratch / 'lines.png'
    Image.fromarray(np.where(mask, 0, 255).astype('uint8'), 'L').save(maskpath)
    raw = scratch / 'traced.svg'
    command = [sys.executable, str(HERE / 'autotrace_runner.py'), str(os.getpid()),
               str(autotrace), str(maskpath), '-background-color', 'FFFFFF', '-centerline',
               '-error-threshold', str(DETAILS[detail]), '-filter-iterations', '4',
               '-corner-threshold', '100', '-corner-always-threshold', '60',
               '-corner-surround', '4', '-line-threshold', '1',
               '-line-reversion-threshold', '.01', '-despeckle-level', '0',
               '-output-format', 'svg', '-output-file', str(raw)]
    timeout = min(7200, 45 + rgb.width * rgb.height / 25000)
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, check=True)
    except subprocess.TimeoutExpired as exc:
        raise ValueError(f'主線のベクター化が制限時間（約{timeout / 60:.1f}分）以内に'
                         '完了しませんでした。線抽出の強さや曲線の細かさを下げて再試行してください。') from exc
    except subprocess.CalledProcessError as exc:
        raise ValueError('主線のベクター化に失敗しました。' + exc.stderr[-1000:]) from exc
    if result.stderr.strip():
        print(result.stderr[-1000:], file=sys.stderr)
    traced = read_svg(raw)
    traced.set('viewBox', f'0 0 {rgb.width} {rgb.height}')
    return traced


def lines_svg(traced, rgb, alpha, correction=False):
    from color_paths import color_paths
    root = svg_root(*rgb.size)
    if correction:
        from line_correction import correct_paths
        traced, stats = correct_paths(traced, tolerance=1.0)
        root.set('data-qviewsr-correction', json.dumps(stats, sort_keys=True))
    rgba = rgb.copy(); rgba.putalpha(alpha)
    colored = color_paths(traced, rgba, exclude_transparent=True)
    if not colored:
        return root
    group = ET.SubElement(root, f'{{{NS}}}g', {'fill': 'none', 'stroke-width': '1',
                                              'stroke-linecap': 'round', 'stroke-linejoin': 'round'})
    for segment in colored:
        ET.SubElement(group, f'{{{NS}}}path', {'d': segment['d'], 'stroke': segment['color'], 'fill': 'none'})
    return root


def fill_svg(rgb, alpha, suppression, cache, scratch, hits):
    from PIL import Image
    import numpy as np
    import vtracer
    if not np.any(np.asarray(alpha)):
        return svg_root(*rgb.size)
    working = rgb
    if suppression:
        cleaned = cache / 'suppressed-source.png'
        try:
            with open_image_checked(cleaned) as existing:
                if existing.size != rgb.size or existing.mode != 'RGB':
                    raise ValueError('cache dimensions')
                clean = existing.copy()
            hits.append('suppression')
        except (OSError, ValueError):
            print('塗りの元線を補間しています。', file=sys.stderr)
            clean = suppress_lines(rgb, alpha)
            save_image(clean, cleaned)
        working = Image.blend(rgb, clean, suppression / 100.)
    working = working.copy(); working.putalpha(alpha)
    inputpath, outputpath = scratch / 'fill.png', scratch / 'fill.svg'
    working.save(inputpath)
    vtracer.convert_image_to_svg_py(str(inputpath), str(outputpath),
        colormode='color', hierarchical='stacked', mode='spline', filter_speckle=2,
        color_precision=7, layer_difference=8, corner_threshold=60, length_threshold=4.,
        max_iterations=10, splice_threshold=45, path_precision=3)
    root = read_svg(outputpath)
    root.set('viewBox', f'0 0 {rgb.width} {rgb.height}')
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--strength', choices=STRENGTHS, default='balanced')
    parser.add_argument('--detail', choices=DETAILS, default='balanced')
    parser.add_argument('--suppression', type=int, default=35)
    parser.add_argument('--correct-lines', action='store_true',
                        help='主線を許容誤差1px以内で直線・円・楕円・少数のベジェ曲線に補正する')
    args = parser.parse_args()
    if not 0 <= args.suppression <= 100:
        raise ValueError('元線の弱化は0〜100で指定してください。')
    autotrace = Path(os.environ.get('QVIEWSR_AUTOTRACE',
                                   str(REPO / '.local/vector-probe-autotrace/bin/autotrace'))).resolve()
    if not autotrace.is_file() or not os.access(autotrace, os.X_OK):
        raise ValueError('AutoTraceが見つかりません。python3 tools/vector/setup_vector.py を実行してください。')
    packages = {name: importlib.metadata.version(name) for name in ('numpy', 'Pillow', 'vtracer')}
    import vtracer
    if (packages['vtracer'] != '0.6.15+qviewsr.4'
            or getattr(vtracer, '__qviewsr_patch__', None) != 'large-color-sums-area-tree-parallel-sparse-v4'):
        raise ValueError('大画像と並列処理に対応したVTracerが必要です。'
                         'python3 tools/vector/setup_vector.py を実行して更新してください。')
    cachebase = args.cache.resolve()
    cachebase.mkdir(parents=True, exist_ok=True)
    hits = []
    with tempfile.TemporaryDirectory(prefix='working-', dir=cachebase) as temporary:
        scratch = Path(temporary)
        snapshot = scratch / 'source'
        source_sha = snapshot_source(args.input, snapshot)
        # The immutable snapshot identifies both cached layers. Read its header
        # and retain the memory preflight before allocating any decoded pixels.
        with open_image_checked(snapshot) as header:
            width, height = header.size
        sourcekey = key([ALGORITHM, source_sha, packages['numpy'], packages['Pillow']])
        cache = cachebase / sourcekey
        cache.mkdir(parents=True, exist_ok=True)
        fillkey = key([ALGORITHM, packages['vtracer'], args.suppression])
        linekey = key([ALGORITHM, hashlib.sha256(autotrace.read_bytes()).hexdigest(), args.strength, args.detail])
        rawpath = cache / f'raw-lines-{linekey}.svg'
        if args.correct_lines:
            from line_correction import CORRECTION_VERSION
            linekey = key([linekey, CORRECTION_VERSION, 1.0])
        fillpath = cache / f'fill-{fillkey}.svg'
        linepath = cache / f'lines-{linekey}.svg'
        fill_cached = validate_svg(fillpath, width, height)
        lines_cached = validate_svg(linepath, width, height)
        if not fill_cached or not lines_cached:
            # Color sampling and tracing need pixels only when an asset is
            # missing. An ON/OFF comparison with both variants already cached
            # must not decode/convert a very large source image again.
            print('入力画像を展開し、色を確認しています。', file=sys.stderr)
            rgb, alpha = load_source(snapshot)
        if fill_cached:
            hits.append('fill')
        else:
            print('塗りをカラーSVGに変換しています。', file=sys.stderr)
            save_svg(fill_svg(rgb, alpha, args.suppression, cache, scratch, hits), fillpath, width, height)
        if lines_cached:
            hits.append('lines')
        else:
            if validate_svg(rawpath, width, height):
                hits.append('raw_lines')
                traced = read_svg(rawpath)
            else:
                print('主線を抽出し、ベジェ曲線に変換しています。', file=sys.stderr)
                traced = trace_lines(rgb, alpha, args.strength, args.detail, autotrace, scratch)
                save_svg(traced, rawpath, width, height)
            if args.correct_lines:
                print('主線を直線・円・楕円・なめらかな曲線に補正しています。', file=sys.stderr)
            save_svg(lines_svg(traced, rgb, alpha, args.correct_lines), linepath, width, height)
        stats = json.loads(read_svg(linepath).get('data-qviewsr-correction', '{}'))
    print(json.dumps({'width': width, 'height': height, 'fill_svg': str(fillpath),
                      'lines_svg': str(linepath), 'cache_hits': hits,
                      'correct_lines': args.correct_lines, 'correction_stats': stats}, ensure_ascii=False))


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('ベクター変換を中止しました。', file=sys.stderr)
        sys.exit(130)
    except MemoryError as exc:
        print(f'ベクター変換に失敗しました: {exc or "処理中にメモリが不足しました。ほかのアプリを閉じて再試行してください。"}',
              file=sys.stderr)
        sys.exit(1)
    except Exception as exc:
        print(f'ベクター変換に失敗しました: {exc}', file=sys.stderr)
        sys.exit(1)
