#!/usr/bin/env python3
"""Compile the Darwin 22 loader on the Intel Mac without changing the saved FD."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

DEVICE = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edk2', type=Path, default=Path('/Volumes/PongDev/source/Mu_Basecore'))
    parser.add_argument('--package', type=Path, default=Path(
        '/Users/desktop/Desktop/Pong-Mac-Handoff/workspace/UniXNU/package'))
    parser.add_argument('--output-dir', type=Path, default=Path('/Volumes/PongDev/build/Pong-DarwinBoot22'))
    parser.add_argument('--llvm', type=Path, default=Path('/opt/local/libexec/llvm-18/bin'))
    args = parser.parse_args()
    source = DEVICE / 'drivers/DarwinBoot22'
    workspace = args.output_dir.resolve()
    conf = workspace / 'Conf'
    conf.mkdir(parents=True, exist_ok=True)
    tools = args.edk2 / 'BaseTools'
    for name in ('target', 'tools_def', 'build_rule'):
        destination = conf / (name + '.txt')
        if not destination.exists():
            shutil.copyfile(tools / 'Conf' / (name + '.template'), destination)
    env = dict(os.environ)
    env.update(WORKSPACE=str(workspace), EDK_TOOLS_PATH=str(tools), CONF_PATH=str(conf),
               PACKAGES_PATH=os.pathsep.join(map(str, (args.edk2, args.package, source))),
               PYTHON_COMMAND=sys.executable,
               PYTHONPATH=str(tools / 'Source/Python'),
               CLANG_BIN=str(args.llvm) + '/', SOURCE_DATE_EPOCH='1792454400')
    env['PATH'] = os.pathsep.join(map(str, (Path(sys.executable).parent,
        tools / 'BinWrappers/PosixLike', tools / 'Source/C/bin', args.llvm,
        '/opt/local/bin', '/usr/bin', '/bin', '/usr/sbin', '/sbin')))
    command = [str(tools / 'BinWrappers/PosixLike/build'),
               '-p', str(source / 'DarwinBootPkg/DarwinBoot.dsc'),
               '-a', 'AARCH64', '-b', 'RELEASE', '-t', 'CLANGPDB', '-n', '2']
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    log = workspace / f'build-{timestamp}.log'
    with log.open('w') as output:
        result = subprocess.run(command, cwd=workspace, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    print(f'Full build log: {log}')
    if result.returncode:
        print('\n'.join(log.read_text(errors='replace').splitlines()[-35:]))
        raise SystemExit(result.returncode)
    binary = workspace / 'build/DarwinBoot/RELEASE_CLANGPDB/AARCH64/DarwinBoot22.efi'
    artifact = DEVICE / 'mac-build/darwinboot22'
    artifact.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binary, artifact / binary.name)
    shutil.copyfile(log, artifact / 'build.log')
    report = {'command': command, 'log': str(log), 'source': str(source),
              'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'boot_args_bytes': '0x2e0', 'command_line_bytes': 608,
              'boot_flags_offset': '0x2d0', 'mem_size_actual_offset': '0x2d8',
              'saved_firmware_modified': False, 'hardware_tested': False,
              'owned_trampoline': True, 'handoff_status_display': True,
              'loader_replacement': 'volatile protocol replacement, restored on driver unload'}
    (artifact / 'BUILD_RESULT.json').write_text(json.dumps(report, indent=2) + '\n')
    print(artifact / binary.name)


if __name__ == '__main__':
    main()
