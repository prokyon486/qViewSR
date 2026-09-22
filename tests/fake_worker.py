#!/usr/bin/env python3
"""Deterministic protocol peer for GUI lifecycle tests; does not perform SR."""
import hashlib
import json
import pathlib
import struct
import sys
import time
import zlib

job = json.loads(pathlib.Path(sys.argv[2]).read_text())
mode = job['devices']


def emit(event, **fields):
    print(json.dumps(dict(protocol_version=1, job_id=job['job_id'], event=event, **fields)), flush=True)


emit('started', total=1)
if mode == 'delay':
    time.sleep(3)
if mode == 'crash':
    sys.exit(3)
width, height = job['width'] * 4, job['height'] * 4


def chunk(name, data):
    return struct.pack('!I', len(data)) + name + data + struct.pack('!I', zlib.crc32(name + data))


png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress((b'\x00' + b'\x50\xa0\xd0' * width) * height))
png += chunk(b'IEND', b'')
pathlib.Path(job['output']).write_bytes(png)
emit('progress', completed=1, total=1, device='FAKE')
emit('completed', source_key=job['source_key'], width=width, height=height, color_space='sRGB',
     output_sha256='wrong' if mode == 'bad-hash' else hashlib.sha256(png).hexdigest(),
     devices=[dict(id='FAKE', tiles=1)], wall_ms=0)
