#!/usr/bin/env python3
"""Build a minimal Apple device tree for Nothing Phone (2) Pong.

The physical DRAM size is required input. Do not substitute the sum of Linux
memory ranges or the DarwinBoot allocation window: XNU treats /chosen/dram-size
as the physical DRAM size reported by iBoot, while DarwinBoot separately owns
one contiguous EfiConventionalMemory range.
"""
from pathlib import Path
import argparse
import json
import os
import struct
import zlib
import uuid
import hashlib
import time
import importlib.util

from pong_adt_contract import audit_adt

ROOT = Path(__file__).resolve().parents[1]

# Values cross-checked against the Pong platform DTS and UEFI memory map.
DRAM_BASE = 0x80000000
APSS_BASE = 0x17000000
APSS_SIZE = 0x02000000
GIC_DIST = 0x17100000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x17180000
GIC_REDIST_SIZE = 0x200000
GIC_REDIST_STRIDE = 0x40000
TIMEBASE = 0x0124F800  # 19.2 MHz from the Pong timer node.
UFS_HC = 0x01D84000
UFS_HC_SIZE = 0x3000
GCC_UFS_PAGE = 0x00187000
UFS_STREAM_ID = 0xE0
UFS_SPI = 265
CPU_AFFINITIES = [i << 8 for i in range(8)]


