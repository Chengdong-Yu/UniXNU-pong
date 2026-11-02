#!/usr/bin/env python3
"""Extend the unchanged Pong boot recipe with the selected restore's HID stack.

IOHIDFamily owns IOHIDResource. Its codeless EventDriver personalities connect
standard Consumer reports. No Apple touch-controller driver is selected.
"""
import importlib.util
import json
import sys
import uuid
from pathlib import Path

path=Path(__file__).with_name('build-ipados18-prelinked-boot.py')
spec=importlib.util.spec_from_file_location('pong_peripheral_boot_base',path)
base=importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

def include_hid(operation):
    def run(native,original,roots,*args,**kwargs):
        roots=list(dict.fromkeys([*roots,'com.apple.iokit.IOHIDFamily',
            'com.apple.iokit.IOHIDEventDriver']))
        return operation(native,original,roots,*args,**kwargs)
    return run

def emit_hid(operation):
    def run(*args,**kwargs):
        # Keep the user's prelink recipe and stock HID personalities intact.
        # Only this board-owned SPMI report device should skip the unavailable
        # AppleUserHIDDrivers dext and use the already selected kernel parser.
        # Ordinary HID Keyboard Driver has IOMatchDefer; on 22H374 it waited
        # for a DriverKit server for about 120 seconds before opening our
        # IOHIDUserDevice. This exact product/transport match is not deferred.
        import emit_prelink
        original=emit_prelink.unserialize_xml
        matches=json.loads((Path(__file__).resolve().parents[1]/
            'configs/pong-hid-match.json').read_text())
        def metadata(blob):
            value=original(blob)
            for entry in value.get('_PrelinkInfoDictionary',[]):
                if entry.get('CFBundleIdentifier')=='com.apple.iokit.IOHIDFamily':
                    entry.setdefault('IOKitPersonalities',{}).update(matches)
            return value
        emit_prelink.unserialize_xml=metadata
        try:
            candidate,report=operation(*args,**kwargs)
        finally:
            emit_prelink.unserialize_xml=original
        report['pong_hid_matching']=matches
        return candidate,report
    return run

base.plan=include_hid(base.plan)
base.emit=include_hid(emit_hid(base.emit))
if __name__=='__main__':
    from pong_native_inputs import options
    # Keep per-phone identity in ignored local.toml. The public board recipe
    # must not bake in an Apple ECID or another developer's hardware serial.
    supplied=any(item.startswith('--kernel-arg=pong_platform_uuid=') or
        (item=='--kernel-arg' and i+1<len(sys.argv) and
         sys.argv[i+1].startswith('pong_platform_uuid='))
        for i,item in enumerate(sys.argv))
    value=options().get('platform_uuid')
    if value and not supplied:
        identity=uuid.UUID(value)
        if not identity.int:raise SystemExit('platform_uuid must not be the null UUID')
        sys.argv.append('--kernel-arg=pong_platform_uuid='+str(identity))
    base.main()
