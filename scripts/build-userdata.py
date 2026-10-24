#!/usr/bin/env python3
"""Assemble the Pong SHNGROOT userdata payload from locally fetched 20G81 disks.

Apple OS and SystemOS components are inputs fetched by fetch-ios16-assets.py.
The SHA-256 fields are part of the native UFS on-disk protocol and are
generated during the one streaming copy pass; there is no second image read,
source checksum comparison, image audit, or readback step.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

DEVICE = Path(__file__).resolve().parents[1]
MIB = 1 << 20
CHUNK = 8 << 20
FIRST_LBA = 0x413248
LAST_LBA = 0x75A40EA
USERDATA_BYTES = (LAST_LBA - FIRST_LBA + 1) * 4096


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component-dir', type=Path,
        help='default: sibling UniXNU/out/ipsw/20G81')
    parser.add_argument('--os-disk', type=Path, help='override the fetched OS disk')
    parser.add_argument('--systemos-disk', type=Path, help='override the fetched SystemOS disk')
    parser.add_argument('--output', type=Path, default=DEVICE / 'out/pong-userdata-root/pong-userdata-root.img')
    args = parser.parse_args()

    unixnu_root = Path(os.environ.get('UNIXNU_ROOT', DEVICE.parent / 'UniXNU')).resolve()
    components = args.component_dir or unixnu_root / 'out/ipsw/20G81'
    disks = [
        ('OS', args.os_disk or components / '096-57366-083.disk.cdr'),
        ('SystemOS', args.systemos_disk or components / '096-56782-083.dmg'),
    ]
    layout = []
    cursor = MIB
    for name, path in disks:
        if not path.is_file():
            raise SystemExit(f'missing fetched {name} component: {path}; run scripts/fetch-ios16-assets.py')
        size = path.stat().st_size
        if size < 512 or size % 512:
            raise SystemExit(f'{name} size must be a nonzero multiple of 512 bytes')
        layout.append((name, cursor, size, path))
        cursor += ((size + MIB - 1) // MIB) * MIB
    if cursor > USERDATA_BYTES:
        raise SystemExit(f'root payload {cursor} exceeds Pong userdata capacity {USERDATA_BYTES}')
    if args.output.exists():
        raise SystemExit(f'refusing to overwrite {args.output}')
    args.output.parent.mkdir(parents=True, exist_ok=True)

    temporary = args.output.with_name(args.output.name + '.partial')
    entries = []
    with temporary.open('wb') as output:
        output.truncate(cursor)
        for name, offset, size, source in layout:
            digest = hashlib.sha256()
            output.seek(offset)
            copied = 0
            with source.open('rb', buffering=0) as input_file:
                while copied < size:
                    block = input_file.read(min(CHUNK, size - copied))
                    if not block:
                        raise RuntimeError(f'short input while copying {name}')
                    output.write(block)
                    digest.update(block)
                    copied += len(block)
            entries.append((name, offset, size, digest.digest()))
            print(f'{name}: copied {size} bytes at offset {offset:#x}', flush=True)

        header = bytearray(4096)
        header[:8] = b'SHNGROOT'
        struct.pack_into('<IIQ', header, 8, 1, len(entries), cursor)
        for index, (name, offset, size, digest) in enumerate(entries):
            struct.pack_into('<32sQQII32s40s', header, 24 + index * 128,
                name.encode(), offset, size, 512, 1, digest, bytes(40))
        header[4064:] = hashlib.sha256(header[:4064]).digest()
        output.seek(0)
        output.write(header)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, args.output)

    report = {
        'image': str(args.output),
        'image_bytes': cursor,
        'userdata': {'first_lba': FIRST_LBA, 'last_lba': LAST_LBA,
                     'block_bytes': 4096, 'capacity_bytes': USERDATA_BYTES},
        'windows': [{'name': name, 'offset': offset, 'bytes': size}
                    for name, offset, size, _ in layout],
        # Report-only reference to the Pong userdata partition. Do not flash this path.
        'write_target': '/dev/block/sda13',
    }
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
