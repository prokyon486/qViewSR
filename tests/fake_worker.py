#!/usr/bin/env python3
"""Persistent deterministic GUI protocol peer. It does not perform SR."""
import hashlib
import json
import os
import pathlib
import queue
import shutil
import struct
import sys
import threading
import time
import zlib

commands = queue.Queue()
cancelled = set()
run_index = 0

def emit(job, event, **fields):
    print(json.dumps(dict(protocol_version=1, job_id=job['job_id'], event=event, **fields)), flush=True)

def reader():
    for line in sys.stdin:
        command = json.loads(line)
        if command['command'] == 'shutdown':
            commands.put(None)
            return
        if command['command'] == 'cancel':
            cancelled.add(command['job_id'])
        else:
            commands.put(command)
    commands.put(None)

def chunk(name, data):
    return struct.pack('!I', len(data)) + name + data + struct.pack('!I', zlib.crc32(name + data))

def run(job):
    global run_index
    run_index += 1
    mode = job['devices']
    if pathlib.Path(job['output']).exists():
        emit(job, 'error', message='Output file already exists.')
        return
    if mode.startswith('animation'):
        shutil.copyfile(job['input'], pathlib.Path(job['model_xml']).with_suffix(f'.frame-{run_index}.png'))
        with pathlib.Path(job['model_xml']).with_suffix('.frames.jsonl').open('a') as log:
            log.write(json.dumps(dict(index=run_index, width=job['width'], height=job['height'], denoise=job.get('denoise', 0))) + '\n')
    shutil.copyfile(job['input'], pathlib.Path(job['model_xml']).with_suffix('.last-input.png'))
    emit(job, 'started', total=1)
    if mode == 'delay' or (mode == 'animation-delay' and run_index > 1):
        for _ in range(60):
            if job['job_id'] in cancelled:
                emit(job, 'cancelled')
                return
            time.sleep(.05)
    if mode == 'crash':
        os._exit(3)
    if mode == 'long-error':
        emit(job, 'error', message=('とても長いエラー表示です\n詳細\r\n' * 80))
        return
    width, height = job['width'] * 4, job['height'] * 4
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, 2, 0, 0, 0))
    color = hashlib.sha256(pathlib.Path(job['input']).read_bytes()).digest()[:3] if mode.startswith('animation') else b'\x50\xa0\xd0'
    png += chunk(b'IDAT', zlib.compress((b'\x00' + color * width) * height))
    png += chunk(b'IEND', b'')
    pathlib.Path(job['output']).write_bytes(png)
    emit(job, 'progress', completed=1, total=1, device='FAKE')
    emit(job, 'completed', source_key=job['source_key'], width=width, height=height, color_space='sRGB',
         output_sha256='wrong' if mode == 'bad-hash' or (mode == 'animation-bad-second' and run_index == 2) else hashlib.sha256(png).hexdigest(),
         devices=[dict(id='FAKE', tiles=1)], wall_ms=0, denoise=job.get('denoise', 0))

if sys.argv[1] == '--list':
    print(json.dumps(dict(protocol_version=1, event='devices', job_id='', devices=['UNOWNED'])))
elif sys.argv[1] == '--serve':
    threading.Thread(target=reader, daemon=True).start()
    while (job := commands.get()) is not None:
        if job['command'] == 'configure':
            run_index = 0
            emit(job, 'session_ready', devices=['FAKE'])
        else:
            run(job)
else:
    run(json.loads(pathlib.Path(sys.argv[2]).read_text()))
