#!/usr/bin/env python3
"""Finish the packaged payload the way a local run packs it, and write the boot image.

`unixnu release` follows docs/porting/packaging.md: it writes kernel.macho.adt
with the shared converter and puts payload.img in the boot image's ramdisk slot
unchanged. A local run does two things that document does not describe, and this
step repeats them with the local parameters so the artifacts agree:

* The ADT comes from scripts/make-pong-adt-ipados18.py, the generator the local
  chain uses. The shared converter expresses root, chosen, defaults, options,
  product and buttons, but not the properties this board's kernel reads from
  /arm-io (the pong,ufs-* windows NativeUfs validates, the touch and button
  resources, the PMIC metadata). The parameters are the ones a local run passes:
  12 GiB of physical DRAM, a 4 GiB DarwinBoot window, one CPU, the J171 chip ID,
  the development policy and the boot manifest hash.
* The boot image is the container a local run writes: Android boot v4 with an
  empty kernel slot (this device boots the firmware from its own partition),
  the payload wrapped in the PONGFAT1 envelope PongBootMemoryDxe publishes, and
  an empty signature area. The firmware refuses an initrd above 128 MiB, which
  is why board.toml [payload] size_mib is 96.

boot/boot.img itself stays exactly what `release` wrote: it is the document's
artifact, with the raw payload as its ramdisk.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import tomllib
import zlib
from pathlib import Path

HEADER_BYTES = 4096          # PONGFAT1 header, and the boot image's first page
SIGNATURE_BYTES = 4096       # boot v4 signature area, left empty
BOOT_HEADER_V4_SIZE = 1584   # boot image header v4, as mkbootimg writes it
BOOT_ARGS_BYTES = 1536       # BOOT_ARGS_SIZE 512 + BOOT_EXTRA_ARGS_SIZE 1024
MAGIC = b'PONGFAT1'

MANIFEST_FORMAT = 'pong-development-boot-manifest-v1'
BOARD = 'Nothing Phone (2) / Pong / SM8475'


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def envelope(fat):
    """PONGFAT1 header followed by the FAT volume, as PongBootMemoryDxe validates it.

    The firmware scans a bounded initrd for this header, then checks the version,
    the header length, a 512-byte multiple length, the FAT signature, the sector
    count and the CRC32 of the volume.
    """
    if len(fat) % 512 or fat[510:512] != b'\x55\xaa' or struct.unpack_from('<H', fat, 11)[0] != 512:
        raise SystemExit('the payload volume is not a FAT volume with 512-byte sectors')
    sectors16 = struct.unpack_from('<H', fat, 19)[0]
    sectors = sectors16 or struct.unpack_from('<I', fat, 32)[0]
    if sectors != len(fat) // 512:
        raise SystemExit(f'the payload volume declares {sectors} sectors, it holds {len(fat) // 512}')
    header = bytearray(HEADER_BYTES)
    struct.pack_into('<8sIIQII', header, 0, MAGIC, 1, HEADER_BYTES, len(fat), zlib.crc32(fat) & 0xffffffff, 0)
    return bytes(header) + fat


def boot_image(ramdisk):
    """Android boot v4: 4096-byte header page, the ramdisk, an empty signature area.

    The kernel slot stays empty because this device boots the UEFI firmware from
    its own partition; the ramdisk is the whole payload. That is the container
    scripts/build-ipados18-bootimg-local.py builds from its v4 template.
    """
    header = bytearray(HEADER_BYTES)
    header[0:8] = b'ANDROID!'
    struct.pack_into('<4I', header, 8, 0, len(ramdisk), 0, BOOT_HEADER_V4_SIZE)
    struct.pack_into('<I', header, 40, 4)
    struct.pack_into('<I', header, 40 + 4 + BOOT_ARGS_BYTES, 0)  # signature_size
    return bytes(header) + ramdisk + bytes(SIGNATURE_BYTES)


def read_member(volume, member):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    probe = subprocess.run(['mtype', '-i', str(volume), f'::{member}'], env=env, capture_output=True)
    return probe.stdout if probe.returncode == 0 else None


def write_member(volume, member, content):
    """Overwrite or add one file inside the FAT image with mtools, without mounting it."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    with tempfile.NamedTemporaryFile(suffix='-' + Path(member).name, delete=False) as handle:
        handle.write(content)
        scratch = Path(handle.name)
    try:
        subprocess.run(['mcopy', '-o', '-i', str(volume), str(scratch), f'::{member}'],
                       check=True, env=env, capture_output=True)
    finally:
        scratch.unlink(missing_ok=True)
    if read_member(volume, member) != content:
        raise SystemExit(f'{volume}: {member} did not read back as written')


