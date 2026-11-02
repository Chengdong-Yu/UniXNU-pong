#!/usr/bin/env python3
"""Install an expanded OS in an unused SHNGROOT window; retain OS rollback/Data.

Streams the OS once, then commits only the 4 KiB directory. No image hashing,
device image readback, Data/SystemOS transfer or boot-partition flashing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from sparsebundle import LogicalImage,describe


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('before-layout','layout','storage-record','boot-image','output-dir'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--adb',type=Path,required=True);p.add_argument('--serial',required=True)
    p.add_argument('--port',type=int,default=5038);p.add_argument('--resume',action='store_true')
    a=p.parse_args();old=json.loads(a.before_layout.read_text());new=json.loads(a.layout.read_text())
    storage=json.loads(a.storage_record.read_text());cmd=[str(a.adb),'-P',str(a.port),'-s',a.serial]
    def shell(text):return subprocess.check_output(cmd+['shell',text],text=True).strip()
    if subprocess.check_output(cmd+['get-state'],text=True).strip()!='recovery':raise RuntimeError('REC required')
    if new['build']!='22H374' or old['build']!=new['build']:raise RuntimeError('restore mismatch')
    if {w['name'] for w in new['windows']}!={'OS','SystemOS','Data'}:raise RuntimeError('unexpected windows')
    prior={w['name']:w for w in old['windows']};selected=next(w for w in new['windows'] if w['name']=='OS')
    for name in ('SystemOS','Data'):
        if next(w for w in new['windows'] if w['name']==name)!=prior[name]:raise RuntimeError(name+' must be retained')
    if selected['volume_uuid']!=prior['OS']['volume_uuid']:raise RuntimeError('OS UUID changed')
    if not a.boot_image.is_file() or not (a.boot_image.parent/'PONG_BUILD_RESULT.json').is_file():raise RuntimeError('completed boot image required')
    manifest=json.loads((a.boot_image.parent/'stage/EFI/UniXNU/pong-boot-manifest.json').read_text())
    if manifest['root_volume_uuid']!=selected['volume_uuid'] or 'pong_rootful=1' not in manifest['boot_args'].split():raise RuntimeError('matching rootful boot required')
    target='/dev/block/by-name/userdata';real=shell('readlink -f '+target)
    geometry=dict(device=real,bytes=int(shell('blockdev --getsize64 '+target)),
        first_512=int(shell('cat /sys/class/block/sda13/start')),
        logical_block_bytes=int(shell('cat /sys/class/block/sda/queue/logical_block_size')))
    if real!='/dev/block/sda13' or geometry['logical_block_bytes']!=4096 or geometry['first_512']!=old['partition_first_4k']*8 or geometry['bytes']!=(old['partition_last_4k']-old['partition_first_4k']+1)*4096:raise RuntimeError('Pong userdata geometry differs')
    if shell('ls /sys/class/block/sda13/holders') or any(line.split()[0] in (target,real) for line in shell('cat /proc/mounts').splitlines()):raise RuntimeError('userdata has active mounts/holders')
    start,size=selected['offset'],selected['size'];end=start+size
    if start%(1<<20) or size%4096 or end>geometry['bytes']:raise RuntimeError('OS bounds')
    for window in old['windows']:
        if start<window['offset']+window['size'] and window['offset']<end:raise RuntimeError('new OS overlaps existing '+window['name'])
    band,logical=describe(storage['image'])
    if logical!=size or storage['logical_bytes']!=size:raise RuntimeError('expanded local OS geometry differs')
    a.output_dir.mkdir(exist_ok=True);receipt=a.output_dir/'installation.json'
    if a.resume:
        state=json.loads(receipt.read_text())
        if state['layout']!=new or state['serial']!=a.serial or state['geometry']!=geometry:raise RuntimeError('resume identity changed')
    else:
        if receipt.exists():raise RuntimeError('existing installation receipt; use --resume')
        before=subprocess.check_output(cmd+['exec-out','dd','if='+target,'bs=4096','count=1','status=none'])
        if len(before)!=4096 or before[:8]!=b'SHNGROOT':raise RuntimeError('SHNGROOT directory missing')
        entries={}
        for i in range(struct.unpack_from('<I',before,12)[0]):
            name,offset,length=struct.unpack_from('<32sQQ',before,24+i*128)
            entries[name.split(b'\0')[0].decode()]=(offset,length)
        if entries!={w['name']:(w['offset'],w['size']) for w in old['windows']}:raise RuntimeError('installed directory differs from previous layout')
        (a.output_dir/'shngroot-before.bin').write_bytes(before)
        state=dict(build='22H374',serial=a.serial,geometry=geometry,layout=new,
            previous_layout=old,completed={'OS':0},state='writing_relocated_os',
            selected_windows=['OS'],Data_preserved=True,SystemOS_preserved=True,
            old_OS_retained=True,image_verification='skipped by request',readback_performed=False)
    def save():receipt.write_text(json.dumps(state,indent=2)+'\n')
    save()
    with LogicalImage(storage['image']) as source:
        done=state['completed']['OS'];source.seek(done)
        while done<size:
            amount=min(256<<20,size-done);offset=start+done
            marker=f'PONG_OS_RELOCATED_{offset}_{amount}'
            command=f'set -o pipefail; gzip -dc | dd of={target} bs=1048576 seek={offset//(1<<20)} conv=notrunc,fsync status=none && printf "%s\\n" {marker}'
            writer=subprocess.Popen(cmd+['shell','-T',command],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                codec=zlib.compressobj(level=1,wbits=31);left=amount
                while left:
                    block=source.read(min(8<<20,left))
                    if not block:raise RuntimeError('short OS source')
                    data=codec.compress(block)
                    if data:writer.stdin.write(data)
                    left-=len(block)
                writer.stdin.write(codec.flush());writer.stdin.close();writer.stdin=None
                out,error=writer.communicate()
                if writer.returncode or out.strip()!=marker.encode():raise RuntimeError('write failed: '+error.decode(errors='replace'))
            except BaseException:
                if writer.poll() is None:writer.kill()
                writer.wait();raise
            done+=amount;state['completed']['OS']=done;save();print(f'OS {done}/{size} committed; previous windows intact',flush=True)
    # SHNGROOT has a mandatory directory checksum. This covers only its
    # 4064-byte metadata record; no filesystem image is hashed or compared.
    header=bytearray((a.output_dir/'shngroot-before.bin').read_bytes())
    struct.pack_into('<Q',header,16,new['image_bytes'])
    for i in range(struct.unpack_from('<I',header,12)[0]):
        at=24+i*128;name=header[at:at+32].split(b'\0')[0].decode()
        if name=='OS':
            struct.pack_into('<QQ',header,at+32,start,size);header[at+56:at+88]=bytes(32)
    header[4064:]=hashlib.sha256(header[:4064]).digest()
    local=a.output_dir/'shngroot-ipados18.bin';local.write_bytes(header)
    subprocess.run(cmd+['push',str(local),'/tmp/pong-rootful-directory.bin'],check=True)
    shell(f'dd if=/tmp/pong-rootful-directory.bin of={target} bs=4096 count=1 conv=notrunc,fsync status=none && sync')
    state.update(state='complete',os_generation_complete=True,boot_performed=False);save()
    storage.update(last_install=str(receipt.resolve()),pending=None,revision='services113-rootful-expanded-os')
    a.storage_record.write_text(json.dumps(storage,indent=2)+'\n')
    print('Expanded OS committed; Data/SystemOS and old OS retained',flush=True)


if __name__=='__main__':main()
