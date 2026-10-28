#!/usr/bin/env python3
"""Link actual QCA6490 ath11k PCI/MHI and Pong Darwin24 network/platform code.

Firmware is read from the user's local recovery extraction, embedded only in
ignored build output, never committed here. No images are hashed/read back.
"""
import argparse,importlib.util,json,os,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--tree',type=Path,required=True)
 p.add_argument('--ath11k',type=Path,default=Path('/Volumes/Untitled/Pong-iPadOS18/wifi-ath11k-build/libPongAth11k.a'))
 p.add_argument('--firmware',type=Path,default=Path('/Volumes/Untitled/Pong-iPadOS18/pong-wifi-firmware'))
 a=p.parse_args();out=a.tree/'build/pong-wifi';out.mkdir(parents=True,exist_ok=True)
 shared=Path('/Volumes/PongDev/upstream/UniXNU-sheng')
 profile=json.loads((ROOT/'configs/pong-wifi-sm8475.json').read_text())
 if profile['soc']!='SM8475' or profile['pci']['device']!=0x1103:raise RuntimeError('Pong QCA6490 profile required')
 hardware=os.environ.get('PONG_HARDWARE_DTB')
 if hardware:
  spec=importlib.util.spec_from_file_location('pong_fdt',ROOT/'scripts/prepare-pong-wifi.py')
  fdt=importlib.util.module_from_spec(spec);spec.loader.exec_module(fdt)
  nodes=fdt.fdt_nodes(Path(hardware).read_bytes())
  host=next(v for v in nodes.values() if v.get('compatible')==b'qcom,pci-msm\0')
  phy=fdt.cells(host,'qcom,phy-sequence')
  if len(phy)%3:raise RuntimeError('Malformed selected Pong PHY sequence')
  profile['pcie']['phy_sequence']=[phy[i:i+3] for i in range(0,len(phy),3)]
  for field,key in (('phy_status_offset','qcom,phy-status-offset'),('phy_status_bit','qcom,phy-status-bit'),
                    ('phy_power_down_offset','qcom,phy-power-down-offset'),('aux_clock_mhz','qcom,aux-clk-freq'),
                    ('endpoint_latency_ms','qcom,ep-latency')):
   profile['pcie'][field]=fdt.cells(host,key)[0]
  profile['pcie']['max_clock_frequency_hz']=fdt.cells(host,'max-clock-frequency-hz')
  # The pinmux/register implementation is specific to the confirmed Cape
  # wiring. Refuse a different DTS instead of silently using stale pins.
  for key,pin in (('wlan-enable-gpios',80),('reset-gpios',94),('clkreq-gpios',95),('wake-gpios',96),('xo-clk-gpios',204)):
   if fdt.cells(host,key)[1]!=pin:raise RuntimeError('Selected DTS needs a new Pong pin mapping: '+key)
 header='struct PongWifiPhyEntry {unsigned off,value,delay;};\nstatic constexpr PongWifiPhyEntry kPongWifiPhy[]={\n'+''.join('{%d,%d,%d},\n'%tuple(e) for e in profile['pcie']['phy_sequence'])+'};\n'
 header+='struct PongWifiRail {const char *name;unsigned mv,maxMv,ppid,type,subtype;};\nstatic constexpr PongWifiRail kPongWifiRails[]={\n'
 for e in profile['rails']:
  physical=e.get('physical',{})
  ppid=(physical.get('spmi_slave',0)<<8)|physical.get('peripheral',0)
  header+='{%s,%d,%d,%d,%d,%d},\n'%(json.dumps(e['resource']),(e['request'][0]+999)//1000,e['request'][1]//1000,ppid,physical.get('type',0),physical.get('subtype',0))
 for e in profile['pcie']['analogue_rails']:
  header+='{%s,%d,%d,0,0,0},\n'%(json.dumps(e['resource']),(e['microvolts']+999)//1000,e['microvolts']//1000)
 header+='};\n'
 domain=profile['pcie']['power_domain']
 if (domain['control'],domain['collapse_vote'],domain['collapse_bit'])!=(0x17b004,0x162128,0):raise RuntimeError('Pong PCIe0 power domain required')
 header+='static constexpr unsigned kPongPcieCollapseVote=%du,kPongPcieCollapseMask=%du;\n'%(domain['collapse_vote']-0x100000,1<<domain['collapse_bit'])
 pcie=profile['pcie']
 if pcie['clock_names'][8]!='pcie_phy_refgen_clk' or pcie['max_clock_frequency_hz'][11]!=100000000:
  raise RuntimeError('Pong pci-msm refgen clock plan changed; adapt its RCG before building')
 for name,value in (('kPongPciePhyStatus',pcie['phy_status_offset']),
                    ('kPongPciePhyStatusMask',1<<pcie['phy_status_bit']),
                    ('kPongPciePhyPowerDown',pcie['phy_power_down_offset']),
                    ('kPongPcieAuxMhz',pcie['aux_clock_mhz']),
                    ('kPongPcieEpLatencyMs',pcie['endpoint_latency_ms']),
                    ('kPongPcieTargetSpeed',pcie['target_link_speed']),
                    ('kPongPciePerstDelayUs',pcie['perst_delay_us'])):
  header+='static constexpr unsigned %s=%du;\n'%(name,value)

 (out/'PongWifiInputs.h').write_text(header)
 fw=sorted(f for f in a.firmware.iterdir() if f.is_file())
 if not all((a.firmware/n).is_file() for n in ('amss.bin','amss20.bin','m3.bin','regdb.bin')):raise RuntimeError('Actual Pong modem firmware required')
 entries=[];at=(8+len(fw)*40+4095)&~4095
 for f in fw:
  name=f.name.encode();size=f.stat().st_size
  if len(name)>31:raise RuntimeError('firmware name too long')
  entries.append((name,at,size,f));at=(at+size+4095)&~4095
 pack=out/'PongWifiFirmware.bin'
 with pack.open('wb') as f:
  f.write(b'PWF2'+struct.pack('<I',len(entries)))
  for name,off,size,_ in entries:f.write(struct.pack('<32sII',name,off,size))
  for _,off,size,path in entries:
   f.seek(off);f.write(path.read_bytes())
 asm=out/'PongWifiFirmware.s';asm.write_text('.section __TEXT,__const\n.p2align 12\n.globl _pong_wifi_firmware\n_pong_wifi_firmware:\n.incbin "'+str(pack)+'"\n.globl _pong_wifi_firmware_end\n_pong_wifi_firmware_end:\n')
 framework=a.tree/'.cache/fakeroot/System/Library/Frameworks/Kernel.framework/Versions/A'
 env=dict(os.environ,DEVELOPER_DIR='/Volumes/PongDev/Applications/Xcode.app/Contents/Developer')
 sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
 flags=['-target','arm64-apple-ios18.0','-isysroot',sdk,'-mkernel','-ffreestanding','-fno-builtin','-fno-exceptions','-fno-rtti','-fno-stack-protector','-mgeneral-regs-only','-std=gnu++17','-fblocks','-O2','-DKERNEL=1','-DKERNEL_PRIVATE=1','-DXNU_KERNEL_PRIVATE=1','-DARM64_BOARD_CONFIG_PONG=1','-I'+str(framework/'PrivateHeaders'),'-I'+str(framework/'Headers'),'-I'+str(shared/'drivers/Ath12kWifi/include'),'-I'+str(shared/'package'),'-I'+str(out),'-Wno-deprecated-declarations']
 objects=[]
 for name in ('PongWifiPlatform','PongWifiNet'):
  obj=out/(name+'.o');cmd=['xcrun','clang++',*flags,'-c',str(ROOT/'drivers/PongWifi'/(name+'.cpp')),'-o',str(obj)]
  r=subprocess.run(cmd,env=env,capture_output=True,text=True);(out/(name+'.log')).write_text(r.stdout+r.stderr)
  if r.returncode:raise SystemExit(r.stderr)
  if name == 'PongWifiNet':
   # Register this owned object's network metaclasses after the radio starts.
   # Preserve all code/relocations; LLVM18 cannot globalize Mach-O symbols.
   raw=bytearray(obj.read_bytes());offset=32;symcmd=None
   for _ in range(struct.unpack_from('<I',raw,16)[0]):
    kind,length=struct.unpack_from('<II',raw,offset)
    if kind==0x19:
     for i in range(struct.unpack_from('<I',raw,offset+64)[0]):
      section=offset+72+80*i
      if raw[section:section+16].rstrip(b'\0')==b'__mod_init_func':
       raw[section:section+16]=b'__pong_init'.ljust(16,b'\0')
       section_flags=struct.unpack_from('<I',raw,section+64)[0];struct.pack_into('<I',raw,section+64,section_flags&~255)
    elif kind==2:symcmd=offset
    offset+=length
   if symcmd is None:raise RuntimeError('owned network object missing symbols')
   symoff,count,stroff,strsize=struct.unpack_from('<4I',raw,symcmd+8)
   if stroff+strsize!=len(raw):raise RuntimeError('unexpected network string table')
   for i in range(count):
    at=symoff+16*i;n=stroff+struct.unpack_from('<I',raw,at)[0];end=raw.index(0,n)
    if raw[n:end]==b'__GLOBAL__sub_I_PongWifiNet.cpp':
     new=b'_PongWifiNetStaticInit\0';struct.pack_into('<I',raw,at,strsize);raw[at+4]|=1
     raw.extend(new);struct.pack_into('<I',raw,symcmd+20,strsize+len(new));break
   else:raise RuntimeError('owned network constructor not found')
   obj=out/'PongWifiNet.late.o';obj.write_bytes(raw)
  objects.append(obj)
 obj=out/'PongWifiFirmware.o';subprocess.run(['xcrun','clang','-target','arm64-apple-ios18.0','-c',str(asm),'-o',str(obj)],env=env,check=True);objects.append(obj)
 obj=out/'PongQcomRpmh.o';subprocess.run(['xcrun','clang',*flags[:flags.index('-std=gnu++17')],'-O2','-I'+str(shared/'package'),'-c',str(shared/'package/QcomRpmh.c'),'-o',str(obj)],env=env,check=True);objects.append(obj)
 archive=out/'libPongWifiNative.a';archive.unlink(missing_ok=True)
 subprocess.run(['xcrun','libtool','-static','-o',str(archive),str(a.ath11k),*map(str,objects)],env=env,check=True)
 (out/'build-report.json').write_text(json.dumps(dict(board='Pong SM8475 QCA6490',guest='22H374',backend='Linux v7.0.14 ath11k',archive=str(archive),firmware_from_REC=str(a.firmware),firmware_files=len(entries),firmware_bytes=pack.stat().st_size,image_verification='skipped by request',runtime_observed=False),indent=2)+'\n')
 print(archive)
if __name__=='__main__':main()
