#!/usr/bin/env python3
"""Check the generated FV header, required FFS modules and ARM64 PE images."""
import hashlib
import json
from pathlib import Path
import struct
import uuid
import argparse

import pefile

workspace = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir', type=Path)
args = parser.parse_args()
root = workspace / 'Mu-Silicium/Build/pongPkg/RELEASE_CLANGPDB'
modules = {
    'UniXNU': '9b665204-a133-47a2-9396-9e2b370a4e25',
    'UniXNUDxe': '29b950cf-586d-49ea-9d71-f88645ccdaf6',
    'DarwinBoot': '1b64af87-8415-4b57-9b16-680d70913b9f',
    'PongBootMemoryDxe': '7b27b767-94a4-4a47-99dd-281f7c604faa',
}
fv = (root / 'FV/FVMAIN.Fv').read_bytes()
assert fv[40:44] == b'_FVH'
length = struct.unpack_from('<H', fv, 48)[0]
assert sum(struct.unpack('<' + 'H' * (length // 2), fv[:length])) & 65535 == 0
files = {}
at = (length + 7) & ~7
while at + 24 <= len(fv) and fv[at:at + 24] != b'\xff' * 24:
    size = int.from_bytes(fv[at + 20:at + 23], 'little')
    if size == 0xffffff:
        size = struct.unpack_from('<Q', fv, at + 24)[0]
    assert size >= 24 and at + size <= len(fv)
    files[str(uuid.UUID(bytes_le=fv[at:at + 16]))] = at
    at = (at + size + 7) & ~7
result = {}
for name, guid in modules.items():
    raw = (root / 'AARCH64' / (name + '.efi')).read_bytes()
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0xaa64
    assert pe.OPTIONAL_HEADER.Subsystem == (10 if name == 'UniXNU' else 11)
    assert guid in files
    result[name] = {
        'machine': 'ARM64', 'bytes': len(raw),
        'sha256': hashlib.sha256(raw).hexdigest(), 'fv_offset': hex(files[guid]),
    }
report = {'fv_header_checksum_valid': True, 'fv_files': len(files),
          'modules': result, 'hardware_tested': False}
out = (args.output_dir or workspace / 'UniXNU/out/pong/boot') / 'firmware-verification.json'
out.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
