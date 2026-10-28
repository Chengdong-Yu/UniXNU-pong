#!/usr/bin/env python3
"""Derive the two real amplifier register profiles from a local Pong CNT."""
import argparse,struct,json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--container',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();b=a.container.read_bytes()
if len(b)<54 or b[:2]!=b'PM' or struct.unpack_from('<H',b,40)[0]!=2:raise RuntimeError('Pong two-amplifier container required')
def ptr(o):
 if o<0 or o+4>len(b):raise RuntimeError('CNT pointer outside file')
 x=struct.unpack_from('<I',b,o)[0];off=x&0xffffff
 if off>=len(b):raise RuntimeError('CNT descriptor outside file')
 return off,x>>24
def bits(at,count):
 v=[]
 for k in range(count):
  off,kind=ptr(at+4*k)
  if kind==17:break # profile defaults after this marker are exit values
  if kind==16:
   value,field=struct.unpack_from('<HH',b,off);width=(field&15)+1;shift=(field>>4)&15
   if shift+width>16 or value>=(1<<width):raise RuntimeError('Invalid register bitfield')
   v.append((field,value))
  elif kind not in (1,5):raise RuntimeError('Unsupported amplifier descriptor '+str(kind))
 return v
arrays=[]
for i in range(2):
 d,kind=ptr(46+i*4)
 if kind or b[d+2]!=0x34+i:raise RuntimeError('Actual Pong amplifier addresses required')
 # Device fields precede its profile pointers and the dummy ROM patch.
 device=[];profile=None
 for k in range(b[d]):
  off,kind=ptr(d+12+4*k)
  if kind==16:
   value,field=struct.unpack_from('<HH',b,off);device.append((field,value))
  if kind==1:
   name,_=ptr(off+4);end=b.index(0,name);text=b[name:end].decode('ascii')
   if text=='speaker':profile=bits(off+8,b[off]-1)
 if not profile:raise RuntimeError('Actual speaker profile absent')
 arrays.append(device+profile)
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text('struct PongAmpField {unsigned field,value;};\n'+''.join('static constexpr PongAmpField kPongAmp%d[]={\n'%i+''.join('{0x%x,0x%x},\n'%x for x in v)+'};\n' for i,v in enumerate(arrays)))
print(json.dumps(dict(amplifiers=2,source=str(a.container),fields=[len(x) for x in arrays],hash_verification=False)))
