#!/usr/bin/env python3
"""Build the Darwin24 codec/host-clock adapter without changing legacy scripts."""
import argparse,json,os,subprocess
from pathlib import Path
import pong_paths
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output-dir',type=Path,required=True)
p.add_argument('--developer-dir',type=Path,default=pong_paths.developer_dir())
a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,DEVELOPER_DIR=str(a.developer_dir))
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
library=out/'libpong-audio-codecs18.dylib'
command=['xcrun','clang','-target','arm64-apple-ios18.0','-isysroot',sdk,'-Os','-Wall','-Wextra','-Werror',
 '-dynamiclib','-Wl,-no_fixup_chains','-install_name','/pong/libpong-audio-codecs18.dylib',
 str(ROOT/'userland/audio_codecs18.c'),'-framework','AudioToolbox','-framework','CoreAudio','-o',str(library)]
subprocess.run(command,env=env,check=True)
subprocess.run(['codesign','--force','--sign','-','--pagesize','4096',str(library)],check=True)
(out/'audio-codecs-build.json').write_text(json.dumps(dict(guest='22H374',command=command,library=str(library),
 codec_policy='installed software decoders; other requests/errors remain native',
 clock_policy='audiomxd actual mach timebase; known frequency getter via validated provider; no PCM ownership changes',
 device_result='host build only'),indent=2)+'\n')
print(library)
