#!/usr/bin/env python3
"""Install a constructed APFS Data disk into Pong's independent UFS window.

Requires recovery ADB and the existing SHNGROOT OS/SystemOS layout. Does not
format the whole userdata partition, change GPT, hash/read back disk images,
or boot the phone. Commits the new directory only after the Data write fsync.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import struct
import subprocess

FIRST_4K=0x413248
LAST_4K=0x75a40ea
PARTITION_BYTES=(LAST_4K-FIRST_4K+1)*4096
TARGET='/dev/block/by-name/userdata'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb',type=Path,required=True)
    p.add_argument('--serial',required=True)
    p.add_argument('--port',type=int,default=5038)
    p.add_argument('--data-image',type=Path,required=True)
    p.add_argument('--volume-uuid',required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    p.add_argument('--offset',type=lambda v:int(v,0),default=8*1024**3)
    mode=p.add_mutually_exclusive_group()
    mode.add_argument('--replace-existing-data',action='store_true',
        help='Explicitly install an expanded disk over the existing Data window')
    mode.add_argument('--update-existing-data',action='store_true',
        help='Explicitly update a migrated disk at the same Data offset and size')
    p.add_argument('--previous-data-backup',type=Path,
        help='Existing Data disk saved for migration before its filesystem was expanded')
    p.add_argument('--chunk-mib',type=int,default=1024,
        help='Bound recovery /tmp usage while transferring a large Data disk')
    p.add_argument('--transfer-dir',type=Path,
        help='Host scratch directory for bounded transfer chunks')
    p.add_argument('--resume',action='store_true',
        help='Resume completed chunks from this output directory before directory commit')
    a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    image=a.data_image.resolve();size=image.stat().st_size
    if not size or size%4096 or a.offset%(1024**2):
        raise SystemExit('Data disk must be whole 4 KiB, offset whole MiB')
    if not 1<=a.chunk_mib<=2048:
        raise SystemExit('Transfer chunks must be between 1 and 2048 MiB')
    adb=[str(a.adb),'-P',str(a.port),'-s',a.serial]

    def run(args,**kw):return subprocess.run(args,check=True,**kw)
    def shell(command):
        return run(adb+['shell',command],capture_output=True,text=True).stdout.strip()
    state=run(adb+['get-state'],capture_output=True,text=True).stdout.strip()
    if state!='recovery':
        raise SystemExit('Recovery mode is required')
    target=shell('readlink -f '+TARGET)
    geometry=dict(device=target,bytes=int(shell('blockdev --getsize64 '+TARGET)),
        start_512=int(shell('cat /sys/class/block/sda13/start')),
        sectors_512=int(shell('cat /sys/class/block/sda13/size')),
        logical_block_bytes=int(shell('cat /sys/class/block/sda/queue/logical_block_size')))
    if (target!='/dev/block/sda13' or geometry['bytes']!=PARTITION_BYTES or
        geometry['start_512']!=FIRST_4K*8 or geometry['sectors_512']*512!=PARTITION_BYTES or
        geometry['logical_block_bytes']!=4096):
        raise SystemExit('Pong userdata geometry differs; refusing writes')
    if shell('ls /sys/class/block/sda13/holders'):
        raise SystemExit('userdata has active device-mapper holders')
    mounts=shell('cat /proc/mounts').splitlines()
    if any(line.split()[0] in (target,TARGET) for line in mounts):
        raise SystemExit('userdata must be unmounted in recovery')
    raw=run(adb+['exec-out','dd','if='+TARGET,'bs=4096','count=1','status=none'],capture_output=True).stdout
    if len(raw)!=4096 or raw[:8]!=b'SHNGROOT':
        raise SystemExit('Existing SHNGROOT directory unavailable')
    version,count,total=struct.unpack_from('<IIQ',raw,8)
    if version!=1 or count not in (2,3) or total>PARTITION_BYTES:
        raise SystemExit('Unsupported SHNGROOT directory')
    records=[]
    for i in range(count):
        name,offset,length,block,flags=struct.unpack_from('<32sQQII',raw,24+128*i)
        name=name.split(b'\0')[0].decode('ascii')
        if block!=512 or flags!=1 or not length or offset<4096 or offset+length>total:
            raise SystemExit('Invalid existing window geometry')
        records.append(dict(name=name,offset=offset,size=length))
    names={r['name'] for r in records}
    if len(names)!=count or names not in ({'OS','SystemOS'},{'OS','SystemOS','Data'}):
        raise SystemExit('Expected the existing Pong OS/SystemOS and optional Data windows')
    previous=next((r for r in records if r['name']=='Data'),None)
    if previous:
        if not (a.replace_existing_data or a.update_existing_data):
            raise SystemExit('Persistent Data already exists; never reset it implicitly')
        if a.offset!=previous['offset']:
            raise SystemExit('Existing Data must keep its offset')
        if a.update_existing_data and size!=previous['size']:
            raise SystemExit('A Data runtime update must keep its size')
        if a.replace_existing_data and size<=previous['size']:
            raise SystemExit('Expansion must increase Data size')
        if (not a.previous_data_backup or
            a.previous_data_backup.resolve()==image or
            a.previous_data_backup.stat().st_size!=previous['size']):
            raise SystemExit('A separate, original-size Data migration backup is required')
    elif a.replace_existing_data or a.update_existing_data:
        raise SystemExit('No existing Data window to update')
    end=a.offset+size
    occupied=max(r['offset']+r['size'] for r in records if r['name']!='Data')
    if (a.offset<occupied+16*1024**2 or end>PARTITION_BYTES-1024**3 or
        size>PARTITION_BYTES//4):
        raise SystemExit('Data must lie beyond root plus guard and below the tail reserve')
    before=out/'shngroot-before.bin'
    record=out/'installation.json'
    start=0
    if a.resume:
        checkpoint=json.loads(record.read_text())
        if (checkpoint['state'] not in ('prepared','writing_data') or
            checkpoint['geometry']!=geometry or checkpoint['existing_windows']!=records or
            checkpoint['data']!=dict(offset=a.offset,size=size,
                volume_uuid=a.volume_uuid,image=str(image)) or before.read_bytes()!=raw):
            raise SystemExit('Resume checkpoint does not match the pending installation')
        start=checkpoint['completed_bytes']
        if not 0<=start<=size or (start!=size and start%(a.chunk_mib*1024**2)):
            raise SystemExit('Resume checkpoint is not at a completed chunk boundary')
    else:
        before.write_bytes(raw)
    header=bytearray(raw)
    struct.pack_into('<IIQ',header,8,1,count if previous else count+1,max(total,end))
    # This is the 4 KiB directory format checksum, not a disk-image digest.
    # Mutable Data has no image digest; existing OS/SystemOS records stay intact.
    data_index=next((i for i,r in enumerate(records) if r['name']=='Data'),count)
    struct.pack_into('<32sQQII32s40s',header,24+128*data_index,b'Data',a.offset,size,512,1,bytes(32),bytes(40))
    header[4064:]=hashlib.sha256(header[:4064]).digest()
    after=out/'shngroot-with-data.bin';after.write_bytes(header)
    plan=dict(geometry=geometry,existing_windows=records,data=dict(offset=a.offset,size=size,
        volume_uuid=a.volume_uuid,image=str(image)),image_bytes=end,
        image_verification='skipped by request',readback_performed=False,boot_performed=False,
        backup=str(before),state='prepared',
        operation='update_existing_data' if a.update_existing_data else 'expand_existing_data' if previous else 'install_data',
        previous_data_backup=str(a.previous_data_backup.resolve()) if previous else None,
        previous_data=previous,completed_bytes=start)
    record.write_text(json.dumps(plan,indent=2)+'\n')
    remote='/tmp/pong-ufs-data-install.cdr';remote_header='/tmp/pong-shngroot-with-data.bin'
    print('Installing Data only: offset='+hex(a.offset)+' bytes='+str(size),flush=True)
    chunk_bytes=a.chunk_mib*1024**2
    transfer=a.transfer_dir.resolve() if a.transfer_dir else out
    transfer.mkdir(parents=True,exist_ok=True)
    chunk=transfer/'data-transfer-chunk.cdr'
    # ADB push is faster than shell stdin on this recovery. Transfer bounded
    # chunks so expanding Data does not require an equally large RAM /tmp.
    zero=bytes(8*1024**2)
    with image.open('rb') as source:
        source.seek(start)
        for at in range(start,size,chunk_bytes):
            length=min(chunk_bytes,size-at)
            if size<=chunk_bytes:
                part=image
            else:
                part=chunk
                with chunk.open('wb') as target_file:
                    remaining=length
                    while remaining:
                        buf=source.read(min(8*1024**2,remaining))
                        if not buf:raise SystemExit('Unexpected EOF while preparing Data transfer')
                        # Preserve zero holes when preparing the transfer file;
                        # adb still sends logical zero bytes to overwrite UFS.
                        # This avoids writing gigabytes of zeroes to the host.
                        if buf==zero[:len(buf)]:target_file.seek(len(buf),1)
                        else:target_file.write(buf)
                        remaining-=len(buf)
                    target_file.truncate(length)
            print('Data transfer '+str(at//1024**2)+'..'+str((at+length)//1024**2)+' MiB',flush=True)
            run(adb+['push','-z','lz4',str(part),remote])
            block=1024**2 if length%(1024**2)==0 else 4096
            print(shell('dd if='+shlex.quote(remote)+' of='+TARGET+' bs='+str(block)+' seek='+
                str((a.offset+at)//block)+' count='+str(length//block)+' conv=notrunc,fsync'),flush=True)
            shell('rm -f '+shlex.quote(remote))
            if part==chunk:chunk.unlink()
            plan['completed_bytes']=at+length;plan['state']='writing_data'
            record.write_text(json.dumps(plan,indent=2)+'\n')
    plan['state']='data_written_and_synced';record.write_text(json.dumps(plan,indent=2)+'\n')
    if header!=raw:
        run(adb+['push',str(after),remote_header])
        print(shell('dd if='+shlex.quote(remote_header)+' of='+TARGET+' bs=4096 count=1 conv=notrunc,fsync'),flush=True)
    shell('sync')
    shell('rm -f '+shlex.quote(remote)+' '+shlex.quote(remote_header))
    plan['state']='installed';record.write_text(json.dumps(plan,indent=2)+'\n')
    print('Persistent Data installed; no image readback or phone boot performed',flush=True)


if __name__=='__main__':main()
