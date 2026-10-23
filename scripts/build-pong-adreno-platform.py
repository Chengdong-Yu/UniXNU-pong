#!/usr/bin/env python3
"""Build SM8475 GMU/CP/native-BO support and its pong_adreno_start=1 entry.
Hardware startup is attempted only by the kernel boot-arg entry. The native
client is published only after real CP init and authenticated nonsecure fences.
This compiler does not access a phone or run image verification.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference-dir',type=Path,
        help='regenerate hardware facts from the matching vendor gen7_reg.h')
    p.add_argument('--output-dir',required=True,type=Path)
    p.add_argument('--tree',type=Path,required=True,help='UniXNU kernel build tree')
    p.add_argument('--firmware-dir',type=Path,required=True,
        help='locally obtained SM8475 firmware directory; do not commit firmware blobs')
    p.add_argument('--developer-dir',type=Path,
        help='optional Xcode developer directory; defaults to xcode-select')
    a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    source=ROOT/'drivers/PongAdreno'
    regs=(a.reference_dir/'gen7_reg.h').read_text() if a.reference_dir else (source/'PongAdrenoCpProfile.h').read_text()
    needed=sorted(set(re.findall(r'\bGEN7_[A-Z0-9_]+\b',(source/'PongAdrenoGmu.c').read_text())))
    values={}
    for name in needed:
        match=re.search(r'^#define\s+'+name+r'\s+(0x[0-9a-fA-F]+)[uU]?\s*$',regs,re.M)
        if not match:raise RuntimeError('Missing actual Pong register '+name)
        values[name]=int(match[1],16)
    header='/* Register facts from NothingOSS SM8475 gen7_reg.h. Dword offsets. */\n'
    header+='\n'.join('#define '+name+' '+hex(value)+'u' for name,value in values.items())+'\n'
    (out/'PongAdrenoRegisters.h').write_text(header)
    env=dict(os.environ)
    if a.developer_dir: env['DEVELOPER_DIR']=str(a.developer_dir)
    sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
    commands=[];objects=[]
    sources=['AdrenoFirmware','PongAdrenoHfi','PongAdrenoGmu','PongAdrenoSmmu',
             'PongAdrenoPower','PongAdrenoClocks','PongAdrenoZap']
    sources+=['submit/AdrenoPM4']
    sources+=['ArmSmmu4K/'+name for name in ['ArmSmmu4K','ArmSmmu4KContext',
        'ArmSmmu4KProgram','ArmSmmu4KStreams','ArmSmmu4KAudit',
        'ArmSmmu4KLive','ArmSmmu4KTableMemory']]
    for name in sources:
        obj=out/(Path(name).name+'.o')
        cmd=['xcrun','clang','-target','arm64-apple-ios16.0','-isysroot',sdk,
             '-mkernel','-ffreestanding','-fno-builtin','-fno-stack-protector','-mgeneral-regs-only',
             '-fno-asynchronous-unwind-tables','-Os',
             '-Wall','-Wextra','-Werror','-I'+str(out),'-I'+str(source),
             '-I'+str(source/'ArmSmmu4K'),
             '-c',str(source/(name+'.c')),'-o',str(obj)]
        subprocess.run(cmd,env=env,check=True);commands.append(cmd);objects.append(obj)
    # Embed only the actual captured GMU firmware for cold startup. No hashes,
    # board substitution, optional diagnostic payload or dummy firmware.
    data=out/'PongAdrenoFirmware.generated.c'
    embedded='#include <stddef.h>\n#include <stdint.h>\n'
    for name,file in [('PongAdrenoGmuFirmware','gmu_gen70000.bin'),
        ('PongAdrenoSqeFirmware','a730_sqe.fw'),('PongAdrenoZapMdt','a730_zap.mdt'),
        ('PongAdrenoZapCode','a730_zap.b01'),('PongAdrenoZapSignature','a730_zap.b02')]:
        firmware=(a.firmware_dir/file).read_bytes()
        embedded+='const uint8_t '+name+'[]={\n'+''.join(
            ','.join(str(x) for x in firmware[i:i+32])+',\n' for i in range(0,len(firmware),32))
        embedded+='};\nconst size_t '+name+'Bytes=sizeof('+name+');\n'
    data.write_text(embedded)
    obj=out/'PongAdrenoFirmware.generated.o'
    cmd=commands[0].copy();cmd[cmd.index('-c')+1]=str(data);cmd[-1]=str(obj)
    subprocess.run(cmd,env=env,check=True);commands.append(cmd);objects.append(obj)
    framework=a.tree/'.cache/fakeroot/System/Library/Frameworks/Kernel.framework/Versions/A'
    obj=out/'PongAdrenoPlatform.o'
    cmd=['xcrun','clang++','-target','arm64-apple-ios16.0','-isysroot',sdk,
        '-mkernel','-ffreestanding','-fno-builtin','-fno-stack-protector','-mgeneral-regs-only',
        '-fno-exceptions','-fno-rtti','-fno-stack-check','-std=gnu++17','-fblocks','-Os',
        '-DKERNEL=1','-DKERNEL_PRIVATE=1','-DXNU_KERNEL_PRIVATE=1','-DARM64_BOARD_CONFIG_PONG=1',
        '-I'+str(framework/'PrivateHeaders'),'-I'+str(framework/'Headers'),
        '-I'+str(out),'-I'+str(source),'-I'+str(source/'ArmSmmu4K'),
        '-Wno-deprecated-declarations',
        '-c',str(source/'PongAdrenoPlatform.cpp'),'-o',str(obj)]
    subprocess.run(cmd,env=env,check=True);commands.append(cmd);objects.append(obj)
    cpp_template=cmd.copy()
    names=['PongAdrenoCp','PongAdrenoClient']+['submit/'+n for n in [
        'NativeAdrenoRing','NativeAdrenoCpInit','NativeAdrenoTimer',
        'NativeAdrenoCpu','NativeAdrenoMemory','NativeAdrenoSmmuMemory',
        'NativeAdrenoSmmuContext','NativeAdrenoVm','NativeAdrenoSubmit','NativeAdrenoUserClient']]
    for name in names:
        obj=out/(Path(name).name+'.o');cmd=cpp_template.copy()
        cmd[cmd.index('-c')+1]=str(source/(name+'.cpp'));cmd[-1]=str(obj)
        cmd+=['-I'+str(source/'submit')]
        subprocess.run(cmd,env=env,check=True);commands.append(cmd);objects.append(obj)
    archive=out/'libPongAdrenoPlatform.a'
    subprocess.run(['xcrun','ar','rcs',str(archive),*[str(x) for x in objects]],env=env,check=True)
    report=dict(archive=str(archive),commands=commands,
        chip_id='0x07030002',gmem_bytes=0x200000,gmu_firmware='gmu_gen70000.bin',
        sqe_firmware='a730_sqe.fw',zap='a730_zap',
        hfi_table_words=210,hfi_queue_offset_bytes=848,active_queues=3,
        cpu_page_bytes=4096,registers=values,automatic_boot_entry=False,
        startup_boot_argument='pong_adreno_start=1',accelerator_published=False,
        cp_ring_compiled=True,cp_board_connected=True,dms_default=False,
        native_user_client='NativeAdrenoGpuPublished',metal_publication=False,
        timestamp_gmu_offsets=[0x16220,0x16224],
        gpu_streams=[0,0x400],gmu_streams=[5,0x405],
        smmu_physical='0x03da0000',smmu_context_limits=[21,24],
        boot_link_status='controlled by CONFIG_XNU_PONG_ADRENO_PLATFORM',
        hardware_qualified=False,image_verification='skipped by request')
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(archive)

if __name__=='__main__':main()
