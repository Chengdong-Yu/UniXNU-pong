#!/usr/bin/env python3
"""Write prepared iPadOS18 windows in REC, committing SHNGROOT last. No readback."""
import argparse,json,os,struct,subprocess,sys,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from sparsebundle import LogicalImage,snapshot,storage_for
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--layout',type=Path,default=ROOT/'out/ipados18/ufs/ufs-layout.json')
p.add_argument('--boot-image',type=Path,required=True)
p.add_argument('--adb',type=Path,default=Path('/Volumes/PongDev/tools/platform-tools/adb'))
p.add_argument('--serial',required=True);p.add_argument('--port',type=int,default=5038)
p.add_argument('--output-dir',type=Path,default=ROOT/'out/ipados18/install')
p.add_argument('--chunk-mib',type=int,default=512);p.add_argument('--resume',action='store_true')
p.add_argument('--windows',nargs='+',choices=('OS','SystemOS','Data'),help='update only these windows without changing the existing layout')
p.add_argument('--stream-compression',choices=('none','gzip'),default='none')
p.add_argument('--os-delta',type=Path,help='tracked OS bands from prepare-ipados18-os-delta.py')
a=p.parse_args()
if not a.boot_image.is_file():raise SystemExit('Build the iPadOS18 boot image before installing its system windows')
boot_result=a.boot_image.parent/'PONG_BUILD_RESULT.json'
boot_manifest=a.boot_image.parent/'stage/EFI/UniXNU/pong-boot-manifest.json'
if not boot_result.is_file() or not boot_manifest.is_file():raise SystemExit('A completed iPadOS18 boot package is required before writing')
boot=json.loads(boot_result.read_text());manifest=json.loads(boot_manifest.read_text())
if boot.get('profile')!='Pong-iPadOS18-22H374-Darwin24' or not boot.get('build_complete') or 'pong_build=ipados18-22H374' not in manifest.get('boot_args','').split():raise SystemExit('Boot package metadata does not select the 22H374 port')
if not 16<=a.chunk_mib<=1024:raise SystemExit('Use 16..1024 MiB transfer chunks')
layout=json.loads(a.layout.read_text());header=a.layout.parent/'shngroot-ipados18.bin'
delta=json.loads(a.os_delta.read_text()) if a.os_delta else None
os_window=next(w for w in layout['windows'] if w['name']=='OS')
tracked=storage_for(os_window['image'])
if tracked and tracked.get('pending') and not delta and (not a.windows or 'OS' in a.windows):
 raise SystemExit('A tracked OS update is pending; finish and install its --os-delta plan')
if delta:
 if a.windows!=['OS'] or delta.get('format')!='pong-os-delta-v1' or delta.get('build')!='22H374':
  raise SystemExit('A tracked delta updates only --windows OS')
 if not tracked or delta['image']!=tracked['image'] or delta['base_install']!=tracked['last_install']:
  raise SystemExit('Delta does not describe the installed OS generation')
 if tracked.get('pending')!=str(a.os_delta.resolve().parent) or delta['window']!=os_window:
  raise SystemExit('Delta generation or OS geometry differs')
 baseline=json.loads(Path(delta['base_install']).read_text())
 if baseline.get('state')!='complete' or baseline['serial']!=a.serial:
  raise SystemExit('Completed baseline installation receipt required')
 # Filesystem write tracking only: no image bytes are read/compared here.
 if snapshot(tracked['image'])!=delta['bands']:
  raise SystemExit('OS was edited after delta preparation; finish the plan again')
 previous_end=0
 for off,size in delta['ranges']:
  if off<previous_end or off%1048576 or size<=0 or size%4096 or off+size>os_window['size']:
   raise SystemExit('Invalid OS delta extent')
  previous_end=off+size
if layout['build']!='22H374' or len(header.read_bytes())!=4096:raise SystemExit('Wrong installation layout')
if manifest.get('root_volume_uuid')!=layout['windows'][0]['volume_uuid']:raise SystemExit('The loader selects a different OS volume')
cmd=[str(a.adb),'-P',str(a.port),'-s',a.serial]
def run(args,**kw):return subprocess.run(args,check=True,**kw)
def shell(s):return run(cmd+['shell',s],capture_output=True,text=True).stdout.strip()
if run(cmd+['get-state'],capture_output=True,text=True).stdout.strip()!='recovery':raise SystemExit('The phone must be in recovery')
if a.stream_compression=='gzip':shell('command -v gzip >/dev/null && set -o pipefail')
target='/dev/block/by-name/userdata';real=shell('readlink -f '+target)
geometry=dict(device=real,bytes=int(shell('blockdev --getsize64 '+target)),first_512=int(shell('cat /sys/class/block/sda13/start')),logical_block_bytes=int(shell('cat /sys/class/block/sda/queue/logical_block_size')))
expected=(layout['partition_last_4k']-layout['partition_first_4k']+1)*4096
if real!='/dev/block/sda13' or geometry['bytes']!=expected or geometry['first_512']!=layout['partition_first_4k']*8 or geometry['logical_block_bytes']!=4096:raise SystemExit('Pong userdata geometry differs')
if delta and (delta['serial']!=a.serial or delta['geometry']!=geometry):
 raise SystemExit('Delta belongs to a different device geometry')
if shell('ls /sys/class/block/sda13/holders') or any(l.split()[0] in (target,real) for l in shell('cat /proc/mounts').splitlines()):raise SystemExit('userdata must have no active mounts or holders')
for w in layout['windows']:
 input_size=tracked['logical_bytes'] if w['name']=='OS' and tracked else Path(w['image']).stat().st_size
 if input_size!=w['size'] or w['offset']%4096 or w['size']%4096 or w['offset']+w['size']>geometry['bytes']:raise SystemExit('An input size or window geometry changed')
