#!/usr/bin/env python3
"""Build the single native4K helper archive shared by GPU, Wi-Fi and audio."""
import argparse
from pathlib import Path
import subprocess
from pong_native_inputs import ROOT, build_env


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree', type=Path, required=True)
    a = p.parse_args()
    out = a.tree.resolve() / 'build/pong-smmu4k'
    out.mkdir(parents=True, exist_ok=True)
    env = build_env()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    source = ROOT / 'drivers/PongAdreno/ArmSmmu4K'
    objects = []
    for name in ('ArmSmmu4K', 'ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KStreams',
                 'ArmSmmu4KAudit', 'ArmSmmu4KLive', 'ArmSmmu4KTableMemory','ArmSmmu4KMmio'):
        obj = out / (name + '.o')
        subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios18.0', '-isysroot', sdk,
                        '-mkernel', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                        '-mgeneral-regs-only', '-Os', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(source), '-c', str(source / (name + '.c')), '-o', str(obj)],
                       env=env, check=True)
        objects.append(obj)
    archive = out / 'libPongSmmu4K.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'libtool', '-static', '-o', str(archive), *map(str, objects)], env=env, check=True)
    print(archive)


if __name__ == '__main__':
    main()
