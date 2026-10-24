#!/usr/bin/env python3
"""Build the actual20G81 Metal discovery bundle; no publication or phone run."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--bridge-directory',type=Path,required=True)
p.add_argument('--turnip-library',type=Path,required=True)
p.add_argument('--output-dir',type=Path,required=True)
p.add_argument('--developer-dir',type=Path,default=Path('/Volumes/PongDev/Applications/Xcode.app/Contents/Developer'))
a=p.parse_args();out=a.output_dir.resolve();bundle=out/'PongAdrenoMetal.bundle';bundle.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,DEVELOPER_DIR=str(a.developer_dir))
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
command=['xcrun','clang++','-target','arm64-apple-ios16.6.1','-isysroot',sdk,
    '-std=c++17','-fobjc-arc','-fblocks','-Os','-Wall','-Wextra','-Werror',
    '-Wno-deprecated-declarations','-I'+str(ROOT/'drivers/PongMetal'),
    '-I'+str(ROOT/'drivers/PongAdreno/submit'),
    '-dynamiclib',str(ROOT/'drivers/PongMetal/PongMetalPlugin.mm'),
    str(a.bridge_directory/'PongMetalBridge.dylib'),
    '-framework','Foundation','-framework','CoreFoundation','-framework','Metal',
    '-Wl,-rpath,@loader_path','-Wl,-install_name,@rpath/PongAdrenoMetal.bundle/PongAdrenoMetal',
    '-o',str(bundle/'PongAdrenoMetal')]
subprocess.run(command,env=env,check=True)
shutil.copyfile(a.bridge_directory/'PongMetalBridge.dylib',bundle/'PongMetalBridge.dylib')
shutil.copyfile(a.turnip_library,bundle/'libvulkan_freedreno.dylib')
for binary in ['PongAdrenoMetal','PongMetalBridge.dylib','libvulkan_freedreno.dylib']:
    subprocess.run(['codesign','--force','--sign','-','--pagesize','4096',str(bundle/binary)],check=True)
with (bundle/'Info.plist').open('wb') as f:
    plistlib.dump(dict(CFBundleIdentifier='dev.unixnu.pong.Metal',CFBundleName='PongAdrenoMetal',
        CFBundleExecutable='PongAdrenoMetal',CFBundlePackageType='BNDL',CFBundleVersion='1',
        NSPrincipalClass='PongAdrenoMetalDevice',MinimumOSVersion='16.6.1',
        CFBundleSupportedPlatforms=['iPhoneOS']),f)
clients=['backboardd', 'SpringBoard', 'CollectionsPoster', 'WidgetRenderer_Default', 'PosterBoard', 'Weather', 'WeatherPoster', 'GradientPosterExtension']
(out/'PongAdrenoMetal.clients').write_text('\n'.join(clients)+'\n')
(out/'PongAdrenoMetal.publish').write_text('1\n')
(out/'build-report.json').write_text(json.dumps(dict(bundle=str(bundle),command=command,
    guest='20G81',chip_id='0x07030002',discovery='IOAcceleratorES -> MetalPluginName -> NSBundle principal class',
    clients=clients,
    required_properties=['PongAdrenoCPReady','PongMetal20G81Enabled'],
    activation='pong_metal=1; native CP fences;20G81 ABI/protocols; Vulkan device creation',
    hardware_acceleration_confirmed=False,
    remaining=['Pong hardware CP/fence result','actual20G81 render-server IOSurface submissions/presentation'],
    image_verification='skipped by request'),indent=2)+'\n')
print(bundle)
