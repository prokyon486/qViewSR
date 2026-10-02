"""CPU-only two-layer vectorization worker. Last stdout line is its JSON result."""
import argparse
import hashlib
import importlib.metadata
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ALGORITHM = 'qviewsr-vector-1'
MAX_PIXELS = 4_000_000
MAX_SIDE = 4096
MAX_SVG_BYTES = 64 * 1024 * 1024
MAX_INPUT_BYTES = 128 * 1024 * 1024
NS = 'http://www.w3.org/2000/svg'
STRENGTHS = {'weak': 20, 'balanced': 12, 'strong': 6}
DETAILS = {'fine': .3, 'balanced': 1., 'simple': 3.}
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
ET.register_namespace('', NS)


def key(data):
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()[:24]


def svg_root(width, height):
    return ET.Element(f'{{{NS}}}svg', {'width': str(width), 'height': str(height),
                                      'viewBox': f'0 0 {width} {height}'})


def validate_svg(path, width, height):
    try:
        if not path.is_file() or path.stat().st_size > MAX_SVG_BYTES:
            return False
        root = ET.parse(path).getroot()
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
    from PIL import Image, ImageCms
    import numpy as np
    with Image.open(path) as decoded:
        width, height = decoded.size
        if width < 1 or height < 1 or width * height > MAX_PIXELS or max(width, height) > MAX_SIDE:
            raise ValueError(f'入力画像は400万画素以下・一辺{MAX_SIDE}px以下にしてください。')
        rgba = decoded.convert('RGBA')
        alpha = rgba.getchannel('A')
        values = np.asarray(alpha)
        if np.any((values != 0) & (values != 255)):
            raise ValueError('半透明を含む入力にはまだ対応していません。不透明なPNG/JPEGで試してください。')
        rgb = rgba.convert('RGB')
        profile = decoded.info.get('icc_profile')
        if profile:
            rgb = ImageCms.profileToProfile(rgb, ImageCms.ImageCmsProfile(io.BytesIO(profile)),
                                           ImageCms.createProfile('sRGB'), outputMode='RGB')
        rgb.putalpha(alpha)
        # Transparent RGB never contributes dark edges or sampled line colors.
        white = Image.new('RGBA', rgba.size, 'white')
        white.alpha_composite(rgb)
        return white.convert('RGB'), alpha


def extract(rgb, alpha, threshold):
    import numpy as np
    from PIL import ImageFilter
    gray = rgb.convert('L')
    closing = gray.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.MinFilter(9))
    delta = np.asarray(closing, dtype=np.int16) - np.asarray(gray, dtype=np.int16)
    return (delta >= threshold) & (np.asarray(alpha) > 0)


def suppress_lines(rgb, alpha):
    import numpy as np
    from PIL import Image, ImageFilter
    mask = extract(rgb, alpha, STRENGTHS['strong'])
    dilated = np.asarray(Image.fromarray((mask * 255).astype('uint8')).filter(ImageFilter.MaxFilter(3))) > 0
    dilated &= np.asarray(alpha) > 0
    values = np.asarray(rgb, dtype=np.float32).copy()
    values[dilated] = np.asarray(rgb.filter(ImageFilter.GaussianBlur(3)), dtype=np.float32)[dilated]
    ys, xs = np.nonzero(dilated)
    ym, yp = np.maximum(ys-1, 0), np.minimum(ys+1, rgb.height-1)
    xm, xp = np.maximum(xs-1, 0), np.minimum(xs+1, rgb.width-1)
    for _ in range(160):
        values[ys, xs] = (values[ym, xs] + values[yp, xs] + values[ys, xm] + values[ys, xp]) * .25
    return Image.fromarray(np.uint8(np.clip(np.rint(values), 0, 255)), 'RGB')


