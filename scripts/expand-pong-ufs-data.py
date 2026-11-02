#!/usr/bin/env python3
"""Relocate a preserved APFS Data sparsebundle into a larger independent window.

Preserve the old window and commit SHNGROOT last. No image hashes/readback.
A required 4 KiB SHNGROOT metadata checksum is unrelated to image verification.
"""
import argparse,hashlib,json,os,plistlib,struct,subprocess,sys,zlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from sparsebundle import describe,LogicalImage
p=argparse.ArgumentParser(description=__doc__)
for name in ('bundle','old-installation','capture-record','output-dir','storage-record'):
 p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--adb',type=Path,required=True);p.add_argument('--serial',required=True)
p.add_argument('--port',type=int,default=5038);p.add_argument('--offset',type=int,default=40<<30)
p.add_argument('--resume',action='store_true');a=p.parse_args()
cmd=[str(a.adb),'-P',str(a.port),'-s',a.serial]
def shell(s):return subprocess.check_output(cmd+['shell',s],text=True).strip()
if subprocess.check_output(cmd+['get-state'],text=True).strip()!='recovery':raise RuntimeError('REC required')
old=json.loads(a.old_installation.read_text());capture=json.loads(a.capture_record.read_text())
if old['state']!='complete' or capture['state']!='complete':raise RuntimeError('completed baseline and live Data preservation required')
layout=old['layout'];prior=next(w for w in layout['windows'] if w['name']=='Data')
if (capture['offset'],capture['size'])!=(prior['offset'],prior['size']):raise RuntimeError('live Data preservation describes another window')
block,size=describe(a.bundle)
if size!=128000000000 or a.offset%(1<<20):raise RuntimeError('explicit 128GB image and MiB-aligned position required')
geometry=dict(device=shell('readlink -f /dev/block/by-name/userdata'),bytes=int(shell('blockdev --getsize64 /dev/block/by-name/userdata')),first_512=int(shell('cat /sys/class/block/sda13/start')),logical_block_bytes=int(shell('cat /sys/class/block/sda/queue/logical_block_size')))
if geometry!=old['geometry']:raise RuntimeError('Pong geometry differs')
if shell('ls /sys/class/block/sda13/holders') or any(x.split()[0] in ('/dev/block/sda13','/dev/block/by-name/userdata') for x in shell('cat /proc/mounts').splitlines()):raise RuntimeError('userdata mounted or held')
if a.offset+size>geometry['bytes']-(1<<30):raise RuntimeError('new Data exceeds tail reserve')
for w in layout['windows']:
 if a.offset<w['offset']+w['size'] and w['offset']<a.offset+size:raise RuntimeError('new Data overlaps '+w['name'])
raw=subprocess.check_output(cmd+['exec-out','dd','if=/dev/block/by-name/userdata','bs=4096','count=1','status=none'])
if len(raw)!=4096 or raw[:8]!=b'SHNGROOT':raise RuntimeError('SHNGROOT unavailable')
entries={}
for i in range(struct.unpack_from('<I',raw,12)[0]):
 n,o,l=struct.unpack_from('<32sQQ',raw,24+128*i);entries[n.split(b'\0')[0].decode()]=(o,l)
if entries!={w['name']:(w['offset'],w['size']) for w in layout['windows']}:raise RuntimeError('installed layout changed')
a.output_dir.mkdir(parents=True,exist_ok=True);receipt=a.output_dir/'installation.json'
bands=sorted(int(f.name,16) for f in (a.bundle/'bands').iterdir())
if any(i*block>=size for i in bands):raise RuntimeError('band outside new Data')
new=json.loads(json.dumps(layout));data=next(w for w in new['windows'] if w['name']=='Data')
data.update(offset=a.offset,size=size,image=str(a.bundle.resolve()));new['image_bytes']=max(w['offset']+w['size'] for w in new['windows'])
state=json.loads(receipt.read_text()) if a.resume else dict(state='prepared',serial=a.serial,build='22H374',geometry=geometry,layout=new,previous_layout=layout,previous_data=prior,completed_bands=[],written_bytes=0,zeroed=False,Data_preserved=True,OS_preserved=True,SystemOS_preserved=True,image_verification='skipped by request',readback_performed=False,boot_performed=False)
if state['layout']!=new or state['geometry']!=geometry or state['state']=='complete':raise RuntimeError('resume identity differs or already committed')
def save():
 t=receipt.with_suffix('.next');t.write_text(json.dumps(state,indent=2)+'\n');os.replace(t,receipt)
