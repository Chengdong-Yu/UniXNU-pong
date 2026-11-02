#!/usr/bin/env python3
"""Build the Pong Darwin 24 kernel aggregate for the UniXNU kernel step.

The component catalog names this file as the build recipe of the PongPlatform24
kit, so the shared kernel step runs it at this kit's archive position and takes
the file it writes as the kernel to prelink.

Two things the shared step cannot do for this board:

* Machine config. The aggregate is compiled with MACHINE_CONFIGS=PONG
  (config/MASTER.arm64.pong, added by patches/xnu-ipados18); the shared step
  compiles the arch profile's BCM2837 machine config.
* Build tree. The device builder links drivers/, kits/ and package/ into the
  tree it builds in, and the shared step's tree already has real directories
  there. So the aggregate is built in a tree of its own, and the resulting
  Mach-O is copied to the path board.toml [kernel.prelink] names inside the
  shared tree, where the prelink and the payload steps read it.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
import tomllib

HERE = Path(__file__).resolve().parent
KERNEL_RELATIVE = Path('build/xnu/ios-obj/DEVELOPMENT_ARM64_PONG/mach.development.pong')


def local_inputs(device):
    """The [pong_build] table of the device repository's local.toml, when it has one."""
    path = device / 'local.toml'
    if not path.is_file():
        return {}
    return tomllib.loads(path.read_text()).get('pong_build', {})


def seed_refer(device, sources):
    """Point the device's refer tree at the Apple sources the UniXNU checkout fetched.

    The device build scripts resolve their sources from refer/ipados18/<ref> and
    refuse a revision that differs from feeds.toml. The shared tree already holds
    those sources at the pinned revisions, so the device tree is linked to them
    instead of cloning everything a second time.
    """
    refer = device / 'refer/ipados18'
    refer.mkdir(parents=True, exist_ok=True)
    for source in tomllib.loads((device / 'feeds.toml').read_text()).get('source', []):
        if source.get('kind') != 'refer':
            continue
        target = sources / 'refer' / source['name']
        link = refer / source['ref']
        if not target.is_dir():
            raise SystemExit(f'{target} missing: the kernel step fetches the Apple sources before this recipe runs')
        if link.is_symlink() or link.exists():
            if link.resolve() == target.resolve():
                continue
            raise SystemExit(f'{link} already points elsewhere; remove it and rerun')
        link.symlink_to(target.resolve())


def hardware_dtb(device, tree, given=None):
    """Compile the hardware device tree the kernel is built against.

    devicetree.compile_hardware() writes it for the payload step, which runs
    after the kernel; the kernel needs it first, so the same dtc invocation is
    made here. The payload step compiles its own copy, so nothing is shared.
    """
    if given:
        return Path(given).resolve()
    board = tomllib.loads((device / 'board.toml').read_text())
    spec = board.get('device_tree', {})
    entry = spec.get('source', 'device:dts/pong.dts')
    kind, _, relative = entry.partition(':')
    source = (device / relative) if kind == 'device' else (tree / relative)
    if not source.is_file():
        raise SystemExit(f'{source} missing: [device_tree] source names it')
    command = [shutil.which('dtc') or '', '-I', 'dts', '-O', 'dtb',
               '-o', str(tree / 'build/pong/hardware.dtb')]
    if not command[0]:
        raise SystemExit('dtc is missing; install it (brew install dtc)')
    for item in spec.get('include_dirs', []):
        kind, _, relative = item.partition(':')
        directory = device / relative if kind == 'device' else tree / relative
        if not directory.is_dir():
            raise SystemExit(f'{directory} missing: [device_tree] include_dirs names it')
        command += ['-i', str(directory)]
    output = Path(command[command.index('-o') + 1])
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([*command, str(source)], check=True)
    if output.read_bytes()[:4] != b'\xd0\x0d\xfe\xed':
        raise SystemExit(f'{output} is not a flattened device tree')
    return output.resolve()


def main():
    device = Path(os.environ.get('UNIXNU_DEVICE', HERE.parents[1])).resolve()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact-dir', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=int(os.environ.get('JOBS', 0)) or 4)
    parser.add_argument('--config-path', type=Path, default=None)
    parser.add_argument('--hardware-dtb', type=Path, default=None)
    parser.add_argument('--tree', type=Path, default=None,
                        help='the shared kernel step build tree; its layout is what [kernel.prelink] names')
    args = parser.parse_args()
    inputs = local_inputs(device)
    shared = args.tree.resolve() if args.tree else None
    if shared:
        seed_refer(device, shared)
    # One tree per configuration, beside the shared one, so a rerun reuses its
    # objects and the shared tree keeps the layout its own steps expect.
    tree = Path(inputs['tree']) if inputs.get('tree') else (
        shared.parent / 'device-tree' if shared else device / 'build/device-tree')
    tree.mkdir(parents=True, exist_ok=True)
    dtb = hardware_dtb(device, tree, args.hardware_dtb)
    env = dict(os.environ, PONG_BUILD_TREE=str(tree), PONG_HARDWARE_DTB=str(dtb),
               UNIXNU_DEVICE=str(device))
    command = [sys.executable, str(device / 'scripts/build-pong-profile-kernel.py'),
               '--source', str(device), '--artifact-dir', str(args.artifact_dir.resolve()),
               '--hardware-dtb', str(dtb), '--jobs', str(args.jobs)]
    # The resolved configuration `release` wrote at its preflight step: the tree
    # of a configuration lives in that configuration's own directory.
    config = args.config_path or (shared.parent / '.config' if shared else None)
    if config and Path(config).is_file():
        command += ['--config-path', str(Path(config).resolve())]
    elif shared:
        raise SystemExit(f'{config} missing: the preflight step writes the resolved configuration')
    subprocess.run(command, cwd=device, env=env, check=True)
    produced = tree / KERNEL_RELATIVE
    if not produced.is_file():
        raise SystemExit(f'{produced} missing: the device builder did not leave its aggregate there')
    if shared:
        target = shared / KERNEL_RELATIVE
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(produced, target)
        print(json.dumps({'aggregate': str(produced), 'staged': str(target),
                          'bytes': target.stat().st_size}, indent=2))
    else:
        print(json.dumps({'aggregate': str(produced), 'bytes': produced.stat().st_size}, indent=2))


if __name__ == '__main__':
    main()
