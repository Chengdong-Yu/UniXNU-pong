#!/usr/bin/env python3
"""Run actual DXE/FAT integration tests in AAVMF, saving complete serial logs."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import argparse
import os

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--firmware', type=Path, help='AAVMF or QEMU edk2-aarch64-code.fd')
args = parser.parse_args()
candidates = [args.firmware, Path(os.environ['PONG_QEMU_FIRMWARE']) if 'PONG_QEMU_FIRMWARE' in os.environ else None,
              Path('/usr/share/qemu-efi-aarch64/QEMU_EFI.fd'),
              Path('/opt/homebrew/share/qemu/edk2-aarch64-code.fd'),
              Path('/usr/local/share/qemu/edk2-aarch64-code.fd')]
firmware = next((p for p in candidates if p is not None and p.is_file()), None)
if firmware is None:
    raise SystemExit('Set PONG_QEMU_FIRMWARE or --firmware to a local ARM64 UEFI image')
bin_dir = ROOT / 'Mu-Silicium/Build/pongPkg/RELEASE_CLANGPDB/AARCH64'
out = ROOT / 'UniXNU/out/pong/payload-probe'
results = []
for name in ('positive', 'negative-crc', 'unmapped-input'):
    negative = name == 'negative-crc'
    with tempfile.TemporaryDirectory(prefix='pong-qemu-') as temporary:
        esp = Path(temporary)
        (esp / 'EFI/BOOT').mkdir(parents=True)
        shutil.copyfile(bin_dir / 'PongInitrdTest.efi', esp / 'EFI/BOOT/BOOTAA64.EFI')
        shutil.copyfile(bin_dir / 'PongBootMemoryDxe.efi', esp / 'PongBootMemoryDxe.efi')
        shutil.copyfile(out / 'payload.ramdisk', esp / 'payload.ramdisk')
        if negative:
            (esp / 'negative.flag').write_bytes(b'CRC refusal control')
        if name == 'unmapped-input':
            (esp / 'unmapped.flag').write_bytes(b'Input pages start read-protected')
        command = ['qemu-system-aarch64', '-machine', 'virt', '-cpu', 'max',
                   '-m', '768M', '-nographic', '-no-reboot', '-nic', 'none',
                   '-bios', str(firmware),
                   '-drive', f'if=virtio,format=raw,file=fat:rw:{esp}']
        try:
            process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            log = process.stdout
        except subprocess.TimeoutExpired as error:
            log = error.stdout or b''
        (out / f'qemu-{name}.log').write_bytes(log)
        expected = b'negative CRC refusal PASS' if negative else b'FAT mount, marker, read-only and bounds PASS'
        passed = expected in log and b'PONG-INITRD-TEST: FAIL' not in log
        results.append({'case': name, 'passed': passed})
        print(f'{name}: {"PASS" if passed else "FAIL"}')
        if not passed:
            print(log.decode(errors='replace')[-2500:])
            raise SystemExit(1)
(out / 'qemu-tests.json').write_text(json.dumps({'tests': results, 'hardware_tested': False}, indent=2) + '\n')
