#!/usr/bin/env python3
"""Describe 22H374 UFS windows from existing images.

Each window's SHA-256 is stored in the directory entry. The images themselves
stay where they are; this writer does not copy them into a new disk image.
"""
import argparse,hashlib,json,struct,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];MIB=1<<20
sys.path.insert(0,str(ROOT/'scripts'))
import pong_paths
FIRST=0x413248;LAST=0x75a40ea

def digest_file(path):
    digest=hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(8*MIB), b''):
            digest.update(chunk)
    return digest.digest()
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--userland',type=Path,default=pong_paths.prepared_userland())
p.add_argument('--systemos',type=Path,default=ROOT/'apple/ipados18-22H374/prepared/pong-ipados18-systemos.cdr')
p.add_argument('--output-dir',type=Path,default=ROOT/'out/ipados18/ufs')
p.add_argument('--volume-layout',type=Path,default=pong_paths.volume_layout())
a=p.parse_args();state=json.loads((a.userland/'result.json').read_text())
if state.get('build')!='22H374' or not (a.userland/'apply-complete.json').exists():raise SystemExit('Apply the matching iPadOS18 userland first')
volumes=json.loads(a.volume_layout.read_text());cursor=MIB;records=[]
for name,path in [('OS',a.userland/'pong-ipados18-system.cdr'),('SystemOS',a.systemos),('Data',a.userland/'pong-ipados18-data.cdr')]:
 path=path.resolve();size=path.stat().st_size
 if size<4096 or size%4096:raise SystemExit(f'{name} must be a native raw 4 KiB-aligned disk/container')
 if name=='Data':cursor=(cursor+16*MIB+MIB-1)//MIB*MIB
 digest=digest_file(path)
 records.append(dict(name=name,image=str(path),offset=cursor,size=size,block_bytes=512,sha256=digest.hex(),volume_uuid=volumes[{'OS':'os','SystemOS':'systemos','Data':'data'}[name]]['uuid']))
 cursor=(cursor+size+MIB-1)//MIB*MIB
if cursor>(LAST-FIRST+1)*4096:raise SystemExit('Windows exceed Pong userdata')
header=bytearray(4096);header[:8]=b'SHNGROOT';struct.pack_into('<IIQ',header,8,1,len(records),cursor)
for i,r in enumerate(records):struct.pack_into('<32sQQII32s40s',header,24+i*128,r['name'].encode(),r['offset'],r['size'],512,1,bytes.fromhex(r['sha256']),bytes(40))
# Directory checksum covers the 4064-byte header, which now includes each window SHA-256.
header[4064:]=hashlib.sha256(header[:4064]).digest()
a.output_dir.mkdir(parents=True,exist_ok=True);(a.output_dir/'shngroot-ipados18.bin').write_bytes(header)
result=dict(build='22H374',board='Pong/SM8475',target='/dev/block/by-name/userdata',partition_first_4k=FIRST,partition_last_4k=LAST,image_bytes=cursor,windows=records,image_verification='window sha256 recorded in the directory',partition_writes=False)
(a.output_dir/'ufs-layout.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