def load_format2_hardware(path):
    """Use the selected DTSI/DTS for resources consumed by the Pong loader.

    Keep the existing Darwin24 handoff/manifest encoding. The generic ADT
    encoder does not yet express Pong's PMIC and Goodix metadata properties.
    """
    global DRAM_BASE, GIC_DIST, GIC_DIST_SIZE, GIC_REDIST, GIC_REDIST_SIZE
    global GIC_REDIST_STRIDE, TIMEBASE, UFS_HC, UFS_HC_SIZE, GCC_UFS_PAGE
    global UFS_STREAM_ID, UFS_SPI, CPU_AFFINITIES
    spec=importlib.util.spec_from_file_location('pong_fdt',ROOT/'scripts/prepare-pong-wifi.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    nodes=module.fdt_nodes(Path(path).read_bytes())
    u32=module.cells
    value=lambda p,k:int.from_bytes(p[k],'big')
    gic=next(p for p in nodes.values() if p.get('compatible')==b'arm,gic-v3\0')
    regs=u32(gic,'reg')
    if len(regs)!=8:raise ValueError('SM8475 GIC distributor and redistributor ranges required')
    GIC_DIST=(regs[0]<<32)|regs[1];GIC_DIST_SIZE=(regs[2]<<32)|regs[3]
    GIC_REDIST=(regs[4]<<32)|regs[5];GIC_REDIST_SIZE=(regs[6]<<32)|regs[7]
    GIC_REDIST_STRIDE=value(gic,'redistributor-stride')
    TIMEBASE=value(nodes['/cpus'],'timebase-frequency')
    cpus=[p for n,p in nodes.items() if n.startswith('/cpus/cpu@') and
          p.get('status',b'okay\0') in (b'okay\0',b'ok\0')]
    CPU_AFFINITIES=sorted(value(p,'reg') for p in cpus)
    if len(CPU_AFFINITIES)!=8 or CPU_AFFINITIES[0]!=0:
        raise ValueError('This Pong profile requires its eight physical CPU affinities')
    memory=next(p for p in nodes.values() if p.get('device_type')==b'memory\0')
    DRAM_BASE=int.from_bytes(memory['reg'][:8],'big')
    ufs=next(p for p in nodes.values() if b'qcom,sm8475-ufshc\0' in p.get('compatible',b''))
    UFS_HC=int.from_bytes(ufs['reg'][:8],'big');UFS_HC_SIZE=int.from_bytes(ufs['reg'][8:16],'big')
    GCC_UFS_PAGE=value(ufs,'pong,gcc-ufs-page');UFS_STREAM_ID=value(ufs,'pong,smmu-stream-id')
    UFS_SPI=value(ufs,'pong,ufs-spi')


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """Create the volatile CHRP NVRAM proxy required by IODTNVRAM."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [
        ('nvram-bank-size', struct.pack('<I', len(image))),
        ('nvram-bank-count', struct.pack('<I', 1)),
        ('nvram-current-bank', struct.pack('<I', 0)),
        ('nvram-proxy-data', bytes(image)),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, choices=range(1, 9), default=1,
                        help='SM8475 CPU nodes, MPIDR 0x000 through 0x700')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/pong/boot/kernel.macho.adt')
    parser.add_argument(
        '--dram-size', type=lambda value: int(value, 0), required=True,
        help='physical DRAM size from the target boot chain; not usable-range sum or window size')
    parser.add_argument(
        '--window-size', type=lambda value: int(value, 0), default=None,
        help='optional DarwinBoot contiguous window, 256 MiB..8 GiB, aligned to 2 MiB')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None,
                        help='software IOPlatformUUID seed, not an Apple silicon identifier')
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None,
                        help='software IOPlatformUUID seed, not an Apple silicon identifier')
    parser.add_argument('--ufs', '--ufs-probe', dest='ufs_probe', action='store_true',
                        help='publish the Pong SM8475 UFS nub; optional probes require a separate boot argument')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching UUID of the APFS system volume')
    parser.add_argument('--boot-manifest', type=Path, default=None,
                        help='actual staged development boot manifest; hash with SHA-384')
    parser.add_argument('--development-debug', action='store_true',
                        help='explicitly enable the DT debugger gate for development boot options')
    parser.add_argument('--amfi-only-platform-code', action='store_true',
                        help='set Sheng-compatible strict AMFI platform-code policy in /chosen')
    parser.add_argument('--system-volume-auth-blob', type=Path, default=None,
                        help='validated iOS 22H374 SystemVolume root-hash payload for /chosen')
    parser.add_argument('--packed-data-ramdisk', action='store_true',
                        help='publish a DarwinBoot PackedRAMDisk range for an ephemeral APFS Data seed')
    parser.add_argument('--skip-image-verification', action='store_true',
                        help='emit the DT without an additional output audit')
    args = parser.parse_args()
    if os.environ.get('PONG_HARDWARE_DTB'):
        load_format2_hardware(os.environ['PONG_HARDWARE_DTB'])
    if args.root_match_volume_uuid:
        try:
            args.root_match_volume_uuid = str(uuid.UUID(args.root_match_volume_uuid))
        except ValueError:
            parser.error('--root-match-volume-uuid must be a valid UUID')

    if args.dram_size <= 0 or args.dram_size > 0xFFFFFFFFFFFFFFFF - DRAM_BASE:
        parser.error('--dram-size must be a positive physical size that does not overflow DRAM_BASE')
    if args.window_size is not None and (
            not 0x10000000 <= args.window_size <= 0x200000000
            or args.window_size % 0x200000):
        parser.error('--window-size must be 256 MiB..8 GiB, aligned to 2 MiB')
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    if args.unique_chip_id is not None and (
            not 0 < args.unique_chip_id < 1 << 64 or not 0 < args.chip_id < 1 << 32):
        parser.error('identity seeds must be nonzero values fitting 8 and 4 bytes')
    system_volume_auth_blob = None
    if args.system_volume_auth_blob is not None:
        system_volume_auth_blob = args.system_volume_auth_blob.read_bytes()
        if len(system_volume_auth_blob) != 208 or struct.unpack_from('<4I', system_volume_auth_blob) != (2, 0, 1, 32):
            parser.error('--system-volume-auth-blob must be the pinned 208-byte 22H374 isys payload')

    # This is a per-image development-loader nonce, not a hardware UID1 nonce
    # or a claim of Apple personalization. Keep only its SHA-384 digest in DT.
    # Rebooting the same image reuses it; rebuilding rolls it.
    boot_nonce_hash = hashlib.sha384(os.urandom(32)).digest()
    backlight = json.loads((ROOT / 'configs/pong-display-sm8475.json').read_text())['brightness']
    chosen = [
        ('random-seed', os.urandom(64)),
        ('osenvironment', b'main\0'),
        # MobileGestalt/FrontBoard use this software coordinate scale. Missing
        # scale becomes 0 in FBSDisplayMode (infinite logical dimensions) and
        # SpringBoard deliberately traps before creating its main display.
        # Keep the real framebuffer pixels; do not substitute an Apple panel.
        ('display-scale', struct.pack('<I', 3)),
        ('pong-dsi-backlight', struct.pack('<Q6I',
            int(backlight['controller'], 0), backlight['register_bytes'],
            backlight['maximum'], backlight['minimum_nonzero'],
            int(backlight['bl_inverted_dbv']), backlight['channel'], backlight['version'])),
        ('pong-audio-dma', bytes(16)),
        ('darwinboot-memory-ranges', bytes(32 + 64 * 16)),
        ('pong-allocatable-ram', b''.join(
            struct.pack('<QQ', int(bank['base'], 0), int(bank['size'], 0))
            for bank in json.loads((ROOT / 'configs/pong-memory-sm8475.json').read_text())['allocatable'])),
        ('display-rotation', struct.pack('<I', 0)),
        # Host UTC sampled for this temporary development boot. The platform
        # expert advances this software clock using the native arch timer.
        ('boot-epoch', struct.pack('<Q', int(time.time()))),
        # PE_init_platform reads this uint32_t before cs_init. Boot arguments
        # alone cannot enable debugger-gated policies when the cell is absent.
        ('debug-enabled', struct.pack('<I', int(args.development_debug))),
        *([('ephemeral-storage', struct.pack('<I', 1))] if args.packed_data_ramdisk else []),
        ('crypto-hash-method', b'sha2-384\0'),
        # This unlocked UEFI development chain has no preceding Apple Image4
        # manifest chain. Darwin 22 AppleImage4 reads a 32-bit prevention flag
        # here and inverts it for its AMNM runtime identifier. This describes
        # our loader policy, not an Apple silicon fuse or hardware identity.
        ('mix-n-match-prevention-status', struct.pack('<I', 0)),
        *([('boot-manifest-hash', hashlib.sha384(args.boot_manifest.read_bytes()).digest())]
          if args.boot_manifest else []),
        ('dram-base', struct.pack('<Q', DRAM_BASE)),
        ('dram-size', struct.pack('<Q', args.dram_size)),
        *([('darwinboot-memory-size', struct.pack('<Q', args.window_size))]
          if args.window_size is not None else []),
        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)),
           ('chip-id', struct.pack('<I', args.chip_id))]
          if args.unique_chip_id is not None else []),
        *([('root-matching', ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                              '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                              args.root_match_volume_uuid.upper() + '</string></dict></dict>\0').encode())]
          if args.root_match_volume_uuid else []),
        # Match Sheng's strict AMFI policy input. This does not create an AMFI
        # service; the corresponding original extension must still be loaded.
        *([('amfi-only-platform-code', struct.pack('<I', 1))]
          if args.amfi_only_platform_code else []),
        *([('system-volume-auth-blob', system_volume_auth_blob)]
          if system_volume_auth_blob is not None else []),
        *empty_nvram_proxy(),
    ]
    chosen.append(('psci-method', b'smc\0'))
    cpus = [node(f'cpu{i}', [
        ('state', b'running\0' if i == 0 else b'stopped\0'),
        ('reg', struct.pack('<I', CPU_AFFINITIES[i])),
        ('device_type', b'cpu\0'),
        ('compatible', b'qcom,kryo\0'),
        ('timebase-frequency', struct.pack('<I', TIMEBASE)),
    ]) for i in range(args.cpus)]
    gic = node('intc', [
        ('compatible', b'arm,gic-v3\0'),
        ('reg', struct.pack('<4Q', GIC_DIST - APSS_BASE, GIC_DIST_SIZE,
                            GIC_REDIST - APSS_BASE, GIC_REDIST_SIZE)),
        ('redistributor-stride', struct.pack('<Q', GIC_REDIST_STRIDE)),
    ])
    ufs = node('ufshc', [
        ('compatible', b'qcom,sm8475-ufshc\0qcom,sm8450-ufshc\0'),
        ('pong,ufs-hc-base', struct.pack('<Q', UFS_HC)),
        ('pong,ufs-hc-size', struct.pack('<Q', UFS_HC_SIZE)),
        ('pong,gcc-ufs-page', struct.pack('<Q', GCC_UFS_PAGE)),
        ('pong,smmu-stream-id', struct.pack('<I', UFS_STREAM_ID)),
        ('pong,smmu-base', struct.pack('<Q', 0x15000000)),
        ('pong,smmu-size', struct.pack('<Q', 0x100000)),
        ('pong,smmu-context-banks', struct.pack('<I', 78)),
        ('pong,smmu-match-groups', struct.pack('<I', 120)),
        # This probe does not register an interrupt. A standard interrupts
        # property invokes IODTMapOneInterrupt during IODeviceTreeAlloc, before
        # ArmCpu starts. Without an interrupt-parent/controller/map it loops
        # on the arm-io parent in Darwin 22. Keep the Linux SPI as board metadata
        # until a complete Pong GIC SPI provider and interrupt specifier exist.
        ('pong,ufs-spi', struct.pack('<I', UFS_SPI)),
    ])
    # Actual Pong live DT: QUP0 SE4 / Goodix Berlin-D, GPIO20 reset/21 IRQ.
    # Absolute metadata avoids an incomplete arm-io interrupt/reg mapping.
    touch = node('pong-touch', [
        ('compatible', b'pong,goodix-berlin-d\0'),
        ('spi-base', struct.pack('<Q', 0x990000)),
        ('tlmm-base', struct.pack('<Q', 0xF000000)),
        ('gcc-base', struct.pack('<Q', 0x100000)),
        ('rsc-base', struct.pack('<Q', 0x17A20000)),
        ('cmd-db-base', struct.pack('<Q', 0x80860000)),
        ('gpio-offset', struct.pack('<I', 0x100000)),
        ('pins', struct.pack('<6I', 16, 17, 18, 19, 21, 20)),
        ('max-x', struct.pack('<I', 1080)), ('max-y', struct.pack('<I', 2412)),
    ])
    tree = node('device-tree', [
        # Keys use the actual SM8475 PMIC-arbiter resources below.
        # MobileGestalt derives ProductType/DeviceClass from hw.product, which
        # comes from this model. Keep the restore's J171 board policy (jetsam,
        # arm64 ABI) separate from the requested iPhone presentation identity.
        # Physical resources remain Pong/SM8475; /product is an ADT node.
        ('model', b'iPhone17,1\0'),
        ('compatible', b'J171AP\0iPhone17,1\0qcom,sm8475\0'),
        ('target-type', b'J171\0'),
        ('target-sub-type', b'J171AP\0'),
        ('pong,physical-model', b'Nothing Phone (2)\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', chosen, [node('memory-map',
            [('PackedRAMDisk', bytes(16))] if args.packed_data_ramdisk else []),
            node('manifest-properties', [('BNCH', boot_nonce_hash)])]),
        node('cpus', [('#address-cells', struct.pack('<I', 1))], cpus),
        node('arm-io', [
            ('device_type', b'arm-io\0'),
            ('#address-cells', struct.pack('<I', 2)),
            ('#size-cells', struct.pack('<I', 2)),
            ('ranges', struct.pack('<QQQ', 0, APSS_BASE, APSS_SIZE)),
        ], [gic, touch, node('pong-buttons', [
            ('compatible', b'pong,pm8350-keys\0'),
            ('core-base', struct.pack('<Q', 0x0c400000)),
            ('cnfg-base', struct.pack('<Q', 0x0c42d000)),
            ('observer-base', struct.pack('<Q', 0x0c440000)),
            ('ee', struct.pack('<I', 0)),
            ('pon-ppid', struct.pack('<I', 0x013)),
            ('volume-up-ppid', struct.pack('<I', 0x18d)),
        ]), *([ufs] if args.ufs_probe else [])]),
        node('defaults', [('sleep-disabled', struct.pack('<I', 1))]),
        node('options'),
        # Missing HomeButtonType defaults to a Home-button iPad in Gestalt.
        # Pong has only the three physical volume/power buttons.
        node('buttons', [
            ('button-names', b'volup\0voldown\0hold\0'),
            ('home-button-type', struct.pack('<I', 2)),
            ('opposed-power-vol-buttons', struct.pack('<I', 1)),
        ]),
        node('product', [
            ('product-name', b'iPhone 16 Pro\0'),
            ('product-description', b'iPhone 16 Pro\0'),
            ('artwork-device-idiom', b'phone\0'),
            # iPhone 16 Pro artwork family. Physical display geometry stays
            # 1080x2412 in /chosen and the framebuffer, not 1206x2622.
            ('artwork-device-subtype', struct.pack('<I', 2622)),
            ('artwork-dynamic-displaymode', b'0\0'),
            ('artwork-display-gamut', b'sRGB\0'),
            ('compatible-device-fallback', b'iPhone17,1\0'),
            ('compatible-app-variant', b'0\0'),
            # Keep Pong's v82 memory performance tier. Phone artwork does
            # not authorize a higher memory budget in the 1 GiB XNU window.
            ('device-perf-memory-class', struct.pack('<I', 3)),
            ('thin-bezel', struct.pack('<I', 1)),
            ('allow-32bit-apps', struct.pack('<I', 0)),
            ('ephemeral-data-mode', struct.pack('<I', 0)),
            ('udid-version', struct.pack('<I', 1)),
            ('artwork-scale-factor', struct.pack('<I', 3)),
        ]),
    ])
    # Hardware-only probes do not load AppleImage4. Full root boot does, and its
    # nonce subsystem requires CHMH even when the chip initializer allows AMNM.
    if not args.skip_image_verification:
        audit_adt(tree, require_manifest=bool(args.root_match_volume_uuid),
                  boot_manifest=args.boot_manifest.read_bytes() if args.boot_manifest else None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'Pong ADT: {len(tree)} bytes; cpu_count={args.cpus}; dram={args.dram_size:#x}; '
          f'window={args.window_size if args.window_size is not None else "DarwinBoot default"}; '
          f'packed-data-ramdisk={int(args.packed_data_ramdisk)}')


if __name__ == '__main__':
    main()