a.output_dir.mkdir(parents=True,exist_ok=True);record=a.output_dir/'installation.json'
if a.resume:
 state=json.loads(record.read_text())
 if state['layout']!=layout or state['serial']!=a.serial or state['geometry']!=geometry or state.get('selected_windows')!=a.windows:raise SystemExit('Resume state belongs to another layout/device')
 if state.get('os_delta')!=(str(a.os_delta.resolve()) if delta else None):raise SystemExit('Resume uses a different delta')
else:
 if record.exists():raise SystemExit('An installation record already exists; use --resume')
 before=run(cmd+['exec-out','dd','if='+target,'bs=4096','count=1','status=none'],capture_output=True).stdout
 if len(before)!=4096:raise SystemExit('Cannot preserve the existing 4 KiB directory')
 if a.windows:
  if before[:8]!=b'SHNGROOT':raise SystemExit('Partial update requires an existing SHNGROOT directory')
  entries={}
  for i in range(struct.unpack_from('<I',before,12)[0]):
   name,offset,size=struct.unpack_from('<32sQQ',before,24+i*128);entries[name.split(b'\0')[0].decode()]=(offset,size)
  if entries!={w['name']:(w['offset'],w['size']) for w in layout['windows']}:raise SystemExit('Partial update must preserve the installed layout')
 (a.output_dir/'shngroot-before.bin').write_bytes(before)
 state=dict(build='22H374',serial=a.serial,geometry=geometry,layout=layout,selected_windows=a.windows,stream_compression=a.stream_compression,completed={},state='writing_windows',image_verification='skipped by request',readback_performed=False,boot_performed=False)
 if delta:
  state.update(os_delta=str(a.os_delta.resolve()),base_install=delta['base_install'],
               revision=delta['revision'],logical_os_bytes=os_window['size'],
               planned_transfer_bytes=delta['transfer_bytes'],completed_ranges={})
def save():record.write_text(json.dumps(state,indent=2)+'\n')
save();chunk=a.chunk_mib*1024**2;scratch=a.output_dir/'transfer.part';remote='/tmp/pong-ipados18.part'
for w in layout['windows']:
 if a.windows and w['name'] not in a.windows:continue
 ranges=delta['ranges'] if delta else [[0,w['size']]]
 source=LogicalImage(tracked['image']) if w['name']=='OS' and tracked else Path(w['image']).open('rb',buffering=0)
 with source as src:
  for range_offset,range_size in ranges:
   done=state.get('completed_ranges',{}).get(str(range_offset),0) if delta else state['completed'].get(w['name'],0)
   src.seek(range_offset+done)
   while done<range_size:
    amount=min(chunk,range_size-done)
    # Stream only the chosen logical range, retaining raw-image support.
    offset=w['offset']+range_offset+done
    if offset%(1<<20):raise RuntimeError('A streaming chunk must start on a MiB boundary')
    receipt=f'PONG_WINDOW_WRITE_DONE_{offset}_{amount}'
    device_write=f'dd of={target} bs=1048576 seek={offset//(1<<20)} conv=notrunc,fsync status=none'
    codec=zlib.compressobj(level=1,wbits=31) if a.stream_compression=='gzip' else None
    if codec:device_write='set -o pipefail; gzip -dc | '+device_write
    device_write+=' && printf "%s\\n" '+receipt
    transfer=subprocess.Popen(cmd+['shell','-T',device_write],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
     left=amount
     while left:
      b=src.read(min(8<<20,left))
      if not b:raise RuntimeError('Unexpected end of '+w['name'])
      encoded=codec.compress(b) if codec else b
      if encoded:transfer.stdin.write(encoded)
      left-=len(b)
     if codec:transfer.stdin.write(codec.flush())
     transfer.stdin.close();transfer.stdin=None
     stdout,stderr=transfer.communicate()
     if transfer.returncode or stdout.strip()!=receipt.encode():raise RuntimeError('Device did not acknowledge the write: '+stderr.decode(errors='replace')+' '+stdout.decode(errors='replace'))
    except BaseException:
     if transfer.poll() is None:transfer.kill()
     transfer.wait();raise
    done+=amount
    if delta:
     state['completed_ranges'][str(range_offset)]=done
     state['completed'][w['name']]=sum(state['completed_ranges'].values())
    else:state['completed'][w['name']]=done
    state['device_write_receipts']=state.get('device_write_receipts',0)+1;save()
    total=delta['transfer_bytes'] if delta else w['size']
    print(f'{w["name"]}: {state["completed"][w["name"]]}/{total} bytes committed',flush=True)
scratch.unlink(missing_ok=True)
if not delta:
 run(cmd+['push',str(header),'/tmp/pong-ipados18-header.bin'])
 shell(f'dd if=/tmp/pong-ipados18-header.bin of={target} bs=4096 count=1 conv=notrunc,fsync status=none && sync')
else:shell('sync')
state['state']='complete'
if tracked and (not a.windows or 'OS' in a.windows):state['os_generation_complete']=True
save();print('iPadOS18 '+','.join(a.windows or ['OS','SystemOS','Data'])+' installed; no image hashes or readback performed')
if tracked and (not a.windows or 'OS' in a.windows):
 tracked.pop('record',None);tracked.update(last_install=str(record.resolve()),pending=None,
                                           revision=delta['revision'] if delta else a.output_dir.name)
 Path(os_window['image']).parent.joinpath('os-storage.json').write_text(json.dumps(tracked,indent=2)+'\n')
