#!/usr/bin/env python3
"""Build the MMU-off probe with strict alignment; retain the qualified firmware.

The template Android v4 kernel, DTB, firmware and signature bytes are preserved.
Only the diagnostic FAT ramdisk changes. This script never contacts a phone.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
DEVICE = Path(__file__).resolve().parents[1]
BASE = 0xfffffff007004000


def tool(*names):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit('Missing tool: ' + ' / '.join(names))


def run(*command, **kwargs):
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build/window-probe-strict-align')
    parser.add_argument('--template', type=Path, default=ROOT / 'images/pong-window-vectors.img')
    parser.add_argument('--audit', type=Path, default=ROOT / 'evidence/returned-kernel-audit.json')
    parser.add_argument('--darwin22-loader', type=Path, help='Test the new Darwin 22 loader and boot_args layout')
    parser.add_argument('--driver-dir', type=Path, default=Path(
        '/Volumes/PongDev/build/UniXNU-XNU-iOS16/pong/tree/drivers/DarwinBoot/DarwinBootPkg/Driver'))
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'pong-window-strict-align.img').exists():
        parser.error('Preserving existing image; choose another --output-dir')
    report = json.loads(args.audit.read_text())
    segments = [s for s in report['segments'] if int(s['vmsize'], 0)]
    span = max(int(s['va'], 0) + int(s['vmsize'], 0) for s in segments) - BASE
    if min(int(s['va'], 0) for s in segments) != BASE or span % 4096 or not 0x4000 < span < 0x10000000:
        raise ValueError('Unexpected kernel geometry')
    clang = tool('clang-mp-18', 'clang')
    linker = tool('ld.lld-mp-18', 'ld.lld')
    objcopy = tool('llvm-objcopy-mp-18', 'llvm-objcopy')
    commands = []
    for source, obj in [('window-entry.S', 'entry.o'), ('window-probe.c', 'probe.o')]:
        command = [clang, '--target=aarch64-none-elf', '-march=armv8-a',
                   '-mgeneral-regs-only', '-mstrict-align', '-ffreestanding',
                   '-fno-stack-protector', '-fno-builtin', '-Os', '-Wall', '-Wextra', '-Werror',
                   '-I', str(args.driver_dir), f'-DPROBE_KERNEL_SPAN={span}',
                   '-DPROBE_MEMSIZE_ACTUAL_OFFSET=' + ('0x2d8' if args.darwin22_loader else '0x478'),
                   '-c', str(DEVICE / 'probes' / source), '-o', str(out / obj)]
        commands.append(command)
        run(*command)
    link = out / 'probe.ld'
    link.write_text(f'ENTRY(_start)\nSECTIONS {{ . = {BASE + 0x1000:#x}; '
                    '.text : { *(.text.entry) *(.text*) *(.rodata*) } '
                    '.data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n')
    run(linker, '-T', link, out / 'entry.o', out / 'probe.o', '-o', out / 'probe.elf')
    run(objcopy, '-O', 'binary', out / 'probe.elf', out / 'probe.bin')
    code = (out / 'probe.bin').read_bytes()
    if len(code) > 0x3000:
        raise ValueError('Probe exceeds the reserved text area')
    text_size = (0x1000 + len(code) + 4095) & ~4095

    def segment(name, va, size, offset, length, protection):
        return struct.pack('<II16sQQQQiiII', 0x19, 72, name, va, size,
                           offset, length, protection, protection, 0, 0)

    state = bytearray(272)
    struct.pack_into('<Q', state, 256, BASE + 0x1000)
    load_commands = (segment(b'__TEXT', BASE, text_size, 0, text_size, 5) +
                     segment(b'__DATA', BASE + span - 4096, 4096, 0, 0, 3) +
                     struct.pack('<4I', 5, 288, 6, 68) + state)
    image = bytearray(text_size)
    image[:32] = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 3, len(load_commands), 0, 0)
    image[32:32 + len(load_commands)] = load_commands
    image[4096:4096 + len(code)] = code
    payload = out / 'stage/EFI/UniXNU'
    payload.mkdir(parents=True, exist_ok=True)
    (payload / 'window.macho').write_bytes(image)
    run(sys.executable, DEVICE / 'scripts/make-pong-adt.py', '--dram-size', '0x300000000',
        '--window-size', '0x10000000', '--output', payload / 'window.macho.adt')
    (payload / 'unixnu.toml').write_text('version = 1\n[boot]\ntimeout = 3\n'
        'default = "window"\n[kits]\nautoload = false\n[[entry]]\nid = "window"\n'
        'title = "Pong strict-alignment memory probe"\ntype = "xnu"\n'
        'path = "window.macho"\nargs = "pong.window-probe=1"\n')
    if args.darwin22_loader:
        shutil.copyfile(args.darwin22_loader, payload / 'DarwinBoot22.efi')
        config = payload / 'unixnu.toml'
        config.write_text(config.read_text().replace('[[entry]]',
                          '[[driver]]\npath = "DarwinBoot22.efi"\nconnect = false\n[[entry]]'))
    fat = out / 'payload-fat.img'
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    run(tool('mformat'), '-C', '-T', '16384', '-v', 'PONGWINDOW', '-i', fat, '::', env=env)
    run(tool('mcopy'), '-i', fat, '-s', out / 'stage/EFI', '::/', env=env)
    raw = fat.read_bytes()
    header = bytearray(4096)
    struct.pack_into('<8sIIQII', header, 0, b'PONGFAT1', 1, 4096, len(raw), zlib.crc32(raw), 0)
    ramdisk = bytes(header) + raw
    (out / 'payload.ramdisk').write_bytes(ramdisk)
    template = args.template.read_bytes()
    if template[:8] != b'ANDROID!' or struct.unpack_from('<I', template, 40)[0] != 4:
        raise ValueError('Expected an Android v4 template')
    kernel_size, old_ramdisk_size = struct.unpack_from('<2I', template, 8)
    page = lambda size: (size + 4095) & ~4095
    ramdisk_at = 4096 + page(kernel_size)
    signature_at = ramdisk_at + page(old_ramdisk_size)
    signature_size = struct.unpack_from('<I', template, 1580)[0]
    if signature_size != 4096 or signature_at + signature_size != len(template) or any(template[signature_at:]):
        raise ValueError('Unexpected template signature')
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    firmware = stream.decompress(template[4096:4096 + kernel_size])
    if not stream.eof or not stream.unused_data.startswith(b'\xd0\x0d\xfe\xed'):
        raise ValueError('Template lacks compressed firmware and appended FDT')
    updated = bytearray(template[:ramdisk_at])
    struct.pack_into('<I', updated, 12, len(ramdisk))
    updated += ramdisk + bytes(page(len(ramdisk)) - len(ramdisk)) + template[signature_at:]
    boot = out / 'pong-window-strict-align.img'
    boot.write_bytes(updated)
    assert updated[4096:ramdisk_at] == template[4096:ramdisk_at]
    (out / 'build.json').write_text(json.dumps({
        'contains_xnu': False, 'hardware_tested': False, 'partition_writes': False,
        'strict_alignment': True, 'baseline_span': hex(span), 'commands': commands,
        'boot_args_abi': 'Darwin 22' if args.darwin22_loader else 'Darwin 23 (saved firmware)',
        'loader_sha256': hashlib.sha256(args.darwin22_loader.read_bytes()).hexdigest() if args.darwin22_loader else None,
        'template_sha256': hashlib.sha256(template).hexdigest(),
        'image_sha256': hashlib.sha256(updated).hexdigest(), 'image_bytes': len(updated),
        'kernel_sha256': hashlib.sha256(image).hexdigest(),
        'firmware_and_dtb_unchanged': True, 'firmware_bytes_including_shim': len(firmware),
    }, indent=2) + '\n')
    print(boot)


if __name__ == '__main__':
    main()
