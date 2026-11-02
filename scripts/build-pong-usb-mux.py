#!/usr/bin/env python3
"""Build the independent iOS18 native-service USB Mux transport daemon."""
import argparse
import json
from pathlib import Path
import plistlib
import subprocess
from pong_native_inputs import build_env

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output-dir',type=Path,required=True)
a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
binary=out/'usb-mux'
command=['xcrun','--sdk','iphoneos','clang','-target','arm64-apple-ios18.0',
         '-Os','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wl,-no_fixup_chains',
         str(ROOT/'userland/usb-mux.c'),'-framework','IOKit','-framework','CoreFoundation','-o',str(binary)]
subprocess.run(command,env=build_env(),check=True)
ent=out/'usb-mux-entitlements.plist'
ent.write_bytes(plistlib.dumps({'platform-application':True,'com.apple.private.security.no-container':True,
                              'com.apple.private.iokit.user-access':True,
                              'com.apple.private.MobileGestalt.AllowedProtectedKeys':['UniqueDeviceID']}))
subprocess.run(['codesign','--force','--sign','-','--pagesize','4096','--entitlements',str(ent),str(binary)],check=True)
(out/'usb-mux-build.json').write_text(json.dumps({'guest':'22H374','command':command,
    'policy':'physical DWC3 -> loopback; real lockdownd trust/pairing/TLS; no synthetic services',
    'device_result':'not confirmed by compilation'},indent=2)+'\n')
print(binary)
