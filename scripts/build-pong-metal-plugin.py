#!/usr/bin/env python3
"""Build the selected guest's Metal discovery bundle; no phone run."""
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
p.add_argument('--guest-build',choices=('20G81','22H374'),default='20G81')
p.add_argument('--turnip-library',type=Path,required=True)
p.add_argument('--output-dir',type=Path,required=True)
p.add_argument('--clients',help='comma-separated process names, or * for system-wide native discovery')
p.add_argument('--developer-dir',type=Path,default=Path('/Volumes/PongDev/Applications/Xcode.app/Contents/Developer'))
a=p.parse_args()
# The upstream catalog separates hardware support from the selected OS ABI.
# Do not link a 22H374 discovery bundle against a cached 20G81 object bridge.
bridge_report_path=a.bridge_directory/'build-report.json'
bridge=json.loads(bridge_report_path.read_text())
bridge_guest=bridge.get('guest')
if bridge_guest is None and bridge.get('device_extent')==528 and bridge.get('deployment') in ('iOS16.6.1','16.6.1'):
    bridge_guest='20G81'  # original Pong build reports predate the guest field
expected_extent=600 if a.guest_build=='22H374' else 528
if bridge_guest!=a.guest_build or bridge.get('device_extent')!=expected_extent:
    raise SystemExit('Metal plugin/bridge OS ABI mismatch; build the bridge for '+a.guest_build)
out=a.output_dir.resolve();bundle=out/'PongAdrenoMetal.bundle';bundle.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,DEVELOPER_DIR=str(a.developer_dir))
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
minimum='18.0' if a.guest_build=='22H374' else '16.6.1'
command=['xcrun','clang++','-target','arm64-apple-ios'+minimum,'-isysroot',sdk,
    *(['-DPONG_GUEST_22H374=1'] if a.guest_build=='22H374' else []),
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
        NSPrincipalClass='PongAdrenoMetalDevice',MinimumOSVersion=minimum,
        CFBundleSupportedPlatforms=['iPhoneOS']),f)
clients=['backboardd', 'SpringBoard', 'CollectionsPoster', 'WidgetRenderer_Default', 'PosterBoard', 'Weather', 'WeatherPoster', 'GradientPosterExtension']
if a.guest_build=='22H374':
    # Actual 22H374 executable names from the supplied restore Info.plists.
    clients+=['Preferences','LegacyPoster','MercuryPosterExtension','Unity2025Poster','PridePosterExtension']
if a.clients is not None:
    clients=[x.strip() for x in a.clients.split(',') if x.strip()]
elif a.guest_build=='22H374':
    clients=['*']
if not clients or any('\n' in x or '\r' in x or len(x)>79 for x in clients):
    raise SystemExit('Invalid Metal client selection')
(out/'PongAdrenoMetal.clients').write_text('\n'.join(clients)+'\n')
(out/'PongAdrenoMetal.publish').write_text('1\n')
(out/'build-report.json').write_text(json.dumps(dict(bundle=str(bundle),command=command,
    guest=a.guest_build,chip_id='0x07030002',discovery='IOAcceleratorES -> MetalPluginName -> NSBundle principal class',
    bridge=dict(report=str(bridge_report_path.resolve()),guest=bridge_guest,device_extent=expected_extent),
    clients=clients,
    required_properties=['PongAdrenoCPReady','PongMetal'+a.guest_build+'Enabled'],
    activation='pong_metal=1; native CP fences; '+a.guest_build+' ABI/protocols; Vulkan device creation',
    hardware_acceleration_confirmed=False,
    remaining=['Pong hardware CP/fence result','actual '+a.guest_build+' render-server IOSurface submissions/presentation'],
    image_verification='skipped by request'),indent=2)+'\n')
print(bundle)
