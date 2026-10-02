#!/usr/bin/env python3
"""Install pinned vector dependencies into the repository's isolated .local tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def run(command, **kwargs):
    print('+ ' + shlex.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def build_autotrace(root, manifest, offline):
    archive = root / (manifest['source_directory'] + '.tar.gz')
    if not archive.exists():
        if offline:
            raise RuntimeError(f'ソースアーカイブがありません: {archive}')
        request = urllib.request.Request(manifest['url'], headers={'User-Agent': 'qViewSR-vector-setup'})
        temp = archive.with_suffix('.download')
        try:
            with urllib.request.urlopen(request, timeout=60) as response, temp.open('wb') as output:
                shutil.copyfileobj(response, output)
            os.replace(temp, archive)
        finally:
            temp.unlink(missing_ok=True)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != manifest['sha256']:
        raise RuntimeError(f'AutoTraceソースのSHA-256が一致しません: {digest}')
    source = root / manifest['source_directory']
    # Validate the hash on every build and replace only the pinned source tree.
    with tarfile.open(archive, 'r:gz') as bundle:
        bundle.extractall(root, filter='data')
    for license_file in manifest['license_files']:
        if not (source / license_file).is_file():
            raise RuntimeError(f'ライセンスファイルがありません: {license_file}')
    flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'glib-2.0', 'gobject-2.0', 'libpng'],
                           capture_output=True, text=True)
    if flags.returncode == 0:
        dependencies = shlex.split(flags.stdout)
    elif (root / 'sdk/usr/include/glib-2.0/glib.h').is_file():
        # Reuse the locally staged Ubuntu SDK used by the original probe.
        archdirs = list((root / 'sdk/usr/lib').glob('*/glib-2.0/include'))
        if len(archdirs) != 1:
            raise RuntimeError('ローカルGLib SDKの構成を確認してください。')
        dependencies = ['-I'+str(root/'sdk/usr/include/glib-2.0'), '-I'+str(archdirs[0]),
                        '-l:libglib-2.0.so.0', '-l:libgobject-2.0.so.0', '-lpng16']
    else:
        raise RuntimeError('開発依存が不足しています。Ubuntuでは gcc pkg-config libglib2.0-dev libpng-dev を導入してください。')
    # Official tag 0.31.10 declares 0.40.0 in its configure.ac.
    (root / 'config.h').write_text('''#define AUTOTRACE_VERSION "0.40.0"
#define AUTOTRACE_WEB "https://github.com/autotrace/autotrace"
#define PACKAGE "autotrace"
#define HAVE_LIBPNG 1
#define HAVE_LOCALTIME_R 1
#define LOCALEDIR ""
''')
    skipped = {'input-magick.c', 'output-pstoedit.c', 'output-swf.c'}
    inputs = [path for path in sorted((source/'src').glob('*.c')) if path.name not in skipped]
    if not inputs:
        raise RuntimeError('AutoTraceソースが見つかりません。')
    executable = root / 'bin/autotrace'
    executable.parent.mkdir(exist_ok=True)
    temporary = executable.with_name('autotrace.new')
    run(['gcc', '-O2', '-DHAVE_CONFIG_H', '-I'+str(root), '-I'+str(source/'src'),
         *inputs, '-Wno-incompatible-pointer-types', '-Wno-deprecated-declarations',
         *dependencies, '-lm', '-o', temporary])
    os.replace(temporary, executable)
    run([executable, '--version'])
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true', help='reuse installed wheels and cached source')
    parser.add_argument('--rebuild-autotrace', action='store_true')
    parser.add_argument('--check', action='store_true', help='check without writing or downloading')
    args = parser.parse_args()
    manifest = json.loads((HERE/'dependencies.json').read_text())
    venv = REPO / '.local/vector-probe-venv'
    python = venv / 'bin/python'
    root = REPO / '.local/vector-probe-autotrace'
    executable = root / 'bin/autotrace'
    if not args.check:
        if not python.exists():
            run([sys.executable, '-m', 'venv', venv])
        if not args.offline:
            run([python, '-m', 'pip', 'install', '--disable-pip-version-check', '-r', HERE/'requirements.txt'])
        root.mkdir(parents=True, exist_ok=True)
        if args.rebuild_autotrace or not executable.is_file():
            executable = build_autotrace(root, manifest['autotrace'], args.offline)
    if not python.is_file() or not executable.is_file():
        raise RuntimeError('依存環境が未導入です。--checkを外して実行してください。')
    result = subprocess.check_output([python, '-c',
        'import importlib.metadata,json; print(json.dumps({n:importlib.metadata.version(n) for n in ["numpy","Pillow","vtracer"]}))'], text=True)
    versions = json.loads(result)
    if versions != manifest['python_packages']:
        raise RuntimeError(f'依存バージョンが一致しません: {versions}')
    run([executable, '--version'])
    record = {'python': str(python), 'versions': versions, 'autotrace': str(executable),
              'autotrace_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
              'source_manifest': manifest['autotrace']}
    if not args.check:
        (root / 'qviewsr-dependencies.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(record, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'ベクター変換環境を準備できませんでした: {exc}', file=sys.stderr)
        sys.exit(1)
