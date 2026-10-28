#!/usr/bin/env python3
"""Write the userdata image from the SHNGROOT layout prepare-ipados18-ufs-layout.py made.

The device installer streams each window to its offset in /dev/block/sda13 and
commits the 4 KiB directory last. This script produces the same bytes as one
image, so the layout can be inspected, flashed as a whole, or downloaded from a
CI run instead of window by window.

Nothing is hashed: the directory already records each window's SHA-256 and this
script checks that the layout and the directory agree on it, plus the offset and
size. The windows are copied verbatim, and no image bytes are read back.

The output is written as one forward pass, never by seeking, so the same run can
emit fixed-size parts for hosts that cap a single upload. Gaps between windows
are the zeroes a sparse file would have had; the tail after the last window is
not written, because the directory stops there.
"""
import argparse
import json
import os
import shutil
import struct
import sys
from pathlib import Path

CHUNK = 8 << 20
ENTRY = 128
HEADER = 4096


def fail(message):
    raise SystemExit('userdata image refused: ' + message)


def read_header(path):
    data = path.read_bytes()
    if len(data) != HEADER:
        fail('%s is %d bytes; the directory is exactly %d' % (path.name, len(data), HEADER))
    if data[:8] != b'SHNGROOT':
        fail(path.name + ' does not start with SHNGROOT')
    version, count, total = struct.unpack_from('<IIQ', data, 8)
    entries = []
    for index in range(count):
        raw = data[24 + index * ENTRY:24 + (index + 1) * ENTRY]
        name, offset, size, block_bytes, entry_version, digest, _pad = struct.unpack(
            '<32sQQII32s40s', raw)
        entries.append(dict(name=name.split(b'\0', 1)[0].decode('ascii', 'replace'),
                            offset=offset, size=size, block_bytes=block_bytes,
                            version=entry_version, sha256=digest.hex()))
    return dict(version=version, entries=entries, image_bytes=total, raw=data)


