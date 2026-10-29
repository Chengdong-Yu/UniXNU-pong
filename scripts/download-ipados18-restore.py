#!/usr/bin/env python3
"""Resume the requested Apple restore download using bounded parallel ranges.

Transport offsets/lengths are checked; no image digest or image audit is run
unless --sha256 asks for one, which is a whole-file hash of what was written.
Apple inputs stay in the ignored local directory, never in source control.

The default is the pinned 22H374 iPad restore this repository builds against.
--url downloads another restore instead, which is what a workflow needs when the
IPSW is an input rather than a constant: the file name, byte length and
checkpoint then describe that URL, and a checkpoint written for one restore is
still refused for another.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time
import urllib.error
import urllib.request

URL = ('https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97955/'
       'A3F72236-89AD-47C5-8394-5E824079C175/iPad_10.2_18.7.10_22H374_Restore.ipsw')
SIZE = 7891396130


def file_name(url):
    """The last path segment, without a query string or fragment."""
    base = url.split('#', 1)[0].split('?', 1)[0]
    name = base.rsplit('/', 1)[-1]
    if not name:
        raise SystemExit('cannot name a file after ' + url)
    return name


def remote_size(url):
    """The byte length of a URL, from HEAD or from a one-byte range request."""
    request = urllib.request.Request(url, method='HEAD')
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            declared = response.headers.get('Content-Length')
            if declared:
                return int(declared)
    except urllib.error.HTTPError as error:
        if error.code not in (403, 405, 501):
            raise
    request = urllib.request.Request(url, headers={'Range': 'bytes=0-0'})
    with urllib.request.urlopen(request, timeout=60) as response:
        content_range = response.headers.get('Content-Range', '')
    _, _, total = content_range.partition('/')
    if not total.isdigit():
        raise SystemExit('%s did not report a byte length; pass --size' % url)
    return int(total)


def digest_of(path):
    hasher = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(8 * 1024**2), b''):
            hasher.update(block)
    return hasher.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--chunk-mib', type=int, default=64)
    parser.add_argument('--url', help='restore URL to download; defaults to the pinned 22H374 iPad restore')
    parser.add_argument('--size', type=int,
                        help='restore byte length; asked of the server when omitted')
    parser.add_argument('--sha256',
                        help='after a complete download, refuse the file unless its sha256 is this')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16 or not 8 <= args.chunk_mib <= 256:
        raise SystemExit('Use1..16 jobs and8..256 MiB chunks')
    url = (args.url or URL).strip()
    if not url.startswith('https://'):
        raise SystemExit('the restore URL must be https, got: ' + url)
    size = args.size or (SIZE if url == URL else remote_size(url))
    if size <= 0:
        raise SystemExit('the restore size must be positive, got %d' % size)
    expected = (args.sha256 or '').strip().lower() or None
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / file_name(url)
    checkpoint = out / 'download-progress.json'
    if checkpoint.exists():
        state = json.loads(checkpoint.read_text())
        if state['url'] != url or state['bytes'] != size:
            raise SystemExit('Download checkpoint describes another restore')
        if not image.exists():
            raise SystemExit('Download data missing; preserve or remove checkpoint explicitly')
    else:
        # A normal curl partial file is a contiguous downloaded prefix.
        prefix = image.stat().st_size if image.exists() else 0
        if prefix > size:
            raise SystemExit('Existing restore is larger than the requested transfer')
        state = dict(url=url, bytes=size, prefix_bytes=prefix, image=image.name,
                     chunk_bytes=args.chunk_mib * 1024**2, completed=[], state='downloading',
                     image_verification='skipped by request')
        checkpoint.write_text(json.dumps(state, indent=2) + '\n')
    state['image'] = image.name
    if state['state'] == 'downloaded' and image.is_file():
        if expected and digest_of(image) != expected:
            raise SystemExit('the downloaded restore does not match the requested sha256')
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
        end = min(size, start + state['chunk_bytes'])
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
                           '--output', str(partial), url]
                subprocess.run(command, check=True)
                ranges = re.findall(r'(?im)^content-range:\s*bytes\s+(\d+)-(\d+)/(\d+)', headers.read_text())
                if not ranges or tuple(map(int, ranges[-1])) != (start, end-1, size) or partial.stat().st_size != end-start:
                    raise RuntimeError('Apple server did not return the requested transport range')
                os.replace(partial, complete)
                with lock:
                    state['completed'].append(start)
                    completed = state['prefix_bytes'] + sum(min(state['chunk_bytes'], size - at) for at in state['completed'])
                    temp = checkpoint.with_suffix('.tmp')
                    temp.write_text(json.dumps(state, indent=2) + '\n')
                    os.replace(temp, checkpoint)
                    print(f'Downloaded {completed / 1024**3:.2f}/{size / 1024**3:.2f} GiB ({time.monotonic()-started:.0f}s)', flush=True)
                return
            except Exception as error:
                print(f'Range {start} attempt {attempt + 1}: {error}', flush=True)
                if attempt == 4:
                    raise
                time.sleep(min(10, 2**attempt))

    ranges = [at for at in range(state['prefix_bytes'], size, state['chunk_bytes']) if at not in state['completed']]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(download, ranges):
            pass
    print('Assembling the downloaded ranges' + ('' if expected else ' (no digest pass)'),
          flush=True)
    assembled = image.with_name(image.name + '.partial')
    with assembled.open('wb') as target:
        for part in [prefix] + [parts / f'{at}.part' for at in range(state['prefix_bytes'], size, state['chunk_bytes'])]:
            with part.open('rb') as source:
                shutil.copyfileobj(source, target, 8*1024**2)
    os.replace(assembled, image)
    if image.stat().st_size != size:
        raise SystemExit('%s is %d bytes, expected %d' % (image, image.stat().st_size, size))
    # Only now is there a whole file to hash. A mismatch leaves the download in
    # place and the checkpoint short of 'downloaded', so a retry re-hashes
    # rather than trusting a byte count.
    if expected:
        print('Hashing the assembled restore for the requested sha256', flush=True)
        actual = digest_of(image)
        if actual != expected:
            state['sha256'] = actual
            state['state'] = 'digest-mismatch'
            checkpoint.write_text(json.dumps(state, indent=2) + '\n')
            raise SystemExit('restore sha256 is %s, expected %s' % (actual, expected))
        state['sha256'] = actual
    state['state'] = 'downloaded'
    checkpoint.write_text(json.dumps(state, indent=2) + '\n')
    shutil.rmtree(parts)
    print(image, flush=True)


if __name__ == '__main__':
    main()
