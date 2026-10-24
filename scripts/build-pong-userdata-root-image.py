#!/usr/bin/env python3
"""Build Pong userdata contents from the pinned iOS 16.6.1 20G81 restore disks.

The first 4 KiB is a SHNGROOT v1 manifest. The unchanged OS and SystemOS
whole-disk images follow at 1 MiB boundaries. NativeUfs publishes the OS
window as a 512-byte logical disk while keeping the underlying UFS geometry
at 4096 bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import uuid
import zlib

DEVICE = Path(__file__).resolve().parents[1]
ROOT = DEVICE.parent
APPLE = ROOT / 'apple/20G81'
OUT = ROOT / 'build/pong-userdata-root'
MIB = 1024 * 1024
CHUNK = 8 * MIB
USERDATA_FIRST_LBA = 0x413248
USERDATA_LAST_LBA = 0x75A40EA
USERDATA_BYTES = (USERDATA_LAST_LBA - USERDATA_FIRST_LBA + 1) * 4096
WINDOWS = (
    ('OS', '096-57366-083.disk.cdr',
     '4cfd18d02a3e31b01d332817650248ca3c03adfebb8254e9e254b3d8dbb4f10ed322f3c80a8fe174e9f3af840254ed07'),
    ('SystemOS', '096-56782-083.dmg',
     'b58cc1d345afceccd4af13c57e1cd549c4414030d8540372e2e549127cb35b84078157440574f74d44cec7e1a4c343b0'),
)


def digest_file(path, algorithm='sha256'):
    digest = hashlib.new(algorithm)
    with path.open('rb') as handle:
        for data in iter(lambda: handle.read(CHUNK), b''):
            digest.update(data)
    return digest.hexdigest()


def validate_raw(name, path):
    size = path.stat().st_size
    with path.open('rb') as disk:
        if name == 'SystemOS':
            block = disk.read(4096)
            if block[32:36] != b'NXSB' or struct.unpack_from('<I', block, 36)[0] != 4096:
                raise SystemExit('SystemOS is not a 4096-byte APFS container')
            if struct.unpack_from('<Q', block, 40)[0] * 4096 != size:
                raise SystemExit('SystemOS APFS size differs from raw file')
            return
        for lba in (1, size // 512 - 1):
            disk.seek(lba * 512)
            header = bytearray(disk.read(512))
            if header[:8] != b'EFI PART':
                raise SystemExit(f'OS GPT header missing at LBA {lba}')
            header_bytes, recorded = struct.unpack_from('<II', header, 12)
            if not 92 <= header_bytes <= 512:
                raise SystemExit('OS GPT header size invalid')
            struct.pack_into('<I', header, 16, 0)
            if zlib.crc32(header[:header_bytes]) != recorded:
                raise SystemExit('OS GPT header CRC mismatch')
            if struct.unpack_from('<Q', header, 24)[0] != lba:
                raise SystemExit('OS GPT current LBA mismatch')
            entries_lba, count, stride, crc = struct.unpack_from('<QIII', header, 72)
            if count > 128 or stride != 128:
                raise SystemExit('OS GPT table geometry invalid')
            disk.seek(entries_lba * 512)
            entries = disk.read(count * stride)
            if len(entries) != count * stride or zlib.crc32(entries) != crc:
                raise SystemExit('OS GPT entries CRC mismatch')
            for offset in range(0, len(entries), stride):
                entry = entries[offset:offset + stride]
                if not any(entry[:16]):
                    continue
                first, last = struct.unpack_from('<QQ', entry, 32)
                if first > last or (last + 1) * 512 > size:
                    raise SystemExit('OS GPT partition outside disk')
                if lba == 1:
                    print('OS GPT partition', uuid.UUID(bytes_le=entry[:16]),
                          first, last, uuid.UUID(bytes_le=entry[16:32]), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT / 'pong-userdata-root.img')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--os-disk', type=Path, default=None,
                        help='Pong APFS System disk to package; default is the pinned raw restore disk')
    parser.add_argument('--allow-modified-os-disk', action='store_true',
                        help='accept a local System image whose restore-disk digest changed during APFS repair')
    parser.add_argument('--systemos-disk', type=Path, default=APPLE / '096-56782-083.dmg')
    args = parser.parse_args()
    if args.os_disk is not None and not args.allow_modified_os_disk:
        parser.error('--os-disk requires --allow-modified-os-disk so the digest exception is explicit')
    if args.allow_modified_os_disk and args.os_disk is None:
        parser.error('--allow-modified-os-disk requires --os-disk')
    os_disk = args.os_disk.resolve() if args.os_disk is not None else APPLE / '096-57366-083.disk.cdr'
    systemos_disk = args.systemos_disk.resolve()
    sources = [
        ('OS', os_disk,
         None if args.allow_modified_os_disk else WINDOWS[0][2]),
        ('SystemOS', systemos_disk, WINDOWS[1][2]),
    ]
    layout, cursor = [], MIB
    for name, path, expected in sources:
        if not path.is_file():
            raise SystemExit(f'Missing decoded restore disk: {path}')
        if expected is not None:
            source = APPLE / '096-57366-083.dmg' if name == 'OS' else path
            got = digest_file(source, 'sha384')
            if got != expected:
                raise SystemExit(f'{source.name}: source SHA-384 differs from BuildManifest')
        else:
            print(f'{path.name}: custom modified System disk; raw SHA-256 {digest_file(path)}', flush=True)
        size = path.stat().st_size
        if size < 4096 or size % 512:
            raise SystemExit(f'{path.name}: invalid raw disk geometry')
        validate_raw(name, path)
        layout.append((name, cursor, size))
        cursor += (size + MIB - 1) // MIB * MIB
        print(f'{name}: {size} bytes; source SHA-384 verified', flush=True)
    if cursor > USERDATA_BYTES:
        raise SystemExit(f'Root image {cursor} exceeds Pong userdata {USERDATA_BYTES}')
    print(f'layout: {cursor} bytes ({cursor / 2**30:.2f} GiB) in {USERDATA_BYTES} byte userdata')
    if args.dry_run:
        return

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        raise SystemExit(f'Refusing to overwrite {args.output}')
    entries = []
    with args.output.open('wb') as output:
        output.truncate(cursor)
        for (name, offset, size), (_, path, _) in zip(layout, sources):
            digest = hashlib.sha256()
            output.seek(offset)
            done = 0
            with path.open('rb', buffering=0) as source:
                while done < size:
                    data = source.read(min(CHUNK, size - done))
                    if not data:
                        raise RuntimeError(f'Short read from {path} at {done}')
                    output.write(data)
                    digest.update(data)
                    done += len(data)
            entries.append((name, offset, size, digest.digest()))
            print(f'{name}: copied at {offset:#x}; raw SHA-256 {digest.hexdigest()}')
        header = bytearray(4096)
        header[:8] = b'SHNGROOT'
        struct.pack_into('<IIQ', header, 8, 1, len(entries), cursor)
        for index, (name, offset, size, digest) in enumerate(entries):
            struct.pack_into('<32sQQII32s40s', header, 24 + index * 128,
                             name.encode(), offset, size, 512, 1, digest, bytes(40))
        header[4064:] = hashlib.sha256(header[:4064]).digest()
        output.seek(0)
        output.write(header)
    report = {
        'image': str(args.output), 'image_bytes': cursor,
        'image_sha256': digest_file(args.output),
        'system_volume_restore_digest_verified': not args.allow_modified_os_disk,
        'modified_os_disk_sha256': digest_file(os_disk) if args.allow_modified_os_disk else None,
        'userdata': {'first_lba': USERDATA_FIRST_LBA, 'last_lba': USERDATA_LAST_LBA,
                     'logical_block_bytes': 4096, 'bytes': USERDATA_BYTES},
        'windows': [{'name': n, 'offset': o, 'size': s, 'sha256': d.hex()}
                    for n, o, s, d in entries],
        'write_target': '/dev/block/sda13',
    }
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
