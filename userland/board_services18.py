#!/usr/bin/env python3
"""Pong runtime policy layered after the upstream image builders.

Keep user-maintained CI/build scripts intact. This updates only job policy
backed by actual hardware/exit evidence and retains every stock executable.
"""
import argparse
import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path
import plistlib


def build_wifi(runtime):
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
    from pong_native_inputs import build_env
    runtime.mkdir(parents=True, exist_ok=True)
    target=runtime/'wifi-bootstrap'
    subprocess.run(['xcrun','--sdk','iphoneos','clang','-target','arm64-apple-ios18.0',
        '-Os','-Wall','-Wextra','-Werror','-Wl,-no_fixup_chains','-framework','CoreFoundation','-framework','IOKit',
        str(Path(__file__).with_name('wifi-bootstrap.c')),'-o',str(target)],env=build_env(),check=True)
    ent=runtime/'wifi-bootstrap-entitlements.plist'
    ent.write_bytes(plistlib.dumps({'platform-application':True,
        'com.apple.private.security.no-container':True,'com.apple.wifi.manager-access':True,
        'com.apple.private.corewifi.internal':True,
        'com.apple.SystemConfiguration.SCPreferences-write-access':['preferences.plist'],
        'com.apple.SystemConfiguration.SCDynamicStore-write-access':True},fmt=plistlib.FMT_XML))
    subprocess.run(['codesign','--force','--sign','-','--pagesize','4096',
        '--entitlements',str(ent),str(target)],check=True)


