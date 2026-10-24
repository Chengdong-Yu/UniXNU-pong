#!/usr/bin/env python3
"""Read-only decmpfs inventory of a mounted Apple restore System volume on macOS."""
import argparse
from collections import Counter
import ctypes
import hashlib
import json
import os
from pathlib import Path
import stat
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--volume', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.volume.resolve()
    if not os.statvfs(root).f_flag & os.ST_RDONLY:
        raise ValueError('Audit requires a read-only mounted source volume')
    libc = ctypes.CDLL(None, use_errno=True)
    getxattr = libc.getxattr
    getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                        ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
    getxattr.restype = ctypes.c_ssize_t
    records, errors, types = [], [], Counter()
    files = 0
    for directory, dirs, names in os.walk(root, followlinks=False,
                                         onerror=lambda e: errors.append(str(e))):
        for name in names:
            path = Path(directory) / name
            try:
                info = path.lstat()
                if not stat.S_ISREG(info.st_mode):
                    continue
                files += 1
                if not info.st_flags & 0x20:  # UF_COMPRESSED
                    continue
                header = ctypes.create_string_buffer(16)
                n = getxattr(os.fsencode(path), b'com.apple.decmpfs', header, 16, 0, 0x20)
                # Inline compressed streams exceed 16 bytes: read the complete
                # attribute when getxattr reports ERANGE, preserving its header.
                if n < 0:
                    size = getxattr(os.fsencode(path), b'com.apple.decmpfs', None, 0, 0, 0x20)
                    if size < 16 or size > 1024 * 1024:
                        raise ValueError(f'invalid decmpfs xattr size {size}, errno={ctypes.get_errno()}')
                    header = ctypes.create_string_buffer(size)
                    n = getxattr(os.fsencode(path), b'com.apple.decmpfs', header, size, 0, 0x20)
                if n < 16:
                    raise ValueError(f'cannot read decmpfs header: {n}')
                magic, kind, logical = struct.unpack_from('<IIQ', header.raw)
                if magic != 0x636d7066 or logical != info.st_size:
                    raise ValueError('decmpfs header magic/logical size mismatch')
                types[kind] += 1
                records.append({'path': '/' + str(path.relative_to(root)),
                                'type': kind, 'logical_bytes': logical})
            except (OSError, ValueError) as error:
                errors.append({'path': str(path), 'error': str(error)})
    # Types advertised by the genuine 20G81 AppleFSCompression personality.
    advertised = {3, 4, 7, 8, 9, 10, 11, 12}
    critical = []
    for relative in ['sbin/launchd', 'usr/lib/dyld', 'usr/bin/launchctl',
                     'usr/libexec/xpcproxy', 'usr/libexec/amfid', 'usr/libexec/logd']:
        path = root / relative
        if not path.is_file():
            critical.append({'path': '/' + relative, 'exists': False})
            continue
        data = path.read_bytes()  # Host filesystem exercises real decompression.
        if len(data) < 32 or data[:4] != bytes.fromhex('cffaedfe'):
            raise ValueError('Expected ARM64 Mach-O: ' + relative)
        cpu, subtype = struct.unpack_from('<II', data, 4)
        critical.append({'path': '/' + relative, 'exists': True,
                         'cpu': hex(cpu), 'subtype': hex(subtype),
                         'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                         'compression_type': next((e['type'] for e in records
                             if e['path'] == '/' + relative), None)})
    result = {'volume': str(root), 'read_only': True, 'regular_files': files,
              'compressed_files': len(records), 'types': dict(sorted(types.items())),
              'advertised_types': sorted(advertised),
              'unsupported_types': sorted(set(types) - advertised),
              'critical_files': critical, 'errors': errors, 'files': records,
              'runtime_qualified_on_phone': False}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('files', 'critical_files')}))
    if errors or result['unsupported_types']:
        raise SystemExit('Incomplete inventory or unsupported compression types; inspect report')


if __name__ == '__main__':
    main()
