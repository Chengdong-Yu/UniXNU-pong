#!/usr/bin/env python3
"""Derive storage imports from the 22H374 kernel; never reuse 20G81 addresses."""
import argparse,json,os,re,shutil,struct,subprocess,sys
from pathlib import Path
DEVICE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(DEVICE/'scripts'))
sys.path.insert(0,str(DEVICE/'kits/NativePrelink'))
import pong_paths
import native_prelink as np
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--tree',type=Path,required=True)
p.add_argument('--kernel',type=Path,default=DEVICE/'apple/ipados18-22H374/components/kernelcache.release.ipad7c.decompressed/kernelcache.release.ipad7c.decompressed')
a=p.parse_args();out=DEVICE/'mac-build/storage-abi18';out.mkdir(parents=True,exist_ok=True)
source=pong_paths.storage_headers()
IOSTORAGE_323='1b96f8f04b9dff4dc0277ac43625bf694e62c5aa'
if not source.is_dir() or not any(source.glob('*.h')):
    raise SystemExit(f'missing IOStorageFamily-323 headers: {source}')
revision=subprocess.check_output(['git','-C',source,'rev-parse','HEAD'],text=True).strip()
if revision!=IOSTORAGE_323:
    raise SystemExit(f'IOStorageFamily-323 must be peeled commit {IOSTORAGE_323} (annotated tag de8da905), found {revision}')
headers=out/'include/IOKit/storage';headers.mkdir(parents=True,exist_ok=True)
for f in source.glob('*.h'):shutil.copyfile(f,headers/f.name)
env=dict(os.environ,DEVELOPER_DIR=str(pong_paths.developer_dir()))
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
macsdk=Path(subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],env=env,text=True).strip())
shutil.copyfile(macsdk/'usr/include/TargetConditionals.h',out/'include/TargetConditionals.h')
fw=a.tree/'.cache/fakeroot/System/Library/Frameworks/Kernel.framework/Versions/A';obj=out/'reference.o'
cmd=['xcrun','--sdk','iphoneos','clang++','-target','arm64-apple-ios18.0','-isysroot',sdk,'-mkernel','-ffreestanding','-fno-builtin','-fno-exceptions','-fno-rtti','-std=gnu++17','-fblocks','-O2','-g','-DKERNEL=1','-DKERNEL_PRIVATE=1','-DXNU_KERNEL_PRIVATE=1','-I'+str(out/'include'),'-I'+str(fw/'PrivateHeaders'),'-I'+str(fw/'Headers'),'-Wno-deprecated-declarations','-Wno-c99-designator','-c',str(source/'IOBlockStorageDevice.cpp'),'-o',str(obj)]
subprocess.run(cmd,env=env,check=True)
b=obj.read_bytes();m=np.macho(b);sc=next(o for c,o,l in m['commands'] if c==2);sy,n,st,sz=struct.unpack_from('<4I',b,sc+8)
ss=[]
for i in range(n):
 ix,k,_,_,v=struct.unpack_from('<IBBHQ',b,sy+i*16);ss.append((b[st+ix:b.index(b'\0',st+ix)].decode(),v,k))
vt=next(v for name,v,k in ss if name=='__ZTV20IOBlockStorageDevice');rel={}
for g in m['segments']:
 for s in g['sections']:
  if s['va']<=vt<s['va']+s['size']:
   ro,rc=struct.unpack_from('<II',b,s['command_offset']+56)
   for i in range(rc):
    at,w=struct.unpack_from('<II',b,ro+i*8)
    if w>>27&1 and w>>28==0 and w>>25&3==3:rel[s['va']+at]=ss[w&0xffffff][0]
methods=[]
while vt+16+len(methods)*8 in rel:methods.append(rel[vt+16+len(methods)*8])
if not 150<len(methods)<256:raise SystemExit('Unexpected public storage vtable size')
b=np.arm64_image(a.kernel.read_bytes());m=np.macho(b);ex=np.exports(b,m);known={n:v for v,ns in ex.items() for n in ns}
s=next(s for g in m['segments'] for s in g['sections'] if s['name']=='__info')
info=np.unserialize_xml(b[s['fileoff']:s['fileoff']+s['size']].rstrip(b'\0'))
e=next(e for e in info['_PrelinkInfoDictionary'] if e.get('CFBundleIdentifier')=='com.apple.iokit.IOStorageFamily')
km=np.macho(b,np.backed_offset(m['segments'],e['_PrelinkExecutableLoadAddr'],32,len(b)))
const=next(s for s in km['segments'] if s['name']=='__DATA_CONST');text=next(s for s in km['segments'] if s['name']=='__TEXT_EXEC')
def read(va,size):
 at=np.backed_offset(m['segments'],va,size,len(b));return b[at:at+size]
blob=read(const['va'],const['size']);matches=[];count=len(methods)
for at in range(0,len(blob)-(count+2)*8+1,8):
 if blob[at:at+16]!=bytes(16):continue
 pointers=struct.unpack_from('<'+'Q'*count,blob,at+16)
 if sum(name in known for name in methods)>count//2 and all(pointers[i]==known[name] for i,name in enumerate(methods) if name in known):matches.append((const['va']+at,pointers))
if len(matches)!=1:raise SystemExit(f'22H374 storage interface does not match the public class: {len(matches)} candidates')
table,pointers=matches[0]
def branch(pc,w):
 v=w&0x3ffffff;return pc+(v-(1<<26) if v&(1<<25) else v)*4
def adrp(pc,w):
 v=((w>>5&0x7ffff)<<2)|(w>>29&3);return (pc&~4095)+(v-(1<<21) if v&(1<<20) else v)*4096
# New kernelcache LTO can inline the abstract base constructor/destructor.
# Their public definitions only forward to IOService; emit source thunks in
# bind-ipados18-storage.py and bind the real original vtable/metaclass.
x,add,ret=struct.unpack('<3I',read(pointers[7],12))
if x&0x9f00001f!=0x90000000 or add&0xffc003ff!=0x91000000 or ret!=0xd65f03c0:raise SystemExit('22H374 storage metaclass getter layout changed')
meta=adrp(pointers[7],x)+(add>>10&4095)
base=min(s['va'] for s in m['segments'] if s['name'].startswith('__PRELINK') and s['size']);fixed=0xfffffff00b000000;slide=fixed-base
bindings={name:pointers[i]+slide for i,name in enumerate(methods) if name not in known and name!='___cxa_pure_virtual'}
bindings.update({'__ZN20IOBlockStorageDevice10gMetaClassE':meta+slide,'__ZTV20IOBlockStorageDevice':table+slide})
report=dict(build='22H374',darwin='24.6.0',source=str(source),method_slots=count,fixed_prelink_base=hex(fixed),bindings={k:hex(v) for k,v in bindings.items()},methods=[dict(slot=i,reference_name=n) for i,n in enumerate(methods)],image_verification='skipped by request')
(out/'bindings-layout.json').write_text(json.dumps(report,indent=2)+'\n');print(f'Derived {len(bindings)} original storage imports from 22H374 ({count} slots)')
