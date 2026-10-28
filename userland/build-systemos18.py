#!/usr/bin/env python3
"""Copy original 22H374 files into an ordinary, unsealed APFS SystemOS volume."""
import argparse,json,os,plistlib,stat,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--mountpoint',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--materialize-existing',action='store_true',help='expand compressed files in an existing ordinary APFS raw output, retaining its UUID and size')
a=p.parse_args()
if os.geteuid()!=0:raise SystemExit('Run with system authentication to preserve original file ownership')
version=plistlib.loads((a.source/'System/Library/CoreServices/SystemVersion.plist').read_bytes())
if version.get('ProductBuildVersion')!='22H374':raise SystemExit('The source must be the original 22H374 SystemOS mount')
if not a.image.is_file():raise SystemExit('Supply an existing ordinary APFS image')
if a.materialize_existing:
 if a.image.resolve()!=a.output.resolve():raise SystemExit('Existing-volume mode requires --image and --output to name the same raw image')
elif a.output.exists():raise SystemExit('Supply an unused output path')
a.mountpoint.mkdir(parents=True,exist_ok=True)
def run(cmd,**kw):return subprocess.run([str(x) for x in cmd],check=True,**kw)
def enable_owners(mount):
 info=plistlib.loads(run(['diskutil','info','-plist',mount],capture_output=True).stdout)
 if not info.get('GlobalPermissionsEnabled',False):
  run(['diskutil','enableOwnership',mount])
  info=plistlib.loads(run(['diskutil','info','-plist',mount],capture_output=True).stdout)
 if not info.get('GlobalPermissionsEnabled',False):raise RuntimeError('Real file ownership is unavailable on '+str(mount))
# Without ownership enabled, stat/ditto can substitute unknown uid 99 for
# original root ownership; Darwin's shared-region mapper correctly rejects it.
enable_owners(a.source)
attached=plistlib.loads(run(['hdiutil','attach','-readwrite','-noverify','-nobrowse','-owners','on','-mountpoint',a.mountpoint,'-plist',a.image],capture_output=True).stdout)
dev=next(e['dev-entry'] for e in attached['system-entities'] if e.get('mount-point')==str(a.mountpoint))
try:
 enable_owners(a.mountpoint)
 options=['ditto','--rsrc','--extattr','--acl','--nopreserveHFSCompression','--nohfsCompression','--noclone']
 if a.materialize_existing:
  # ditto preserves UF_COMPRESSED by default. Expand only compressed files
  # so an already ordinary 5 GiB volume does not need space for a second
  # temporary copy of every large, uncompressed dyld subcache.
  pending=[]
  for directory,dirs,files in os.walk(a.mountpoint,followlinks=False):
   for name in files:
    target=Path(directory)/name;metadata=target.lstat()
    if stat.S_ISREG(metadata.st_mode) and metadata.st_flags&stat.UF_COMPRESSED:pending.append(target)
  print(f'Materializing {len(pending)} compressed SystemOS files from the original 22H374 mount',flush=True)
  for index,target in enumerate(pending,1):
   source=a.source/target.relative_to(a.mountpoint)
   run(options+[source,target])
   if target.stat().st_flags&stat.UF_COMPRESSED:raise RuntimeError('ditto retained compression: '+str(target))
   if index%100==0 or index==len(pending):print(f'Expanded files: {index}/{len(pending)}',flush=True)
 else:
  print('Copying original SystemOS files without inherited volume sealing or filesystem compression',flush=True)
  run(options+[a.source,a.mountpoint])
 # Repair earlier copies too, preserving each original owner/group/mode
 # rather than making the kernel accept caches owned by an arbitrary uid.
 changed=0
 for directory,dirs,files in os.walk(a.mountpoint,followlinks=False):
  targets=[Path(directory)]+[Path(directory)/name for name in files]
  targets += [Path(directory)/name for name in dirs if (Path(directory)/name).is_symlink()]
  for target in targets:
   source=a.source/target.relative_to(a.mountpoint)
   try:original=source.lstat()
   except FileNotFoundError:continue # local APFS housekeeping has no restore counterpart
   current=target.lstat()
   if (current.st_uid,current.st_gid)!=(original.st_uid,original.st_gid):
    os.chown(target,original.st_uid,original.st_gid,follow_symlinks=False);changed+=1
   if not stat.S_ISLNK(original.st_mode):os.chmod(target,stat.S_IMODE(original.st_mode))
 print(f'Original ownership restored on {changed} entries',flush=True)
 info=plistlib.loads(run(['diskutil','info','-plist',dev],capture_output=True).stdout)
 record=dict(build='22H374',source=str(a.source),image=str(a.image),volume_uuid=info['VolumeUUID'],volume_name=info.get('VolumeName'),volume_bytes=info.get('TotalSize'),operation='copy original files to ordinary APFS without filesystem compression; no binary patches',image_verification='skipped by request')
 record['ownership_repaired_entries']=changed;record['source_ownership_enabled']=True;record['destination_ownership_enabled']=True
 if a.materialize_existing:record['materialized_files']=len(pending)
finally:run(['hdiutil','detach',dev])
if not a.materialize_existing:
 run(['hdiutil','convert',a.image,'-format','UDTO','-o',a.output])
 if not a.output.exists() and Path(str(a.output)+'.dmg').exists():Path(str(a.output)+'.dmg').rename(a.output)
record['output']=str(a.output);record['output_bytes']=a.output.stat().st_size
Path(str(a.output)+'.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2),flush=True)