class SplitStream:
    """A forward-only byte stream that rolls over into numbered part files."""

    def __init__(self, target, part_bytes):
        self.target = target
        self.part_bytes = part_bytes
        self.parts = []
        self.handle = None
        self.written = 0
        self.zeros = bytes(1 << 20)

    def _open(self):
        if self.part_bytes:
            name = '%s.part%02d' % (self.target.name, len(self.parts))
            path = self.target.parent / name
        else:
            path = self.target
        self.parts.append(path)
        self.handle = path.open('wb', buffering=0)

    def write(self, data):
        position = 0
        while position < len(data):
            if self.handle is None:
                self._open()
            room = self.part_bytes - self.handle.tell() if self.part_bytes else len(data)
            take = min(room, len(data) - position)
            self.handle.write(data[position:position + take])
            self.handle.flush()
            position += take
            self.written += take
            if self.part_bytes and self.handle.tell() >= self.part_bytes:
                self.close_part()

    def fill(self, count):
        zero = self.zeros
        while count:
            amount = min(count, len(zero))
            self.write(zero[:amount])
            count -= amount

    def close_part(self):
        if self.handle is not None:
            os.fsync(self.handle.fileno())
            self.handle.close()
            self.handle = None

    def close(self):
        self.close_part()
        return self.parts


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--layout', type=Path, required=True,
                        help='ufs-layout.json written beside the directory')
    parser.add_argument('--header', type=Path, required=True,
                        help='shngroot-ipados18.bin written by prepare-ipados18-ufs-layout.py')
    parser.add_argument('--output', type=Path, required=True,
                        help='image to write; must not exist. With --part-mib this name gets a '
                             '.partNN suffix on every part')
    parser.add_argument('--part-mib', type=int, default=0,
                        help='roll over into a new part every N MiB (0 keeps one file)')
    parser.add_argument('--max-parts', type=int, default=0,
                        help='size the parts so there are at most N of them, never above '
                             '--part-max-mib. A CI run uploads one artifact per part and cannot '
                             'enumerate an unbounded number, so it sets both.')
    parser.add_argument('--part-max-mib', type=int, default=8192,
                        help='ceiling for one part when --max-parts sizes them (default 8192)')
    parser.add_argument('--json-out', type=Path, help='write the build report here')
    parser.add_argument('--allow-fewer-windows', action='store_true',
                        help='copy the windows the layout names even when the directory lists '
                             'more; the default refuses so a partial image cannot be mistaken '
                             'for the whole userdata')
    parser.add_argument('--delete-sources', action='store_true',
                        help='remove each window image once its bytes are in the output. A CI '
                             'runner holds the unpacked windows and the output at once; a local '
                             'build usually keeps the sources for the next run.')
    args = parser.parse_args()
    layout_path = args.layout.expanduser().resolve()
    header_path = args.header.expanduser().resolve()
    output = args.output.expanduser().resolve()
    if args.part_mib < 0 or (args.part_mib and args.part_mib < 1024):
        parser.error('--part-mib must be 0 or at least 1024')
    if args.max_parts < 0 or (args.max_parts and args.max_parts < 1):
        parser.error('--max-parts must be 0 or at least 1')
    if args.part_mib and args.max_parts:
        parser.error('--part-mib and --max-parts pick the split two different ways; use one')
    if not layout_path.is_file():
        fail('layout not found: ' + str(layout_path))
    layout = json.loads(layout_path.read_text())
    if not header_path.is_file():
        fail('directory not found: ' + str(header_path))
    header = read_header(header_path)
    windows = layout.get('windows') or []
    if not windows:
        fail('the layout records no windows')
    # The image has to be exactly what the directory describes. A layout built
    # for another userland would otherwise produce an image whose directory
    # points at the wrong offsets, and only the device would notice.
    if header['image_bytes'] != layout.get('image_bytes'):
        fail('the directory ends at %d bytes but the layout says %d'
             % (header['image_bytes'], layout.get('image_bytes')))
    if header['version'] != 1:
        fail('directory version %d is not the one this writer implements' % header['version'])
    by_name = {entry['name']: entry for entry in header['entries']}
    if len(by_name) != len(header['entries']):
        fail('the directory repeats a window name')
    if len(header['entries']) != len(windows) and not args.allow_fewer_windows:
        fail('the directory holds %d windows and the layout %d; pass --allow-fewer-windows '
             'only if a partial image is what you want'
             % (len(header['entries']), len(windows)))
    sources = []
    for window in windows:
        entry = by_name.get(window['name'])
        if entry is None:
            fail('the layout has window %r and the directory does not' % window['name'])
        if (entry['offset'], entry['size']) != (window['offset'], window['size']):
            fail('window %s is at %d+%d in the layout and %d+%d in the directory'
                 % (window['name'], window['offset'], window['size'],
                    entry['offset'], entry['size']))
        if entry['sha256'] != window.get('sha256'):
            fail('window %s carries a different SHA-256 in the layout and the directory'
                 % window['name'])
        source = Path(window['image'])
        if not source.is_file():
            fail('window %s names a missing image: %s' % (window['name'], source))
        if source.stat().st_size != window['size']:
            fail('window %s is %d bytes but the layout records %d'
                 % (window['name'], source.stat().st_size, window['size']))
        sources.append((window['name'], source, window['offset'], window['size']))
    total = header['image_bytes']
    part_mib = args.part_mib
    if args.max_parts and not part_mib:
        needed = -(-total // args.max_parts)
        part_mib = -(-needed // (1 << 20))
        if part_mib > args.part_max_mib:
            fail('the image is %d bytes, which does not fit in %d parts of at most %d MiB '
                 '(need %d MiB each). Supply a smaller userland, or split the image yourself '
                 'after the run.'
                 % (total, args.max_parts, args.part_max_mib, part_mib))
    ordered = sorted(sources, key=lambda item: item[2])
    previous = None
    for name, _path, offset, size in ordered:
        if offset < HEADER:
            fail('window %s starts at %d, inside the 4 KiB directory' % (name, offset))
        if previous is not None and offset < previous[1]:
            fail('windows %s and %s overlap' % (previous[0], name))
        previous = (name, offset + size)
    if previous is None:
        fail('the layout records no windows to write')
    if previous[1] > total:
        fail('the last window ends at %d, past the recorded image length %d'
             % (previous[1], total))
    if output.exists():
        fail('refusing to overwrite ' + str(output))
    if (output.parent / ('%s.part00' % output.name)).exists():
        fail('refusing to overwrite ' + str(output.parent / ('%s.part00' % output.name)))
    output.parent.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(output.parent).free
    # The output grows one window at a time. With --delete-sources each source
    # leaves right after it is copied, so the run only holds the largest single
    # window on top of what it has already written.
    reclaimable = sum(window['size'] for window in windows) if args.delete_sources else 0
    if free + reclaimable < total + (1 << 30):
        fail('%s has %d bytes free%s; this image needs %d plus headroom'
             % (output.parent, free,
                ' and the sources free %d more' % reclaimable if args.delete_sources else '',
                total))
    stream = SplitStream(output, part_mib << 20 if part_mib else 0)
    try:
        stream.write(header['raw'])
        for name, source, offset, size in ordered:
            gap = offset - stream.written
            if gap < 0:
                raise RuntimeError('window %s would overwrite bytes at %d' % (name, offset))
            if gap:
                print('%s: %d zero bytes to %#x' % (name, gap, offset), flush=True)
                stream.fill(gap)
            print('%s: %d bytes at %#x' % (name, size, offset), flush=True)
            copied = 0
            with source.open('rb', buffering=0) as handle:
                while copied < size:
                    block = handle.read(min(CHUNK, size - copied))
                    if not block:
                        raise RuntimeError('short read from %s at %d' % (source, copied))
                    stream.write(block)
                    copied += len(block)
            if args.delete_sources:
                source.unlink()
                print('%s: source removed' % name, flush=True)
        if stream.written < total:
            print('tail: %d zero bytes to %d' % (total - stream.written, total), flush=True)
            stream.fill(total - stream.written)
    finally:
        parts = stream.close()
    if stream.written != total:
        fail('wrote %d bytes, expected %d' % (stream.written, total))
    for part in parts:
        if part.stat().st_size == 0:
            fail('wrote an empty part: ' + str(part))
    report = {
        'parts': [{'path': str(part), 'bytes': part.stat().st_size} for part in parts],
        'image_bytes': total,
        'part_mib': part_mib,
        'partition_capacity_bytes': (layout.get('partition_last_4k', 0) -
                                     layout.get('partition_first_4k', 0) + 1) * 4096,
        'windows': [{'name': name, 'offset': offset, 'bytes': size}
                    for name, _source, offset, size in ordered],
        'directory_bytes': HEADER,
        'write_target': layout.get('target'),
        'sources_removed': bool(args.delete_sources),
        'image_verification': 'not performed: windows copied verbatim, directory SHA-256 '
                              'values checked against the layout only',
    }
    if args.json_out:
        out = args.json_out.expanduser().resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
