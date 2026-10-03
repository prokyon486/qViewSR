#!/usr/bin/env python3
"""Install pinned vector dependencies into the repository's isolated .local tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
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


def run_pip(python, *arguments):
    env = dict(os.environ, PIP_CACHE_DIR=str(REPO/'.local/vector-patched-vtracer/pip-cache'))
    return run([python, '-m', 'pip', '--disable-pip-version-check', *arguments], env=env)


def checked_archive(root, manifest, offline):
    """Keep the exact upstream archive and reject mismatched cached downloads."""
    archive = root / manifest['archive']
    if not archive.exists():
        if offline:
            raise RuntimeError(f'ソースアーカイブがありません: {archive}')
        request = urllib.request.Request(manifest['url'], headers={'User-Agent': 'qViewSR-vector-setup'})
        temporary = archive.with_suffix('.download')
        try:
            with urllib.request.urlopen(request, timeout=60) as response, temporary.open('wb') as output:
                shutil.copyfileobj(response, output)
            os.replace(temporary, archive)
        finally:
            temporary.unlink(missing_ok=True)
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest != manifest['sha256']:
        raise RuntimeError(f'アーカイブのSHA-256が一致しません: {archive.name}: {digest}')
    return archive


def prepare_rust(root, manifest, offline):
    """Install the pinned official compiler under .local, without rustup or sudo."""
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        raise RuntimeError('修正版VTracerの自動ビルドはLinux x86_64に対応しています。')
    prefix = root / 'rust'
    expected = manifest['version']
    for program in ('rustc', 'cargo'):
        executable = prefix/'bin'/program
        if executable.is_file():
            actual = subprocess.check_output([executable, '--version'], text=True).split()[1]
            if actual != expected:
                raise RuntimeError(f'ローカル{program}の版が一致しません: {actual}')
    marker = prefix / 'qviewsr-toolchain.json'
    if marker.is_file() and json.loads(marker.read_text()) == manifest:
        if not (prefix/'bin/rustc').is_file() or not (prefix/'bin/cargo').is_file():
            raise RuntimeError('ローカルRust環境が不完全です。rustディレクトリを退避して再実行してください。')
        return prefix
    unpacked = root / 'rust-sources'
    unpacked.mkdir(exist_ok=True)
    for component in manifest['components']:
        archive = checked_archive(root, component, offline)
        with tarfile.open(archive) as bundle:
            bundle.extractall(unpacked, filter='data')
        run(['sh', unpacked/component['source_directory']/'install.sh',
             '--prefix='+str(prefix), '--disable-ldconfig'])
    marker.write_text(json.dumps(manifest, indent=2)+'\n')
    return prefix


def prepare_vtracer_sources(root, manifest, offline):
    source = root / 'source'
    source.mkdir(exist_ok=True)
    for name in ('upstream', 'visioncortex'):
        dependency = manifest[name]
        archive = checked_archive(root, dependency, offline)
        # All patched files exist in the pinned archives and are reset on every build.
        with tarfile.open(archive) as bundle:
            bundle.extractall(source, filter='data')
        for license_file in dependency['license_files']:
            if not (source/dependency['source_directory']/license_file).is_file():
                raise RuntimeError(f'ライセンスファイルがありません: {license_file}')
    patch = HERE / manifest['patch']['file']
    if hashlib.sha256(patch.read_bytes()).hexdigest() != manifest['patch']['sha256']:
        raise RuntimeError('VTracer修正パッチのSHA-256が一致しません。')
    run(['patch', '--batch', '--fuzz=0', '-p1', '-i', patch], cwd=source)
    package = source / manifest['upstream']['source_directory']
    licenses = package / 'LICENSES'
    licenses.mkdir(exist_ok=True)
    for name in manifest['visioncortex']['license_files']:
        shutil.copyfile(source/manifest['visioncortex']['source_directory']/name,
                        licenses/('visioncortex-0.8.10-'+name))
    return package


def build_vtracer(root, python, manifest, offline):
    root.mkdir(parents=True, exist_ok=True)
    rust = prepare_rust(root, manifest['rust'], offline)
    source = prepare_vtracer_sources(root, manifest, offline)
    maturin_version = manifest['maturin']
    try:
        actual_maturin = subprocess.check_output([python, '-c',
            'import importlib.metadata; print(importlib.metadata.version("maturin"))'],
            stderr=subprocess.DEVNULL, text=True).strip()
    except subprocess.CalledProcessError:
        actual_maturin = None
    if actual_maturin != maturin_version:
        if offline:
            raise RuntimeError(f'オフラインビルドにはmaturin=={maturin_version}が必要です。')
        run_pip(python, 'install', 'maturin=='+maturin_version)
    env = dict(os.environ, PATH=str(rust/'bin')+os.pathsep+os.environ.get('PATH', ''),
               CARGO_HOME=str(root/'cargo-home'), CARGO_TARGET_DIR=str(root/'target'),
               PYO3_PYTHON=str(python), PYO3_USE_ABI3_FORWARD_COMPATIBILITY='1',
               CARGO_BUILD_JOBS='2')
    if offline:
        env['CARGO_NET_OFFLINE'] = 'true'
    # --locked preserves every transitive version/checksum in upstream Cargo.lock.
    run([rust/'bin/cargo', 'test', '--locked', '--release', '-p', 'visioncortex',
         'qviewsr_'], cwd=source, env=env)
    wheels = root / 'wheels'
    wheels.mkdir(exist_ok=True)
    # A locally built Linux wheel may use this host's glibc, rather than manylinux2014.
    run([python, '-m', 'maturin', 'build', '--release', '--locked', '--compatibility', 'linux',
         '--interpreter', python, '--out', wheels], cwd=source, env=env)
    wheel_candidates = list(wheels.glob('vtracer-'+manifest['version']+'-*.whl'))
    if len(wheel_candidates) != 1:
        raise RuntimeError(f'修正版wheelを一意に選べません: {wheel_candidates}')
    wheel = wheel_candidates[0]
    run_pip(python, 'install', '--no-deps', '--force-reinstall', wheel)
    record = dict(manifest, wheel=str(wheel), wheel_sha256=hashlib.sha256(wheel.read_bytes()).hexdigest(),
                  rustc=subprocess.check_output([rust/'bin/rustc', '--version'], text=True).strip(),
                  cargo_lock_sha256=hashlib.sha256((source/'Cargo.lock').read_bytes()).hexdigest())
    (root/'qviewsr-build.json').write_text(json.dumps(record, indent=2)+'\n')
    return record


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
    parser.add_argument('--rebuild-vtracer', action='store_true')
    parser.add_argument('--check', action='store_true', help='check without writing or downloading')
    args = parser.parse_args()
    manifest = json.loads((HERE/'dependencies.json').read_text())
    venv = REPO / '.local/vector-probe-venv'
    python = venv / 'bin/python'
    root = REPO / '.local/vector-probe-autotrace'
    executable = root / 'bin/autotrace'
    vtracer_root = REPO / '.local/vector-patched-vtracer'
    if not args.check:
        if not python.exists():
            run([sys.executable, '-m', 'venv', venv])
        if not args.offline:
            run_pip(python, 'install', '-r', HERE/'requirements.txt')
        try:
            current_vtracer = subprocess.check_output([python, '-c',
                'import importlib.metadata; print(importlib.metadata.version("vtracer"))'],
                stderr=subprocess.DEVNULL, text=True).strip()
        except subprocess.CalledProcessError:
            current_vtracer = None
        if args.rebuild_vtracer or current_vtracer != manifest['vtracer']['version']:
            build_vtracer(vtracer_root, python, manifest['vtracer'], args.offline)
            run([python, HERE/'test_vtracer_patch.py'])
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
    marker = subprocess.check_output([python, '-c',
        'import vtracer; print(getattr(vtracer, "__qviewsr_patch__", "unpatched"))'], text=True).strip()
    if marker != manifest['vtracer']['marker']:
        raise RuntimeError('VTracerに巨大画像向けの修正が入っていません。--rebuild-vtracerで再ビルドしてください。')
    run([executable, '--version'])
    record = {'python': str(python), 'versions': versions, 'autotrace': str(executable),
              'autotrace_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
              'source_manifest': manifest['autotrace'], 'vtracer_patch': marker,
              'vtracer_build_record': str(vtracer_root/'qviewsr-build.json')}
    if not args.check:
        (root / 'qviewsr-dependencies.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(record, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'ベクター変換環境を準備できませんでした: {exc}', file=sys.stderr)
        sys.exit(1)