def main():
    device = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir', required=True, type=Path,
                        help='the release configuration directory (out/<device>/<os>/<config>)')
    parser.add_argument('--uefi', type=Path, help='Mu-Silicium checkout, exported to the generator as UNIXNU_UEFI')
    parser.add_argument('--unique-chip-id', required=True, help='platform seed, the value the release used')
    parser.add_argument('--volume-layout', type=Path, help='layout JSON the kernel was configured with')
    parser.add_argument('--hardware-dtb', type=Path,
                        help='compiled board DTS (default: board.toml [bootimg] dtb under the work tree)')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), default=0x300000000,
                        help='physical DRAM the boot chain reports; a local run passes 12 GiB')
    parser.add_argument('--window-size', type=lambda value: int(value, 0), default=0x100000000,
                        help="DarwinBoot's allocation window; a local run passes 4 GiB")
    parser.add_argument('--cpus', type=int, choices=range(1, 9), default=1,
                        help='CPU nodes in the ADT; a local bring-up run passes 1')
    parser.add_argument('--chip-id', default=None, help='BuildIdentity chip ID (default: board.toml [payload.adt])')
    parser.add_argument('--ufs-probe', action='store_true',
                        help='publish the UFS nub; a local bring-up image leaves it out')
    parser.add_argument('--no-development-debug', dest='development_debug', action='store_false',
                        help='leave the DT debugger gate off; a local run turns it on')
    args = parser.parse_args()

    work = args.work_dir.resolve()
    board = tomllib.loads((device / 'board.toml').read_text())
    payload = work / 'payload/payload.img'
    if not payload.is_file():
        raise SystemExit(f'{payload} missing: run scripts/package.sh first')
    dtb = (args.hardware_dtb or work / 'tree' / str(board.get('bootimg', {}).get('dtb', ''))).resolve()
    if not dtb.is_file():
        raise SystemExit(f'{dtb} missing: the firmware preflight step compiles the board DTS')

    layout = json.loads(args.volume_layout.read_text()) if args.volume_layout else {}
    root_uuid = (layout.get('os') or {}).get('uuid')
    chip_id = args.chip_id or str(board.get('payload', {}).get('adt', {}).get('chip_id', '0x8010'))

    spec = board.get('payload', {})
    boot_args = 'keepsyms=1 cpus=%d' % args.cpus
    extra = [item for item in spec.get('kernel_args', []) if item.split('=', 1)[0] not in ('keepsyms', 'cpus')]
    boot_args += ''.join(' ' + item for item in extra)
    # The manifest a local run stages: scripts/build-ipados18-bootimg-local.py
    # writes exactly these keys, and with --skip-image-verification the digests
    # of the kernel and the loader stay null.
    manifest = json.dumps({
        'format': MANIFEST_FORMAT,
        'apple_personalization': False,
        'development_debug': bool(args.development_debug),
        'amfi_only_platform_code': False,
        'system_volume_auth_blob_sha256': None,
        'kernel_sha384': None,
        'loader_sha384': None,
        'board': BOARD,
        'boot_args': boot_args,
        'root_volume_uuid': root_uuid,
        'requested_memory_window_bytes': args.window_size,
    }, sort_keys=True, separators=(',', ':')).encode() + b'\n'
    manifest_file = work / 'payload/pong-boot-manifest.json'
    manifest_file.write_bytes(manifest)

    generated = work / 'payload/kernel.macho.adt.pong'
    command = [sys.executable, str(device / 'scripts/make-pong-adt-ipados18.py'),
               '--cpus', str(args.cpus),
               '--dram-size', hex(args.dram_size),
               '--window-size', hex(args.window_size),
               '--output', str(generated),
               '--unique-chip-id', args.unique_chip_id,
               '--chip-id', chip_id,
               '--boot-manifest', str(manifest_file),
               # The local chain records no image audit for this development image.
               '--skip-image-verification']
    if args.development_debug:
        command.append('--development-debug')
    if args.ufs_probe:
        command.append('--ufs-probe')
    if root_uuid:
        command += ['--root-match-volume-uuid', root_uuid]
    environment = dict(os.environ, PONG_HARDWARE_DTB=str(dtb))
    if args.uefi:
        environment['UNIXNU_UEFI'] = str(args.uefi.resolve())
    subprocess.run(command, cwd=device, check=True, env=environment)
    adt = generated.read_bytes()

    # kernel.macho.adt is what the firmware loads; pong-boot-manifest.json is the
    # unsigned description a local image carries beside it.
    write_member(payload, 'EFI/UniXNU/Payloads/kernel.macho.adt', adt)
    write_member(payload, 'EFI/UniXNU/Payloads/pong-boot-manifest.json', manifest)

    ramdisk = envelope(payload.read_bytes())
    image = boot_image(ramdisk)
    target = work / 'boot/pong-xnu-initial.img'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(image)
    if image[:8] != b'ANDROID!' or struct.unpack_from('<I', image, 40)[0] != 4:
        raise SystemExit('the boot image is not an Android v4 container')
    if image[HEADER_BYTES:HEADER_BYTES + 8] != MAGIC:
        raise SystemExit('the ramdisk does not begin with the PONGFAT1 envelope')

    record = {'generator': 'scripts/make-pong-adt-ipados18.py',
              'container': {'format': 'android-boot-v4', 'header_bytes': BOOT_HEADER_V4_SIZE,
                            'kernel_bytes': 0, 'signature_bytes': 0,
                            'ramdisk_bytes': len(ramdisk), 'image_bytes': len(image),
                            'note': ('the device boots the UEFI firmware from its own partition; '
                                     'the ramdisk carries the whole payload')},
              'parameters': {'dram_size': hex(args.dram_size), 'window_size': hex(args.window_size),
                             'cpus': args.cpus, 'unique_chip_id': args.unique_chip_id, 'chip_id': chip_id,
                             'ufs_probe': bool(args.ufs_probe),
                             'development_debug': bool(args.development_debug),
                             'skip_image_verification': True,
                             'root_match_volume_uuid': root_uuid},
              'hardware_dtb': {'path': str(dtb), 'bytes': dtb.stat().st_size, 'sha256': sha256(dtb)},
              'adt_bytes': len(adt), 'adt_sha256': sha256(generated),
              'boot_manifest_bytes': len(manifest), 'boot_manifest_sha256': hashlib.sha256(manifest).hexdigest(),
              'boot_args': boot_args,
              'payload_bytes': payload.stat().st_size, 'payload_sha256': sha256(payload),
              'image_sha256': sha256(target), 'hardware_tested': False}
    (work / 'boot/pong-boot.json').write_text(json.dumps(record, indent=2) + '\n')
    (work / 'payload/pong-adt.json').write_text(json.dumps(record, indent=2) + '\n')
    manifest_path = work / 'package.manifest.json'
    if manifest_path.is_file():
        package = json.loads(manifest_path.read_text())
        outputs = package.setdefault('outputs', {})
        for key, path in (('boot.img', work / 'boot/boot.img'), ('payload.img', payload),
                          ('pong-xnu-initial.img', target)):
            if path.is_file():
                outputs[key] = {'bytes': path.stat().st_size, 'sha256': sha256(path)}
        manifest_path.write_text(json.dumps(package, indent=2, sort_keys=True) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
