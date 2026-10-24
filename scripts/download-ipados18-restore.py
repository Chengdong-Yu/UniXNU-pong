#!/usr/bin/env python3
"""Resume the requested Apple restore download using bounded parallel ranges.

Transport offsets/lengths are checked; no image digest or image audit is run.
Apple inputs stay in the ignored local directory, never in source control.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time

URL = ('https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97955/'
       'A3F72236-89AD-47C5-8394-5E824079C175/iPad_10.2_18.7.10_22H374_Restore.ipsw')
SIZE = 7891396130


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--chunk-mib', type=int, default=64)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16 or not 8 <= args.chunk_mib <= 256:
        raise SystemExit('Use1..16 jobs and8..256 MiB chunks')
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / URL.rsplit('/', 1)[1]
    checkpoint = out / 'download-progress.json'
    if checkpoint.exists():
        state = json.loads(checkpoint.read_text())
        if state['url'] != URL or state['bytes'] != SIZE:
            raise SystemExit('Download checkpoint describes another restore')
        if not image.exists():
            raise SystemExit('Download data missing; preserve or remove checkpoint explicitly')
    else:
        # A normal curl partial file is a contiguous downloaded prefix.
        prefix = image.stat().st_size if image.exists() else 0
        if prefix > SIZE:
            raise SystemExit('Existing restore is larger than the requested transfer')
        state = dict(url=URL, bytes=SIZE, prefix_bytes=prefix,
                     chunk_bytes=args.chunk_mib * 1024**2, completed=[], state='downloading',
                     image_verification='skipped by request')
        checkpoint.write_text(json.dumps(state, indent=2) + '\n')
    if state['state'] == 'downloaded':
        print('Restore download already completed', flush=True)
        return
    parts = out / '.restore-parts'
    parts.mkdir(exist_ok=True)
    prefix = parts / 'prefix.bin'
    if not prefix.exists() and not state['prefix_bytes']:
        prefix.touch()
    if not prefix.exists():
        with image.open('rb') as source, prefix.open('wb') as target:
            remaining = state['prefix_bytes']
            while remaining:
                data = source.read(min(8*1024**2, remaining))
                if not data:
                    raise RuntimeError('Downloaded prefix is missing')
                target.write(data)
                remaining -= len(data)
    # Old urllib checkpoints described in-place ranges; curl uses separate
    # completed parts so interrupted transfers never publish an incomplete file.
    if state.get('transport') != 'curl-ranges':
        state['completed'] = []
    state['transport'] = 'curl-ranges'
    state['completed'] = [at for at in state['completed'] if (parts / f'{at}.part').exists()]
    checkpoint.write_text(json.dumps(state, indent=2) + '\n')
    lock = threading.Lock()
    started = time.monotonic()

    def download(start):
        end = min(SIZE, start + state['chunk_bytes'])
        partial = parts / f'{start}.partial'
        complete = parts / f'{start}.part'
        headers = parts / f'{start}.headers'
        print(f'Starting range {start}..{end}', flush=True)
        for attempt in range(5):
            try:
                command = ['curl', '--fail', '--location', '--silent', '--show-error',
                           '--connect-timeout', '15', '--speed-time', '30', '--speed-limit', '1024',
                           '--max-time', '180', '--range', f'{start}-{end-1}',
                           '--max-filesize', str(end-start), '--dump-header', str(headers),
                           '--output', str(partial), URL]
                subprocess.run(command, check=True)
                ranges = re.findall(r'(?im)^content-range:\s*bytes\s+(\d+)-(\d+)/(\d+)', headers.read_text())
                if not ranges or tuple(map(int, ranges[-1])) != (start, end-1, SIZE) or partial.stat().st_size != end-start:
                    raise RuntimeError('Apple server did not return the requested transport range')
                os.replace(partial, complete)
                with lock:
                    state['completed'].append(start)
                    completed = state['prefix_bytes'] + sum(min(state['chunk_bytes'], SIZE - at) for at in state['completed'])
                    temp = checkpoint.with_suffix('.tmp')
                    temp.write_text(json.dumps(state, indent=2) + '\n')
                    os.replace(temp, checkpoint)
                    print(f'Downloaded {completed / 1024**3:.2f}/{SIZE / 1024**3:.2f} GiB ({time.monotonic()-started:.0f}s)', flush=True)
                return
            except Exception as error:
                print(f'Range {start} attempt {attempt + 1}: {error}', flush=True)
                if attempt == 4:
                    raise
                time.sleep(min(10, 2**attempt))

    ranges = [at for at in range(state['prefix_bytes'], SIZE, state['chunk_bytes']) if at not in state['completed']]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(download, ranges):
            pass
    print('Assembling the downloaded ranges (no digest pass)', flush=True)
    assembled = image.with_name(image.name + '.partial')
    with assembled.open('wb') as target:
        for part in [prefix] + [parts / f'{at}.part' for at in range(state['prefix_bytes'], SIZE, state['chunk_bytes'])]:
            with part.open('rb') as source:
                shutil.copyfileobj(source, target, 8*1024**2)
    os.replace(assembled, image)
    state['state'] = 'downloaded'
    checkpoint.write_text(json.dumps(state, indent=2) + '\n')
    shutil.rmtree(parts)
    print(image, flush=True)


if __name__ == '__main__':
    main()
