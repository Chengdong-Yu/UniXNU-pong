#!/usr/bin/env python3
"""Link Pong UFS to the selected Darwin 24 storage family without old addresses."""
import argparse,json,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'kits/NativePrelink'))
import native_prelink as np
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--object',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
r=json.loads((ROOT/'mac-build/storage-abi18/bindings-layout.json').read_text())
if r['build']!='22H374':raise SystemExit('Wrong target storage bindings')
b=a.object.read_bytes();m=np.macho(b);at=next(o for c,o,l in m['commands'] if c==2);sy,n,st,sz=struct.unpack_from('<4I',b,at+8)
undefined=[]
for i in range(n):
 ix,k,_,_,v=struct.unpack_from('<IBBHQ',b,sy+i*16);name=b[st+ix:b.index(b'\0',st+ix)].decode()
 if k==1 and 'IOBlockStorageDevice' in name:undefined.append(name)
ctor='__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass';dtor='__ZN20IOBlockStorageDeviceD2Ev'
missing=set(undefined)-r['bindings'].keys()-{ctor,dtor}
if missing:raise SystemExit('Unmapped Darwin 24 storage imports: '+repr(missing))
text=''.join(f'.globl {n}\n.set {n}, {int(r["bindings"][n],16):#x}\n' for n in sorted(set(undefined)-{ctor,dtor}))
vptr=int(r['bindings']['__ZTV20IOBlockStorageDevice'],16)+16
text+='\n.text\n.p2align 2\n'
text+=f'.globl {ctor}\n{ctor}:\n stp x29,x30,[sp,#-16]!\n mov x29,sp\n bl __ZN9IOServiceC2EPK11OSMetaClass\n'
text+=f' movz x8, #{vptr&65535}\n'+''.join(f' movk x8, #{vptr>>shift&65535}, lsl #{shift}\n' for shift in (16,32,48))
text+=' str x8,[x0]\n ldp x29,x30,[sp],#16\n ret\n'
text+=f'.globl {dtor}\n{dtor}:\n b __ZN9IOServiceD2Ev\n'
source=a.output/'original-bindings.S';source.write_text(text)
subprocess.run(['xcrun','--sdk','iphoneos','clang','-target','arm64-apple-ios18.0','-c',str(source),'-o',str(a.output/'original-bindings.o')],check=True)
r.update(bindings={n:r['bindings'][n] for n in undefined if n in r['bindings']},constructor='public IOBlockStorageDevice forwarding definition, Darwin 24 IOService ABI',image_verification='skipped by request')
(a.output/'bindings.json').write_text(json.dumps(r,indent=2)+'\n');print('Linked UFS with actual 22H374 storage dispatch and public base constructor/destructor')
