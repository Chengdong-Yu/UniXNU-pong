#!/usr/bin/env python3
"""Compile Pong's QCA6490 PCI handoff adapter for Darwin24; no phone access.

This archive is a transport prerequisite, not a working Wi-Fi stack. Do not
select it as an Ath12kWifi backend or publish Apple80211 before the ath11k/MHI,
DMA/MSI and selected Darwin24 ABI are connected.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import pong_paths


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree', type=Path, default=pong_paths.build_tree())
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--developer-dir', type=Path, default=pong_paths.developer_dir())
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    profile = json.loads((ROOT / 'configs/pong-wifi-sm8475.json').read_text())
    if profile['soc'] != 'SM8475' or profile['pci']['device'] != 0x1103 or profile['driver_family'] != 'ath11k':
        raise RuntimeError('QCA6490 board inputs required')
    framework = a.tree / '.cache/fakeroot/System/Library/Frameworks/Kernel.framework/Versions/A'
    env = dict(os.environ, DEVELOPER_DIR=str(a.developer_dir))
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    source = ROOT / 'drivers/PongWifi/PongWifiPci.cpp'
    command = ['xcrun', 'clang++', '-target', 'arm64-apple-ios18.0', '-isysroot', sdk,
        '-mkernel', '-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti',
        '-fno-stack-protector', '-mgeneral-regs-only', '-std=gnu++17', '-fblocks', '-Os',
        '-DKERNEL=1', '-DKERNEL_PRIVATE=1', '-DXNU_KERNEL_PRIVATE=1', '-DARM64_BOARD_CONFIG_PONG=1',
        '-I' + str(framework / 'PrivateHeaders'), '-I' + str(framework / 'Headers'),
        '-Wno-deprecated-declarations', '-Wall', '-Wextra',
        '-c', str(source), '-o', str(out / 'PongWifiPci.o')]
    subprocess.run(command, env=env, check=True)
    subprocess.run(['xcrun', 'libtool', '-static', '-o', str(out / 'libPongWifiPlatform.a'),
                    str(out / 'PongWifiPci.o')], env=env, check=True)
    (out / 'build-report.json').write_text(json.dumps(dict(guest='22H374', soc='SM8475',
        backend='ath11k PCI handoff prerequisite', command=command,
        linked_into_boot_kernel=False, wifi_operational=False,
        image_verification='skipped by request'), indent=2) + '\n')
    print(out / 'libPongWifiPlatform.a')


if __name__ == '__main__': main()