def lines_svg(rgb, alpha, strength, detail, autotrace, scratch):
    import numpy as np
    from PIL import Image
    from color_paths import color_paths
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
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=45, check=True)
    except subprocess.TimeoutExpired as exc:
        raise ValueError('主線のベクター化が45秒以内に完了しませんでした。画像を小さくして試してください。') from exc
    except subprocess.CalledProcessError as exc:
        raise ValueError('主線のベクター化に失敗しました。' + exc.stderr[-1000:]) from exc
    if result.stderr.strip():
        print(result.stderr[-1000:], file=sys.stderr)
    if raw.stat().st_size > MAX_SVG_BYTES:
        raise ValueError('主線SVGが大きすぎます。画像を小さくして試してください。')
    traced = ET.parse(raw).getroot()
    rgba = rgb.copy(); rgba.putalpha(alpha)
    group = ET.SubElement(root, f'{{{NS}}}g', {'fill': 'none', 'stroke-width': '1',
                                              'stroke-linecap': 'round', 'stroke-linejoin': 'round'})
    for segment in color_paths(traced, rgba, exclude_transparent=True):
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
            with Image.open(cleaned) as existing:
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
    if outputpath.stat().st_size > MAX_SVG_BYTES:
        raise ValueError('塗りSVGが大きすぎます。画像を小さくして試してください。')
    root = ET.parse(outputpath).getroot()
    root.set('viewBox', f'0 0 {rgb.width} {rgb.height}')
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--strength', choices=STRENGTHS, default='balanced')
    parser.add_argument('--detail', choices=DETAILS, default='balanced')
    parser.add_argument('--suppression', type=int, default=35)
    args = parser.parse_args()
    if not 0 <= args.suppression <= 100:
        raise ValueError('元線の弱化は0〜100で指定してください。')
    # Keep an accidental replacement/symlink from sharing another source's cache.
    if args.input.stat().st_size > MAX_INPUT_BYTES:
        raise ValueError('入力ファイルは128MB以下にしてください。')
    input_bytes = args.input.read_bytes()
    source_sha = hashlib.sha256(input_bytes).hexdigest()
    # Decode the same immutable byte snapshot used for the key.
    rgb, alpha = load_source(io.BytesIO(input_bytes))
    width, height = rgb.size
    autotrace = Path(os.environ.get('QVIEWSR_AUTOTRACE',
                                   str(REPO / '.local/vector-probe-autotrace/bin/autotrace'))).resolve()
    if not autotrace.is_file() or not os.access(autotrace, os.X_OK):
        raise ValueError('AutoTraceが見つかりません。python3 tools/vector/setup_vector.py を実行してください。')
    packages = {name: importlib.metadata.version(name) for name in ('numpy', 'Pillow', 'vtracer')}
    sourcekey = key([ALGORITHM, source_sha, packages['numpy'], packages['Pillow']])
    cache = args.cache.resolve() / sourcekey
    cache.mkdir(parents=True, exist_ok=True)
    fillkey = key([ALGORITHM, packages['vtracer'], args.suppression])
    linekey = key([ALGORITHM, hashlib.sha256(autotrace.read_bytes()).hexdigest(), args.strength, args.detail])
    fillpath = cache / f'fill-{fillkey}.svg'
    linepath = cache / f'lines-{linekey}.svg'
    hits = []
    with tempfile.TemporaryDirectory(prefix='working-', dir=cache) as temporary:
        scratch = Path(temporary)
        if validate_svg(fillpath, width, height):
            hits.append('fill')
        else:
            print('塗りをカラーSVGに変換しています。', file=sys.stderr)
            save_svg(fill_svg(rgb, alpha, args.suppression, cache, scratch, hits), fillpath, width, height)
        if validate_svg(linepath, width, height):
            hits.append('lines')
        else:
            print('主線を抽出し、ベジェ曲線に変換しています。', file=sys.stderr)
            save_svg(lines_svg(rgb, alpha, args.strength, args.detail, autotrace, scratch), linepath, width, height)
    print(json.dumps({'width': width, 'height': height, 'fill_svg': str(fillpath),
                      'lines_svg': str(linepath), 'cache_hits': hits}, ensure_ascii=False))


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('ベクター変換を中止しました。', file=sys.stderr)
        sys.exit(130)
    except Exception as exc:
        print(f'ベクター変換に失敗しました: {exc}', file=sys.stderr)
        sys.exit(1)
