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
    parser.add_argument('--artifact-dir', type=Path,
        default=DEVICE/'mac-build/ios16-xnu-pong',
        help='native kernel/result export, including an independent development build')
    parser.add_argument('--data-ramdisk', type=Path)
    parser.add_argument('--ufs-data', action='store_true',
        help='mount the configured APFS Data UUID from the dedicated UFS window; omit Data initrd')
    parser.add_argument('--kernel-arg', action='append', default=[])
    parser.add_argument('--cpus', type=int, choices=range(1, 9), default=1)
    parser.add_argument('--fat-mib', type=int, default=64)
    parser.add_argument('--window-size', type=lambda value: int(value, 0), default=0x40000000,
        help='memory to request from DarwinBoot; still requires an actual UEFI allocation')
    parser.add_argument('--verbose-console', action='store_true',
        help='opt in to -v; screen text is disabled by default')
    parser.add_argument('--launchd-ramdisk', action='store_true',
        help='20G81 launchd RAM boot policy, retaining the UUID-selected UFS root')
    parser.add_argument('--metal-accelerator',action='store_true',
        help='include actual20G81 IOGPUFamily for native Metal discovery')
    args = parser.parse_args()
    if bool(args.data_ramdisk)==bool(args.ufs_data):
        parser.error('select exactly one of --data-ramdisk or --ufs-data')
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    previous = args.previous.resolve()
    artifact = args.artifact_dir.resolve()
    report = json.loads((artifact / 'PONG_BUILD_RESULT.json').read_text())
    settings=report.get('settings',{})
    if args.ufs_data and (str(settings.get('XNU_OWNER_DATA_UFS','0'))!='1' or
                         str(settings.get('XNU_NATIVE_RAM_BLOCK','0'))!='0'):
        raise ValueError('UFS Data packaging requires the UFS Data kernel with NativeRamBlock disabled')
    if args.data_ramdisk and (str(settings.get('XNU_NATIVE_RAM_BLOCK','0'))!='1' or
                            str(settings.get('XNU_OWNER_DATA_UFS','0'))!='0'):
        raise ValueError('RAM Data packaging requires NativeRamBlock and no UFS Data backend')
    report['data_storage']='ufs-apfs' if args.ufs_data else 'packed-ramdisk'
    pong_gpu = str(report.get('settings', {}).get('XNU_PONG_ADRENO_PLATFORM', '0')) == '1'
    if args.metal_accelerator and not pong_gpu:
        raise ValueError('Metal packaging requires the compiled Pong SM8475 GPU owner')
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
    bindings = json.loads((DEVICE / 'mac-build/native-ufs/bindings.json').read_text())
    slide = int(bindings['fixed_prelink_base'], 16) - old_base
    roots = json.loads((previous / 'prelink.json').read_text())['components']
    if args.metal_accelerator and 'com.apple.iokit.IOGPUFamily' not in roots:
        roots.append('com.apple.iokit.IOGPUFamily')
    # IOGPUFamily's link closure also includes AppleARMPlatform/PMGR and the
    # original IOSurface/AMFI implementations. Pong already owns these paths.
    # v104 let AppleARMCPU match Pong's CPU nubs and panic in start(). Keep
    # these added components as libraries, without hardware/security autoload.
    gpu_libraries={
        'com.apple.driver.AppleARMPlatform','com.apple.driver.ApplePMGR',
        'com.apple.driver.AppleMobileFileIntegrity','com.apple.iokit.IOGPUFamily',
        'com.apple.iokit.IOReportFamily','com.apple.iokit.IOSlowAdaptiveClockingFamily',
        'com.apple.iokit.IOSurface'}
    # The root closure is computed inside emit: apply the policy to implicit
    # dependencies as well, including a fresh build from a pre-GPU manifest.
    library_only=gpu_libraries if 'com.apple.iokit.IOGPUFamily' in roots else set()
    candidate, selection = emit(native, original, roots, slide, verify_images=False,
        library_only_components=library_only)
    kernel = out / 'kernel-corecrypto.macho'
    kernel.write_bytes(candidate)
    (out / 'prelink.json').write_text(json.dumps(selection, indent=2) + '\n')
    report['base_kernel'] = report['kernel']
    report['kernel'] = {'path': str(kernel), 'bytes': len(candidate)}
    report['prelink'] = {'components': selection['components'],
        'library_only_components':selection['library_only_components'],
        'image_verification': 'skipped by request'}
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
    runtime_args = {value.split('=', 1)[0]: value.split('=', 1)[1]
                    for value in boot_args if '=' in value}
    if runtime_args.get('pong_metal') == '1' and (
            not pong_gpu or runtime_args.get('pong_adreno_start') != '1' or not args.metal_accelerator):
        raise ValueError('pong_metal=1 requires Pong GPU compilation, pong_adreno_start=1 and --metal-accelerator')
    if runtime_args.get('pong_gpu_scanout') == '1' and runtime_args.get('pong_metal') != '1':
        raise ValueError('GPU scanout requires the Metal publication path')
    if not any(v.startswith('epoch=') for v in boot_args):
        boot_args.append('epoch=' + str(int(time.time())))
    if args.launchd_ramdisk:
        if any(v.startswith(('rd=', 'rootdev=')) for v in boot_args):
            raise ValueError('launchd RAM policy requires the existing UUID-selected UFS root')
        # 20G81 launchd recognizes RAM bring-up by strstr(bootargs, "rd=md0"),
        # as used by HackiOS. XNU parses complete argument names, so this
        # userspace marker does not request a nonexistent /dev/md0 root.
        boot_args.append('launchd_rd=md0')
    auth = DEVICE.parent / 'apple/20G81/system-volume-auth-blob.bin'
    subprocess.run([sys.executable, str(DEVICE / 'scripts/build-native-bootimg-local.py'),
        '--kernel', str(kernel), '--result', str(result), '--output-dir', str(out),
        '--cpus', str(args.cpus),
        '--fat-mib', str(args.fat_mib), '--window-size', hex(args.window_size), '--ufs',
        '--root-match-volume-uuid', command['root_volume_uuid'],
        '--development-debug', '--amfi-only-platform-code',
        '--system-volume-auth-blob', str(auth),
        *(['--data-ramdisk', str(args.data_ramdisk), '--packed-data-ramdisk'] if args.data_ramdisk else []),
        '--skip-image-verification',
        *(['--verbose-console'] if args.verbose_console else []),
        *['--kernel-arg=' + v for v in boot_args]], check=True)
    symbols = Path(report['tree']) / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_PONG/mach.development.pong.unstripped'
    shutil.copyfile(symbols, out / symbols.name)
    print('Built without image verification:', out / 'pong-xnu-initial.img')


if __name__ == '__main__':
    main()
