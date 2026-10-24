#!/usr/bin/env python3
"""Fetch only the 20G81 IPSW members selected by board.toml, using HTTP ranges.

No IPSW archive is downloaded as a whole and no member checksum is computed.
The extracted restore files are local build inputs and belong under the ignored
UniXNU output directory, never in this repository.
"""
import argparse
import io
import json
import os
from pathlib import Path
import plistlib
import struct
import sys
import tomllib
import urllib.request
import zipfile
import zlib

DEVICE = Path(__file__).resolve().parents[1]
BLOCK = 1 << 20


class RangeFile(io.RawIOBase):
    def __init__(self, url):
        self.url = url
        self.position = 0
        self.blocks = {}
        request = urllib.request.Request(url, method='HEAD')
        with urllib.request.urlopen(request, timeout=60) as response:
            self.size = int(response.headers['Content-Length'])
        request = urllib.request.Request(url, headers={'Range': 'bytes=0-0'})
        with urllib.request.urlopen(request, timeout=60) as response:
            if response.status != 206:
                raise RuntimeError('Apple restore URL does not support HTTP range reads')

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.position

    def seek(self, offset, whence=0):
        if whence == 0:
            self.position = offset
        elif whence == 1:
            self.position += offset
        elif whence == 2:
            self.position = self.size + offset
        else:
            raise ValueError('invalid seek mode')
        return self.position

    def _block(self, index):
        if index not in self.blocks:
            start = index * BLOCK
            end = min(start + BLOCK, self.size) - 1
            request = urllib.request.Request(self.url, headers={'Range': f'bytes={start}-{end}'})
            with urllib.request.urlopen(request, timeout=120) as response:
                if response.status != 206:
                    raise RuntimeError('Apple restore server stopped serving ranges')
                data = response.read()
            if len(data) != end - start + 1:
                raise RuntimeError('short HTTP range response')
            if len(self.blocks) >= 8:
                self.blocks.pop(next(iter(self.blocks)))
            self.blocks[index] = data
        return self.blocks[index]

    def readinto(self, target):
        remaining = min(len(target), self.size - self.position)
        done = 0
        while done < remaining:
            index, inside = divmod(self.position, BLOCK)
            block = self._block(index)
            amount = min(remaining - done, len(block) - inside)
            target[done:done + amount] = block[inside:inside + amount]
            self.position += amount
            done += amount
        return done


def raw_member(archive, info, output):
    """Stream a stored/deflated ZIP member without running ZIP CRC checks."""
    source = archive.fp
    source.seek(info.header_offset)
    header = source.read(30)
    if len(header) != 30 or struct.unpack_from('<I', header)[0] != 0x04034b50:
        raise RuntimeError(f'invalid local ZIP header for {info.filename}')
    flags, method = struct.unpack_from('<HH', header, 6)
    if flags & 1:
        raise RuntimeError(f'encrypted IPSW member is unsupported: {info.filename}')
    name_length, extra_length = struct.unpack_from('<HH', header, 26)
    source.seek(info.header_offset + 30 + name_length + extra_length)
    remaining = info.compress_size
    inflater = zlib.decompressobj(-15) if method == zipfile.ZIP_DEFLATED else None
    written = 0
    while remaining:
        block = source.read(min(BLOCK, remaining))
        if not block:
            raise RuntimeError(f'short ZIP member read: {info.filename}')
        remaining -= len(block)
        if method == zipfile.ZIP_STORED:
            output.write(block)
            written += len(block)
        elif inflater:
            data = inflater.decompress(block)
            output.write(data)
            written += len(data)
        else:
            raise RuntimeError(f'unsupported ZIP compression method {method}: {info.filename}')
    if inflater:
        tail = inflater.flush()
        output.write(tail)
        written += len(tail)
    if written != info.file_size:
        raise RuntimeError(f'extracted size differs from ZIP directory for {info.filename}')


def member_bytes(archive, info):
    buffer = io.BytesIO()
    raw_member(archive, info, buffer)
    return buffer.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ipsw', help='optional local IPSW; otherwise use the board restore URL')
    parser.add_argument('--output-dir', type=Path,
        help='default: sibling UniXNU/out/ipsw/<build>')
    args = parser.parse_args()

    board = tomllib.loads((DEVICE / 'board.toml').read_text())['ipsw']
    unixnu_root = Path(os.environ.get('UNIXNU_ROOT', DEVICE.parent / 'UniXNU')).resolve()
    output = args.output_dir or unixnu_root / 'out/ipsw' / board['build']
    output.mkdir(parents=True, exist_ok=True)
    source = Path(args.ipsw).open('rb') if args.ipsw else RangeFile(board['url'])
    archive = zipfile.ZipFile(source)
    manifest = plistlib.loads(member_bytes(archive, archive.getinfo('BuildManifest.plist')))
    if (manifest['ProductVersion'], manifest['ProductBuildVersion']) != (board['version'], board['build']):
        raise RuntimeError('restore build differs from board.toml')
    if board['product'] not in manifest['SupportedProductTypes']:
        raise RuntimeError('restore product differs from board.toml')
    identity = next(item for item in manifest['BuildIdentities']
        if item['Info']['DeviceClass'] == board['device_class']
        and item['Info']['RestoreBehavior'] == 'Erase'
        and int(item['ApChipID'], 16) == board['chip_id']
        and int(item['ApBoardID'], 16) == board['board_id'])

    report = []
    for component in board['fetch']:
        member_name = identity['Manifest'][component]['Info']['Path']
        info = archive.getinfo(member_name)
        destination = output / Path(member_name).name
        if destination.is_file() and destination.stat().st_size == info.file_size:
            report.append({'component': component, 'file': destination.name, 'bytes': info.file_size, 'cached': True})
            continue
        partial = destination.with_name(destination.name + '.partial')
        with partial.open('wb') as target:
            raw_member(archive, info, target)
        os.replace(partial, destination)
        report.append({'component': component, 'file': destination.name, 'bytes': info.file_size, 'cached': False})
        print(f'{component}: {destination} ({info.file_size} bytes)', flush=True)
    (output / 'fetch-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Fetched {len(report)} restore components into {output}')


if __name__ == '__main__':
    main()
