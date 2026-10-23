#!/usr/bin/env python3
"""Rebuild an existing Pong prelink selection and boot payload without image verification."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE / 'kits/NativePrelink'))
from native_prelink import macho
from emit_prelink import emit, COALESCED


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--previous', required=True, type=Path)
    parser.add_argument('--original-input', type=Path,
        help='existing decompressed Apple kernel containing the selected kexts')
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--artifact-dir', required=True, type=Path,
        help='native kernel/result export, including an independent development build')
    parser.add_argument('--bindings', required=True, type=Path,
        help='bindings.json produced by the local NativeUfs build')
    parser.add_argument('--bootimg-builder', required=True, type=Path,
        help='boot image builder from the UniXNU core checkout')
    parser.add_argument('--data-ramdisk', required=True, type=Path)
    parser.add_argument('--system-volume-auth-blob', required=True, type=Path,
        help='locally fetched/extracted 20G81 system-volume authentication blob')
    parser.add_argument('--kernel-arg', action='append', default=[])
    parser.add_argument('--cpus', type=int, choices=range(1, 9), default=1)
    parser.add_argument('--fat-mib', type=int, default=64)
    parser.add_argument('--window-size', type=lambda value: int(value, 0), default=0x40000000,
        help='memory to request from DarwinBoot; still requires an actual UEFI allocation')
    parser.add_argument('--verbose-console', action='store_true',
        help='opt in to -v; screen text is disabled by default')
    parser.add_argument('--launchd-ramdisk', action='store_true',
        help='20G81 launchd RAM boot policy, retaining the UUID-selected UFS root')
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    previous = args.previous.resolve()
    artifact = args.artifact_dir.resolve()
    report = json.loads((artifact / 'PONG_BUILD_RESULT.json').read_text())
    native = (artifact / 'mach.development.pong').read_bytes()
    original_path = args.original_input or previous / 'original.fat'
    original = original_path.read_bytes()
    # Parse only the format needed to construct the new relocations. No
    # image digests, output comparisons or instruction/output audits run.
    if original[:4] == bytes.fromhex('cafebabe'):
        count = struct.unpack_from('>I', original, 4)[0]
        slices = [struct.unpack_from('>5I', original, 8 + 20 * i) for i in range(count)]
        entry = next(s for s in slices if s[:2] == (0x100000c, 0))
        original = original[entry[2]:entry[2] + entry[3]]
    original_image = macho(original)
    old_base = min(s['va'] for s in original_image['segments'] if s['name'] in COALESCED and s['size'])
    bindings = json.loads(args.bindings.read_text())
    slide = int(bindings['fixed_prelink_base'], 16) - old_base
    roots = json.loads((previous / 'prelink.json').read_text())['components']
    candidate, selection = emit(native, original, roots, slide, verify_images=False)
    kernel = out / 'kernel-corecrypto.macho'
    kernel.write_bytes(candidate)
    (out / 'prelink.json').write_text(json.dumps(selection, indent=2) + '\n')
    report['base_kernel'] = report['kernel']
    report['kernel'] = {'path': str(kernel), 'bytes': len(candidate)}
    report['prelink'] = {'components': selection['components'], 'image_verification': 'skipped by request'}
    report['image_verification'] = 'skipped by request'
    result = out / 'PONG_BUILD_RESULT.json'
    result.write_text(json.dumps(report, indent=2) + '\n')
    command = json.loads((previous / 'stage/EFI/UniXNU/pong-boot-manifest.json').read_text())
    # Older manifests start with -v; quiet manifests do not. Remove named
    # prefix options instead of dropping a positional number of arguments.
    replacement_keys = {v.split('=', 1)[0] for v in args.kernel_arg if '=' in v}
    # Preserve the selected PID1 helper just like other runtime policies.
    # Dropping pong_init_path silently executes the older helper in the UFS
    # root instead of the updated one installed in RAM Data. That helper
    # inherits an interactive stdout, which20G81 launchd rejects fatally.
    boot_args = [v for v in command['boot_args'].split()
                 if v != '-v' and not v.startswith(('keepsyms=', 'cpus='))
                 and v.split('=', 1)[0] not in replacement_keys]
    boot_args += args.kernel_arg
    if not any(v.startswith('epoch=') for v in boot_args):
        boot_args.append('epoch=' + str(int(time.time())))
    if args.launchd_ramdisk:
        if any(v.startswith(('rd=', 'rootdev=')) for v in boot_args):
            raise ValueError('launchd RAM policy requires the existing UUID-selected UFS root')
        # 20G81 launchd recognizes RAM bring-up by strstr(bootargs, "rd=md0"),
        # as used by HackiOS. XNU parses complete argument names, so this
        # userspace marker does not request a nonexistent /dev/md0 root.
        boot_args.append('launchd_rd=md0')
    subprocess.run([sys.executable, str(args.bootimg_builder),
        '--kernel', str(kernel), '--result', str(result), '--output-dir', str(out),
        '--cpus', str(args.cpus),
        '--fat-mib', str(args.fat_mib), '--window-size', hex(args.window_size), '--ufs',
        '--root-match-volume-uuid', command['root_volume_uuid'],
        '--development-debug', '--amfi-only-platform-code',
        '--system-volume-auth-blob', str(args.system_volume_auth_blob), '--data-ramdisk', str(args.data_ramdisk),
        '--packed-data-ramdisk', '--skip-image-verification',
        *(['--verbose-console'] if args.verbose_console else []),
        *['--kernel-arg=' + v for v in boot_args]], check=True)
    symbols = Path(report['tree']) / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_PONG/mach.development.pong.unstripped'
    shutil.copyfile(symbols, out / symbols.name)
    print('Built without image verification:', out / 'pong-xnu-initial.img')


if __name__ == '__main__':
    main()
