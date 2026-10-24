#!/usr/bin/env python3
"""Pack Pong's v4 boot image and verify its contents without touching a device.

UniXNU's current bootimg.verify assumes the legacy header layout. Use the same
verified shim compiler and Mu-Silicium mkbootimg with a Pong-specific v4 reader.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'UniXNU/scripts'))
from unixnu.bootimg import compile_shim


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify(data, combined, dtb, ramdisk):
    if len(data) < 4096 or data[:8] != b'ANDROID!':
        raise ValueError('Missing Android boot header')
    kernel_size, ramdisk_size, _, header_size = struct.unpack_from('<4I', data, 8)
    if header_size != 1584 or struct.unpack_from('<I', data, 40)[0] != 4:
        raise ValueError('Expected Pong boot header v4')
    if ramdisk_size != len(ramdisk):
        raise ValueError('Ramdisk size differs')
    kernel = data[4096:4096 + kernel_size]
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    if stream.decompress(kernel) != combined or not stream.eof or stream.unused_data != dtb:
        raise ValueError('Shim, firmware or appended DTB differs')
    ramdisk_start = 4096 + ((kernel_size + 4095) // 4096) * 4096
    if data[ramdisk_start:ramdisk_start + ramdisk_size] != ramdisk:
        raise ValueError('Ramdisk content differs')
    signature_start = ramdisk_start + ((ramdisk_size + 4095) // 4096) * 4096
    signature_size = struct.unpack_from('<I', data, 1580)[0]
    if signature_size != 4096 or len(data) != signature_start + signature_size:
        raise ValueError('Unexpected v4 signature area or image size')
    if any(data[signature_start:]):
        raise ValueError('Expected the upstream unsigned, zero-filled signature area')
    return {'header_version': 4, 'page_size': 4096,
            'kernel_bytes': kernel_size, 'ramdisk_bytes': ramdisk_size,
            'gki_signature': 'unsigned zero-filled upstream placeholder'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ramdisk', type=Path, help='Optional FAT payload; empty for UEFI bring-up')
    parser.add_argument('--output-dir', type=Path, help='Keep staged bring-up images in separate directories')
    parser.add_argument('--fd', type=Path, help='Use a saved firmware volume for controlled comparisons')
    args = parser.parse_args()
    uefi = ROOT / 'Mu-Silicium'
    config = tomllib.loads((uefi / 'Resources/Configs/pong.toml').read_text())
    base, size = config['uefi_fd']['base'], config['uefi_fd']['size']
    fd = args.fd or uefi / 'Build/pongPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd'
    firmware = fd.read_bytes()
    if len(firmware) != size:
        raise ValueError('Firmware size differs from the Pong configuration')
    dtb = (uefi / 'Resources/DTBs/pong.dtb').read_bytes()
    if dtb[:4] != b'\xd0\x0d\xfe\xed' or struct.unpack_from('>I', dtb, 4)[0] != len(dtb):
        raise ValueError('Invalid Pong DTB')
    out = args.output_dir or ROOT / 'UniXNU/out/pong/boot'
    out.mkdir(parents=True, exist_ok=True)
    image = out / 'pong-unixnu-uefi.img'
    ramdisk = args.ramdisk.read_bytes() if args.ramdisk else b''
    with tempfile.TemporaryDirectory(prefix='pong-bootimg-') as name:
        scratch = Path(name)
        shim = compile_shim(uefi, base, size, True, scratch)
        combined = shim + firmware
        kernel = gzip.compress(combined, mtime=0) + dtb
        (scratch / 'kernel').write_bytes(kernel)
        (scratch / 'ramdisk').write_bytes(ramdisk)
        subprocess.run([sys.executable, str(uefi / 'Resources/Scripts/mkbootimg.py'),
                        '--header_version', '4', '--kernel', str(scratch / 'kernel'),
                        '--ramdisk', str(scratch / 'ramdisk'), '-o', str(image)], check=True)
        data = image.read_bytes()
        report = verify(data, combined, dtb, ramdisk)
        # Ensure the verifier rejects changed payloads and truncated images.
        bad = bytearray(data)
        bad[4096 + 20] ^= 1
        for candidate in (bytes(bad), data[:-1]):
            try:
                verify(candidate, combined, dtb, ramdisk)
            except (ValueError, zlib.error):
                continue
            raise ValueError('Corruption control was incorrectly accepted')
    shutil.copyfile(fd, out / 'SILICIUM_UEFI.fd')
    report.update(image_bytes=len(data), image_sha256=digest(data),
                  fd_bytes=len(firmware), fd_sha256=digest(firmware),
                  dtb_sha256=digest(dtb), shim_bytes=len(shim), fd_base=hex(base),
                  hardware_tested=False, xnu_kernel_included=False)
    (out / 'boot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    print(image)


if __name__ == '__main__':
    main()
