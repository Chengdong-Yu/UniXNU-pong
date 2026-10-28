#!/usr/bin/env python3
"""Prelink matching 22H374 kexts and package them with Pong DarwinBoot24."""
import argparse,json,os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
sys.path.insert(0,str(ROOT/'kits/NativePrelink'))
import pong_paths
from emit_prelink import emit,COALESCED
from native_prelink import macho,plan

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--artifact-dir',type=Path,default=ROOT/'out/ipados18/kernel')
 p.add_argument('--original',type=Path,default=ROOT/'apple/ipados18-22H374/components/kernelcache.release.ipad7c.decompressed/kernelcache.release.ipad7c.decompressed')
 p.add_argument('--volume-layout',type=Path,default=pong_paths.volume_layout())
 p.add_argument('--output-dir',type=Path,required=True)
 p.add_argument('--cpus',type=int,choices=range(1,9),default=8,
                help='actual Pong CPU nodes; use --cpus 1 for isolated bring-up')
 p.add_argument('--window-size',type=lambda s:int(s,0),default=0x100000000)
 p.add_argument('--kernel-arg',action='append',default=[])
 a=p.parse_args()
 if (a.output_dir/'pong-xnu-initial.img').exists():raise SystemExit('Existing boot image retained; choose another --output-dir before prelinking')
 a.output_dir.mkdir(parents=True,exist_ok=True)
 report=json.loads((a.artifact_dir/'PONG_BUILD_RESULT.json').read_text())
 if not report['build_complete'] or report['profile']!='Pong-iPadOS18-22H374-Darwin24':raise SystemExit('A completed Darwin 24 build is required')
 active=Path(report['tree']).parent/'PONG_BUILD_RESULT.json'
 if active.exists():
  current=json.loads(active.read_text())
  if not current.get('build_complete') or current.get('log')!=report.get('log'):raise SystemExit('Kernel build/export still pending; package after the current build completes')
 native=(a.artifact_dir/'mach.development.pong').read_bytes();original=arm64_image(a.original.read_bytes());m=macho(original)
 bindings=json.loads((ROOT/'mac-build/native-ufs/bindings.json').read_text())
 if bindings['build']!='22H374':raise SystemExit('Storage imports belong to another restore')
 base=min(s['va'] for s in m['segments'] if s['name'] in COALESCED and s['size']);slide=int(bindings['fixed_prelink_base'],16)-base
 roots=['com.apple.kec.corecrypto','com.apple.kec.pthread','com.apple.filesystems.apfs','com.apple.kec.Compression','com.apple.AppleFSCompression.AppleFSCompressionTypeZlib']
 library={'com.apple.driver.AppleARMWatchdogTimer','com.apple.driver.AppleARMPlatform','com.apple.driver.ApplePMGR','com.apple.driver.AppleEffaceableStorage','com.apple.driver.AppleMobileFileIntegrity'}
 gpu=report['settings'].get('XNU_PONG_ADRENO_PLATFORM')=='1'
 if gpu:
  roots.append('com.apple.iokit.IOGPUFamily')
  library.update({'com.apple.iokit.IOGPUFamily','com.apple.iokit.IOSurface',
                  'com.apple.iokit.IOReportFamily','com.apple.iokit.IOSlowAdaptiveClockingFamily'})
 closure=plan(native,original,roots,slide,verify_images=False)
 library&=set(closure['components'])
 candidate,selection=emit(native,original,roots,slide,verify_images=False,library_only_components=library)
 kernel=a.output_dir/'kernel-ipados18.macho';kernel.write_bytes(candidate)
 (a.output_dir/'prelink.json').write_text(json.dumps(selection,indent=2)+'\n')
 report['base_kernel']=report['kernel'];report['kernel']={'path':str(kernel),'bytes':len(candidate)};report['prelink']={'components':selection['components'],'library_only_components':selection['library_only_components']};report['image_verification']='skipped by request'
 result=a.output_dir/'PONG_BUILD_RESULT.json';result.write_text(json.dumps(report,indent=2)+'\n')
 layout=json.loads(a.volume_layout.read_text())
 opts=['pong_allow_unauthenticated_root=1','cs_enforcement_disable=1','cs_library_val_enable=0','amfi_get_out_of_my_way=1','-apfs_shared_datavolume','debug=0x14e','serial=0','pong_disable_swap=1','pong_skip_setup=1','pong_adsp=2','pong_audio=2','pong_ufs_readwrite=1','ignition_level=1','dyld_flags=0x8','launchd_rd=md0','pong_init_trace=1','applog=1','applog_name=launchd,backboardd,SpringBoard,containermanagerd,lsd,runningboardd,logd_helper,powerd','pong_software_keybag=1','pong_display=1','pong_software_ui=1','pong_ui_software=1','pong_touch=1','pong_buttons=1','pong_init_path=/pong/init','pong_build=ipados18-22H374','epoch='+str(int(time.time()))]
 # The existing CPU scanout scaler divides independent rows among workers.
 # Leave CPU time for the software compositor and stock service startup.
 if gpu:
  opts=[x for x in opts if x not in ('pong_ui_software=1','pong_software_ui=1')]
  opts+=['pong_ui_software=0','pong_software_ui=0','pong_adreno_start=1','pong_metal=1',
         'pong_gpu_scanout=1','pong_gpu_pool_mib=256','pong_metal_global=1']
 # PCM stays optional and publishes only after the real APM/amp/DMA route
 # is ready. Do not silently disable it in an otherwise audio-enabled build.
 # Keep the live board's Wi-Fi worker and wallpaper snapshot policy in
 # every package; rebuilding must not silently drop these opt-in paths.
 opts+=['pong_ram_banks=1','launchd_unsecure_cache=1','pong_wifi=1','pong_poster_snapshot=1',f'fb_threads={min(a.cpus,4)}']
 if report['settings'].get('XNU_PONG_BACKLIGHT')=='1':opts+=['pong_backlight=1']
 # PE_parse_boot_argn can select the first duplicate. Apply command-line
 # overrides before packaging so there is exactly one value per named key.
 for value in a.kernel_arg:
  for item in value.split():
   key=item.split('=',1)[0]
   opts=[old for old in opts if old.split('=',1)[0]!=key]
   opts.append(item)
 cmd=[sys.executable,str(ROOT/'scripts/build-ipados18-bootimg-local.py'),'--kernel',str(kernel),'--result',str(result),'--output-dir',str(a.output_dir),'--loader',str(ROOT/'mac-build/darwinboot24/DarwinBoot24.efi'),'--cpus',str(a.cpus),'--fat-mib','96','--window-size',hex(a.window_size),'--ufs','--root-match-volume-uuid',layout['os']['uuid'],'--development-debug','--verbose-console','--skip-image-verification']
 auth=ROOT/'out/ipados18/system-volume-auth-blob.bin'
 if auth.exists():cmd+=['--system-volume-auth-blob',str(auth)]
 cmd+=['--kernel-arg='+v for v in opts]
 env=dict(os.environ)
 if report.get('schema_version')==2:
  hardware=Path(report['hardware_dtb'])
  if not hardware.is_file():raise RuntimeError('Compiled format2 hardware DTB required')
  env['PONG_HARDWARE_DTB']=str(hardware)
 subprocess.run(cmd,check=True,env=env)
if __name__=='__main__':main()
