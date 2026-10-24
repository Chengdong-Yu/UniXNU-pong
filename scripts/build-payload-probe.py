#!/usr/bin/env python3
"""Create an 8 MiB FAT EFI probe and its CRC-checked Pong initrd envelope."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[2]
out = ROOT / 'UniXNU/out/pong/payload-probe'
stage = out / 'stage'
efi = stage / 'EFI/UniXNU'
efi.mkdir(parents=True, exist_ok=True)
binary = ROOT / 'Mu-Silicium/Build/pongPkg/RELEASE_CLANGPDB/AARCH64/PongPayloadProbe.efi'
shutil.copyfile(binary, efi / 'PongPayloadProbe.efi')
(stage / 'PONG-PROBE.TXT').write_bytes(b'PONG-INITRD-PROBE-20261020')
(efi / 'unixnu.toml').write_text('''version = 1
[boot]
timeout = 3
default = "probe"
[kits]
autoload = false
[[entry]]
id = "probe"
title = "Pong initrd FAT test (not iPadOS)"
type = "efi"
path = "PongPayloadProbe.efi"
''')
for path in [stage, *stage.rglob('*')]:
    os.utime(path, (315532800, 315532800))
fat = out / 'payload-fat.img'
fat.unlink(missing_ok=True)
env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
subprocess.run(['mformat', '-C', '-T', '16384', '-v', 'PONGPROBE', '-N', '504f4e47', '-i', str(fat), '::'], check=True, env=env)
for entry in sorted(stage.iterdir()):
    subprocess.run(['mcopy', '-i', str(fat), '-s', '-Q', '-m', str(entry), '::/'], check=True, env=env)
raw = fat.read_bytes()
if len(raw) != 8 * 1024 * 1024 or raw[510:512] != b'\x55\xaa':
    raise ValueError('Unexpected FAT image')
subprocess.run(['mtype', '-i', str(fat), '::/PONG-PROBE.TXT'], check=True, env=env, stdout=subprocess.PIPE)
header = bytearray(4096)
struct.pack_into('<8sIIQII', header, 0, b'PONGFAT1', 1, 4096, len(raw), zlib.crc32(raw), 0)
container = out / 'payload.ramdisk'
container.write_bytes(header + raw)
report = {'fat_bytes': len(raw), 'ramdisk_bytes': container.stat().st_size,
          'fat_sha256': hashlib.sha256(raw).hexdigest(),
          'ramdisk_sha256': hashlib.sha256(container.read_bytes()).hexdigest(),
          'includes_xnu': False, 'hardware_tested': False}
(out / 'payload.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
