#!/usr/bin/env python3
"""Check installed patched VTracer against SVGs produced by the original 0.6.15 wheel."""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import random
import tempfile

from PIL import Image, ImageDraw
import vtracer

HERE = Path(__file__).resolve().parent
BASELINE = HERE / 'patches/vtracer-small-image-baseline.json'
PARAMETERS = dict(colormode='color', hierarchical='stacked', mode='spline',
                  filter_speckle=2, color_precision=7, layer_difference=8,
                  corner_threshold=60, length_threshold=4., max_iterations=10,
                  splice_threshold=45, path_precision=3)


def fixtures():
    yield 'white', Image.new('RGB', (96, 64), 'white')
    shapes = Image.new('RGB', (128, 96), '#edece9')
    draw = ImageDraw.Draw(shapes)
    draw.rectangle((7, 9, 54, 78), fill='#f0aaaa', outline='#523242', width=3)
    draw.ellipse((38, 18, 115, 84), fill='#5da4d8', outline='#24334d', width=3)
    draw.line((1, 90, 122, 4), fill='#221822', width=2)
    yield 'shapes', shapes
    gradient = bytes(value for y in range(64) for x in range(96)
                     for value in (96+x, 90+x+y//2, 180-y))
    yield 'low-difference-gradient', Image.frombytes('RGB', (96, 64), gradient)
    checker = Image.new('RGB', (64, 64))
    checker.putdata([(241, 243, 246) if (x//4+y//4)%2 else (24, 54, 91)
                     for y in range(64) for x in range(64)])
    yield 'small-regions', checker
    rng = random.Random(8934)
    noise = bytes(160+rng.randrange(48) for _ in range(48*48*3))
    yield 'noise', Image.frombytes('RGB', (48, 48), noise)
    alpha = Image.new('RGBA', (96, 64), (0, 0, 0, 0))
    draw = ImageDraw.Draw(alpha)
    draw.ellipse((5, 3, 89, 60), fill='#ab89ba')
    draw.rectangle((32, 20, 66, 45), fill=(0, 0, 0, 0))
    yield 'binary-alpha', alpha


def convert_fixtures(num_threads=None):
    results = {}
    with tempfile.TemporaryDirectory(prefix='qviewsr-vtracer-test-') as directory:
        root = Path(directory)
        for name, image in fixtures():
            input_path, output_path = root/(name+'.png'), root/(name+'.svg')
            image.save(input_path)
            options = dict(PARAMETERS)
            if num_threads is not None:
                options['num_threads'] = num_threads
            vtracer.convert_image_to_svg_py(str(input_path), str(output_path), **options)
            svg = output_path.read_bytes()
            results[name] = {'input_sha256': hashlib.sha256(image.tobytes()).hexdigest(),
                             'svg_sha256': hashlib.sha256(svg).hexdigest(), 'svg_bytes': len(svg)}
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record-baseline', action='store_true',
                        help='record only when using the original unpatched 0.6.15 wheel')
    args = parser.parse_args()
    version = importlib.metadata.version('vtracer')
    if args.record_baseline:
        if version != '0.6.15' or hasattr(vtracer, '__qviewsr_patch__'):
            raise RuntimeError('Baseline must come from the original VTracer 0.6.15 wheel.')
        results = convert_fixtures()
        if results != convert_fixtures():
            raise AssertionError('Original output is not deterministic.')
        extension = Path(vtracer.__file__).parent / Path(vtracer.vtracer.__file__).name
        baseline = {'vtracer': version, 'Pillow': importlib.metadata.version('Pillow'),
                    'original_extension_sha256': hashlib.sha256(extension.read_bytes()).hexdigest(),
                    'parameters': PARAMETERS, 'fixtures': results}
        BASELINE.write_text(json.dumps(baseline, indent=2)+'\n')
        print(f'Recorded {len(results)} deterministic upstream SVGs: {BASELINE}')
    else:
        assert version == '0.6.15+qviewsr.4', version
        assert vtracer.__qviewsr_patch__ == 'large-color-sums-area-tree-parallel-sparse-v4'
        baseline = json.loads(BASELINE.read_text())
        assert baseline['parameters'] == PARAMETERS
        for threads in (None, 1, 8, 16):
            actual = convert_fixtures(threads)
            for name, expected in baseline['fixtures'].items():
                assert actual[name] == expected, (
                    f'SVG changed for {name}, threads={threads}: {actual[name]} != {expected}')
        with tempfile.TemporaryDirectory(prefix='qviewsr-vtracer-errors-') as directory:
            root = Path(directory)
            source = root/'input.png'
            Image.new('RGB', (16, 16), 'white').save(source)
            output = root/'output.svg'
            for threads in (0, 17):
                try:
                    vtracer.convert_image_to_svg_py(str(source), str(output), num_threads=threads)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f'Invalid thread budget accepted: {threads}')
                assert not output.exists()
            try:
                vtracer.convert_image_to_svg_py(str(source), str(root))
            except Exception:
                pass
            else:
                raise AssertionError('Output creation failure was not propagated to Python.')
        print(f'{len(actual)} SVGs × auto/1/8/16 threads match upstream bytes; marker/errors OK.')


if __name__ == '__main__':
    main()
