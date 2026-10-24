#!/usr/bin/env python3
"""Exercise the real DarwinBoot handoff with an own MMU-off diagnostic kernel."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'UniXNU/out/pong/ios16/window-probe'
BASE = 0xfffffff007004000


def run(*cmd):
    subprocess.run([str(x) for x in cmd], check=True)


def main():
    stage = OUT / 'stage'
    payload = stage / 'EFI/UniXNU'
    payload.mkdir(parents=True, exist_ok=True)
    report = json.loads((OUT.parent / 'returned-kernel-audit.json').read_text())
    segments = [s for s in report['segments'] if int(s['vmsize'], 0)]
    base = min(int(s['va'], 0) for s in segments)
    span = max(int(s['va'], 0) + int(s['vmsize'], 0) for s in segments) - base
    if base != BASE or span % 4096 or not 0x4000 < span < 0x10000000:
        raise ValueError('Unexpected returned XNU geometry')
    includes = ROOT / 'UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver'
    tools = ROOT / 'UniXNU-pong/probes'
    for source, target in [('window-entry.S', 'entry.o'), ('window-probe.c', 'probe.o')]:
        run('clang', '--target=aarch64-none-elf', '-march=armv8-a', '-mgeneral-regs-only', '-mstrict-align',
            '-ffreestanding', '-fno-stack-protector', '-fno-builtin', '-Os', '-Wall', '-Wextra', '-Werror',
            '-I', includes, f'-DPROBE_KERNEL_SPAN={span}', '-c', tools / source, '-o', OUT / target)
    link = OUT / 'probe.ld'
    link.write_text(f'ENTRY(_start)\nSECTIONS {{ . = {BASE + 0x1000:#x}; '
                    '.text : { *(.text.entry) *(.text*) *(.rodata*) } '
                    '.data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n')
    run('ld.lld', '-T', link, OUT / 'entry.o', OUT / 'probe.o', '-o', OUT / 'probe.elf')
    run('llvm-objcopy', '-O', 'binary', OUT / 'probe.elf', OUT / 'probe.bin')
    code = (OUT / 'probe.bin').read_bytes()
    if len(code) > 0x3000:
        raise ValueError('Probe code too large')
    text_size = (0x1000 + len(code) + 4095) & ~4095

    def segment(name, va, size, offset, length, protection):
        return struct.pack('<II16sQQQQiiII', 0x19, 72, name, va, size,
                           offset, length, protection, protection, 0, 0)

    state = bytearray(272)
    struct.pack_into('<Q', state, 256, BASE + 0x1000)
    commands = (segment(b'__TEXT', BASE, text_size, 0, text_size, 5) +
                segment(b'__DATA', BASE + span - 4096, 4096, 0, 0, 3) +
                struct.pack('<4I', 5, 288, 6, 68) + state)
    image = bytearray(text_size)
    image[:32] = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 3, len(commands), 0, 0)
    image[32:32 + len(commands)] = commands
    image[4096:4096 + len(code)] = code
    (payload / 'window.macho').write_bytes(image)
    run('python3', ROOT / 'UniXNU-pong/scripts/make-pong-adt.py', '--dram-size', '0x300000000',
        '--window-size', '0x10000000', '--output', payload / 'window.macho.adt')
    (payload / 'unixnu.toml').write_text('''version = 1
[boot]
timeout = 3
default = "window"
[kits]
autoload = false
[[entry]]
id = "window"
title = "Pong DarwinBoot 256 MiB handoff test"
type = "xnu"
path = "window.macho"
args = "pong.window-probe=1"
''')
    fat = OUT / 'payload-fat.img'
    fat.unlink(missing_ok=True)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    subprocess.run(['mformat', '-C', '-T', '16384', '-v', 'PONGWINDOW', '-i', str(fat), '::'], check=True, env=env)
    subprocess.run(['mcopy', '-i', str(fat), '-s', str(stage / 'EFI'), '::/'], check=True, env=env)
    raw = fat.read_bytes()
    header = bytearray(4096)
    struct.pack_into('<8sIIQII', header, 0, b'PONGFAT1', 1, 4096, len(raw), zlib.crc32(raw), 0)
    (OUT / 'payload.ramdisk').write_bytes(header + raw)
    info = {'kind': 'own diagnostic kernel, not XNU', 'baseline_kernel_span': hex(span),
            'kernel_bytes': len(image), 'window_bytes': 0x10000000,
            'kernel_sha256': hashlib.sha256(image).hexdigest(), 'hardware_result': 'not tested'}
    (OUT / 'probe.json').write_text(json.dumps(info, indent=2) + '\n')
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
