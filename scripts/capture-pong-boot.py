#!/usr/bin/env python3
"""Run one timed Pong boot and collect the existing REC pstore paths.

The image must contain pong_capture_seconds and pong_capture_id. No image
hashes, flashing, image readback, or endless reboot loop. Partial evidence
and the failed stage are retained on timeout. Default final state is REC.
"""
import argparse
import datetime
import json
from pathlib import Path
import subprocess
import sys
import time


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--boot-image',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--boot-id',required=True)
    p.add_argument('--serial',required=True)
    p.add_argument('--adb',default='adb')
    p.add_argument('--fastboot',default='fastboot')
    p.add_argument('--port',type=int,default=5038)
    p.add_argument('--timeout',type=int,default=360)
    p.add_argument('--reader-image',type=Path,
                   help='optional temporary reader when the kernel targets bootloader')
    p.add_argument('--return-to',choices=('recovery','bootloader'),default='recovery')
    a=p.parse_args()
    if not a.boot_image.is_file():p.error('boot image does not exist')
    if a.reader_image and not a.reader_image.is_file():p.error('reader image does not exist')
    if not 60<=a.timeout<=7200:p.error('timeout must be 60..7200 seconds')
    if not a.boot_id or len(a.boot_id)>63 or not all(c.isalnum() or c in '-_.' for c in a.boot_id):
        p.error('boot-id must be a short identifier matching pong_capture_id')
    if (a.out/'capture.json').exists():p.error('existing evidence retained; choose a new output directory')
    a.out.mkdir(parents=True,exist_ok=True)
    receipt=dict(boot_id=a.boot_id,serial=a.serial,boot_image=str(a.boot_image.resolve()),
        start_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        log_source='/sys/fs/pstore',stage='starting',success=False,
        permanent_partition_writes=False,image_verification='skipped by request')

    def save(stage,**fields):
        receipt.update(stage=stage,**fields)
        (a.out/'capture.json').write_text(json.dumps(receipt,indent=2)+'\n')

    def run(command,timeout=15):
        return subprocess.run(command,capture_output=True,text=True,timeout=timeout)

    def adb(*args):return [a.adb,'-P',str(a.port),'-s',a.serial,*args]
    def fastboot(*args):return [a.fastboot,'-s',a.serial,*args]
    def poll(command,timeout=8):
        try:return run(command,timeout)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(command,124,'','transport query timed out')
    def require(command,name,timeout=30):
        result=run(command,timeout)
        (a.out/(name+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(name+' failed; evidence saved')
        return result

    try:
        devices=run([a.fastboot,'devices','-l'])
        if not any(line.split()[:1]==[a.serial] for line in devices.stdout.splitlines()):
            state=run(adb('get-state'))
            if state.returncode or state.stdout.strip() not in ('recovery','device'):
                raise RuntimeError('selected device is neither ADB nor Fastboot')
            save('entering-bootloader')
            require(adb('reboot','bootloader'),'enter-bootloader')
            limit=time.monotonic()+45
            while time.monotonic()<limit:
                result=run([a.fastboot,'devices','-l'])
                if any(line.split()[:1]==[a.serial] for line in result.stdout.splitlines()):break
                time.sleep(1)
            else:raise TimeoutError('selected device did not enter Fastboot')
        save('temporary-boot')
        result=require(fastboot('boot',str(a.boot_image.resolve())),'fastboot',120)
        print(result.stdout+result.stderr,flush=True)
        save('waiting-for-timed-return')
        limit=time.monotonic()+a.timeout
        reader_started=False
        while time.monotonic()<limit:
            state=poll(adb('get-state'))
            if not state.returncode and state.stdout.strip() in ('recovery','device'):
                mode=poll(adb('shell','getprop','ro.bootmode'))
                if state.stdout.strip()=='recovery' or mode.stdout.strip()=='recovery':break
            if a.reader_image and not reader_started:
                devices=poll([a.fastboot,'devices','-l'])
                if any(line.split()[:1]==[a.serial] for line in devices.stdout.splitlines()):
                    save('temporary-reader')
                    require(fastboot('boot',str(a.reader_image.resolve())),'reader-boot',120)
                    reader_started=True
            time.sleep(2)
        else:raise TimeoutError('no REC return within the bounded capture interval')
        save('reading-pstore')
        require(adb('shell','id'),'reader-id')
        require(adb('shell','cat','/proc/sys/kernel/random/boot_id'),'reader-boot-id')
        require(adb('pull','/sys/fs/pstore',str(a.out)),'pstore-pull',60)
        # Check text provenance, not storage images. Preserve old/partial
        # pstore on mismatch and do not report a stale boot as this run.
        sources=list((a.out/'pstore').glob('*'))
        marker=('PONG CAPTURE: id='+a.boot_id+' ').encode()
        matching=[f.name for f in sources if f.is_file() and marker in f.read_bytes()]
        if not matching:raise RuntimeError('pstore lacks this capture boot ID; raw files retained')
        text=b'\n'.join(f.read_bytes() for f in sources if f.is_file())
        sync_seen=b'phase=timed-return-sync ' in text
        committed=b'phase=logs-committed-reset ' in text
        if not committed:
            save('logs-saved-return-unconfirmed',matching_files=matching,
                 sync_requested=sync_seen,reset_commit_seen=False,final_state='recovery')
            raise RuntimeError('logs collected, but automatic reset commit is absent; return cause unconfirmed')
        final='recovery'
        if a.return_to=='bootloader':
            save('returning-to-bootloader')
            require(adb('reboot','bootloader'),'return-bootloader')
            until=time.monotonic()+45
            while time.monotonic()<until:
                devices=run([a.fastboot,'devices','-l'])
                if any(line.split()[:1]==[a.serial] for line in devices.stdout.splitlines()):break
                time.sleep(1)
            else:raise TimeoutError('logs saved but final bootloader state not reached')
            final='bootloader'
        save('complete',success=True,matching_files=matching,sync_requested=sync_seen,
             reset_commit_seen=committed,final_state=final,
             finish_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        print('Captured '+a.boot_id+' to '+str(a.out)+'; final='+final,flush=True)
    except (OSError,subprocess.SubprocessError,RuntimeError,TimeoutError) as error:
        save(receipt['stage'],error=str(error),success=False)
        print(str(error),file=sys.stderr);return 1
    return 0


if __name__=='__main__':sys.exit(main())
