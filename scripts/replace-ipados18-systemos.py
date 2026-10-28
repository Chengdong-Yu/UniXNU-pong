#!/usr/bin/env python3
"""Install a rebuilt SystemOS, relocating existing Data before committing SHNGROOT."""
import argparse,json,os,struct,subprocess,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];MIB=1<<20
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--layout',type=Path,default=ROOT/'out/ipados18/ufs/ufs-layout.json')
p.add_argument('--old-installation',type=Path,default=ROOT/'out/ipados18/install/installation.json')
p.add_argument('--boot-image',type=Path,required=True)
p.add_argument('--serial',required=True);p.add_argument('--port',type=int,default=5038)
p.add_argument('--output-dir',type=Path,default=ROOT/'out/ipados18/install-systemos4')
p.add_argument('--stream-compression',choices=('none','gzip'),default='none',help='compress only the USB stream; leave the installed filesystem uncompressed')
p.add_argument('--resume',action='store_true');a=p.parse_args()
new=json.loads(a.layout.read_text());previous=json.loads(a.old_installation.read_text())
if previous.get('state')!='complete':raise SystemExit('The previous installation must have committed its directory')
old=previous.get('layout',previous.get('new_layout'))
if old is None:raise SystemExit('The previous installation has no completed layout')
windows={w['name']:w for w in new['windows']};before={w['name']:w for w in old['windows']}
report=json.loads((a.boot_image.parent/'PONG_BUILD_RESULT.json').read_text())
if not a.boot_image.is_file() or not report['build_complete'] or report['profile']!='Pong-iPadOS18-22H374-Darwin24' or report['settings']['XNU_OWNER_CACHE_VOLUME_UUID']!=windows['SystemOS']['volume_uuid']:raise SystemExit('Build the matching SystemOS boot package first')
if old['windows'][0]!=new['windows'][0] or before['Data']['size']!=windows['Data']['size'] or before['Data']['volume_uuid']!=windows['Data']['volume_uuid']:raise SystemExit('OS and Data identity must be preserved')
system=windows['SystemOS'];src=before['Data'];dst=windows['Data'];target='/dev/block/by-name/userdata'
delta=dst['offset']-src['offset']
if delta<0 or 0<delta<64*MIB:raise SystemExit('Keep Data in place or relocate it higher by at least 64 MiB')
if any(w['offset']%MIB or w['size']%4096 for w in new['windows']):raise SystemExit('Unaligned layout')
if Path(system['image']).stat().st_size!=system['size']:raise SystemExit('SystemOS input size changed')
cmd=['/Volumes/PongDev/tools/platform-tools/adb','-P',str(a.port),'-s',a.serial]
def run(args,**kw):return subprocess.run(args,check=True,**kw)
def shell(s):return run(cmd+['shell',s],capture_output=True,text=True).stdout.strip()
if run(cmd+['get-state'],capture_output=True,text=True).stdout.strip()!='recovery':raise SystemExit('Enter recovery first')
if a.stream_compression=='gzip':shell('command -v gzip >/dev/null && set -o pipefail')
if shell('readlink -f '+target)!='/dev/block/sda13' or int(shell('blockdev --getsize64 '+target))!=(new['partition_last_4k']-new['partition_first_4k']+1)*4096 or int(shell('cat /sys/class/block/sda13/start'))!=new['partition_first_4k']*8:raise SystemExit('Pong userdata geometry differs')
if shell('ls /sys/class/block/sda13/holders') or any(l.split()[0] in (target,'/dev/block/sda13') for l in shell('cat /proc/mounts').splitlines()):raise SystemExit('userdata must have no active mounts or holders')
if dst['offset']+dst['size']>int(shell('blockdev --getsize64 '+target)) or system['offset']+system['size']>dst['offset']:raise SystemExit('Window bounds overlap or exceed userdata')
a.output_dir.mkdir(parents=True,exist_ok=True);record=a.output_dir/'replacement.json'
if a.resume:
 state=json.loads(record.read_text())
 if state['new_layout']!=new or state['old_layout']!=old or state['serial']!=a.serial:raise SystemExit('Resume record differs')