if not a.resume:(a.output_dir/'shngroot-before.bin').write_bytes(raw)
save()
# BLKZEROOUT guarantees zeros, unlike ordinary discard where this Pong
# reports discard_zeroes_data=0. Restrict it to the new unused Data range.
if not state['zeroed']:
 state['state']='zeroing_new_data';save();print('Zero-initializing only new unused Data window',flush=True)
 shell(f'/system/bin/toybox blkdiscard -z -o {a.offset} -l {size} /dev/block/by-name/userdata && sync')
 state['zeroed']=True;save();print('New Data window initialized; original Data retained',flush=True)
with LogicalImage(a.bundle) as source:
 for i in bands:
  if i in state['completed_bands']:continue
  relative=i*block;amount=min(block,size-relative);source.seek(relative)
  marker=f'PONG_DATA_BAND_{i}_{amount}'
  command=f'set -o pipefail; gzip -dc | dd of=/dev/block/by-name/userdata bs=4096 seek={(a.offset+relative)//4096} conv=notrunc,fsync status=none && printf "%s\\n" {marker}'
  writer=subprocess.Popen(cmd+['shell','-T',command],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  try:
   payload=source.read(amount);writer.stdin.write(zlib.compress(payload,1,wbits=31));writer.stdin.close();writer.stdin=None
   stdout,error=writer.communicate()
   if writer.returncode or stdout.strip()!=marker.encode():raise RuntimeError('Data write failed: '+error.decode(errors='replace'))
  except BaseException:
   if writer.poll() is None:writer.kill()
   writer.wait();raise
  state['completed_bands'].append(i);state['written_bytes']+=amount;state['state']='writing_preserved_data';save()
  if len(state['completed_bands'])%32==0:print(f'Preserved Data bands committed: {len(state["completed_bands"])}/{len(bands)}',flush=True)
header=bytearray(raw);struct.pack_into('<Q',header,16,new['image_bytes'])
for i in range(struct.unpack_from('<I',header,12)[0]):
 at=24+i*128
 if header[at:at+32].split(b'\0')[0]==b'Data':
  struct.pack_into('<QQ',header,at+32,a.offset,size);header[at+56:at+88]=bytes(32)
header[4064:]=hashlib.sha256(header[:4064]).digest()
(a.output_dir/'shngroot-ipados18.bin').write_bytes(header)
subprocess.run(cmd+['push',str(a.output_dir/'shngroot-ipados18.bin'),'/tmp/pong-data128-directory.bin'],check=True)
shell('dd if=/tmp/pong-data128-directory.bin of=/dev/block/by-name/userdata bs=4096 count=1 conv=notrunc,fsync status=none && sync')
state.update(state='complete',os_generation_complete=True,completed=dict(OS=next(w for w in new['windows'] if w['name']=='OS')['size'],Data=size),revision='services144-data128');save()
(a.output_dir/'ufs-layout.json').write_text(json.dumps(new,indent=2)+'\n')
storage=json.loads(a.storage_record.read_text())
if storage.get('pending'):raise RuntimeError('OS update unexpectedly pending; retain completed Data receipt')
storage.update(last_install=str(receipt.resolve()),revision='services144-data128');a.storage_record.write_text(json.dumps(storage,indent=2)+'\n')
print('128GB Data committed; previous 8GB Data/OS/SystemOS retained',flush=True)
