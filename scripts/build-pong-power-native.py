#!/usr/bin/env python3
"""Build SM8475 ADSP/GLINK, Nothing battery protocol and audio GPR for Darwin24.
No private firmware is embedded in the archive. No phone I/O or image audits.
"""
import argparse,os,json,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def late_init(obj, filename, symbol):
 raw=bytearray(obj.read_bytes());at=32;sym=None
 for _ in range(struct.unpack_from('<I',raw,16)[0]):
  kind,length=struct.unpack_from('<II',raw,at)
  if kind==0x19:
   for i in range(struct.unpack_from('<I',raw,at+64)[0]):
    sc=at+72+80*i
    if raw[sc:sc+16].rstrip(b'\0')==b'__mod_init_func':
     raw[sc:sc+16]=b'__pong_init'.ljust(16,b'\0');v=struct.unpack_from('<I',raw,sc+64)[0];struct.pack_into('<I',raw,sc+64,v&~255)
  elif kind==2:sym=at
  at+=length
 if sym is None:raise RuntimeError('Battery object has no symbol table')
 so,count,st,size=struct.unpack_from('<4I',raw,sym+8)
 if st+size!=len(raw):raise RuntimeError('Unexpected owned battery object layout')
 for i in range(count):
  at=so+16*i;p=st+struct.unpack_from('<I',raw,at)[0];z=raw.index(0,p)
  if raw[p:z]==('__GLOBAL__sub_I_'+filename).encode():
   new=('_'+symbol+'\0').encode();struct.pack_into('<I',raw,at,size);raw[at+4]|=1;raw.extend(new);struct.pack_into('<I',raw,sym+20,size+len(new));break
 else:raise RuntimeError('Battery initializer missing')
 dest=obj.with_suffix('.late.o');dest.write_bytes(raw);return dest

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tree',type=Path,required=True);a=p.parse_args()
 out=a.tree/'build/pong-power';out.mkdir(parents=True,exist_ok=True)
 framework=a.tree/'.cache/fakeroot/System/Library/Frameworks/Kernel.framework/Versions/A'
 env=dict(os.environ,DEVELOPER_DIR='/Volumes/PongDev/Applications/Xcode.app/Contents/Developer')
 sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
 flags=['-target','arm64-apple-ios18.0','-isysroot',sdk,'-mkernel','-ffreestanding','-fno-builtin','-fno-stack-protector','-mgeneral-regs-only','-fblocks','-O2','-DKERNEL=1','-DKERNEL_PRIVATE=1','-DXNU_KERNEL_PRIVATE=1','-DARM64_BOARD_CONFIG_PONG=1','-I'+str(framework/'PrivateHeaders'),'-I'+str(framework/'Headers'),'-I'+str(ROOT/'drivers/PongPower'),'-I'+str(out),'-I/Volumes/PongDev/upstream/UniXNU-sheng/package','-Wno-deprecated-declarations']
 subprocess.run([os.sys.executable,str(ROOT/'scripts/prepare-pong-audio.py'),'--container','/Volumes/Untitled/Pong-iPadOS18/pong-audio-inputs/tfa98xx.cnt','--output',str(out/'PongAmpProfiles.h')],check=True)
 objects=[]
 for name in ('QcomSmem.c','QcomSmp2p.c','QcomGlink.c','QcomMdt.c','QcomQmp.c','ShengBattmgr.c','ShengGpr.c','ShengAudioReach.c','GeniI2c.c','PongFirmware.c','PongAdsp.cpp','PongBattery.cpp','PongAudio.cpp','PongAudioService.cpp','PongPower.cpp','PongBacklight.cpp'):
  source=ROOT/'drivers/PongPower'/name;obj=out/(source.stem+'.o');cpp=source.suffix=='.cpp'
  cmd=['xcrun','clang++' if cpp else 'clang',*flags,*(['-std=gnu++17','-fno-exceptions','-fno-rtti'] if cpp else []),'-c',str(source),'-o',str(obj)]
  r=subprocess.run(cmd,env=env,capture_output=True,text=True);(out/(source.stem+'.log')).write_text(r.stdout+r.stderr)
  if r.returncode:raise SystemExit(r.stderr)
  if name=='PongBattery.cpp':obj=late_init(obj,name,'PongBatteryStaticInit')
  if name=='PongAudioService.cpp':obj=late_init(obj,name,'PongAudioServiceStaticInit')
  if name=='PongBacklight.cpp':obj=late_init(obj,name,'PongBacklightStaticInit')
  objects.append(obj)
 archive=out/'libPongPower.a';archive.unlink(missing_ok=True)
 subprocess.run(['xcrun','libtool','-static','-o',str(archive),*map(str,objects)],env=env,check=True)
 (out/'build-report.json').write_text(json.dumps(dict(board='SM8475 Pong',guest='22H374',adsp_pas=1,adsp_carveout=[0x85700000,0x2800000],ipcc=0xed18000,smem=0x80900000,battery='Nothing standard PMIC GLINK, single pack, read only',audio='adsp_apps APM graphs, SID1801, TFA9873 stereo PCM ring; no microphone endpoint',firmware='Local REC image staged to /pong/firmware/adsp.mbn',runtime_observed=False),indent=2)+'\n')
 print(archive)
if __name__=='__main__':main()
