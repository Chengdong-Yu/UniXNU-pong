#!/usr/bin/env python3
"""Describe selected Objective-C layouts from the supplied 20G81 cache.
Metadata reads only, no image hashes, code execution or host/device tests.
"""
import argparse
import json
import mmap
from pathlib import Path
import struct

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cache-directory',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--device-class',type=lambda x:int(x,0),default=0x1e1bb7a58)
p.add_argument('--expected-class',default='_MTLDevice',help='also describe another selected20G81 class')
a=p.parse_args()
maps=[];slides=[];files=[];main=None
for path in sorted(a.cache_directory.glob('dyld_shared_cache_arm64*')):
    if path.suffix=='.symbols': continue
    f=path.open('rb');data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
    files.append((f,data))
    if path.name=='dyld_shared_cache_arm64': main=data
    off,count=struct.unpack_from('<II',data,16)
    for i in range(count):
        va,size,at,_,_=struct.unpack_from('<QQQII',data,off+32*i)
        maps.append((va,va+size,at,data))
    off,count=struct.unpack_from('<II',data,312)
    for i in range(count):
        va,size,_,at,n,_,_,_=struct.unpack_from('<QQQQQQII',data,off+56*i)
        if n:
            version=struct.unpack_from('<I',data,at)[0]
            if version!=2: raise RuntimeError('unsupported metadata slide format '+str(version))
            mask,base=struct.unpack_from('<QQ',data,at+24)
            slides.append((va,va+size,mask,base))

def read(va,n):
    for start,end,off,data in maps:
        if start<=va and va+n<=end: return data[off+va-start:off+va-start+n]
    raise RuntimeError('unbacked metadata address '+hex(va))

def ptr(va):
    raw=struct.unpack('<Q',read(va,8))[0]
    for start,end,mask,base in slides:
        if start<=va<end:
            payload=raw&(~mask&0xffffffffffffffff)
            return base+payload if payload else 0
    return raw

def string(va):
    out=bytearray()
    for i in range(4096):
        c=read(va+i,1)
        if c==b'\0': return out.decode()
        out+=c
    raise RuntimeError('unterminated metadata string')

region=struct.unpack_from('<Q',main,224)[0]
opts=region+struct.unpack_from('<Q',main,464)[0]
if struct.unpack('<I',read(opts,4))[0]!=1: raise RuntimeError('unsupported ObjC optimization metadata')
selectors=region+struct.unpack('<Q',read(opts+48,8))[0]

def methods(va):
    if not va: return []
    if va&1:
        header=va&~1;stride,count=struct.unpack('<II',read(header,8))
        if stride!=8 or not 0<count<=4096: raise RuntimeError('unsupported category metadata')
        entry=header+count*8
        va=entry+(struct.unpack('<q',read(entry,8))[0]>>16)
    flags,count=struct.unpack('<II',read(va,8));stride=flags&0xfffc
    if count>4096 or stride!=(12 if flags&0x80000000 else 24): raise RuntimeError('unsupported method metadata')
    result=[]
    for i in range(count):
        at=va+8+stride*i
        if flags&0x80000000:
            nameoff,typeoff,impoff=struct.unpack('<Iii',read(at,12))
            name=selectors+nameoff if flags&0x40000000 else ptr(at+struct.unpack('<i',read(at,4))[0])
            types=at+4+typeoff;imp=at+8+impoff
        else: name,types,imp=(ptr(at+j) for j in (0,8,16))
        result.append(dict(selector=string(name),encoding=string(types),implementation=hex(imp)))
    return result

def layout(va):
    ro=ptr(va+32)&0x7ffffffffff8
    flags,start,size=struct.unpack('<III',read(ro,12))
    if size<start or size>1048576: raise RuntimeError('invalid class size')
    result=dict(address=hex(va),name=string(ptr(ro+24)),instance_start=start,instance_size=size,
        superclass=hex(ptr(va+8)),methods=methods(ptr(ro+32)))
    ivars=ptr(ro+48)
    result['ivars']=[]
    if ivars:
        stride,count=struct.unpack('<II',read(ivars,8))
        if stride!=32 or count>2048: raise RuntimeError('unsupported ivar metadata')
        for i in range(count):
            at=ivars+8+stride*i
            result['ivars'].append(dict(name=string(ptr(at+8)),encoding=string(ptr(at+16)),
                offset=struct.unpack('<I',read(ptr(at),4))[0]))
    return result

device=layout(a.device_class);meta=layout(ptr(a.device_class))
if device['name']!=a.expected_class: raise RuntimeError('requested class differs: '+device['name'])
report=dict(system='iOS16.6.1/20G81',cache_uuid=main[88:104].hex(),
    device=device,metaclass=meta,runtime_observed=False,hardware_acceleration=False)
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(report,indent=2)+'\n')
print('Actual 20G81 '+device['name']+':',device['instance_size'],'bytes,',len(device['methods']),'own methods')