def stage(root, runtime=None):
    cache_path = root / 'System/Library/xpc/launchd.plist'
    cache = plistlib.loads(cache_path.read_bytes())
    changes = []
    for path, job in cache['LaunchDaemons'].items():
        label = job.get('Label', '')
        if label in ('com.apple.wifid','com.apple.configd','com.apple.audiomxd'):
            environment=job.setdefault('EnvironmentVariables',{})
            if label=='com.apple.audiomxd':environment['OS_ACTIVITY_DT_MODE']='YES'
            elif environment.get('OS_ACTIVITY_DT_MODE')=='YES':
                environment.pop('OS_ACTIVITY_DT_MODE') # WLAN is working; avoid broad configd/wifid debug flooding
            # audiomxd runs as mobile; root-owned /var/logs cannot create its
            # redirected stderr and xpcproxy rejects the whole spawn (EACCES).
            # Preserve the working stock console, already mirrored to pstore.
            job['StandardErrorPath']='/dev/console'
        if label == 'dev.unixnu.pong.audio-route':
            # Embedded AudioObject APIs start a HALS_System in their caller.
            # The actual ASPL now selects its route in an audiomxd worker;
            # do not start another audio server in this external helper.
            job['Disabled']=True
            job.pop('RunAtLoad',None)
            reason='native output selection now runs outside callbacks in the actual audiomxd host'
        elif label == 'com.apple.TextInput.kbd':
            # Setup is intentionally skipped. Its setupexited event may never
            # arrive, but UIKit still needs the stock keyboard XPC provider.
            # Publish the real same-build daemon once and retain its native
            # MachServices; no busy KeepAlive or replacement input service.
            job['RunAtLoad']=True
            job.pop('Disabled',None)
            reason='start the actual TextInput daemon when Setup completion is skipped'
        elif label == 'com.apple.wifid':
            # 22H374 wifid contains WiFiServerCreate("com.apple.wifi.manager")
            # and bootstrap_check_in for that legacy MobileWiFi MIG server.
            # Its restore plist declares only the newer XPC listeners; on
            # this imported launchd cache, no job owns the legacy receive
            # right and real callers repeatedly get BOOTSTRAP_UNKNOWN_SERVICE.
            # Declare the port on the actual daemon, not a substitute server.
            services = job.setdefault('MachServices', {})
            # A reserved receive right is not a running MIG server. With
            # True, launchd handed clients a send right before wifid called
            # bootstrap_check_in. services105 proves SpringBoard blocked
            # in wifi_manager_open on that right, preventing its first
            # frame. Expose the native server only once it actually checks
            # in; its own check-in and later normal clients remain intact.
            services['com.apple.wifi.manager'] = {'HideUntilCheckIn': True}
            reason = 'native MobileWiFi listener is discoverable only after its actual check-in; no unserved MIG port during UI startup'
        elif label in ('com.apple.audiomxd','com.apple.configd'):
            reason='native daemon diagnostics use the existing pstore console without a mobile-user file-open failure'
        elif label == 'com.apple.bluetoothd':
            # Logs confirm no BT address/transport and exit(0), immediately
            # followed by KeepAlive/event-driven restarts. A real BT provider
            # must publish its DT node before this hardware daemon is loaded.
            job['_LimitLoadToDeviceTree'] = ['bluetooth', 'marconi-bt']
            reason = 'requires a real Pong Bluetooth transport; none implemented'
        elif label in ('com.apple.lskdd','com.apple.fairplayd.A2','com.apple.wifip2pd'):
            # services125: 957 lskdd exits and ~100 exits each of the other
            # jobs in retained console alone. Preserve demand/IPC and files;
            # avoid continuous unsupported-hardware process creation storms.
            job['ThrottleInterval']=max(int(job.get('ThrottleInterval',0)),60)
            reason = 'REC services125 confirmed repeated rapid exits; retain on-demand retry with 60s throttle'
        elif label == 'com.apple.spaceattributiond':
            # Normal successful exits should stay dormant until next demand.
            # Preserve Mach services and requests from Storage Settings.
            if 'KeepAlive' not in job:
                continue
            job.pop('KeepAlive')
            reason = 'retain on-demand service; do not restart successful exits'
        elif label == 'com.apple.mobile.softwareupdated':
            # Repeated 22H374 IPS reports show objc_release(NULL+0x20) on
            # startup, including boots before services86. OTA restore/update
            # is not implemented on this board. Keep the component files and
            # job definition, but do not run a crashing optional updater.
            job['Disabled'] = True
            job.pop('KeepAlive', None)
            job.pop('RunAtLoad', None)
            reason = 'Pong OTA unsupported; repeated softwareupdated SIGSEGV in REC crash reports'
        else:
            continue
        target = root / path.lstrip('/')
        target.write_bytes(plistlib.dumps(job, fmt=plistlib.FMT_BINARY))
        os.chown(target, 0, 0)
        os.chmod(target, 0o644)
        changes.append(dict(label=label, reason=reason))
    if runtime is not None:
        target=root/'pong/wifi-bootstrap'
        shutil.copyfile(runtime/'wifi-bootstrap',target)
        os.chown(target,0,0);os.chmod(target,0o755)
        label='dev.unixnu.pong.wifi-bootstrap'
        path='/System/Library/LaunchDaemons/'+label+'.plist'
        job=dict(Label=label,ProgramArguments=['/pong/wifi-bootstrap'],UserName='root',
            RunAtLoad=True,LaunchOnlyOnce=True,ProcessType='Background',
            StandardOutPath='/private/var/logs/pong-wifi-bootstrap.log',
            StandardErrorPath='/private/var/logs/pong-wifi-bootstrap.log')
        target=root/path.lstrip('/')
        target.write_bytes(plistlib.dumps(job,fmt=plistlib.FMT_BINARY))
        os.chown(target,0,0);os.chmod(target,0o644)
        cache['LaunchDaemons'][path]=job
        changes.append(dict(label=label,reason='one native power request after real device discovery; no fake interface/control state'))
    if (root/'Applications/MobilePhone.app/Info.plist').is_file() and (root/'pong/phone-register').is_file():
        label='dev.unixnu.pong.phone-register'
        path='/System/Library/LaunchDaemons/'+label+'.plist'
        job=dict(Label=label,ProgramArguments=['/pong/phone-register'],UserName='mobile',
            RunAtLoad=True,LaunchOnlyOnce=True,ProcessType='Background',
            StandardOutPath='/dev/console',StandardErrorPath='/dev/console')
        target=root/path.lstrip('/')
        target.write_bytes(plistlib.dumps(job,fmt=plistlib.FMT_BINARY))
        os.chown(target,0,0);os.chmod(target,0o644)
        cache['LaunchDaemons'][path]=job
        changes.append(dict(label=label,reason='register actual same-build stock Phone app in mobile LaunchServices'))
    cache_path.write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
    os.chown(cache_path, 0, 0)
    os.chmod(cache_path, 0o644)
    return changes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path)
    parser.add_argument('--record', type=Path)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--build-wifi-runtime',type=Path)
    args = parser.parse_args()
    if args.build_wifi_runtime:
        build_wifi(args.build_wifi_runtime);return
    if not args.prepared or not args.record:
        parser.error('--prepared and --record required for staging')
    if os.geteuid() != 0:
        raise RuntimeError('system authentication required to edit the local OS copy')
    spec = importlib.util.spec_from_file_location('pong_runtime', Path(__file__).with_name('build-ipados18.py'))
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
    root = args.prepared / 'system-mount'
    device = runtime.attach(runtime.os_copy(args.prepared), root)
    try:
        changes = stage(root, args.runtime)
    finally:
        runtime.run(['hdiutil', 'detach', device])
    args.record.write_text(json.dumps(dict(build='22H374', changes=changes,
                                         binaries_removed=False), indent=2) + '\n')


if __name__ == '__main__':
    main()
