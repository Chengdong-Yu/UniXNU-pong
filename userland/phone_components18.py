#!/usr/bin/env python3
"""Supplement a phone persona with resources from its matching Apple restore.

Only local restore inputs are consumed; no Apple files belong in this repository.
The kernel, dyld cache and baseband binaries are never replaced by this step.
"""
from pathlib import Path
import os
import plistlib
import shutil
import subprocess
import struct

MACHO = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca', b'\xfe\xed\xfa\xce'}


def stage(root, donor, manifest, runtime):
    root, donor, runtime = Path(root), Path(donor), Path(runtime)
    release = plistlib.loads(Path(manifest).read_bytes())
    if (release['ProductVersion'], release['ProductBuildVersion']) != ('18.7.10', '22H374'):
        raise RuntimeError('Phone components must match the 22H374 guest')
    if not any(p.startswith('iPhone') for p in release['SupportedProductTypes']):
        raise RuntimeError('An actual iPhone restore is required')
    if os.geteuid():
        raise RuntimeError('OS component staging requires system ownership')
    target_version=plistlib.loads((root/'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if target_version.get('ProductBuildVersion')!='22H374':
        raise RuntimeError('Destination is not the selected 22H374 OS')
    copied = []
    # The iPad restore omits phone layouts/artwork; keep the base framework's
    # executable/Info and OS ABI, and add the same-build phone resource view.
    relatives = [Path('System/Library/PrivateFrameworks')/(name+'.framework') for name in
        ('TextInputUI','TextInput','TextInputCore','KeyboardServices','WiFiKitUI','TelephonyUI','PhoneNumbers')]
    relatives += [Path('System/Library/Frameworks/CoreTelephony.framework')]
    relatives += [Path('System/Library/PreferenceBundles')/name for name in
        ('CarrierSettings.bundle','CellularNetworkTelephonySettings.bundle',
         'MobilePhoneSettings.bundle','PhonebookTelephonySettings.bundle','SettingsCellular.bundle')]
    for relative in relatives:
        name = relative.stem
        source, target = donor / relative, root / relative
        if not source.is_dir() or not target.is_dir():
            continue
        executable = plistlib.loads((source / 'Info.plist').read_bytes()).get('CFBundleExecutable', name)
        for item in source.rglob('*'):
            rel = item.relative_to(source)
            if item.is_symlink() or not item.is_file() or '_CodeSignature' in rel.parts:
                continue
            if len(rel.parts) == 1 and rel.name in ('Info.plist', executable):
                continue
            with item.open('rb') as f:
                if f.read(4) in MACHO:
                    continue
            dest = target / rel
            if dest.is_symlink():
                raise RuntimeError('Unexpected component resource alias: ' + str(dest))
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item, dest)
            os.chown(dest, 0, 0)
            os.chmod(dest, 0o644)
            copied.append((relative / rel).as_posix())
    applications, deferred = [], []
    source = donor / 'Applications/MobilePhone.app'
    if source.is_dir():
        info = plistlib.loads((source / 'Info.plist').read_bytes())
        if info.get('CFBundleIdentifier') != 'com.apple.mobilephone':
            raise RuntimeError('Unexpected Phone application identity')
        executable=source/info['CFBundleExecutable']
        with executable.open('rb') as binary:
            header=binary.read(32)
        subtype=struct.unpack_from('<I',header,8)[0]&0x00ffffff if header[:4]==b'\xcf\xfa\xed\xfe' else None
        # This profile consumes the iPad7 ARM64 dyld cache. The XR program's
        # arm64e ABI is not made compatible by copying it or changing a header.
        if subtype==2:
            deferred.append(dict(identifier=info['CFBundleIdentifier'],architecture='arm64e',
                reason='current iPad7 profile uses ARM64 shared cache; complete donor retained locally'))
        elif subtype==0:
            target = root / 'Applications/MobilePhone.app'
            if target.is_symlink():
                raise RuntimeError('Phone application must be an ordinary bundle')
            if target.exists():
                old = plistlib.loads((target / 'Info.plist').read_bytes())
                if old.get('CFBundleIdentifier') != info['CFBundleIdentifier']:
                    raise RuntimeError('Preserving a foreign application bundle')
                shutil.rmtree(target)
            shutil.copytree(source, target, symlinks=True)
            subprocess.run(['codesign', '--force', '--deep', '--sign', '-', '--pagesize', '4096',
                            '--preserve-metadata=entitlements,requirements,flags', str(target)], check=True)
            for item in (target, *target.rglob('*')):
                if not item.is_symlink():
                    os.chown(item, 0, 0)
            applications.append(info['CFBundleIdentifier'])
        else:
            raise RuntimeError('Unrecognized stock Phone executable architecture')
    # Carrier files are configuration, not a modem transport. Preserve the
    # actual cellular/noncellular CommCenter selection until a baseband exists.
    carriers = donor / 'System/Library/Carrier Bundles/iPhone'
    if carriers.is_dir():
        target = root / 'System/Library/Carrier Bundles/iPhone'
        if target.is_symlink():
            raise RuntimeError('Unexpected carrier configuration alias')
        shutil.copytree(carriers, target, dirs_exist_ok=True, symlinks=True)
        copied.append('System/Library/Carrier Bundles/iPhone')
    if applications:
        helper = root / 'pong/phone-register'
        shutil.copyfile(runtime / 'phone-register', helper)
        os.chown(helper, 0, 0)
        os.chmod(helper, 0o755)
    return dict(build='22H374', products=release['SupportedProductTypes'], resources=copied,
                applications=applications, deferred_applications=deferred, native_code='base dyld cache/kernel retained; incompatible donor application not registered',
                cellular='carrier configuration only; real Pong baseband transport still required')


if __name__ == '__main__':
    import argparse
    import json
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('root','donor','manifest','runtime','report'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    result=stage(args.root,args.donor,args.manifest,args.runtime)
    args.report.write_text(json.dumps(result,indent=2)+'\n')
    print('Installed matching phone resources:',len(result['resources']))
