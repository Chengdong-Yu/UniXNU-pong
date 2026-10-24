#!/usr/bin/env python3
"""Cross-build Pong's Metal objects for actual iOS16.6.1. No device/test run.
Translator source must already contain the upstream NativeMetal bridge patches.
Output is a library only: no plugin publication or claim of acceleration.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--translator',required=True,type=Path)
p.add_argument('--output-dir',required=True,type=Path)
p.add_argument('--developer-dir',type=Path,default=Path('/Volumes/PongDev/Applications/Xcode.app/Contents/Developer'))
a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
policy=a.translator/'crates/metal2vulkan-runtime/src/metal_level.rs'
if 'IOS16_6_1' not in policy.read_text():
    subprocess.run(['git','apply',str(ROOT/'patches/metal2vulkan/0008-pong-ios16-20G81-policy.patch')],
        cwd=a.translator,check=True)
surface=a.translator/'crates/metal2vulkan-runtime/src/buffer_texture.rs'
if 'surface_row_padding' not in surface.read_text():
    subprocess.run(['git','apply',str(ROOT/'patches/metal2vulkan/0009-pong-owned-iosurface-row-padding.patch')],
        cwd=a.translator,check=True)
env=dict(os.environ,DEVELOPER_DIR=str(a.developer_dir),IPHONEOS_DEPLOYMENT_TARGET='16.6.1')
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
frontend=out/'PongMetalFrontend.o'
cmd=['xcrun','clang++','-target','arm64-apple-ios16.6.1','-isysroot',sdk,
    '-std=c++17','-fobjc-arc','-fblocks','-Os','-Wall','-Wextra','-Werror',
    '-Wno-deprecated-declarations','-I'+str(ROOT/'drivers/PongMetal'),
    '-c',str(ROOT/'drivers/PongMetal/NativeBridgeMetalFrontend.mm'),'-o',str(frontend)]
subprocess.run(cmd,env=env,check=True)
manifest=out/'Cargo.toml'
manifest.write_text('\n'.join([
    '[package]','name="pong-metal-bridge"','version="0.1.0"','edition="2024"','rust-version="1.98"','[workspace]',
    '[lib]','crate-type=["staticlib"]','path='+json.dumps(str(ROOT/'drivers/PongMetal/bridge/lib.rs')),
    '[dependencies]',
    'metal2vulkan={path='+json.dumps(str(a.translator/'crates/metal2vulkan'))+',features=["ash"]}',
    'metal2vulkan-runtime={path='+json.dumps(str(a.translator/'crates/metal2vulkan-runtime'))+'}',
    'libc="0.2"','[profile.release]','panic="unwind"','debug=0','',
]))
env['PATH']='/Users/desktop/.cargo/bin:'+env['PATH']
env['SDKROOT']=sdk
env['RUSTFLAGS']='-C link-arg=-miphoneos-version-min=16.6.1'
cargo=['cargo','build','--release','--manifest-path',str(manifest),
    '--target','aarch64-apple-ios','--target-dir',str(out/'target')]
with (out/'cargo-build.log').open('w') as log:
    r=subprocess.run(cargo,env=env,stdout=log,stderr=subprocess.STDOUT)
if r.returncode:raise SystemExit('Rust bridge build failed: '+str(out/'cargo-build.log'))
archive=out/'target/aarch64-apple-ios/release/libpong_metal_bridge.a'
library=out/'PongMetalBridge.dylib'
link=['xcrun','clang++','-target','arm64-apple-ios16.6.1','-isysroot',sdk,
    '-dynamiclib','-Wl,-install_name,@rpath/PongMetalBridge.dylib',
    str(frontend),'-Wl,-force_load,'+str(archive),
    '-framework','Foundation','-framework','Metal','-framework','IOSurface',
    '-framework','QuartzCore','-framework','CoreGraphics','-liconv','-lresolv',
    '-o',str(library)]
with (out/'link.log').open('w') as log:
    r=subprocess.run(link,env=env,stdout=log,stderr=subprocess.STDOUT)
if r.returncode:raise SystemExit('Metal dylib link failed: '+str(out/'link.log'))
report=dict(frontend=str(frontend),archive=str(archive),library=str(library),
    deployment='iOS16.6.1',device_extent=528,automatic_publication=False,
    hardware_acceleration=False,commands=[cmd,cargo,link],
    runtime_format_policy='iOS16.6.1/20G81 guest format metadata; Vulkan capabilities; no Apple heap/compression assumptions',
    image_verification='skipped by request')
(out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(report['library'])
