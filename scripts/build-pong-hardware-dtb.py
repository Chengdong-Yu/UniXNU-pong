#!/usr/bin/env python3
"""Build the boot image's device tree from the board DTS, inside the build tree.

A firmware preflight script: unixnu runs it with the build tree as the working
directory and UNIXNU_UEFI / UNIXNU_DEVICE in the environment, and the boot image
names its output in board.toml [bootimg] dtb. The device tree is the one the
firmware hands to the kernel, so it is compiled once, here, and never copied
from a workstation.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def dtc():
    for name in ('dtc',):
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit('dtc is missing; install it (brew install dtc)')


def main():
    device = Path(os.environ.get('UNIXNU_DEVICE', Path(__file__).resolve().parents[1]))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', default='dts/pong.dts',
                        help='board DTS, relative to the device repository')
    parser.add_argument('--include', action='append', default=[],
                        help='include directory, relative to the device repository (repeatable)')
    parser.add_argument('--output', default='build/pong/pong.dtb',
                        help='output, relative to the build tree')
    args = parser.parse_args()
    source = (device / args.source).resolve()
    if not source.is_file():
        raise SystemExit(f'{source} missing: the board DTS is part of the device repository')
    includes = [Path(item) for item in args.include] or [Path('migration/format2/soc-seed/dts')]
    directories = []
    for item in includes:
        directory = (device / item).resolve() if not item.is_absolute() else item
        if not directory.is_dir():
            raise SystemExit(f'{directory} missing: the SoC DTSI include directory')
        directories.append(directory)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [dtc(), '-I', 'dts', '-O', 'dtb', '-o', str(output)]
    for directory in directories:
        command += ['-i', str(directory)]
    subprocess.run([*command, str(source)], check=True)
    data = output.read_bytes()
    if data[:4] != b'\xd0\x0d\xfe\xed':
        raise SystemExit(f'{output} is not a flattened device tree')
    record = {'source': str(source), 'includes': [str(item) for item in directories],
              'output': str(output), 'bytes': len(data)}
    output.with_suffix('.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
