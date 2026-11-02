#!/usr/bin/env python3
"""Build the independent Darwin24 native software-codec selection adapter."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import pong_paths

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output-dir', type=Path, required=True)
p.add_argument('--developer-dir', type=Path, default=pong_paths.developer_dir())
a = p.parse_args()
out = a.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, DEVELOPER_DIR=str(a.developer_dir))
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
binary = out / 'libpong-audio-codecs18.dylib'
cmd = ['xcrun', 'clang', '-target', 'arm64-apple-ios18.0', '-isysroot', sdk,
       '-Os', '-Wall', '-Wextra', '-Werror', '-dynamiclib', '-Wl,-no_fixup_chains',
       '-install_name', '/pong/libpong-audio-codecs18.dylib',
       str(ROOT / 'userland/audio_codecs18.c'), '-framework', 'AudioToolbox', '-o', str(binary)]
subprocess.run(cmd, env=env, check=True)
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', str(binary)], check=True)
(out / 'audio-codecs-build.json').write_text(json.dumps(dict(
    guest='22H374', command=cmd, library=str(binary),
    targets=['audiomxd', 'AudioConverterService', 'AUHostingServiceXPC_arrow'],
    policy='installed Apple software decoders; native explicit successes/errors retained',
    device_result='not observed by host compilation'), indent=2) + '\n')
print(binary)