else:
 if record.exists():raise SystemExit('Use --resume for an existing replacement')
 header=run(cmd+['exec-out','dd','if='+target,'bs=4096','count=1','status=none'],capture_output=True).stdout
 if len(header)!=4096 or header[:8]!=b'SHNGROOT':raise SystemExit('Missing current SHNGROOT directory')
 count=struct.unpack_from('<I',header,12)[0]
 entries={}
 for i in range(count):
  name,offset,size=struct.unpack_from('<32sQQ',header,24+i*128);entries[name.split(b'\0')[0].decode()]=(offset,size)
 if entries!={w['name']:(w['offset'],w['size']) for w in old['windows']}:raise SystemExit('Current directory differs from the installed layout')
 (a.output_dir/'shngroot-before.bin').write_bytes(header)
 state=dict(build='22H374',serial=a.serial,old_layout=old,new_layout=new,data_moved=0 if delta else src['size'],data_relocated=bool(delta),systemos_written=0,state='relocating_data' if delta else 'writing_systemos',stream_compression=a.stream_compression,image_verification='skipped by request',image_readback=False)
def save():
 temp=record.with_suffix('.next')
 with temp.open('w') as f:f.write(json.dumps(state,indent=2)+'\n');f.flush();os.fsync(f.fileno())
 os.replace(temp,record)
save();remote='/tmp/pong-data-relocate.part'
while state['data_moved']<src['size']:
 amount=min(64*MIB,src['size']-state['data_moved']);relative=src['size']-state['data_moved']-amount
 read=(src['offset']+relative)//MIB;write=(dst['offset']+relative)//MIB
 shell(f'dd if={target} of={remote} bs=1048576 skip={read} count={amount//MIB} status=none')
 if int(shell('stat -c %s '+remote))!=amount:raise SystemExit('Data relocation encountered a short read')
 shell(f'dd if={remote} of={target} bs=1048576 seek={write} conv=notrunc,fsync status=none')
 state['data_moved']+=amount;save()
 if state['data_moved']%(512*MIB)==0:print(f'Data relocated: {state["data_moved"]}/{src["size"]}',flush=True)
shell('rm -f '+remote);state['state']='writing_systemos';save()
with Path(system['image']).open('rb',buffering=0) as f:
 f.seek(state['systemos_written'])
 while state['systemos_written']<system['size']:
  amount=min(512*MIB,system['size']-state['systemos_written']);offset=system['offset']+state['systemos_written']
  device_write=f'dd of={target} bs=1048576 seek={offset//MIB} conv=notrunc,fsync status=none'
  codec=zlib.compressobj(level=1,wbits=31) if a.stream_compression=='gzip' else None
  if codec:device_write='set -o pipefail; gzip -dc | '+device_write
  # exec-in uses the raw exec service: its process return code does not
  # provide shell-v2's remote command status. Use binary-safe shell-v2 and
  # require a device-side completion receipt after both pipeline stages.
  receipt=f'PONG_SYSTEMOS_WRITE_DONE_{offset}_{amount}'
  device_write+=' && printf "%s\\n" '+receipt
  proc=subprocess.Popen(cmd+['shell','-T',device_write],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  try:
   left=amount
   while left:
    data=f.read(min(8*MIB,left))
    if not data:raise RuntimeError('Unexpected end of SystemOS input')
    encoded=codec.compress(data) if codec else data
    if encoded:proc.stdin.write(encoded)
    left-=len(data)
   if codec:proc.stdin.write(codec.flush())
   proc.stdin.close();proc.stdin=None;stdout,stderr=proc.communicate()
   if proc.returncode or stdout.strip()!=receipt.encode():raise RuntimeError('Device did not acknowledge the write: '+stderr.decode(errors='replace')+' '+stdout.decode(errors='replace'))
  except BaseException:
   if proc.poll() is None:proc.kill()
   proc.wait();raise
  state['systemos_written']+=amount;state['device_write_receipts']=state.get('device_write_receipts',0)+1;save();print(f'SystemOS device acknowledged: {state["systemos_written"]}/{system["size"]}',flush=True)
run(cmd+['push',str(a.layout.parent/'shngroot-ipados18.bin'),'/tmp/pong-systemos-header.bin'])
shell(f'dd if=/tmp/pong-systemos-header.bin of={target} bs=4096 count=1 conv=notrunc,fsync status=none && sync')
state['state']='complete';save();print('SystemOS replaced; existing 8 GiB Data preserved; directory committed last',flush=True)
